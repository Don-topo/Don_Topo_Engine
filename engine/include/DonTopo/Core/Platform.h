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
