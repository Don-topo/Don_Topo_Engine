// Test headless del exportador de juego (sin GUI, sin GPU, sin PhysX, sin
// Lua). Plain main + asserts, mismo patrón que content_browser_tests.cpp.
//
// Los Mesh se construyen a mano en vez de vía ModelLoader: el test no
// necesita geometría real, solo los campos de path. Y no se crea ningún
// collider, así que no se instancia PhysicsManager — el motor solo admite una
// PxFoundation por proceso.
#include "DonTopo/Editor/GameExporter.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Scripting/ScriptComponent.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <system_error>
#include <vector>

using namespace DonTopo;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Crea un proyecto de mentira en disco con los ficheros que la escena de
// prueba referenciará. Devuelve la raíz canonicalizada.
static fs::path makeProjectFixture()
{
    std::error_code ec;
    fs::path root = fs::temp_directory_path(ec) / "dt_exporter_test";
    fs::remove_all(root, ec);
    fs::create_directories(root / "assets" / "chars", ec);
    fs::create_directories(root / "Scripts", ec);
    std::ofstream(root / "assets" / "hero.fbx")            << "fbx";
    std::ofstream(root / "assets" / "hero_diffuse.png")    << "png";
    std::ofstream(root / "assets" / "chars" / "enemy.fbx") << "fbx";
    std::ofstream(root / "assets" / "step.wav")            << "wav";
    std::ofstream(root / "Scripts" / "Player.lua")         << "-- lua";
    fs::path canon = fs::canonical(root, ec);
    return ec ? root : canon;
}

// Mesh estático con path de origen y, opcionalmente, textura difusa.
static std::shared_ptr<Mesh> makeMesh(const fs::path& source, const fs::path& diffuse = {})
{
    auto m = std::make_shared<Mesh>();
    m->sourcePath = source.string();
    if (!diffuse.empty())
        m->material.texturePath = diffuse.string();
    return m;
}

// Criterio de aceptación 2: 2 meshes + 1 textura + 1 script + 1 audio -> 5
// paths exactos, ni uno más.
static void test_collects_exactly_referenced(const fs::path& root)
{
    Scene scene;

    auto* hero = scene.addGameObject("hero");
    hero->setMesh(makeMesh(root / "assets" / "hero.fbx", root / "assets" / "hero_diffuse.png"));
    hero->addScript(std::make_unique<ScriptComponent>("Player", hero));

    auto* enemy = scene.addGameObject("enemy");
    enemy->setMesh(makeMesh(root / "assets" / "chars" / "enemy.fbx"));
    enemy->setAudioClip(std::make_shared<AudioClipComponent>(
        nullptr, (root / "assets" / "step.wav").string(), -1, false, false));

    std::map<std::string, fs::path> scriptPaths{ { "Player", root / "Scripts" / "Player.lua" } };
    std::vector<ExportAsset> assets = collectSceneAssets(scene, root, scriptPaths);

    CHECK(assets.size() == 5);
    for (const ExportAsset& a : assets)
        CHECK(a.existsOnDisk);

    // Ordenado por packagePath: el orden es determinista y comprobable.
    std::vector<std::string> pkg;
    for (const ExportAsset& a : assets) pkg.push_back(a.packagePath);
    CHECK(std::find(pkg.begin(), pkg.end(), "assets/hero.fbx")         != pkg.end());
    CHECK(std::find(pkg.begin(), pkg.end(), "assets/hero_diffuse.png") != pkg.end());
    CHECK(std::find(pkg.begin(), pkg.end(), "assets/chars/enemy.fbx")  != pkg.end());
    CHECK(std::find(pkg.begin(), pkg.end(), "assets/step.wav")         != pkg.end());
    CHECK(std::find(pkg.begin(), pkg.end(), "Scripts/Player.lua")      != pkg.end());
}

// Mesh procedural (Cube/Sphere/Plane/Capsule): sourcePath vacío, geometría ya
// serializada en el .scene. No aporta ningún asset.
static void test_procedural_mesh_contributes_nothing(const fs::path& root)
{
    Scene scene;
    auto* cube = scene.addGameObject("cube");
    cube->setMesh(std::make_shared<Mesh>()); // sourcePath vacío

    CHECK(collectSceneAssets(scene, root, {}).empty());
}

// Dos GameObjects con el mismo FBX -> una sola entrada.
static void test_deduplicates_shared_mesh(const fs::path& root)
{
    Scene scene;
    scene.addGameObject("a")->setMesh(makeMesh(root / "assets" / "hero.fbx"));
    scene.addGameObject("b")->setMesh(makeMesh(root / "assets" / "hero.fbx"));

    CHECK(collectSceneAssets(scene, root, {}).size() == 1);
}

// Las animationSources extra se recolectan; la fuente builtin comparte valor
// con sourcePath y no debe duplicarlo.
static void test_animation_sources(const fs::path& root)
{
    std::ofstream(root / "assets" / "run.fbx") << "fbx";

    Scene scene;
    auto skinned = std::make_shared<SkinnedMesh>();
    skinned->sourcePath = (root / "assets" / "hero.fbx").string();
    skinned->animationSources.push_back({ (root / "assets" / "hero.fbx").string(), true,  {} });
    skinned->animationSources.push_back({ (root / "assets" / "run.fbx").string(),  false, {} });
    scene.addGameObject("hero")->setMesh(skinned);

    std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    CHECK(assets.size() == 2);
}

// loadSkinned NUNCA puebla el Material heredado de Mesh: reparte uno por
// submalla en SkinnedMesh::materials. materialsOf() tiene una rama aparte
// para leer justo ese vector, y sin este test nadie la ejercitaba (el resto
// de tests deja SkinnedMesh::materials vacío). Ese punto ciego exacto ya
// causó un bug real en el Content Browser: borrar una textura usada por un
// personaje con rig informaba "0 objetos afectados" en un diálogo
// destructivo, porque el buscador de referencias solo miraba `mesh.material`.
static void test_skinned_mesh_materials(const fs::path& root)
{
    std::ofstream(root / "assets" / "hero_skin.png") << "png";

    Scene scene;
    auto skinned = std::make_shared<SkinnedMesh>();
    skinned->sourcePath = (root / "assets" / "hero.fbx").string();
    Material submeshMaterial;
    submeshMaterial.texturePath = (root / "assets" / "hero_skin.png").string();
    skinned->materials.push_back(submeshMaterial);
    scene.addGameObject("hero")->setMesh(skinned);

    std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});

    std::vector<std::string> pkg;
    for (const ExportAsset& a : assets) pkg.push_back(a.packagePath);
    CHECK(std::find(pkg.begin(), pkg.end(), "assets/hero_skin.png") != pkg.end());

    // Y la textura debe llegar marcada como existente, no solo presente.
    auto it = std::find_if(assets.begin(), assets.end(), [](const ExportAsset& a) {
        return a.packagePath == "assets/hero_skin.png";
    });
    CHECK(it != assets.end() && it->existsOnDisk);
}

// Asset fuera de la raíz del proyecto -> assets/_external/, con sufijo ante
// colisión de nombres.
static void test_external_assets(const fs::path& root)
{
    std::error_code ec;
    fs::path outside = fs::temp_directory_path(ec) / "dt_exporter_outside";
    fs::remove_all(outside, ec);
    fs::create_directories(outside / "a", ec);
    fs::create_directories(outside / "b", ec);
    std::ofstream(outside / "a" / "prop.fbx") << "fbx";
    std::ofstream(outside / "b" / "prop.fbx") << "fbx";

    Scene scene;
    scene.addGameObject("a")->setMesh(makeMesh(outside / "a" / "prop.fbx"));
    scene.addGameObject("b")->setMesh(makeMesh(outside / "b" / "prop.fbx"));

    std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    CHECK(assets.size() == 2);
    CHECK(assets[0].packagePath != assets[1].packagePath);
    for (const ExportAsset& a : assets)
        CHECK(a.packagePath.rfind("assets/_external/", 0) == 0);

    fs::remove_all(outside, ec);
}

// Un asset referenciado que no está en disco se marca, no se filtra: el
// llamador necesita listarlos en el error.
static void test_missing_asset_flagged(const fs::path& root)
{
    Scene scene;
    scene.addGameObject("ghost")->setMesh(makeMesh(root / "assets" / "no_existe.fbx"));

    std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    CHECK(assets.size() == 1);
    CHECK(!assets[0].existsOnDisk);
}

int main()
{
    fs::path root = makeProjectFixture();

    test_collects_exactly_referenced(root);
    test_procedural_mesh_contributes_nothing(root);
    test_deduplicates_shared_mesh(root);
    test_animation_sources(root);
    test_skinned_mesh_materials(root);
    test_external_assets(root);
    test_missing_asset_flagged(root);

    std::error_code ec;
    fs::remove_all(root, ec);

    if (g_failures) { std::printf("%d FAILURES\n", g_failures); return 1; }
    std::printf("OK\n");
    return 0;
}
