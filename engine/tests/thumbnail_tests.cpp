// Tests headless de las miniaturas del Content Browser (sin GPU ni ImGui).
// Plain main + CHECK, mismo patron que content_browser_tests.cpp.
#include "DonTopo/Editor/Thumbnail.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <system_error>
#include <vector>

using namespace DonTopo;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

using Rgba = std::array<uint8_t, 4>;

// TGA sin comprimir de 32 bits, origen arriba a la izquierda: el formato mas
// simple que stb_image lee con alfa, y no hace falta stb_image_write.
static void writeTga(const fs::path& p, int w, int h, const std::function<Rgba(int, int)>& pixel)
{
    std::ofstream f(p, std::ios::binary);
    uint8_t hdr[18] = {};
    hdr[2]  = 2;                               // truecolor sin comprimir
    hdr[12] = static_cast<uint8_t>(w & 0xFF);
    hdr[13] = static_cast<uint8_t>((w >> 8) & 0xFF);
    hdr[14] = static_cast<uint8_t>(h & 0xFF);
    hdr[15] = static_cast<uint8_t>((h >> 8) & 0xFF);
    hdr[16] = 32;                              // bits por pixel
    hdr[17] = 0x28;                            // origen arriba-izquierda, 8 bits de alfa
    f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const Rgba c = pixel(x, y);
            const uint8_t bgra[4] = { c[2], c[1], c[0], c[3] };
            f.write(reinterpret_cast<const char*>(bgra), 4);
        }
}

static Rgba pixelAt(const ThumbnailResult& r, uint32_t x, uint32_t y)
{
    const size_t i = (static_cast<size_t>(y) * kThumbCell + x) * 4;
    return { r.rgba[i], r.rgba[i + 1], r.rgba[i + 2], r.rgba[i + 3] };
}

static bool isRed(const Rgba& c)   { return c[0] == 255 && c[1] == 0 && c[2] == 0 && c[3] == 255; }
static bool isBlue(const Rgba& c)  { return c[0] == 0 && c[1] == 0 && c[2] == 255 && c[3] == 255; }
static bool isClear(const Rgba& c) { return c[3] == 0; }

static const Rgba kRed  = { 255, 0, 0, 255 };
static const Rgba kBlue = { 0, 0, 255, 255 };

static fs::path makeDir()
{
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / "dt_thumbnail_test";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

// Apaisada 128x64: se reduce a 64x32 y se centra (16 filas transparentes arriba y abajo).
static void test_landscape_is_letterboxed(const fs::path& dir)
{
    writeTga(dir / "land.tga", 128, 64, [](int, int) { return kRed; });
    ThumbnailResult r = makeThumbnail(dir / "land.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    CHECK(r.rgba.size() == static_cast<size_t>(kThumbCell) * kThumbCell * 4);
    if (r.rgba.size() != static_cast<size_t>(kThumbCell) * kThumbCell * 4) return;
    CHECK(isRed(pixelAt(r, 32, 32)));
    CHECK(isRed(pixelAt(r, 0, 16)));
    CHECK(isRed(pixelAt(r, 63, 47)));
    CHECK(isClear(pixelAt(r, 32, 15)));
    CHECK(isClear(pixelAt(r, 32, 48)));
}

// Vertical 32x128: escala 0.5 -> 16x64, centrada en horizontal.
static void test_portrait_is_pillarboxed(const fs::path& dir)
{
    writeTga(dir / "port.tga", 32, 128, [](int, int) { return kRed; });
    ThumbnailResult r = makeThumbnail(dir / "port.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    CHECK(isRed(pixelAt(r, 24, 0)));
    CHECK(isRed(pixelAt(r, 39, 63)));
    CHECK(isClear(pixelAt(r, 23, 32)));
    CHECK(isClear(pixelAt(r, 40, 32)));
}

// Mas pequena que la casilla: NO se amplia, se centra tal cual.
static void test_small_image_is_not_upscaled(const fs::path& dir)
{
    writeTga(dir / "small.tga", 16, 16, [](int, int) { return kRed; });
    ThumbnailResult r = makeThumbnail(dir / "small.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    CHECK(isRed(pixelAt(r, 24, 24)));
    CHECK(isRed(pixelAt(r, 39, 39)));
    CHECK(isClear(pixelAt(r, 23, 24)));
    CHECK(isClear(pixelAt(r, 40, 40)));
}

// Exactamente del tamano de la casilla: pasa sin tocar.
static void test_exact_size_fills_the_tile(const fs::path& dir)
{
    writeTga(dir / "exact.tga", 64, 64, [](int, int) { return kBlue; });
    ThumbnailResult r = makeThumbnail(dir / "exact.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    CHECK(isBlue(pixelAt(r, 0, 0)));
    CHECK(isBlue(pixelAt(r, 63, 63)));
}

// Reduccion 2:1 con caja: mitad izquierda roja, derecha azul, sin mezcla en el centro.
static void test_downscale_keeps_the_halves(const fs::path& dir)
{
    writeTga(dir / "halves.tga", 128, 128, [](int x, int) { return x < 64 ? kRed : kBlue; });
    ThumbnailResult r = makeThumbnail(dir / "halves.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    CHECK(isRed(pixelAt(r, 10, 32)));
    CHECK(isBlue(pixelAt(r, 53, 32)));
}

// Alfa 128: el color NO debe oscurecerse (promedio ponderado por alfa); sin eso
// aparece un halo oscuro en los bordes de cualquier sprite recortado.
static void test_alpha_does_not_darken_color(const fs::path& dir)
{
    writeTga(dir / "alpha.tga", 128, 128, [](int, int) { return Rgba{ 0, 255, 0, 128 }; });
    ThumbnailResult r = makeThumbnail(dir / "alpha.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    const Rgba c = pixelAt(r, 32, 32);
    CHECK(c[0] == 0);
    CHECK(c[1] == 255);
    CHECK(c[2] == 0);
    CHECK(c[3] == 128);
}

// Alfa a cero en una zona: el color de esos pixeles es irrelevante pero no debe
// contaminar a los vecinos opacos al promediar.
static void test_transparent_neighbours_do_not_bleed(const fs::path& dir)
{
    writeTga(dir / "edge.tga", 128, 128, [](int x, int) {
        return x < 64 ? Rgba{ 255, 0, 0, 255 } : Rgba{ 0, 255, 0, 0 };
    });
    ThumbnailResult r = makeThumbnail(dir / "edge.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    CHECK(isRed(pixelAt(r, 31, 32)));
    CHECK(pixelAt(r, 32, 32)[3] == 0);
}

static void test_unreadable_files(const fs::path& dir)
{
    CHECK(makeThumbnail(dir / "no_existe.png").status == ThumbnailStatus::Unreadable);

    std::ofstream(dir / "corrupta.png", std::ios::binary) << "esto no es una imagen, solo texto";
    CHECK(makeThumbnail(dir / "corrupta.png").status == ThumbnailStatus::Unreadable);

    std::ofstream(dir / "vacia.png", std::ios::binary).flush();
    CHECK(makeThumbnail(dir / "vacia.png").status == ThumbnailStatus::Unreadable);
}

// Solo la cabecera de un TGA de 20000x20000 (400 MP): se rechaza por las
// dimensiones, SIN decodificar (no hay cuerpo que decodificar).
static void test_huge_image_is_rejected_without_decoding(const fs::path& dir)
{
    {
        std::ofstream f(dir / "enorme.tga", std::ios::binary);
        uint8_t hdr[18] = {};
        hdr[2] = 2; hdr[12] = 20000 & 0xFF; hdr[13] = 20000 >> 8;
        hdr[14] = 20000 & 0xFF; hdr[15] = 20000 >> 8; hdr[16] = 32; hdr[17] = 0x28;
        f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    }
    ThumbnailResult r = makeThumbnail(dir / "enorme.tga");
    CHECK(r.status == ThumbnailStatus::TooLarge);
    CHECK(r.rgba.empty());
}

static bool nearF(float a, float b) { return std::fabs(a - b) < 1e-6f; }

// thumbnailUv: casilla -> rect UV con medio texel de margen por lado.
static void test_thumbnail_uv()
{
    const float size = static_cast<float>(kThumbAtlasSize);

    const UvRect first = thumbnailUv(0);
    CHECK(nearF(first.u0, 0.5f / size));
    CHECK(nearF(first.v0, 0.5f / size));
    CHECK(nearF(first.u1, 63.5f / size));
    CHECK(nearF(first.v1, 63.5f / size));

    const UvRect second = thumbnailUv(1);              // siguiente columna, misma fila
    CHECK(nearF(second.u0, 64.5f / size));
    CHECK(nearF(second.v0, 0.5f / size));

    const UvRect row1 = thumbnailUv(kThumbAtlasCells); // primera columna, segunda fila
    CHECK(nearF(row1.u0, 0.5f / size));
    CHECK(nearF(row1.v0, 64.5f / size));

    const UvRect last = thumbnailUv(kThumbSlotCount - 1);
    CHECK(nearF(last.u1, 2047.5f / size));
    CHECK(nearF(last.v1, 2047.5f / size));
}

int main()
{
    fs::path dir = makeDir();
    test_landscape_is_letterboxed(dir);
    test_portrait_is_pillarboxed(dir);
    test_small_image_is_not_upscaled(dir);
    test_exact_size_fills_the_tile(dir);
    test_downscale_keeps_the_halves(dir);
    test_alpha_does_not_darken_color(dir);
    test_transparent_neighbours_do_not_bleed(dir);
    test_unreadable_files(dir);
    test_huge_image_is_rejected_without_decoding(dir);
    test_thumbnail_uv();
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (g_failures == 0) std::printf("ALL THUMBNAIL TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
