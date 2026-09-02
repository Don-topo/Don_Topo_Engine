// Test headless del formato de los tiempos de GPU (sin GUI, sin ImGui).
// gpuMsText es formato puro, asi que la regla que gobierna los dos sitios donde
// se pintan esos tiempos se puede afirmar aqui entera. Plain main + asserts,
// coherente con frustum_tests.cpp.
//
// Lo que se protege es la distincion entre "no hay medida" y "cuesta cero". El
// menu View sacaba "0.000 ms" para un pase apagado (H57), que se lee como «este
// efecto es gratis» — la conclusion contraria, y ademas indistinguible de un
// pase que de verdad no cuesta nada.
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

    // Los tres casos en que el pase NO ha corrido: apagado, sin los dos frames
    // de la captura, o un contador que aun no se ha escrito.
    CHECK(std::strcmp(fmt(0.0f, buf), "--") == 0);
    CHECK(std::strcmp(fmt(-1.0f, buf), "--") == 0);
    CHECK(std::strcmp(fmt(-0.001f, buf), "--") == 0);
}

static void test_con_medida()
{
    char buf[kGpuMsTextSize];

    CHECK(std::strcmp(fmt(0.123f, buf), "0.123") == 0);
    CHECK(std::strcmp(fmt(12.5f, buf), "12.500") == 0);

    // Un pase medible pero baratisimo NO es lo mismo que uno sin medir: tiene
    // que salir con sus tres decimales, no como "--". Es la mitad de la
    // distincion que este helper existe para mantener.
    CHECK(std::strcmp(fmt(0.0004f, buf), "0.000") == 0);
    CHECK(std::strcmp(fmt(0.0004f, buf), "--") != 0);
}

// El helper devuelve el propio buffer para poder llamarlo dentro de un
// ImGui::Text sin variable intermedia; si devolviera otra cosa, los dos
// llamantes pintarian basura.
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
