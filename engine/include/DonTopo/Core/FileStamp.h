#pragma once
#include <cstdint>
#include <filesystem>
#include <system_error>

namespace DonTopo {

// A file and its state at a given moment. Used by the Content Browser thumbnails
// to know when to regenerate: a thumbnail depends on its asset and on what
// that asset reads (external textures, sidecar, .mtl).
struct FileStamp
{
    std::filesystem::path path;
    bool                  exists  = false;
    int64_t               mtime   = 0;       // file_time_type::time_since_epoch().count(); 0 if it does not exist
    bool                  stamped = false;   // false = path only, disk not looked at yet
};

// path, exists and mtime. `stamped` does not count: it is how it was obtained, not which file it is.
inline bool operator==(const FileStamp& a, const FileStamp& b)
{
    return a.exists == b.exists && a.mtime == b.mtime && a.path == b.path;
}

// State of a file NOW. Never throws: if it cannot be read, exists = false.
// Whoever reads a file stamps it BEFORE reading it: if it changes in the meantime, the
// old stamp reveals the change on the next check.
inline FileStamp stampFile(const std::filesystem::path& path)
{
    FileStamp s;
    s.path    = path;
    s.stamped = true;
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (!ec)
    {
        s.exists = true;
        s.mtime  = static_cast<int64_t>(t.time_since_epoch().count());
    }
    return s;
}

} // namespace DonTopo
