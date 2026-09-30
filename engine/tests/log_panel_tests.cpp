// Headless test of the Log buffer (no GUI). Plain main + asserts, no
// framework, same pattern as content_browser_tests.cpp.
//
// What is covered and why: the panel draws the rows with ImGuiListClipper, which
// assumes ALL of them have the same height. A message with '\n' inside (Lua
// errors carry "stack traceback:" over several lines) takes 2 or 3 lines, the
// clipper counts 1, and the real content ends up lower than where the clipper
// thinks. Measured consequence: SetScrollHereY(1.0f) points 52 px above the real
// bottom and the panel scrolls up by itself every time the user reaches the
// bottom. That is why push() splits the message into one entry per line: that way
// all rows are one line tall and the clipper tells the truth again.
#include "DonTopo/Editor/LogPanel.h"

#include <cstdio>
#include <string>

using namespace DonTopo;

static int g_failures = 0;
// The fflush is not decoration: indexing a deque out of range aborts with exit 3
// and without a single line of output, so what was already known has to be in the
// terminal BEFORE the next CHECK.
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); std::fflush(stdout); ++g_failures; } } while (0)
// Miscounting cannot turn into a silent abort: if the number of rows is not the
// expected one, the test says so and exits before touching the indices.
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
    // No row may keep the line break: it is exactly what throws the clipper off.
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
    // The "[Module] " protocol of the one-argument push has to survive the
    // splitting: lines 2 and 3 carry the same chip as the first.
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
    // 500 lines at once: the ring buffer cap is applied per ROW, not per
    // call, or a single long message would skip the limit.
    std::string big;
    for (int i = 0; i < 500; ++i)
        big += "linea " + std::to_string(i) + "\n";
    log.push(big);
    REQUIRE_COUNT(log, 200u);
    // The last ones stay, as with any other overflow.
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
