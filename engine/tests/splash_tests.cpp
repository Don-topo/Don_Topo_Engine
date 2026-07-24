// Test headless del splash: la carga del PNG (sin GPU) y el calculo puro del
// alpha por fase. La parte Vulkan de SplashScreen NO se testea aqui (requiere
// device, fragil sin GPU): va a verificacion manual.
#include "DonTopo/Renderer/SplashScreen.h"
#include "SplashDriver.h"

#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <cmath>

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

// SplashDriver tests: calculo puro del alpha por fase.
static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

static void test_fade_in_rises()
{
    SplashTimings t; // fadeIn=0.3
    // A mitad del fade-in, alpha ~0.5, aun no crossfade ni done.
    SplashState s = splashStateAt(t, 0.15f, /*loadingDone=*/false, 0.0f);
    CHECK(near(s.alpha, 0.5f));
    CHECK(!s.crossfading);
    CHECK(!s.done);
}

static void test_hold_while_loading()
{
    SplashTimings t;
    // Tras el fade-in, cargando todavia: alpha 1, sin crossfade.
    SplashState s = splashStateAt(t, 1.0f, /*loadingDone=*/false, 0.0f);
    CHECK(near(s.alpha, 1.0f));
    CHECK(!s.crossfading);
    CHECK(!s.done);
}

static void test_min_total_respected()
{
    SplashTimings t; // minTotal=1.5
    // Carga termino pronto (a 0.5s) pero aun no se alcanzo minTotal: sigue en
    // hold a alpha 1, sin empezar el fade-out.
    SplashState s = splashStateAt(t, 1.0f, /*loadingDone=*/true, 0.5f);
    CHECK(near(s.alpha, 1.0f));
    CHECK(!s.crossfading);
    CHECK(!s.done);
}

static void test_crossfade_after_load_and_min()
{
    SplashTimings t; // minTotal=1.5, fadeOut=0.3
    // Carga termino a 1.0s; fade-out empieza en max(1.0,1.5)=1.5. A 1.65s
    // (mitad del fade-out) alpha ~0.5 y crossfading.
    SplashState s = splashStateAt(t, 1.65f, /*loadingDone=*/true, 1.0f);
    CHECK(near(s.alpha, 0.5f));
    CHECK(s.crossfading);
    CHECK(!s.done);
}

static void test_done_after_fade_out()
{
    SplashTimings t;
    // Fade-out completo (1.5 + 0.3 = 1.8): done, alpha 0.
    SplashState s = splashStateAt(t, 2.0f, /*loadingDone=*/true, 1.0f);
    CHECK(near(s.alpha, 0.0f));
    CHECK(s.done);
}

int main()
{
    test_load_valid_png();
    test_missing_png_returns_false();
    test_fade_in_rises();
    test_hold_while_loading();
    test_min_total_respected();
    test_crossfade_after_load_and_min();
    test_done_after_fade_out();
    if (g_failures == 0) std::printf("ALL SPLASH TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
