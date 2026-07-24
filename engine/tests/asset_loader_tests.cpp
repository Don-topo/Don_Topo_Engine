// Tests de AsyncAssetLoader. Headless: no crea device Vulkan, solo comprueba
// que el worker produce los mismos datos en RAM que la ruta sincrona.
//
// Las aserciones comparan contra ModelLoader::load / loadAuto, NO contra
// constantes hardcodeadas: si manana cambian los flags de Assimp, el test sigue
// siendo valido en vez de convertirse en una constante a reajustar.
#include "DonTopo/Core/JobSystem.h"
#include "DonTopo/Renderer/AsyncAssetLoader.h"
#include "DonTopo/Renderer/ModelLoader.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

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

// Un path inexistente NO lanza: devuelve error no vacio y mesh nulo. Sabotaje:
// quitar el try/catch del job — el proceso muere por std::terminate.
void testMissingFileReportsError()
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

// pumpCompleted(0) no procesa nada Y no pierde nada: el siguiente pump entrega
// todo. Sabotaje: hacer que pumpCompleted vacie el buzon antes de mirar el
// presupuesto — el segundo drain se queda a cero y el test se cuelga hasta el
// timeout, fallando el assert de tamano.
void testZeroBudgetKeepsResults(const std::string& fbx)
{
    DonTopo::JobSystem js;
    js.start();
    DonTopo::AsyncAssetLoader loader(js);

    loader.requestMesh(fbx, 1);
    // Espera activa a que el worker deje el resultado en el buzon.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (loader.pending() > 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    assert(loader.pumpCompleted(0.0f).empty() && "presupuesto 0 no procesa nada");

    std::vector<DonTopo::LoadedMesh> got = drain(loader, 1);
    assert(got.size() == 1 && "un pump con presupuesto 0 no puede perder resultados");

    js.shutdown();
}

// Las texturas del material llegan DECODIFICADAS desde el worker. Es el punto
// de la feature: si stbi_load siguiera en el hilo principal, se perderia la
// mayor parte de la ganancia. Sabotaje: en el job, no rellenar images — el
// assert de w/h salta.
//
// Solo aplica si el FBX de pruebas trae textura; si no, el caso se salta.
void testTexturesArriveDecoded(const std::string& fbx)
{
    const std::shared_ptr<DonTopo::Mesh> sync = DonTopo::ModelLoader::loadAuto(fbx);
    if (sync->material.texturePath.empty() && sync->material.embeddedTexture.empty())
    {
        std::printf("  (saltado: el FBX de pruebas no trae textura)\n");
        return;
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

} // namespace

int main()
{
    const std::string fbx = findTestFbx();

    testMissingFileReportsError();

    if (fbx.empty())
    {
        std::printf("asset_loader_tests OK (casos con FBX saltados: no se encontro el asset)\n");
        return 0;
    }

    testAsyncMatchesSync(fbx);
    testZeroBudgetKeepsResults(fbx);
    testTexturesArriveDecoded(fbx);

    std::printf("asset_loader_tests OK\n");
    return 0;
}
