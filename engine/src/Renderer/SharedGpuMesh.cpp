#include "DonTopo/Renderer/SharedGpuMesh.h"
#include "DonTopo/Renderer/Mesh.h"

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
        // Vertex no tiene padding (14 floats seguidos, todos alineados a 4), así
        // que hashear sus bytes en crudo es determinista: no hay huecos sin
        // inicializar que metan ruido.
        static_assert(sizeof(Vertex) == 14 * sizeof(float),
                      "makeSharedMeshKey hashea Vertex en crudo: si gana padding, hay que hashear campo a campo");

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

        // Los discriminantes exactos van en claro delante del hash: para que dos
        // meshes DISTINTOS colisionen no basta con una colisión de FNV, tienen
        // que coincidir además en tamaños, paths y factores PBR. Compartir dos
        // mallas distintas sería corrupción visible, así que el coste de esta
        // cadena de más está justificado.
        char tail[64];
        std::snprintf(tail, sizeof(tail), "|%llu|%a|%a",
                      (unsigned long long)h, (double)m.metallic, (double)m.roughness);

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
        // Después de marcar el slot vivo: si create lanza, release/destroyAll
        // siguen viendo una entrada coherente que limpiar.
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

        // Copia ANTES de vaciar: el slot vuelve al freelist ya, y un acquire de
        // este mismo frame puede reutilizarlo mientras la destrucción de verdad
        // sigue encolada. Capturar la entrada por referencia sería destruir los
        // handles del inquilino nuevo.
        const SharedGpuMesh snapshot = e.gpu;

        m_byKey.erase(e.key);
        e = Entry{};
        m_freeSlots.push_back(index);

        destroy(snapshot);
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
