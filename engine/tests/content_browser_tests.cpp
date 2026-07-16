// Test headless de los helpers del Content Browser (sin GUI). Plain main +
// asserts, sin framework — mismo patrón que physics_tests.cpp.
#include "DonTopo/Editor/ContentBrowserPanel.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
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
    std::ofstream(root / "readme.txt") << "x";
    return root;
}

// Lista sólo subcarpetas: ni ficheros, ni ocultas, ni build-ninja.
static void test_filters_noise(const fs::path& root)
{
    std::vector<fs::path> dirs = listVisibleSubdirs(root);
    CHECK(dirs.size() == 2);
    if (dirs.size() == 2) {
        // std::sort sobre paths: 'S' (83) va antes que 'a' (97).
        CHECK(dirs[0].filename() == "Scripts");
        CHECK(dirs[1].filename() == "assets");
    }
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

int main()
{
    fs::path root = makeFixture();
    test_filters_noise(root);
    test_empty_dir(root);
    test_missing_dir(root);
    test_path_is_file(root);
    std::error_code ec;
    fs::remove_all(root, ec);
    if (g_failures == 0) std::printf("ALL CONTENT BROWSER TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
