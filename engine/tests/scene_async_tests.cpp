// Tests de Scene::fromJson con y sin AsyncAssetLoader.
//
// El caso mas importante es el primero: con loader == nullptr el
// comportamiento tiene que ser IDENTICO al de antes de esta feature. Es lo que
// protege el restore de Play->Stop (EditorUI.cpp:170) y las ocho suites que ya
// existen. Ese test (testSyncPathUnchanged) es un DIFERENCIAL sobre
// pendingMeshJob con una escena que SI tiene sourcePath — no una comparacion
// de toJson() entre dos cargas nullptr, que no puede detectar una seleccion
// de rama invertida (ver el comentario junto a la funcion para el porque).
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Core/JobSystem.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Renderer/AsyncAssetLoader.h"
#include "DonTopo/Renderer/Mesh.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cassert>
#include <cstdio>
#include <memory>

namespace {

// Check con fallo RUIDOSO y valido en Release: assert() se compila a nada bajo
// NDEBUG, asi que un `assert(ptr); ptr->campo` deja un deref de puntero
// potencialmente nulo sin red. CHECK cuenta el fallo, lo imprime y NO
// desreferencia — un test roto reporta en vez de petar con un 0xC0000005 mudo.
int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
    ++g_failures; } } while (0)

// Una sola PhysicsManager para todo el fichero: crear y liberar una por test
// crashea al segundo init porque PxFoundation es unica por proceso.
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

// Escena minima con dos nodos anidados y transforms NO neutros: un test que
// afirmase la identidad pasaria igual si nadie leyera el campo. Sin
// sourcePath: nunca dispara la rama de carga de mesh, sincrona o asincrona
// por igual — es justo lo que hace falta para comparar los dos caminos sin
// que el resultado dependa de si el asset existe en disco.
//
// Envuelta en version/root: Scene::fromJson exige ambos campos (los mismos
// que produce toJson()) y devuelve false si faltan — el fixture del brief
// no los traia y por tanto nunca cargaba nada. Sin el envoltorio los tres
// tests fallarian en el primer assert, antes de ejercitar nada del loader.
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

// Escena con un nodo que SI dispara la rama de carga de mesh: sourcePath
// apunta a un fichero que a proposito no existe. No hace falta un asset real
// — el worker fallara con un LoadedMesh::error no vacio, que es justo el
// caso que hay que soportar sin crashear. Sirve para probar de verdad la
// rama async (la fixture del brief, sin "mesh", nunca la disparaba).
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

// Quita "id" recursivamente de un nodo serializado (mismo patron que el
// stripIds de Scene::cloneGameObject). node->id lo asigna un contador
// atomico GLOBAL en el constructor de GameObject: dos Scene distintas
// cargando el MISMO JSON (sin "id" en el fichero) reusan la posicion en el
// contador pero nunca el mismo valor absoluto, asi que comparar toJson() sin
// quitar "id" fallaria SIEMPRE, incluso entre dos cargas identicas — no
// probaria nada sobre loader nullptr vs default.
void stripIds(nlohmann::json& node)
{
    if (!node.is_object()) return;
    node.erase("id");
    if (auto it = node.find("children"); it != node.end() && it->is_array())
        for (auto& child : *it)
            stripIds(child);
}

// El caso mas importante: con loader == nullptr el comportamiento tiene que
// ser IDENTICO al de antes de esta feature — en particular, la rama estatica
// que esta tarea toco (el `else if (!sourcePath.empty()) { if (loader) {...}
// else {...} }` dentro de nodeFromJson) NO debe encolar ninguna peticion.
//
// Es un DIFERENCIAL sobre pendingMeshJob, no una comparacion de toJson():
// carga la MISMA escena con y sin loader y compara el campo que la rama
// if(loader)/else realmente escribe. Esto SI prueba seleccion de rama — a
// diferencia de comparar dos toJson() donde AMBOS lados usan loader==nullptr
// (ver testSyncDefaultArgEqualsExplicitNullptr mas abajo): con los dos lados
// nullptr, una rama corrupta corrompe los dos por igual y esa comparacion
// seguiria cuadrando. Hace falta un nodo con sourcePath (sceneWithPendingLoad,
// no twoNodeScene) para que la rama exista siquiera — con twoNodeScene
// j.contains("mesh") es false y todo el bloque queda muerto para el test.
//
// Sabotaje: invertir la condicion a `if (!loader)` en la rama estatica de
// nodeFromJson -> los dos asserts de abajo se intercambian (sin loader
// encolaria, con loader no) y el test falla.
void testSyncPathUnchanged(DonTopo::AsyncAssetLoader& loader)
{
    DonTopo::Scene sync;
    CHECK(sync.fromJson(sceneWithPendingLoad(), physics(), audio(), nullptr), "fromJson nullptr debe cargar");
    DonTopo::GameObject* hijoASync = nullptr;
    sync.traverse([&](DonTopo::GameObject* go) { if (go->name == "hijoA") hijoASync = go; });
    CHECK(hijoASync, "hijoA debe existir (sync)");
    // Sin loader: corre la rama sincrona (ModelLoader::load). El fichero no
    // existe, asi que no hay mesh — pero sobre todo NO se llamo a
    // requestMesh en absoluto, y pendingMeshJob se queda en su valor por
    // defecto (0).
    if (hijoASync)
        CHECK(hijoASync->pendingMeshJob == 0,
              "sin loader no debe encolarse ninguna peticion (rama sincrona)");

    DonTopo::Scene withLoader;
    CHECK(withLoader.fromJson(sceneWithPendingLoad(), physics(), audio(), &loader), "fromJson &loader debe cargar");
    DonTopo::GameObject* hijoAAsync = nullptr;
    withLoader.traverse([&](DonTopo::GameObject* go) { if (go->name == "hijoA") hijoAAsync = go; });
    CHECK(hijoAAsync, "hijoA debe existir (async)");
    // Con loader: corre la rama asincrona -> requestMesh SI se llamo.
    if (hijoAAsync)
        CHECK(hijoAAsync->pendingMeshJob != 0,
              "con loader debe encolarse una peticion real (rama asincrona)");
}

// Comprobacion secundaria de determinismo: el parametro por defecto y el
// nullptr explicito son el MISMO camino (misma jerarquia/transforms tras
// stripIds). NO prueba seleccion de rama — ver testSyncPathUnchanged para
// eso — porque los dos lados son loader==nullptr: una rama estatica
// corrupta corrompe ambos por igual y esta comparacion seguiria cuadrando.
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

// Con loader, los GameObject existen ya con su jerarquia y su transform: lo
// unico que falta es el mesh. El nodo con sourcePath queda con una peticion
// en vuelo (pendingMeshJob != 0) y sin render index — nada se ha bombeado
// todavia. Sabotaje: crear los nodos solo al bombear — el assert de nombres
// falla porque la escena esta vacia.
void testAsyncCreatesNodesImmediately(DonTopo::AsyncAssetLoader& loader)
{
    DonTopo::Scene s;
    CHECK(s.fromJson(sceneWithPendingLoad(), physics(), audio(), &loader), "fromJson &loader debe cargar");

    int found = 0;
    bool hijoAHasPendingJob = false;
    s.traverse([&](DonTopo::GameObject* go) {
        if (go->name == "hijoA" || go->name == "hijoB") ++found;
        // Nada se ha bombeado todavia: ningun nodo puede tener indice de render.
        CHECK(go->staticRenderIndex  == -1, "sin bombear no hay indice static");
        CHECK(go->skinnedRenderIndex == -1, "sin bombear no hay indice skinned");
        if (go->name == "hijoA")
        {
            // hijoA traia sourcePath: debe tener una peticion en vuelo y
            // NINGUN mesh todavia (el GameObject existe completo desde el
            // frame 0, sin esperar al asset).
            hijoAHasPendingJob = (go->pendingMeshJob != 0);
            CHECK(!go->hasMesh(), "hijoA no debe tener mesh todavia");
        }
        if (go->name == "hijoB")
        {
            // hijoB no traia sourcePath: no hay nada que pedir.
            CHECK(go->pendingMeshJob == 0, "hijoB sin sourcePath no encola");
        }
    });
    CHECK(found == 2, "los GameObject existen desde el frame 0, sin esperar al asset");
    CHECK(hijoAHasPendingJob, "el nodo con sourcePath debe encolar una peticion real");
}

// Borrar un GameObject con carga pendiente y bombear despues no crashea: el
// resultado se descarta porque su targetId ya no esta en la escena viva.
//
// Es el test con mas valor de los tres: el use-after-free clasico de este
// patron es guardar un GameObject* en la peticion. Sabotaje: guardar el
// puntero en vez del id y desreferenciarlo al bombear — crash o basura.
void testDeletedTargetIsDiscarded(DonTopo::AsyncAssetLoader& loader)
{
    DonTopo::Scene s;
    // sceneWithPendingLoad, no twoNodeScene: hace falta una peticion REAL en
    // vuelo (hijoA tiene sourcePath) para que este test compruebe algo — con
    // la fixture sin mesh del brief, pumpCompleted() no tenia nada que
    // entregar y el test pasaba sin ejercitar el camino de descarte.
    CHECK(s.fromJson(sceneWithPendingLoad(), physics(), audio(), &loader), "fromJson &loader debe cargar");

    DonTopo::GameObject* victim = nullptr;
    s.traverse([&](DonTopo::GameObject* go) { if (go->name == "hijoA") victim = go; });
    CHECK(victim, "hijoA debe existir");
    if (!victim) return;   // Release-safe: sin victim no se puede seguir sin desreferenciar nulo
    CHECK(victim->pendingMeshJob != 0, "hace falta una peticion real en vuelo para este test");

    const uint64_t goneId = victim->id;
    s.removeGameObject(victim);

    // Un resultado dirigido a un id que ya no existe no puede tocar memoria
    // liberada. Se comprueba que sigue sin aparecer tras bombear.
    for (auto& r : loader.pumpCompleted(1000.0f))
        (void)r;   // solo importa que no crashee

    bool stillThere = false;
    s.traverse([&](DonTopo::GameObject* go) { if (go->id == goneId) stillThere = true; });
    CHECK(!stillThere, "el nodo borrado no puede resucitar al bombear");
}

// La cache de precarga (PreloadedMeshCache) se consulta ANTES de leer disco: un
// sourcePath presente en la cache usa una copia profunda de la malla cacheada
// sin tocar el fichero. Es lo que permite al runtime cargar la escena desde
// mallas ya precargadas en paralelo (con progreso en el splash) sin cambiar el
// modelo de registro ni perder la config de animacion.
//
// Verificable sin asset real: se fabrica una malla en RAM con nombre y
// vertices DISTINTIVOS y se mete en la cache bajo el sourcePath de hijoA, que
// apunta a un fichero que NO existe. Si el nodo acaba con esa malla, la cache
// se consulto de verdad — sin cache, un path inexistente no da mesh alguno.
//
// Sabotaje: si nodeFromJson ignorase `preloaded` (no consultara la cache), el
// primer bloque falla en `hijoA->hasMesh()`: el path inexistente cae al disco,
// que no puede leerse, y el nodo se queda sin mesh.
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

    // Con cache: el nodo recibe la malla ficticia sin leer disco (el fichero no
    // existe: sin cache no habria mesh). Ademas debe ser COPIA PROFUNDA, no el
    // mismo shared_ptr — dos GameObject no pueden compartir un Mesh mutable.
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
            // Contrato desde el Apéndice B: la malla estática precargada se
            // COMPARTE (es const para el GameObject), y editarla copia, así que
            // la de la caché no se toca.
            CHECK(hijoA->getMesh().get() == fabricated.get(), "la malla precargada se comparte, no se copia");
            hijoA->editMesh()->name = "editada";
            CHECK(fabricated->name == "malla_precargada_ficticia", "editar el nodo no debe tocar la malla de la cache");
            CHECK(hijoA->getMesh().get() != fabricated.get(), "tras editar, el nodo tiene su propia copia");
        }
    }

    // Cache-miss (cache con otra clave que no casa): cae al disco inexistente ->
    // sin mesh. Prueba que un miss no inventa nada y respeta el fallback.
    DonTopo::PreloadedMeshCache otherCache;
    otherCache["assets/otra_cosa.fbx"] = fabricated;
    DonTopo::Scene withMiss;
    CHECK(withMiss.fromJson(sceneWithPendingLoad(), physics(), audio(), nullptr, &otherCache), "fromJson cache-miss debe cargar");
    DonTopo::GameObject* missA = nullptr;
    withMiss.traverse([&](DonTopo::GameObject* go) { if (go->name == "hijoA") missA = go; });
    CHECK(missA, "hijoA debe existir (miss)");
    if (missA)
        CHECK(!missA->hasMesh(), "cache-miss para un path inexistente debe caer al disco y quedarse sin mesh");

    // preloaded == nullptr: identico al miss (fallback a disco), byte-compatible
    // con todos los callers de siempre.
    DonTopo::Scene noCache;
    CHECK(noCache.fromJson(sceneWithPendingLoad(), physics(), audio(), nullptr, nullptr), "fromJson nullptr cache debe cargar");
    DonTopo::GameObject* nullA = nullptr;
    noCache.traverse([&](DonTopo::GameObject* go) { if (go->name == "hijoA") nullA = go; });
    CHECK(nullA, "hijoA debe existir (nullptr cache)");
    if (nullA)
        CHECK(!nullA->hasMesh(), "sin cache el path inexistente no da mesh");
}

// Un clon NO puede heredar los indices de render del original (H14).
//
// Son la ranura del backend donde vive la malla del original: si el clon se los
// quedara, moverlo moveria la malla del ORIGINAL, y borrarlo soltaria una
// ranura que el original sigue usando — que desde que las ranuras se reciclan
// significa que el siguiente objeto en darse de alta la estrenaria mientras el
// original la dibuja. Nada de eso da error en ningun sitio.
//
// HONESTIDAD SOBRE LO QUE ESTE TEST PRUEBA Y LO QUE NO. Hoy pasa por
// CONSTRUCCION: cloneGameObject serializa con nodeToJson y reconstruye con
// nodeFromJson, y los indices NO se serializan, asi que los nodos nuevos ya
// nacen a -1 por su valor por defecto. Se comprobo quitando el traverse de
// reseteo de Scene::cloneGameObject y el test seguia verde, o sea que ese
// traverse es defensivo y hoy no cubre nada.
//
// Se queda igualmente porque afirma la PROPIEDAD y no la implementacion: el dia
// que alguien serialice los indices en nodeToJson, o cambie el clonado a una
// copia directa en vez de pasar por JSON, este test se pone rojo. Lo que NO
// hace es proteger ese traverse — para eso habria que poder construir un clon
// que si llegase con indices puestos, y por ese camino no se puede.
//
// Se afirma sobre el ARBOL entero y no solo sobre la raiz del clon: clonar un
// modelo importado siempre trae jerarquia.
void testClonNoHeredaIndicesDeRender()
{
    DonTopo::Scene scene;
    CHECK(scene.fromJson(twoNodeScene(), physics(), audio(), nullptr, nullptr),
          "fromJson debe cargar la escena de dos nodos");

    DonTopo::GameObject* original = nullptr;
    scene.traverse([&](DonTopo::GameObject* go) { if (go->name == "hijoA") original = go; });
    CHECK(original, "hijoA debe existir");
    if (!original) return;

    // Un hijo, para que el clon tenga jerarquia que recorrer.
    DonTopo::GameObject* hijo = scene.cloneGameObject(original, original, physics(), audio());
    CHECK(hijo, "el hijo de prueba debe crearse");
    if (!hijo) return;

    // El original y su hijo YA registrados: valores distintos y no triviales,
    // para que heredarlos se note y no se confunda con un cero por defecto.
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

    // Y el original intacto: clonar no le toca los suyos.
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

// Review Focus 4: "piece" va y vuelve; la pieza 0 no escribe el campo; un nodo
// con "piece" se sirve de la entrada de SU pieza en la cache.
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

// Review Focus 1: lo que hace el undo de Delete (collectMeshes + subtreeToJson +
// insertFromJson con esa cache) devuelve a cada hijo SU malla.
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

} // namespace

int main()
{
    // UN solo JobSystem + AsyncAssetLoader para todo el fichero, creados aqui y
    // pasados por referencia — igual que en produccion, donde el editor y el
    // runtime crean UNA instancia de cada, viva toda la app. Antes cada test
    // creaba y destruia los suyos (start/shutdown por test): ese churn repetido
    // de arranque/parada de hilos es lo que este experimento aisla.
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

    jobSystem.shutdown();

    if (g_failures == 0)
    {
        std::printf("scene_async_tests OK\n");
        return 0;
    }
    std::fprintf(stderr, "scene_async_tests FAILED: %d checks\n", g_failures);
    return 1;
}
