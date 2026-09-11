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

    // VRAM: DXGI da el uso REAL del proceso y el presupuesto que le concede el
    // sistema. Vulkan por si solo no lo expone sin VK_EXT_memory_budget. El
    // adaptador se abre una vez y vive lo que el proceso.
    std::optional<GpuMemoryBudget> gpuMemoryBudget()
    {
        static bool           tried   = false;
        static IDXGIAdapter3* adapter = nullptr;
        if (!tried)
        {
            tried = true;
            IDXGIFactory1* factory = nullptr;
            if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) && factory)
            {
                IDXGIAdapter1* adapter1 = nullptr;
                // Adaptador 0: el motor no expone el LUID del device de Vulkan, y en
                // una maquina de un solo GPU dedicado es el mismo.
                if (SUCCEEDED(factory->EnumAdapters1(0, &adapter1)) && adapter1)
                {
                    IDXGIAdapter3* adapter3 = nullptr;
                    if (SUCCEEDED(adapter1->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&adapter3)))
                        adapter = adapter3;
                    adapter1->Release();
                }
                factory->Release();
            }
        }
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
