// Headless test of the GPU time format (no GUI, no ImGui).
// gpuMsText is pure formatting, so the rule governing the two places where those
// times are drawn can be asserted here in full. Plain main + asserts, consistent
// with frustum_tests.cpp.
//
// What is protected is the distinction between "no measurement" and "costs zero".
// The View menu showed "0.000 ms" for a disabled pass (H57), which reads as "this
// effect is free", the opposite conclusion, and also indistinguishable from a
// pass that really costs nothing.
#include "DonTopo/Editor/GpuTimeFormat.h"

#include <cstdio>
#include <cstring>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static const char* fmt(float ms, char* buf)
{
    return gpuMsText(ms, buf, kGpuMsTextSize);
}

static void test_sin_medida()
{
    char buf[kGpuMsTextSize];

    // The three cases in which the pass has NOT run: disabled, without the two
    // capture frames, or a counter that has not been written yet.
    CHECK(std::strcmp(fmt(0.0f, buf), "--") == 0);
    CHECK(std::strcmp(fmt(-1.0f, buf), "--") == 0);
    CHECK(std::strcmp(fmt(-0.001f, buf), "--") == 0);
}

static void test_con_medida()
{
    char buf[kGpuMsTextSize];

    CHECK(std::strcmp(fmt(0.123f, buf), "0.123") == 0);
    CHECK(std::strcmp(fmt(12.5f, buf), "12.500") == 0);

    // A measurable but very cheap pass is NOT the same as an unmeasured one: it has
    // to come out with its three decimals, not as "--". It is half of the
    // distinction this helper exists to maintain.
    CHECK(std::strcmp(fmt(0.0004f, buf), "0.000") == 0);
    CHECK(std::strcmp(fmt(0.0004f, buf), "--") != 0);
}

// The helper returns the buffer itself so it can be called inside an
// ImGui::Text without an intermediate variable; if it returned anything else,
// both callers would draw garbage.
static void test_devuelve_el_buffer()
{
    char buf[kGpuMsTextSize];
    CHECK(gpuMsText(1.0f, buf, kGpuMsTextSize) == buf);
}

int main()
{
    test_sin_medida();
    test_con_medida();
    test_devuelve_el_buffer();

    if (g_failures == 0) std::printf("gpu_time_format_tests: OK\n");
    else                 std::printf("gpu_time_format_tests: %d FALLOS\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
