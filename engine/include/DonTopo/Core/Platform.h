#pragma once
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>

// Everything the engine asks of the operating system, in one place. One
// implementation per platform (Platform_win.cpp, Platform_posix.cpp); callers
// do not know which one they are on. dt_portability_tests prevents Windows code
// from showing up again outside Platform_win.cpp.
namespace DonTopo::platform
{
    // Executable folder; current_path() if the system does not report it.
    std::filesystem::path executableDir();

    // Error that prevents continuing. Windows: MessageBox. POSIX: stderr (the runtime
    // already redirects stderr to game.log).
    void showFatalError(const std::string& title, const std::string& messageUtf8);

    struct ProcessStats
    {
        double workingSetMb     = 0.0;
        double peakWorkingSetMb = 0.0;
        double cpuSeconds       = 0.0;   // user + kernel, accumulated
        bool   valid            = false;
    };
    ProcessStats processStats();

    // Process VRAM and system budget. Only exists through DXGI:
    // nullopt on POSIX.
    struct GpuMemoryBudget { double usedMb = 0.0; double budgetMb = 0.0; };
    std::optional<GpuMemoryBudget> gpuMemoryBudget();

    std::tm       localTime(std::time_t t);
    unsigned long processId();

    // glibc version ("2.39"); empty where it does not apply.
    std::string libcVersion();

    // Which system the binary runs on. It is decided by the compiled implementation
    // (Platform_win.cpp returns Windows; Platform_posix.cpp, Linux — macOS
    // will add its value in its spec), not by an #ifdef in the caller.
    enum class Os { Windows, Linux };
    Os currentOs();
}
