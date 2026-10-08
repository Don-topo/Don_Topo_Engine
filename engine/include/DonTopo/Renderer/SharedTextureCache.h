#pragma once
#include "DonTopo/Renderer/Material.h"

#include <cassert>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace DonTopo
{
    // What gets uploaded: the same file as color (sRGB) or as normal/ORM
    // (linear) are different images.
    enum class TextureKind : uint8_t { BaseColor, Normal, Orm };

    // FNV-1a 64 of embedded texture bytes: what the key of an embedded texture is
    // made of. ModelLoader stores it in Material::embedded*Hash when it fills the
    // bytes, so the key does not hash them again per registration.
    inline uint64_t hashEmbeddedTexture(const std::vector<uint8_t>& bytes)
    {
        uint64_t h = 1469598103934665603ull;
        for (uint8_t b : bytes) { h ^= b; h *= 1099511628211ull; }
        return h;
    }

    // Sets the three Material::embedded*Hash from the bytes (0 where there are
    // none). Whoever fills embedded bytes calls it right after.
    inline void hashEmbeddedTextures(Material& m)
    {
        m.embeddedTextureHash           = m.embeddedTexture.empty() ? 0 : hashEmbeddedTexture(m.embeddedTexture);
        m.embeddedNormalMapHash         = m.embeddedNormalMap.empty() ? 0 : hashEmbeddedTexture(m.embeddedNormalMap);
        m.embeddedMetallicRoughnessHash = m.embeddedMetallicRoughness.empty()
                                              ? 0 : hashEmbeddedTexture(m.embeddedMetallicRoughness);
    }

    // Content key of a material texture: it names what the upload DECODES, which
    // is the path when there is one, else the embedded bytes -- the same choice
    // as chooseTextureSource / decodeMaterialTexture (material_texture_tests ties
    // the two). From a file: the path (+ its import-settings suffix). Embedded in
    // the FBX: size + FNV-1a of the bytes, so two FBX with the same texture inside
    // share an image. Both set happens after an override (a .mat path on an FBX
    // with the texture embedded): the path decodes, so the path is the key; with
    // the bytes as key it shared one image with the plain FBX. Empty if there is no
    // texture: those materials use the white filler, which is already lent out on
    // its own (GpuResources::releaseMaterialImage) and must NOT go in here.
    // embeddedHash: hashEmbeddedTexture(embedded) when the caller has it
    // (Material::embedded*Hash); 0 = hash the bytes here.
    inline std::string makeTextureKey(const std::string& path, const std::vector<uint8_t>& embedded,
                                      TextureKind kind, const std::string& settingsSuffix = {},
                                      uint64_t embeddedHash = 0)
    {
        if (path.empty() && embedded.empty()) return {};
        const char tipo = kind == TextureKind::BaseColor ? 'c' : (kind == TextureKind::Normal ? 'n' : 'o');
        if (path.empty())
        {
            // A stale hash would silently share the wrong image: Debug checks it.
            assert(embeddedHash == 0 || embeddedHash == hashEmbeddedTexture(embedded));
            const uint64_t h = embeddedHash != 0 ? embeddedHash : hashEmbeddedTexture(embedded);
            // Import settings belong to a FILE: without a path there is no sidecar.
            return std::string(1, tipo) + ":emb:" + std::to_string(embedded.size()) + ":" + std::to_string(h);
        }
        return std::string(1, tipo) + ":" + path + settingsSuffix;
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
