// Portability guard: reads the code on disk and fails if anything that only
// compiles on Windows shows up out of place. It runs on Windows on purpose: the
// error shows up when it is written, not weeks later on Linux.
#include "DonTopo/Core/CopyToBuffer.h"
#include "DonTopo/Core/Platform.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

namespace fs = std::filesystem;
static int g_failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL: %s (line %d)\n", #c, __LINE__); ++g_failures; } } while (0)

static const char* kRaices[] = { "engine", "sandbox", "runtime" };
static const char* kIncludeRoots[] = { "engine/include", "engine/src", "sandbox/src", "runtime" };

static bool esFuente(const fs::path& p)
{
    const std::string e = p.extension().string();
    return e == ".cpp" || e == ".h" || e == ".hpp" || e == ".inl";
}

// Where Windows code CAN be.
static bool permitidoWin32(const std::string& g)
{
    return g == "engine/src/Core/Platform_win.cpp" ||
           g.rfind("engine/src/Renderer/D3D12/", 0) == 0 ||
           g.rfind("engine/include/DonTopo/Renderer/D3D12/", 0) == 0 ||
           g == "engine/tests/portability_tests.cpp";   // names the patterns
}

// Strips the line comment: the comments in this repo often name these APIs, and a
// false positive from a comment is noise.
static std::string sinComentario(const std::string& l)
{
    const size_t c = l.find("//");
    return c == std::string::npos ? l : l.substr(0, c);
}

// 1 if `rel` exists under `base` with EXACTLY that capitalization, 0 if it only
// exists ignoring case, -1 if it does not exist (third-party header).
static int existeExacto(const fs::path& base, const fs::path& rel)
{
    fs::path cur = base;
    bool exacto = true;
    for (const fs::path& part : rel)
    {
        if (part == ".") continue;
        if (part == "..") { cur = cur.parent_path(); continue; }
        std::error_code ec;
        if (!fs::is_directory(cur, ec)) return -1;
        bool hit = false, hitCase = false;
        for (const auto& e : fs::directory_iterator(cur, ec))
        {
            const std::string n = e.path().filename().string();
            if (n == part.string()) { hit = true; break; }
            std::string a = n, b = part.string();
            std::transform(a.begin(), a.end(), a.begin(), ::tolower);
            std::transform(b.begin(), b.end(), b.begin(), ::tolower);
            if (a == b) hitCase = true;
        }
        if (!hit && !hitCase) return -1;   // does not exist
        if (!hit) exacto = false;
        cur /= part;
    }
    return exacto ? 1 : 0;
}

static void test_no_windows_only_code_outside_its_place()
{
    const std::regex win32Pre(R"(^\s*#.*\b_WIN32\b)");
    const std::regex winInc(R"(#\s*include\s*<(windows|psapi|dxgi[0-9_]*)\.h>)", std::regex::icase);
    const std::regex win32Api(R"(\b(GetModuleFileName\w*|MessageBox\w*|GetProcessMemoryInfo|GetProcessTimes|GetCurrentProcessId|GetCurrentProcess|GetSystemInfo|MultiByteToWideChar|WideCharToMultiByte|QueryPerformanceCounter|QueryPerformanceFrequency|CreateDXGIFactory\w*)\s*\()");
    const std::regex msvcSafe(R"(\b(strncpy_s|strcpy_s|strcat_s|localtime_s|gmtime_s|sprintf_s|snprintf_s|fopen_s|sscanf_s|_stricmp|_strnicmp)\s*\()");
    const std::regex inc(R"(^\s*#\s*include\s*["<]([^">]+)[">])");

    int ficheros = 0, fallos = 0;
    for (const char* raiz : kRaices)
    {
        CHECK(fs::is_directory(raiz));   // other cwd: FAILS, does not pass
        if (!fs::is_directory(raiz)) continue;
        for (const auto& e : fs::recursive_directory_iterator(raiz))
        {
            if (!e.is_regular_file() || !esFuente(e.path())) continue;
            ++ficheros;
            const std::string g = e.path().generic_string();
            std::ifstream f(e.path());
            std::string l;
            int n = 0;
            while (std::getline(f, l))
            {
                ++n;
                const std::string c = sinComentario(l);
                auto falla = [&](const char* regla) {
                    std::printf("  %s:%d: %s\n", g.c_str(), n, regla);
                    ++fallos;
                };
                if (!permitidoWin32(g))
                {
                    if (std::regex_search(c, win32Pre)) falla("_WIN32 fuera de Platform_win.cpp");
                    if (std::regex_search(c, winInc))   falla("cabecera de Windows fuera de Platform_win.cpp");
                    if (std::regex_search(c, win32Api)) falla("API de Win32 fuera de Platform_win.cpp");
                }
                // Same exceptions as Win32: D3D12 only compiles on Windows.
                if (!permitidoWin32(g) && std::regex_search(c, msvcSafe))
                    falla("funcion *_s de MSVC");

                std::smatch m;
                if (std::regex_search(c, m, inc))
                {
                    const std::string ruta = m[1].str();
                    const bool comillas = c.find('"') != std::string::npos;
                    if (!comillas && ruta.rfind("DonTopo/", 0) != 0) continue;
                    std::vector<fs::path> bases{ e.path().parent_path() };
                    for (const char* r : kIncludeRoots) bases.emplace_back(r);
                    int mejor = -1;
                    for (const fs::path& b : bases) mejor = std::max(mejor, existeExacto(b, ruta));
                    if (mejor == 0) falla("include con mayusculas distintas al fichero real");
                }
            }
        }
    }
    CHECK(ficheros > 200);   // it really walked the tree
    CHECK(fallos == 0);
}

// The platform layer answers the same as the system through other routes.
static void test_platform_basics(const char* argv0)
{
    const fs::path yo = fs::weakly_canonical(fs::absolute(argv0)).parent_path();
    CHECK(fs::equivalent(DonTopo::platform::executableDir(), yo));

    const auto s = DonTopo::platform::processStats();
    CHECK(s.valid && s.workingSetMb > 0.0 && s.peakWorkingSetMb >= s.workingSetMb);

    CHECK(DonTopo::platform::processId() != 0);
    CHECK(DonTopo::platform::processId() == DonTopo::platform::processId());

    std::tm ref{};
    ref.tm_year = 124; ref.tm_mon = 6; ref.tm_mday = 15; ref.tm_hour = 13; ref.tm_isdst = -1;
    const std::time_t t = std::mktime(&ref);
    const std::tm got = DonTopo::platform::localTime(t);
    CHECK(got.tm_year == 124 && got.tm_mon == 6 && got.tm_mday == 15 && got.tm_hour == 13);
}

static void test_copy_to_buffer()
{
    char b[4];
    std::memset(b, 'x', sizeof(b));
    DonTopo::copyToBuffer(b, "abcdef");
    CHECK(std::strcmp(b, "abc") == 0);   // truncates and ends in '\0'
    DonTopo::copyToBuffer(b, "");
    CHECK(b[0] == '\0');
}

int main(int, char** argv)
{
    test_no_windows_only_code_outside_its_place();
    test_platform_basics(argv[0]);
    test_copy_to_buffer();
    if (g_failures == 0) std::printf("ALL PORTABILITY TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
