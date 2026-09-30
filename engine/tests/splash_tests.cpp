// Headless splash test: the PNG load (no GPU) and the pure computation of the
// alpha per phase. The Vulkan part of SplashScreen is NOT tested here (it needs a
// device, fragile without a GPU): it goes to manual verification.
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

// A real PNG from the repo loads as RGBA with dimensions > 0.
static void test_load_valid_png()
{
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    CHECK(loadSplashImage("assets/MainEngineLogo.png", rgba, w, h));
    CHECK(w > 0);
    CHECK(h > 0);
    CHECK(rgba.size() == (size_t)w * h * 4);
}

// A nonexistent file returns false without touching the outputs (guarantee that a
// "missing logo does not block startup").
static void test_missing_png_returns_false()
{
    std::vector<uint8_t> rgba;
    int w = -1, h = -1;
    CHECK(!loadSplashImage("assets/no_existe_splash.png", rgba, w, h));
    CHECK(rgba.empty());
}

// SplashDriver tests: pure computation of the alpha per phase.
static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

static void test_fade_in_rises()
{
    SplashTimings t; // fadeIn=0.3
    // Halfway through the fade-in, alpha ~0.5, no crossfade or done yet.
    SplashState s = splashStateAt(t, 0.15f, /*loadingDone=*/false, 0.0f);
    CHECK(near(s.alpha, 0.5f));
    CHECK(!s.crossfading);
    CHECK(!s.done);
}

static void test_hold_while_loading()
{
    SplashTimings t;
    // After the fade-in, still loading: alpha 1, no crossfade.
    SplashState s = splashStateAt(t, 1.0f, /*loadingDone=*/false, 0.0f);
    CHECK(near(s.alpha, 1.0f));
    CHECK(!s.crossfading);
    CHECK(!s.done);
}

static void test_min_total_respected()
{
    SplashTimings t; // minTotal=1.5
    // Loading finished early (at 0.5s) but minTotal has not been reached yet: it stays
    // in hold at alpha 1, without starting the fade-out.
    SplashState s = splashStateAt(t, 1.0f, /*loadingDone=*/true, 0.5f);
    CHECK(near(s.alpha, 1.0f));
    CHECK(!s.crossfading);
    CHECK(!s.done);
}

static void test_crossfade_after_load_and_min()
{
    SplashTimings t; // minTotal=1.5, fadeOut=0.3
    // Loading finished at 1.0s; the fade-out starts at max(1.0,1.5)=1.5. At 1.65s
    // (halfway through the fade-out) alpha ~0.5 and crossfading.
    SplashState s = splashStateAt(t, 1.65f, /*loadingDone=*/true, 1.0f);
    CHECK(near(s.alpha, 0.5f));
    CHECK(s.crossfading);
    CHECK(!s.done);
}

static void test_done_after_fade_out()
{
    SplashTimings t;
    // Fade-out complete (1.5 + 0.3 = 1.8): done, alpha 0.
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
