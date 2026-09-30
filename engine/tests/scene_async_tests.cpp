// Tests of Scene::fromJson with and without AsyncAssetLoader.
//
// The most important case is the first one: with loader == nullptr the
// behavior has to be IDENTICAL to that before this feature. It is what
// protects the Play->Stop restore (EditorUI.cpp:170) and the eight suites that already
// exist. That test (testSyncPathUnchanged) is a DIFFERENTIAL on
// pendingMeshJob with a scene that DOES have a sourcePath, not a comparison
// of toJson() between two nullptr loads, which cannot detect an inverted
// branch selection (see the comment next to the function for why).
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Core/JobSystem.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Renderer/AsyncAssetLoader.h"
#include "DonTopo/Renderer/Mesh.h"
#include "gltf_fixtures.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace {

// Check with a LOUD failure and valid in Release: assert() compiles to nothing under
// NDEBUG, so an `assert(ptr); ptr->field` leaves a dereference of a
// potentially null pointer with no net. CHECK counts the failure, prints it and does NOT
// dereference; a broken test reports instead of crashing with a silent 0xC0000005.
int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
    ++g_failures; } } while (0)

// A single PhysicsManager for the whole file: creating and freeing one per test
// crashes on the second init because PxFoundation is unique per process.
DonTopo::PhysicsManager& physics()
{
    static DonTopo::PhysicsManager p;
    static bool inited = (p.init(), true);
    (void)inited;
    return p;
}

DonTopo::AudioManager& audio()
{
    static DonTopo::AudioManager a;
    static bool inited = (a.init(), true);
    (void)inited;
    return a;
}

// Minimal scene with two nested nodes and NON-neutral transforms: a test that
// asserted identity would pass all the same if nobody read the field. Without
// sourcePath: it never triggers the mesh load branch, synchronous or asynchronous
// alike; it is exactly what is needed to compare the two paths without
// the result depending on whether the asset exists on disk.
//
// Wrapped in version/root: Scene::fromJson requires both fields (the same
// ones toJson() produces) and returns false if they are missing; the brief's fixture
// did not carry them and therefore never loaded anything. Without the wrapper the three
// tests would fail on the first assert, before exercising anything of the loader.
nlohmann::json twoNodeScene()
{
    return nlohmann::json::parse(R"({
        "version": 1,
        "root": {
            "name": "root",
            "children": [
                { "name": "hijoA", "position": [1.5, -2.0, 3.25], "children": [] },
                { "name": "hijoB", "position": [-4.0, 5.5, 6.75], "children": [] }
            ]
        }
    })");
}

// Scene with a node that DOES trigger the mesh load branch: sourcePath
// points to a file that on purpose does not exist. A real asset is not needed
// since the worker will fail with a non-empty LoadedMesh::error, which is exactly the
// case that has to be supported without crashing. It serves to really test the
// async branch (the brief's fixture, without "mesh", never triggered it).
nlohmann::json sceneWithPendingLoad()
{
    return nlohmann::json::parse(R"({
        "version": 1,
        "root": {
            "name": "root",
            "children": [
                { "name": "hijoA", "position": [1.5, -2.0, 3.25], "children": [],
                  "mesh": { "sourcePath": "assets/__no_existe__.fbx" } },
                { "name": "hijoB", "position": [-4.0, 5.5, 6.75], "children": [] }
            ]
        }
    })");
}

// Removes "id" recursively from a serialized node (same pattern as the
// stripIds of Scene::cloneGameObject). node->id is assigned by a GLOBAL atomic
// counter in the GameObject constructor: two different Scenes
// loading the SAME JSON (without "id" in the file) reuse the position in the
// counter but never the same absolute value, so comparing toJson() without
// removing "id" would ALWAYS fail, even between two identical loads; it
// would prove nothing about loader nullptr vs default.
void stripIds(nlohmann::json& node)
{
    if (!node.is_object()) return;
    node.erase("id");
    if (auto it = node.find("children"); it != node.end() && it->is_array())
        for (auto& child : *it)
            stripIds(child);
}

// The most important case: with loader == nullptr the behavior has to
// be IDENTICAL to that before this feature; in particular, the static branch
// that this task touched (the `else if (!sourcePath.empty()) { if (loader) {...}
// else {...} }` inside nodeFromJson) must NOT enqueue any request.
//
// It is a DIFFERENTIAL on pendingMeshJob, not a comparison of toJson():
// it loads the SAME scene with and without loader and compares the field that the
// if(loader)/else branch really writes. This DOES test branch selection, unlike
// comparing two toJson() where BOTH sides use loader==nullptr
// (see testSyncDefaultArgEqualsExplicitNullptr further down): with both sides
// nullptr, a corrupted branch corrupts both equally and that comparison
// would keep matching. A node with sourcePath is needed (sceneWithPendingLoad,
// not twoNodeScene) for the branch to even exist; with twoNodeScene
// j.contains("mesh") is false and the whole block is dead for the test.
//
// Sabotage: invert the condition to `if (!loader)` in the static branch of
// nodeFromJson -> the two asserts below swap (without a loader it
// would enqueue, with a loader it would not) and the test fails.
void testSyncPathUnchanged(DonTopo::AsyncAssetLoader& loader)
{
    DonTopo::Scene sync;
    CHECK(sync.fromJson(sceneWithPendingLoad(), physics(), audio(), nullptr), "fromJson nullptr debe cargar");
    DonTopo::GameObject* hijoASync = nullptr;
    sync.traverse([&](DonTopo::GameObject* go) { if (go->name == "hijoA") hijoASync = go; });
    CHECK(hijoASync, "hijoA debe existir (sync)");
    // Without a loader: the synchronous branch runs (ModelLoader::load). The file does not
    // exist, so there is no mesh; but above all requestMesh was NOT called
    // at all, and pendingMeshJob stays at its default value (0).
    if (hijoASync)
        CHECK(hijoASync->pendingMeshJob == 0,
              "sin loader no debe encolarse ninguna peticion (rama sincrona)");

    DonTopo::Scene withLoader;
    CHECK(withLoader.fromJson(sceneWithPendingLoad(), physics(), audio(), &loader), "fromJson &loader debe cargar");
    DonTopo::GameObject* hijoAAsync = nullptr;
    withLoader.traverse([&](DonTopo::GameObject* go) { if (go->name == "hijoA") hijoAAsync = go; });
    CHECK(hijoAAsync, "hijoA debe existir (async)");
    // With a loader: the asynchronous branch runs -> requestMesh WAS called.
    if (hijoAAsync)
        CHECK(hijoAAsync->pendingMeshJob != 0,
              "con loader debe encolarse una peticion real (rama asincrona)");
}

// Secondary determinism check: the default parameter and the explicit
// nullptr are the SAME path (same hierarchy/transforms after
// stripIds). It does NOT test branch selection (see testSyncPathUnchanged for
// that) because both sides are loader==nullptr: a corrupted static
// branch corrupts both equally and this comparison would keep matching.
void testSyncDefaultArgEqualsExplicitNullptr()
{
    DonTopo::Scene a, b;
    CHECK(a.fromJson(twoNodeScene(), physics(), audio()), "fromJson default-arg debe cargar");
    CHECK(b.fromJson(twoNodeScene(), physics(), audio(), nullptr), "fromJson nullptr explicito debe cargar");

    nlohmann::json ja = a.toJson();
    nlohmann::json jb = b.toJson();
    stripIds(ja["root"]);
    stripIds(jb["root"]);
    CHECK(ja == jb, "default-arg y nullptr explicito dan el mismo resultado");
}

// With a loader, the GameObjects already exist with their hierarchy and their transform: the
// only thing missing is the mesh. The node with sourcePath is left with a request
// in flight (pendingMeshJob != 0) and without a render index; nothing has been pumped
// yet. Sabotage: create the nodes only when pumping; the names assert
// fails because the scene is empty.
void testAsyncCreatesNodesImmediately(DonTopo::AsyncAssetLoader& loader)
{
    DonTopo::Scene s;
    CHECK(s.fromJson(sceneWithPendingLoad(), physics(), audio(), &loader), "fromJson &loader debe cargar");

    int found = 0;
    bool hijoAHasPendingJob = false;
    s.traverse([&](DonTopo::GameObject* go) {
        if (go->name == "hijoA" || go->name == "hijoB") ++found;
        // Nothing has been pumped yet: no node can have a render index.
        CHECK(go->staticRenderIndex  == -1, "sin bombear no hay indice static");
        CHECK(go->skinnedRenderIndex == -1, "sin bombear no hay indice skinned");
        if (go->name == "hijoA")
        {
            // hijoA carried sourcePath: it must have a request in flight and
            // NO mesh yet (the GameObject exists complete from
            // frame 0, without waiting for the asset).
            hijoAHasPendingJob = (go->pendingMeshJob != 0);
            CHECK(!go->hasMesh(), "hijoA no debe tener mesh todavia");
        }
        if (go->name == "hijoB")
        {
            // hijoB did not carry sourcePath: there is nothing to request.
            CHECK(go->pendingMeshJob == 0, "hijoB sin sourcePath no encola");
        }
    });
    CHECK(found == 2, "los GameObject existen desde el frame 0, sin esperar al asset");
    CHECK(hijoAHasPendingJob, "el nodo con sourcePath debe encolar una peticion real");
}

// Deleting a GameObject with a pending load and pumping afterwards does not crash: the
// result is discarded because its targetId is no longer in the live scene.
//
// It is the most valuable test of the three: the classic use-after-free of this
// pattern is storing a GameObject* in the request. Sabotage: store the
// pointer instead of the id and dereference it when pumping; crash or garbage.
void testDeletedTargetIsDiscarded(DonTopo::AsyncAssetLoader& loader)
{
    DonTopo::Scene s;
    // sceneWithPendingLoad, not twoNodeScene: a REAL request in flight is needed
    // (hijoA has sourcePath) for this test to check anything; with
    // the brief's fixture without a mesh, pumpCompleted() had nothing to
    // deliver and the test passed without exercising the discard path.
    CHECK(s.fromJson(sceneWithPendingLoad(), physics(), audio(), &loader), "fromJson &loader debe cargar");

    DonTopo::GameObject* victim = nullptr;
    s.traverse([&](DonTopo::GameObject* go) { if (go->name == "hijoA") victim = go; });
    CHECK(victim, "hijoA debe existir");
    if (!victim) return;   // Release-safe: without victim you cannot continue without dereferencing null
    CHECK(victim->pendingMeshJob != 0, "hace falta una peticion real en vuelo para este test");

    const uint64_t goneId = victim->id;
    s.removeGameObject(victim);

    // A result addressed to an id that no longer exists cannot touch freed
    // memory. It is checked that it still does not show up after pumping.
    for (auto& r : loader.pumpCompleted(1000.0f))
        (void)r;   // all that matters is that it does not crash

    bool stillThere = false;
    s.traverse([&](DonTopo::GameObject* go) { if (go->id == goneId) stillThere = true; });
    CHECK(!stillThere, "el nodo borrado no puede resucitar al bombear");
}

// The preload cache (PreloadedMeshCache) is consulted BEFORE reading disk: a
// sourcePath present in the cache uses a deep copy of the cached mesh
// without touching the file. It is what lets the runtime load the scene from
// meshes already preloaded in parallel (with progress on the splash) without changing the
// registration model or losing the animation config.
//
// Verifiable without a real asset: a mesh is fabricated in RAM with a DISTINCTIVE name and
// vertices and put in the cache under the sourcePath of hijoA, which
// points to a file that does NOT exist. If the node ends up with that mesh, the cache
// was really consulted; without the cache, a nonexistent path gives no mesh at all.
//
// Sabotage: if nodeFromJson ignored `preloaded` (did not consult the cache), the
// first block fails on `hijoA->hasMesh()`: the nonexistent path falls to disk,
// which cannot be read, and the node is left without a mesh.
void testPreloadedCacheConsulted()
{
    const std::string src = "assets/__no_existe__.fbx";

    auto fabricated = std::make_shared<DonTopo::Mesh>();
    fabricated->name = "malla_precargada_ficticia";
    DonTopo::Vertex v{};
    v.pos = glm::vec3(7.0f, 8.0f, 9.0f);
    fabricated->vertices.push_back(v);

    DonTopo::PreloadedMeshCache cache;
    cache[src] = fabricated;

    // With cache: the node receives the fake mesh without reading disk (the file does not
    // exist: without the cache there would be no mesh). It must also be a DEEP COPY, not the
    // same shared_ptr; two GameObjects cannot share a mutable Mesh.
    DonTopo::Scene withCache;
    CHECK(withCache.fromJson(sceneWithPendingLoad(), physics(), audio(), nullptr, &cache), "fromJson con cache debe cargar");
    DonTopo::GameObject* hijoA = nullptr;
    withCache.traverse([&](DonTopo::GameObject* go) { if (go->name == "hijoA") hijoA = go; });
    CHECK(hijoA, "hijoA debe existir (cache)");
    if (hijoA)
    {
        CHECK(hijoA->hasMesh(), "con cache el nodo debe recibir la malla precargada, sin leer disco");
        if (hijoA->hasMesh())
        {
            CHECK(hijoA->getMesh()->name == "malla_precargada_ficticia", "el nombre debe venir de la malla cacheada");
            CHECK(hijoA->getMesh()->vertices.size() == 1 &&
                  hijoA->getMesh()->vertices[0].pos == glm::vec3(7.0f, 8.0f, 9.0f),
                  "los vertices deben ser los de la malla cacheada");
            // Contract since Appendix B: the preloaded static mesh is
            // SHARED (it is const for the GameObject), and editing it copies, so
            // the one in the cache is not touched.
            CHECK(hijoA->getMesh().get() == fabricated.get(), "la malla precargada se comparte, no se copia");
            hijoA->editMesh()->name = "editada";
            CHECK(fabricated->name == "malla_precargada_ficticia", "editar el nodo no debe tocar la malla de la cache");
            CHECK(hijoA->getMesh().get() != fabricated.get(), "tras editar, el nodo tiene su propia copia");
        }
    }

    // Cache miss (cache with another key that does not match): falls to the nonexistent disk ->
    // no mesh. Proves that a miss does not invent anything and respects the fallback.
    DonTopo::PreloadedMeshCache otherCache;
    otherCache["assets/otra_cosa.fbx"] = fabricated;
    DonTopo::Scene withMiss;
    CHECK(withMiss.fromJson(sceneWithPendingLoad(), physics(), audio(), nullptr, &otherCache), "fromJson cache-miss debe cargar");
    DonTopo::GameObject* missA = nullptr;
    withMiss.traverse([&](DonTopo::GameObject* go) { if (go->name == "hijoA") missA = go; });
    CHECK(missA, "hijoA debe existir (miss)");
    if (missA)
        CHECK(!missA->hasMesh(), "cache-miss para un path inexistente debe caer al disco y quedarse sin mesh");

    // preloaded == nullptr: identical to the miss (fallback to disk), byte-compatible
    // with all the usual callers.
    DonTopo::Scene noCache;
    CHECK(noCache.fromJson(sceneWithPendingLoad(), physics(), audio(), nullptr, nullptr), "fromJson nullptr cache debe cargar");
    DonTopo::GameObject* nullA = nullptr;
    noCache.traverse([&](DonTopo::GameObject* go) { if (go->name == "hijoA") nullA = go; });
    CHECK(nullA, "hijoA debe existir (nullptr cache)");
    if (nullA)
        CHECK(!nullA->hasMesh(), "sin cache el path inexistente no da mesh");
}

// A clone can NOT inherit the render indices of the original (H14).
//
// They are the backend slot where the original's mesh lives: if the clone kept them,
// moving it would move the ORIGINAL's mesh, and deleting it would release a
// slot that the original is still using; which, since slots are recycled,
// means that the next object to be registered would take it over while the
// original draws it. None of that gives an error anywhere.
//
// HONESTY ABOUT WHAT THIS TEST PROVES AND WHAT IT DOES NOT. Today it passes by
// CONSTRUCTION: cloneGameObject serializes with nodeToJson and rebuilds with
// nodeFromJson, and the indices are NOT serialized, so the new nodes are already
// born at -1 by their default value. It was checked by removing the reset traverse of
// Scene::cloneGameObject and the test stayed green, that is, that
// traverse is defensive and today covers nothing.
//
// It stays all the same because it asserts the PROPERTY and not the implementation: the day
// someone serializes the indices in nodeToJson, or changes the cloning to a
// direct copy instead of going through JSON, this test turns red. What it does NOT
// do is protect that traverse; for that it would be necessary to be able to build a clone
// that did arrive with indices set, and that cannot be done through that path.
//
// It is asserted over the WHOLE tree and not only over the clone's root: cloning an
// imported model always brings a hierarchy.
void testClonNoHeredaIndicesDeRender()
{
    DonTopo::Scene scene;
    CHECK(scene.fromJson(twoNodeScene(), physics(), audio(), nullptr, nullptr),
          "fromJson debe cargar la escena de dos nodos");

    DonTopo::GameObject* original = nullptr;
    scene.traverse([&](DonTopo::GameObject* go) { if (go->name == "hijoA") original = go; });
    CHECK(original, "hijoA debe existir");
    if (!original) return;

    // A child, so that the clone has a hierarchy to walk.
    DonTopo::GameObject* hijo = scene.cloneGameObject(original, original, physics(), audio());
    CHECK(hijo, "el hijo de prueba debe crearse");
    if (!hijo) return;

    // The original and its child ALREADY registered: distinct and non-trivial values,
    // so that inheriting them is noticeable and not mistaken for a default zero.
    original->staticRenderIndex  = 7;
    original->skinnedRenderIndex = 3;
    hijo->staticRenderIndex      = 11;
    hijo->skinnedRenderIndex     = 5;

    DonTopo::GameObject* clon = scene.cloneGameObject(original, &scene.getRoot(),
                                                      physics(), audio());
    CHECK(clon, "el clon debe crearse");
    if (!clon) return;

    int nodos = 0;
    clon->traverse([&](DonTopo::GameObject* go) {
        nodos++;
        CHECK(go->staticRenderIndex  == -1, "el clon no hereda staticRenderIndex");
        CHECK(go->skinnedRenderIndex == -1, "el clon no hereda skinnedRenderIndex");
    });
    CHECK(nodos == 2, "el clon debe traer su hijo (raiz + 1)");

    // And the original intact: cloning does not touch its own.
    CHECK(original->staticRenderIndex  == 7, "el original conserva su indice static");
    CHECK(original->skinnedRenderIndex == 3, "el original conserva su indice skinned");
}

void testPieceCacheKeys()
{
    CHECK(DonTopo::meshCacheKey("a/b.fbx", 0) == "a/b.fbx", "la pieza 0 conserva la clave de siempre");
    CHECK(DonTopo::meshCacheKey("a/b.fbx", 2) == "a/b.fbx#piece=2", "las demas llevan la pieza");
}

std::shared_ptr<DonTopo::Mesh> fakePiece(const std::string& src, int piece, const char* name)
{
    auto m = std::make_shared<DonTopo::Mesh>();
    m->sourcePath = src;
    m->piece      = piece;
    m->name       = name;
    DonTopo::Vertex v{};
    v.pos = glm::vec3(static_cast<float>(piece), 0.0f, 0.0f);
    m->vertices.push_back(v);
    return m;
}

// Review Focus 4: "piece" goes back and forth; piece 0 does not write the field; a node
// with "piece" uses the entry of ITS piece in the cache.
void testPieceRoundTripsThroughJson()
{
    const std::string src = "assets/__no_existe__.gltf";
    DonTopo::Scene scene;
    auto* p0 = scene.addGameObject("p0");
    auto* p1 = scene.addGameObject("p1");
    p0->setMesh(fakePiece(src, 0, "malla0"));
    p1->setMesh(fakePiece(src, 1, "malla1"));
    const nlohmann::json j = scene.toJson();
    const auto& kids = j["root"]["children"];
    CHECK(kids.size() == 2, "dos hijos serializados");
    if (kids.size() != 2) return;
    CHECK(!kids[0]["mesh"].contains("piece"), "la pieza 0 no escribe el campo");
    CHECK(kids[1]["mesh"].value("piece", -1) == 1, "la pieza 1 se guarda");

    DonTopo::PreloadedMeshCache cache;
    cache[DonTopo::meshCacheKey(src, 0)] = fakePiece(src, 0, "cache0");
    cache[DonTopo::meshCacheKey(src, 1)] = fakePiece(src, 1, "cache1");
    DonTopo::Scene back;
    CHECK(back.fromJson(j, physics(), audio(), nullptr, &cache), "recarga con cache");
    DonTopo::GameObject* b0 = nullptr;
    DonTopo::GameObject* b1 = nullptr;
    back.traverse([&](DonTopo::GameObject* go) { if (go->name == "p0") b0 = go; if (go->name == "p1") b1 = go; });
    CHECK(b0 && b0->hasMesh() && b0->getMesh()->name == "cache0", "p0 recibe la pieza 0");
    CHECK(b1 && b1->hasMesh() && b1->getMesh()->name == "cache1" && b1->getMesh()->piece == 1,
          "p1 recibe la pieza 1 y la conserva");
}

// Review Focus 1: what the Delete undo does (collectMeshes + subtreeToJson +
// insertFromJson with that cache) returns to each child ITS mesh.
void testDeleteUndoKeepsEachPiece()
{
    const std::string src = "assets/__no_existe__.gltf";
    DonTopo::Scene scene;
    auto* casa = scene.addGameObject("Casa");
    auto* paredes = scene.addGameObject("Paredes", casa);
    auto* tejado  = scene.addGameObject("Tejado", casa);
    paredes->setMesh(fakePiece(src, 0, "paredes"));
    tejado->setMesh(fakePiece(src, 1, "tejado"));

    const DonTopo::PreloadedMeshCache cache = DonTopo::Scene::collectMeshes(casa);
    const nlohmann::json snap = scene.subtreeToJson(casa);
    scene.removeGameObject(casa);
    DonTopo::GameObject* back = scene.insertFromJson(snap, nullptr, 0, physics(), audio(), &cache);
    CHECK(back && back->children.size() == 2, "Casa vuelve con sus dos hijos");
    if (!back || back->children.size() != 2) return;
    for (const auto& child : back->children)
    {
        CHECK(child->hasMesh(), "cada hijo recupera malla");
        if (!child->hasMesh()) continue;
        const std::string esperado = child->name == "Paredes" ? "paredes" : "tejado";
        CHECK(child->getMesh()->name == esperado, "cada hijo recupera SU malla, no la de su hermano");
    }
}

// Final review: Play Stop reloads the scene without a loader or cache and the synchronous
// static branch did an Assimp ReadFile PER NODE (60 pieces = 60
// reads of the same file). Now there is a StaticModel cache per
// sourcePath with the lifetime of the fromJson call: each node receives ITS piece and
// those that ask for the same piece share the mesh (proof that it came from the
// cache and not from another read). An out-of-range piece is still the same
// failed-load warning as when ModelLoader::load threw.
void testSyncStaticPiecesShareOneLoad()
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "dt_scene_sync_pieces";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path gltf = dir / "casa.gltf";
    dt_fixture::writeThreePieceGltf(gltf);

    nlohmann::json j = nlohmann::json::parse(R"({ "version": 1, "root": { "name": "root", "children": [
        { "name": "a0", "children": [], "mesh": { "sourcePath": "" } },
        { "name": "b1", "children": [], "mesh": { "sourcePath": "", "piece": 1 } },
        { "name": "c0", "children": [], "mesh": { "sourcePath": "", "piece": 0 } },
        { "name": "x9", "children": [], "mesh": { "sourcePath": "", "piece": 9 } } ] } })");
    for (auto& c : j["root"]["children"]) c["mesh"]["sourcePath"] = gltf.string();

    DonTopo::Scene scene;
    CHECK(scene.fromJson(j, physics(), audio(), nullptr, nullptr), "carga sincrona de las piezas");
    DonTopo::GameObject* a0 = nullptr; DonTopo::GameObject* b1 = nullptr;
    DonTopo::GameObject* c0 = nullptr; DonTopo::GameObject* x9 = nullptr;
    scene.traverse([&](DonTopo::GameObject* go) {
        if (go->name == "a0") a0 = go; if (go->name == "b1") b1 = go;
        if (go->name == "c0") c0 = go; if (go->name == "x9") x9 = go; });
    CHECK(a0 && a0->hasMesh() && a0->getMesh()->piece == 0, "a0 recibe la pieza 0");
    CHECK(b1 && b1->hasMesh() && b1->getMesh()->piece == 1, "b1 recibe la pieza 1");
    CHECK(c0 && c0->hasMesh() && c0->getMesh()->piece == 0, "c0 recibe la pieza 0");
    // triB (piece 1) is in the YZ plane: no vertex with x != 0.
    if (b1 && b1->hasMesh())
    {
        bool yz = !b1->getMesh()->vertices.empty();
        for (const auto& v : b1->getMesh()->vertices) yz = yz && v.pos.x == 0.0f;
        CHECK(yz, "la malla de b1 es la de la pieza 1, no la 0");
    }
    CHECK(a0 && c0 && a0->hasMesh() && c0->hasMesh() && a0->getMesh() == c0->getMesh(),
          "la misma pieza sale de UNA carga del fichero, compartida");
    CHECK(x9 && !x9->hasMesh(), "una pieza que no existe deja el nodo sin malla");
    bool aviso = false;
    for (const std::string& w : scene.lastWarnings()) aviso = aviso || w.find("piece 9") != std::string::npos;
    CHECK(aviso, "y avisa como cuando ModelLoader::load lanzaba");
}

} // namespace

int main()
{
    // ONE single JobSystem + AsyncAssetLoader for the whole file, created here and
    // passed by reference, same as in production, where the editor and the
    // runtime create ONE instance of each, alive for the whole app. Before, each test
    // created and destroyed its own (start/shutdown per test): that repeated churn of
    // thread start/stop is what this experiment isolates.
    DonTopo::JobSystem jobSystem;
    jobSystem.start();
    DonTopo::AsyncAssetLoader loader(jobSystem);

    testSyncPathUnchanged(loader);
    testSyncDefaultArgEqualsExplicitNullptr();
    testAsyncCreatesNodesImmediately(loader);
    testDeletedTargetIsDiscarded(loader);
    testPreloadedCacheConsulted();
    testPieceCacheKeys();
    testPieceRoundTripsThroughJson();
    testDeleteUndoKeepsEachPiece();
    testClonNoHeredaIndicesDeRender();
    testSyncStaticPiecesShareOneLoad();

    jobSystem.shutdown();

    if (g_failures == 0)
    {
        std::printf("scene_async_tests OK\n");
        return 0;
    }
    std::fprintf(stderr, "scene_async_tests FAILED: %d checks\n", g_failures);
    return 1;
}
