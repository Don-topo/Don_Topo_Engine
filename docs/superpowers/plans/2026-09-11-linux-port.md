# Portabilidad a Linux — Plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** editor, runtime, tests y exportador funcionando en Linux, con guardas que impidan volver a romperlo desde Windows.

**Architecture:** capa `DonTopo::platform` en Core (una implementación Windows, una POSIX); build por presets de CMake + scripts `.sh`; exportador guiado por una tabla `ExportPlatform`; test de escaneo de código (corre en Windows) + CI de Linux.

**Tech Stack:** C++20, CMake ≥ 3.25 + Ninja, MSVC (Windows) / GCC 12+ (Linux), Vulkan, GLFW, FMOD, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-09-11-linux-port-design.md`

## Global Constraints

- Ficheros fuente en **CRLF**; comprobar bytes con Python tras cada Edit (`b.count(b'\r\n')`, `b'\r\r\n'` debe ser 0). Los `.sh` y el `.yml` van en **LF** (añadir `*.sh text eol=lf` y `*.yml text eol=lf` a `.gitattributes`).
- Build Windows: `& .\build.bat` / `& .\build-release.bat` desde PowerShell. Tests desde la raíz del repo. Un sabotaje solo vale si el build salió con exit 0.
- Toda fase termina con **todos** los tests en verde en Debug y Release en Windows.
- Commits con `git commit -F <fichero>`; terminar con `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`.
- Sin `#ifdef _WIN32` ni APIs de Win32 fuera de `engine/src/Core/Platform_win.cpp` y `Renderer/D3D12/`.
- Sin funciones `*_s` de MSVC fuera de `Platform_win.cpp`.

---

### Task 1: Guarda de portabilidad (falla primero)

**Files:**
- Create: `engine/tests/portability_tests.cpp`
- Modify: `engine/tests/CMakeLists.txt` (añadir target)

**Interfaces:**
- Produces: binario `dt_portability_tests`. Tasks 2 y 3 le añaden tests de `Platform` y `copyToBuffer`.

- [ ] **Step 1: Escribir el test**

```cpp
// Guarda de portabilidad: lee el codigo en disco y falla si aparece algo que
// solo compila en Windows fuera de su sitio. Corre en Windows a proposito: el
// error cae al escribirlo, no semanas despues en Linux.
#include <algorithm>
#include <cstdio>
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

// Donde SI puede haber codigo de Windows.
static bool permitidoWin32(const std::string& g)
{
    return g == "engine/src/Core/Platform_win.cpp" ||
           g.rfind("engine/src/Renderer/D3D12/", 0) == 0 ||
           g.rfind("engine/include/DonTopo/Renderer/D3D12/", 0) == 0 ||
           g == "engine/tests/portability_tests.cpp";   // nombra los patrones
}

// Quita el comentario de linea: los comentarios de este repo nombran estas APIs
// a menudo, y un falso positivo por comentario es ruido.
static std::string sinComentario(const std::string& l)
{
    const size_t c = l.find("//");
    return c == std::string::npos ? l : l.substr(0, c);
}

// true si `rel` existe bajo `base` con EXACTAMENTE esas mayusculas; nullopt si
// no existe ni ignorando mayusculas (cabecera de terceros).
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
        if (!hit && !hitCase) return -1;   // no existe
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
        CHECK(fs::is_directory(raiz));   // otro cwd: FALLA, no aprueba
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
                if (g != "engine/src/Core/Platform_win.cpp" && g != "engine/tests/portability_tests.cpp" &&
                    std::regex_search(c, msvcSafe))
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
    CHECK(ficheros > 200);   // de verdad recorrio el arbol
    CHECK(fallos == 0);
}

int main()
{
    test_no_windows_only_code_outside_its_place();
    if (g_failures == 0) std::printf("ALL PORTABILITY TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Registrar el target** (al final de `engine/tests/CMakeLists.txt`, antes del bloque de FMOD)

```cmake
# Lee el codigo en disco: no enlaza nada del motor para el escaneo. Enlaza
# DonTopoCore porque la Task 2 le añade los tests de DonTopo::platform.
add_executable(dt_portability_tests portability_tests.cpp)
target_link_libraries(dt_portability_tests PRIVATE DonTopoCore)
target_compile_features(dt_portability_tests PRIVATE cxx_std_20)
```

- [ ] **Step 3: Compilar y ejecutar desde la raíz: tiene que FALLAR**

Run: `& .\build.bat; & .\build-ninja\engine\tests\dt_portability_tests.exe`
Expected: FAIL listando, como mínimo, los 15 `strncpy_s` de `PropertiesPanel.cpp`, `localtime_s` en `LogPanel.cpp`, y `_WIN32`/APIs en `PerformancePanel.cpp`, `ProjectContext.cpp`, `runtime/main.cpp` y `audio_tests.cpp`. Si aparece algo más, anotarlo: entra en la Task 2. Si da un falso positivo por comentario de bloque, reescribir ese comentario.

- [ ] **Step 4: Commit** (el test en rojo es intencionado; mensaje: `test(portability): guarda que falla con el codigo solo-Windows que hay hoy`)

---

### Task 2: Capa de plataforma y migración de llamantes

**Files:**
- Create: `engine/include/DonTopo/Core/Platform.h`, `engine/src/Core/Platform_win.cpp`, `engine/src/Core/Platform_posix.cpp`, `engine/include/DonTopo/Core/CopyToBuffer.h`
- Modify: `engine/CMakeLists.txt`, `engine/src/Editor/ProjectContext.cpp:16-18,40-51`, `runtime/main.cpp:39-44,68-85,118-136`, `engine/src/Editor/PerformancePanel.cpp:13-30,93-~200,616`, `engine/include/DonTopo/Editor/PerformancePanel.h:105-115`, `engine/src/Editor/LogPanel.cpp:85`, `engine/src/Editor/PropertiesPanel.cpp` (15 `strncpy_s`), `engine/tests/audio_tests.cpp:22-26,736-740`
- Test: `engine/tests/portability_tests.cpp`

**Interfaces:**
- Produces (`DonTopo::platform`): `executableDir()`, `showFatalError(title, msg)`, `ProcessStats processStats()`, `std::optional<GpuMemoryBudget> gpuMemoryBudget()`, `std::tm localTime(std::time_t)`, `unsigned long processId()`, `std::string libcVersion()` y `Os currentOs()` (los usa la Task 5). `DonTopo::copyToBuffer(char(&)[N], std::string_view)`.

- [ ] **Step 1: Tests de la capa** (añadir a `portability_tests.cpp`, con `#include "DonTopo/Core/Platform.h"`, `"DonTopo/Core/CopyToBuffer.h"`, `<cstring>`, `<ctime>`, y llamarlos desde `main`; `main` pasa a `int main(int, char** argv)`)

```cpp
static void test_platform_basics(const char* argv0)
{
    const fs::path yo = fs::weakly_canonical(fs::absolute(argv0)).parent_path();
    CHECK(fs::equivalent(DonTopo::platform::executableDir(), yo));

    const auto s = DonTopo::platform::processStats();
    CHECK(s.valid && s.workingSetMb > 0.0 && s.peakWorkingSetMb >= s.workingSetMb);

    CHECK(DonTopo::platform::processId() != 0);
    CHECK(DonTopo::platform::processId() == DonTopo::platform::processId());

    std::tm ref{}; ref.tm_year = 124; ref.tm_mon = 6; ref.tm_mday = 15; ref.tm_hour = 13; ref.tm_isdst = -1;
    const std::time_t t = std::mktime(&ref);
    const std::tm got = DonTopo::platform::localTime(t);
    CHECK(got.tm_year == 124 && got.tm_mon == 6 && got.tm_mday == 15 && got.tm_hour == 13);
}

static void test_copy_to_buffer()
{
    char b[4];
    std::memset(b, 'x', sizeof(b));
    DonTopo::copyToBuffer(b, "abcdef");
    CHECK(std::strcmp(b, "abc") == 0);   // trunca y termina
    DonTopo::copyToBuffer(b, "");
    CHECK(b[0] == '\0');
}
```

- [ ] **Step 2: `Platform.h`**

```cpp
#pragma once
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>

// Todo lo que el motor le pide al sistema operativo, en un solo sitio. Una
// implementacion por plataforma (Platform_win.cpp, Platform_posix.cpp); los
// llamantes no saben en cual estan. dt_portability_tests impide que vuelva a
// aparecer codigo de Windows fuera de Platform_win.cpp.
namespace DonTopo::platform
{
    // Carpeta del ejecutable; current_path() si el sistema no lo dice.
    std::filesystem::path executableDir();

    // Error que impide seguir. Windows: MessageBox. POSIX: stderr (el runtime
    // ya redirige stderr a game.log).
    void showFatalError(const std::string& title, const std::string& messageUtf8);

    struct ProcessStats
    {
        double workingSetMb     = 0.0;
        double peakWorkingSetMb = 0.0;
        double cpuSeconds       = 0.0;   // usuario + kernel, acumulado
        bool   valid            = false;
    };
    ProcessStats processStats();

    // VRAM del proceso y presupuesto del sistema. Solo existe via DXGI:
    // nullopt en POSIX.
    struct GpuMemoryBudget { double usedMb = 0.0; double budgetMb = 0.0; };
    std::optional<GpuMemoryBudget> gpuMemoryBudget();

    std::tm       localTime(std::time_t t);
    unsigned long processId();

    // Version de glibc ("2.39"); vacio donde no aplica.
    std::string libcVersion();

    // En que sistema corre el binario. Lo decide la implementacion compilada
    // (Platform_win.cpp devuelve Windows; Platform_posix.cpp, Linux — macOS
    // añadira su valor en su spec), no un #ifdef en el llamante.
    enum class Os { Windows, Linux };
    Os currentOs();
}
```

- [ ] **Step 3: `CopyToBuffer.h`**

```cpp
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace DonTopo
{
    // Copia a un buffer fijo (los de ImGui::InputText) truncando y terminando
    // siempre en '\0'. Sustituye a strncpy_s, que solo existe en MSVC.
    template<std::size_t N>
    void copyToBuffer(char (&buf)[N], std::string_view s)
    {
        const std::size_t n = std::min(s.size(), N - 1);
        std::memcpy(buf, s.data(), n);
        buf[n] = '\0';
    }
}
```

- [ ] **Step 4: `Platform_win.cpp`**

```cpp
#include "DonTopo/Core/Platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>

namespace DonTopo::platform
{
    std::filesystem::path executableDir()
    {
        wchar_t buffer[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (n == 0 || n == MAX_PATH) return std::filesystem::current_path();
        return std::filesystem::path(buffer).parent_path();
    }

    static std::wstring widen(const std::string& s)
    {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        if (n <= 0) return {};
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    }

    // MessageBoxW y no A: el texto viene en UTF-8 y la version ANSI lo leeria
    // con la codepage del sistema (acentos rotos).
    void showFatalError(const std::string& title, const std::string& messageUtf8)
    {
        MessageBoxW(nullptr, widen(messageUtf8).c_str(), widen(title).c_str(), MB_OK | MB_ICONERROR);
    }

    ProcessStats processStats()
    {
        ProcessStats s;
        PROCESS_MEMORY_COUNTERS pmc{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        {
            s.workingSetMb     = double(pmc.WorkingSetSize) / (1024.0 * 1024.0);
            s.peakWorkingSetMb = double(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
            s.valid = true;
        }
        FILETIME c{}, e{}, k{}, u{};
        if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
        {
            ULARGE_INTEGER kk{}, uu{};
            kk.LowPart = k.dwLowDateTime; kk.HighPart = k.dwHighDateTime;
            uu.LowPart = u.dwLowDateTime; uu.HighPart = u.dwHighDateTime;
            s.cpuSeconds = double(kk.QuadPart + uu.QuadPart) / 1e7;   // unidades de 100 ns
        }
        return s;
    }

    // PEGAR AQUI el bloque DXGI de PerformancePanel::sampleProcess tal cual
    // (desde `if (!m_dxgiTried)` hasta el QueryVideoMemoryInfo), cambiando los
    // miembros m_dxgiTried/m_dxgiAdapter por estas dos estaticas y devolviendo
    // {CurrentUsage, Budget} en MB. El adaptador vive lo que el proceso.
    std::optional<GpuMemoryBudget> gpuMemoryBudget()
    {
        static bool           tried   = false;
        static IDXGIAdapter3* adapter = nullptr;
        // ... cuerpo movido desde PerformancePanel.cpp (ver arriba) ...
        if (!adapter) return std::nullopt;
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            return std::nullopt;
        return GpuMemoryBudget{ double(info.CurrentUsage) / (1024.0 * 1024.0),
                                double(info.Budget)       / (1024.0 * 1024.0) };
    }

    std::tm localTime(std::time_t t)
    {
        std::tm out{};
        localtime_s(&out, &t);
        return out;
    }

    unsigned long processId() { return static_cast<unsigned long>(GetCurrentProcessId()); }

    std::string libcVersion() { return {}; }

    Os currentOs() { return Os::Windows; }
}
```

> Nota para quien implemente: el "PEGAR AQUI" es un movimiento de código existente (`PerformancePanel.cpp`, desde `if (!m_dxgiTried)` hasta el final de la consulta de VRAM), no código nuevo. Verificarlo con la técnica de la memoria *verify_pure_move_refactor*: el cuerpo movido tiene que coincidir con el original salvo los nombres de las dos variables.

- [ ] **Step 5: `Platform_posix.cpp`**

```cpp
#include "DonTopo/Core/Platform.h"

#include <cstdio>
#include <fstream>
#include <sys/resource.h>
#include <unistd.h>
#if defined(__GLIBC__)
#include <gnu/libc-version.h>
#endif

namespace DonTopo::platform
{
    std::filesystem::path executableDir()
    {
        std::error_code ec;
        const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
        return ec ? std::filesystem::current_path() : self.parent_path();
    }

    void showFatalError(const std::string& title, const std::string& messageUtf8)
    {
        std::fprintf(stderr, "%s: %s\n", title.c_str(), messageUtf8.c_str());
    }

    // VmRSS / VmHWM vienen en kB.
    ProcessStats processStats()
    {
        ProcessStats s;
        std::ifstream f("/proc/self/status");
        std::string key;
        double kb = 0.0;
        while (f >> key)
        {
            if (key == "VmRSS:" && (f >> kb)) { s.workingSetMb = kb / 1024.0; s.valid = true; }
            else if (key == "VmHWM:" && (f >> kb)) s.peakWorkingSetMb = kb / 1024.0;
        }
        rusage ru{};
        if (getrusage(RUSAGE_SELF, &ru) == 0)
            s.cpuSeconds = double(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) +
                           double(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
        return s;
    }

    std::optional<GpuMemoryBudget> gpuMemoryBudget() { return std::nullopt; }

    std::tm localTime(std::time_t t)
    {
        std::tm out{};
        localtime_r(&t, &out);
        return out;
    }

    unsigned long processId() { return static_cast<unsigned long>(getpid()); }

    std::string libcVersion()
    {
#if defined(__GLIBC__)
        return gnu_get_libc_version();
#else
        return {};
#endif
    }

    Os currentOs() { return Os::Linux; }
}
```

- [ ] **Step 6: CMake** (en `engine/CMakeLists.txt`, tras `add_library(DonTopoCore ...)`)

```cmake
# Una implementacion de DonTopo::platform por sistema (ver Platform.h).
if(WIN32)
    target_sources(DonTopoCore PRIVATE src/Core/Platform_win.cpp)
    target_link_libraries(DonTopoCore PRIVATE psapi dxgi)
else()
    target_sources(DonTopoCore PRIVATE src/Core/Platform_posix.cpp)
endif()
```

- [ ] **Step 7: Migrar llamantes**

- `ProjectContext.cpp`: borrar el `#include <windows.h>` y el cuerpo con `#ifdef`; `executableDir()` pasa a `return platform::executableDir();` (con `#include "DonTopo/Core/Platform.h"`).
- `runtime/main.cpp`: sacar los tres includes de componentes FUERA del `#ifdef` y borrar `<windows.h>`; `executableDir()` → `platform::executableDir()`; en `reportFatal`, sustituir el bloque `#ifdef` por `DonTopo::platform::showFatalError("Don Topo Engine", msg);` (el `std::cerr` de antes se queda).
- `PerformancePanel`: borrar includes y `#pragma comment` de Windows y todos los `#ifdef _WIN32`. `sampleProcess()`: RAM de `processStats()`; CPU % = `100 * (s.cpuSeconds - m_lastCpuSeconds) / (wall * cores)` con `cores = std::max(1u, std::thread::hardware_concurrency())`, conservando el clamp a [0,100] y la primera muestra sin porcentaje; VRAM de `gpuMemoryBudget()`. Miembros: `m_lastCpuTicks` (uint64) → `double m_lastCpuSeconds = -1.0`; borrar `m_dxgiAdapter` y `m_dxgiTried`, y el cuerpo del destructor. Dibujo (`:616`): quitar el `#ifdef`; si `m_gpuBudgetMb <= 0` la rama `else` ya existente dice "no disponible".
- `LogPanel.cpp:85`: `const std::tm tmBuf = platform::localTime(t);` (quitar la declaración previa de `tmBuf`).
- `PropertiesPanel.cpp`: los 15 `strncpy_s(buf, X.c_str(), sizeof(buf) - 1);` → `copyToBuffer(buf, X);` (añadir `#include "DonTopo/Core/CopyToBuffer.h"`). Si alguno no es un array (`char*`), el template no compila: dejarlo con `std::snprintf(buf, size, "%s", X.c_str())`.
- `audio_tests.cpp`: borrar el bloque de includes `#ifdef`; el nombre del temporal usa `DonTopo::platform::processId()`.

- [ ] **Step 8: Compilar y ejecutar** — `dt_portability_tests` en verde y los 28 tests en Debug y Release.

- [ ] **Step 9: Sabotaje de uno en uno** (compilar → ejecutar → revertir; el patrón saboteado único en el fichero): volver a poner un `strncpy_s` (1 FAIL), un `#ifdef _WIN32` en `LogPanel.cpp` (1 FAIL), un `#include "DonTopo/core/Platform.h"` en minúscula (1 FAIL), y `executableDir()` devolviendo `current_path()` (FAIL en `test_platform_basics` al lanzarlo desde la raíz).

- [ ] **Step 10: Commit** — `feat(core): capa de plataforma; el codigo solo-Windows vive en Platform_win.cpp`

---

### Task 3: Build de Linux

**Files:**
- Create: `configure.sh`, `build.sh`, `cmake/FmodRuntime.cmake`
- Modify: `CMakePresets.json`, `.gitattributes`, `cmake/FindFMOD.cmake`, `CMakeLists.txt`, `engine/CMakeLists.txt`, `sandbox/CMakeLists.txt`, `runtime/CMakeLists.txt`, `engine/tests/CMakeLists.txt`, `engine/src/Editor/EditorUI.cpp:1955`, `README.md`

**Interfaces:**
- Produces: presets `linux-debug`/`linux-release` (dirs `build-linux`/`build-linux-release`); `dt_copy_fmod_runtime(<target>)`; `dt_add_test(<name> <source> <lib>)`; definición `DT_RUNTIME_FILE_NAME` en DonTopoEditor.

- [ ] **Step 1: Presets.** A los dos presets actuales, añadir `"condition": {"type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows"}`. Añadir:

```json
{ "name": "linux-debug", "displayName": "Debug Linux", "generator": "Ninja",
  "binaryDir": "${sourceDir}/build-linux",
  "condition": {"type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux"},
  "cacheVariables": {"CMAKE_BUILD_TYPE": "Debug", "CMAKE_EXPORT_COMPILE_COMMANDS": "ON"} },
{ "name": "linux-release", "displayName": "Release Linux", "generator": "Ninja",
  "binaryDir": "${sourceDir}/build-linux-release",
  "condition": {"type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux"},
  "cacheVariables": {"CMAKE_BUILD_TYPE": "Release"} }
```
y los `buildPresets` `linux-debug`/`linux-release` con su `configurePreset`.

- [ ] **Step 2: Scripts** (LF, `git update-index --chmod=+x` a los dos)

`configure.sh`:
```bash
#!/usr/bin/env bash
# Equivalente Linux de configure.bat / configure-release.bat.
# Uso: ./configure.sh [linux-debug|linux-release]
set -euo pipefail
cd "$(dirname "$0")"
preset="${1:-linux-debug}"
falta() { echo "Falta '$1'. Instalar: $2" >&2; exit 1; }
command -v cmake >/dev/null || falta cmake "sudo apt install cmake"
command -v ninja >/dev/null || falta ninja "sudo apt install ninja-build"
command -v glslc >/dev/null || falta glslc "sudo apt install glslc"
ver=$(cmake --version | head -1 | awk '{print $3}')
if [ "$(printf '%s\n3.25\n' "$ver" | sort -V | head -1)" != "3.25" ]; then
    echo "cmake $ver es demasiado viejo: hace falta 3.25 o superior (Ubuntu 22.04 trae 3.22)." >&2
    exit 1
fi
cmake --preset "$preset"
```

`build.sh`:
```bash
#!/usr/bin/env bash
# Uso: ./build.sh [linux-debug|linux-release]
set -euo pipefail
cd "$(dirname "$0")"
cmake --build --preset "${1:-linux-debug}"
```

`.gitattributes`: añadir `*.sh text eol=lf` y `*.yml text eol=lf`.

- [ ] **Step 3: FMOD.** En `FindFMOD.cmake`, en las dos llamadas a `find_library`, añadir `"api/core/lib/x86_64"` a `PATH_SUFFIXES`. Crear `cmake/FmodRuntime.cmake`:

```cmake
# Copia la biblioteca de FMOD junto al binario del target: fmod.dll en Windows,
# libfmod.so* (con su soname) en Linux. Un solo sitio para Sandbox, runtime y
# tests; antes era un bloque solo-Windows con una lista de 21 targets a mano.
function(dt_copy_fmod_runtime target)
    if(NOT FMOD_FOUND)
        return()
    endif()
    get_filename_component(_dir "${FMOD_LIBRARY}" DIRECTORY)
    if(WIN32)
        set(_files "${_dir}/fmod.dll")
    else()
        file(GLOB _files "${_dir}/libfmod.so*")
    endif()
    foreach(_f IN LISTS _files)
        if(EXISTS "${_f}")
            get_filename_component(_n "${_f}" NAME)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_f}" "$<TARGET_FILE_DIR:${target}>/${_n}"
                COMMENT "Copying ${_n} next to ${target}")
        endif()
    endforeach()
endfunction()
```
En `CMakeLists.txt` raíz, justo después de `find_package(FMOD)`: `include(cmake/FmodRuntime.cmake)`.

- [ ] **Step 4: rpath y nombre del runtime.** En `CMakeLists.txt` raíz, justo antes de `add_subdirectory(engine)`:

```cmake
# Linux: los binarios buscan libfmod.so en su propia carpeta (el juego
# exportado depende de lo mismo). Solo afecta a los targets de aqui abajo.
if(UNIX AND NOT APPLE)
    set(CMAKE_BUILD_RPATH "\$ORIGIN")
endif()

# Nombre del binario del runtime, UNA vez: lo usan el POST_BUILD que lo copia
# junto al editor y el editor al exportar (DT_RUNTIME_FILE_NAME).
set(DT_RUNTIME_FILE_NAME "DonTopoRuntime${CMAKE_EXECUTABLE_SUFFIX}")
```
En `engine/CMakeLists.txt`: `target_compile_definitions(DonTopoEditor PRIVATE DT_RUNTIME_FILE_NAME="${DT_RUNTIME_FILE_NAME}")`. En `runtime/CMakeLists.txt`, el destino del POST_BUILD pasa a `$<TARGET_FILE_DIR:Sandbox>/${DT_RUNTIME_FILE_NAME}`, y al final `dt_copy_fmod_runtime(DonTopoRuntime)`. En `EditorUI.cpp:1955`: `projectRoot / DT_RUNTIME_FILE_NAME`.

- [ ] **Step 5: Sandbox.** Quitar `Sandbox.rc` de `add_executable` y añadir `if(WIN32) target_sources(Sandbox PRIVATE Sandbox.rc) endif()`. `find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/Bin $ENV{VULKAN_SDK}/bin)`. Sustituir el bloque final "Copy FMOD runtime DLL" por `dt_copy_fmod_runtime(Sandbox)`.

- [ ] **Step 6: Tests.** Al principio de `engine/tests/CMakeLists.txt`:

```cmake
# Todo test pasa por aqui: asi la copia de FMOD (y lo que venga) no depende de
# acordarse de apuntar el target en una lista aparte.
function(dt_add_test name source lib)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${lib})
    target_compile_features(${name} PRIVATE cxx_std_20)
    dt_copy_fmod_runtime(${name})
endfunction()
```
Convertir cada trío `add_executable`/`target_link_libraries`/`target_compile_features` en `dt_add_test(...)` (conservando líneas extra como el `target_include_directories` de `dt_splash_tests` y los comentarios), incluido `dt_portability_tests`. Borrar el bloque final `if(FMOD_FOUND AND WIN32) ... foreach ... endif()`.

- [ ] **Step 7: README.** Sección "Compilar en Linux (Ubuntu/Debian)":
```
sudo apt install build-essential cmake ninja-build glslc libvulkan-dev \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    libwayland-dev libxkbcommon-dev wayland-protocols pkg-config
# FMOD: descargar "FMOD Engine" para Linux de fmod.com y extraerlo en third_party/fmod
./configure.sh && ./build.sh            # Debug
./configure.sh linux-release && ./build.sh linux-release
```

- [ ] **Step 8: Verificar en Windows:** reconfigurar (`& .\configure.bat`, `& .\configure-release.bat`), compilar, 28 tests Debug + Release; `fmod.dll` sigue junto a `Sandbox.exe`, `DonTopoRuntime.exe` y cada `dt_*.exe`; exportar un juego desde el editor sigue funcionando (el exportador busca el runtime por `DT_RUNTIME_FILE_NAME`).

- [ ] **Step 9: Commit** — `build: presets y scripts de Linux, FMOD y nombre del runtime en un solo sitio`

---

### Task 4: CI de Linux y errores de GCC

**Files:**
- Create: `.github/workflows/linux.yml`
- Modify: lo que el compilador de Linux señale

- [ ] **Step 1: Workflow**

```yaml
name: linux
on:
  push:
    branches: [main]
  pull_request:
    branches: [main]
jobs:
  build:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - name: Dependencias
        run: |
          sudo apt-get update
          sudo apt-get install -y ninja-build glslc libvulkan-dev pkg-config ccache \
            libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
            libwayland-dev libxkbcommon-dev wayland-protocols
      - uses: actions/cache@v4
        with:
          path: |
            build-linux-release/_deps/*-src
            ~/.cache/ccache
          key: linux-${{ hashFiles('CMakeLists.txt', 'cmake/**') }}-${{ github.sha }}
          restore-keys: linux-${{ hashFiles('CMakeLists.txt', 'cmake/**') }}-
      - name: Configurar (sin FMOD)
        run: ./configure.sh linux-release
        env:
          CMAKE_C_COMPILER_LAUNCHER: ccache
          CMAKE_CXX_COMPILER_LAUNCHER: ccache
      - name: Compilar
        run: ./build.sh linux-release
      - name: Tests (desde la raiz del repo)
        run: |
          fallos=0
          for t in build-linux-release/engine/tests/dt_*; do
            [ -f "$t" ] && [ -x "$t" ] || continue
            if ! "$t" > /tmp/salida.txt 2>&1; then
              echo "::error::FALLA $t"; cat /tmp/salida.txt; fallos=1
            fi
          done
          exit $fallos
```

- [ ] **Step 2: Push y leer el resultado.** Pedir al usuario `gh auth login` (con `unset GITHUB_TOKEN` antes, ver memoria *gh_token_env_override*) para poder usar `gh run watch` / `gh run view --log-failed`; si no, el usuario pega el log.

- [ ] **Step 3: Iterar.** Cada error de compilación de GCC: arreglarlo en el sitio (no con `#ifdef`), comprobar que Windows sigue compilando y pasando, commit y push. Arreglos esperables: includes de la biblioteca estándar que faltan (`<cstring>`, `<algorithm>`, `<cstdint>`, `<array>`), código que MSVC acepta por permisividad. **Si PhysX no compila con GCC**, añadir a los dos presets de Linux `"CMAKE_C_COMPILER": "clang", "CMAKE_CXX_COMPILER": "clang++"` y `clang` al `apt install` del workflow y del README. **Si aparecen más de ~50 errores distintos, parar y consultar** antes de seguir.

- [ ] **Step 4: Tests sin FMOD.** Si algún test falla en CI solo por no tener FMOD, protegerlo con `DT_FMOD_ENABLED` (el caso se salta con un mensaje, no aprueba en silencio).

- [ ] **Step 5: Hecho cuando** el workflow sale en verde en `main`. Commits por arreglo: `fix(linux): ...`.

---

### Task 5: Exportador multiplataforma

**Files:**
- Modify: `engine/include/DonTopo/Editor/GameExporter.h`, `engine/src/Editor/GameExporter.cpp:432-800`, `runtime/CMakeLists.txt`
- Test: `engine/tests/exporter_tests.cpp`

**Interfaces:**
- Consumes: `platform::libcVersion()`, `platform::Os`, `platform::currentOs()` (Task 2).
- Produces: `ExportPlatform`, `exportPlatformFor(platform::Os)`, `isAudioLibFile(name, plat)`; `writeExportPackage(..., const ExportPlatform& plat = exportPlatformFor(platform::currentOs()))`.

- [ ] **Step 1: Tests** (en `exporter_tests.cpp`)

```cpp
static void test_export_platform_rows()
{
    const ExportPlatform w = exportPlatformFor(platform::Os::Windows);
    CHECK(w.executableSuffix == ".exe" && !w.setExecutableBit && w.copyMsvcCrt && w.warnDebugCrt && !w.warnGlibc);
    CHECK(w.audioLibPrefixes == std::vector<std::string>{"fmod.dll"});

    const ExportPlatform l = exportPlatformFor(platform::Os::Linux);
    CHECK(l.executableSuffix.empty() && l.setExecutableBit && !l.copyMsvcCrt && !l.warnDebugCrt && l.warnGlibc);
    CHECK(l.audioLibPrefixes == std::vector<std::string>{"libfmod.so"});

    CHECK(isAudioLibFile("libfmod.so.13", l) && isAudioLibFile("libfmod.so", l));
    CHECK(!isAudioLibFile("libfmodL.so.13", l));   // la variante de logging no va
    CHECK(isAudioLibFile("fmod.dll", w) && !isAudioLibFile("fmodL.dll", w));
}
```
Y un test que escribe un paquete **de Linux desde Windows** con `writeExportPackage(..., exportPlatformFor(platform::Os::Linux))`, en un temporal con un runtime y un `libfmod.so.13` falsos junto al `projectRoot` falso: comprueba que existe `<juego>` sin `.exe`, que está `libfmod.so.13`, que NO hay `msvcp140*`, y — solo `if (platform::currentOs() == platform::Os::Linux)` — que `<juego>` tiene `fs::perms::owner_exec`. Seguir la forma de los tests existentes de `writeExportPackage` en ese fichero (mismos helpers de temporales).

- [ ] **Step 2: Ejecutar: falla** (no compila: `ExportPlatform` no existe).

- [ ] **Step 3: Implementar** en `GameExporter.h`:

```cpp
// Que lleva el paquete exportado en cada plataforma. writeExportPackage recorre
// esto en vez de tener ramas por sistema: macOS sera otra fila.
struct ExportPlatform {
    std::string              executableSuffix;   // ".exe" | ""
    bool                     setExecutableBit;   // chmod +x al ejecutable
    std::vector<std::string> audioLibPrefixes;   // fichero exacto o prefijo + "."
    bool                     copyMsvcCrt;        // msvcp140* / vcruntime140* junto al editor
    bool                     warnDebugCrt;       // aviso de CRT de depuracion no redistribuible
    bool                     warnGlibc;          // aviso de version minima de glibc
};

ExportPlatform exportPlatformFor(platform::Os os);
bool           isAudioLibFile(const std::string& fileName, const ExportPlatform& plat);
```
En `GameExporter.cpp`:
```cpp
ExportPlatform exportPlatformFor(platform::Os os)
{
    if (os == platform::Os::Windows)
        return { ".exe", false, { "fmod.dll" }, true, true, false };
    return { "", true, { "libfmod.so" }, false, false, true };
}

bool isAudioLibFile(const std::string& n, const ExportPlatform& plat)
{
    for (const std::string& p : plat.audioLibPrefixes)
        if (n == p || n.rfind(p + ".", 0) == 0) return true;
    return false;
}
```
Recableado de `writeExportPackage` (añadir el parámetro `plat`, que `exportGame` pasa como `exportPlatformFor(platform::currentOs())`):
- `:518` → `pkg / (gameName + plat.executableSuffix)`; tras copiar, si `plat.setExecutableBit`: `fs::permissions(dst, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec)`.
- `:703-716` (fmod.dll) → recorrer `projectRoot` y copiar todo fichero con `isAudioLibFile(nombre, plat)`; si no hay ninguno y el motor tiene FMOD, el aviso actual con `plat.audioLibPrefixes[0]` en vez de `"fmod.dll"`.
- `:719-766` (CRT) → solo si `plat.copyMsvcCrt`.
- `:768-790` (CRT de depuración) → solo si `plat.warnDebugCrt`.
- Nuevo, si `plat.warnGlibc`: `r.messages.push_back("Aviso: el juego necesita glibc " + platform::libcVersion() + " o superior; no arrancara en distros mas antiguas que esta.")`.

Y en `runtime/CMakeLists.txt`:
```cmake
# El juego exportado no depende de la libstdc++ del jugador (evita el
# "GLIBCXX_3.4.xx not found" en distros viejas). glibc sigue siendo dinamica.
if(UNIX AND NOT APPLE)
    target_link_options(DonTopoRuntime PRIVATE -static-libstdc++ -static-libgcc)
endif()
```

- [ ] **Step 4: Ejecutar** — `dt_exporter_tests` y los 28 en verde en Debug y Release; sabotaje de uno en uno: fila de Linux con `".exe"` (FAIL), `isAudioLibFile` aceptando `libfmodL` (FAIL), copiar el CRT sin mirar `copyMsvcCrt` (FAIL en el test del paquete de Linux).

- [ ] **Step 5: Commit** — `feat(export): tabla de plataforma; exportar un juego de Linux`

---

### Task 6: Verificación manual en Ubuntu (usuario)

- [ ] `./configure.sh && ./build.sh` con FMOD en `third_party/fmod`; los 28 tests desde la raíz.
- [ ] El editor (`build-linux/sandbox/Sandbox`) abre un proyecto y renderiza con Vulkan; el audio suena.
- [ ] Exportar un juego desde el editor, copiar la carpeta a otro sitio y arrancarlo con doble clic; revisar `game.log`. Si se puede, en otra máquina u otra versión de Ubuntu.
- [ ] Anotar cualquier ruta de asset que falle por mayúsculas (fuera de alcance, pero se registra).
