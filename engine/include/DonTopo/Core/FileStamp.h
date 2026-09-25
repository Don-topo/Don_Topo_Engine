#pragma once
#include <cstdint>
#include <filesystem>
#include <system_error>

namespace DonTopo {

// Un fichero y su estado en un momento dado. Lo usan las miniaturas del Content
// Browser para saber cuando regenerar: una miniatura depende de su asset y de lo
// que ese asset lee (texturas externas, sidecar, .mtl).
struct FileStamp
{
    std::filesystem::path path;
    bool                  exists  = false;
    int64_t               mtime   = 0;       // file_time_type::time_since_epoch().count(); 0 si no existe
    bool                  stamped = false;   // false = solo la ruta, sin mirar aun el disco
};

// path, exists y mtime. `stamped` no cuenta: es como se obtuvo, no que fichero es.
inline bool operator==(const FileStamp& a, const FileStamp& b)
{
    return a.exists == b.exists && a.mtime == b.mtime && a.path == b.path;
}

// Estado de un fichero AHORA. Nunca lanza: si no se puede leer, exists = false.
// Quien lee un fichero lo sella ANTES de leerlo: si cambia mientras tanto, el
// sello viejo delata el cambio en la siguiente comprobacion.
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
