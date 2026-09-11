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
