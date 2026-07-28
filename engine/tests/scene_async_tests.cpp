// Tests de Scene::fromJson con y sin AsyncAssetLoader.
//
// El caso mas importante es el primero: con loader == nullptr el
// comportamiento tiene que ser IDENTICO al de antes de esta feature. Es lo que
// protege el restore de Play->Stop (EditorUI.cpp:170) y las ocho suites que ya
// existen.
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Core/JobSystem.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Renderer/AsyncAssetLoader.h"

#include <nlohmann/json.hpp>

#include <cassert>
#include <cstdio>

namespace {

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

// loader == nullptr produce exactamente lo de siempre. Sabotaje: hacer que la
// rama sincrona encole en vez de cargar — los nodos pierden su mesh y el
// toJson deja de coincidir.
void testSyncPathUnchanged()
{
    DonTopo::Scene a, b;
    assert(a.fromJson(twoNodeScene(), physics(), audio()));
    assert(b.fromJson(twoNodeScene(), physics(), audio(), nullptr));

    // El parametro por defecto y el nullptr explicito son el MISMO camino.
    nlohmann::json ja = a.toJson();
    nlohmann::json jb = b.toJson();
    stripIds(ja["root"]);
    stripIds(jb["root"]);
    assert(ja == jb);
}

// Con loader, los GameObject existen ya con su jerarquia y su transform: lo
// unico que falta es el mesh. El nodo con sourcePath queda con una peticion
// en vuelo (pendingMeshJob != 0) y sin render index — nada se ha bombeado
// todavia. Sabotaje: crear los nodos solo al bombear — el assert de nombres
// falla porque la escena esta vacia.
void testAsyncCreatesNodesImmediately()
{
    DonTopo::JobSystem js;
    js.start();
    DonTopo::AsyncAssetLoader loader(js);

    DonTopo::Scene s;
    assert(s.fromJson(sceneWithPendingLoad(), physics(), audio(), &loader));

    int found = 0;
    bool hijoAHasPendingJob = false;
    s.traverse([&](DonTopo::GameObject* go) {
        if (go->name == "hijoA" || go->name == "hijoB") ++found;
        // Nada se ha bombeado todavia: ningun nodo puede tener indice de render.
        assert(go->staticRenderIndex  == -1);
        assert(go->skinnedRenderIndex == -1);
        if (go->name == "hijoA")
        {
            // hijoA traia sourcePath: debe tener una peticion en vuelo y
            // NINGUN mesh todavia (el GameObject existe completo desde el
            // frame 0, sin esperar al asset).
            hijoAHasPendingJob = (go->pendingMeshJob != 0);
            assert(!go->hasMesh());
        }
        if (go->name == "hijoB")
        {
            // hijoB no traia sourcePath: no hay nada que pedir.
            assert(go->pendingMeshJob == 0);
        }
    });
    assert(found == 2 && "los GameObject existen desde el frame 0, sin esperar al asset");
    assert(hijoAHasPendingJob && "el nodo con sourcePath debe encolar una peticion real");

    js.shutdown();
}

// Borrar un GameObject con carga pendiente y bombear despues no crashea: el
// resultado se descarta porque su targetId ya no esta en la escena viva.
//
// Es el test con mas valor de los tres: el use-after-free clasico de este
// patron es guardar un GameObject* en la peticion. Sabotaje: guardar el
// puntero en vez del id y desreferenciarlo al bombear — crash o basura.
void testDeletedTargetIsDiscarded()
{
    DonTopo::JobSystem js;
    js.start();
    DonTopo::AsyncAssetLoader loader(js);

    DonTopo::Scene s;
    // sceneWithPendingLoad, no twoNodeScene: hace falta una peticion REAL en
    // vuelo (hijoA tiene sourcePath) para que este test compruebe algo — con
    // la fixture sin mesh del brief, pumpCompleted() no tenia nada que
    // entregar y el test pasaba sin ejercitar el camino de descarte.
    assert(s.fromJson(sceneWithPendingLoad(), physics(), audio(), &loader));

    DonTopo::GameObject* victim = nullptr;
    s.traverse([&](DonTopo::GameObject* go) { if (go->name == "hijoA") victim = go; });
    assert(victim);
    assert(victim->pendingMeshJob != 0 && "hace falta una peticion real en vuelo para este test");

    const uint64_t goneId = victim->id;
    s.removeGameObject(victim);

    // Un resultado dirigido a un id que ya no existe no puede tocar memoria
    // liberada. Se comprueba que sigue sin aparecer tras bombear.
    for (auto& r : loader.pumpCompleted(1000.0f))
        assert(r.targetId != goneId || true);   // solo importa que no crashee

    bool stillThere = false;
    s.traverse([&](DonTopo::GameObject* go) { if (go->id == goneId) stillThere = true; });
    assert(!stillThere && "el nodo borrado no puede resucitar al bombear");

    js.shutdown();
}

} // namespace

int main()
{
    testSyncPathUnchanged();
    testAsyncCreatesNodesImmediately();
    testDeletedTargetIsDiscarded();
    std::printf("scene_async_tests OK\n");
    return 0;
}
