// Test headless de AssetImport (sin GUI). Plain main + CHECK, mismo patron
// que content_browser_tests.cpp.
#include "DonTopo/Editor/AssetImport.h"
#include "DonTopo/Core/ImportSettings.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

using namespace DonTopo;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static fs::path makeFixture()
{
    std::error_code ec;
    fs::path root = fs::temp_directory_path(ec) / "dt_asset_import_test";
    fs::remove_all(root, ec);
    fs::create_directories(root / "source", ec);
    fs::create_directories(root / "dest", ec);
    return root;
}

static void test_is_importable_extension()
{
    CHECK(isImportableExtension(".fbx"));
    CHECK(isImportableExtension(".PNG"));   // mayusculas
    CHECK(isImportableExtension(".wav"));
    CHECK(isImportableExtension(".ttf"));
    CHECK(!isImportableExtension(".txt"));
    CHECK(!isImportableExtension(".spv"));
    CHECK(!isImportableExtension(""));
    for (const char* e : { ".obj", ".gltf", ".GLB" })
        CHECK(isImportableExtension(e));
}

static void test_imported_asset_dest_dir()
{
    fs::path root = fs::path("proyecto");
    CHECK(importedAssetDestDir(root, ".fbx") == root / "assets" / "Imported" / "Meshes");
    CHECK(importedAssetDestDir(root, ".WAV") == root / "assets" / "Imported" / "Audio");
    CHECK(importedAssetDestDir(root, ".png") == root / "assets" / "Imported" / "Textures");
    CHECK(importedAssetDestDir(root, ".ttf") == root / "assets" / "Imported" / "Fonts");
    CHECK(importedAssetDestDir(root, ".xyz").empty());
    for (const char* e : { ".obj", ".gltf", ".glb" })
        CHECK(importedAssetDestDir(root, e) == root / "assets" / "Imported" / "Meshes");
}

static void test_import_copies_file(const fs::path& root)
{
    fs::path source = root / "source" / "modelo.fbx";
    std::ofstream(source) << "contenido-fbx";
    fs::path destDir = root / "dest" / "copia1";

    AssetImportOutcome outcome = importExternalAsset(source, destDir);

    CHECK(outcome.result == AssetImportResult::Copied);
    CHECK(outcome.destPath == destDir / "modelo.fbx");
    CHECK(outcome.sourcePath == source);
    CHECK(fs::exists(outcome.destPath));
    std::ifstream in(outcome.destPath);
    std::stringstream ss; ss << in.rdbuf();
    CHECK(ss.str() == "contenido-fbx");
}

static void test_import_missing_source_fails(const fs::path& root)
{
    AssetImportOutcome outcome = importExternalAsset(root / "no_existe.fbx", root / "dest");
    CHECK(outcome.result == AssetImportResult::RejectedCopyFailed);
    CHECK(!outcome.errorMessage.empty());
}

// Soltar una CARPETA (no un fichero) desde el Explorador: no debe intentar
// copy_file sobre un directorio.
static void test_import_directory_source_fails(const fs::path& root)
{
    AssetImportOutcome outcome = importExternalAsset(root / "source", root / "dest");
    CHECK(outcome.result == AssetImportResult::RejectedCopyFailed);
}

// El destino ya tiene un fichero con ese nombre: se rechaza SIN tocarlo, no
// se sobreescribe ni se renombra en automatico.
static void test_import_name_conflict_does_not_overwrite(const fs::path& root)
{
    fs::path destDir = root / "dest" / "copia2";
    std::error_code ec;
    fs::create_directories(destDir, ec);
    std::ofstream(destDir / "textura.png") << "version-vieja";

    fs::path source = root / "source" / "textura.png";
    std::ofstream(source) << "version-nueva";

    AssetImportOutcome outcome = importExternalAsset(source, destDir);

    CHECK(outcome.result == AssetImportResult::RejectedNameConflict);
    std::ifstream in(destDir / "textura.png");
    std::stringstream ss; ss << in.rdbuf();
    CHECK(ss.str() == "version-vieja");
}

// destDir vacio: "" / "x.png" resolveria a un path relativo y copiaria al CWD
// del proceso. Se rechaza antes de tocar disco.
static void test_import_empty_dest_dir_is_rejected(const fs::path& root)
{
    fs::path source = root / "source" / "vacio.png";
    std::ofstream(source) << "x";

    AssetImportOutcome outcome = importExternalAsset(source, fs::path());

    CHECK(outcome.result == AssetImportResult::RejectedCopyFailed);
    CHECK(!outcome.errorMessage.empty());
    std::error_code ec;
    CHECK(!fs::exists(fs::path("vacio.png"), ec)); // nada aterrizo en el CWD
}

// Si destDir no se puede crear (un tramo del path es un FICHERO), el mensaje
// tiene que decir que falto la carpeta, no el vago "no se encuentra la ruta"
// que devuelve copy_file despues.
static void test_import_uncreatable_dest_dir_reports_cause(const fs::path& root)
{
    fs::path blocker = root / "dest" / "es_un_fichero";
    std::ofstream(blocker) << "x";
    fs::path source = root / "source" / "bloqueado.png";
    std::ofstream(source) << "x";

    AssetImportOutcome outcome = importExternalAsset(source, blocker / "subcarpeta");

    CHECK(outcome.result == AssetImportResult::RejectedCopyFailed);
    CHECK(outcome.errorMessage.rfind("No se pudo crear la carpeta destino", 0) == 0);
}

static void test_import_external_copies_sidecar(const fs::path& root)
{
    const fs::path source = root / "source" / "tablero.png";
    std::ofstream(source) << "png";
    TextureImportSettings s;
    s.colorSpace = ColorSpaceOverride::Linear;
    s.mipmaps    = true;
    std::string err;
    CHECK(saveTextureImportSettings(source, s, &err));

    const fs::path destDir = root / "dest" / "conSidecar";
    const AssetImportOutcome outcome = importExternalAsset(source, destDir);
    CHECK(outcome.result == AssetImportResult::Copied);
    CHECK(outcome.errorMessage.empty());
    CHECK(loadTextureImportSettings(outcome.destPath) == s);
    CHECK(fs::exists(importSidecarPath(source)));                   // el origen conserva el suyo

    // Sin sidecar en el origen: no aparece ninguno en el destino.
    const fs::path plain = root / "source" / "liso.png";
    std::ofstream(plain) << "png";
    const AssetImportOutcome o2 = importExternalAsset(plain, destDir);
    CHECK(o2.result == AssetImportResult::Copied);
    CHECK(!fs::exists(importSidecarPath(o2.destPath)));
}

int main()
{
    fs::path root = makeFixture();
    test_import_external_copies_sidecar(root);
    test_is_importable_extension();
    test_imported_asset_dest_dir();
    test_import_copies_file(root);
    test_import_missing_source_fails(root);
    test_import_directory_source_fails(root);
    test_import_name_conflict_does_not_overwrite(root);
    test_import_empty_dest_dir_is_rejected(root);
    test_import_uncreatable_dest_dir_reports_cause(root);
    std::error_code ec;
    fs::remove_all(root, ec);
    if (g_failures == 0) std::printf("ALL ASSET IMPORT TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
