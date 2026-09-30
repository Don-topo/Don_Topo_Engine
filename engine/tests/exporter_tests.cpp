// Headless test of the game exporter (no GUI, no GPU, no PhysX, no
// Lua). Plain main + asserts, same pattern as content_browser_tests.cpp.
//
// The Meshes are built by hand instead of via ModelLoader: the test does not
// need real geometry, only the path fields. And no collider is created, so
// PhysicsManager is not instantiated: the engine only supports one
// PxFoundation per process.
#include "DonTopo/Editor/GameExporter.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/CameraComponent.h"
#include "DonTopo/Core/ImportSettings.h"
#include "DonTopo/Core/MaterialAsset.h"
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Scripting/ScriptComponent.h"
#include "DonTopo/UI/ButtonComponent.h"

#include <nlohmann/json.hpp>

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

// Creates a fake project on disk with the files that the test scene
// will reference. Returns the canonicalized root.
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
    std::ofstream(root / "assets" / "ui_atlas.png")        << "png";
    // The default UI font lives INSIDE the project: the fixture
    // reproduces it at its same relative path.
    fs::create_directories((root / DonTopo::kDefaultUiFontPath).parent_path(), ec);
    std::ofstream(root / DonTopo::kDefaultUiFontPath)      << "ttf";
    fs::path canon = fs::canonical(root, ec);
    return ec ? root : canon;
}

// Static mesh with a source path and, optionally, a diffuse texture.
static std::shared_ptr<Mesh> makeMesh(const fs::path& source, const fs::path& diffuse = {})
{
    auto m = std::make_shared<Mesh>();
    m->sourcePath = source.string();
    if (!diffuse.empty())
        m->material.texturePath = diffuse.string();
    return m;
}

// Acceptance criterion 2: 2 meshes + 1 texture + 1 script + 1 audio -> 5
// exact paths, not one more.
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

    // Sorted by packagePath: the order is deterministic and checkable.
    std::vector<std::string> pkg;
    for (const ExportAsset& a : assets) pkg.push_back(a.packagePath);
    CHECK(std::find(pkg.begin(), pkg.end(), "assets/hero.fbx")         != pkg.end());
    CHECK(std::find(pkg.begin(), pkg.end(), "assets/hero_diffuse.png") != pkg.end());
    CHECK(std::find(pkg.begin(), pkg.end(), "assets/chars/enemy.fbx")  != pkg.end());
    CHECK(std::find(pkg.begin(), pkg.end(), "assets/step.wav")         != pkg.end());
    CHECK(std::find(pkg.begin(), pkg.end(), "Scripts/Player.lua")      != pkg.end());
}

// A Button contributes its atlas and its font. Without its own font, the one that travels is the
// default one: it is the one the runtime will draw, and without it the exported game
// comes out with mute buttons.
static void test_button_assets(const fs::path& root)
{
    const std::map<std::string, fs::path> noScripts;

    {   // Without text there is no label to draw: no font to copy.
        Scene scene;
        auto* go = scene.addGameObject("mudo");
        go->setButton(std::make_shared<ButtonComponent>());
        CHECK(collectSceneAssets(scene, root, noScripts).empty());
    }

    {   // With text and without its own font: the default one, and with its
        // project-relative path intact inside the package.
        Scene scene;
        auto* go = scene.addGameObject("aceptar");
        auto b = std::make_shared<ButtonComponent>();
        b->text = "Aceptar";
        go->setButton(b);

        std::vector<ExportAsset> assets = collectSceneAssets(scene, root, noScripts);
        CHECK(assets.size() == 1);
        if (assets.size() == 1)
        {
            CHECK(assets[0].packagePath == DonTopo::kDefaultUiFontPath);
            CHECK(assets[0].existsOnDisk);
        }
    }

    {   // Own atlas + own font: its own, and the default one is NOT copied.
        Scene scene;
        auto* go = scene.addGameObject("skin");
        auto b = std::make_shared<ButtonComponent>();
        b->text      = "Jugar";
        b->atlasPath = (root / "assets" / "ui_atlas.png").string();
        b->fontPath  = (root / "assets" / "hero.fbx").string();   // any file will do
        go->setButton(b);

        std::vector<ExportAsset> assets = collectSceneAssets(scene, root, noScripts);
        CHECK(assets.size() == 2);
        std::vector<std::string> pkg;
        for (const ExportAsset& a : assets) pkg.push_back(a.packagePath);
        CHECK(std::find(pkg.begin(), pkg.end(), "assets/ui_atlas.png") != pkg.end());
        CHECK(std::find(pkg.begin(), pkg.end(), "assets/hero.fbx")     != pkg.end());
        CHECK(std::find(pkg.begin(), pkg.end(), DonTopo::kDefaultUiFontPath) == pkg.end());

        // And the .scene comes out pointing to the PACKAGE's paths, not to those of the PC
        // that exported.
        std::map<std::string, std::string> sourceToPackage;
        for (const ExportAsset& a : assets)
            sourceToPackage[exportPathKey(a.sourcePath)] = a.packagePath;

        nlohmann::json j = scene.toJson();
        CHECK(rewriteScenePaths(j, sourceToPackage, scene.assetRoot()) == 2);
        const nlohmann::json& btn = j["root"]["children"][0]["button"];
        CHECK(btn["atlasPath"] == "assets/ui_atlas.png");
        CHECK(btn["fontPath"]  == "assets/hero.fbx");
    }
}

// Procedural mesh (Cube/Sphere/Plane/Capsule): empty sourcePath, geometry already
// serialized in the .scene. It contributes no asset.
static void test_procedural_mesh_contributes_nothing(const fs::path& root)
{
    Scene scene;
    auto* cube = scene.addGameObject("cube");
    cube->setMesh(std::make_shared<Mesh>()); // empty sourcePath

    CHECK(collectSceneAssets(scene, root, {}).empty());
}

// Two GameObjects with the same FBX -> a single entry.
static void test_deduplicates_shared_mesh(const fs::path& root)
{
    Scene scene;
    scene.addGameObject("a")->setMesh(makeMesh(root / "assets" / "hero.fbx"));
    scene.addGameObject("b")->setMesh(makeMesh(root / "assets" / "hero.fbx"));

    CHECK(collectSceneAssets(scene, root, {}).size() == 1);
}

// The extra animationSources are collected; the builtin source shares its value
// with sourcePath and must not duplicate it.
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

// loadSkinned NEVER populates the Material inherited from Mesh: it hands out one per
// submesh in SkinnedMesh::materials. materialsOf() has a separate branch
// to read exactly that vector, and without this test nobody exercised it (the rest
// of the tests leave SkinnedMesh::materials empty). That exact blind spot already
// caused a real bug in the Content Browser: deleting a texture used by a
// rigged character reported "0 objects affected" in a destructive
// dialog, because the reference finder only looked at `mesh.material`.
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

    // And the texture must arrive marked as existing, not just present.
    auto it = std::find_if(assets.begin(), assets.end(), [](const ExportAsset& a) {
        return a.packagePath == "assets/hero_skin.png";
    });
    CHECK(it != assets.end() && it->existsOnDisk);
}

// Asset outside the project root -> assets/_external/, with a suffix on
// name collision.
static void test_external_assets(const fs::path& root)
{
    std::error_code ec;
    fs::path tempRoot = fs::temp_directory_path(ec);
    if (ec || tempRoot.empty())
    {
        // Without a temporary directory there is no safe place to write: the test fails
        // instead of falling back to a path relative to the CWD, which would leave the
        // remove_all calls below pointing inside the repo.
        CHECK(false);
        return;
    }

    fs::path outside = tempRoot / "dt_exporter_outside";
    fs::remove_all(outside, ec);
    fs::create_directories(outside / "a", ec);
    fs::create_directories(outside / "b", ec);
    // Each FBX with its sibling texture and THE SAME name in both folders:
    // it is the case that the old scheme (flatten to assets/_external/<name> with
    // numeric suffix) silently broke.
    std::ofstream(outside / "a" / "prop.fbx") << "fbx";
    std::ofstream(outside / "a" / "prop.png") << "png";
    std::ofstream(outside / "b" / "prop.fbx") << "fbx";
    std::ofstream(outside / "b" / "prop.png") << "png";

    Scene scene;
    scene.addGameObject("a")->setMesh(makeMesh(outside / "a" / "prop.fbx", outside / "a" / "prop.png"));
    scene.addGameObject("b")->setMesh(makeMesh(outside / "b" / "prop.fbx", outside / "b" / "prop.png"));

    std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    CHECK(assets.size() == 4);
    for (const ExportAsset& a : assets)
        CHECK(a.packagePath.rfind("assets/_external/", 0) == 0);

    // The invariant that really matters is not the literal paths, but that
    // each FBX remains a sibling of ITS texture: ModelLoader derives the
    // texture as dirname(fbx)/basename (ModelLoader.cpp:156), so if the
    // two pairs end up in the same folder, the second model loads the texture
    // of the first and the render comes out wrong with no error. Asserting only
    // "packagePath[0] != packagePath[1]" did not detect that: the old scheme
    // also gave different names.
    auto packagePathOf = [&](const fs::path& source) {
        const std::string key = exportPathKey(source.string());
        for (const ExportAsset& a : assets)
            if (exportPathKey(a.sourcePath) == key) return a.packagePath;
        return std::string();
    };

    const fs::path fbxA = fs::path(packagePathOf(outside / "a" / "prop.fbx")).parent_path();
    const fs::path texA = fs::path(packagePathOf(outside / "a" / "prop.png")).parent_path();
    const fs::path fbxB = fs::path(packagePathOf(outside / "b" / "prop.fbx")).parent_path();
    const fs::path texB = fs::path(packagePathOf(outside / "b" / "prop.png")).parent_path();

    CHECK(!fbxA.empty() && !fbxB.empty());
    CHECK(fbxA == texA);   // the pair of 'a' travels together
    CHECK(fbxB == texB);   // the pair of 'b' travels together
    CHECK(fbxA != fbxB);   // and the two pairs go to different folders

    fs::remove_all(outside, ec);
}

// A referenced asset that is not on disk is flagged, not filtered out: the
// caller needs to list them in the error.
static void test_missing_asset_flagged(const fs::path& root)
{
    Scene scene;
    scene.addGameObject("ghost")->setMesh(makeMesh(root / "assets" / "no_existe.fbx"));

    std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    CHECK(assets.size() == 1);
    CHECK(!assets[0].existsOnDisk);
}

// After rewriting, no path in the .scene can remain absolute: the
// package runs on another machine and another directory.
static void test_rewrite_makes_paths_relative(const fs::path& root)
{
    Scene scene;

    auto* hero = scene.addGameObject("hero");
    auto skinned = std::make_shared<SkinnedMesh>();
    skinned->sourcePath = (root / "assets" / "hero.fbx").string();
    skinned->animationSources.push_back({ (root / "assets" / "run.fbx").string(), false, {} });
    hero->setMesh(skinned);
    hero->setAudioClip(std::make_shared<AudioClipComponent>(
        nullptr, (root / "assets" / "step.wav").string(), -1, false, false));

    std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    std::map<std::string, std::string> sourceToPackage;
    for (const ExportAsset& a : assets)
        sourceToPackage[exportPathKey(a.sourcePath)] = a.packagePath;

    nlohmann::json j = scene.toJson();
    int rewritten = rewriteScenePaths(j, sourceToPackage, scene.assetRoot());

    // hero.fbx (mesh) + hero.fbx (builtin animationSource? there is none) +
    // run.fbx + step.wav = 3 rewritten fields.
    CHECK(rewritten == 3);

    const nlohmann::json& node = j["root"]["children"][0];
    CHECK(node["mesh"]["sourcePath"].get<std::string>() == "assets/hero.fbx");
    CHECK(node["mesh"]["animationSources"][0]["path"].get<std::string>() == "assets/run.fbx");
    CHECK(node["audioClip"]["path"].get<std::string>() == "assets/step.wav");

    // No residual absolute path (on Windows: no drive ':').
    CHECK(node["mesh"]["sourcePath"].get<std::string>().find(':') == std::string::npos);
    CHECK(node["audioClip"]["path"].get<std::string>().find(':') == std::string::npos);
}

// Review Focus (exporter): the .mat travels with the scene, and matAsset is
// rewritten to its path inside the package.
static void test_mat_asset_is_collected_and_rewritten(const fs::path& root)
{
    const fs::path mat = root / "assets" / "rojo.mat";
    std::string err;
    CHECK(saveMaterialAsset(mat, MaterialAsset{}, &err));

    Scene scene;
    scene.setAssetRoot(root.string());
    auto* go = scene.addGameObject("prop");
    go->setMesh(makeMesh(root / "assets" / "hero.fbx"));
    MaterialOverride ov; ov.index = 0; ov.matAsset = mat.string();
    go->materialOverrides.push_back(ov);

    std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    std::vector<std::string> pkg;
    for (const ExportAsset& a : assets) pkg.push_back(a.packagePath);
    CHECK(std::find(pkg.begin(), pkg.end(), "assets/rojo.mat") != pkg.end());

    std::map<std::string, std::string> sourceToPackage;
    for (const ExportAsset& a : assets)
        sourceToPackage[exportPathKey(a.sourcePath)] = a.packagePath;

    nlohmann::json j = scene.toJson();
    CHECK(j["root"]["children"][0]["mesh"]["materials"][0]["matAsset"].get<std::string>() == "assets/rojo.mat");

    const int rewritten = rewriteScenePaths(j, sourceToPackage, scene.assetRoot());
    CHECK(rewritten >= 1);
    CHECK(j["root"]["children"][0]["mesh"]["materials"][0]["matAsset"].get<std::string>() == "assets/rojo.mat");
}

// The editor's real layout: the user's project lives in a subfolder
// (projects/<name>) of the export root (the editor's own directory, which holds
// the runtime, shaders and skybox). Scene::toJson stores matAsset and texture
// overrides RELATIVE to the scene's assetRoot (the project), so a lookup keyed
// on the raw stored string resolved it against the working directory, missed,
// and left "assets/Materials/x.mat" in game.scene while the package had it at
// projects/<name>/assets/Materials/x.mat: the exported procedural cube lost its
// albedo and normal map in both backends. The test above hid it because there
// assetRoot and the export root are the same folder.
static void test_rewrite_resolves_paths_stored_relative_to_project(const fs::path& root)
{
    const fs::path project = root / "projects" / "p";
    const fs::path mat     = project / "assets" / "Materials" / "Test.mat";
    const fs::path normal  = project / "assets" / "Textures" / "n.png";
    std::error_code ec;
    fs::create_directories(mat.parent_path(), ec);
    fs::create_directories(normal.parent_path(), ec);
    std::ofstream(normal) << "png";
    std::string err;
    CHECK(saveMaterialAsset(mat, MaterialAsset{}, &err));

    Scene scene;
    scene.setAssetRoot(project.string());
    auto* go = scene.addGameObject("Cube");
    auto mesh = makeMesh({});   // procedural: no source file
    mesh->material.normalMapPath = normal.string();
    go->setMesh(mesh);
    MaterialOverride ov;
    ov.index    = 0;
    ov.matAsset = mat.string();
    ov.normal   = normal.string();
    go->materialOverrides.push_back(ov);

    std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    std::map<std::string, std::string> sourceToPackage;
    for (const ExportAsset& a : assets)
        sourceToPackage[exportPathKey(a.sourcePath)] = a.packagePath;
    CHECK(sourceToPackage[exportPathKey(mat.string())] == "projects/p/assets/Materials/Test.mat");

    nlohmann::json j = scene.toJson();
    const nlohmann::json& before = j["root"]["children"][0]["mesh"]["materials"][0];
    // Precondition: stored relative to the project, not to the export root.
    CHECK(before["matAsset"].get<std::string>() == "assets/Materials/Test.mat");

    CHECK(rewriteScenePaths(j, sourceToPackage, scene.assetRoot()) == 2);
    const nlohmann::json& after = j["root"]["children"][0]["mesh"]["materials"][0];
    CHECK(after["matAsset"].get<std::string>() == "projects/p/assets/Materials/Test.mat");
    CHECK(after["normal"].get<std::string>()   == "projects/p/assets/Textures/n.png");

    fs::remove_all(root / "projects", ec);
}

// A path that is not in the map is left intact, it is neither deleted nor emptied.
static void test_rewrite_leaves_unknown_paths(const fs::path& root)
{
    nlohmann::json j;
    j["version"] = 1;
    j["root"] = { { "name", "root" },
                  { "mesh", { { "sourcePath", "C:/otro/sitio/x.fbx" } } },
                  { "children", nlohmann::json::array() } };

    int rewritten = rewriteScenePaths(j, {}, {});
    CHECK(rewritten == 0);
    CHECK(j["root"]["mesh"]["sourcePath"].get<std::string>() == "C:/otro/sitio/x.fbx");
}

// The mesh.materials block (texture overrides set by hand from
// Properties, Task 6) is as much an asset as sourcePath or audioClip, but
// rewriteNode did not know about it: the file WAS packaged needlessly
// (collectSceneAssets already walks it via materialsOf), but the path that
// ended up written in the package's game.scene was still the editor's
// on-disk one. An override OUTSIDE the project root is the case that
// gives it away: toStoredPath already leaves it absolute for that case (same as it does
// with sourcePath), so without the fix it reaches the package intact, pointing to the
// machine that exported and not to assets/_external/.
static void test_rewrite_materials_override_outside_root(const fs::path& root)
{
    std::error_code ec;
    fs::path tempRoot = fs::temp_directory_path(ec);
    if (ec || tempRoot.empty())
    {
        // Same criterion as test_external_assets: without temp_directory_path there is
        // nowhere to write safely.
        CHECK(false);
        return;
    }
    fs::path outside = tempRoot / "dt_exporter_materials_outside";
    fs::remove_all(outside, ec);
    fs::create_directories(outside, ec);
    const fs::path albedoFile = outside / "override_albedo.png";
    std::ofstream(albedoFile) << "png";

    Scene scene;
    scene.setAssetRoot(root.string());

    auto* go = scene.addGameObject("prop");
    auto mesh = makeMesh(root / "assets" / "hero.fbx");
    // The override lives in two places at once: Material (what
    // collectSceneAssets walks via materialsOf) and
    // GameObject::materialOverrides (what nodeToJson serializes). In the
    // real path applyMaterialOverrides sets them together; here, with the mesh
    // built by hand, both are replicated on purpose.
    mesh->material.texturePath = albedoFile.string();
    go->setMesh(mesh);
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = albedoFile.string();
    go->materialOverrides.push_back(ov);

    std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    std::map<std::string, std::string> sourceToPackage;
    for (const ExportAsset& a : assets)
        sourceToPackage[exportPathKey(a.sourcePath)] = a.packagePath;
    const std::string expectedPackagePath = sourceToPackage[exportPathKey(albedoFile.string())];
    CHECK(!expectedPackagePath.empty());

    nlohmann::json j = scene.toJson();
    // Before rewriting: the override is still absolute, exactly as toStoredPath
    // left it for falling outside the root. If this failed, the rest of the test
    // would not be testing what it claims to test.
    CHECK(j["root"]["children"][0]["mesh"]["materials"][0]["albedo"].get<std::string>()
          == albedoFile.string());

    int rewritten = rewriteScenePaths(j, sourceToPackage, scene.assetRoot());
    // sourcePath (hero.fbx, inside the root) + materials[0].albedo: 2 fields.
    CHECK(rewritten == 2);

    const nlohmann::json& mats = j["root"]["children"][0]["mesh"]["materials"];
    CHECK(mats.size() == 1);
    const std::string albedoAfter = mats[0]["albedo"].get<std::string>();
    CHECK(albedoAfter == expectedPackagePath);
    // And above all: it is no longer the absolute path of the machine that exported.
    CHECK(albedoAfter.find(':') == std::string::npos);

    fs::remove_all(outside, ec);
}

// The package contains the renamed exe, game.scene, the plan's assets, the
// skybox, the shaders and Scripts/, and no project asset that the scene
// does not reference (acceptance criterion 3).
static void test_package_contents(const fs::path& root)
{
    std::error_code ec;

    // Complete the fixture with what writeExportPackage adds on its own.
    fs::create_directories(root / "assets" / "skybox", ec);
    for (const char* face : { "px", "nx", "py", "ny", "pz", "nz" })
        std::ofstream(root / "assets" / "skybox" / (std::string(face) + ".png")) << "png";
    fs::create_directories(root / "shaders", ec);
    std::ofstream(root / "shaders" / "triangle.vert.spv") << "spv";
    // The DirectX 12 backend .dxil files travel alongside the .spv.
    std::ofstream(root / "shaders" / "triangle.vert.dxil") << "dxil";
    // Project asset that the scene does NOT reference: it must not end up copied.
    std::ofstream(root / "assets" / "huerfano.fbx") << "fbx";
    // Fake runtime.
    std::ofstream(root / "DonTopoRuntime.exe") << "MZ";

    Scene scene;
    scene.addGameObject("hero")->setMesh(makeMesh(root / "assets" / "hero.fbx"));
    std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});

    fs::path tempRoot = fs::temp_directory_path(ec);
    // Without temp_directory_path there is nowhere to write safely: if ec
    // is set or the path comes back empty, "dest" would become relative to the
    // current working directory and the remove_all/writeExportPackage below
    // would operate outside the temp, breaking the contract that this test
    // only touches fs::temp_directory_path().
    if (ec || tempRoot.empty())
    {
        CHECK(!ec && !tempRoot.empty());
        return;
    }
    fs::path dest = tempRoot / "dt_exporter_out";
    fs::remove_all(dest, ec);

    // It is exported asking for DirectX 12 on purpose, and not the default:
    // with Vulkan, a game.cfg that was never written would give exactly the
    // same result as one written correctly.
    ExportResult r = writeExportPackage(assets, scene.toJson(), dest, "MiJuego",
                                        root, root / "Scripts", root / "DonTopoRuntime.exe",
                                        RenderBackend::D3D12);

    const fs::path pkg = dest / "MiJuego";
    CHECK(r.ok);
    // The executable name is decided by the table of the exporting platform:
    // MiJuego.exe on Windows, MiJuego on Linux.
    CHECK(fs::exists(pkg / ("MiJuego" + exportPlatformFor(platform::currentOs()).executableSuffix)));
    CHECK(fs::exists(pkg / "game.scene"));
    CHECK(fs::exists(pkg / "assets" / "hero.fbx"));
    CHECK(fs::exists(pkg / "assets" / "skybox" / "px.png"));
    CHECK(fs::exists(pkg / "assets" / "skybox" / "nz.png"));
    CHECK(fs::exists(pkg / "shaders" / "triangle.vert.spv"));
    CHECK(fs::exists(pkg / "shaders" / "triangle.vert.dxil"));
    CHECK(fs::exists(pkg / "Scripts" / "Player.lua"));

    // The chosen backend reaches the package, and arrives with the name the runtime
    // knows how to read.
    CHECK(fs::exists(pkg / "game.cfg"));
    {
        std::ifstream  cfgIn(pkg / "game.cfg");
        nlohmann::json cfg;
        bool           parsed = false;
        if (cfgIn.is_open())
        {
            try { cfgIn >> cfg; parsed = true; } catch (const std::exception&) {}
        }
        CHECK(parsed);
        CHECK(parsed && cfg.is_object() && cfg.contains("renderBackend"));
        CHECK(parsed && cfg.value("renderBackend", std::string{}) == "DirectX 12");
    }
    // Criterion 3: the unreferenced asset stays out.
    CHECK(!fs::exists(pkg / "assets" / "huerfano.fbx"));

    // Exact number of files, not just "> 0": an off-by-one or a
    // category counted too many would not be detected by a lax CHECK. Breakdown for
    // this specific scene/fixture:
    //   1  MiJuego.exe            (renamed runtimeExe)
    //   1  assets/hero.fbx        (only asset the scene references)
    //   6  assets/skybox/*.png    (the 6 faces, hardcoded, always go)
    //   1  shaders/triangle.vert.spv (only .spv created by this fixture)
    //   1  shaders/triangle.vert.dxil (only .dxil created by this fixture)
    //   1  Scripts/Player.lua     (only file under Scripts/ in the fixture)
    //   0  fmod.dll               (this fixture does not create it)
    //   1  game.scene
    //   1  game.cfg               (startup backend)
    //  = 13
    CHECK(r.fileCount == 13);
    CHECK(r.totalBytes > 0);

    // Finding 1: totalBytes must also include game.scene. It is checked by
    // recomputing the real on-disk size of the whole copied package and
    // demanding that it match exactly what was reported; without adding
    // game.scene, this CHECK would fail on the low side by exactly the size of that
    // file.
    std::uintmax_t diskTotal = 0;
    std::error_code walkEc;
    for (fs::recursive_directory_iterator it(pkg, walkEc), end; !walkEc && it != end; it.increment(walkEc))
    {
        if (!it->is_regular_file()) continue;
        std::error_code sizeEc;
        diskTotal += fs::file_size(it->path(), sizeEc);
    }
    CHECK(r.totalBytes == diskTotal);

    fs::remove_all(dest, ec);
}

// The splash logo is copied as splash.png next to the .exe even if the scene
// does not reference it (the scene does not reference it; the runtime looks for it by that
// fixed name). It reuses the skybox/shaders/DonTopoRuntime.exe that
// test_package_contents already left in root.
static void test_package_includes_splash(const fs::path& root)
{
    std::error_code ec;
    std::ofstream(root / "assets" / "MainEngineLogo.png") << "png";

    Scene scene;
    scene.addGameObject("hero")->setMesh(makeMesh(root / "assets" / "hero.fbx"));
    std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});

    fs::path tempRoot = fs::temp_directory_path(ec);
    if (ec || tempRoot.empty())
    {
        CHECK(!ec && !tempRoot.empty());
        return;
    }
    fs::path dest = tempRoot / "dt_exporter_splash_out";
    fs::remove_all(dest, ec);

    ExportResult r = writeExportPackage(assets, scene.toJson(), dest, "MiJuego",
                                        root, root / "Scripts", root / "DonTopoRuntime.exe");

    const fs::path pkg = dest / "MiJuego";
    CHECK(r.ok);
    CHECK(fs::exists(pkg / "splash.png"));

    fs::remove_all(dest, ec);
}

// Re-exporting over a package from a previous export leaves it clean: nothing
// from the old export survives. The folder carries game.scene, the only mark
// that inspectExportTarget accepts as "this is mine and I can regenerate it"
// (GameExporter.cpp: inspectExportTarget); without it this same fixture
// would become Occupied and writeExportPackage would abort without deleting anything,
// which is exactly the other case covered by test_writeExportPackage_aborts_on_occupied.
static void test_package_overwrite_is_clean(const fs::path& root)
{
    std::error_code ec;
    fs::path tempRoot = fs::temp_directory_path(ec);
    // Same reason as in test_package_contents: without this check, a
    // temp_directory_path failure would make "dest" fall outside the temp.
    if (ec || tempRoot.empty())
    {
        CHECK(!ec && !tempRoot.empty());
        return;
    }
    fs::path dest = tempRoot / "dt_exporter_out2";
    fs::remove_all(dest, ec);
    fs::create_directories(dest / "MiJuego" / "assets", ec);
    std::ofstream(dest / "MiJuego" / "assets" / "basura_vieja.fbx") << "x";
    std::ofstream(dest / "MiJuego" / "game.scene") << "{}";

    Scene scene;
    scene.addGameObject("hero")->setMesh(makeMesh(root / "assets" / "hero.fbx"));

    ExportResult r = writeExportPackage(collectSceneAssets(scene, root, {}), scene.toJson(),
                                        dest, "MiJuego", root, root / "Scripts",
                                        root / "DonTopoRuntime.exe");
    CHECK(r.ok);
    CHECK(!fs::exists(dest / "MiJuego" / "assets" / "basura_vieja.fbx"));

    fs::remove_all(dest, ec);
}

// A .mat stores its textures relative to its own folder. Copied verbatim, a
// texture outside the project (packaged under assets/_external/N) was still
// referenced by its path on the exporting machine ("../../../Temp/..."), so the
// game only found it there. The package's .mat must point at the packaged
// copy, and the texture must be collected even when the in-memory material
// does not carry it (only the matAsset override names it).
static void test_mat_textures_are_packaged_and_repointed(const fs::path& root)
{
    std::error_code ec;
    const fs::path tempRoot = fs::temp_directory_path(ec);
    if (ec || tempRoot.empty()) { CHECK(!ec && !tempRoot.empty()); return; }

    const fs::path outside = tempRoot / "dt_exporter_mat_outside";
    fs::remove_all(outside, ec);
    fs::create_directories(outside, ec);
    const fs::path albedo = outside / "far_albedo.png";
    std::ofstream(albedo) << "png";

    const fs::path mat = root / "assets" / "Materials" / "far.mat";
    fs::create_directories(mat.parent_path(), ec);
    MaterialAsset asset;
    asset.albedo    = albedo.string();
    asset.roughness = 0.25f;
    std::string err;
    CHECK(saveMaterialAsset(mat, asset, &err));

    Scene scene;
    scene.setAssetRoot(root.string());
    auto* go = scene.addGameObject("Cube");
    go->setMesh(makeMesh({}));   // procedural, material not applied in memory
    MaterialOverride ov; ov.index = 0; ov.matAsset = mat.string();
    go->materialOverrides.push_back(ov);

    const std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    std::string albedoPkg;
    for (const ExportAsset& a : assets)
        if (exportPathKey(a.sourcePath) == exportPathKey(albedo.string())) albedoPkg = a.packagePath;
    CHECK(!albedoPkg.empty());

    const fs::path dest = tempRoot / "dt_exporter_out_mat";
    fs::remove_all(dest, ec);
    const ExportResult r = writeExportPackage(assets, scene.toJson(), dest, "MiJuego", root,
                                              root / "Scripts", root / "DonTopoRuntime.exe");
    CHECK(r.ok);

    const fs::path pkg    = dest / "MiJuego";
    const fs::path pkgMat = pkg / "assets" / "Materials" / "far.mat";
    const MaterialAsset loaded = loadMaterialAsset(pkgMat);
    CHECK(!albedoPkg.empty() &&
          exportPathKey(loaded.albedo) == exportPathKey((pkg / fs::path(albedoPkg)).string()));
    CHECK(loaded.roughness == 0.25f);   // the rest of the material survives
    std::ifstream in(pkgMat);
    const nlohmann::json raw = nlohmann::json::parse(in, nullptr, false);
    CHECK(raw.is_object() && raw.value("albedo", std::string(":")).find(':') == std::string::npos);
    // Relative inside the package, never through the exporting machine's folders.
    CHECK(raw.is_object() &&
          raw.value("albedo", std::string("dt_exporter_mat_outside")).find("dt_exporter_mat_outside") == std::string::npos);

    fs::remove_all(dest, ec);
    fs::remove_all(outside, ec);
    fs::remove(mat, ec);
}

// A destination directory with foreign content (without game.scene) makes
// writeExportPackage abort WITHOUT touching anything: what was there is neither deleted nor is the
// package created. It is the case that broke before inspectExportTarget/Occupied; the
// real example is <repo>/assets, which remove_all used to take out
// (see the writeExportPackage comment about the destination state).
static void test_writeExportPackage_aborts_on_occupied(const fs::path& root)
{
    std::error_code ec;
    fs::path tempRoot = fs::temp_directory_path(ec);
    if (ec || tempRoot.empty())
    {
        CHECK(!ec && !tempRoot.empty());
        return;
    }
    fs::path dest = tempRoot / "dt_exporter_occupied";
    fs::remove_all(dest, ec);
    fs::create_directories(dest / "MiJuego", ec);
    std::ofstream(dest / "MiJuego" / "algo_del_usuario.fbx") << "dato importante";

    Scene scene;
    scene.addGameObject("hero")->setMesh(makeMesh(root / "assets" / "hero.fbx"));

    ExportResult r = writeExportPackage(collectSceneAssets(scene, root, {}), scene.toJson(),
                                        dest, "MiJuego", root, root / "Scripts",
                                        root / "DonTopoRuntime.exe");
    CHECK(!r.ok);
    CHECK(!r.messages.empty());
    // The foreign content is still exactly where it was: neither a partial
    // remove_all nor a create_directories on top of it touched it.
    CHECK(fs::exists(dest / "MiJuego" / "algo_del_usuario.fbx"));

    fs::remove_all(dest, ec);
}

// inspectExportTarget in its four states: the classification looks at WHAT is
// inside the directory, not WHERE it is, and fails closed (Occupied) on
// anything that does not fit a recognized pattern.
static void test_inspect_export_target_states()
{
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec) / "dt_exporter_inspect";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);

    // Missing: it does not exist at all.
    CHECK(inspectExportTarget(base / "no_existe") == ExportTargetState::Missing);

    // Empty: it exists and is empty.
    fs::path empty = base / "vacia";
    fs::create_directories(empty, ec);
    CHECK(inspectExportTarget(empty) == ExportTargetState::Empty);

    // PriorPackage: it exists and contains game.scene at its root.
    fs::path prior = base / "paquete_previo";
    fs::create_directories(prior, ec);
    std::ofstream(prior / "game.scene") << "{}";
    CHECK(inspectExportTarget(prior) == ExportTargetState::PriorPackage);

    // Occupied: it exists with content that is not a game.scene.
    fs::path occupied = base / "ocupada";
    fs::create_directories(occupied, ec);
    std::ofstream(occupied / "documento_del_usuario.txt") << "no me borres";
    CHECK(inspectExportTarget(occupied) == ExportTargetState::Occupied);

    // Occupied also for a file (not a directory) with that name:
    // remove_all would take it out all the same, and it is not a package of ours.
    fs::path fileTarget = base / "esto_es_un_fichero.txt";
    std::ofstream(fileTarget) << "x";
    CHECK(inspectExportTarget(fileTarget) == ExportTargetState::Occupied);

    fs::remove_all(base, ec);
}

// isValidExportGameName: the cases that writeExportPackage needs to reject
// before building destDir/name, because that path is the one it later deletes.
static void test_valid_export_game_name()
{
    std::string reason;
    CHECK(!isValidExportGameName("..", reason));               // goes up one level
    CHECK(!isValidExportGameName(".", reason));                 // Win32 discards it when creating the folder
    CHECK(!isValidExportGameName("C:\\Windows", reason));       // absolute: operator/ would ignore destDir
    CHECK(!isValidExportGameName("carpeta/nombre", reason));    // separator '/'
    CHECK(!isValidExportGameName("carpeta\\nombre", reason));   // separator '\'
    CHECK(!isValidExportGameName("nombre*raro", reason));       // reserved Windows character
    CHECK(!isValidExportGameName("   ", reason));                // whitespace only
    CHECK(!isValidExportGameName("NUL", reason));                // reserved device name
    CHECK(isValidExportGameName("MiJuegoValido", reason));
}

// Without a runtime binary nothing is exported: explicit error and folder not created.
static void test_missing_runtime_aborts(const fs::path& root)
{
    std::error_code ec;
    fs::path tempRoot = fs::temp_directory_path(ec);
    // Same reason as in test_package_contents: without this check, a
    // temp_directory_path failure would make "dest" fall outside the temp.
    if (ec || tempRoot.empty())
    {
        CHECK(!ec && !tempRoot.empty());
        return;
    }
    fs::path dest = tempRoot / "dt_exporter_out3";
    fs::remove_all(dest, ec);

    Scene scene;
    ExportResult r = writeExportPackage({}, scene.toJson(), dest, "MiJuego",
                                        root, root / "Scripts", root / "no_existe_runtime.exe");
    CHECK(!r.ok);
    CHECK(!r.messages.empty());
    // Contract: without a runtime not even the destination folder
    // is created, it is not merely aborted "halfway".
    CHECK(!fs::exists(dest / "MiJuego"));

    fs::remove_all(dest, ec);
}

// Incomplete skybox (one of the 6 faces is missing) -> ok == false with a message:
// Skybox.cpp:84 throws at startup if any is missing, so an export
// "completed" with fewer than 6 faces produces a package that does not start.
static void test_incomplete_skybox_marks_not_ok()
{
    std::error_code ec;
    fs::path fixRoot = fs::temp_directory_path(ec) / "dt_exporter_skybox_fixture";
    fs::remove_all(fixRoot, ec);
    fs::create_directories(fixRoot / "assets" / "skybox", ec);
    fs::create_directories(fixRoot / "shaders", ec);
    // Only 5 of the 6 faces: "nz" is missing.
    for (const char* face : { "px", "nx", "py", "ny", "pz" })
        std::ofstream(fixRoot / "assets" / "skybox" / (std::string(face) + ".png")) << "png";
    std::ofstream(fixRoot / "shaders" / "triangle.vert.spv") << "spv";
    std::ofstream(fixRoot / "DonTopoRuntime.exe") << "MZ";

    fs::path dest = fs::temp_directory_path(ec) / "dt_exporter_skybox_out";
    fs::remove_all(dest, ec);

    Scene scene;
    ExportResult r = writeExportPackage({}, scene.toJson(), dest, "MiJuego",
                                        fixRoot, fixRoot / "Scripts", fixRoot / "DonTopoRuntime.exe");

    CHECK(!r.ok);
    bool hasSkyboxMsg = std::any_of(r.messages.begin(), r.messages.end(), [](const std::string& m) {
        return m.find("skybox") != std::string::npos;
    });
    CHECK(hasSkyboxMsg);

    fs::remove_all(fixRoot, ec);
    fs::remove_all(dest, ec);
}

// Zero .spv shaders copied -> ok == false with a message: without any the
// runtime dies in createPipeline, and a Log saying "completed" would hide
// exactly the failure that makes the package unbootable.
static void test_zero_shaders_marks_not_ok()
{
    std::error_code ec;
    fs::path fixRoot = fs::temp_directory_path(ec) / "dt_exporter_noshader_fixture";
    fs::remove_all(fixRoot, ec);
    fs::create_directories(fixRoot / "assets" / "skybox", ec);
    fs::create_directories(fixRoot / "shaders", ec); // exists but empty: 0 .spv
    for (const char* face : { "px", "nx", "py", "ny", "pz", "nz" })
        std::ofstream(fixRoot / "assets" / "skybox" / (std::string(face) + ".png")) << "png";
    std::ofstream(fixRoot / "DonTopoRuntime.exe") << "MZ";

    fs::path dest = fs::temp_directory_path(ec) / "dt_exporter_noshader_out";
    fs::remove_all(dest, ec);

    Scene scene;
    ExportResult r = writeExportPackage({}, scene.toJson(), dest, "MiJuego",
                                        fixRoot, fixRoot / "Scripts", fixRoot / "DonTopoRuntime.exe");

    CHECK(!r.ok);
    bool hasShaderMsg = std::any_of(r.messages.begin(), r.messages.end(), [](const std::string& m) {
        return m.find("shader") != std::string::npos;
    });
    CHECK(hasShaderMsg);

    fs::remove_all(fixRoot, ec);
    fs::remove_all(dest, ec);
}

// A package exported from a Debug editor links the MSVC debug CRT, which is not
// redistributable: it starts on the machine that exported it and fails with
// "ucrtbased.dll is missing" on anyone else's. The export is still
// valid (testing locally is legitimate), but it has to say so, because it is a
// failure that no test on the developer's machine can uncover.
//
// The test checks BOTH branches depending on how the test itself was compiled: in
// Debug the warning has to be there, and in Release it CANNOT be (a warning that
// always showed up would be noise that ends up ignored, exactly when it matters).
static void test_debug_build_warns_about_crt(const fs::path& root)
{
    std::error_code ec;
    fs::path fixRoot = fs::temp_directory_path(ec) / "dt_exporter_crt_fixture";
    fs::remove_all(fixRoot, ec);
    fs::create_directories(fixRoot / "assets" / "skybox", ec);
    fs::create_directories(fixRoot / "shaders", ec);
    for (const char* face : { "px", "nx", "py", "ny", "pz", "nz" })
        std::ofstream(fixRoot / "assets" / "skybox" / (std::string(face) + ".png")) << "png";
    std::ofstream(fixRoot / "shaders" / "triangle.vert.spv") << "spv";
    std::ofstream(fixRoot / "DonTopoRuntime.exe") << "MZ";
    (void)root;

    fs::path dest = fs::temp_directory_path(ec) / "dt_exporter_crt_out";
    fs::remove_all(dest, ec);

    Scene scene;
    ExportResult r = writeExportPackage({}, scene.toJson(), dest, "MiJuego",
                                        fixRoot, fixRoot / "Scripts", fixRoot / "DonTopoRuntime.exe",
                                        // Tests the MSVC CRT: a Windows row, wherever it runs.
                                        RenderBackend::Vulkan, "assets/skybox",
                                        exportPlatformFor(platform::Os::Windows));
    CHECK(r.ok); // the package is correct: this is a warning, not an error

    bool avisa = std::any_of(r.messages.begin(), r.messages.end(), [](const std::string& m) {
        return m.find("Debug") != std::string::npos && m.find("ucrtbased") != std::string::npos;
    });
#ifndef NDEBUG
    CHECK(avisa);
#else
    CHECK(!avisa);
#endif

    fs::remove_all(fixRoot, ec);
    fs::remove_all(dest, ec);
}

// A Release package has to carry the MSVC CRT (VCRUNTIME140.dll,
// MSVCP140.dll...) inside: on a machine without Visual Studio or the VC++
// Redistributable the .exe does not start. The DLLs are collected from next to the editor
// (here, from the fixture acting as projectRoot), same as fmod.dll.
//
// BOTH branches are checked depending on how the test was compiled, and in Debug the
// requirement is the opposite: do NOT copy anything. The debug CRT is not
// redistributable, so a Debug that copied it would be handing out DLLs that
// Microsoft does not allow to be redistributed, and the Debug warning would stop making
// sense.
static void test_release_package_bundles_msvc_crt()
{
    std::error_code ec;
    fs::path fixRoot = fs::temp_directory_path(ec) / "dt_exporter_msvccrt_fixture";
    fs::remove_all(fixRoot, ec);
    fs::create_directories(fixRoot / "assets" / "skybox", ec);
    fs::create_directories(fixRoot / "shaders", ec);
    for (const char* face : { "px", "nx", "py", "ny", "pz", "nz" })
        std::ofstream(fixRoot / "assets" / "skybox" / (std::string(face) + ".png")) << "png";
    std::ofstream(fixRoot / "shaders" / "triangle.vert.spv") << "spv";
    std::ofstream(fixRoot / "DonTopoRuntime.exe") << "MZ";
    // The three CRT DLLs with different capitalization: the filter compares in
    // lowercase and on Windows the real names come with uppercase letters.
    std::ofstream(fixRoot / "MSVCP140.dll")      << "dll";
    std::ofstream(fixRoot / "VCRUNTIME140.dll")  << "dll";
    std::ofstream(fixRoot / "vcruntime140_1.dll") << "dll";
    // Decoys: a DLL that is not from the CRT and a file whose name starts
    // the same but is not .dll. Copying them would put garbage in the package.
    std::ofstream(fixRoot / "otra.dll")          << "dll";
    std::ofstream(fixRoot / "msvcp140.lib")      << "lib";

    fs::path dest = fs::temp_directory_path(ec) / "dt_exporter_msvccrt_out";
    fs::remove_all(dest, ec);

    Scene scene;
    ExportResult r = writeExportPackage({}, scene.toJson(), dest, "MiJuego",
                                        fixRoot, fixRoot / "Scripts", fixRoot / "DonTopoRuntime.exe",
                                        // Tests the MSVC CRT: a Windows row, wherever it runs.
                                        RenderBackend::Vulkan, "assets/skybox",
                                        exportPlatformFor(platform::Os::Windows));
    const fs::path pkg = dest / "MiJuego";
    CHECK(r.ok);

    // Base of the package for this fixture: MiJuego.exe + 6 skybox faces +
    // 1 .spv + game.scene = 9 files. In Release the 3 CRT DLLs are added.
#ifdef NDEBUG
    CHECK(fs::exists(pkg / "MSVCP140.dll"));
    CHECK(fs::exists(pkg / "VCRUNTIME140.dll"));
    CHECK(fs::exists(pkg / "vcruntime140_1.dll"));
    CHECK(!fs::exists(pkg / "otra.dll"));
    CHECK(!fs::exists(pkg / "msvcp140.lib"));
    // +1 over what it counted before game.cfg existed.
    CHECK(r.fileCount == 13);
#else
    CHECK(!fs::exists(pkg / "MSVCP140.dll"));
    CHECK(!fs::exists(pkg / "VCRUNTIME140.dll"));
    CHECK(!fs::exists(pkg / "vcruntime140_1.dll"));
    CHECK(r.fileCount == 10);
#endif

    // Without any CRT DLL next to the editor: warning in Release (not an error,
    // the rest of the package is correct) and absolute silence in Debug, where not
    // copying the CRT is correct and warning would be noise.
    fs::remove(fixRoot / "MSVCP140.dll", ec);
    fs::remove(fixRoot / "VCRUNTIME140.dll", ec);
    fs::remove(fixRoot / "vcruntime140_1.dll", ec);
    fs::remove_all(dest, ec);

    ExportResult r2 = writeExportPackage({}, scene.toJson(), dest, "MiJuego",
                                         fixRoot, fixRoot / "Scripts", fixRoot / "DonTopoRuntime.exe",
                                         RenderBackend::Vulkan, "assets/skybox",
                                         exportPlatformFor(platform::Os::Windows));
    CHECK(r2.ok);
    bool avisaCrt = std::any_of(r2.messages.begin(), r2.messages.end(), [](const std::string& m) {
        return m.find("CRT de MSVC") != std::string::npos &&
               m.find("next to the editor") != std::string::npos;
    });
#ifdef NDEBUG
    CHECK(avisaCrt);
    CHECK(r2.fileCount == 10);
#else
    CHECK(!avisaCrt);
    CHECK(r2.fileCount == 10);
#endif

    fs::remove_all(fixRoot, ec);
    fs::remove_all(dest, ec);
}

// exportGame aborts without a camera in the scene, before touching disk: without one
// the game could not render and the failure must happen here, not in an .exe
// that opens a black window.
static void test_exportGame_aborts_without_camera(const fs::path& root)
{
    std::error_code ec;
    if (!fs::exists(root / "DonTopoRuntime.exe", ec))
        std::ofstream(root / "DonTopoRuntime.exe") << "MZ";

    Scene scene; // no CameraComponent on any GameObject
    scene.addGameObject("hero")->setMesh(makeMesh(root / "assets" / "hero.fbx"));

    fs::path dest = fs::temp_directory_path(ec) / "dt_exporter_nocam_out";
    fs::remove_all(dest, ec);

    ExportResult r = exportGame(scene, {}, dest, "MiJuego", root,
                                root / "Scripts", root / "DonTopoRuntime.exe");
    CHECK(!r.ok);
    CHECK(!r.messages.empty());
    CHECK(!fs::exists(dest / "MiJuego")); // aborts before creating anything

    fs::remove_all(dest, ec);
}

// exportGame aborts if any referenced asset does not exist on disk. The
// scene DOES have a camera, to isolate exactly this guard from the one
// above.
static void test_exportGame_aborts_missing_asset(const fs::path& root)
{
    std::error_code ec;
    if (!fs::exists(root / "DonTopoRuntime.exe", ec))
        std::ofstream(root / "DonTopoRuntime.exe") << "MZ";

    Scene scene;
    auto* cam = scene.addGameObject("cam");
    cam->setCameraComponent(std::make_shared<CameraComponent>());
    scene.addGameObject("ghost")->setMesh(makeMesh(root / "assets" / "no_existe_de_verdad.fbx"));

    fs::path dest = fs::temp_directory_path(ec) / "dt_exporter_missingasset_out";
    fs::remove_all(dest, ec);

    ExportResult r = exportGame(scene, {}, dest, "MiJuego", root,
                                root / "Scripts", root / "DonTopoRuntime.exe");
    CHECK(!r.ok);
    CHECK(!r.messages.empty());
    CHECK(!fs::exists(dest / "MiJuego"));

    fs::remove_all(dest, ec);
}

// The two rows of the table, checked from any OS: the Linux one is not
// left untested until someone exports on Linux.
static void test_export_platform_rows()
{
    const ExportPlatform w = exportPlatformFor(platform::Os::Windows);
    CHECK(w.executableSuffix == ".exe" && !w.setExecutableBit && w.copyMsvcCrt && w.warnDebugCrt && !w.warnGlibc);
    CHECK(w.audioLibPrefixes == std::vector<std::string>{"fmod.dll"});

    const ExportPlatform l = exportPlatformFor(platform::Os::Linux);
    CHECK(l.executableSuffix.empty() && l.setExecutableBit && !l.copyMsvcCrt && !l.warnDebugCrt && l.warnGlibc);
    CHECK(l.audioLibPrefixes == std::vector<std::string>{"libfmod.so."});

    // Only the soname: the development .so and the full version are the same
    // file repeated.
    CHECK(isAudioLibFile("libfmod.so.13", l));
    CHECK(!isAudioLibFile("libfmod.so", l) && !isAudioLibFile("libfmod.so.13.2", l));
    CHECK(!isAudioLibFile("libfmodL.so.13", l));   // the logging variant does not go
    CHECK(isAudioLibFile("fmod.dll", w) && !isAudioLibFile("fmodL.dll", w));
}

// A LINUX package written from any OS. It reuses the skybox, the
// shaders and the fake runtime that test_package_contents left in root. It goes
// last in main: it leaves fake libraries in root and deletes them when it finishes.
static void test_linux_package(const fs::path& root)
{
    std::error_code ec;
    std::ofstream(root / "libfmod.so.13")  << "so";
    std::ofstream(root / "libfmodL.so.13") << "so";
    std::ofstream(root / "libfmod.so")     << "so";
    std::ofstream(root / "msvcp140.dll")   << "dll";

    Scene scene;
    scene.addGameObject("hero")->setMesh(makeMesh(root / "assets" / "hero.fbx"));
    const fs::path tempRoot = fs::temp_directory_path(ec);
    if (ec || tempRoot.empty()) { CHECK(!ec && !tempRoot.empty()); return; }
    const fs::path dest = tempRoot / "dt_exporter_linux";
    fs::remove_all(dest, ec);

    const ExportResult r = writeExportPackage(collectSceneAssets(scene, root, {}), scene.toJson(),
                                              dest, "MiJuego", root, root / "Scripts",
                                              root / "DonTopoRuntime.exe", RenderBackend::Vulkan,
                                              "assets/skybox", exportPlatformFor(platform::Os::Linux));
    const fs::path pkg = dest / "MiJuego";
    CHECK(r.ok);
    CHECK(fs::exists(pkg / "MiJuego") && !fs::exists(pkg / "MiJuego.exe"));
    CHECK(!fs::exists(pkg / "msvcp140.dll"));     // el CRT de MSVC no va en Linux
    CHECK(!fs::exists(pkg / "libfmodL.so.13"));
    CHECK(!fs::exists(pkg / "libfmod.so"));
#ifdef DT_FMOD_ENABLED
    CHECK(fs::exists(pkg / "libfmod.so.13"));
#endif
    if (platform::currentOs() == platform::Os::Linux)
    {
        const fs::perms pr = fs::status(pkg / "MiJuego", ec).permissions();
        CHECK((pr & fs::perms::owner_exec) != fs::perms::none);
    }

    fs::remove_all(dest, ec);
    for (const char* f : { "libfmod.so.13", "libfmodL.so.13", "libfmod.so", "msvcp140.dll" })
        fs::remove(root / f, ec);
}

// The runtime reads "<texture>.import.json" relative to the texture: it has to travel
// in the package with the same hierarchy. Only MATERIAL textures.
static void test_texture_sidecar_travels_with_the_texture(const fs::path& root)
{
    std::error_code ec;
    const fs::path tex = root / "assets" / "tablero.png";
    std::ofstream(tex) << "png";
    TextureImportSettings s;
    s.mipmaps = true;
    std::string err;
    CHECK(saveTextureImportSettings(tex, s, &err));

    const fs::path plain = root / "assets" / "liso.png";        // no sidecar
    std::ofstream(plain) << "png";

    Scene scene;
    auto* go = scene.addGameObject("suelo");
    go->setMesh(makeMesh(root / "assets" / "hero.fbx", tex));
    auto* go2 = scene.addGameObject("pared");
    go2->setMesh(makeMesh(root / "assets" / "chars" / "enemy.fbx", plain));

    const std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    std::vector<std::string> pkg;
    for (const ExportAsset& a : assets) pkg.push_back(a.packagePath);

    CHECK(std::find(pkg.begin(), pkg.end(), "assets/tablero.png")             != pkg.end());
    CHECK(std::find(pkg.begin(), pkg.end(), "assets/tablero.png.import.json") != pkg.end());
    CHECK(std::find(pkg.begin(), pkg.end(), "assets/liso.png.import.json")    == pkg.end());
    for (const ExportAsset& a : assets)
        if (a.packagePath == "assets/tablero.png.import.json") CHECK(a.existsOnDisk);

    fs::remove(tex, ec);
    fs::remove(importSidecarPath(tex), ec);
    fs::remove(plain, ec);
}

// A clip's sidecar travels with the clip, with the same hierarchy (or
// assets/_external/N if the clip is outside the project: Review Focus 7).
static void test_audio_sidecar_travels_with_the_clip(const fs::path& root)
{
    std::error_code ec;
    AudioImportSettings s;
    s.gainDb = -3.0f;
    std::string err;

    const fs::path inside = root / "assets" / "step.wav";
    CHECK(saveAudioImportSettings(inside, s, &err));
    const fs::path plain = root / "assets" / "chars" / "plain.wav";
    std::ofstream(plain) << "wav";                                   // no sidecar

    // A clip outside the project, with a sidecar.
    const fs::path outside = fs::temp_directory_path(ec) / "dt_exporter_audio_outside" / "voz.wav";
    fs::create_directories(outside.parent_path(), ec);
    std::ofstream(outside) << "wav";
    CHECK(saveAudioImportSettings(outside, s, &err));

    Scene scene;
    auto* a = scene.addGameObject("con_sidecar");
    a->setAudioClip(std::make_shared<AudioClipComponent>(nullptr, inside.string(), -1, false, false));
    auto* b = scene.addGameObject("sin_sidecar");
    b->setAudioClip(std::make_shared<AudioClipComponent>(nullptr, plain.string(), -1, false, false));
    auto* c = scene.addGameObject("fuera");
    c->setAudioClip(std::make_shared<AudioClipComponent>(nullptr, outside.string(), -1, false, false));

    const std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    std::vector<std::string> pkg;
    for (const ExportAsset& x : assets) pkg.push_back(x.packagePath);
    auto has = [&](const std::string& p) { return std::find(pkg.begin(), pkg.end(), p) != pkg.end(); };

    CHECK(has("assets/step.wav"));
    CHECK(has("assets/step.wav.import.json"));
    CHECK(has("assets/chars/plain.wav"));
    CHECK(!has("assets/chars/plain.wav.import.json"));
    // The outside one: the sidecar lands in the SAME _external subfolder as its clip.
    std::string outClip, outSide;
    for (const std::string& p : pkg)
    {
        if (p.find("_external") == std::string::npos) continue;
        if (p.size() > 16 && p.substr(p.size() - 16) == ".wav.import.json") outSide = p;
        else if (p.size() > 4 && p.substr(p.size() - 4) == ".wav") outClip = p;
    }
    CHECK(!outClip.empty() && outSide == outClip + ".import.json");
    for (const ExportAsset& x : assets)
        if (x.packagePath.find(".import.json") != std::string::npos) CHECK(x.existsOnDisk);

    fs::remove(importSidecarPath(inside), ec);
    fs::remove(plain, ec);
    fs::remove_all(outside.parent_path(), ec);
}

// Review Focus 6: a model's sidecar travels with the FBX, and that of each external
// animation source with ITS file (each FBX uses its own sidecar).
static void test_model_sidecar_travels_with_the_fbx_and_its_animation_sources(const fs::path& root)
{
    std::error_code ec;
    // Own names: hero.fbx and company are from the fixture and other tests use them.
    const fs::path hero = root / "assets" / "dt_modelo_hero.fbx";
    const fs::path run  = root / "assets" / "dt_modelo_run.fbx";
    const fs::path idle = root / "assets" / "dt_modelo_idle.fbx";   // no sidecar
    const fs::path prop = root / "assets" / "dt_modelo_prop.fbx";   // static, no sidecar
    for (const fs::path& p : { hero, run, idle, prop })
        std::ofstream(p) << "fbx";

    ModelImportSettings s;
    s.scale = 0.01f;
    std::string err;
    CHECK(saveModelImportSettings(hero, s, &err));
    CHECK(saveModelImportSettings(run,  s, &err));

    Scene scene;
    auto* personaje = scene.addGameObject("personaje");
    auto skinned = std::make_shared<SkinnedMesh>();
    skinned->sourcePath = hero.string();
    skinned->animationSources.push_back({ hero.string(), true,  {} });
    skinned->animationSources.push_back({ run.string(),  false, {} });
    skinned->animationSources.push_back({ idle.string(), false, {} });
    personaje->setMesh(skinned);
    scene.addGameObject("prop")->setMesh(makeMesh(prop));

    const std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    std::vector<std::string> pkg;
    for (const ExportAsset& a : assets) pkg.push_back(a.packagePath);
    auto has = [&](const std::string& p) { return std::find(pkg.begin(), pkg.end(), p) != pkg.end(); };

    CHECK(has("assets/dt_modelo_hero.fbx"));
    CHECK(has("assets/dt_modelo_hero.fbx.import.json"));
    CHECK(has("assets/dt_modelo_run.fbx.import.json"));
    CHECK(has("assets/dt_modelo_idle.fbx"));
    CHECK(!has("assets/dt_modelo_idle.fbx.import.json"));
    CHECK(has("assets/dt_modelo_prop.fbx"));
    CHECK(!has("assets/dt_modelo_prop.fbx.import.json"));
    for (const ExportAsset& a : assets)
        if (a.packagePath.find(".import.json") != std::string::npos) CHECK(a.existsOnDisk);

    for (const fs::path& p : { hero, run, idle, prop })
    {
        fs::remove(importSidecarPath(p), ec);
        fs::remove(p, ec);
    }
}

static std::vector<std::string> packagePaths(const std::vector<ExportAsset>& assets)
{
    std::vector<std::string> pkg;
    for (const ExportAsset& a : assets) pkg.push_back(a.packagePath);
    return pkg;
}

static bool contains(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

static void writeGltfWithCompanions(const fs::path& dir)
{
    std::error_code ec;
    fs::create_directories(dir / "textures", ec);
    std::ofstream(dir / "tri.gltf") << R"({"asset":{"version":"2.0"},)"
        R"("buffers":[{"uri":"tri.bin","byteLength":60}],"images":[{"uri":"textures/rojo%20x.tga"}]})";
    std::ofstream(dir / "tri.bin") << "bin";
    std::ofstream(dir / "textures" / "rojo x.tga") << "tga";
}

// Inside the project: the hierarchy is preserved and the .bin and the texture travel.
static void test_gltf_inside_the_project_travels_with_its_companions(const fs::path& root)
{
    const fs::path dir = root / "assets" / "gl";
    writeGltfWithCompanions(dir);
    Scene scene;
    scene.addGameObject("g")->setMesh(makeMesh(dir / "tri.gltf", dir / "textures" / "rojo x.tga"));
    const std::vector<std::string> pkg = packagePaths(collectSceneAssets(scene, root, {}));
    CHECK(contains(pkg, "assets/gl/tri.gltf"));
    CHECK(contains(pkg, "assets/gl/tri.bin"));
    CHECK(contains(pkg, "assets/gl/textures/rojo x.tga"));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Review Focus 4: outside the project, the .bin and the subfolder texture end up
// in the SAME assets/_external/N as the .gltf, with their subfolder.
static void test_gltf_outside_the_project_keeps_its_companions_together(const fs::path& root)
{
    std::error_code ec;
    const fs::path outside = fs::temp_directory_path(ec) / "dt_exporter_gltf_outside";
    fs::remove_all(outside, ec);
    writeGltfWithCompanions(outside);
    Scene scene;
    scene.addGameObject("g")->setMesh(makeMesh(outside / "tri.gltf", outside / "textures" / "rojo x.tga"));
    const std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    std::string gltfPkg;
    for (const ExportAsset& a : assets)
        if (exportPathKey(a.sourcePath) == exportPathKey((outside / "tri.gltf").string())) gltfPkg = a.packagePath;
    CHECK(!gltfPkg.empty());
    const std::string base = fs::path(gltfPkg).parent_path().generic_string();
    const std::vector<std::string> pkg = packagePaths(assets);
    CHECK(contains(pkg, base + "/tri.bin"));
    CHECK(contains(pkg, base + "/textures/rojo x.tga"));
    CHECK(assets.size() == 3);                                  // the material texture is not duplicated
    fs::remove_all(outside, ec);
}

static void test_obj_travels_with_its_mtl(const fs::path& root)
{
    const fs::path dir = root / "assets" / "o";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream(dir / "cube.obj") << "mtllib cube.mtl\nv 0 0 0\n";
    std::ofstream(dir / "cube.mtl") << "newmtl m\n";
    Scene scene;
    scene.addGameObject("o")->setMesh(makeMesh(dir / "cube.obj"));
    const std::vector<std::string> pkg = packagePaths(collectSceneAssets(scene, root, {}));
    CHECK(contains(pkg, "assets/o/cube.obj"));
    CHECK(contains(pkg, "assets/o/cube.mtl"));
    fs::remove_all(dir, ec);
}

static std::string packagePathOf(const std::vector<ExportAsset>& assets, const fs::path& source)
{
    const std::string key = exportPathKey(source.string());
    for (const ExportAsset& a : assets)
        if (exportPathKey(a.sourcePath) == key) return a.packagePath;
    return {};
}

// Final review, Important 3a: the material texture hanging from the model folder
// is placed relative to the model in the package, like an associated file. The
// runtime derives it from there (game.scene does not store the base texture).
static void test_external_model_texture_in_subfolder_stays_with_the_model(const fs::path& root)
{
    std::error_code ec;
    const fs::path outside = fs::temp_directory_path(ec) / "dt_exporter_fbx_subtex";
    fs::remove_all(outside, ec);
    fs::create_directories(outside / "textures", ec);
    std::ofstream(outside / "prop.fbx") << "fbx";
    std::ofstream(outside / "textures" / "x.png") << "png";
    Scene scene;
    scene.addGameObject("p")->setMesh(makeMesh(outside / "prop.fbx", outside / "textures" / "x.png"));
    const std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    const std::string base = fs::path(packagePathOf(assets, outside / "prop.fbx")).parent_path().generic_string();
    CHECK(packagePathOf(assets, outside / "textures" / "x.png") == base + "/textures/x.png");
    fs::remove_all(outside, ec);
}

// Final review, Important 3b: even if another object walked BEFORE uses the same
// texture through its material, the placement is decided by the model that reads it.
static void test_model_companions_win_over_an_earlier_material_use(const fs::path& root)
{
    std::error_code ec;
    const fs::path a = fs::temp_directory_path(ec) / "dt_exporter_order_a";
    const fs::path b = fs::temp_directory_path(ec) / "dt_exporter_order_b";
    fs::remove_all(a, ec);
    fs::remove_all(b, ec);
    fs::create_directories(a, ec);
    std::ofstream(a / "other.fbx") << "fbx";
    writeGltfWithCompanions(b);                                  // b/textures/rojo x.tga
    Scene scene;
    scene.addGameObject("a")->setMesh(makeMesh(a / "other.fbx", b / "textures" / "rojo x.tga"));
    scene.addGameObject("b")->setMesh(makeMesh(b / "tri.gltf"));
    const std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    const std::string base = fs::path(packagePathOf(assets, b / "tri.gltf")).parent_path().generic_string();
    CHECK(packagePathOf(assets, b / "textures" / "rojo x.tga") == base + "/textures/rojo x.tga");
    fs::remove_all(a, ec);
    fs::remove_all(b, ec);
}

// Final review, Important 3c: the sidecar of an associated file goes NEXT TO its asset in
// the package, not where the general rule would put it.
static void test_companion_sidecar_travels_next_to_it(const fs::path& root)
{
    std::error_code ec;
    const fs::path outside = fs::temp_directory_path(ec) / "dt_exporter_companion_sidecar";
    fs::remove_all(outside, ec);
    writeGltfWithCompanions(outside);
    std::ofstream(importSidecarPath(outside / "textures" / "rojo x.tga")) << "{}";
    Scene scene;
    scene.addGameObject("g")->setMesh(makeMesh(outside / "tri.gltf"));
    const std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    const std::string tex = packagePathOf(assets, outside / "textures" / "rojo x.tga");
    CHECK(!tex.empty());
    CHECK(packagePathOf(assets, importSidecarPath(outside / "textures" / "rojo x.tga")) ==
          importSidecarPath(fs::path(tex)).generic_string());
    fs::remove_all(outside, ec);
}

int main()
{
    fs::path root = makeProjectFixture();

    test_audio_sidecar_travels_with_the_clip(root);
    test_texture_sidecar_travels_with_the_texture(root);
    test_model_sidecar_travels_with_the_fbx_and_its_animation_sources(root);
    test_gltf_inside_the_project_travels_with_its_companions(root);
    test_gltf_outside_the_project_keeps_its_companions_together(root);
    test_obj_travels_with_its_mtl(root);
    test_external_model_texture_in_subfolder_stays_with_the_model(root);
    test_model_companions_win_over_an_earlier_material_use(root);
    test_companion_sidecar_travels_next_to_it(root);
    test_collects_exactly_referenced(root);
    test_button_assets(root);
    test_procedural_mesh_contributes_nothing(root);
    test_deduplicates_shared_mesh(root);
    test_animation_sources(root);
    test_skinned_mesh_materials(root);
    test_external_assets(root);
    test_missing_asset_flagged(root);
    test_rewrite_makes_paths_relative(root);
    test_mat_asset_is_collected_and_rewritten(root);
    test_rewrite_resolves_paths_stored_relative_to_project(root);
    test_rewrite_leaves_unknown_paths(root);
    test_rewrite_materials_override_outside_root(root);
    test_package_contents(root);
    test_package_includes_splash(root);
    test_export_platform_rows();
    test_package_overwrite_is_clean(root);
    test_mat_textures_are_packaged_and_repointed(root);
    test_writeExportPackage_aborts_on_occupied(root);
    test_missing_runtime_aborts(root);
    test_inspect_export_target_states();
    test_valid_export_game_name();
    test_incomplete_skybox_marks_not_ok();
    test_zero_shaders_marks_not_ok();
    test_debug_build_warns_about_crt(root);
    test_release_package_bundles_msvc_crt();
    test_exportGame_aborts_without_camera(root);
    test_exportGame_aborts_missing_asset(root);
    test_linux_package(root);

    std::error_code ec;
    fs::remove_all(root, ec);

    if (g_failures) { std::printf("%d FAILURES\n", g_failures); return 1; }
    std::printf("OK\n");
    return 0;
}
