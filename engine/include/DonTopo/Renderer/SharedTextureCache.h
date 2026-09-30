#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace DonTopo
{
    // What gets uploaded: the same file as color (sRGB) or as normal/ORM
    // (linear) are different images.
    enum class TextureKind : uint8_t { BaseColor, Normal, Orm };

    // Content key of a material texture. From a file: the path. Embedded
    // in the FBX: size + FNV-1a of the bytes, so two FBX with the same
    // texture inside share an image, and the path does not count. Empty if there is no
    // texture: those materials use the white filler, which is already lent out on
    // its own (GpuResources::releaseMaterialImage) and must NOT go in here.
    inline std::string makeTextureKey(const std::string& path, const std::vector<uint8_t>& embedded,
                                      TextureKind kind, const std::string& settingsSuffix = {})
    {
        if (path.empty() && embedded.empty()) return {};
        const char tipo = kind == TextureKind::BaseColor ? 'c' : (kind == TextureKind::Normal ? 'n' : 'o');
        // Import settings belong to a FILE: without a path there is no sidecar.
        // By value on purpose: a reference bound to a ternary that mixes
        // a temporary and an lvalue would depend on the temporary's lifetime extension.
        const std::string sufijo = path.empty() ? std::string() : settingsSuffix;
        if (!embedded.empty())
        {
            uint64_t h = 1469598103934665603ull;
            for (uint8_t b : embedded) { h ^= b; h *= 1099511628211ull; }
            return std::string(1, tipo) + ":emb:" + std::to_string(embedded.size()) + ":" + std::to_string(h) + sufijo;
        }
        return std::string(1, tipo) + ":" + path + sufijo;
    }

    // Reference-counted shared material textures. It knows nothing about the GPU:
    // creating and destroying belong to the backend, which is what lets it be tested
    // without a device (shared_texture_cache_tests). Few entries per scene, so
    // a linear vector is enough.
    template <typename Handle>
    class SharedTextureCache
    {
    public:
        // The handle of `key`, creating it with `create` only the first time. With an empty
        // key, or if `create` returns an empty handle (Handle{}: it could not
        // be created), nothing is stored: it does not belong to the cache.
        Handle acquire(const std::string& key, const std::function<Handle()>& create,
                       bool* createdOut = nullptr)
        {
            if (!key.empty())
                for (auto& e : m_entries)
                    if (e.key == key)
                    {
                        ++e.refs;
                        if (createdOut) *createdOut = false;
                        return e.handle;
                    }
            if (createdOut) *createdOut = true;
            Handle h = create();
            if (!key.empty() && !(h == Handle{})) m_entries.push_back({ key, h, 1 });
            return h;
        }

        // One reference less; at zero, out of the table and `destroy`. A handle
        // that is not there (the filler, or already released) is a no-op: whoever asked for it
        // goes on its usual way.
        void release(const Handle& h, const std::function<void(const Handle&)>& destroy)
        {
            for (size_t i = 0; i < m_entries.size(); i++)
            {
                if (!(m_entries[i].handle == h)) continue;
                if (--m_entries[i].refs > 0) return;
                const Handle copia = m_entries[i].handle;
                m_entries.erase(m_entries.begin() + i);
                destroy(copia);
                return;
            }
        }

        bool contains(const Handle& h) const
        {
            for (const auto& e : m_entries) if (e.handle == h) return true;
            return false;
        }
        int refCount(const std::string& key) const
        {
            for (const auto& e : m_entries) if (e.key == key) return e.refs;
            return 0;
        }
        size_t size() const { return m_entries.size(); }

    private:
        struct Entry { std::string key; Handle handle; int refs; };
        std::vector<Entry> m_entries;
    };
}
