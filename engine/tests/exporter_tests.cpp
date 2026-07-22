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
#include "DonTopo/Core/CameraComponent.h"
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Scripting/ScriptComponent.h"

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

// Tras reescribir, ningún path del .scene puede seguir siendo absoluto: el
// paquete se ejecuta en otra máquina y otro directorio.
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
    int rewritten = rewriteScenePaths(j, sourceToPackage);

    // hero.fbx (mesh) + hero.fbx (animationSource builtin? no lo hay) +
    // run.fbx + step.wav = 3 campos reescritos.
    CHECK(rewritten == 3);

    const nlohmann::json& node = j["root"]["children"][0];
    CHECK(node["mesh"]["sourcePath"].get<std::string>() == "assets/hero.fbx");
    CHECK(node["mesh"]["animationSources"][0]["path"].get<std::string>() == "assets/run.fbx");
    CHECK(node["audioClip"]["path"].get<std::string>() == "assets/step.wav");

    // Ningún path absoluto residual (en Windows: sin ':' de unidad).
    CHECK(node["mesh"]["sourcePath"].get<std::string>().find(':') == std::string::npos);
    CHECK(node["audioClip"]["path"].get<std::string>().find(':') == std::string::npos);
}

// Un path que no está en el mapa se deja intacto, no se borra ni se vacía.
static void test_rewrite_leaves_unknown_paths(const fs::path& root)
{
    nlohmann::json j;
    j["version"] = 1;
    j["root"] = { { "name", "root" },
                  { "mesh", { { "sourcePath", "C:/otro/sitio/x.fbx" } } },
                  { "children", nlohmann::json::array() } };

    int rewritten = rewriteScenePaths(j, {});
    CHECK(rewritten == 0);
    CHECK(j["root"]["mesh"]["sourcePath"].get<std::string>() == "C:/otro/sitio/x.fbx");
}

// El paquete contiene el exe renombrado, game.scene, los assets del plan, el
// skybox, los shaders y Scripts/ — y ningún asset del proyecto que la escena
// no referencie (criterio de aceptación 3).
static void test_package_contents(const fs::path& root)
{
    std::error_code ec;

    // Completar el fixture con lo que writeExportPackage añade por su cuenta.
    fs::create_directories(root / "assets" / "skybox", ec);
    for (const char* face : { "px", "nx", "py", "ny", "pz", "nz" })
        std::ofstream(root / "assets" / "skybox" / (std::string(face) + ".png")) << "png";
    fs::create_directories(root / "shaders", ec);
    std::ofstream(root / "shaders" / "triangle.vert.spv") << "spv";
    // Asset del proyecto que la escena NO referencia: no debe acabar copiado.
    std::ofstream(root / "assets" / "huerfano.fbx") << "fbx";
    // Runtime de mentira.
    std::ofstream(root / "DonTopoRuntime.exe") << "MZ";

    Scene scene;
    scene.addGameObject("hero")->setMesh(makeMesh(root / "assets" / "hero.fbx"));
    std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});

    fs::path tempRoot = fs::temp_directory_path(ec);
    // Sin temp_directory_path no hay dónde escribir con seguridad: si ec
    // queda puesto o el path vuelve vacío, "dest" pasaría a ser relativo al
    // directorio de trabajo actual y el remove_all/writeExportPackage de
    // abajo operarían fuera del temp, rompiendo el contrato de que este test
    // solo toca fs::temp_directory_path().
    if (ec || tempRoot.empty())
    {
        CHECK(!ec && !tempRoot.empty());
        return;
    }
    fs::path dest = tempRoot / "dt_exporter_out";
    fs::remove_all(dest, ec);

    ExportResult r = writeExportPackage(assets, scene.toJson(), dest, "MiJuego",
                                        root, root / "Scripts", root / "DonTopoRuntime.exe");

    const fs::path pkg = dest / "MiJuego";
    CHECK(r.ok);
    CHECK(fs::exists(pkg / "MiJuego.exe"));
    CHECK(fs::exists(pkg / "game.scene"));
    CHECK(fs::exists(pkg / "assets" / "hero.fbx"));
    CHECK(fs::exists(pkg / "assets" / "skybox" / "px.png"));
    CHECK(fs::exists(pkg / "assets" / "skybox" / "nz.png"));
    CHECK(fs::exists(pkg / "shaders" / "triangle.vert.spv"));
    CHECK(fs::exists(pkg / "Scripts" / "Player.lua"));
    // Criterio 3: el asset no referenciado se queda fuera.
    CHECK(!fs::exists(pkg / "assets" / "huerfano.fbx"));

    // Número exacto de ficheros, no solo "> 0": un off-by-one o una
    // categoría contada de más no lo detectaría un CHECK laxo. Desglose para
    // esta escena/fixture concretos:
    //   1  MiJuego.exe            (runtimeExe renombrado)
    //   1  assets/hero.fbx        (único asset que la escena referencia)
    //   6  assets/skybox/*.png    (las 6 caras, hardcoded, van siempre)
    //   1  shaders/triangle.vert.spv (único .spv creado por este fixture)
    //   1  Scripts/Player.lua     (único fichero bajo Scripts/ en el fixture)
    //   0  fmod.dll               (este fixture no lo crea)
    //   1  game.scene
    //  = 11
    CHECK(r.fileCount == 11);
    CHECK(r.totalBytes > 0);

    // Hallazgo 1: totalBytes debe incluir también game.scene. Se comprueba
    // recalculando el tamaño real en disco de todo el paquete copiado y
    // exigiendo que coincida exactamente con lo reportado; sin sumar
    // game.scene, este CHECK fallaría por debajo en justo el tamaño de ese
    // fichero.
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

// Re-exportar sobre un paquete de un export anterior lo deja limpio: nada
// del export viejo sobrevive. La carpeta lleva game.scene, la única marca
// que inspectExportTarget acepta como "esto es mio y lo puedo regenerar"
// (GameExporter.cpp: inspectExportTarget) — sin ella este mismo fixture
// pasaria a ser Occupied y writeExportPackage abortaria sin borrar nada,
// que es justo el otro caso que cubre test_writeExportPackage_aborts_on_occupied.
static void test_package_overwrite_is_clean(const fs::path& root)
{
    std::error_code ec;
    fs::path tempRoot = fs::temp_directory_path(ec);
    // Mismo motivo que en test_package_contents: sin esta comprobación, un
    // fallo de temp_directory_path haría que "dest" cayera fuera del temp.
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

// Un directorio destino con contenido ajeno (sin game.scene) hace abortar a
// writeExportPackage SIN tocar nada: ni se borra lo que habia ni se crea el
// paquete. Es el caso que rompia antes de inspectExportTarget/Occupied — el
// ejemplo real es <repo>/assets, que remove_all se llevaba por delante
// (ver comentario de writeExportPackage sobre el estado del destino).
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
    // El contenido ajeno sigue exactamente donde estaba: ni un remove_all
    // parcial ni un create_directories encima lo tocaron.
    CHECK(fs::exists(dest / "MiJuego" / "algo_del_usuario.fbx"));

    fs::remove_all(dest, ec);
}

// inspectExportTarget en sus cuatro estados: la clasificacion mira QUE hay
// dentro del directorio, no DONDE esta, y falla en cerrado (Occupied) ante
// cualquier cosa que no encaje en un patron reconocido.
static void test_inspect_export_target_states()
{
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec) / "dt_exporter_inspect";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);

    // Missing: no existe en absoluto.
    CHECK(inspectExportTarget(base / "no_existe") == ExportTargetState::Missing);

    // Empty: existe y esta vacio.
    fs::path empty = base / "vacia";
    fs::create_directories(empty, ec);
    CHECK(inspectExportTarget(empty) == ExportTargetState::Empty);

    // PriorPackage: existe y contiene game.scene en su raiz.
    fs::path prior = base / "paquete_previo";
    fs::create_directories(prior, ec);
    std::ofstream(prior / "game.scene") << "{}";
    CHECK(inspectExportTarget(prior) == ExportTargetState::PriorPackage);

    // Occupied: existe con contenido que no es un game.scene.
    fs::path occupied = base / "ocupada";
    fs::create_directories(occupied, ec);
    std::ofstream(occupied / "documento_del_usuario.txt") << "no me borres";
    CHECK(inspectExportTarget(occupied) == ExportTargetState::Occupied);

    // Occupied tambien para un fichero (no un directorio) con ese nombre:
    // remove_all se lo llevaria igual, y no es un paquete nuestro.
    fs::path fileTarget = base / "esto_es_un_fichero.txt";
    std::ofstream(fileTarget) << "x";
    CHECK(inspectExportTarget(fileTarget) == ExportTargetState::Occupied);

    fs::remove_all(base, ec);
}

// isValidExportGameName: los casos que writeExportPackage necesita rechazar
// antes de construir destDir/name, porque ese path es el que luego borra.
static void test_valid_export_game_name()
{
    std::string reason;
    CHECK(!isValidExportGameName("..", reason));               // sube un nivel
    CHECK(!isValidExportGameName(".", reason));                 // Win32 lo descarta al crear la carpeta
    CHECK(!isValidExportGameName("C:\\Windows", reason));       // absoluto: operator/ ignoraria destDir
    CHECK(!isValidExportGameName("carpeta/nombre", reason));    // separador '/'
    CHECK(!isValidExportGameName("carpeta\\nombre", reason));   // separador '\'
    CHECK(!isValidExportGameName("nombre*raro", reason));       // caracter reservado de Windows
    CHECK(!isValidExportGameName("   ", reason));                // solo espacios en blanco
    CHECK(!isValidExportGameName("NUL", reason));                // nombre de dispositivo reservado
    CHECK(isValidExportGameName("MiJuegoValido", reason));
}

// Sin binario de runtime no se exporta nada: error explícito y carpeta sin crear.
static void test_missing_runtime_aborts(const fs::path& root)
{
    std::error_code ec;
    fs::path tempRoot = fs::temp_directory_path(ec);
    // Mismo motivo que en test_package_contents: sin esta comprobación, un
    // fallo de temp_directory_path haría que "dest" cayera fuera del temp.
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
    // Contrato: sin runtime no se llega a crear ni siquiera la carpeta
    // destino, no solo se aborta "a medias".
    CHECK(!fs::exists(dest / "MiJuego"));

    fs::remove_all(dest, ec);
}

// Skybox incompleto (falta una de las 6 caras) -> ok == false con mensaje:
// Skybox.cpp:84 lanza al arrancar si falta cualquiera, asi que un export
// "completado" con menos de 6 caras produce un paquete que no arranca.
static void test_incomplete_skybox_marks_not_ok()
{
    std::error_code ec;
    fs::path fixRoot = fs::temp_directory_path(ec) / "dt_exporter_skybox_fixture";
    fs::remove_all(fixRoot, ec);
    fs::create_directories(fixRoot / "assets" / "skybox", ec);
    fs::create_directories(fixRoot / "shaders", ec);
    // Solo 5 de las 6 caras: falta "nz".
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

// Cero shaders .spv copiados -> ok == false con mensaje: sin ninguno el
// runtime muere en createPipeline, y un Log que diga "completado" esconderia
// justo el fallo que hace inarrancable al paquete.
static void test_zero_shaders_marks_not_ok()
{
    std::error_code ec;
    fs::path fixRoot = fs::temp_directory_path(ec) / "dt_exporter_noshader_fixture";
    fs::remove_all(fixRoot, ec);
    fs::create_directories(fixRoot / "assets" / "skybox", ec);
    fs::create_directories(fixRoot / "shaders", ec); // existe pero vacia: 0 .spv
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

// exportGame aborta sin camara en la escena, antes de tocar disco: sin ella
// el juego no podria renderizar y el fallo debe ocurrir aqui, no en un .exe
// que abre una ventana negra.
static void test_exportGame_aborts_without_camera(const fs::path& root)
{
    std::error_code ec;
    if (!fs::exists(root / "DonTopoRuntime.exe", ec))
        std::ofstream(root / "DonTopoRuntime.exe") << "MZ";

    Scene scene; // sin CameraComponent en ningun GameObject
    scene.addGameObject("hero")->setMesh(makeMesh(root / "assets" / "hero.fbx"));

    fs::path dest = fs::temp_directory_path(ec) / "dt_exporter_nocam_out";
    fs::remove_all(dest, ec);

    ExportResult r = exportGame(scene, {}, dest, "MiJuego", root,
                                root / "Scripts", root / "DonTopoRuntime.exe");
    CHECK(!r.ok);
    CHECK(!r.messages.empty());
    CHECK(!fs::exists(dest / "MiJuego")); // aborta antes de crear nada

    fs::remove_all(dest, ec);
}

// exportGame aborta si algun asset referenciado no existe en disco. La
// escena SI tiene camara, para aislar exactamente esta guarda de la de
// arriba.
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
    test_rewrite_makes_paths_relative(root);
    test_rewrite_leaves_unknown_paths(root);
    test_package_contents(root);
    test_package_overwrite_is_clean(root);
    test_writeExportPackage_aborts_on_occupied(root);
    test_missing_runtime_aborts(root);
    test_inspect_export_target_states();
    test_valid_export_game_name();
    test_incomplete_skybox_marks_not_ok();
    test_zero_shaders_marks_not_ok();
    test_exportGame_aborts_without_camera(root);
    test_exportGame_aborts_missing_asset(root);

    std::error_code ec;
    fs::remove_all(root, ec);

    if (g_failures) { std::printf("%d FAILURES\n", g_failures); return 1; }
    std::printf("OK\n");
    return 0;
}
