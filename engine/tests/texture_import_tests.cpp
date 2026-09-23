// Test headless de TextureImport (cadena de mips, resolucion sRGB, claves) y de
// como decodeMaterialTexture lee los ajustes. Sin GPU. Desde la raiz del repo.
#include "DonTopo/Renderer/TextureImport.h"
#include "DonTopo/Renderer/SharedTextureCache.h"
#include "DonTopo/Renderer/MaterialTextureSource.h"
#include "DonTopo/Core/ImportSettings.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

using namespace DonTopo;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static void test_mip_level_count()
{
    CHECK(mipLevelCount(0, 0) == 1);
    CHECK(mipLevelCount(1, 1) == 1);
    CHECK(mipLevelCount(2, 2) == 2);
    CHECK(mipLevelCount(4, 4) == 3);
    CHECK(mipLevelCount(5, 3) == 3);     // 5x3 -> 2x1 -> 1x1
    CHECK(mipLevelCount(1, 8) == 4);     // 1x8 -> 1x4 -> 1x2 -> 1x1
    CHECK(mipLevelCount(256, 256) == 9);
    CHECK(mipLevelCount(300, 100) == 9);
}

// Review Focus 2.
static void test_mip_chain_shapes()
{
    std::vector<uint8_t> px(4 * 4 * 4);
    for (size_t i = 0; i < 16; ++i) { px[i * 4 + 0] = 255; px[i * 4 + 1] = 0; px[i * 4 + 2] = 0; px[i * 4 + 3] = 255; }

    auto chain = buildMipChain(px.data(), 4, 4);
    CHECK(chain.size() == 2);
    CHECK(chain.size() == 2 && chain[0].w == 2 && chain[0].h == 2 && chain[0].rgba.size() == 2 * 2 * 4);
    CHECK(chain.size() == 2 && chain[1].w == 1 && chain[1].h == 1 && chain[1].rgba.size() == 4);
    for (const TextureMip& m : chain)
        for (size_t i = 0; i < m.rgba.size(); i += 4)
            CHECK(m.rgba[i] == 255 && m.rgba[i + 1] == 0 && m.rgba[i + 2] == 0 && m.rgba[i + 3] == 255);

    // 1x1: sin niveles extra.
    const uint8_t one[4] = { 1, 2, 3, 255 };
    CHECK(buildMipChain(one, 1, 1).empty());

    // No potencia de 2 y tiras 1xN.
    std::vector<uint8_t> a(5 * 3 * 4, 200);
    chain = buildMipChain(a.data(), 5, 3);
    CHECK(chain.size() == 2);
    CHECK(chain.size() == 2 && chain[0].w == 2 && chain[0].h == 1);
    CHECK(chain.size() == 2 && chain[1].w == 1 && chain[1].h == 1);

    std::vector<uint8_t> strip(1 * 8 * 4, 90);
    chain = buildMipChain(strip.data(), 1, 8);
    CHECK(chain.size() == 3);
    CHECK(chain.size() == 3 && chain[0].w == 1 && chain[0].h == 4);
    CHECK(chain.size() == 3 && chain[1].w == 1 && chain[1].h == 2);
    CHECK(chain.size() == 3 && chain[2].w == 1 && chain[2].h == 1);

    // Entrada invalida: vacio, sin lanzar.
    CHECK(buildMipChain(nullptr, 4, 4).empty());
    CHECK(buildMipChain(px.data(), 0, 4).empty());
}

// El nivel 0 no se toca y los pixeles transparentes no oscurecen a los opacos.
static void test_mip_chain_alpha_weighted_and_input_untouched()
{
    // 2x1: un pixel rojo opaco y uno TRANSPARENTE negro.
    std::vector<uint8_t> px = { 255, 0, 0, 255,   0, 0, 0, 0 };
    const std::vector<uint8_t> before = px;
    const auto chain = buildMipChain(px.data(), 2, 1);
    CHECK(px == before);
    CHECK(chain.size() == 1);
    CHECK(chain.size() == 1 && chain[0].w == 1 && chain[0].h == 1);
    CHECK(chain.size() == 1 && chain[0].rgba[0] == 255);    // el rojo no se oscurece a 128
    CHECK(chain.size() == 1 && chain[0].rgba[3] == 128);    // el alfa si es la media
}

static void test_resolve_srgb_table()
{
    const TextureKind kinds[] = { TextureKind::BaseColor, TextureKind::Normal, TextureKind::Orm };
    for (TextureKind k : kinds)
    {
        CHECK(resolveSrgb(k, ColorSpaceOverride::Srgb));
        CHECK(!resolveSrgb(k, ColorSpaceOverride::Linear));
    }
    CHECK(resolveSrgb(TextureKind::BaseColor, ColorSpaceOverride::Auto));
    CHECK(!resolveSrgb(TextureKind::Normal,   ColorSpaceOverride::Auto));
    CHECK(!resolveSrgb(TextureKind::Orm,      ColorSpaceOverride::Auto));
}

static fs::path makeDir()
{
    std::error_code ec;
    fs::path d = fs::temp_directory_path(ec) / "dt_texture_import_test";
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

// Review Focus 3.
static void test_texture_key_suffix_and_key()
{
    const fs::path d = makeDir();
    const std::string a = (d / "a.png").string();

    CHECK(textureKeySuffix("").empty());
    CHECK(textureKeySuffix(a).empty());                       // sin sidecar: la clave de hoy
    CHECK(makeTextureKey(a, {}, TextureKind::BaseColor, textureKeySuffix(a)) ==
          makeTextureKey(a, {}, TextureKind::BaseColor));     // y no cambia

    TextureImportSettings lin;
    lin.colorSpace = ColorSpaceOverride::Linear;
    std::string err;
    CHECK(saveTextureImportSettings(d / "a.png", lin, &err));
    const std::string sufLin = textureKeySuffix(a);
    CHECK(!sufLin.empty());

    TextureImportSettings mips;
    mips.mipmaps = true;
    CHECK(saveTextureImportSettings(d / "a.png", mips, &err));
    const std::string sufMips = textureKeySuffix(a);
    CHECK(!sufMips.empty());
    CHECK(sufMips != sufLin);

    CHECK(makeTextureKey(a, {}, TextureKind::BaseColor, sufLin) !=
          makeTextureKey(a, {}, TextureKind::BaseColor, sufMips));
    CHECK(makeTextureKey(a, {}, TextureKind::BaseColor, sufLin) !=
          makeTextureKey(a, {}, TextureKind::BaseColor));

    // Sin ruta, el sufijo no cuenta (una textura embebida no tiene sidecar).
    const std::vector<uint8_t> emb = { 1, 2, 3 };
    CHECK(makeTextureKey("", emb, TextureKind::BaseColor, "#s") ==
          makeTextureKey("", emb, TextureKind::BaseColor));
}

static void writeTga(const fs::path& p, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    std::ofstream o(p, std::ios::binary);
    const uint8_t hd[18] = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                             static_cast<uint8_t>(w & 255), static_cast<uint8_t>(w >> 8),
                             static_cast<uint8_t>(h & 255), static_cast<uint8_t>(h >> 8), 32, 0x28 };
    o.write(reinterpret_cast<const char*>(hd), 18);
    for (int i = 0; i < w * h; ++i) { o.put(static_cast<char>(b)); o.put(static_cast<char>(g)); o.put(static_cast<char>(r)); o.put(static_cast<char>(a)); }
}

static void test_decode_without_sidecar_is_unchanged()
{
    const fs::path d = makeDir();
    writeTga(d / "t.tga", 8, 4, 10, 20, 30, 255);
    const DecodedTexture tex = decodeMaterialTexture((d / "t.tga").string(), {});
    CHECK(tex && tex.w == 8 && tex.h == 4);
    CHECK(tex.colorSpace == ColorSpaceOverride::Auto);
    CHECK(tex.mips.empty());
}

static void test_decode_with_sidecar_carries_settings_and_mips()
{
    const fs::path d = makeDir();
    writeTga(d / "t.tga", 8, 4, 10, 20, 30, 255);
    TextureImportSettings s;
    s.colorSpace = ColorSpaceOverride::Linear;
    s.mipmaps    = true;
    std::string err;
    CHECK(saveTextureImportSettings(d / "t.tga", s, &err));

    const DecodedTexture tex = decodeMaterialTexture((d / "t.tga").string(), {});
    CHECK(tex);
    CHECK(tex.colorSpace == ColorSpaceOverride::Linear);
    CHECK(tex.mips.size() == mipLevelCount(8, 4) - 1);          // 8x4 -> 4x2 -> 2x1 -> 1x1
    CHECK(!tex.mips.empty() && tex.mips.front().w == 4 && tex.mips.front().h == 2);
    CHECK(!tex.mips.empty() && tex.mips.back().w == 1 && tex.mips.back().h == 1);
}

static void test_decode_1x1_with_mipmaps_has_no_extra_levels()
{
    const fs::path d = makeDir();
    writeTga(d / "one.tga", 1, 1, 1, 2, 3, 255);
    TextureImportSettings s;
    s.mipmaps = true;
    std::string err;
    CHECK(saveTextureImportSettings(d / "one.tga", s, &err));
    const DecodedTexture tex = decodeMaterialTexture((d / "one.tga").string(), {});
    CHECK(tex && tex.mips.empty());
}

// Un sidecar roto no impide cargar la textura: defecto.
static void test_decode_with_broken_sidecar_falls_back_to_default()
{
    const fs::path d = makeDir();
    writeTga(d / "t.tga", 4, 4, 1, 2, 3, 255);
    std::ofstream(importSidecarPath(d / "t.tga")) << "{ roto";
    const DecodedTexture tex = decodeMaterialTexture((d / "t.tga").string(), {});
    CHECK(tex && tex.colorSpace == ColorSpaceOverride::Auto && tex.mips.empty());
}

// Las embebidas del FBX no tienen sidecar: siempre el defecto.
static void test_decode_embedded_ignores_sidecars()
{
    const fs::path d = makeDir();
    writeTga(d / "t.tga", 4, 4, 1, 2, 3, 255);
    std::ifstream in(d / "t.tga", std::ios::binary);
    const std::vector<uint8_t> bytes{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    TextureImportSettings s;
    s.mipmaps = true;
    std::string err;
    CHECK(saveTextureImportSettings(d / "t.tga", s, &err));
    const DecodedTexture tex = decodeMaterialTexture("", bytes);
    CHECK(tex && tex.colorSpace == ColorSpaceOverride::Auto && tex.mips.empty());
}

static std::string readAll(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
static size_t countOf(const std::string& hay, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size())) ++n;
    return n;
}

// Politica: el formato de una textura de material lo decide resolveSrgb, no un
// literal en cada uploader. Si alguien vuelve a hardcodear el formato de un slot,
// los ajustes de importacion dejan de respetarse en silencio.
static void test_policy_vulkan_material_format_comes_from_resolveSrgb()
{
    const std::string res = readAll("engine/src/Renderer/GpuResources.cpp");
    const std::string rnd = readAll("engine/src/Renderer/Renderer.cpp");
    CHECK(!res.empty() && !rnd.empty());
    CHECK(countOf(res, "resolveSrgb(") >= 2);                       // createTextureImage y createNormalMapImage
    CHECK(countOf(rnd, "createTextureImageView(obj.textureImage, obj.textureView);") == 0);
    CHECK(countOf(rnd, "createTextureImageView(gpu.textureImage, gpu.textureView);") == 0);
    CHECK(countOf(rnd, "createTextureImageView(mgfx.textureImage, mgfx.textureView);") == 0);
    CHECK(countOf(rnd, "normalView, VK_FORMAT_R8G8B8A8_UNORM)") == 0);
    CHECK(countOf(rnd, "ormView, VK_FORMAT_R8G8B8A8_UNORM)") == 0);
}

int main()
{
    test_policy_vulkan_material_format_comes_from_resolveSrgb();
    test_decode_without_sidecar_is_unchanged();
    test_decode_with_sidecar_carries_settings_and_mips();
    test_decode_1x1_with_mipmaps_has_no_extra_levels();
    test_decode_with_broken_sidecar_falls_back_to_default();
    test_decode_embedded_ignores_sidecars();
    test_mip_level_count();
    test_mip_chain_shapes();
    test_mip_chain_alpha_weighted_and_input_untouched();
    test_resolve_srgb_table();
    test_texture_key_suffix_and_key();

    if (g_failures == 0) std::printf("ALL TEXTURE IMPORT TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
