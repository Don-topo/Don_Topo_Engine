// Test headless de readSpvFile, la lectura de un .spv compartida por los 16
// ficheros que antes llevaban su propia copia (H11).
//
// Crear el VkShaderModule necesita device, asi que eso no se prueba aqui. Lo
// que si se prueba es lo UNICO que la copia repetida no hacia y que era el
// motivo de unificar: validar el tamano antes de pasarselo a Vulkan. `pCode` es
// un `const uint32_t*`, asi que un fichero truncado —una compilacion de shaders
// a medias, un .spv a medio escribir— hacia que vkCreateShaderModule leyera
// fuera del buffer.
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

// El caso de siempre: un .spv bien formado se lee entero.
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

// Un .spv truncado NO es un modulo valido: SPIR-V son palabras de 32 bits.
// Antes esto llegaba tal cual a vkCreateShaderModule, que lee `codeSize` bytes
// como uint32_t y se pasaba del final del vector.
static void test_tamano_no_multiplo_de_cuatro()
{
    const std::filesystem::path p = writeBytes("dt_test_trunc.spv", 13);
    CHECK(lanza(p));
    std::filesystem::remove(p);
}

// Fichero de cero bytes: lo deja un build de shaders interrumpido a mitad.
static void test_fichero_vacio()
{
    const std::filesystem::path p = writeBytes("dt_test_empty.spv", 0);
    CHECK(lanza(p));
    std::filesystem::remove(p);
}

// El que ya funcionaba, y que hay que conservar: el mensaje lleva la RUTA. Era
// lo unico que distinguia a las cuatro variantes que habia sueltas por el
// motor, y sin el, "failed to open shader" no dice cual.
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
