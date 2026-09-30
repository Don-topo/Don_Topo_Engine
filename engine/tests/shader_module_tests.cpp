// Headless test of readSpvFile, the .spv read shared by the 16 files that
// used to carry their own copy (H11).
//
// Creating the VkShaderModule needs a device, so that is not tested here. What
// is tested is the ONE thing the repeated copy did not do and the reason for
// unifying: validating the size before handing it to Vulkan. `pCode` is a
// `const uint32_t*`, so a truncated file (a half-finished shader compilation, a
// half-written .spv) made vkCreateShaderModule read outside the buffer.
#include "DonTopo/Renderer/ShaderModule.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static std::filesystem::path writeBytes(const char* name, size_t count)
{
    const std::filesystem::path p = std::filesystem::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    const std::vector<char> bytes(count, 0x07);
    f.write(bytes.data(), (std::streamsize)bytes.size());
    return p;
}

static bool lanza(const std::filesystem::path& p)
{
    try {
        readSpvFile(p.string());
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

// The usual case: a well-formed .spv is read in full.
static void test_lee_un_spv_valido()
{
    const std::filesystem::path p = writeBytes("dt_test_ok.spv", 16);
    std::vector<char> code;
    try {
        code = readSpvFile(p.string());
    } catch (const std::exception& e) {
        std::printf("FAIL: lanzo con un fichero valido: %s\n", e.what());
        ++g_failures;
    }
    CHECK(code.size() == 16u);
    std::filesystem::remove(p);
}

// A truncated .spv is NOT a valid module: SPIR-V is 32-bit words.
// Before, this reached vkCreateShaderModule as is, which reads `codeSize` bytes
// as uint32_t and ran past the end of the vector.
static void test_tamano_no_multiplo_de_cuatro()
{
    const std::filesystem::path p = writeBytes("dt_test_trunc.spv", 13);
    CHECK(lanza(p));
    std::filesystem::remove(p);
}

// Zero-byte file: left behind by a shader build interrupted halfway.
static void test_fichero_vacio()
{
    const std::filesystem::path p = writeBytes("dt_test_empty.spv", 0);
    CHECK(lanza(p));
    std::filesystem::remove(p);
}

// The one that already worked, and that must be kept: the message carries the
// PATH. It was the only thing that told apart the four variants scattered around
// the engine, and without it, "failed to open shader" does not say which one.
static void test_fichero_que_no_existe_nombra_la_ruta()
{
    const std::string ruta = "shaders/no_existe_jamas.spv";
    bool lanzo = false;
    bool nombra = false;
    try {
        readSpvFile(ruta);
    } catch (const std::exception& e) {
        lanzo  = true;
        nombra = std::string(e.what()).find(ruta) != std::string::npos;
    }
    CHECK(lanzo);
    CHECK(nombra);
}

int main()
{
    test_lee_un_spv_valido();
    test_tamano_no_multiplo_de_cuatro();
    test_fichero_vacio();
    test_fichero_que_no_existe_nombra_la_ruta();

    if (g_failures == 0) std::printf("ALL SHADER MODULE TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
