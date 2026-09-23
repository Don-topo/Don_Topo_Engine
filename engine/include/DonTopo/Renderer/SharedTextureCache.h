#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace DonTopo
{
    // Qué se sube: el mismo fichero como color (sRGB) o como normal/ORM
    // (lineal) son imágenes distintas.
    enum class TextureKind : uint8_t { BaseColor, Normal, Orm };

    // Clave de contenido de una textura de material. De fichero: la ruta. Embebida
    // en el FBX: tamaño + FNV-1a de los bytes, así que dos FBX con la misma
    // textura dentro comparten imagen, y la ruta no cuenta. Vacía si no hay
    // textura: esos materiales usan la blanca de relleno, que ya se presta por
    // su cuenta (GpuResources::releaseMaterialImage) y NO debe entrar aquí.
    inline std::string makeTextureKey(const std::string& path, const std::vector<uint8_t>& embedded,
                                      TextureKind kind, const std::string& settingsSuffix = {})
    {
        if (path.empty() && embedded.empty()) return {};
        const char tipo = kind == TextureKind::BaseColor ? 'c' : (kind == TextureKind::Normal ? 'n' : 'o');
        // Los ajustes de importacion son de un FICHERO: sin ruta no hay sidecar.
        // Por valor a proposito: una referencia ligada a un ternario que mezcla
        // un temporal y un lvalue dependeria de la extension de vida del temporal.
        const std::string sufijo = path.empty() ? std::string() : settingsSuffix;
        if (!embedded.empty())
        {
            uint64_t h = 1469598103934665603ull;
            for (uint8_t b : embedded) { h ^= b; h *= 1099511628211ull; }
            return std::string(1, tipo) + ":emb:" + std::to_string(embedded.size()) + ":" + std::to_string(h) + sufijo;
        }
        return std::string(1, tipo) + ":" + path + sufijo;
    }

    // Texturas de material compartidas con recuento de referencias. No sabe nada
    // de la GPU: crear y destruir son del backend, que es lo que la deja probar
    // sin device (shared_texture_cache_tests). Pocas entradas por escena, así
    // que un vector lineal basta.
    template <typename Handle>
    class SharedTextureCache
    {
    public:
        // El handle de `key`, creándolo con `create` solo la primera vez. Con
        // clave vacía, o si `create` devuelve un handle vacío (Handle{}: no se
        // pudo crear), no se guarda nada: no es de la caché.
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

        // Una referencia menos; a cero, fuera de la tabla y `destroy`. Un handle
        // que no está (el relleno, o ya soltado) es un no-op: quien lo pidió
        // sigue su camino de siempre.
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
