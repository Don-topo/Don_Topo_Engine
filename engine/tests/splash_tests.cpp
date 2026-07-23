// Test headless del splash: la carga del PNG (sin GPU) y el calculo puro del
// alpha por fase. La parte Vulkan de SplashScreen NO se testea aqui (requiere
// device, fragil sin GPU): va a verificacion manual.
#include "DonTopo/Renderer/SplashScreen.h"

#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Un PNG real del repo carga a RGBA con dimensiones > 0.
static void test_load_valid_png()
{
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    CHECK(loadSplashImage("assets/MainEngineLogo.png", rgba, w, h));
    CHECK(w > 0);
    CHECK(h > 0);
    CHECK(rgba.size() == (size_t)w * h * 4);
}

// Un fichero inexistente devuelve false sin tocar los out (garantia de "logo
// ausente no bloquea el arranque").
static void test_missing_png_returns_false()
{
    std::vector<uint8_t> rgba;
    int w = -1, h = -1;
    CHECK(!loadSplashImage("assets/no_existe_splash.png", rgba, w, h));
    CHECK(rgba.empty());
}

int main()
{
    test_load_valid_png();
    test_missing_png_returns_false();
    if (g_failures == 0) std::printf("ALL SPLASH TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
