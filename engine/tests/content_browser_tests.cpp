// Test headless de los helpers del Content Browser (sin GUI). Plain main +
// asserts, sin framework — mismo patrón que physics_tests.cpp.
#include "DonTopo/Editor/ContentBrowserPanel.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Editor/AssetImport.h"
#include "DonTopo/Editor/ProjectContext.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/ImportSettings.h"
#include "DonTopo/Core/MaterialAsset.h"
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Editor/ModelReimport.h"
#include "DonTopo/Renderer/SkinnedMeshAnimations.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <system_error>
#include <vector>

using namespace DonTopo;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Crea un árbol temporal con carpetas normales, ruido a filtrar y un fichero
// suelto. Devuelve la raíz.
static fs::path makeFixture()
{
    std::error_code ec;
    fs::path root = fs::temp_directory_path(ec) / "dt_content_browser_test";
    fs::remove_all(root, ec);
    fs::create_directories(root / "assets", ec);
    fs::create_directories(root / "Scripts", ec);
    fs::create_directories(root / ".hidden", ec);
    fs::create_directories(root / "build-ninja", ec);
    fs::create_directories(root / "cmake-build-debug", ec); // CLion
    // Nombre que ninguna lista contempla, pero con el CMakeCache dentro: es el
    // criterio que de verdad decide (x64-Debug lo genera Visual Studio).
    fs::create_directories(root / "x64-Debug", ec);
    std::ofstream(root / "x64-Debug" / "CMakeCache.txt") << "x";
    // Nombre que EMPIEZA por "build" pero no es un árbol de build: se ve.
    fs::create_directories(root / "build_assets", ec);
    // Nombre genérico de carpeta de build, pero SIN CMakeCache: aquí es una
    // carpeta de assets del usuario y tiene que verse (ver isHiddenDir).
    fs::create_directories(root / "out", ec);
    std::ofstream(root / "readme.txt") << "x";
    return root;
}

// Lista sólo subcarpetas: ni ficheros, ni ocultas, ni árboles de build.
static void test_filters_noise(const fs::path& root)
{
    std::vector<fs::path> dirs = listVisibleSubdirs(root);
    CHECK(dirs.size() == 4);
    if (dirs.size() == 4) {
        // std::sort sobre paths: 'S' (83) va antes que las minúsculas.
        CHECK(dirs[0].filename() == "Scripts");
        CHECK(dirs[1].filename() == "assets");
        // Ni el prefijo "build" ni el sufijo lo esconden: no tiene CMakeCache.
        CHECK(dirs[2].filename() == "build_assets");
        CHECK(dirs[3].filename() == "out");
    }
}

// Una carpeta con nombre de build pero sin CMakeCache dentro es del usuario y se
// ve. Fija la decisión de NO enumerar nombres genéricos: esconder "out" por el
// nombre le tapaba al usuario una carpeta de assets suya.
static void test_generic_name_without_cache_is_visible(const fs::path& root)
{
    std::vector<fs::path> dirs = listVisibleSubdirs(root);
    bool encontrada = false;
    for (const auto& d : dirs)
        if (d.filename() == "out") encontrada = true;
    CHECK(encontrada);
}

// El criterio que manda es el contenido, no el nombre: una carpeta de build con
// nombre que no está en ninguna lista se filtra igual por su CMakeCache.txt.
// Fija la diferencia con la versión anterior, que sólo conocía "build-ninja".
static void test_hides_build_tree_by_content(const fs::path& root)
{
    std::vector<fs::path> dirs = listVisibleSubdirs(root);
    for (const auto& d : dirs)
        CHECK(d.filename() != "x64-Debug");
}

// Las carpetas vacías salen como tal, no como error.
static void test_empty_dir(const fs::path& root)
{
    CHECK(listVisibleSubdirs(root / "assets").empty());
}

// Un directorio inexistente devuelve vacío sin lanzar.
static void test_missing_dir(const fs::path& root)
{
    CHECK(listVisibleSubdirs(root / "no_existe_esta_carpeta").empty());
}

// Un fichero (no directorio) devuelve vacío sin lanzar.
static void test_path_is_file(const fs::path& root)
{
    CHECK(listVisibleSubdirs(root / "readme.txt").empty());
}

// GameObject con SkinnedMesh (ModelLoader::loadSkinned) y un path de textura
// conocido fijado a mano en materials[0] en vez del que traiga el FBX: el
// objetivo de los tests siguientes es fijar el recorrido de materialsOf()
// (countSceneReferences/updateSceneReferencesForRename/
// detachSceneReferencesForDelete), no el contenido concreto del asset.
static std::unique_ptr<GameObject> makeSkinnedFixture(const std::string& texturePath)
{
    auto go = std::make_unique<GameObject>("Rigged");
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    CHECK(!mesh->materials.empty());
    if (!mesh->materials.empty())
        mesh->materials[0].texturePath = texturePath;
    go->setMesh(mesh);
    return go;
}

// loadSkinned nunca puebla el Mesh::material heredado (reparte materiales en
// SkinnedMesh::materials); mirar sólo material dejaba a este caso devolviendo
// 0 en silencio — éste es exactamente el "0 objetos afectados" que veía un
// personaje con rig en el diálogo destructivo de delete antes del fix.
static void test_count_references_finds_skinned_material_texture()
{
    const std::string knownPath = "assets/knownTexture.png";
    auto go = makeSkinnedFixture(knownPath);
    CHECK(countSceneReferences(go.get(), knownPath, /*isDir=*/false) == 1);
    CHECK(countSceneReferences(go.get(), std::string("assets/otraTextura.png"), false) == 0);
}

// El rename debe reescribir el path dentro de SkinnedMesh::materials, no sólo
// en el Mesh::material heredado (que loadSkinned deja vacío).
static void test_rename_rewrites_skinned_material_path()
{
    const std::string oldPath = "assets/knownTexture.png";
    const std::string newPath = "assets/renamedTexture.png";
    auto go = makeSkinnedFixture(oldPath);
    GameObject* selected = nullptr;
    bool isPlaying = false;
    EditorContext ctx{selected, isPlaying};
    updateSceneReferencesForRename(ctx, go.get(), oldPath, newPath, /*isDir=*/false);
    CHECK(go->getSkinnedMesh()->materials[0].texturePath == newPath);
}

// El detach debe limpiar el path dentro de SkinnedMesh::materials antes de
// borrar el fichero de disco, para que un re-register posterior no intente
// stbi_load sobre una ruta ya borrada.
static void test_detach_clears_skinned_material_path()
{
    const std::string knownPath = "assets/knownTexture.png";
    auto go = makeSkinnedFixture(knownPath);
    GameObject* selected = nullptr;
    bool isPlaying = false;
    EditorContext ctx{selected, isPlaying};
    detachSceneReferencesForDelete(ctx, go.get(), knownPath, /*isDir=*/false);
    CHECK(go->getSkinnedMesh()->materials[0].texturePath.empty());
}

// El rename tenía el mismo punto ciego con GameObject::materialOverrides que
// antes tenía con SkinnedMesh::materials: reescribía el Material EN MEMORIA
// pero no el override, que es lo que de verdad sobrevive a un guardado
// (mesh.materials, Task 6). Sin esto, el override serializado seguía
// apuntando al nombre viejo y el siguiente Load no encontraba la textura —
// pérdida de datos silenciosa. Los tres slots a la vez: la guarda tiene que
// cubrir albedo, normal y orm, no solo el primero que se pruebe.
static void test_rename_rewrites_material_override_path()
{
    const std::string oldPath = "assets/knownTexture.png";
    const std::string newPath = "assets/renamedTexture.png";
    auto go = makeSkinnedFixture(oldPath);
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = oldPath;
    ov.normal = oldPath;
    ov.orm    = oldPath;
    go->materialOverrides.push_back(ov);

    GameObject* selected = nullptr;
    bool isPlaying = false;
    EditorContext ctx{selected, isPlaying};
    updateSceneReferencesForRename(ctx, go.get(), oldPath, newPath, /*isDir=*/false);

    CHECK(go->materialOverrides[0].albedo == newPath);
    CHECK(go->materialOverrides[0].normal == newPath);
    CHECK(go->materialOverrides[0].orm    == newPath);
}

// Mismo caso que arriba pero para borrar: el override que apunte al fichero
// que se borra tiene que vaciarse igual que el Material, o el siguiente Save
// reescribe la ruta muerta y el siguiente Load la reaplica sobre el material
// recién derivado del FBX.
static void test_detach_clears_material_override_path()
{
    const std::string knownPath = "assets/knownTexture.png";
    auto go = makeSkinnedFixture(knownPath);
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = knownPath;
    ov.normal = knownPath;
    ov.orm    = knownPath;
    go->materialOverrides.push_back(ov);

    GameObject* selected = nullptr;
    bool isPlaying = false;
    EditorContext ctx{selected, isPlaying};
    detachSceneReferencesForDelete(ctx, go.get(), knownPath, /*isDir=*/false);

    CHECK(go->materialOverrides[0].albedo.empty());
    CHECK(go->materialOverrides[0].normal.empty());
    CHECK(go->materialOverrides[0].orm.empty());
}

static std::unique_ptr<GameObject> makeMatAssetFixture(const std::string& matPath)
{
    auto go = std::make_unique<GameObject>("ConMaterial");
    go->setMesh(std::make_shared<Mesh>());
    MaterialOverride ov; ov.index = 0; ov.matAsset = matPath;
    go->materialOverrides = {ov};
    return go;
}

// Review Focus 4.
static void test_count_references_finds_mat_asset()
{
    const std::string knownPath = "assets/rojo.mat";
    auto go = makeMatAssetFixture(knownPath);
    CHECK(countSceneReferences(go.get(), knownPath, /*isDir=*/false) == 1);
    CHECK(countSceneReferences(go.get(), std::string("assets/otro.mat"), false) == 0);
}

static void test_rename_rewrites_mat_asset_path()
{
    const std::string oldPath = "assets/rojo.mat";
    const std::string newPath = "assets/carmesi.mat";
    auto go = makeMatAssetFixture(oldPath);
    GameObject* selected = nullptr;
    bool isPlaying = false;
    EditorContext ctx{selected, isPlaying};
    updateSceneReferencesForRename(ctx, go.get(), oldPath, newPath, /*isDir=*/false);
    CHECK(go->materialOverrides[0].matAsset == newPath);
}

// Un objeto que NO usa el .mat renombrado no se toca.
static void test_rename_leaves_other_mat_asset_untouched()
{
    auto go = makeMatAssetFixture("assets/otro.mat");
    GameObject* selected = nullptr;
    bool isPlaying = false;
    EditorContext ctx{selected, isPlaying};
    updateSceneReferencesForRename(ctx, go.get(), "assets/rojo.mat", "assets/carmesi.mat", false);
    CHECK(go->materialOverrides[0].matAsset == "assets/otro.mat");
}

// Review Focus 5: borrar el .mat en uso vacia matAsset Y el material efectivo
// vuelve al del modelo (no se queda con la ultima textura que trajo el .mat).
// Esto exige applyMaterialOverrides + rebuild dentro de detachSceneReferencesForDelete,
// no solo vaciar el campo: hallazgo del reviewer final, el test anterior solo
// comprobaba matAsset.empty() y no detectaba que el material en memoria se
// quedaba con la textura del .mat borrado hasta la siguiente reaplicacion.
static void test_detach_clears_mat_asset_and_falls_back_to_model()
{
    std::error_code ec;
    std::filesystem::path d = std::filesystem::temp_directory_path(ec) / "dt_matasset_detach_fallback";
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d, ec);
    const std::string matPath = (d / "rojo.mat").string();

    MaterialAsset asset; asset.albedo = (d / "rojo.png").string();
    std::string err;
    CHECK(saveMaterialAsset(matPath, asset, &err));

    auto go = makeMatAssetFixture(matPath);
    go->editMesh()->material.texturePath = "assets/model_default.png";   // baseline que trae el FBX
    applyMaterialOverrides(*go);   // como en la app real: el .mat ya resolvio el slot
    CHECK(go->getMesh()->material.texturePath == asset.albedo);

    GameObject* selected = nullptr;
    bool isPlaying = false;
    EditorContext ctx{selected, isPlaying};
    detachSceneReferencesForDelete(ctx, go.get(), matPath, /*isDir=*/false);

    CHECK(go->materialOverrides[0].matAsset.empty());
    CHECK(go->getMesh()->material.texturePath == "assets/model_default.png");
}

// El baseline (base*) apuntando al fichero que se acaba de borrar no era un
// detalle cosmético: con el override vacío y el flag *Taken en alto,
// applyMaterialOverrides toma la rama "vuelve al baseline" y REESCRIBE esa
// ruta muerta en el Material. Y el siguiente applyMaterialOverrides puede ser
// el de editar cualquier OTRO slot, así que deshacía en silencio el
// replaceStaticTextureWithMissing que el borrado acababa de hacer.
//
// El flag se queda en alto a propósito (se comprueba abajo): es lo que hace
// que la reaplicación deje el slot VACÍO en vez de resucitar el fichero
// borrado.
static void test_detach_clears_material_override_baseline()
{
    const std::string knownPath = "assets/knownTexture.png";
    auto go = makeSkinnedFixture(knownPath);
    MaterialOverride ov;
    ov.index           = 0;
    ov.baseAlbedo      = knownPath;   // el usuario ya hizo Clear: override vacío,
    ov.baseAlbedoTaken = true;        // baseline tomado y apuntando al fichero
    ov.baseNormal      = knownPath;
    ov.baseNormalTaken = true;
    ov.baseOrm         = knownPath;
    ov.baseOrmTaken    = true;
    go->materialOverrides.push_back(ov);

    GameObject* selected = nullptr;
    bool isPlaying = false;
    EditorContext ctx{selected, isPlaying};
    detachSceneReferencesForDelete(ctx, go.get(), knownPath, /*isDir=*/false);

    CHECK(go->materialOverrides[0].baseAlbedo.empty());
    CHECK(go->materialOverrides[0].baseNormal.empty());
    CHECK(go->materialOverrides[0].baseOrm.empty());
    CHECK(go->materialOverrides[0].baseAlbedoTaken);

    // La prueba de que importaba: reaplicar ahora NO devuelve el fichero
    // borrado al material. Antes del fix, esta línea lo resucitaba.
    applyMaterialOverrides(*go);
    CHECK(go->getSkinnedMesh()->materials[0].texturePath.empty());
}

// La cara del renombrado: ahí el asset SIGUE existiendo con otro nombre, así
// que el baseline se corrige en vez de tirarse. Sin esto, un Clear posterior
// devolvía al material el nombre viejo, que ya no existe en disco.
static void test_rename_rewrites_material_override_baseline()
{
    const std::string oldPath = "assets/knownTexture.png";
    const std::string newPath = "assets/renamedTexture.png";
    auto go = makeSkinnedFixture(oldPath);
    MaterialOverride ov;
    ov.index           = 0;
    ov.albedo          = "assets/override.png";
    ov.baseAlbedo      = oldPath;
    ov.baseAlbedoTaken = true;
    ov.baseNormal      = oldPath;
    ov.baseNormalTaken = true;
    ov.baseOrm         = oldPath;
    ov.baseOrmTaken    = true;
    go->materialOverrides.push_back(ov);

    GameObject* selected = nullptr;
    bool isPlaying = false;
    EditorContext ctx{selected, isPlaying};
    updateSceneReferencesForRename(ctx, go.get(), oldPath, newPath, /*isDir=*/false);

    CHECK(go->materialOverrides[0].baseAlbedo == newPath);
    CHECK(go->materialOverrides[0].baseNormal == newPath);
    CHECK(go->materialOverrides[0].baseOrm    == newPath);
}

// importDroppedFilesInto: dentro del rect se importa, fuera se ignora en
// silencio, extension no importable se rechaza, y un conflicto de nombre no
// aborta el resto del lote (las cuatro reglas del diseño de drop externo).
static void test_import_dropped_files_into(const fs::path& root)
{
    std::error_code ec;
    fs::path externalDir = root / "external_src";
    fs::create_directories(externalDir, ec);
    fs::path destDir = root / "import_dest";
    fs::create_directories(destDir, ec);

    std::ofstream(externalDir / "nuevo.png")     << "nuevo";
    std::ofstream(externalDir / "conflicto.png") << "version-nueva";
    std::ofstream(destDir     / "conflicto.png") << "version-vieja"; // ya existe
    std::ofstream(externalDir / "notas.txt")     << "no importable";
    std::ofstream(externalDir / "lejos.png")     << "fuera del rect";

    const float rectX = 0.0f, rectY = 0.0f, rectW = 100.0f, rectH = 100.0f;
    std::vector<DroppedFile> dropped = {
        { externalDir / "nuevo.png",     50.0f, 50.0f },   // dentro
        { externalDir / "conflicto.png", 50.0f, 50.0f },   // dentro, conflicto
        { externalDir / "notas.txt",     50.0f, 50.0f },   // dentro, no importable
        { externalDir / "lejos.png",     500.0f, 500.0f }, // fuera del rect
    };

    std::vector<AssetImportOutcome> outcomes =
        importDroppedFilesInto(dropped, rectX, rectY, rectW, rectH, destDir);

    // "lejos.png" ni siquiera genera una entrada: cayo fuera del rect.
    CHECK(outcomes.size() == 3);
    if (outcomes.size() == 3)
    {
        CHECK(outcomes[0].result == AssetImportResult::Copied);
        CHECK(outcomes[0].destPath == destDir / "nuevo.png");
        CHECK(outcomes[1].result == AssetImportResult::RejectedNameConflict);
        CHECK(outcomes[2].result == AssetImportResult::RejectedExtension);
    }
    CHECK(fs::exists(destDir / "nuevo.png"));
    CHECK(!fs::exists(destDir / "lejos.png"));
    std::ifstream in(destDir / "conflicto.png");
    std::stringstream ss; ss << in.rdbuf();
    CHECK(ss.str() == "version-vieja"); // el conflicto no lo toco
}

// classifyAsset: una sola clasificacion para el icono del grid y para el filtro
// por tipo. Mayusculas da igual; una carpeta es Folder aunque se llame "a.png".
static void test_classify_asset()
{
    CHECK(classifyAsset(".fbx",  false) == AssetKind::Model3D);
    CHECK(classifyAsset(".GLB",  false) == AssetKind::Model3D);
    CHECK(classifyAsset(".wav",  false) == AssetKind::Audio);
    CHECK(classifyAsset(".png",  false) == AssetKind::Image);
    CHECK(classifyAsset(".bmp",  false) == AssetKind::Image);
    CHECK(classifyAsset(".ttf",  false) == AssetKind::Font);
    CHECK(classifyAsset(".json", false) == AssetKind::Scene);
    CHECK(classifyAsset(".lua",  false) == AssetKind::Script);
    CHECK(classifyAsset(".spv",  false) == AssetKind::Shader);
    CHECK(classifyAsset(".xyz",  false) == AssetKind::Other);
    CHECK(classifyAsset("",      false) == AssetKind::Other);
    CHECK(classifyAsset(".png",  true)  == AssetKind::Folder);
}

// assetMatchesFilter: texto = subcadena sin distinguir mayusculas; tipo = igualdad
// exacta; los dos se combinan con AND; "sin filtro" deja pasar todo.
static void test_asset_matches_filter()
{
    CHECK(assetMatchesFilter("Hero.fbx", AssetKind::Model3D, "", std::nullopt));
    CHECK(assetMatchesFilter("Hero.fbx", AssetKind::Model3D, "hero", std::nullopt));
    CHECK(assetMatchesFilter("Hero.fbx", AssetKind::Model3D, "RO.F", std::nullopt));
    CHECK(!assetMatchesFilter("Hero.fbx", AssetKind::Model3D, "villain", std::nullopt));
    CHECK(assetMatchesFilter("Hero.fbx", AssetKind::Model3D, "", AssetKind::Model3D));
    CHECK(!assetMatchesFilter("Hero.fbx", AssetKind::Model3D, "", AssetKind::Audio));
    // Con un tipo elegido, las carpetas se ocultan (salvo que el tipo sea Folder).
    CHECK(!assetMatchesFilter("Models", AssetKind::Folder, "", AssetKind::Model3D));
    CHECK(assetMatchesFilter("Models", AssetKind::Folder, "", AssetKind::Folder));
    // AND: el tipo coincide pero el texto no.
    CHECK(!assetMatchesFilter("Hero.fbx", AssetKind::Model3D, "villain", AssetKind::Model3D));
}

// uniqueFolderName: "Nueva carpeta", y si existe "Nueva carpeta 2", "3"... sin
// reutilizar el primer hueco de forma ambigua.
static void test_unique_folder_name(const fs::path& root)
{
    std::error_code ec;
    fs::path dir = root / "unique_name_dir";
    fs::create_directories(dir, ec);

    CHECK(uniqueFolderName(dir) == "Nueva carpeta");
    fs::create_directories(dir / "Nueva carpeta", ec);
    CHECK(uniqueFolderName(dir) == "Nueva carpeta 2");
    fs::create_directories(dir / "Nueva carpeta 2", ec);
    CHECK(uniqueFolderName(dir) == "Nueva carpeta 3");
    // Un FICHERO con ese nombre tambien ocupa el sitio.
    std::ofstream(dir / "Nueva carpeta 3") << "x";
    CHECK(uniqueFolderName(dir) == "Nueva carpeta 4");
}

// breadcrumbSegments: de la raiz del proyecto a la carpeta actual, con rutas
// acumulativas. Fuera de la raiz (o la propia raiz) solo queda el tramo raiz.
static void test_breadcrumb_segments(const fs::path& root)
{
    std::error_code ec;
    fs::path deep = root / "bc_assets" / "Imported";
    fs::create_directories(deep, ec);

    std::vector<BreadcrumbSegment> atRoot = breadcrumbSegments(root, root);
    CHECK(atRoot.size() == 1);
    if (atRoot.size() == 1)
    {
        CHECK(atRoot[0].name == root.filename().string());
        CHECK(atRoot[0].path == root);
    }

    std::vector<BreadcrumbSegment> nested = breadcrumbSegments(root, deep);
    CHECK(nested.size() == 3);
    if (nested.size() == 3)
    {
        CHECK(nested[0].path == root);
        CHECK(nested[1].name == "bc_assets");
        CHECK(nested[1].path == root / "bc_assets");
        CHECK(nested[2].name == "Imported");
        CHECK(nested[2].path == deep);
    }

    // Una carpeta hermana de la raiz no cuelga de ella: solo el tramo raiz.
    std::vector<BreadcrumbSegment> outside =
        breadcrumbSegments(root, root.parent_path() / "otra_carpeta_ajena");
    CHECK(outside.size() == 1);
}

static std::string readAll(const fs::path& p)
{
    std::ifstream in(p);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}

// moveAsset: mueve un fichero o carpeta a otra carpeta SIN sobreescribir nunca.
static void test_move_asset(const fs::path& root)
{
    std::error_code ec;
    fs::path base = root / "mv";
    fs::path dest = base / "dest";
    fs::create_directories(dest, ec);

    // Fichero a otra carpeta.
    std::ofstream(base / "a.png") << "contenido-a";
    MoveOutcome m1 = moveAsset(base / "a.png", dest);
    CHECK(m1.result == MoveResult::Moved);
    CHECK(m1.newPath == dest / "a.png");
    CHECK(!fs::exists(base / "a.png", ec));
    CHECK(readAll(dest / "a.png") == "contenido-a");

    // Carpeta con contenido.
    fs::create_directories(base / "folderX" / "sub", ec);
    std::ofstream(base / "folderX" / "inner.txt") << "dentro";
    MoveOutcome m2 = moveAsset(base / "folderX", dest);
    CHECK(m2.result == MoveResult::Moved);
    CHECK(m2.newPath == dest / "folderX");
    CHECK(readAll(dest / "folderX" / "inner.txt") == "dentro");
    CHECK(fs::exists(dest / "folderX" / "sub", ec));
    CHECK(!fs::exists(base / "folderX", ec));

    // Ya esta en esa carpeta: no hay nada que mover.
    MoveOutcome m3 = moveAsset(dest / "a.png", dest);
    CHECK(m3.result == MoveResult::RejectedSameFolder);
    CHECK(fs::exists(dest / "a.png", ec));

    // Una carpeta no puede entrar en si misma ni en un descendiente.
    MoveOutcome m4 = moveAsset(dest / "folderX", dest / "folderX");
    CHECK(m4.result == MoveResult::RejectedIntoSelf);
    MoveOutcome m5 = moveAsset(dest / "folderX", dest / "folderX" / "sub");
    CHECK(m5.result == MoveResult::RejectedIntoSelf);
    CHECK(fs::exists(dest / "folderX" / "inner.txt", ec));

    // Conflicto de nombre: rechaza y no toca ni el destino ni el origen.
    fs::create_directories(base / "otra", ec);
    std::ofstream(base / "otra" / "a.png") << "version-nueva";
    MoveOutcome m6 = moveAsset(base / "otra" / "a.png", dest);
    CHECK(m6.result == MoveResult::RejectedNameConflict);
    CHECK(readAll(dest / "a.png") == "contenido-a");
    CHECK(readAll(base / "otra" / "a.png") == "version-nueva");

    // Origen inexistente.
    MoveOutcome m7 = moveAsset(base / "no_existe.png", dest);
    CHECK(m7.result == MoveResult::RejectedFailed);
    CHECK(!m7.errorMessage.empty());
}

static bool selectionIs(const AssetSelection& s, std::initializer_list<const char*> names)
{
    if (s.items.size() != names.size()) return false;
    size_t i = 0;
    for (const char* n : names)
        if (s.items[i++] != fs::path(n)) return false;
    return true;
}

// applyAssetClick: clic = solo ese; Ctrl = alterna; Shift = rango desde el ancla
// en el orden VISIBLE. Los seleccionados quedan siempre en orden visible.
static void test_apply_asset_click()
{
    const std::vector<fs::path> vis = { "a", "b", "c", "d", "e" };
    AssetSelection s;

    applyAssetClick(s, vis, "b", false, false);
    CHECK(selectionIs(s, {"b"}));
    CHECK(s.anchor && *s.anchor == fs::path("b"));

    applyAssetClick(s, vis, "d", true, false);          // Ctrl anade
    CHECK(selectionIs(s, {"b", "d"}));
    applyAssetClick(s, vis, "a", true, false);          // orden visible, no de clic
    CHECK(selectionIs(s, {"a", "b", "d"}));
    applyAssetClick(s, vis, "b", true, false);          // Ctrl sobre uno ya marcado lo quita
    CHECK(selectionIs(s, {"a", "d"}));
    CHECK(s.anchor && *s.anchor == fs::path("b"));      // el ancla es el ultimo clic

    applyAssetClick(s, vis, "c", false, false);         // clic simple reemplaza todo
    CHECK(selectionIs(s, {"c"}));
    applyAssetClick(s, vis, "e", false, true);          // Shift: rango c..e
    CHECK(selectionIs(s, {"c", "d", "e"}));
    CHECK(s.anchor && *s.anchor == fs::path("c"));      // Shift no mueve el ancla
    applyAssetClick(s, vis, "a", false, true);          // rango hacia atras: a..c
    CHECK(selectionIs(s, {"a", "b", "c"}));

    // Sin ancla, Shift se comporta como un clic normal.
    AssetSelection empty;
    applyAssetClick(empty, vis, "d", false, true);
    CHECK(selectionIs(empty, {"d"}));

    // Ancla que ya no esta visible (un filtro la oculto): tambien clic normal.
    AssetSelection stale;
    stale.items  = { "z" };
    stale.anchor = fs::path("z");
    applyAssetClick(stale, vis, "c", false, true);
    CHECK(selectionIs(stale, {"c"}));
}

// pruneSelection: quita lo que ya no existe; si el ancla desaparece, se olvida.
static void test_prune_selection()
{
    AssetSelection s;
    s.items  = { "a", "b", "c" };
    s.anchor = fs::path("b");
    pruneSelection(s, { "a", "c", "d" });
    CHECK(selectionIs(s, {"a", "c"}));
    CHECK(!s.anchor);
    CHECK(s.contains("a"));
    CHECK(!s.contains("b"));
}

// listVisibleEntries: ficheros y carpetas visibles de UNA carpeta, ordenados; las
// carpetas ocultas / de build quedan fuera igual que en el arbol. Es lo que el
// polling compara entre pasadas para saber si algo cambio por fuera del editor.
static void test_list_visible_entries(const fs::path& root)
{
    std::error_code ec;
    fs::path dir = root / "lve";
    fs::create_directories(dir / "sub", ec);
    fs::create_directories(dir / ".hidden", ec);
    fs::create_directories(dir / "build-ninja", ec);
    std::ofstream(dir / "b.txt") << "x";
    std::ofstream(dir / "a.txt") << "x";

    std::vector<fs::path> before = listVisibleEntries(dir);
    CHECK(before.size() == 3);
    if (before.size() == 3)
    {
        CHECK(before[0].filename() == "a.txt");
        CHECK(before[1].filename() == "b.txt");
        CHECK(before[2].filename() == "sub");
    }

    // Alguien crea un fichero por fuera: la siguiente lectura ya lo trae.
    std::ofstream(dir / "c.txt") << "x";
    std::vector<fs::path> afterCreate = listVisibleEntries(dir);
    CHECK(afterCreate.size() == 4);
    CHECK(afterCreate != before);

    // ...y otro lo borra.
    fs::remove(dir / "a.txt", ec);
    std::vector<fs::path> afterRemove = listVisibleEntries(dir);
    CHECK(afterRemove.size() == 3);
    CHECK(afterRemove != afterCreate);

    // Sin cambios, dos lecturas iguales (es lo que evita refrescar en falso).
    CHECK(listVisibleEntries(dir) == afterRemove);

    // Carpeta inexistente o un fichero: vacio, sin lanzar.
    CHECK(listVisibleEntries(dir / "no_existe").empty());
    CHECK(listVisibleEntries(dir / "b.txt").empty());
}

// nearestExistingDir: si la carpeta actual desaparece por fuera, el panel sube al
// ancestro existente mas cercano sin salirse de la raiz del proyecto.
static void test_nearest_existing_dir(const fs::path& root)
{
    std::error_code ec;
    fs::path a = root / "ned" / "a";
    fs::create_directories(a, ec);

    CHECK(nearestExistingDir(a, root) == a);                         // existe: ella misma
    CHECK(nearestExistingDir(a / "b" / "c", root) == a);             // faltan dos niveles
    CHECK(nearestExistingDir(root / "ned" / "x", root) == root / "ned");
    CHECK(nearestExistingDir(root / "zzz" / "y", root) == root);     // falta todo hasta la raiz
    CHECK(nearestExistingDir(root, root) == root);
    CHECK(nearestExistingDir(root.parent_path() / "ajena", root) == root); // fuera de la raiz
}

static TextureImportSettings mipsOn()
{
    TextureImportSettings s;
    s.mipmaps = true;
    return s;
}

static void test_list_hides_import_sidecars()
{
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "dt_cb_sidecar_list";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    std::ofstream(dir / "foto.png") << "x";
    std::ofstream(dir / "escena.json") << "{}";
    std::string err;
    CHECK(saveTextureImportSettings(dir / "foto.png", mipsOn(), &err));
    CHECK(fs::exists(importSidecarPath(dir / "foto.png")));

    const std::vector<fs::path> entries = listVisibleEntries(dir);
    CHECK(entries.size() == 2);                       // foto.png y escena.json, NO el sidecar
    for (const fs::path& p : entries)
        CHECK(!isImportSidecar(p));
}

static void test_move_asset_carries_sidecar()
{
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "dt_cb_sidecar_move";
    fs::remove_all(base, ec);
    fs::create_directories(base / "A", ec);
    fs::create_directories(base / "B", ec);
    std::ofstream(base / "A" / "foto.png") << "x";
    std::string err;
    CHECK(saveTextureImportSettings(base / "A" / "foto.png", mipsOn(), &err));

    const MoveOutcome m = moveAsset(base / "A" / "foto.png", base / "B");
    CHECK(m.result == MoveResult::Moved);
    CHECK(fs::exists(base / "B" / "foto.png"));
    CHECK(loadTextureImportSettings(base / "B" / "foto.png") == mipsOn());
    CHECK(!fs::exists(importSidecarPath(base / "A" / "foto.png")));
}

// Hallazgo del reviewer final: las rutas de textura de un .mat se guardan
// RELATIVAS A SU PROPIA CARPETA (MaterialAsset.cpp). Mover el .mat sin
// reescribirlas las deja resueltas contra la carpeta nueva, que no es donde
// esta la textura.
static void test_move_asset_rewrites_mat_relative_texture()
{
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "dt_cb_mat_move";
    fs::remove_all(base, ec);
    fs::create_directories(base / "A", ec);
    fs::create_directories(base / "A" / "Metal", ec);
    fs::create_directories(base / "B", ec);

    const fs::path texture = base / "A" / "rojo.png";
    std::ofstream(texture) << "x";
    MaterialAsset asset; asset.albedo = texture.string();   // absoluta en memoria
    std::string err;
    CHECK(saveMaterialAsset(base / "A" / "x.mat", asset, &err));

    // La textura queda FUERA de la carpeta destino: mover el .mat a B tiene
    // que seguir apuntando a A/rojo.png, no a B/rojo.png.
    const MoveOutcome m = moveAsset(base / "A" / "x.mat", base / "B");
    CHECK(m.result == MoveResult::Moved);
    CHECK(fs::exists(base / "B" / "x.mat"));
    const MaterialAsset reloaded = loadMaterialAsset(base / "B" / "x.mat");
    CHECK(fs::equivalent(reloaded.albedo, texture, ec));
}

// Review Focus 4.
static void test_move_asset_rejects_when_destination_sidecar_exists()
{
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "dt_cb_sidecar_move_conflict";
    fs::remove_all(base, ec);
    fs::create_directories(base / "A", ec);
    fs::create_directories(base / "B", ec);
    std::ofstream(base / "A" / "foto.png") << "x";
    std::string err;
    CHECK(saveTextureImportSettings(base / "A" / "foto.png", mipsOn(), &err));
    // Sidecar HUERFANO en el destino: la foto no existe alli, su sidecar si.
    TextureImportSettings other;
    other.colorSpace = ColorSpaceOverride::Linear;
    CHECK(saveTextureImportSettings(base / "B" / "foto.png", other, &err));

    const MoveOutcome m = moveAsset(base / "A" / "foto.png", base / "B");
    CHECK(m.result == MoveResult::RejectedNameConflict);
    CHECK(fs::exists(base / "A" / "foto.png"));
    CHECK(loadTextureImportSettings(base / "A" / "foto.png") == mipsOn());     // origen intacto
    CHECK(loadTextureImportSettings(base / "B" / "foto.png") == other);        // destino sin pisar
    CHECK(!fs::exists(base / "B" / "foto.png"));
}

static void test_rename_asset_file_carries_sidecar_and_rejects_conflict()
{
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "dt_cb_sidecar_rename";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    std::ofstream(base / "x.png") << "x";
    std::string err;
    CHECK(saveTextureImportSettings(base / "x.png", mipsOn(), &err));

    RenameFileOutcome r = renameAssetFile(base / "x.png", base / "y.png", false);
    CHECK(r.ok);
    CHECK(r.warning.empty());
    CHECK(fs::exists(base / "y.png"));
    CHECK(loadTextureImportSettings(base / "y.png") == mipsOn());
    CHECK(!fs::exists(importSidecarPath(base / "x.png")));

    // El destino ya tiene un sidecar (huerfano): se rechaza sin tocar nada.
    std::ofstream(base / "z.png") << "z";
    CHECK(saveTextureImportSettings(base / "z.png", mipsOn(), &err));
    fs::remove(base / "z.png", ec);                                  // queda solo el sidecar
    r = renameAssetFile(base / "y.png", base / "z.png", false);
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    CHECK(fs::exists(base / "y.png"));
    CHECK(loadTextureImportSettings(base / "y.png") == mipsOn());
    CHECK(!fs::exists(base / "z.png"));

    // Una carpeta se renombra sin mirar sidecars.
    fs::create_directories(base / "dirA", ec);
    r = renameAssetFile(base / "dirA", base / "dirB", true);
    CHECK(r.ok);
    CHECK(fs::is_directory(base / "dirB"));
}

// Fix del review final: en un sistema de ficheros que no distingue mayusculas
// (NTFS), renombrar "foto.png" a "Foto.png" tiene el MISMO sidecar en origen y
// destino: eso no es un conflicto, es el mismo fichero.
static void test_rename_asset_file_case_only_is_not_a_sidecar_conflict()
{
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "dt_cb_sidecar_rename_case";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    std::ofstream(base / "foto.png") << "x";
    std::string err;
    CHECK(saveTextureImportSettings(base / "foto.png", mipsOn(), &err));

    const RenameFileOutcome r = renameAssetFile(base / "foto.png", base / "Foto.png", false);
    CHECK(r.ok);
    CHECK(r.error.empty());
    CHECK(loadTextureImportSettings(base / "Foto.png") == mipsOn());   // los ajustes siguen ahi
}

// Review Focus 5.
static void test_remove_asset_path_removes_sidecar()
{
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "dt_cb_sidecar_remove";
    fs::remove_all(base, ec);
    fs::create_directories(base / "carpeta" / "sub", ec);
    std::ofstream(base / "foto.png") << "x";
    std::ofstream(base / "carpeta" / "sub" / "t.png") << "x";
    std::string err;
    CHECK(saveTextureImportSettings(base / "foto.png", mipsOn(), &err));
    CHECK(saveTextureImportSettings(base / "carpeta" / "sub" / "t.png", mipsOn(), &err));

    CHECK(!removeAssetPath(base / "foto.png", false));
    CHECK(!fs::exists(base / "foto.png"));
    CHECK(!fs::exists(importSidecarPath(base / "foto.png")));

    CHECK(!removeAssetPath(base / "carpeta", true));
    CHECK(!fs::exists(base / "carpeta"));

    // Un sidecar HUERFANO no da error al listar ni al mover otro asset.
    CHECK(saveTextureImportSettings(base / "fantasma.png", mipsOn(), &err));
    CHECK(listVisibleEntries(base).empty());
    std::ofstream(base / "otra.png") << "x";
    fs::create_directories(base / "dest", ec);
    CHECK(moveAsset(base / "otra.png", base / "dest").result == MoveResult::Moved);
}

static std::shared_ptr<Mesh> meshWithTexture(const fs::path& tex)
{
    auto m = std::make_shared<Mesh>();
    m->material.texturePath = tex.string();
    return m;
}

static void test_apply_writes_sidecar_and_rebuilds_only_users()
{
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "dt_cb_apply";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    const fs::path foto = base / "foto.png";
    const fs::path otra = base / "otra.png";
    std::ofstream(foto) << "x";
    std::ofstream(otra) << "x";

    GameObject root("root");
    GameObject* a = root.addChild("A");
    GameObject* b = root.addChild("B");
    root.addChild("C");                         // sin mesh
    a->setMesh(meshWithTexture(foto));
    b->setMesh(meshWithTexture(otra));

    std::vector<GameObject*> rebuilt;
    const auto rebuild = [&](GameObject& go) { rebuilt.push_back(&go); };

    TextureImportSettings s;
    s.colorSpace = ColorSpaceOverride::Linear;
    s.mipmaps    = true;
    TextureImportApplyResult r = applyTextureImportSettings(&root, foto, s, rebuild);
    CHECK(r.ok);
    CHECK(r.error.empty());
    CHECK(r.refreshed == 1);
    CHECK(rebuilt.size() == 1 && rebuilt[0] == a);
    CHECK(loadTextureImportSettings(foto) == s);

    // Con el defecto: borra el sidecar y sigue reconstruyendo al que la usa.
    rebuilt.clear();
    r = applyTextureImportSettings(&root, foto, TextureImportSettings{}, rebuild);
    CHECK(r.ok);
    CHECK(r.refreshed == 1);
    CHECK(rebuilt.size() == 1 && rebuilt[0] == a);
    CHECK(!fs::exists(importSidecarPath(foto)));
}

// Review Focus 6.
static void test_apply_reports_write_failure_and_rebuilds_nothing()
{
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "dt_cb_apply_fail";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    const fs::path foto = base / "no_existe_carpeta" / "foto.png";     // el sidecar no se puede escribir

    GameObject root("root");
    root.addChild("A")->setMesh(meshWithTexture(foto));

    int calls = 0;
    TextureImportSettings s;
    s.mipmaps = true;
    const TextureImportApplyResult r =
        applyTextureImportSettings(&root, foto, s, [&](GameObject&) { ++calls; });
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    CHECK(r.refreshed == 0);
    CHECK(calls == 0);
}

static void test_apply_without_renderer_still_writes()
{
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "dt_cb_apply_norender";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    const fs::path foto = base / "foto.png";
    std::ofstream(foto) << "x";

    GameObject root("root");
    root.addChild("A")->setMesh(meshWithTexture(foto));

    TextureImportSettings s;
    s.mipmaps = true;
    const TextureImportApplyResult r = applyTextureImportSettings(&root, foto, s, {});
    CHECK(r.ok);
    CHECK(r.refreshed == 0);
    CHECK(loadTextureImportSettings(foto) == s);
}

static void test_apply_audio_writes_sidecar_and_refreshes_once()
{
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "dt_cb_apply_audio";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    const fs::path clip = base / "disparo.wav";
    std::ofstream(clip) << "wav";

    std::vector<std::string> refreshed;
    const auto refresh = [&](const std::string& p) { refreshed.push_back(p); };

    AudioImportSettings s;
    s.gainDb    = -4.0f;
    s.forceMono = true;
    AudioImportApplyResult r = applyAudioImportSettings(clip, s, refresh);
    CHECK(r.ok);
    CHECK(r.error.empty());
    CHECK(refreshed.size() == 1 && refreshed[0] == clip.string());
    CHECK(loadAudioImportSettings(clip) == s);

    // El defecto borra el sidecar y aun asi refresca (la voz vuelve a su volumen).
    r = applyAudioImportSettings(clip, AudioImportSettings{}, refresh);
    CHECK(r.ok);
    CHECK(refreshed.size() == 2);
    CHECK(!fs::exists(importSidecarPath(clip)));
}

// Review Focus 8: si el sidecar no se puede escribir, no se refresca nada.
static void test_apply_audio_write_failure_does_not_refresh()
{
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "dt_cb_apply_audio_fail";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    int calls = 0;
    AudioImportSettings s;
    s.gainDb = 3.0f;
    const AudioImportApplyResult r = applyAudioImportSettings(
        base / "no_existe_carpeta" / "x.wav", s, [&](const std::string&) { ++calls; });
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    CHECK(calls == 0);
}

static void test_apply_audio_without_audio_manager_still_writes()
{
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "dt_cb_apply_audio_norefresh";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    const fs::path clip = base / "a.wav";
    std::ofstream(clip) << "wav";
    AudioImportSettings s;
    s.forceMono = true;
    const AudioImportApplyResult r = applyAudioImportSettings(clip, s, {});
    CHECK(r.ok);
    CHECK(loadAudioImportSettings(clip) == s);
}

// Review Focus 8: que assets ofrecen "Import Settings...".
static void test_import_settings_menu_kind()
{
    CHECK(importSettingsKindFor(".png", false)  == ImportSettingsKind::Texture);
    CHECK(importSettingsKindFor(".WAV", false)  == ImportSettingsKind::Audio);
    CHECK(importSettingsKindFor(".mp3", false)  == ImportSettingsKind::Audio);
    CHECK(importSettingsKindFor(".ogg", false)  == ImportSettingsKind::Audio);
    CHECK(importSettingsKindFor(".flac", false) == ImportSettingsKind::Audio);
    CHECK(importSettingsKindFor(".fbx", false)  == ImportSettingsKind::Model);
    CHECK(importSettingsKindFor(".GLB", false)  == ImportSettingsKind::Model);   // cualquier modelo 3D
    CHECK(importSettingsKindFor(".fbx", true)   == ImportSettingsKind::None);    // una carpeta
    CHECK(importSettingsKindFor(".lua", false)  == ImportSettingsKind::None);
    CHECK(importSettingsKindFor(".wav", true)   == ImportSettingsKind::None);   // una carpeta
}

static void test_classify_material_extension()
{
    CHECK(classifyAsset(".mat", false) == AssetKind::Material);
    CHECK(classifyAsset(".MAT", false) == AssetKind::Material);
    CHECK(classifyAsset(".mat", true)  == AssetKind::Folder);   // una carpeta manda
}

// Hallazgo del reviewer final: el Browse del modal de edicion de un .mat
// aceptaba una ruta absoluta de fuera del proyecto tal cual; ese .mat viaja
// con ella y el juego exportado deja de encontrar la textura en otra maquina.
static void test_accept_or_import_mat_texture_imports_external_path()
{
    std::error_code ec;
    const fs::path projectRoot = fs::temp_directory_path(ec) / "dt_cb_matimport_project";
    const fs::path outside     = fs::temp_directory_path(ec) / "dt_cb_matimport_outside";
    fs::remove_all(projectRoot, ec);
    fs::remove_all(outside, ec);
    fs::create_directories(projectRoot, ec);
    fs::create_directories(outside, ec);
    std::ofstream(outside / "textura.png") << "x";

    ProjectContext project(projectRoot);
    const auto imported = acceptOrImportMatTexture(&project, outside / "textura.png");
    CHECK(imported.has_value());
    if (imported)
    {
        CHECK(project.contains(*imported));
        CHECK(fs::exists(*imported));
    }
}

// Una ruta que YA está dentro del proyecto se devuelve tal cual (sin copiar
// una segunda vez).
static void test_accept_or_import_mat_texture_leaves_internal_path()
{
    std::error_code ec;
    const fs::path projectRoot = fs::temp_directory_path(ec) / "dt_cb_matimport_internal";
    fs::remove_all(projectRoot, ec);
    fs::create_directories(projectRoot / "assets", ec);
    std::ofstream(projectRoot / "assets" / "textura.png") << "x";

    ProjectContext project(projectRoot);
    const auto result = acceptOrImportMatTexture(&project, projectRoot / "assets" / "textura.png");
    CHECK(result.has_value());
    if (result) CHECK(fs::equivalent(*result, projectRoot / "assets" / "textura.png", ec));
}

// Sin proyecto (tests headless, o el .mat se edita antes de elegir uno) se
// acepta tal cual, como el resto del editor sin proyecto abierto.
static void test_accept_or_import_mat_texture_without_project_passes_through()
{
    const fs::path anyPath = "C:/no/existe/x.png";
    const auto result = acceptOrImportMatTexture(nullptr, anyPath);
    CHECK(result.has_value());
    if (result) CHECK(*result == anyPath);
}

// Hallazgo del reviewer final: un .mat suelto no se podia arrastrar desde el
// grid (isImportableExtension no lo cubre, porque un .mat nunca se importa
// de fuera del proyecto), asi que ni "Drop .mat here" en Properties ni mover
// un .mat a otra carpeta arrastrandolo funcionaban.
static void test_material_is_draggable()
{
    CHECK(isAssetDraggable(".mat", /*isDir=*/false, /*inMultiSelection=*/false));
    CHECK(isAssetDraggable(".MAT", false, false));
    CHECK(!isAssetDraggable(".unknownext", false, false));
    CHECK(isAssetDraggable(".unknownext", false, /*inMultiSelection=*/true));
    CHECK(isAssetDraggable("", /*isDir=*/true, false));
}

static void test_unique_material_name()
{
    std::error_code ec;
    const fs::path d = fs::temp_directory_path(ec) / "dt_cb_unique_mat";
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    CHECK(uniqueMaterialName(d) == "Nuevo material.mat");
    std::ofstream(d / "Nuevo material.mat") << "x";
    CHECK(uniqueMaterialName(d) == "Nuevo material 2.mat");
}

// applyMaterialAssetSettings escribe el fichero y reconstruye SOLO a quien
// referencia ese .mat, sea cual sea el objeto (Review Focus 2: varios usuarios).
static void test_apply_material_asset_writes_and_refreshes_all_users()
{
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "dt_cb_apply_mat";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    const fs::path mat = base / "x.mat";
    std::string err;
    CHECK(saveMaterialAsset(mat, MaterialAsset{}, &err));

    GameObject root("root");
    GameObject* a = root.addChild("A");
    GameObject* b = root.addChild("B");
    GameObject* c = root.addChild("C");   // no usa el .mat
    a->setMesh(std::make_shared<Mesh>());
    b->setMesh(std::make_shared<Mesh>());
    c->setMesh(std::make_shared<Mesh>());
    MaterialOverride ovA; ovA.matAsset = mat.string();
    a->materialOverrides = {ovA};
    MaterialOverride ovB; ovB.matAsset = mat.string();
    b->materialOverrides = {ovB};

    std::vector<GameObject*> rebuilt;
    const auto rebuild = [&](GameObject& go) { rebuilt.push_back(&go); };

    MaterialAsset s; s.roughness = 0.3f;
    const MaterialAssetApplyResult r = applyMaterialAssetSettings(&root, mat, s, rebuild);
    CHECK(r.ok);
    CHECK(r.error.empty());
    CHECK(r.refreshed == 2);
    CHECK(rebuilt.size() == 2);
    CHECK((rebuilt[0] == a && rebuilt[1] == b) || (rebuilt[0] == b && rebuilt[1] == a));
    CHECK(loadMaterialAsset(mat) == s);
    CHECK(a->getMesh()->material.roughness == 0.3f);
    CHECK(c->getMesh()->material.roughness == 0.5f);   // el defecto de Material, sin tocar
}

// Reporte del usuario: "al abrir el material no guarda lo anadido, por
// ejemplo el albedo". Mismo camino EXACTO que Aplicar en el modal: guardar
// via applyMaterialAssetSettings y releer via loadMaterialAsset (lo que hace
// el doble clic al reabrir), con una textura ABSOLUTA como la que deja
// acceptOrImportMatTexture/un drag del grid.
static void test_apply_material_asset_albedo_survives_reopen()
{
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "dt_cb_apply_mat_albedo";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    const fs::path mat = base / "x.mat";
    const fs::path tex = base / "rojo.png";
    std::ofstream(tex) << "x";
    std::string err;
    CHECK(saveMaterialAsset(mat, MaterialAsset{}, &err));

    MaterialAsset edit = loadMaterialAsset(mat);   // lo que hace el doble clic al abrir
    edit.albedo = tex.string();                    // lo que deja Browse/drop en el modal

    const MaterialAssetApplyResult r = applyMaterialAssetSettings(nullptr, mat, edit, nullptr);
    CHECK(r.ok);
    CHECK(r.error.empty());

    const MaterialAsset reopened = loadMaterialAsset(mat);   // reabrir el .mat
    CHECK(fs::equivalent(reopened.albedo, tex, ec));
}

// Review Focus (escritura): un fallo de escritura no reconstruye nada.
static void test_apply_material_asset_write_failure_rebuilds_nothing()
{
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "dt_cb_apply_mat_fail";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    GameObject root("root");
    root.addChild("A");
    int calls = 0;
    const MaterialAssetApplyResult r = applyMaterialAssetSettings(
        &root, base / "no_existe_carpeta" / "x.mat", MaterialAsset{}, [&](GameObject&) { ++calls; });
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    CHECK(r.refreshed == 0);
    CHECK(calls == 0);
}

// ── reimportModelUsers ───────────────────────────────────────────────────────

static fs::path writeTriangleObj(const char* dirName)
{
    std::error_code ec;
    const fs::path d = fs::temp_directory_path(ec) / dirName;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    const fs::path obj = d / "tri.obj";
    std::ofstream(obj) << "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n";
    return obj;
}

static float maxAbsX(const Mesh& m)
{
    float r = 0.0f;
    for (const Vertex& v : m.vertices) r = std::max(r, std::abs(v.pos.x));
    return r;
}

// Los objetos de ese FBX se recargan con los ajustes nuevos; los demas ni se tocan.
static void test_reimport_replaces_the_meshes_of_that_fbx_only()
{
    const fs::path obj = writeTriangleObj("dt_cb_reimport_static");
    GameObject root("root");
    GameObject* a = root.addChild("A");
    GameObject* b = root.addChild("B");
    GameObject* c = root.addChild("C");   // procedural: sin sourcePath
    a->setMesh(ModelLoader::loadAuto(obj.string()));
    b->setMesh(ModelLoader::loadAuto(obj.string()));
    c->setMesh(std::make_shared<Mesh>());
    CHECK(maxAbsX(*a->getMesh()) == 1.0f);

    // Un override de textura del objeto A: tiene que sobrevivir al reimport.
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/x.png";
    a->materialOverrides = { ov };

    ModelImportSettings s;
    s.scale = 2.0f;
    std::string err;
    CHECK(saveModelImportSettings(obj, s, &err));

    const ModelReimportResult r = reimportModelUsers(&root, obj, nullptr);
    CHECK(r.reimported == 2);
    CHECK(r.skipped == 0);
    CHECK(r.warnings.empty());
    CHECK(maxAbsX(*a->getMesh()) == 2.0f);
    CHECK(maxAbsX(*b->getMesh()) == 2.0f);
    CHECK(c->getMesh()->sourcePath.empty());                          // no era de ese FBX
    CHECK(a->getMesh()->sourcePath == obj.string());
    CHECK(a->materialOverrides[0].albedo == "assets/x.png");
    CHECK(a->getMesh()->material.texturePath == "assets/x.png");      // reaplicado sobre la malla nueva
}

// Review Focus 2: el FBX ya no carga -> los objetos se quedan como estaban.
static void test_reimport_of_an_unloadable_fbx_leaves_the_objects_intact()
{
    const fs::path obj = writeTriangleObj("dt_cb_reimport_missing");
    GameObject root("root");
    GameObject* a = root.addChild("A");
    a->setMesh(ModelLoader::loadAuto(obj.string()));
    const Mesh* antes = a->getMesh().get();

    std::error_code ec;
    fs::remove(obj, ec);                                               // el fichero desaparece

    const ModelReimportResult r = reimportModelUsers(&root, obj, nullptr);
    CHECK(r.reimported == 0);
    CHECK(r.skipped == 1);
    CHECK(r.warnings.size() == 1);
    CHECK(a->hasMesh());
    CHECK(a->getMesh().get() == antes);
    CHECK(maxAbsX(*a->getMesh()) == 1.0f);
}

// Sin usuarios no hay nada que hacer ni que avisar.
static void test_reimport_without_users_is_a_noop()
{
    const fs::path obj = writeTriangleObj("dt_cb_reimport_nousers");
    GameObject root("root");
    root.addChild("A")->setMesh(std::make_shared<Mesh>());
    const ModelReimportResult r = reimportModelUsers(&root, obj, nullptr);
    CHECK(r.reimported == 0 && r.skipped == 0 && r.warnings.empty());
    const ModelReimportResult r2 = reimportModelUsers(nullptr, obj, nullptr);
    CHECK(r2.reimported == 0 && r2.skipped == 0);
}

// Defensivo: un objeto con malla y a la vez con una carga asincrona en vuelo no
// se toca (mismo criterio que MeshComponentCommand::put).
static void test_reimport_skips_an_object_with_a_pending_load()
{
    const fs::path obj = writeTriangleObj("dt_cb_reimport_pending");
    GameObject root("root");
    GameObject* a = root.addChild("A");
    a->setMesh(ModelLoader::loadAuto(obj.string()));
    a->pendingMeshJob = 7;

    ModelImportSettings s;
    s.scale = 2.0f;
    std::string err;
    CHECK(saveModelImportSettings(obj, s, &err));

    const ModelReimportResult r = reimportModelUsers(&root, obj, nullptr);
    CHECK(r.reimported == 0);
    CHECK(r.skipped == 1);
    CHECK(r.warnings.size() == 1);
    CHECK(maxAbsX(*a->getMesh()) == 1.0f);
}

// Review Focus 3: un personaje con clip renombrado, fuente externa y Animator.
static void test_reimport_skinned_keeps_the_animation_config_and_rebinds()
{
    std::error_code ec;
    const fs::path d = fs::temp_directory_path(ec) / "dt_cb_reimport_skinned";
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    const fs::path fbx = d / "char.fbx";
    fs::copy_file("assets/modelAnimation.fbx", fbx, fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);

    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned(fbx.string()));
    std::vector<std::string> w;
    CHECK(addAnimationSource(*mesh, fbx.string(), w));
    CHECK(mesh->animationSources.size() == 2u);
    if (mesh->animationSources.size() != 2u) return;
    CHECK(renameClip(*mesh, mesh->animationSources[0].clipNames[0], "CaminarRenombrado"));
    CHECK(renameClip(*mesh, mesh->animationSources[1].clipNames[0], "SaltoRenombrado"));
    const float extentAntes = [&] {
        float r = 0.0f;
        for (const SkinnedVertex& v : mesh->skinnedVertices) r = std::max(r, std::abs(v.position.x));
        return r;
    }();

    GameObject root("root");
    GameObject* go = root.addChild("Personaje");
    go->setMesh(mesh);
    auto anim = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State st;
    st.name = "Salto"; st.clipName = "SaltoRenombrado";
    anim->addState(st);
    go->setAnimator(anim);

    ModelImportSettings s;
    s.scale = 2.0f;
    std::string err;
    CHECK(saveModelImportSettings(fbx, s, &err));

    const ModelReimportResult r = reimportModelUsers(&root, fbx, nullptr);
    CHECK(r.reimported == 1);
    CHECK(r.skipped == 0);

    const SkinnedMesh* nuevo = go->getSkinnedMesh();
    CHECK(nuevo != nullptr);
    if (!nuevo) return;
    CHECK(nuevo != mesh.get());                                        // malla nueva, no la vieja
    float extentDespues = 0.0f;
    for (const SkinnedVertex& v : nuevo->skinnedVertices)
        extentDespues = std::max(extentDespues, std::abs(v.position.x));
    CHECK(std::abs(extentDespues - 2.0f * extentAntes) <= 1e-3f * std::max(1.0f, extentAntes));

    CHECK(nuevo->animationSources.size() == 2u);
    if (nuevo->animationSources.size() == 2u)
    {
        CHECK(nuevo->animationSources[0].builtin);
        CHECK(nuevo->animationSources[0].clipNames[0] == "CaminarRenombrado");
        CHECK(!nuevo->animationSources[1].builtin);
        CHECK(nuevo->animationSources[1].clipNames[0] == "SaltoRenombrado");
    }
    CHECK(go->getAnimator()->states()[0].clipIndex >= 0);              // rebindClips lo resolvio
}

// Hallazgo del reviewer final: un FBX usado solo como fuente de animacion externa
// (el flujo Mixamo que documenta el README) tambien es "usado" por el personaje.
// Sin esto, poner la escala en el FBX de animacion decia "0 objetos" y el
// personaje se quedaba con las traslaciones de raiz a otra escala.
static const BoneKeyframe* firstPosKeyOfClip(const SkinnedMesh& m, const std::string& clipName)
{
    for (const AnimationClip& c : m.animationClips)
    {
        if (c.name != clipName) continue;
        for (const BoneChannel& ch : c.channels)
            if (!ch.posKeys.empty()) return &ch.posKeys[0];
    }
    return nullptr;
}

static void test_reimport_also_reloads_characters_that_use_the_fbx_as_animation_source()
{
    std::error_code ec;
    const fs::path d = fs::temp_directory_path(ec) / "dt_cb_reimport_animsource";
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    const fs::path character = d / "char.fbx";
    const fs::path anim      = d / "anim.fbx";
    fs::copy_file("assets/modelAnimation.fbx", character, fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    fs::copy_file("assets/modelAnimation.fbx", anim, fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);

    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned(character.string()));
    std::vector<std::string> w;
    CHECK(addAnimationSource(*mesh, anim.string(), w));
    CHECK(mesh->animationSources.size() == 2u);
    if (mesh->animationSources.size() != 2u) return;
    const std::string externalClip = mesh->animationSources[1].clipNames[0];
    const BoneKeyframe* antes = firstPosKeyOfClip(*mesh, externalClip);
    CHECK(antes != nullptr);
    if (!antes) return;
    const glm::vec3 keyAntes = antes->value;
    float extentAntes = 0.0f;
    for (const SkinnedVertex& v : mesh->skinnedVertices) extentAntes = std::max(extentAntes, std::abs(v.position.x));

    GameObject root("root");
    GameObject* go = root.addChild("Personaje");
    go->setMesh(mesh);

    // Escala SOLO en el fichero de animacion.
    ModelImportSettings s;
    s.scale = 2.0f;
    std::string err;
    CHECK(saveModelImportSettings(anim, s, &err));

    const ModelReimportResult r = reimportModelUsers(&root, anim, nullptr);
    CHECK(r.reimported == 1);

    const SkinnedMesh* nuevo = go->getSkinnedMesh();
    CHECK(nuevo != nullptr);
    if (!nuevo) return;
    const BoneKeyframe* despues = firstPosKeyOfClip(*nuevo, externalClip);
    CHECK(despues != nullptr);
    if (despues)
        CHECK(std::abs(despues->value.y - 2.0f * keyAntes.y) <= 1e-3f * std::max(1.0f, std::abs(keyAntes.y)) &&
              std::abs(despues->value.z - 2.0f * keyAntes.z) <= 1e-3f * std::max(1.0f, std::abs(keyAntes.z)));
    // La malla del personaje NO cambia: su propio sidecar no existe.
    float extentDespues = 0.0f;
    for (const SkinnedVertex& v : nuevo->skinnedVertices) extentDespues = std::max(extentDespues, std::abs(v.position.x));
    CHECK(extentDespues == extentAntes);

    // Un FBX que no usa nadie sigue sin tocar nada.
    const ModelReimportResult otro = reimportModelUsers(&root, d / "otro.fbx", nullptr);
    CHECK(otro.reimported == 0 && otro.skipped == 0);
}

// ── applyModelImportSettings ─────────────────────────────────────────────────

static void test_apply_model_writes_sidecar_and_reimports_once()
{
    std::error_code ec;
    const fs::path d = fs::temp_directory_path(ec) / "dt_cb_apply_model";
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    const fs::path fbx = d / "nave.fbx";
    std::ofstream(fbx) << "fbx";

    ModelImportSettings s;
    s.scale = 0.5f;
    int calls = 0;
    fs::path seen;
    const ModelImportApplyResult r = applyModelImportSettings(
        fbx, s, [&](const fs::path& p) { ++calls; seen = p; return 3; });
    CHECK(r.ok);
    CHECK(r.error.empty());
    CHECK(r.refreshed == 3);
    CHECK(calls == 1);
    CHECK(seen == fbx);
    CHECK(loadModelImportSettings(fbx) == s);
}

// Un fallo de escritura no recarga nada y el modal muestra el error.
static void test_apply_model_write_failure_does_not_reimport()
{
    std::error_code ec;
    const fs::path d = fs::temp_directory_path(ec) / "dt_cb_apply_model_fail";
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    const fs::path fbx = d / "carpeta_que_no_existe" / "nave.fbx";

    ModelImportSettings s;
    s.scale = 0.5f;
    int calls = 0;
    const ModelImportApplyResult r = applyModelImportSettings(
        fbx, s, [&](const fs::path&) { ++calls; return 1; });
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    CHECK(calls == 0);
    CHECK(r.refreshed == 0);
}

// Sin reimport (tests, o sin escena) solo se escribe.
static void test_apply_model_without_reimport_only_writes()
{
    std::error_code ec;
    const fs::path d = fs::temp_directory_path(ec) / "dt_cb_apply_model_plain";
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    const fs::path fbx = d / "nave.fbx";
    std::ofstream(fbx) << "fbx";

    ModelImportSettings s;
    s.flipUVs = false;
    const ModelImportApplyResult r = applyModelImportSettings(fbx, s, nullptr);
    CHECK(r.ok);
    CHECK(r.refreshed == 0);
    CHECK(loadModelImportSettings(fbx) == s);
}

int main()
{
    test_reimport_also_reloads_characters_that_use_the_fbx_as_animation_source();
    test_apply_model_writes_sidecar_and_reimports_once();
    test_apply_model_write_failure_does_not_reimport();
    test_apply_model_without_reimport_only_writes();
    test_reimport_replaces_the_meshes_of_that_fbx_only();
    test_reimport_of_an_unloadable_fbx_leaves_the_objects_intact();
    test_reimport_without_users_is_a_noop();
    test_reimport_skips_an_object_with_a_pending_load();
    test_reimport_skinned_keeps_the_animation_config_and_rebinds();
    test_classify_material_extension();
    test_material_is_draggable();
    test_accept_or_import_mat_texture_imports_external_path();
    test_accept_or_import_mat_texture_leaves_internal_path();
    test_accept_or_import_mat_texture_without_project_passes_through();
    test_unique_material_name();
    test_apply_material_asset_writes_and_refreshes_all_users();
    test_apply_material_asset_albedo_survives_reopen();
    test_apply_material_asset_write_failure_rebuilds_nothing();
    test_apply_audio_writes_sidecar_and_refreshes_once();
    test_apply_audio_write_failure_does_not_refresh();
    test_apply_audio_without_audio_manager_still_writes();
    test_import_settings_menu_kind();
    test_apply_writes_sidecar_and_rebuilds_only_users();
    test_apply_reports_write_failure_and_rebuilds_nothing();
    test_apply_without_renderer_still_writes();
    test_list_hides_import_sidecars();
    test_move_asset_carries_sidecar();
    test_move_asset_rewrites_mat_relative_texture();
    test_move_asset_rejects_when_destination_sidecar_exists();
    test_rename_asset_file_carries_sidecar_and_rejects_conflict();
    test_rename_asset_file_case_only_is_not_a_sidecar_conflict();
    test_remove_asset_path_removes_sidecar();
    fs::path root = makeFixture();
    test_filters_noise(root);
    test_hides_build_tree_by_content(root);
    test_generic_name_without_cache_is_visible(root);
    test_empty_dir(root);
    test_missing_dir(root);
    test_path_is_file(root);
    test_count_references_finds_skinned_material_texture();
    test_rename_rewrites_skinned_material_path();
    test_detach_clears_skinned_material_path();
    test_rename_rewrites_material_override_path();
    test_detach_clears_material_override_path();
    test_detach_clears_material_override_baseline();
    test_rename_rewrites_material_override_baseline();
    test_count_references_finds_mat_asset();
    test_rename_rewrites_mat_asset_path();
    test_rename_leaves_other_mat_asset_untouched();
    test_detach_clears_mat_asset_and_falls_back_to_model();
    test_import_dropped_files_into(root);
    test_classify_asset();
    test_asset_matches_filter();
    test_unique_folder_name(root);
    test_breadcrumb_segments(root);
    test_move_asset(root);
    test_apply_asset_click();
    test_prune_selection();
    test_list_visible_entries(root);
    test_nearest_existing_dir(root);
    std::error_code ec;
    fs::remove_all(root, ec);
    if (g_failures == 0) std::printf("ALL CONTENT BROWSER TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
