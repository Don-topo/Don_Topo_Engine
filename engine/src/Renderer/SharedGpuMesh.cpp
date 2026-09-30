#include "DonTopo/Renderer/SharedGpuMesh.h"
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/TextureImport.h"

#include <algorithm>
#include <cstdio>

namespace DonTopo
{
    namespace
    {
        constexpr uint64_t kFnvOffset = 1469598103934665603ull;
        constexpr uint64_t kFnvPrime  = 1099511628211ull;

        uint64_t fnv1a(const void* data, size_t bytes, uint64_t h)
        {
            const uint8_t* p = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < bytes; ++i)
            {
                h ^= p[i];
                h *= kFnvPrime;
            }
            return h;
        }
    }

    std::string makeSharedMeshKey(const Mesh& mesh)
    {
        // Vertex has no padding (14 consecutive floats, all 4-aligned), so
        // hashing its raw bytes is deterministic: there are no uninitialized gaps
        // that add noise.
        static_assert(sizeof(Vertex) == 14 * sizeof(float),
                      "makeSharedMeshKey hashes Vertex raw: if it gains padding, it must be hashed field by field");

        uint64_t h = kFnvOffset;
        if (!mesh.vertices.empty())
            h = fnv1a(mesh.vertices.data(), mesh.vertices.size() * sizeof(Vertex), h);
        if (!mesh.indices.empty())
            h = fnv1a(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t), h);

        const Material& m = mesh.material;
        if (!m.embeddedTexture.empty())
            h = fnv1a(m.embeddedTexture.data(), m.embeddedTexture.size(), h);
        if (!m.embeddedNormalMap.empty())
            h = fnv1a(m.embeddedNormalMap.data(), m.embeddedNormalMap.size(), h);
        if (!m.embeddedMetallicRoughness.empty())
            h = fnv1a(m.embeddedMetallicRoughness.data(), m.embeddedMetallicRoughness.size(), h);

        // The exact discriminants go in the clear ahead of the hash: for two
        // DIFFERENT meshes to collide an FNV collision is not enough, they
        // must also match in sizes and paths. Sharing two different
        // meshes would be a visible corruption, so the cost of this extra
        // string is justified.
        //
        // metallic and roughness WERE here and were removed on purpose: they are per
        // object (RenderObject) and travel by push constant, so they say
        // nothing about the GPU resources this key names. While
        // they were here, changing a number forced re-keying the object and
        // rebuilding its resources (waitForGpu and three textures back), which is
        // exactly why the sliders could not apply live; and
        // incidentally two identical cubes with different metallic did not share VRAM.
        // What DOES remain here is the PATH of the ORM map, which names a real
        // texture.
        char tail[64];
        std::snprintf(tail, sizeof(tail), "|%llu", (unsigned long long)h);

        std::string key;
        key.reserve(m.texturePath.size() + m.normalMapPath.size()
                    + m.metallicRoughnessPath.size() + 96);
        key += std::to_string(mesh.vertices.size());
        key += '|';
        key += std::to_string(mesh.indices.size());
        key += '|';
        key += m.texturePath;
        key += '|';
        key += m.normalMapPath;
        key += '|';
        key += m.metallicRoughnessPath;
        key += '|';
        key += std::to_string(m.embeddedTexture.size());
        key += '|';
        key += std::to_string(m.embeddedNormalMap.size());
        key += '|';
        key += std::to_string(m.embeddedMetallicRoughness.size());
        key += tail;

        // Import settings of the three textures, AT THE END (the geometry
        // prefix that rebuildStaticMesh compares is the first two fields) and
        // only if any is not the usual one: today's keys do not change.
        const std::string ts0 = textureKeySuffix(m.texturePath);
        const std::string ts1 = textureKeySuffix(m.normalMapPath);
        const std::string ts2 = textureKeySuffix(m.metallicRoughnessPath);
        if (!ts0.empty() || !ts1.empty() || !ts2.empty())
        {
            key += "|ts";
            key += ts0; key += '|'; key += ts1; key += '|'; key += ts2;
        }
        return key;
    }

    int SharedGpuMeshCache::acquire(const std::string& key, const Creator& create,
                                    bool* createdOut)
    {
        auto it = m_byKey.find(key);
        if (it != m_byKey.end())
        {
            Entry& e = m_entries[(size_t)it->second];
            ++e.refs;
            if (createdOut) *createdOut = false;
            return it->second;
        }

        int index;
        if (!m_freeSlots.empty())
        {
            index = m_freeSlots.back();
            m_freeSlots.pop_back();
        }
        else
        {
            m_entries.emplace_back();
            index = (int)m_entries.size() - 1;
        }

        Entry& e = m_entries[(size_t)index];
        e.gpu  = SharedGpuMesh{};
        e.key  = key;
        e.refs = 1;
        e.live = true;
        // After marking the slot alive: if create throws, release/destroyAll
        // still see a consistent entry to clean up.
        create(e.gpu);

        m_byKey.emplace(key, index);
        if (createdOut) *createdOut = true;
        return index;
    }

    void SharedGpuMeshCache::release(int index, const Destroyer& destroy)
    {
        if (index < 0 || index >= (int)m_entries.size()) return;
        Entry& e = m_entries[(size_t)index];
        if (!e.live) return;

        if (--e.refs > 0) return;

        // Copy BEFORE emptying: the slot goes back to the freelist right away, and an acquire of
        // this same frame can reuse it while the real destruction
        // is still queued. Capturing the entry by reference would destroy
        // the new tenant's handles.
        const SharedGpuMesh snapshot = e.gpu;

        m_byKey.erase(e.key);
        e = Entry{};
        m_freeSlots.push_back(index);

        destroy(snapshot);
    }

    bool SharedGpuMeshCache::rekey(int index, const std::string& newKey)
    {
        if (index < 0 || index >= (int)m_entries.size()) return false;
        Entry& e = m_entries[(size_t)index];
        if (!e.live) return false;
        if (e.key == newKey) return true;

        auto choque = m_byKey.find(newKey);
        if (choque != m_byKey.end() && choque->second != index) return false;

        m_byKey.erase(e.key);
        e.key = newKey;
        m_byKey[newKey] = index;
        return true;
    }

    void SharedGpuMeshCache::destroyAll(const Destroyer& destroy)
    {
        for (Entry& e : m_entries)
        {
            if (!e.live) continue;
            const SharedGpuMesh snapshot = e.gpu;
            e = Entry{};
            destroy(snapshot);
        }
        m_entries.clear();
        m_byKey.clear();
        m_freeSlots.clear();
    }

    SharedGpuMesh* SharedGpuMeshCache::get(int index)
    {
        if (index < 0 || index >= (int)m_entries.size()) return nullptr;
        Entry& e = m_entries[(size_t)index];
        return e.live ? &e.gpu : nullptr;
    }

    const SharedGpuMesh* SharedGpuMeshCache::get(int index) const
    {
        if (index < 0 || index >= (int)m_entries.size()) return nullptr;
        const Entry& e = m_entries[(size_t)index];
        return e.live ? &e.gpu : nullptr;
    }

    const std::string& SharedGpuMeshCache::keyOf(int index) const
    {
        // Static and not a temporary: it is returned by reference, so it has
        // to outlive the call just like the key of a live entry.
        static const std::string kSinClave;
        if (index < 0 || index >= (int)m_entries.size()) return kSinClave;
        const Entry& e = m_entries[(size_t)index];
        return e.live ? e.key : kSinClave;
    }

    int SharedGpuMeshCache::refCount(int index) const
    {
        if (index < 0 || index >= (int)m_entries.size()) return 0;
        const Entry& e = m_entries[(size_t)index];
        return e.live ? e.refs : 0;
    }

    size_t SharedGpuMeshCache::liveCount() const
    {
        return (size_t)std::count_if(m_entries.begin(), m_entries.end(),
                                     [](const Entry& e) { return e.live; });
    }

    std::vector<int> SharedGpuMeshCache::liveIndices() const
    {
        std::vector<int> out;
        out.reserve(m_entries.size());
        for (size_t i = 0; i < m_entries.size(); ++i)
            if (m_entries[i].live) out.push_back((int)i);
        return out;
    }
}
