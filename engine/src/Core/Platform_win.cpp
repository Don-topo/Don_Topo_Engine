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

    // MessageBoxW and not A: the text comes in UTF-8 and the ANSI version would read it
    // with the system codepage (broken accents).
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
            s.cpuSeconds = double(kk.QuadPart + uu.QuadPart) / 1e7;   // units of 100 ns
        }
        return s;
    }

    // VRAM: DXGI gives the REAL usage of the process and the budget the system grants it.
    // Vulkan alone does not expose it without VK_EXT_memory_budget. The
    // adapter is opened once and lives as long as the process.
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
                // Adapter 0: the engine does not expose the Vulkan device LUID, and on
                // a single dedicated GPU machine it is the same.
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
