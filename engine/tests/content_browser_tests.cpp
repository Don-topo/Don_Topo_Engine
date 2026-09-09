// Test headless de los helpers del Content Browser (sin GUI). Plain main +
// asserts, sin framework — mismo patrón que physics_tests.cpp.
#include "DonTopo/Editor/ContentBrowserPanel.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/SkinnedMesh.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
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

int main()
{
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
    std::error_code ec;
    fs::remove_all(root, ec);
    if (g_failures == 0) std::printf("ALL CONTENT BROWSER TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
