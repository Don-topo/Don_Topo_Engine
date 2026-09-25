#include "DonTopo/Editor/ThumbnailDiskCache.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>

namespace DonTopo {

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kMagic     = 0x48545444u;   // "DTTH"
constexpr uint32_t kMaxPath   = 4096;
constexpr uint32_t kMaxDeps   = 64;
constexpr size_t   kTileBytes = static_cast<size_t>(kThumbCell) * kThumbCell * 4;

std::string toUtf8(const fs::path& p)
{
    const std::u8string s = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

fs::path fromUtf8(const std::string& s)
{
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

// Absoluta y normalizada: la misma ruta pedida de dos formas da el mismo fichero.
std::string normalizedKey(const fs::path& p)
{
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec) abs = p;
    fs::path canon = fs::weakly_canonical(abs, ec);
    if (ec) canon = abs.lexically_normal();
    return toUtf8(canon);
}

uint64_t fnv1a(const std::string& s)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

void putU32(std::string& b, uint32_t v) { char t[4]; std::memcpy(t, &v, 4); b.append(t, 4); }
void putI64(std::string& b, int64_t v)  { char t[8]; std::memcpy(t, &v, 8); b.append(t, 8); }
void putStr(std::string& b, const std::string& s) { putU32(b, static_cast<uint32_t>(s.size())); b += s; }

struct Reader
{
    const std::string& b;
    size_t             pos = 0;

    bool raw(void* dst, size_t n)
    {
        if (b.size() - pos < n) return false;
        std::memcpy(dst, b.data() + pos, n);
        pos += n;
        return true;
    }
    bool u32(uint32_t& v) { return raw(&v, 4); }
    bool i64(int64_t& v)  { return raw(&v, 8); }
    bool str(std::string& s, uint32_t max)
    {
        uint32_t n = 0;
        if (!u32(n) || n > max || b.size() - pos < n) return false;
        s.assign(b.data() + pos, n);
        pos += n;
        return true;
    }
};

} // namespace

ThumbnailDiskCache::ThumbnailDiskCache(std::filesystem::path dir) : m_dir(std::move(dir)) {}

fs::path ThumbnailDiskCache::fileFor(const fs::path& asset) const
{
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.bin", static_cast<unsigned long long>(fnv1a(normalizedKey(asset))));
    return m_dir / name;
}

std::optional<ThumbnailResult> ThumbnailDiskCache::load(const fs::path& asset) const
{
    try
    {
        std::ifstream f(fileFor(asset), std::ios::binary);
        if (!f) return std::nullopt;
        const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        Reader r{ bytes };

        uint32_t    magic = 0, version = 0, status = 0, depCount = 0;
        std::string key;
        if (!r.u32(magic) || magic != kMagic) return std::nullopt;
        if (!r.u32(version) || version != kThumbDiskVersion) return std::nullopt;
        if (!r.u32(status) || status > static_cast<uint32_t>(ThumbnailStatus::AnimationOnly)) return std::nullopt;
        if (!r.str(key, kMaxPath) || key != normalizedKey(asset)) return std::nullopt;   // colision de hash
        if (!r.u32(depCount) || depCount == 0 || depCount > kMaxDeps) return std::nullopt;

        ThumbnailResult out;
        out.status = static_cast<ThumbnailStatus>(status);
        for (uint32_t i = 0; i < depCount; ++i)
        {
            std::string path;
            uint32_t    exists = 0;
            int64_t     mtime  = 0;
            if (!r.str(path, kMaxPath) || !r.u32(exists) || !r.i64(mtime)) return std::nullopt;
            ThumbnailDependency stored{ fromUtf8(path), exists != 0, mtime, true };
            if (!(stampFile(stored.path) == stored)) return std::nullopt;                // cambio algo
            out.dependencies.push_back(std::move(stored));
        }
        if (out.status == ThumbnailStatus::Ok)
        {
            out.rgba.resize(kTileBytes);
            if (!r.raw(out.rgba.data(), kTileBytes)) return std::nullopt;
        }
        if (r.pos != bytes.size()) return std::nullopt;
        return out;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

bool ThumbnailDiskCache::store(const fs::path& asset, const ThumbnailResult& res) const
{
    if (res.dependencies.empty()) return false;
    if (res.status == ThumbnailStatus::Ok && res.rgba.size() != kTileBytes) return false;
    try
    {
        std::string b;
        putU32(b, kMagic);
        putU32(b, kThumbDiskVersion);
        putU32(b, static_cast<uint32_t>(res.status));
        putStr(b, normalizedKey(asset));
        putU32(b, static_cast<uint32_t>(res.dependencies.size()));
        for (const ThumbnailDependency& d : res.dependencies)
        {
            putStr(b, toUtf8(d.path));
            putU32(b, d.exists ? 1u : 0u);
            putI64(b, d.mtime);
        }
        if (res.status == ThumbnailStatus::Ok)
            b.append(reinterpret_cast<const char*>(res.rgba.data()), res.rgba.size());

        auto fail = [this]() {
            if (!m_warned.exchange(true))
                std::fprintf(stderr, "[Thumbnails] no se puede escribir la cache en %s: las miniaturas se generan sin guardar\n",
                             m_dir.string().c_str());
            return false;
        };

        std::error_code ec;
        fs::create_directories(m_dir, ec);
        static std::atomic<uint64_t> counter{ 0 };
        const fs::path finalPath = fileFor(asset);
        fs::path tmp = finalPath;
        tmp += "." + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + "." +
               std::to_string(counter.fetch_add(1)) + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return fail();
            f.write(b.data(), static_cast<std::streamsize>(b.size()));
            if (!f)
            {
                f.close();
                fs::remove(tmp, ec);
                return fail();
            }
        }
        fs::rename(tmp, finalPath, ec);
        if (ec)
        {
            std::error_code ec2;
            fs::remove(tmp, ec2);
            return fail();
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

} // namespace DonTopo
