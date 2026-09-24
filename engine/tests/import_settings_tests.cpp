// Test headless de ImportSettings (sin GUI, sin GPU). Plain main + CHECK,
// mismo patron que content_browser_tests.cpp. Se ejecuta desde la raiz del repo.
#include "DonTopo/Core/ImportSettings.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using namespace DonTopo;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static fs::path makeDir()
{
    std::error_code ec;
    fs::path d = fs::temp_directory_path(ec) / "dt_import_settings_test";
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

static void writeText(const fs::path& p, const std::string& s)
{
    std::ofstream(p, std::ios::binary) << s;
}

static void test_sidecar_path_and_detection()
{
    CHECK(importSidecarPath(fs::path("a") / "foto.png") == fs::path("a") / "foto.png.import.json");
    CHECK(isImportSidecar(fs::path("a") / "foto.png.import.json"));
    CHECK(isImportSidecar("FOTO.PNG.IMPORT.JSON"));
    CHECK(!isImportSidecar("scene.json"));
    CHECK(!isImportSidecar("foto.png"));
    CHECK(!isImportSidecar(".import.json"));     // sin nombre delante: no es de nadie
}

static void test_missing_file_is_default_without_warning()
{
    const fs::path d = makeDir();
    std::string warning = "x";
    const TextureImportSettings s = loadTextureImportSettings(d / "no_existe.png", &warning);
    CHECK(isDefault(s));
    CHECK(warning.empty());            // ausente no es un problema, es lo normal
}

static void test_roundtrip_and_default_removes_sidecar()
{
    const fs::path d = makeDir();
    const fs::path asset = d / "foto.png";
    std::string err;

    TextureImportSettings in;
    in.colorSpace = ColorSpaceOverride::Linear;
    in.mipmaps    = true;
    CHECK(saveTextureImportSettings(asset, in, &err));
    CHECK(fs::exists(importSidecarPath(asset)));
    CHECK(loadTextureImportSettings(asset) == in);

    // Guardar el defecto BORRA el fichero.
    CHECK(saveTextureImportSettings(asset, TextureImportSettings{}, &err));
    CHECK(!fs::exists(importSidecarPath(asset)));
    // Y guardar el defecto sin fichero previo no es un error.
    CHECK(saveTextureImportSettings(asset, TextureImportSettings{}, &err));
}

static void test_broken_json_is_default_with_warning()
{
    const fs::path d = makeDir();
    const fs::path asset = d / "roto.png";
    writeText(importSidecarPath(asset), "{ esto no es json");
    std::string warning;
    CHECK(isDefault(loadTextureImportSettings(asset, &warning)));
    CHECK(!warning.empty());
}

static void test_wrong_version_type_and_shape_are_default()
{
    const fs::path d = makeDir();
    const struct { const char* name; const char* json; } bad[] = {
        { "v2.png",    R"({"version":2,"type":"texture","colorSpace":"linear","mipmaps":true})" },
        { "audio.png", R"({"version":1,"type":"audio","colorSpace":"linear","mipmaps":true})"   },
        { "notype.png",R"({"version":1,"colorSpace":"linear","mipmaps":true})"                  },
        { "array.png", R"([1,2,3])"                                                              },
        { "verstr.png",R"({"version":"1","type":"texture","colorSpace":"linear"})"              },
    };
    for (const auto& b : bad)
    {
        writeText(importSidecarPath(d / b.name), b.json);
        std::string warning;
        CHECK(isDefault(loadTextureImportSettings(d / b.name, &warning)));
        CHECK(!warning.empty());
    }
}

static void test_unknown_field_values_keep_the_good_field()
{
    const fs::path d = makeDir();
    writeText(importSidecarPath(d / "a.png"),
              R"({"version":1,"type":"texture","colorSpace":"hdr","mipmaps":true})");
    std::string w1;
    const TextureImportSettings a = loadTextureImportSettings(d / "a.png", &w1);
    CHECK(a.colorSpace == ColorSpaceOverride::Auto);   // desconocido -> auto
    CHECK(a.mipmaps);                                   // el otro campo sobrevive
    CHECK(!w1.empty());

    writeText(importSidecarPath(d / "b.png"),
              R"({"version":1,"type":"texture","colorSpace":"srgb","mipmaps":"si"})");
    std::string w2;
    const TextureImportSettings b = loadTextureImportSettings(d / "b.png", &w2);
    CHECK(b.colorSpace == ColorSpaceOverride::Srgb);
    CHECK(!b.mipmaps);
    CHECK(!w2.empty());
}

// Review Focus 1: entradas hostiles.
static void test_hostile_sidecars_do_not_throw_or_blow_the_stack()
{
    const fs::path d = makeDir();
    // 200 KB de '[': anidado a 200000 niveles. Un parseo recursivo sin tope
    // reventaria la pila; el tope de tamano lo corta antes de parsear.
    writeText(importSidecarPath(d / "nested.png"), std::string(200 * 1024, '['));
    std::string warning;
    CHECK(isDefault(loadTextureImportSettings(d / "nested.png", &warning)));
    CHECK(!warning.empty());

    // 5 MB de basura.
    writeText(importSidecarPath(d / "big.png"), std::string(5 * 1024 * 1024, 'x'));
    warning.clear();
    CHECK(isDefault(loadTextureImportSettings(d / "big.png", &warning)));
    CHECK(!warning.empty());

    // Un DIRECTORIO con el nombre del sidecar: no es un fichero regular.
    fs::create_directories(importSidecarPath(d / "dir.png"));
    CHECK(isDefault(loadTextureImportSettings(d / "dir.png")));

    // Fichero vacio.
    writeText(importSidecarPath(d / "empty.png"), "");
    CHECK(isDefault(loadTextureImportSettings(d / "empty.png")));
}

static void test_unicode_path()
{
    const fs::path d = makeDir() / fs::path(u8"ñandú");
    std::error_code ec;
    fs::create_directories(d, ec);
    const fs::path asset = d / fs::path(u8"tabló.png");
    TextureImportSettings in;
    in.mipmaps = true;
    std::string err;
    CHECK(saveTextureImportSettings(asset, in, &err));
    CHECK(loadTextureImportSettings(asset) == in);
}

static void test_save_into_missing_folder_reports_error()
{
    const fs::path d = makeDir();
    TextureImportSettings in;
    in.mipmaps = true;
    std::string err;
    CHECK(!saveTextureImportSettings(d / "no_existe" / "x.png", in, &err));
    CHECK(!err.empty());
}

static void test_move_copy_remove_sidecar()
{
    const fs::path d = makeDir();
    TextureImportSettings s;
    s.mipmaps = true;
    std::string err;

    // Sin sidecar: mover y copiar son un no-op que va bien.
    CHECK(moveImportSidecar(d / "a.png", d / "b.png", &err));
    CHECK(copyImportSidecar(d / "a.png", d / "c.png", &err));
    CHECK(!fs::exists(importSidecarPath(d / "b.png")));

    CHECK(saveTextureImportSettings(d / "a.png", s, &err));
    CHECK(copyImportSidecar(d / "a.png", d / "c.png", &err));
    CHECK(loadTextureImportSettings(d / "c.png") == s);
    CHECK(fs::exists(importSidecarPath(d / "a.png")));           // copiar no quita el original

    CHECK(moveImportSidecar(d / "a.png", d / "b.png", &err));
    CHECK(!fs::exists(importSidecarPath(d / "a.png")));
    CHECK(loadTextureImportSettings(d / "b.png") == s);

    removeImportSidecar(d / "b.png");
    CHECK(!fs::exists(importSidecarPath(d / "b.png")));
    removeImportSidecar(d / "b.png");                             // ya no hay: no pasa nada
}

// Review Focus 4: si el destino YA tiene sidecar, hay conflicto y no se pisa.
static void test_sidecar_conflict_detection()
{
    const fs::path d = makeDir();
    TextureImportSettings s;
    s.mipmaps = true;
    std::string err;
    CHECK(!importSidecarConflict(d / "a.png", d / "b.png"));
    CHECK(saveTextureImportSettings(d / "a.png", s, &err));
    CHECK(!importSidecarConflict(d / "a.png", d / "b.png"));      // solo el origen
    CHECK(saveTextureImportSettings(d / "b.png", s, &err));
    CHECK(importSidecarConflict(d / "a.png", d / "b.png"));
}

int main()
{
    test_move_copy_remove_sidecar();
    test_sidecar_conflict_detection();
    test_sidecar_path_and_detection();
    test_missing_file_is_default_without_warning();
    test_roundtrip_and_default_removes_sidecar();
    test_broken_json_is_default_with_warning();
    test_wrong_version_type_and_shape_are_default();
    test_unknown_field_values_keep_the_good_field();
    test_hostile_sidecars_do_not_throw_or_blow_the_stack();
    test_unicode_path();
    test_save_into_missing_folder_reports_error();

    if (g_failures == 0) std::printf("ALL IMPORT SETTINGS TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
