// Headless test of MaterialAsset (no GUI, no GPU). Plain main + CHECK, same
// pattern as import_settings_tests.cpp. It runs from the repo root.
#include "DonTopo/Core/MaterialAsset.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>

using namespace DonTopo;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static fs::path makeDir()
{
    std::error_code ec;
    fs::path d = fs::temp_directory_path(ec) / "dt_material_asset_test";
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}
static void writeText(const fs::path& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; }

static void test_default_is_all_inherit()
{
    CHECK(isDefault(MaterialAsset{}));
    MaterialAsset m; m.albedo = "x";
    CHECK(!isDefault(m));
}

// Finding from the final reviewer: the spec (line 133) asks for a warning for "a
// missing OR unreadable .mat", not only for the unreadable one; without this, a
// .mat deleted outside the editor loaded the scene with the model and without
// explaining why. The cost on the hot path is zero: applyMaterialOverrides
// calls loadMaterialAsset without `warning` (nullptr), so this only costs
// when collectMaterialOverrideWarnings (only on scene load) asks for it.
static void test_missing_file_is_default_with_warning()
{
    const fs::path d = makeDir();
    std::string warning = "x";
    CHECK(isDefault(loadMaterialAsset(d / "no_existe.mat", &warning)));
    CHECK(!warning.empty());
}

static void test_roundtrip_always_writes_the_file()
{
    const fs::path d = makeDir();
    const fs::path mat = d / "rojo.mat";
    std::string err;

    // Saving the default (everything inherit) DOES write the file: unlike the import
    // sidecar, a .mat is a named asset.
    CHECK(saveMaterialAsset(mat, MaterialAsset{}, &err));
    CHECK(fs::exists(mat));
    CHECK(isDefault(loadMaterialAsset(mat)));

    MaterialAsset in;
    in.roughness = 0.25f;
    CHECK(saveMaterialAsset(mat, in, &err));
    const MaterialAsset out = loadMaterialAsset(mat);
    CHECK(out.roughness == 0.25f);
    CHECK(out.albedo.empty() && out.normal.empty() && out.orm.empty());
    CHECK(out.metallic == -1.0f);         // only roughness was set
}

static void test_broken_and_wrong_type_are_default_with_warning()
{
    const fs::path d = makeDir();
    writeText(d / "roto.mat", "{ esto no es json");
    std::string warning;
    CHECK(isDefault(loadMaterialAsset(d / "roto.mat", &warning)));
    CHECK(!warning.empty());

    writeText(d / "textura.mat", R"({"version":1,"type":"texture","metallic":0.5})");
    warning.clear();
    CHECK(isDefault(loadMaterialAsset(d / "textura.mat", &warning)));
    CHECK(!warning.empty());

    writeText(d / "v2.mat", R"({"version":2,"type":"material","metallic":0.5})");
    warning.clear();
    CHECK(isDefault(loadMaterialAsset(d / "v2.mat", &warning)));
    CHECK(!warning.empty());
}

// Review Focus (format): hostile values in metallic/roughness.
static void test_hostile_factor_values_are_clamped_or_inherited()
{
    const fs::path d = makeDir();
    struct Case { const char* name; const char* json; float wantMetallic; };
    const Case cases[] = {
        { "big.mat",  R"({"version":1,"type":"material","metallic":1e30})",   1.0f },
        { "neg.mat",  R"({"version":1,"type":"material","metallic":-1e30})",  0.0f },   // NOT -1: it is clamped to 0, not confused with "inherit"
        { "str.mat",  R"({"version":1,"type":"material","metallic":"alto"})", -1.0f },
        { "nan.mat",  R"({"version":1,"type":"material","metallic":null})",   -1.0f },
    };
    for (const Case& c : cases)
    {
        writeText(d / c.name, c.json);
        std::string warning;
        const MaterialAsset m = loadMaterialAsset(d / c.name, &warning);
        CHECK(m.metallic == c.wantMetallic);
        CHECK(std::isfinite(m.metallic));
        if (c.wantMetallic != -1.0f) CHECK(!warning.empty()) ; // the out-of-range value DOES warn (it was clamped)
    }
    // Hostile sidecar in size, same cap as ImportSettings.
    writeText(d / "huge.mat", std::string(5 * 1024 * 1024, 'x'));
    std::string warning;
    CHECK(isDefault(loadMaterialAsset(d / "huge.mat", &warning)));
    CHECK(!warning.empty());
}

// Texture paths are saved RELATIVE to the .mat folder, and are resolved to
// absolute when read.
static void test_texture_paths_are_relative_to_the_mat_folder()
{
    const fs::path d = makeDir();
    fs::create_directories(d / "textures");
    const fs::path tex = d / "textures" / "piel.png";
    writeText(tex, "png");

    MaterialAsset in;
    in.albedo = tex.string();
    std::string err;
    CHECK(saveMaterialAsset(d / "piel.mat", in, &err));

    // The file on disk stores a relative path (not the test's absolute one).
    std::ifstream raw(d / "piel.mat");
    std::string text{ std::istreambuf_iterator<char>(raw), std::istreambuf_iterator<char>() };
    CHECK(text.find(d.string()) == std::string::npos);
    CHECK(text.find("textures") != std::string::npos);

    const MaterialAsset out = loadMaterialAsset(d / "piel.mat");
    CHECK(fs::equivalent(out.albedo, tex));
}

static void test_unicode_path_and_missing_folder_error()
{
    const fs::path d = makeDir() / fs::path(u8"ñandú");
    std::error_code ec;
    fs::create_directories(d, ec);
    MaterialAsset in; in.metallic = 0.1f;
    std::string err;
    CHECK(saveMaterialAsset(d / fs::path(u8"tabló.mat"), in, &err));
    CHECK(loadMaterialAsset(d / fs::path(u8"tabló.mat")) == in);

    CHECK(!saveMaterialAsset(makeDir() / "no_existe" / "x.mat", in, &err));
    CHECK(!err.empty());
}

int main()
{
    test_default_is_all_inherit();
    test_missing_file_is_default_with_warning();
    test_roundtrip_always_writes_the_file();
    test_broken_and_wrong_type_are_default_with_warning();
    test_hostile_factor_values_are_clamped_or_inherited();
    test_texture_paths_are_relative_to_the_mat_folder();
    test_unicode_path_and_missing_folder_error();

    if (g_failures == 0) std::printf("ALL MATERIAL ASSET TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
