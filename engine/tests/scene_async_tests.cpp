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
            CHECK(hijoA->getMesh().get() != fabricated.get(), "debe ser copia profunda, no el mismo objeto compartido");
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

    jobSystem.shutdown();

    if (g_failures == 0)
    {
        std::printf("scene_async_tests OK\n");
        return 0;
    }
    std::fprintf(stderr, "scene_async_tests FAILED: %d checks\n", g_failures);
    return 1;
}
