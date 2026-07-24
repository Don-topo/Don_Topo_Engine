// Tests de AsyncAssetLoader. Headless: no crea device Vulkan, solo comprueba
// que el worker produce los mismos datos en RAM que la ruta sincrona.
//
// Las aserciones comparan contra ModelLoader::load / loadAuto, NO contra
// constantes hardcodeadas: si manana cambian los flags de Assimp, el test sigue
// siendo valido en vez de convertirse en una constante a reajustar.
//
// Cada caso corre kIters veces, igual que jobsystem_tests.cpp: un race que
// aparece 1 de cada 20 ejecuciones no se caza en una sola pasada.
#include "DonTopo/Core/JobSystem.h"
#include "DonTopo/Renderer/AsyncAssetLoader.h"
#include "DonTopo/Renderer/ModelLoader.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kIters = 50;

// Ruta al FBX de pruebas. El test se salta los casos que dependen de el si no
// existe, en vez de fallar: un clone sin assets debe poder correr el resto.
//
// No hay assets/models/cube.fbx en el repo; assets/modelTexture.fbx si existe
// y ademas trae textura de albedo, lo que ejercita testTexturesArriveDecoded.
// Se prueban varias profundidades porque el exe de test corre headless desde
// build-ninja/engine/tests/.
std::string findTestFbx()
{
    for (const char* rel : { "assets/modelTexture.fbx", "../assets/modelTexture.fbx",
                             "../../assets/modelTexture.fbx", "../../../assets/modelTexture.fbx",
                             "../../../../assets/modelTexture.fbx",
                             "assets/model.fbx", "../assets/model.fbx",
                             "../../assets/model.fbx", "../../../assets/model.fbx",
                             "../../../../assets/model.fbx" })
        if (std::filesystem::exists(rel)) return rel;
    return {};
}

// Bombea hasta que lleguen `expected` resultados o se agote el plazo. Devuelve
// lo recogido. Sin plazo, un fallo del loader colgaria el test para siempre.
std::vector<DonTopo::LoadedMesh> drain(DonTopo::AsyncAssetLoader& loader,
                                       size_t expected, int timeoutMs = 30000)
{
    std::vector<DonTopo::LoadedMesh> out;
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeoutMs);
    while (out.size() < expected && std::chrono::steady_clock::now() < deadline)
    {
        for (auto& r : loader.pumpCompleted(1000.0f))
            out.push_back(std::move(r));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return out;
}

// El mesh que sale del worker es el mismo que da la ruta sincrona. Sabotaje:
// en el job, cambiar loadAuto(path) por loadAuto(path) y vaciar mesh->indices
// — el assert de indices salta.
void testAsyncMatchesSync(const std::string& fbx)
{
    for (int it = 0; it < kIters; ++it)
    {
        const std::shared_ptr<DonTopo::Mesh> expected = DonTopo::ModelLoader::loadAuto(fbx);
        assert(expected && "la ruta sincrona debe cargar el FBX de pruebas");

        DonTopo::JobSystem js;
        js.start();
        DonTopo::AsyncAssetLoader loader(js);

        loader.requestMesh(fbx, /*targetId=*/42);
        std::vector<DonTopo::LoadedMesh> got = drain(loader, 1);

        assert(got.size() == 1 && "el buzon debe entregar exactamente un resultado");
        assert(got[0].error.empty() && "un FBX valido no debe reportar error");
        assert(got[0].targetId == 42 && "el targetId debe viajar intacto");
        assert(got[0].mesh != nullptr);
        assert(got[0].mesh->vertices.size() == expected->vertices.size());
        assert(got[0].mesh->indices.size()  == expected->indices.size());
        assert(got[0].mesh->name            == expected->name);

        js.shutdown();
    }
}

// Un path inexistente NO lanza: devuelve error no vacio y mesh nulo. Sabotaje:
// quitar el try/catch del job — el proceso muere por std::terminate (o, con
// la red de seguridad de JobSystem::workerLoop ya puesta, el resultado nunca
// llega al buzon y drain() agota el timeout: el assert de tamano igual salta).
void testMissingFileReportsError()
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start();
        DonTopo::AsyncAssetLoader loader(js);

        loader.requestMesh("no/existe/ningun/fichero.fbx", 7);
        std::vector<DonTopo::LoadedMesh> got = drain(loader, 1);

        assert(got.size() == 1);
        assert(!got[0].error.empty() && "un path invalido debe llenar error");
        assert(got[0].mesh == nullptr && "sin mesh cuando hay error");

        js.shutdown();
    }
}

// pumpCompleted(0) no procesa nada Y no pierde nada: el siguiente pump entrega
// todo. Sabotaje: hacer que pumpCompleted vacie el buzon antes de mirar el
// presupuesto — el segundo drain se queda a cero y el test se cuelga hasta el
// timeout, fallando el assert de tamano.
//
// kIters a 50 como el resto de casos de concurrencia (pump/leftover/cancel):
// la version anterior esperaba a "pending() == 0" antes de tocar
// pumpCompleted, pero pending() SOLO decrementa cuando pumpCompleted entrega
// algo (ver el header de AsyncAssetLoader) — ese while nunca salia antes del
// deadline, asi que cada pasada quemaba 30s fijos por diseno, no por el coste
// real de la aserción. El fix es detectar el "ya esta listo" CONSUMIENDO con
// un presupuesto real (pumpCompleted(1000ms)) dentro del propio bucle de
// espera, en vez de mirar pending(): cada iteracion termina en cuanto el
// worker postea, del orden de milisegundos, y 50 pasadas quedan dominadas
// solo por 50 cargas reales del FBX.
void testZeroBudgetKeepsResults(const std::string& fbx)
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start();
        DonTopo::AsyncAssetLoader loader(js);

        loader.requestMesh(fbx, 1);

        bool everReady = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            // Presupuesto 0: SIEMPRE vacio, este resultado ya este listo o
            // no — es la aserción central del test, se comprueba en CADA
            // vuelta del sondeo, no solo una vez al final.
            assert(loader.pumpCompleted(0.0f).empty() && "presupuesto 0 no procesa nada");

            // Detecta que el worker ya termino consumiendo con presupuesto
            // real. Si llega algo, es el resultado que buscabamos: ni
            // pumpCompleted(0) lo devolvio antes (assert de arriba) ni lo
            // perdio (si no, este pump se quedaria vacio para siempre y el
            // assert de everReady de mas abajo saltaria al agotar el plazo).
            std::vector<DonTopo::LoadedMesh> got = loader.pumpCompleted(1000.0f);
            if (!got.empty())
            {
                assert(got.size() == 1 && "un pump con presupuesto 0 no puede perder resultados");
                everReady = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(everReady && "el resultado debe llegar; presupuesto 0 no puede perderlo");

        js.shutdown();
    }
}

// Las texturas del material llegan DECODIFICADAS desde el worker. Es el punto
// de la feature: si stbi_load siguiera en el hilo principal, se perderia la
// mayor parte de la ganancia. Sabotaje: en el job, no rellenar images — el
// assert de w/h salta.
//
// Solo aplica si el FBX de pruebas trae textura; si no, el caso se salta.
//
// NOTA: los dos únicos FBX trackeados en este repo (model.fbx y
// modelTexture.fbx) son personajes Mixamo CON rig — ModelLoader::loadAuto
// siempre devuelve un SkinnedMesh para ellos, y SkinnedMesh guarda sus
// texturas en materials[] (plural, por submesh), no en el Mesh::material
// (singular) que decodeSlot() lee aquí. Con los assets de este repo este
// caso se salta siempre — ver el comentario de runJob() en
// AsyncAssetLoader.cpp y el informe de la Task 2 (hallazgo documentado,
// decisión: diferido).
void testTexturesArriveDecoded(const std::string& fbx)
{
    for (int it = 0; it < kIters; ++it)
    {
        const std::shared_ptr<DonTopo::Mesh> sync = DonTopo::ModelLoader::loadAuto(fbx);
        if (sync->material.texturePath.empty() && sync->material.embeddedTexture.empty())
        {
            if (it == 0) std::printf("  (saltado: el FBX de pruebas no trae textura)\n");
            continue;
        }

        DonTopo::JobSystem js;
        js.start();
        DonTopo::AsyncAssetLoader loader(js);

        loader.requestMesh(fbx, 3);
        std::vector<DonTopo::LoadedMesh> got = drain(loader, 1);
        assert(got.size() == 1 && got[0].error.empty());

        bool foundAlbedo = false;
        for (const auto& img : got[0].images)
            if (img.slot == DonTopo::DecodedImage::Albedo)
            {
                foundAlbedo = true;
                assert(img.w > 1 && img.h > 1 && "una textura real no es 1x1");
                assert(img.pixels.size() == static_cast<size_t>(img.w) * img.h * 4
                       && "los pixeles llegan en RGBA8, sin padding");
            }
        assert(foundAlbedo && "el worker debe decodificar el albedo, no dejarlo al main");

        js.shutdown();
    }
}

// cancel() de una peticion todavia en cola no debe dejar pending() colgado
// para siempre. Con 1 solo worker y un job bloqueante por delante, la
// peticion de requestMesh() no puede haber arrancado cuando llega cancel():
// JobSystem la salta en workerLoop (igual que testCancelPreventsQueuedJob en
// jobsystem_tests.cpp) y AsyncAssetLoader::runJob() nunca se ejecuta para ese
// id — nadie mas iba a decrementar m_pending. Sabotaje: en cancel(), no tocar
// m_pending (dejar solo el m_jobs.cancel(id)) — pending() se queda en 1 para
// siempre y el ultimo assert salta.
void testCancelBeforeStartDropsPending()
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start(1);   // 1 hilo: garantiza que la peticion se quede en cola

        std::atomic<bool> release{false};
        js.submit([&release] { while (!release.load(std::memory_order_acquire)) {} });

        DonTopo::AsyncAssetLoader loader(js);
        assert(loader.pending() == 0 && "sin peticiones, pending() debe empezar en 0");

        const DonTopo::JobSystem::JobId id =
            loader.requestMesh("no/importa/no/deberia/cargar/nunca.fbx", 99);
        assert(loader.pending() == 1 && "requestMesh debe contar como pendiente de inmediato");

        loader.cancel(id);

        release.store(true, std::memory_order_release);
        js.shutdown();   // drena la cola: el job cancelado se salta ahi

        // Nada debe llegar para este id: el worker nunca lo ejecuto.
        std::vector<DonTopo::LoadedMesh> got = loader.pumpCompleted(1000.0f);
        assert(got.empty() && "un job cancelado antes de arrancar no debe entregar resultado");
        assert(loader.pending() == 0 &&
               "cancel() debe liberar el contador de pending, no dejarlo colgado");
    }
}

// Cuatro peticiones del mismo path con targetId distintos comparten UN ReadFile
// y producen cuatro LoadedMesh de contenido identico con punteros DISTINTOS.
//
// Los dos lados importan. Comprobar solo el contenido pasaria tambien
// compartiendo el shared_ptr, que es justo lo que el diseno descarta: hoy cada
// nodo de Scene::nodeFromJson tiene su propio make_shared<Mesh> (Scene.cpp:721),
// y dos GameObject sobre el mismo Mesh mutable cambiaria esa semantica.
//
// Fresh JobSystem+loader por iteracion, como el resto de casos: readFileCount()
// es acumulativo por loader, asi que con un loader nuevo por vuelta la
// aserción "== 1" vale en cada pasada. Cuatro peticiones deduplicadas = un solo
// ReadFile por iteracion, mas barato que testAsyncMatchesSync (que carga dos
// veces por vuelta), asi que las 50 iteraciones no penalizan.
//
// Sabotaje: devolver el mismo shared_ptr a los cuatro (quitar la copia en
// buildResultFor) — el assert de punteros distintos salta.
void testDedupSharesReadFileNotPointers(const std::string& fbx)
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start();
        DonTopo::AsyncAssetLoader loader(js);

        for (uint64_t t = 100; t < 104; ++t)
            loader.requestMesh(fbx, t);

        std::vector<DonTopo::LoadedMesh> got = drain(loader, 4);
        assert(got.size() == 4 && "cuatro peticiones, cuatro resultados");

        assert(loader.readFileCount() == 1 && "cuatro peticiones del mismo path = un solo ReadFile");

        std::vector<uint64_t> targets;
        for (const auto& r : got)
        {
            assert(r.error.empty());
            assert(r.mesh != nullptr);
            assert(r.mesh->vertices.size() == got[0].mesh->vertices.size());
            assert(r.mesh->indices.size()  == got[0].mesh->indices.size());
            targets.push_back(r.targetId);
        }

        // Los cuatro targetId son distintos y estan los cuatro esperados.
        std::sort(targets.begin(), targets.end());
        assert((targets == std::vector<uint64_t>{100, 101, 102, 103}));

        // Y los punteros NO se comparten.
        for (size_t i = 0; i < got.size(); ++i)
            for (size_t k = i + 1; k < got.size(); ++k)
                assert(got[i].mesh.get() != got[k].mesh.get()
                       && "cada target recibe su propia copia del Mesh");

        js.shutdown();
    }
}

} // namespace

int main()
{
    const std::string fbx = findTestFbx();

    testMissingFileReportsError();
    testCancelBeforeStartDropsPending();

    if (fbx.empty())
    {
        std::printf("asset_loader_tests OK (casos con FBX saltados: no se encontro el asset)\n");
        return 0;
    }

    testAsyncMatchesSync(fbx);
    testZeroBudgetKeepsResults(fbx);
    testTexturesArriveDecoded(fbx);
    testDedupSharesReadFileNotPointers(fbx);

    std::printf("asset_loader_tests OK\n");
    return 0;
}
