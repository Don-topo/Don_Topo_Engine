// Test headless del buffer del Log (sin GUI). Plain main + asserts, sin
// framework — mismo patrón que content_browser_tests.cpp.
//
// Qué se cubre y por qué: el panel pinta las filas con ImGuiListClipper, que
// asume que TODAS miden lo mismo. Un mensaje con '\n' dentro (los errores de
// Lua traen "stack traceback:" en varias líneas) ocupa 2 o 3 renglones, el
// clipper cuenta 1, y el contenido real acaba más abajo de donde el clipper
// cree. Consecuencia medida: SetScrollHereY(1.0f) apunta 52 px por encima del
// fondo de verdad y el panel se sube solo cada vez que el usuario llega abajo.
// Por eso push() trocea el mensaje en una entrada por línea: así todas las
// filas miden un renglón y el clipper vuelve a decir la verdad.
#include "DonTopo/Editor/LogPanel.h"

#include <cstdio>
#include <string>

using namespace DonTopo;

static int g_failures = 0;
// El fflush no es adorno: indexar un deque fuera de rango aborta con exit 3 y
// sin una línea de salida, así que lo que ya se sabía tiene que estar en el
// terminal ANTES del siguiente CHECK.
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); std::fflush(stdout); ++g_failures; } } while (0)
// Contar mal no puede convertirse en un abort mudo: si el número de filas no es
// el esperado, el test lo dice y se sale antes de tocar los índices.
#define REQUIRE_COUNT(log, n) do { CHECK((log).entryCount() == (n)); if ((log).entryCount() != (n)) return; } while (0)

static void test_single_line_is_one_entry()
{
    LogPanel log;
    log.push("una linea sin saltos");
    REQUIRE_COUNT(log, 1u);
    CHECK(log.entryMessage(0) == "una linea sin saltos");
}

static void test_newline_splits_into_rows()
{
    LogPanel log;
    log.push("Script 'x': error\nstack traceback:\n\tx.lua:1: in main chunk");
    REQUIRE_COUNT(log, 3u);
    CHECK(log.entryMessage(0) == "Script 'x': error");
    CHECK(log.entryMessage(1) == "stack traceback:");
    CHECK(log.entryMessage(2) == "\tx.lua:1: in main chunk");
    // Ninguna fila puede conservar el salto: es justo lo que descuadra al
    // clipper.
    for (size_t i = 0; i < log.entryCount(); ++i)
        CHECK(log.entryMessage(i).find('\n') == std::string::npos);
}

static void test_crlf_leaves_no_carriage_return()
{
    LogPanel log;
    log.push("primera\r\nsegunda");
    REQUIRE_COUNT(log, 2u);
    CHECK(log.entryMessage(0) == "primera");
    CHECK(log.entryMessage(1) == "segunda");
}

static void test_trailing_newline_adds_no_empty_row()
{
    LogPanel log;
    log.push("mensaje que acaba en salto\n");
    REQUIRE_COUNT(log, 1u);
    CHECK(log.entryMessage(0) == "mensaje que acaba en salto");
}

static void test_empty_message_still_logs_one_row()
{
    LogPanel log;
    log.push("");
    REQUIRE_COUNT(log, 1u);
    CHECK(log.entryMessage(0).empty());
}

static void test_module_applies_to_every_line()
{
    LogPanel log;
    // El protocolo "[Modulo] " del push de un argumento tiene que sobrevivir al
    // troceado: las líneas 2 y 3 llevan el mismo chip que la primera.
    log.push("[Lua] error\nsegunda linea\ntercera");
    REQUIRE_COUNT(log, 3u);
    for (size_t i = 0; i < log.entryCount(); ++i)
        CHECK(log.entryModule(i) == "Lua");
    CHECK(log.entryMessage(0) == "error");
}

static void test_explicit_module_applies_to_every_line()
{
    LogPanel log;
    log.push("uno\ndos", "Physics");
    REQUIRE_COUNT(log, 2u);
    CHECK(log.entryModule(0) == "Physics");
    CHECK(log.entryModule(1) == "Physics");
}

static void test_ring_buffer_cap_survives_a_huge_message()
{
    LogPanel log;
    // 500 líneas de golpe: el tope del ring buffer se aplica por FILA, no por
    // llamada, o un solo mensaje largo se saltaría el límite.
    std::string big;
    for (int i = 0; i < 500; ++i)
        big += "linea " + std::to_string(i) + "\n";
    log.push(big);
    REQUIRE_COUNT(log, 200u);
    // Se quedan las últimas, como con cualquier otro desbordamiento.
    CHECK(log.entryMessage(log.entryCount() - 1) == "linea 499");
}

int main()
{
    test_single_line_is_one_entry();
    test_newline_splits_into_rows();
    test_crlf_leaves_no_carriage_return();
    test_trailing_newline_adds_no_empty_row();
    test_empty_message_still_logs_one_row();
    test_module_applies_to_every_line();
    test_explicit_module_applies_to_every_line();
    test_ring_buffer_cap_survives_a_huge_message();
    if (g_failures == 0) std::printf("ALL LOG PANEL TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
