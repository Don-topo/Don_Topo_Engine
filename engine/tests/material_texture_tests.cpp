// Headless test of the Mesh texture overrides (no GPU). Plain main +
// asserts, no framework, same pattern as content_browser_tests.cpp.
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/MaterialAsset.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Renderer/AsyncAssetLoader.h"
#include "DonTopo/Renderer/MaterialTextureSource.h"
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Editor/Command.h"
#include "DonTopo/Editor/DeferredSlider.h"
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace DonTopo;

static int g_failures = 0;

// Alias of the mesh that does NOT count as an owner (see the same helper in
// animator_tests.cpp): without it, the test's shared_ptr would make editMesh()
// see the mesh as shared and copy it.
template <typename M>
static std::shared_ptr<M> soloObservador(const std::shared_ptr<M>& m)
{
    return std::shared_ptr<M>(m.get(), [](M*) {});
}
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Static with an albedo texture coming from the FBX.
static std::unique_ptr<GameObject> makeStaticFixture()
{
    auto go = std::make_unique<GameObject>("Estatico");
    auto mesh = std::make_shared<Mesh>();
    mesh->name = "cubo";
    mesh->material.texturePath = "assets/fbx_albedo.png";
    go->setMesh(std::move(mesh));
    return go;
}

// Skinned with THREE per-submesh materials, each with its albedo from the FBX.
static std::unique_ptr<GameObject> makeSkinnedFixture()
{
    auto go = std::make_unique<GameObject>("Personaje");
    auto mesh = std::make_shared<SkinnedMesh>();
    mesh->name = "heroe";
    mesh->materials.resize(3);
    mesh->materials[0].texturePath = "assets/cuerpo.png";
    mesh->materials[1].texturePath = "assets/ropa.png";
    mesh->materials[2].texturePath = "assets/pelo.png";
    go->setMesh(std::move(mesh));
    return go;
}

// A static one exposes ONE material: the inherited one. The materials vector of an
// empty SkinnedMesh does not count.
static void test_materials_of_static_mesh()
{
    auto go = makeStaticFixture();
    std::vector<const Material*> mats = materialsOfMesh(*go);
    CHECK(mats.size() == 1);
    if (mats.size() == 1)
        CHECK(mats[0]->texturePath == "assets/fbx_albedo.png");
}

// A skinned one with submeshes exposes ITS materials, not the inherited one.
static void test_materials_of_skinned_mesh()
{
    auto go = makeSkinnedFixture();
    std::vector<const Material*> mats = materialsOfMesh(*go);
    CHECK(mats.size() == 3);
    if (mats.size() == 3)
        CHECK(mats[2]->texturePath == "assets/pelo.png");
}

// Assigning overwrites the material and stores as baseline what the FBX brought.
static void test_override_writes_material_and_captures_baseline()
{
    auto go = makeStaticFixture();
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";
    go->materialOverrides.push_back(ov);

    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.texturePath == "assets/mia.png");
    CHECK(go->materialOverrides[0].baseAlbedo == "assets/fbx_albedo.png");
}

// Changing the texture does NOT move the baseline: it is still the FBX one, not the
// intermediate override. Without this, a Clear after two changes would give back the
// first texture the user set instead of the model's.
static void test_second_override_keeps_original_baseline()
{
    auto go = makeStaticFixture();
    go->materialOverrides.push_back(MaterialOverride{});
    go->materialOverrides[0].index  = 0;
    go->materialOverrides[0].albedo = "assets/primera.png";
    applyMaterialOverrides(*go);

    go->materialOverrides[0].albedo = "assets/segunda.png";
    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.texturePath == "assets/segunda.png");
    CHECK(go->materialOverrides[0].baseAlbedo == "assets/fbx_albedo.png");
}

// Clear = empty the override. The material goes back to the captured baseline.
static void test_clear_restores_baseline()
{
    auto go = makeStaticFixture();
    go->materialOverrides.push_back(MaterialOverride{});
    go->materialOverrides[0].index  = 0;
    go->materialOverrides[0].albedo = "assets/mia.png";
    applyMaterialOverrides(*go);

    go->materialOverrides[0].albedo.clear();
    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.texturePath == "assets/fbx_albedo.png");
}

// The override of one submesh does not touch its neighbors.
static void test_skinned_override_touches_only_its_index()
{
    auto go = makeSkinnedFixture();
    MaterialOverride ov;
    ov.index  = 2;
    ov.albedo = "assets/pelo_rubio.png";
    go->materialOverrides.push_back(ov);

    applyMaterialOverrides(*go);

    const SkinnedMesh* sm = go->getSkinnedMesh();
    CHECK(sm->materials[2].texturePath == "assets/pelo_rubio.png");
    CHECK(sm->materials[0].texturePath == "assets/cuerpo.png");
    CHECK(sm->materials[1].texturePath == "assets/ropa.png");
}

// Index that no longer exists (FBX re-exported with fewer submeshes): it is ignored without
// touching anything and without a crash.
static void test_out_of_range_index_is_ignored()
{
    auto go = makeSkinnedFixture();
    MaterialOverride ov;
    ov.index  = 7;
    ov.albedo = "assets/fantasma.png";
    go->materialOverrides.push_back(ov);

    applyMaterialOverrides(*go);

    const SkinnedMesh* sm = go->getSkinnedMesh();
    CHECK(sm->materials[0].texturePath == "assets/cuerpo.png");
    CHECK(sm->materials[1].texturePath == "assets/ropa.png");
    CHECK(sm->materials[2].texturePath == "assets/pelo.png");
}

// The three slots are independent: overwriting the albedo does not touch normal or ORM.
static void test_slots_are_independent()
{
    auto go = makeStaticFixture();
    go->editMesh()->material.normalMapPath          = "assets/fbx_normal.png";
    go->editMesh()->material.metallicRoughnessPath  = "assets/fbx_orm.png";

    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";
    go->materialOverrides.push_back(ov);
    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.normalMapPath == "assets/fbx_normal.png");
    CHECK(go->getMesh()->material.metallicRoughnessPath == "assets/fbx_orm.png");
}

// The normal override and the ORM one each write their own field, not the
// neighboring one. Without this test, a wrong copy-paste wiring in
// applyMaterialOverrides (e.g. passing mat.texturePath as the destination of the
// normal) would have passed the tests above: none of them touches ov.normal
// or ov.orm.
static void test_normal_and_orm_overrides_write_their_own_field()
{
    auto go = makeStaticFixture();
    go->editMesh()->material.normalMapPath         = "assets/fbx_normal.png";
    go->editMesh()->material.metallicRoughnessPath = "assets/fbx_orm.png";

    MaterialOverride ov;
    ov.index  = 0;
    ov.normal = "assets/mi_normal.png";
    ov.orm    = "assets/mi_orm.png";
    go->materialOverrides.push_back(ov);

    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.normalMapPath == "assets/mi_normal.png");
    CHECK(go->getMesh()->material.metallicRoughnessPath == "assets/mi_orm.png");
    // The albedo, with no override in this slot, has not been touched.
    CHECK(go->getMesh()->material.texturePath == "assets/fbx_albedo.png");
    CHECK(go->materialOverrides[0].baseNormal == "assets/fbx_normal.png");
    CHECK(go->materialOverrides[0].baseOrm    == "assets/fbx_orm.png");
}

// Case that justifies the baseAlbedoTaken/baseNormalTaken/baseOrmTaken flags:
// a procedural mesh (no FBX texture) has an empty texturePath FROM THE
// START, and that legitimate emptiness cannot be told apart from "the baseline has not yet been
// taken" by looking only at whether base* is empty. If applyMaterialOverrides
// used that heuristic instead of the explicit flags, Clear would not restore
// the original emptiness: it would leave in place the texture the user set.
static void test_clear_restores_empty_baseline_on_procedural_mesh()
{
    auto go = std::make_unique<GameObject>("Procedural");
    auto mesh = std::make_shared<Mesh>();
    mesh->name = "esfera";
    // texturePath is left empty on purpose: it does not come from any FBX.
    go->setMesh(std::move(mesh));

    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";
    go->materialOverrides.push_back(ov);
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.texturePath == "assets/mia.png");

    go->materialOverrides[0].albedo.clear();
    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.texturePath.empty());
}

// Task 7: the override is applied to the mesh that ARRIVES, not afterwards: if it
// were applied after registering with the renderer (AsyncAssetLoader::applyLoadedMesh),
// the GPU would upload the FBX texture and the user's would not be seen until the
// next rebuild. This test is the pure mechanism (without EditorRenderer, which
// is 73 pure virtuals); that applyLoadedMesh calls it in the right order
// is verified in the GUI, like test_remove_notifies_listener in
// camera_tests.cpp.
static void test_overrides_applied_to_incoming_mesh()
{
    auto go = makeStaticFixture();
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";
    go->materialOverrides.push_back(ov);

    // The mesh that "arrives" carries what is in the FBX.
    auto llegada = std::make_shared<Mesh>();
    llegada->material.texturePath = "assets/fbx_albedo.png";
    go->setMesh(llegada);

    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.texturePath == "assets/mia.png");
}

// A GameObject without a mesh does not blow up.
static void test_no_mesh_is_noop()
{
    GameObject go("Vacio");
    go.materialOverrides.push_back(MaterialOverride{});
    applyMaterialOverrides(go);
    CHECK(materialsOfMesh(go).empty());
}

// The explicit path WINS over the embedded bytes. It is the opposite of what the
// engine did before this feature, and it is what makes Clear possible: if the
// embedded one won, assigning a texture by hand would require destroying the FBX bytes and
// there would be nothing to go back to.
static void test_path_wins_over_embedded()
{
    const std::vector<uint8_t> bytes{1, 2, 3};
    CHECK(chooseTextureSource("assets/x.png", bytes) == TextureSource::Path);
}

// Without a path, the embedded one.
static void test_embedded_when_no_path()
{
    const std::vector<uint8_t> bytes{1, 2, 3};
    CHECK(chooseTextureSource("", bytes) == TextureSource::Embedded);
}

// With nothing, nothing: the caller puts in its own filler (shared white in Vulkan,
// global neutral in D3D12).
static void test_none_when_empty()
{
    CHECK(chooseTextureSource("", {}) == TextureSource::None);
}

// Path only.
static void test_path_when_no_embedded()
{
    CHECK(chooseTextureSource("assets/x.png", {}) == TextureSource::Path);
}

// Round-trip: the overrides of ALL the indices survive saving and loading.
static void test_overrides_survive_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto mesh = std::make_shared<SkinnedMesh>();
    mesh->sourcePath = "assets/hero.fbx";
    mesh->materials.resize(3);
    go->setMesh(std::move(mesh));

    MaterialOverride a; a.index = 0; a.albedo = "assets/cuerpo.png";
    MaterialOverride b; b.index = 2; b.albedo = "assets/pelo.png";
                               b.normal = "assets/pelo_n.png";
    go->materialOverrides = {a, b};

    const nlohmann::json j = scene.toJson();

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Personaje") leido = n; });
    CHECK(leido != nullptr);
    if (!leido) return;
    CHECK(leido->materialOverrides.size() == 2);
    if (leido->materialOverrides.size() != 2) return;
    CHECK(leido->materialOverrides[0].index  == 0);
    CHECK(leido->materialOverrides[0].albedo == "assets/cuerpo.png");
    CHECK(leido->materialOverrides[1].index  == 2);
    CHECK(leido->materialOverrides[1].normal == "assets/pelo_n.png");
    // The baseline does NOT travel: it is recaptured when applying over the freshly
    // derived material from the FBX.
    CHECK(leido->materialOverrides[0].baseAlbedo.empty());
}

// Review Focus 6: an entry with ONLY matAsset (no textures or factors) is not
// omitted on save, and it survives saving and loading.
static void test_mat_asset_survives_round_trip_alone(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto mesh = std::make_shared<SkinnedMesh>();
    mesh->sourcePath = "assets/hero.fbx";
    mesh->materials.resize(1);
    go->setMesh(std::move(mesh));

    MaterialOverride ov; ov.index = 0; ov.matAsset = "assets/rojo.mat";
    go->materialOverrides = {ov};

    const nlohmann::json j = scene.toJson();
    CHECK(j["root"]["children"][0]["mesh"].contains("materials"));

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Personaje") leido = n; });
    CHECK(leido != nullptr);
    if (!leido) return;
    CHECK(leido->materialOverrides.size() == 1);
    if (leido->materialOverrides.empty()) return;
    CHECK(leido->materialOverrides[0].matAsset == "assets/rojo.mat");
}

// An old scene, without the matAsset field at all, loads the same as today.
static void test_scene_without_mat_asset_field_loads_unchanged(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->material.texturePath = "assets/fbx_albedo.png";
    go->setMesh(std::move(mesh));
    MaterialOverride ov; ov.index = 0; ov.albedo = "assets/override.png";
    go->materialOverrides = {ov};

    nlohmann::json j = scene.toJson();
    CHECK(!j["root"]["children"][0]["mesh"]["materials"][0].contains("matAsset"));

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Cubo") leido = n; });
    CHECK(leido && leido->materialOverrides.size() == 1);
    if (leido) CHECK(leido->materialOverrides[0].matAsset.empty());
}

// An object without overrides does not write the key: old scenes and new ones
// without touched textures are byte for byte equal.
static void test_no_overrides_writes_no_key()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));

    const nlohmann::json j = scene.toJson();
    // The root node hangs from "root"; locate the child by name instead of
    // assuming the index.
    CHECK(!j.dump().empty());
    CHECK(j.dump().find("\"materials\"") == std::string::npos);
}

// Scene WITHOUT the materials key: it loads exactly the same as today, without overrides
// and without warnings.
static void test_scene_without_materials_key_loads_clean(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));
    nlohmann::json j = scene.toJson();

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Cubo") leido = n; });
    CHECK(leido != nullptr);
    if (leido) CHECK(leido->materialOverrides.empty());
}

// With a project root set, a path under it is saved RELATIVE with "/".
static void test_path_under_root_is_stored_relative(PhysicsManager& pm, AudioManager& am)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "dt_mat_root";

    Scene scene("Test");
    scene.setAssetRoot(root.string());
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = (root / "assets" / "x.png").string();
    go->materialOverrides.push_back(ov);

    const std::string texto = scene.toJson().dump();
    CHECK(texto.find("assets/x.png") != std::string::npos);
    CHECK(texto.find(root.string()) == std::string::npos);

    // And when read with the same root it comes back absolute.
    Scene cargada("Vacia");
    cargada.setAssetRoot(root.string());
    CHECK(cargada.fromJson(scene.toJson(), pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Cubo") leido = n; });
    CHECK(leido != nullptr);
    if (leido && leido->materialOverrides.size() == 1)
        CHECK(fs::path(leido->materialOverrides[0].albedo) == (root / "assets" / "x.png"));
}

// A path OUTSIDE the root is saved absolute as is: making it relative would give
// a string of ".." that does not survive moving the project.
static void test_path_outside_root_stays_absolute()
{
    namespace fs = std::filesystem;
    const fs::path root  = fs::temp_directory_path() / "dt_mat_root";
    const fs::path fuera = fs::temp_directory_path() / "dt_otro" / "y.png";

    Scene scene("Test");
    scene.setAssetRoot(root.string());
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = fuera.string();
    go->materialOverrides.push_back(ov);

    const std::string texto = scene.toJson().dump();
    CHECK(texto.find("y.png") != std::string::npos);
    CHECK(texto.find("..") == std::string::npos);
}

// Without a root set (tests, headless runtime), the path goes and comes back IDENTICAL.
static void test_without_root_path_is_verbatim(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/tal/cual.png";
    go->materialOverrides.push_back(ov);

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(scene.toJson(), pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Cubo") leido = n; });
    if (leido && leido->materialOverrides.size() == 1)
        CHECK(leido->materialOverrides[0].albedo == "assets/tal/cual.png");
}

// A corrupt "materials" block does not bring down the load: it warns and carries on.
static void test_corrupt_materials_block_warns(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    // PROCEDURAL mesh (empty sourcePath), not "assets/cubo.fbx": that file does not
    // exist in assets/, so ModelLoader::load would throw, nodeFromJson's catch
    // would push ITS OWN "could not load the mesh" warning and
    // lastWarnings() would already come out non-empty BEFORE looking at the materials block;
    // the CHECK below would pass even if the warning it claims to check were deleted.
    // With a procedural mesh (empty vertices/indices, rebuilt without
    // touching disk) the only possible warning of this load is the one for the corrupt
    // materials block.
    auto mesh = std::make_shared<Mesh>();
    go->setMesh(std::move(mesh));
    nlohmann::json j = scene.toJson();

    // Inject garbage where the block would go: an object instead of an array.
    // Locate the cube node by walking the JSON by name: toJson()
    // hangs the root's children from root->children, each with its own
    // "mesh" (here without "materials" because the Cube has no overrides yet).
    nlohmann::json& hijos = j["root"]["children"];
    nlohmann::json* cuboJson = nullptr;
    for (auto& hijo : hijos)
        if (hijo.value("name", std::string()) == "Cubo") cuboJson = &hijo;
    CHECK(cuboJson != nullptr);
    if (!cuboJson) return;
    (*cuboJson)["mesh"]["materials"] = nlohmann::json::object();

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));
    // The SUBSTRING of the warning that belongs to THIS block, not just "there is some
    // warning": the latter would pass all the same even if the push_back of the
    // "!mats.is_array()" branch were deleted and some other unrelated warning slipped in by chance.
    bool warned = false;
    for (const auto& w : cargada.lastWarnings())
        if (w.find("not a list") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

// An entry without a valid "index" is discarded with a warning, without dropping the others
// of the same array: same hand-edited file that only corrupts one entry.
static void test_materials_entry_without_valid_index_is_discarded(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/valida.png";
    go->materialOverrides.push_back(ov);

    nlohmann::json j = scene.toJson();
    nlohmann::json& hijos = j["root"]["children"];
    nlohmann::json* cuboJson = nullptr;
    for (auto& hijo : hijos)
        if (hijo.value("name", std::string()) == "Cubo") cuboJson = &hijo;
    CHECK(cuboJson != nullptr);
    if (!cuboJson) return;
    CHECK((*cuboJson)["mesh"].contains("materials"));
    // A second entry without "index" is added: it has to be discarded ALONE,
    // leaving the first one alive.
    (*cuboJson)["mesh"]["materials"].push_back({ {"albedo", "assets/sin_indice.png"} });

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Cubo") leido = n; });
    CHECK(leido != nullptr);
    if (!leido) return;
    CHECK(leido->materialOverrides.size() == 1);
    if (leido->materialOverrides.size() == 1)
        CHECK(leido->materialOverrides[0].albedo == "assets/valida.png");

    bool warned = false;
    for (const auto& w : cargada.lastWarnings())
        if (w.find("index") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

// GUARD FOR THE DECISION "in the memory paths the path travels verbatim"
// (review round 2): with the root SET in the scene, cloning an object
// whose override is an ABSOLUTE path under that root has to leave the clone with the
// SAME string, byte for byte: neither relativized, nor with the separators
// rewritten by the relative()/weakly_canonical trip of toStoredPath. Without
// this test, handing m_assetRoot back to cloneGameObject leaves the suite green
// while the clone receives a different path from the original's.
static void test_clone_keeps_override_path_verbatim_with_root_set(PhysicsManager& pm, AudioManager& am)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "dt_mat_root";

    Scene scene("Test");
    scene.setAssetRoot(root.string());
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = (root / "assets" / "x.png").string();
    go->materialOverrides.push_back(ov);

    const std::string original = go->materialOverrides[0].albedo;

    GameObject* clone = scene.cloneGameObject(go, nullptr, pm, am);
    CHECK(clone != nullptr);
    if (!clone) return;
    CHECK(clone->materialOverrides.size() == 1);
    if (clone->materialOverrides.size() == 1)
        CHECK(clone->materialOverrides[0].albedo == original);
}

// H15: cloning a procedural mesh shares the live geometry (no per-vertex JSON
// round trip), and editing the clone's material copies it without touching
// the source. Checked on a child too, since the memRef travels in the subtree.
static void test_clone_shares_procedural_mesh(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Sphere");
    auto mesh = std::make_shared<Mesh>();
    mesh->name = "sphere";
    mesh->vertices.resize(3);
    mesh->indices = { 0, 1, 2 };
    go->setMesh(std::move(mesh));
    GameObject* child = go->addChild("Child");
    auto childMesh = std::make_shared<Mesh>();
    childMesh->indices = { 0 };
    childMesh->vertices.resize(1);
    child->setMesh(std::move(childMesh));

    GameObject* clone = scene.cloneGameObject(go, nullptr, pm, am);
    CHECK(clone != nullptr);
    if (!clone) return;
    CHECK(clone->getMesh() == go->getMesh());
    CHECK(clone->children.size() == 1);
    if (clone->children.size() == 1)
        CHECK(clone->children[0]->getMesh() == child->getMesh());
    CHECK(scene.lastWarnings().empty());

    clone->editMesh()->material.texturePath = "assets/mine.png";
    CHECK(clone->getMesh() != go->getMesh());
    CHECK(go->getMesh()->material.texturePath.empty());
    CHECK(clone->getMesh()->indices.size() == 3);
}

// Same guard for the subtreeToJson/insertFromJson pair used by the Undo/Redo of
// Create/Delete: the full cycle (capture snapshot, delete the original,
// reinsert from the snapshot) has to return the identical path.
static void test_undo_redo_keeps_override_path_verbatim_with_root_set(PhysicsManager& pm, AudioManager& am)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "dt_mat_root";

    Scene scene("Test");
    scene.setAssetRoot(root.string());
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = (root / "assets" / "y.png").string();
    go->materialOverrides.push_back(ov);

    const std::string original = go->materialOverrides[0].albedo;
    const nlohmann::json snapshot = scene.subtreeToJson(go);

    // The real cycle of a Delete Undo: the node is destroyed and rebuilt
    // from the snapshot captured BEFORE deleting it.
    scene.removeGameObject(go);
    GameObject* restored = scene.insertFromJson(snapshot, nullptr, 0, pm, am);
    CHECK(restored != nullptr);
    if (!restored) return;
    CHECK(restored->materialOverrides.size() == 1);
    if (restored->materialOverrides.size() == 1)
        CHECK(restored->materialOverrides[0].albedo == original);
}

// Task 7: nodeFromJson has to call applyMaterialOverrides with the mesh
// ALREADY SET, for each of the three load branches (here, the procedural one).
// Without that call, an object freshly loaded from disk looks like the FBX's texture
// until the first edit that triggers an external applyMaterialOverrides.
static void test_scene_load_applies_override_to_material(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();   // procedural: empty sourcePath
    go->setMesh(std::move(mesh));
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";
    go->materialOverrides.push_back(ov);

    const nlohmann::json j = scene.toJson();
    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Cubo") leido = n; });
    CHECK(leido != nullptr);
    if (!leido) return;
    CHECK(leido->hasMesh());
    if (leido->hasMesh())
        CHECK(leido->getMesh()->material.texturePath == "assets/mia.png");
}

// Task 7, review point 2: the comment of GameObject::applyMaterialOverrides
// promises that "the warning [for an out-of-range index] is given by the scene reader,
// which is the one that has a channel to give it". This test is that promise kept: an
// index that no longer exists in the freshly loaded mesh leaves a warning in
// lastWarnings(), not a silent failure.
static void test_out_of_range_index_warns_on_scene_load(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();   // procedural: exposes ONE material (index 0)
    go->setMesh(std::move(mesh));
    MaterialOverride ov;
    ov.index  = 3;   // out of range: the procedural mesh only has index 0
    ov.albedo = "assets/fantasma.png";
    go->materialOverrides.push_back(ov);

    const nlohmann::json j = scene.toJson();
    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));

    // The SUBSTRING of THIS warning, not just "there is some warning": same criterion
    // as test_corrupt_materials_block_warns.
    bool warned = false;
    for (const auto& w : cargada.lastWarnings())
        if (w.find("out of range") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

// The same warning, but through the ASYNCHRONOUS path. The one above only covers
// Scene::fromJson; a large scene loads its meshes through the pump
// (AsyncAssetLoader::applyLoadedMesh), which called applyMaterialOverrides
// plain and swallowed the invalid index without a line. The warning now lives in
// collectMaterialOverrideWarnings, which is what is tested here: both
// paths say the same because they call the same function.
static void test_override_warnings_helper()
{
    GameObject go("Cubo");
    auto mesh = std::make_shared<Mesh>();   // procedural: ONE material (index 0)
    go.setMesh(std::move(mesh));

    MaterialOverride dentro;
    dentro.index  = 0;
    dentro.albedo = "assets/mia.png";
    go.materialOverrides.push_back(dentro);

    std::vector<std::string> avisos;
    collectMaterialOverrideWarnings(go, avisos);
    // A valid index says nothing: if it always warned, the warning would not
    // distinguish anything and the async path would fill with noise for every
    // object with overrides in a large scene.
    CHECK(avisos.empty());

    MaterialOverride fuera;
    fuera.index  = 3;
    fuera.albedo = "assets/fantasma.png";
    go.materialOverrides.push_back(fuera);

    collectMaterialOverrideWarnings(go, avisos);
    CHECK(avisos.size() == 1);
    if (avisos.size() == 1)
    {
        // The text, not just the number: it is what the user reads in the Log, and
        // it has to say WHICH object and index it is talking about.
        CHECK(avisos[0].find("Cubo") != std::string::npos);
        CHECK(avisos[0].find("index 3") != std::string::npos);
        CHECK(avisos[0].find("out of range") != std::string::npos);
    }

    // A negative counts the same as an index past the end: applyMaterialOverrides discards them
    // by the same condition.
    MaterialOverride negativo;
    negativo.index  = -1;
    negativo.albedo = "assets/otro.png";
    go.materialOverrides.push_back(negativo);
    avisos.clear();
    collectMaterialOverrideWarnings(go, avisos);
    CHECK(avisos.size() == 2);

    // Without a mesh there are no materials to compare against: it is not that all the
    // indices are out of range, it is that the question does not apply yet (the
    // pump calls this AFTER setMesh, but the order is guaranteed by the
    // caller, not by this function).
    GameObject sinMalla("Vacio");
    sinMalla.materialOverrides.push_back(fuera);
    avisos.clear();
    collectMaterialOverrideWarnings(sinMalla, avisos);
    CHECK(avisos.empty());
}

// Task 7, review point 1: the clone trap. cloneGameObject seeds
// the clone's mesh from a PreloadedMeshCache with the original's LIVE mesh,
// that is, with the material ALREADY OVERWRITTEN by the override. Without the
// real baseline traveling in the cloning JSON (carryOverrideBaseline), the
// clone would capture the override's texture as "original", and a Clear on
// the clone would leave that texture in place instead of returning the FBX's.
static void test_clone_clear_restores_fbx_texture_not_override(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    // Non-empty sourcePath: it is what makes cloneGameObject use the
    // PreloadedMeshCache (meshes) instead of going to disk (which would fail, the
    // file does not exist, and the clone would be left without a mesh).
    mesh->sourcePath = "assets/cubo.fbx";
    mesh->material.texturePath = "assets/fbx_albedo.png";
    go->setMesh(std::move(mesh));

    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";
    go->materialOverrides.push_back(ov);
    // go's LIVE material already carries the baked override, just like an
    // object edited in the editor before duplicating it.
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.texturePath == "assets/mia.png");

    GameObject* clone = scene.cloneGameObject(go, nullptr, pm, am);
    CHECK(clone != nullptr);
    if (!clone) return;
    CHECK(clone->materialOverrides.size() == 1);
    if (clone->materialOverrides.empty()) return;

    // Clear on the CLONE, not on the original.
    clone->materialOverrides[0].albedo.clear();
    applyMaterialOverrides(*clone);

    CHECK(clone->getMesh()->material.texturePath == "assets/fbx_albedo.png");
}

// Task 7 review round, point 1 (Critical): r.images carries the pixels
// the worker decoded from the FBX BEFORE any override overwrote anything
// (AsyncAssetLoader::runJob), and Renderer::createSharedGpuMesh (Vulkan)
// PREFERS those already decoded pixels over the material's path; without
// filtering them, an override on an FBX with its own texture would upload the
// FBX's to the GPU. This is the only seam of that bug that can be tested without a GPU: the
// filtering function itself, isolated from applyLoadedMesh (which does need a
// real EditorRenderer and is left uncovered at test level, like the
// rest of that function; it is verified in the GUI). It checks that ONLY the slot
// that the override overwrites is discarded, leaving the others intact so that the
// worker's work is not thrown away entirely.
static void test_discard_overridden_decoded_images_removes_only_overridden_slot()
{
    DecodedImage albedo; albedo.slot = DecodedImage::Albedo; albedo.w = 1; albedo.h = 1; albedo.pixels = {1, 2, 3, 4};
    DecodedImage normal; normal.slot = DecodedImage::Normal; normal.w = 1; normal.h = 1; normal.pixels = {5, 6, 7, 8};
    DecodedImage orm;    orm.slot    = DecodedImage::ORM;    orm.w    = 1; orm.h    = 1; orm.pixels = {9, 10, 11, 12};
    std::vector<DecodedImage> images{albedo, normal, orm};

    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";   // only the albedo is overridden
    std::vector<MaterialOverride> overrides{ov};

    discardOverriddenDecodedImages(images, overrides);

    CHECK(images.size() == 2);
    bool hasAlbedo = false, hasNormal = false, hasOrm = false;
    for (const DecodedImage& img : images)
    {
        if (img.slot == DecodedImage::Albedo) hasAlbedo = true;
        if (img.slot == DecodedImage::Normal) hasNormal = true;
        if (img.slot == DecodedImage::ORM)    hasOrm    = true;
    }
    CHECK(!hasAlbedo);
    CHECK(hasNormal);
    CHECK(hasOrm);
}

// r.images only decodes Mesh::material (index 0, see the comment of
// runJob): an override of ANOTHER index (multi-material SkinnedMesh) has
// nothing to discard in this vector.
static void test_discard_overridden_decoded_images_ignores_other_index()
{
    DecodedImage albedo; albedo.slot = DecodedImage::Albedo;
    std::vector<DecodedImage> images{albedo};
    MaterialOverride ov;
    ov.index  = 2;
    ov.albedo = "assets/mia.png";
    std::vector<MaterialOverride> overrides{ov};

    discardOverriddenDecodedImages(images, overrides);

    CHECK(images.size() == 1);
}

static void test_discard_overridden_decoded_images_considers_mat_asset()
{
    std::error_code ec;
    const std::filesystem::path d = std::filesystem::temp_directory_path(ec) / "dt_discard_matasset";
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d, ec);
    MaterialAsset m; m.normal = (d / "n.png").string();   // solo normal
    std::string err;
    CHECK(saveMaterialAsset(d / "x.mat", m, &err));

    DecodedImage albedo; albedo.slot = DecodedImage::Albedo;
    DecodedImage normal; normal.slot = DecodedImage::Normal;
    DecodedImage orm;    orm.slot    = DecodedImage::ORM;
    std::vector<DecodedImage> images{albedo, normal, orm};

    MaterialOverride ov; ov.index = 0; ov.matAsset = (d / "x.mat").string();   // without overrides of its own
    std::vector<MaterialOverride> overrides{ov};

    discardOverriddenDecodedImages(images, overrides);

    CHECK(images.size() == 2);
    bool hasAlbedo = false, hasNormal = false;
    for (const DecodedImage& img : images)
    {
        if (img.slot == DecodedImage::Albedo) hasAlbedo = true;
        if (img.slot == DecodedImage::Normal) hasNormal = true;
    }
    CHECK(hasAlbedo);    // the .mat provides no albedo: the FBX's decoded one stays
    CHECK(!hasNormal);   // the .mat DOES provide normal: the FBX's is discarded
}

// Task 7 review round, point 2 (Important): the baseline belongs to
// THE MESH it came from. setMesh is the only point through which the
// mesh changes, so it has to reset the base*/base*Taken there; if
// they survived a mesh change (to null, or to a different one), a later Clear
// would return the texture of a model that is no longer the one
// loaded.
static void test_set_mesh_resets_stale_baseline()
{
    auto go = makeStaticFixture();
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";
    go->materialOverrides.push_back(ov);
    applyMaterialOverrides(*go);
    CHECK(go->materialOverrides[0].baseAlbedoTaken);
    CHECK(go->materialOverrides[0].baseAlbedo == "assets/fbx_albedo.png");

    // setMesh(nullptr): what the catch of AsyncAssetLoader::applyLoadedMesh does
    // (before this fix, it restored the mesh but not the baseline) and what
    // Renderer::removeMeshComponent does when removing the component.
    go->setMesh(nullptr);

    CHECK(!go->materialOverrides[0].baseAlbedoTaken);
    CHECK(go->materialOverrides[0].baseAlbedo.empty());
    // The override itself is still alive (Remove does not delete it): only the baseline
    // is invalidated.
    CHECK(go->materialOverrides[0].albedo == "assets/mia.png");
}

// Same mechanism as the test above, but end to end with the exact
// scenario the review described: a failed load captures a baseline of a
// mesh that never takes hold, and the next load (a different FBX) has to
// capture ITS OWN, not inherit the earlier one.
static void test_reload_after_failed_load_recaptures_correct_baseline()
{
    auto go = makeStaticFixture();   // material.texturePath == "assets/fbx_albedo.png"
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";
    go->materialOverrides.push_back(ov);
    applyMaterialOverrides(*go);
    CHECK(go->materialOverrides[0].baseAlbedo == "assets/fbx_albedo.png");

    // Simulates the catch of applyLoadedMesh: the GPU registration threw, the
    // setMesh is undone. Without the setMesh reset, the baseline above
    // ("assets/fbx_albedo.png") would survive here even though that mesh never
    // ended up loaded.
    go->setMesh(nullptr);

    // Simulates the next attempt with ANOTHER FBX, this time successful: setMesh
    // followed by applyMaterialOverrides, same as applyLoadedMesh does.
    auto otraMalla = std::make_shared<Mesh>();
    otraMalla->material.texturePath = "assets/otro_fbx.png";
    go->setMesh(otraMalla);
    applyMaterialOverrides(*go);

    CHECK(go->materialOverrides[0].baseAlbedo == "assets/otro_fbx.png");
    CHECK(go->getMesh()->material.texturePath == "assets/mia.png");

    // Clear: without the fix, this returned "assets/fbx_albedo.png" (the mesh that
    // never got loaded), not that of the FBX actually loaded.
    go->materialOverrides[0].albedo.clear();
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.texturePath == "assets/otro_fbx.png");
}

// Undo/redo of an assignment, with the renderer at nullptr (no GPU): what is
// tested is the data, which is the only thing that survives the cycle.
static void test_set_material_asset_override_creates_entry_and_applies()
{
    auto go = makeStaticFixture();
    setMaterialAssetOverride(*go, 0, "assets/rojo.mat");
    CHECK(go->materialOverrides.size() == 1);
    CHECK(go->materialOverrides[0].matAsset == "assets/rojo.mat");
}

static void test_material_asset_command_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    go->setMesh(std::make_shared<Mesh>());
    const uint64_t id = go->id;

    MaterialAssetCommand cmd(scene, nullptr, "Material de 'Cubo'", id, 0, "", "assets/rojo.mat");
    cmd.execute();
    CHECK(scene.findById(id)->materialOverrides[0].matAsset == "assets/rojo.mat");

    cmd.undo();
    CHECK(scene.findById(id)->materialOverrides[0].matAsset.empty());

    cmd.execute();   // redo
    CHECK(scene.findById(id)->materialOverrides[0].matAsset == "assets/rojo.mat");
}

static void test_command_undo_redo_assignment(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->material.texturePath = "assets/fbx_albedo.png";
    go->setMesh(std::move(mesh));
    const uint64_t id = go->id;

    MaterialTextureCommand cmd(scene, nullptr, "Textura de 'Cubo'", id, 0,
                                MaterialTextureSlot::Albedo, "", "assets/mia.png");
    cmd.execute();
    CHECK(scene.findById(id)->getMesh()->material.texturePath == "assets/mia.png");

    cmd.undo();
    CHECK(scene.findById(id)->getMesh()->material.texturePath == "assets/fbx_albedo.png");

    cmd.execute();
    CHECK(scene.findById(id)->getMesh()->material.texturePath == "assets/mia.png");
    (void)pm; (void)am;
}

// Undo of a Clear: puts back the path the user had assigned.
static void test_command_undo_of_clear(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->material.texturePath = "assets/fbx_albedo.png";
    go->setMesh(std::move(mesh));
    const uint64_t id = go->id;

    setMaterialTextureOverride(*go, 0, MaterialTextureSlot::Albedo, "assets/mia.png");

    MaterialTextureCommand clear(scene, nullptr, "Quitar textura", id, 0,
                                  MaterialTextureSlot::Albedo, "assets/mia.png", "");
    clear.execute();
    CHECK(scene.findById(id)->getMesh()->material.texturePath == "assets/fbx_albedo.png");

    clear.undo();
    CHECK(scene.findById(id)->getMesh()->material.texturePath == "assets/mia.png");
    (void)pm; (void)am;
}

// The command resolves by id on EVERY application: a stored pointer would be left
// dangling after a Delete undo that rebuilds the object.
static void test_command_survives_object_rebuild(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->material.texturePath = "assets/fbx_albedo.png";
    go->setMesh(std::move(mesh));
    const uint64_t id = go->id;

    MaterialTextureCommand cmd(scene, nullptr, "Textura", id, 0,
                                MaterialTextureSlot::Albedo, "", "assets/mia.png");

    // The object disappears: the command cannot blow up or write to freed
    // memory, it must just do nothing.
    scene.removeGameObject(scene.findById(id));
    cmd.execute();
    CHECK(scene.findById(id) == nullptr);
    (void)pm; (void)am;
}

// Without setMesh: the object has no material to apply anything to. Without the
// !go->hasMesh() guard in apply(), setMaterialTextureOverride would still create
// an orphan entry in materialOverrides -- an override written on an
// object that has no mesh, with no visible effect and nothing to give it away.
static void test_command_on_object_without_mesh_is_noop(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("SinMalla");
    const uint64_t id = go->id;

    MaterialTextureCommand cmd(scene, nullptr, "Textura", id, 0,
                                MaterialTextureSlot::Albedo, "", "assets/mia.png");
    cmd.execute();
    CHECK(scene.findById(id)->materialOverrides.empty());
    (void)pm; (void)am;
}

// The index was valid when the command was built (skinned with 3
// materials, index 2 = "pelo"), but before undo/redo replays it
// the mesh changes to a flat Mesh of a SINGLE material -- the same scenario
// that the guard comment in Command.cpp describes. Without
// "m_materialIndex >= mats.size()" in apply(), setMaterialTextureOverride
// would still create an orphan entry with index=2 in materialOverrides (the
// cut in applyMaterialOverrides itself avoids the out-of-range write
// into the Material, but does not avoid the orphan entry): the object is left with a
// serializable override that describes no real material of the current
// mesh, silent until the next save.
static void test_command_stale_index_after_mesh_shrinks(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto skinned = std::make_shared<SkinnedMesh>();
    skinned->materials.resize(3);
    skinned->materials[0].texturePath = "assets/cuerpo.png";
    skinned->materials[1].texturePath = "assets/ropa.png";
    skinned->materials[2].texturePath = "assets/pelo.png";
    go->setMesh(skinned);
    const uint64_t id = go->id;

    // Command built for index 2 ("pelo") while the mesh is
    // skinned with 3 materials.
    MaterialTextureCommand cmd(scene, nullptr, "Textura de pelo", id, 2,
                                MaterialTextureSlot::Albedo, "", "assets/mia.png");

    // The mesh is replaced by a flat static Mesh: a SINGLE material, without
    // the command finding out -- exactly the reordering that the
    // guard comment warns about.
    auto plano = std::make_shared<Mesh>();
    plano->material.texturePath = "assets/plano.png";
    go->setMesh(plano);

    cmd.execute();
    CHECK(scene.findById(id)->materialOverrides.empty());
    CHECK(scene.findById(id)->getMesh()->material.texturePath == "assets/plano.png");

    cmd.undo();
    CHECK(scene.findById(id)->materialOverrides.empty());
    CHECK(scene.findById(id)->getMesh()->material.texturePath == "assets/plano.png");
    (void)pm; (void)am;
}

// ---------------------------------------------------------------------------
// PBR factors (metallic/roughness) of the material: same mechanism as the
// three texture slots above, but with a float sentinel (-1.0f, outside
// the slider's 0..1 range) instead of an empty string; see the big note
// of MaterialOverride in GameObject.h.
// ---------------------------------------------------------------------------

// Assigning a factor overwrites the material and stores as baseline what the
// model brought (here, the Material defaults: metallic=0.0f, roughness=0.5f). The
// factor that is not touched stays at its sentinel, with no baseline captured.
static void test_factor_override_writes_material_and_captures_baseline()
{
    auto go = makeStaticFixture();
    MaterialOverride ov;
    ov.index    = 0;
    ov.metallic = 0.8f;
    go->materialOverrides.push_back(ov);

    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.metallic == 0.8f);
    CHECK(go->materialOverrides[0].baseMetallic == 0.0f);
    CHECK(go->materialOverrides[0].baseMetallicTaken);
    // roughness was not touched: it is still at the sentinel, and the material stays with
    // the model's default, not with 0.0.
    CHECK(go->materialOverrides[0].roughness < 0.0f);
    CHECK(!go->materialOverrides[0].baseRoughnessTaken);
    CHECK(go->getMesh()->material.roughness == 0.5f);
}

// Changing the value does NOT move the baseline: it is still the model's, not the
// first value the user set. Same reason as
// test_second_override_keeps_original_baseline with the textures.
static void test_factor_second_change_keeps_original_baseline()
{
    auto go = makeStaticFixture();
    go->materialOverrides.push_back(MaterialOverride{});
    go->materialOverrides[0].index    = 0;
    go->materialOverrides[0].metallic = 0.3f;
    applyMaterialOverrides(*go);

    go->materialOverrides[0].metallic = 0.9f;
    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.metallic == 0.9f);
    CHECK(go->materialOverrides[0].baseMetallic == 0.0f);
}

// Clear (the -1.0f sentinel) = the material goes back to the captured baseline, that
// is, to the model's value. There is no dedicated "Clear" button for the factors in
// the panel (unlike the textures), but the data mechanism is the
// same and is tested the same way, directly on the override.
static void test_factor_clear_restores_baseline()
{
    auto go = makeStaticFixture();
    go->materialOverrides.push_back(MaterialOverride{});
    go->materialOverrides[0].index    = 0;
    go->materialOverrides[0].metallic = 0.75f;
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.metallic == 0.75f);

    go->materialOverrides[0].metallic = -1.0f;
    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.metallic == 0.0f);
}

// A factor's baseline belongs to THE MESH it came from, just like a
// texture's: setMesh is the only point through which the mesh changes, so
// it has to reset baseMetallicTaken/baseRoughnessTaken there.
static void test_set_mesh_resets_stale_factor_baseline()
{
    auto go = makeStaticFixture();
    MaterialOverride ov;
    ov.index    = 0;
    ov.metallic = 0.5f;
    go->materialOverrides.push_back(ov);
    applyMaterialOverrides(*go);
    CHECK(go->materialOverrides[0].baseMetallicTaken);

    go->setMesh(nullptr);

    CHECK(!go->materialOverrides[0].baseMetallicTaken);
    // The override itself is still alive (Remove does not delete it): only the baseline
    // is invalidated.
    CHECK(go->materialOverrides[0].metallic == 0.5f);
}

// Round-trip: metallic AND roughness survive saving and loading.
static void test_factor_overrides_survive_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    // Procedural: "assets/cubo.fbx" does not exist on disk (see the comment of
    // test_factor_absent_in_json_does_not_touch_material, further down) and this
    // test DOES need the reload to leave a mesh set in order to check the
    // material.
    auto mesh = std::make_shared<Mesh>();
    go->setMesh(std::move(mesh));
    MaterialOverride ov;
    ov.index     = 0;
    ov.metallic  = 0.6f;
    ov.roughness = 0.2f;
    go->materialOverrides = {ov};

    const nlohmann::json j = scene.toJson();

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Cubo") leido = n; });
    CHECK(leido != nullptr);
    if (!leido) return;
    CHECK(leido->materialOverrides.size() == 1);
    if (leido->materialOverrides.empty()) return;
    CHECK(leido->materialOverrides[0].metallic  == 0.6f);
    CHECK(leido->materialOverrides[0].roughness == 0.2f);
    // The baseline does NOT travel through disk: the assertion that really proves it is
    // that the STRING "baseMetallic"/"baseRoughness" does not appear in the JSON itself
    // (gated by carryOverrideBaseline, which Scene::toJson() never
    // sets to true); same criterion as
    // test_factor_absent_in_json_does_not_touch_material with "metallic".
    // Review round: checking baseMetallicTaken/baseMetallic of the RELOADED
    // object discriminates nothing, because in this fixture the source already
    // has baseMetallicTaken=false and baseMetallic=0.0f; if the bug
    // also wrote those keys to disk, READING THEM would give the same two
    // values (recapturing from a fresh Material with metallic=0.0 produces
    // exactly "taken=true, base=0.0" just as if those values
    // came from the JSON), and the test would pass with the bug in place.
    CHECK(j.dump().find("baseMetallic")  == std::string::npos);
    CHECK(j.dump().find("baseRoughness") == std::string::npos);
    // These two are still true and document the real behavior
    // (recapture on load, not reading from the file), but they are not the ones that
    // defend against the regression; the ones above are.
    CHECK(leido->materialOverrides[0].baseMetallicTaken);
    CHECK(leido->materialOverrides[0].baseMetallic == 0.0f);
    // And already applied onto the material (nodeFromJson calls
    // applyMaterialOverrides with the mesh set, Task 7).
    CHECK(leido->hasMesh());
    if (leido->hasMesh())
    {
        CHECK(leido->getMesh()->material.metallic  == 0.6f);
        CHECK(leido->getMesh()->material.roughness == 0.2f);
    }
}

// A metallic/roughness absent from the JSON does not touch the material: it stays with
// the model's default, it is not forced to 0. The albedo override IS
// present (so that the "materials" block exists in the JSON) but without the
// "metallic"/"roughness" keys.
static void test_factor_absent_in_json_does_not_touch_material(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    // Procedural (empty sourcePath): "assets/cubo.fbx" does not exist on disk, and
    // with a sourcePath that does not resolve, ModelLoader::load throws and the catch of
    // nodeFromJson leaves the node without a mesh -- exactly what this test needs
    // to avoid in order to check the material after the reload (same reason
    // documented by test_corrupt_materials_block_warns above).
    auto mesh = std::make_shared<Mesh>();
    go->setMesh(std::move(mesh));
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";
    go->materialOverrides.push_back(ov);

    const std::string texto = scene.toJson().dump();
    CHECK(texto.find("\"metallic\"") == std::string::npos);
    CHECK(texto.find("\"roughness\"") == std::string::npos);

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(scene.toJson(), pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Cubo") leido = n; });
    CHECK(leido != nullptr);
    if (!leido || !leido->hasMesh()) return;
    CHECK(leido->getMesh()->material.metallic  == 0.0f);
    CHECK(leido->getMesh()->material.roughness == 0.5f);
    CHECK(leido->materialOverrides[0].metallic  < 0.0f);
    CHECK(leido->materialOverrides[0].roughness < 0.0f);
}

// Undo/redo of a MaterialFactorCommand, modeled on
// test_command_undo_redo_assignment with the textures: the "before" is the
// -1.0f sentinel (no override), just as "" is for a path.
static void test_factor_command_undo_redo(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    go->setMesh(std::move(mesh));
    const uint64_t id = go->id;

    MaterialFactorCommand cmd(scene, nullptr, "Metallic de 'Cubo'", id, 0,
                               MaterialFactorSlot::Metallic, -1.0f, 0.8f);
    cmd.execute();
    CHECK(scene.findById(id)->getMesh()->material.metallic == 0.8f);

    cmd.undo();
    CHECK(scene.findById(id)->getMesh()->material.metallic == 0.0f);

    cmd.execute();
    CHECK(scene.findById(id)->getMesh()->material.metallic == 0.8f);
    (void)pm; (void)am;
}

// Review round: the "before" of a MaterialFactorCommand built by
// the panel has to be the RAW OVERRIDE (the sentinel, if the slot was never
// touched), NOT the already applied EFFECTIVE value (mat.metallic) --
// exactly the bug that PropertiesPanel::drawTexturesSection had before
// this fix (before currentFactorOverride() replaced mat.metallic
// as the "before" in the command construction). With the effective one as
// "before", undoing the FIRST edit on a never-touched factor
// would write the model's value AS AN ACTIVE OVERRIDE -- and nodeToJson DOES
// serialize an active override: the factor would be pinned in the .scene
// forever, resurrecting on every load even though the user never
// touched it (and silently overwriting a different metallic if the FBX is
// re-exported later).
static void test_factor_command_undo_of_first_edit_leaves_no_active_override(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    // No override yet: the state that currentFactorOverride() reads as
    // "-1.0f, no override" and that mat.metallic reads as "0.0f, the default"
    // -- the two readings the bug confused.
    go->setMesh(std::move(mesh));
    const uint64_t id = go->id;
    CHECK(go->getMesh()->material.metallic == 0.0f);   // el EFECTIVO
    CHECK(go->materialOverrides.empty());               // the RAW override: none

    // before = -1.0f (the sentinel; what currentFactorOverride() returns
    // for a slot with no entry in materialOverrides), NOT 0.0f (what
    // mat.metallic would return, the bug).
    MaterialFactorCommand cmd(scene, nullptr, "Metallic de 'Cubo'", id, 0,
                               MaterialFactorSlot::Metallic, -1.0f, 0.8f);
    cmd.execute();
    CHECK(scene.findById(id)->materialOverrides.size() == 1);
    if (scene.findById(id)->materialOverrides.size() == 1)
        CHECK(scene.findById(id)->materialOverrides[0].metallic == 0.8f);

    cmd.undo();

    // With the bug (before=0.0f, the effective one) this would have left
    // materialOverrides[0].metallic == 0.0f -- an ACTIVE override with the
    // model's value, not the sentinel. With the fix, it goes back to -1.0f: no
    // override.
    GameObject* despues = scene.findById(id);
    CHECK(despues->materialOverrides.size() == 1);
    if (!despues->materialOverrides.empty())
        CHECK(despues->materialOverrides[0].metallic < 0.0f);
    CHECK(despues->getMesh()->material.metallic == 0.0f);

    // And the JSON round-trip does not resurrect it: with no active override (the
    // sentinel), nodeToJson does not write the "metallic" key for this
    // object, so a reload brings nothing back.
    const std::string texto = scene.toJson().dump();
    CHECK(texto.find("\"metallic\"") == std::string::npos);

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(scene.toJson(), pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Cubo") leido = n; });
    CHECK(leido != nullptr);
    if (leido && leido->hasMesh())
        CHECK(leido->getMesh()->material.metallic == 0.0f);
}

// Without setMesh: same criterion as test_command_on_object_without_mesh_is_noop.
static void test_factor_command_on_object_without_mesh_is_noop(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("SinMalla");
    const uint64_t id = go->id;

    MaterialFactorCommand cmd(scene, nullptr, "Metallic", id, 0,
                               MaterialFactorSlot::Metallic, -1.0f, 0.8f);
    cmd.execute();
    CHECK(scene.findById(id)->materialOverrides.empty());
    (void)pm; (void)am;
}

// Valid index when building the command, mesh shrunk before the replay:
// same scenario as test_command_stale_index_after_mesh_shrinks.
static void test_factor_command_stale_index_after_mesh_shrinks(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto skinned = std::make_shared<SkinnedMesh>();
    skinned->materials.resize(3);
    go->setMesh(skinned);
    const uint64_t id = go->id;

    MaterialFactorCommand cmd(scene, nullptr, "Metallic de pelo", id, 2,
                               MaterialFactorSlot::Metallic, -1.0f, 0.8f);

    auto plano = std::make_shared<Mesh>();
    go->setMesh(plano);

    cmd.execute();
    CHECK(scene.findById(id)->materialOverrides.empty());

    cmd.undo();
    CHECK(scene.findById(id)->materialOverrides.empty());
    (void)pm; (void)am;
}

// The object disappears between building the command and executing it: same
// criterion as test_command_survives_object_rebuild.
static void test_factor_command_survives_object_rebuild(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    go->setMesh(std::move(mesh));
    const uint64_t id = go->id;

    MaterialFactorCommand cmd(scene, nullptr, "Metallic", id, 0,
                               MaterialFactorSlot::Metallic, -1.0f, 0.8f);

    scene.removeGameObject(scene.findById(id));
    cmd.execute();
    CHECK(scene.findById(id) == nullptr);
    (void)pm; (void)am;
}

// GUARD for the same finding as the texture baseline on clone (see
// test_clone_clear_restores_fbx_texture_not_override): without
// baseMetallic/baseMetallicTaken traveling in the MEMORY JSON
// (carryOverrideBaseline, the one cloneGameObject uses), the clone would capture
// the override's ALREADY BAKED value as "original", and a Clear on the
// clone would return THAT value instead of the model's metallic. Without this test
// it went unnoticed: no other covers the memory path for the
// factors, only the disk one (test_factor_overrides_survive_round_trip,
// which never carries the baseline).
static void test_factor_clone_clear_restores_model_value_not_override(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    // Non-empty sourcePath: same reason as
    // test_clone_clear_restores_fbx_texture_not_override -- it makes
    // cloneGameObject reuse the ALREADY LOADED mesh (PreloadedMeshCache) instead
    // of going to disk, where "assets/cubo.fbx" does not exist.
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));

    MaterialOverride ov;
    ov.index    = 0;
    ov.metallic = 0.9f;
    go->materialOverrides.push_back(ov);
    // go's LIVE material already carries the baked override, just like an
    // object edited in the editor before duplicating it.
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.metallic == 0.9f);

    GameObject* clone = scene.cloneGameObject(go, nullptr, pm, am);
    CHECK(clone != nullptr);
    if (!clone) return;
    CHECK(clone->materialOverrides.size() == 1);
    if (clone->materialOverrides.empty()) return;

    // Clear on the CLONE, not on the original.
    clone->materialOverrides[0].metallic = -1.0f;
    applyMaterialOverrides(*clone);

    // Without the fix, this returned 0.9 (the baked override that the
    // clone brought), not the model's default.
    CHECK(clone->getMesh()->material.metallic == 0.0f);
}

// --- MeshComponentCommand: adding and removing the Mesh through the undo stack ---
//
// Debt accepted at the time: the Mesh "x" removed the mesh and emptied the
// overrides outside the undo, so Ctrl+Z gave nothing back and the textures
// assigned by hand were lost. Without a renderer (nullptr): what is tested is the
// CPU part, which is where the two real risks lived.

// Removing and undoing returns THE SAME mesh with its overrides, and a later Clear
// returns the FBX's, not the user's. That second half is the one that
// requires restoring the overrides AFTER setMesh: the stored mesh carries the
// user's baked into its Material, and with the baselines lowered
// applyMaterialOverrides would recapture it as "original". The starting
// metallic is 0.3 and not the default 0.0, which would not prove that anything was read.
static void test_remove_mesh_undo_restores_mesh_and_overrides()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->material.texturePath = "assets/fbx_albedo.png";
    mesh->material.metallic    = 0.3f;
    go->setMesh(mesh); mesh = soloObservador(mesh);
    const uint64_t id = go->id;
    setMaterialTextureOverride(*go, 0, MaterialTextureSlot::Albedo, "assets/mia.png");
    setMaterialFactorOverride(*go, 0, MaterialFactorSlot::Metallic, 0.8f);

    MeshComponentCommand cmd(scene, nullptr, "Quitar Mesh", *go, /*add=*/false);
    cmd.execute();
    CHECK(!scene.findById(id)->hasMesh());
    CHECK(scene.findById(id)->materialOverrides.empty());

    cmd.undo();
    GameObject* vuelto = scene.findById(id);
    CHECK(vuelto->getMesh() == mesh);   // the SAME one, not a reload
    CHECK(vuelto->materialOverrides.size() == 1);
    CHECK(mesh->material.texturePath == "assets/mia.png");
    CHECK(mesh->material.metallic == 0.8f);

    // Clear of both: the FBX's has to come back. It is read through the
    // object: the command still holds the mesh (for a redo), so
    // editing it copies it, as intended, and `mesh` is the earlier one.
    setMaterialTextureOverride(*vuelto, 0, MaterialTextureSlot::Albedo, "");
    CHECK(vuelto->getMesh()->material.texturePath == "assets/fbx_albedo.png");
    setMaterialFactorOverride(*vuelto, 0, MaterialFactorSlot::Metallic, -1.0f);
    CHECK(vuelto->getMesh()->material.metallic == 0.3f);

    // Redo: it is removed again.
    cmd.execute();
    CHECK(!scene.findById(id)->hasMesh());
    CHECK(scene.findById(id)->materialOverrides.empty());
}

// Adding a Mesh does NOT go through the undo (it is asynchronous). "Remove A, put B,
// Ctrl+Z" cannot overwrite B with A: B could not be recovered. And the same with B
// still loading: its result would land on top of A.
static void test_remove_mesh_undo_does_not_overwrite_newer_mesh()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto a = std::make_shared<Mesh>();
    a->material.texturePath = "assets/a.png";
    go->setMesh(a);
    const uint64_t id = go->id;
    setMaterialTextureOverride(*go, 0, MaterialTextureSlot::Albedo, "assets/mia.png");

    MeshComponentCommand cmd(scene, nullptr, "Quitar Mesh", *go, /*add=*/false);
    cmd.execute();

    auto b = std::make_shared<Mesh>();
    b->material.texturePath = "assets/b.png";
    scene.findById(id)->setMesh(b);

    cmd.undo();
    CHECK(scene.findById(id)->getMesh() == b);
    CHECK(b->material.texturePath == "assets/b.png");
    CHECK(scene.findById(id)->materialOverrides.empty());

    scene.findById(id)->setMesh(nullptr);
    scene.findById(id)->pendingMeshJob = 7;
    cmd.undo();
    CHECK(!scene.findById(id)->hasMesh());
    CHECK(scene.findById(id)->materialOverrides.empty());
}

// Adding: the command is stacked when the load ALREADY landed (applyLoadedMesh did
// the setMesh and applied the overrides), WITHOUT execute. Undo removes it; redo returns
// the same mesh with its overrides and the FBX baseline intact.
static void test_add_mesh_command_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->material.texturePath = "assets/fbx_albedo.png";
    go->setMesh(mesh); mesh = soloObservador(mesh);   // what applyLoadedMesh does on landing
    const uint64_t id = go->id;
    setMaterialTextureOverride(*go, 0, MaterialTextureSlot::Albedo, "assets/mia.png");

    MeshComponentCommand cmd(scene, nullptr, "Añadir Mesh", *go, /*add=*/true);

    cmd.undo();
    CHECK(!scene.findById(id)->hasMesh());
    CHECK(scene.findById(id)->materialOverrides.empty());

    cmd.execute();
    CHECK(scene.findById(id)->getMesh() == mesh);
    CHECK(mesh->material.texturePath == "assets/mia.png");
    setMaterialTextureOverride(*scene.findById(id), 0, MaterialTextureSlot::Albedo, "");
    // Through the object: the command holds the mesh and editing it copies it.
    CHECK(scene.findById(id)->getMesh()->material.texturePath == "assets/fbx_albedo.png");
}

// Undoing "add A" only removes A. If the mesh is already another one (it arrived through a path
// without undo, such as deleting the FBX in use from the Content Browser and loading another),
// this command does not take it away.
static void test_add_mesh_undo_only_removes_its_own_mesh()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto a = std::make_shared<Mesh>();
    go->setMesh(a);
    const uint64_t id = go->id;

    MeshComponentCommand cmd(scene, nullptr, "Añadir Mesh", *go, /*add=*/true);

    auto b = std::make_shared<Mesh>();
    b->material.texturePath = "assets/b.png";
    setMaterialTextureOverride(*scene.findById(id), 0, MaterialTextureSlot::Albedo, "assets/mia.png");
    scene.findById(id)->setMesh(b);

    cmd.undo();
    CHECK(scene.findById(id)->getMesh() == b);
    CHECK(scene.findById(id)->materialOverrides.size() == 1);
}

// --- decodeMaterialTexture: the path wins, in the FOUR uploaders ---
//
// Debt accepted at the time: each uploader carried its own switch on
// chooseTextureSource, and the chooseTextureSource tests did not bind any of them.
// Reverting just one to "the embedded one wins" left the suite green. Now
// they all decode through decodeMaterialTexture, and these two tests cover the two
// halves: that this function chooses correctly, and that nobody decodes on their own.

static std::vector<uint8_t> leeFichero(const std::string& ruta)
{
    std::ifstream f(ruta, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Two PNGs of DIFFERENT SIZE, to know which one was decoded without comparing
// pixels: the path one measures 512x512 and the "embedded" one 672x768.
static void test_decode_material_texture_path_beats_embedded()
{
    const std::string          ruta     = "assets/skybox/_test_nx.png";
    const std::vector<uint8_t> embebida = leeFichero("assets/MainEngineLogo.png");
    // Launched from another cwd it does not find the assets: it FAILS, it does not pass.
    CHECK(std::filesystem::exists(ruta));
    CHECK(!embebida.empty());

    const DecodedTexture lasDos = decodeMaterialTexture(ruta, embebida);
    CHECK(lasDos && lasDos.w == 512 && lasDos.h == 512);

    const DecodedTexture soloEmbebida = decodeMaterialTexture("", embebida);
    CHECK(soloEmbebida && soloEmbebida.w == 672 && soloEmbebida.h == 768);

    // Broken path with the embedded one set: NOTHING, not the FBX's. A broken path
    // has to be visible (checkerboard in the caller), never covered up with the original.
    const DecodedTexture rota = decodeMaterialTexture("assets/no_existe_en_el_repo.png", embebida);
    CHECK(!rota && rota.w == 0 && rota.h == 0);

    CHECK(!decodeMaterialTexture("", {}));
}

// The other half: decoding the embedded one requires stbi_load_from_memory, so
// an uploader that went back to its own switch would have to call it. It reads the
// code on disk, like the GameObject.h includes test in camera_tests.
static void test_no_uploader_decodes_embedded_on_its_own()
{
    int ficheros = 0;
    std::vector<std::string> culpables;
    for (const char* raiz : { "engine/src", "engine/include" })
    {
        CHECK(std::filesystem::is_directory(raiz));
        if (!std::filesystem::is_directory(raiz)) continue;
        for (const auto& e : std::filesystem::recursive_directory_iterator(raiz))
        {
            if (!e.is_regular_file()) continue;
            const std::string ext = e.path().extension().string();
            if (ext != ".cpp" && ext != ".h") continue;
            ++ficheros;
            // The .cpp calls it and the .h names it in the comment that explains
            // all this: the two are the function's home.
            if (e.path().stem() == "MaterialTextureSource") continue;
            // The Content Browser thumbnails are not a material uploader:
            // they read THEIR file (through ifstream, which accepts Unicode paths) and check
            // the dimensions with stbi_info before decoding. There is no embedded one
            // to choose, which is the only thing this test watches.
            if (e.path().stem() == "Thumbnail") continue;
            // The same for the textures of a model preview (thumbnails):
            // the loader fills in path OR embedded, never both, so there is also
            // nothing to choose. Only this file; ModelLoader.cpp is still watched.
            if (e.path().stem() == "PreviewImage") continue;
            const std::vector<uint8_t> bytes = leeFichero(e.path().string());
            const std::string texto(bytes.begin(), bytes.end());
            if (texto.find("stbi_load_from_memory") != std::string::npos)
                culpables.push_back(e.path().generic_string());
        }
    }
    for (const std::string& c : culpables)
        std::printf("  stbi_load_from_memory fuera de decodeMaterialTexture: %s\n", c.c_str());
    CHECK(ficheros > 100);   // it really walked the tree
    CHECK(culpables.empty());
}

// --- Metallic/Roughness sliders against a REAL ImGui, without a window ---
//
// The bug that these tests cover was not in any command or in the Material:
// it was in WHICH FRAME ImGui delivers a SliderFloat's value. That is why the
// real widget is driven with mouse events, instead of calling draw() with
// made-up values: a double of the slider would have inherited the same false
// assumption that caused the bug.
struct ImGuiSinVentana
{
    ImGuiContext* ctx = nullptr;

    ImGuiSinVentana()
    {
        ctx = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename  = nullptr;
        io.LogFilename  = nullptr;
        io.DisplaySize  = ImVec2(800.0f, 600.0f);
        io.DeltaTime    = 1.0f / 60.0f;
        // Without a render backend: with this flag the font atlas is built
        // only in NewFrame and nobody has to upload the texture anywhere.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }
    ~ImGuiSinVentana() { ImGui::DestroyContext(ctx); }

    template<typename Fn> void frame(Fn&& body)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(400.0f, 200.0f));
        ImGui::Begin("Test", nullptr, ImGuiWindowFlags_NoDecoration |
                                      ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings);
        body();
        ImGui::End();
        ImGui::Render();
    }
};

// Click at 10% of the slider, drag past the right end (clamps to 1.0)
// and release. One event per frame: ImGui delivers them that way anyway
// (ConfigInputTrickleEventQueue), and kept apart you can tell which frame does what.
// `body` draws the slider and nothing behind it, so that GetItemRect* is its own.
template<typename Fn>
static void arrastraHastaElTope(ImGuiSinVentana& ui, Fn&& drawSlider)
{
    ImVec2 min(0.0f, 0.0f), max(0.0f, 0.0f);
    auto body = [&] {
        drawSlider();
        min = ImGui::GetItemRectMin();
        max = ImGui::GetItemRectMax();
    };
    ImGuiIO& io = ImGui::GetIO();
    ui.frame(body);                                                  // layout
    const float y = (min.y + max.y) * 0.5f;
    io.AddMousePosEvent(min.x + (max.x - min.x) * 0.1f, y); ui.frame(body);  // hover
    io.AddMouseButtonEvent(0, true);                        ui.frame(body);  // click
    io.AddMousePosEvent(max.x + 50.0f, y);                  ui.frame(body);  // drag
    io.AddMouseButtonEvent(0, false);                       ui.frame(body);  // release
    ui.frame(body);
}

// CHARACTERIZATION of ImGui, not of our code: it is the premise of the fix, and
// if a future ImGui version changes it this test says so before anyone else.
// The pattern the panel had (local from the data, commit reading the local in
// IsItemDeactivatedAfterEdit) with data that is NOT written live: ImGui DOES
// report that there was an edit, but that frame does not write the value, and the local
// holds what it was before the drag.
static void test_imgui_slider_release_frame_does_not_deliver_value()
{
    ImGuiSinVentana ui;
    const float material = 0.0f;
    float maxVisto = -1.0f, commitIngenuo = -1.0f;
    bool  huboCommit = false;
    arrastraHastaElTope(ui, [&] {
        float v = material;
        ImGui::SliderFloat("Metallic", &v, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemActive()) maxVisto = std::max(maxVisto, v);
        if (ImGui::IsItemDeactivatedAfterEdit()) { huboCommit = true; commitIngenuo = v; }
    });
    CHECK(maxVisto > 0.99f);            // the drag reached the end
    CHECK(huboCommit);                  // and ImGui reports that there was an edit
    CHECK(commitIngenuo == material);   // but the local does not carry the value
}

// The bug the user saw: dragging Metallic to 1 and, on release, the slider
// going back to 0 even though the object looked metallic. The commit has to carry the
// DRAGGED value, and `begin` that of the data before the click (-0.25), not the 0.1
// that SliderFloat jumps to on the click frame.
static void test_deferred_slider_commits_dragged_value()
{
    ImGuiSinVentana ui;
    DeferredSliderFloat slider;
    float material = 0.25f;
    int   commits = 0;
    bool  vivo = false;
    float begin = -1.0f, value = -1.0f;
    arrastraHastaElTope(ui, [&] {
        const auto r = slider.draw("Metallic", material, 0.0f, 1.0f, "%.2f");
        if (r.active) vivo = true;
        if (r.committed)
        {
            ++commits;
            begin    = r.begin;
            value    = r.value;
            material = r.value;   // what MaterialFactorCommand does
        }
    });
    CHECK(vivo);
    CHECK(commits == 1);
    CHECK(begin == 0.25f);
    CHECK(value > 0.99f);
    CHECK(material > 0.99f);
}

// The widget disappears mid-drag (the selection moves to an object without a
// mesh) and comes back later: it has to show the data, not the pending value of a
// drag that was never delivered. Without the frame check in draw(),
// m_activeId stays set and the slider reappears with the abandoned value.
static void test_deferred_slider_forgets_drag_of_vanished_widget()
{
    ImGuiSinVentana ui;
    DeferredSliderFloat slider;
    const float material = 0.25f;
    ImVec2 min(0.0f, 0.0f), max(0.0f, 0.0f);
    DeferredSliderFloat::Result r;
    auto conSlider = [&] {
        r   = slider.draw("Metallic", material, 0.0f, 1.0f, "%.2f");
        min = ImGui::GetItemRectMin();
        max = ImGui::GetItemRectMax();
    };
    auto sinSlider = [] { ImGui::TextUnformatted("otro objeto"); };

    ImGuiIO& io = ImGui::GetIO();
    ui.frame(conSlider);
    const float y = (min.y + max.y) * 0.5f;
    io.AddMousePosEvent(min.x + (max.x - min.x) * 0.1f, y); ui.frame(conSlider);
    io.AddMouseButtonEvent(0, true);                        ui.frame(conSlider);
    io.AddMousePosEvent(max.x + 50.0f, y);                  ui.frame(conSlider);
    CHECK(r.active && r.value > 0.99f);   // the drag is under way

    // Several frames without the widget, as when going to another object and coming back with a click
    // in the Hierarchy. With ONE it does not work: ImGui lets a widget that
    // reappears right on the frame after losing the ActiveId see that
    // deactivation (IsItemDeactivatedAfterEdit), and that is already ImGui
    // behavior, not the abandoned drag that is tested here.
    ui.frame(sinSlider);
    io.AddMouseButtonEvent(0, false); ui.frame(sinSlider);
    io.AddMousePosEvent(0.0f, 0.0f);  ui.frame(sinSlider);
    ui.frame(sinSlider);
    ui.frame(conSlider);
    CHECK(!r.active);
    CHECK(!r.committed);
    CHECK(r.value == material);
}

static std::filesystem::path matAssetTestDir(const char* name)
{
    std::error_code ec;
    std::filesystem::path d = std::filesystem::temp_directory_path(ec) / name;
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d, ec);
    return d;
}

// Finding from the final reviewer (spec line 133): a referenced .mat that
// is missing on disk has to warn just like a broken one, not only the unreadable one;
// before this fix, a .mat deleted outside the editor loaded the scene with
// the model and without any clue as to why.
static void test_missing_mat_asset_warns()
{
    const auto d = matAssetTestDir("dt_matasset_missing_warns");
    auto go = makeStaticFixture();
    go->materialOverrides.push_back(MaterialOverride{});
    go->materialOverrides[0].matAsset = (d / "no_existe.mat").string();

    std::vector<std::string> avisos;
    collectMaterialOverrideWarnings(*go, avisos);
    CHECK(avisos.size() == 1);
    if (avisos.size() == 1)
        CHECK(avisos[0].find(go->materialOverrides[0].matAsset) != std::string::npos);
}

// Spec: "single warning per path". Two objects that share the same broken .mat
// do not duplicate the warning (a large scene with many users of the same .mat
// must not fill the Log with the same repeated line).
static void test_shared_broken_mat_asset_warns_once()
{
    const auto d = matAssetTestDir("dt_matasset_shared_warns_once");
    const std::string matPath = (d / "no_existe.mat").string();

    auto goA = makeStaticFixture();
    goA->materialOverrides.push_back(MaterialOverride{});
    goA->materialOverrides[0].matAsset = matPath;

    auto goB = makeStaticFixture();
    goB->materialOverrides.push_back(MaterialOverride{});
    goB->materialOverrides[0].matAsset = matPath;

    std::vector<std::string> avisos;
    collectMaterialOverrideWarnings(*goA, avisos);
    collectMaterialOverrideWarnings(*goB, avisos);
    int matches = 0;
    for (const std::string& w : avisos) if (w.find(matPath) != std::string::npos) ++matches;
    CHECK(matches == 1);
}

// The .mat rules when there is NO object override for that field.
static void test_mat_asset_supplies_texture_when_no_override()
{
    const auto d = matAssetTestDir("dt_matasset_no_override");
    MaterialAsset m; m.albedo = (d / "rojo.png").string();
    std::string err;
    CHECK(saveMaterialAsset(d / "x.mat", m, &err));

    auto go = makeStaticFixture();   // material.texturePath = "assets/fbx_albedo.png"
    go->materialOverrides.push_back(MaterialOverride{});
    go->materialOverrides[0].matAsset = (d / "x.mat").string();
    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.texturePath == m.albedo);
}

// The OBJECT's override rules over the .mat.
static void test_object_override_wins_over_mat_asset()
{
    const auto d = matAssetTestDir("dt_matasset_object_wins");
    MaterialAsset m; m.albedo = (d / "rojo.png").string();
    std::string err;
    CHECK(saveMaterialAsset(d / "x.mat", m, &err));

    auto go = makeStaticFixture();
    MaterialOverride ov; ov.matAsset = (d / "x.mat").string();
    ov.albedo = (d / "azul.png").string();
    go->materialOverrides.push_back(ov);
    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.texturePath == ov.albedo);
}

// Review Focus 3: a .mat with ONLY roughness leaves the three FBX textures.
static void test_mat_asset_with_only_one_field_leaves_the_rest_alone()
{
    const auto d = matAssetTestDir("dt_matasset_partial");
    MaterialAsset m; m.roughness = 0.2f;
    std::string err;
    CHECK(saveMaterialAsset(d / "x.mat", m, &err));

    auto go = makeStaticFixture();
    const std::string fbxAlbedo = go->getMesh()->material.texturePath;
    go->materialOverrides.push_back(MaterialOverride{});
    go->materialOverrides[0].matAsset = (d / "x.mat").string();
    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.texturePath == fbxAlbedo);   // intact
    CHECK(go->getMesh()->material.roughness == 0.2f);
}

// Clear of the object (empty override) falls to the .mat, not to the model.
static void test_clear_falls_back_to_mat_asset_not_model()
{
    const auto d = matAssetTestDir("dt_matasset_clear");
    MaterialAsset m; m.albedo = (d / "rojo.png").string();
    std::string err;
    CHECK(saveMaterialAsset(d / "x.mat", m, &err));

    auto go = makeStaticFixture();
    const std::string fbxAlbedo = go->getMesh()->material.texturePath;
    MaterialOverride ov; ov.matAsset = (d / "x.mat").string();
    ov.albedo = (d / "azul.png").string();
    go->materialOverrides.push_back(ov);
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.texturePath == ov.albedo);

    go->materialOverrides[0].albedo.clear();                  // Clear
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.texturePath == m.albedo);   // to the .mat, not to the FBX
    CHECK(go->getMesh()->material.texturePath != fbxAlbedo);
}

// Unlinking the .mat (matAsset = "") falls to the model.
static void test_unlinking_mat_asset_falls_back_to_model()
{
    const auto d = matAssetTestDir("dt_matasset_unlink");
    MaterialAsset m; m.albedo = (d / "rojo.png").string();
    std::string err;
    CHECK(saveMaterialAsset(d / "x.mat", m, &err));

    auto go = makeStaticFixture();
    const std::string fbxAlbedo = go->getMesh()->material.texturePath;
    go->materialOverrides.push_back(MaterialOverride{});
    go->materialOverrides[0].matAsset = (d / "x.mat").string();
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.texturePath == m.albedo);

    go->materialOverrides[0].matAsset.clear();
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.texturePath == fbxAlbedo);
}

// Review Focus 1: a nonexistent .mat inherits everything, without throwing.
static void test_missing_mat_asset_inherits_everything()
{
    auto go = makeStaticFixture();
    const std::string fbxAlbedo = go->getMesh()->material.texturePath;
    go->materialOverrides.push_back(MaterialOverride{});
    go->materialOverrides[0].matAsset = "no/existe/en/el/repo.mat";
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.texturePath == fbxAlbedo);
}

// The .mat works the same in skinned, at an index other than 0.
static void test_mat_asset_on_skinned_slot_two()
{
    const auto d = matAssetTestDir("dt_matasset_skinned");
    MaterialAsset m; m.metallic = 0.7f;
    std::string err;
    CHECK(saveMaterialAsset(d / "x.mat", m, &err));

    auto go = makeSkinnedFixture();   // 3 materiales
    MaterialOverride ov; ov.index = 2; ov.matAsset = (d / "x.mat").string();
    go->materialOverrides.push_back(ov);
    applyMaterialOverrides(*go);

    CHECK(go->getSkinnedMesh()->materials[2].metallic == 0.7f);
    CHECK(go->getSkinnedMesh()->materials[0].metallic == 0.0f);   // 0 is not touched
}

// Without matAsset (with or without other overrides), the result is IDENTICAL to today's.
static void test_no_mat_asset_is_unchanged()
{
    auto go = makeStaticFixture();
    const std::string before = go->getMesh()->material.texturePath;
    go->materialOverrides.push_back(MaterialOverride{});   // without matAsset, without anything
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.texturePath == before);
}

int main()
{
    test_set_material_asset_override_creates_entry_and_applies();
    test_material_asset_command_undo_redo();
    test_missing_mat_asset_warns();
    test_shared_broken_mat_asset_warns_once();
    test_mat_asset_supplies_texture_when_no_override();
    test_object_override_wins_over_mat_asset();
    test_mat_asset_with_only_one_field_leaves_the_rest_alone();
    test_clear_falls_back_to_mat_asset_not_model();
    test_unlinking_mat_asset_falls_back_to_model();
    test_missing_mat_asset_inherits_everything();
    test_mat_asset_on_skinned_slot_two();
    test_no_mat_asset_is_unchanged();

    // PhysicsManager/AudioManager share an instance among the tests that
    // need it: creating and destroying a PhysicsManager per test crashes on the
    // second init (one PxFoundation per process), same pattern as
    // audio_tests.cpp.
    PhysicsManager pm;
    pm.init();
    AudioManager am;
    if (!am.init())
        std::printf("AVISO: FMOD no disponible; los tests que lo necesitan se saltarán\n");

    test_materials_of_static_mesh();
    test_materials_of_skinned_mesh();
    test_override_writes_material_and_captures_baseline();
    test_second_override_keeps_original_baseline();
    test_clear_restores_baseline();
    test_skinned_override_touches_only_its_index();
    test_out_of_range_index_is_ignored();
    test_slots_are_independent();
    test_normal_and_orm_overrides_write_their_own_field();
    test_clear_restores_empty_baseline_on_procedural_mesh();
    test_no_mesh_is_noop();
    test_path_wins_over_embedded();
    test_embedded_when_no_path();
    test_none_when_empty();
    test_path_when_no_embedded();
    test_overrides_survive_round_trip(pm, am);
    test_mat_asset_survives_round_trip_alone(pm, am);
    test_scene_without_mat_asset_field_loads_unchanged(pm, am);
    test_no_overrides_writes_no_key();
    test_scene_without_materials_key_loads_clean(pm, am);
    test_path_under_root_is_stored_relative(pm, am);
    test_path_outside_root_stays_absolute();
    test_without_root_path_is_verbatim(pm, am);
    test_corrupt_materials_block_warns(pm, am);
    test_materials_entry_without_valid_index_is_discarded(pm, am);
    test_clone_keeps_override_path_verbatim_with_root_set(pm, am);
    test_clone_shares_procedural_mesh(pm, am);
    test_undo_redo_keeps_override_path_verbatim_with_root_set(pm, am);
    test_overrides_applied_to_incoming_mesh();
    test_scene_load_applies_override_to_material(pm, am);
    test_out_of_range_index_warns_on_scene_load(pm, am);
    test_override_warnings_helper();
    test_clone_clear_restores_fbx_texture_not_override(pm, am);
    test_discard_overridden_decoded_images_removes_only_overridden_slot();
    test_discard_overridden_decoded_images_ignores_other_index();
    test_discard_overridden_decoded_images_considers_mat_asset();
    test_set_mesh_resets_stale_baseline();
    test_reload_after_failed_load_recaptures_correct_baseline();
    test_command_undo_redo_assignment(pm, am);
    test_command_undo_of_clear(pm, am);
    test_command_survives_object_rebuild(pm, am);
    test_command_on_object_without_mesh_is_noop(pm, am);
    test_command_stale_index_after_mesh_shrinks(pm, am);

    test_factor_override_writes_material_and_captures_baseline();
    test_factor_second_change_keeps_original_baseline();
    test_factor_clear_restores_baseline();
    test_set_mesh_resets_stale_factor_baseline();
    test_factor_overrides_survive_round_trip(pm, am);
    test_factor_absent_in_json_does_not_touch_material(pm, am);
    test_factor_command_undo_redo(pm, am);
    test_factor_command_undo_of_first_edit_leaves_no_active_override(pm, am);
    test_factor_command_on_object_without_mesh_is_noop(pm, am);
    test_factor_command_stale_index_after_mesh_shrinks(pm, am);
    test_factor_command_survives_object_rebuild(pm, am);
    test_factor_clone_clear_restores_model_value_not_override(pm, am);

    test_imgui_slider_release_frame_does_not_deliver_value();
    test_deferred_slider_commits_dragged_value();
    test_deferred_slider_forgets_drag_of_vanished_widget();

    test_decode_material_texture_path_beats_embedded();
    test_no_uploader_decodes_embedded_on_its_own();

    test_remove_mesh_undo_restores_mesh_and_overrides();
    test_remove_mesh_undo_does_not_overwrite_newer_mesh();
    test_add_mesh_command_undo_redo();
    test_add_mesh_undo_only_removes_its_own_mesh();

    am.shutdown();
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL MATERIAL TEXTURE TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
