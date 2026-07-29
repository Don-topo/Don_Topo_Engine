#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace DonTopo
{
    struct Mesh;

    // Los recursos GPU que N objetos idénticos comparten. Todo lo que aquí hay
    // se deriva SOLO del contenido del Mesh y su Material: nada por instancia
    // (el transform y el nombre siguen en el RenderObject). Ese es el criterio
    // pa decidir si un campo entra o no: si dos cubos iguales en sitios
    // distintos pueden compartirlo, entra.
    struct SharedGpuMesh
    {
        VkBuffer        vertexBuffer  = VK_NULL_HANDLE;
        VkDeviceMemory  vertexMemory  = VK_NULL_HANDLE;
        VkBuffer        indexBuffer   = VK_NULL_HANDLE;
        VkDeviceMemory  indexMemory   = VK_NULL_HANDLE;
        uint32_t        indexCount    = 0;

        VkImage         textureImage  = VK_NULL_HANDLE;
        VkDeviceMemory  textureMem    = VK_NULL_HANDLE;
        VkImageView     textureView   = VK_NULL_HANDLE;
        VkSampler       sampler       = VK_NULL_HANDLE;

        VkImage         normalImage   = VK_NULL_HANDLE;
        VkDeviceMemory  normalMem     = VK_NULL_HANDLE;
        VkImageView     normalView    = VK_NULL_HANDLE;
        VkSampler       normalSampler = VK_NULL_HANDLE;

        VkImage         ormImage      = VK_NULL_HANDLE;
        VkDeviceMemory  ormMem        = VK_NULL_HANDLE;
        VkImageView     ormView       = VK_NULL_HANDLE;
        VkSampler       ormSampler    = VK_NULL_HANDLE;

        float           metallic      = 0.0f;
        float           roughness     = 0.5f;

        // Un solo descriptor set por entrada: sus cinco bindings (UBO, difusa,
        // normal, shadow, ORM) son idénticos entre objetos que comparten
        // malla y material. Lo por-objeto va por push constants.
        VkDescriptorSet descriptorSets[2] = {};

        // AABB en espacio local, para el frustum culling. hasBounds=false (mesh
        // sin vértices) significa "no se puede acotar": se dibuja siempre.
        glm::vec3       aabbMin{0.0f};
        glm::vec3       aabbMax{0.0f};
        bool            hasBounds     = false;

        // 0 = subido y visible. >0 = esperando a que la fence del batch con ese
        // ticket señale. Vive aquí y no en el RenderObject porque son los
        // recursos los que están en vuelo: un segundo objeto que adquiera esta
        // misma entrada antes del flush tiene que esperar igual.
        uint64_t        uploadTicket  = 0;
    };

    // Clave de contenido: dos Mesh que produzcan la misma clave generan
    // exactamente los mismos recursos GPU. Mezcla discriminantes exactos
    // (tamaños, paths de textura, metallic/roughness) con un FNV-1a de los
    // bytes de vértices, índices y texturas embebidas. El nombre del mesh y su
    // sourcePath NO entran: no afectan a un solo byte de lo que sube a GPU.
    std::string makeSharedMeshKey(const Mesh& mesh);

    // Tabla de recursos GPU compartidos con refcount. No conoce Vulkan más allá
    // de los handles: crear y destruir son callbacks del caller (el Renderer los
    // rellena con sus createVertexBuffer/DeferredDelete). Eso es lo que la hace
    // testeable sin device.
    class SharedGpuMeshCache
    {
        public:
            using Creator   = std::function<void(SharedGpuMesh&)>;
            using Destroyer = std::function<void(const SharedGpuMesh&)>;

            // Devuelve el índice de la entrada de `key`, creándola con `create`
            // solo la primera vez. En las siguientes llamadas incrementa el
            // refcount y NO invoca `create`. createdOut (si se pasa) dice cuál
            // de los dos casos ha sido: el caller lo necesita pa saber si tiene
            // que alojar el descriptor set o si ya venía alojado.
            int acquire(const std::string& key, const Creator& create,
                        bool* createdOut = nullptr);

            // Decrementa el refcount. Al llegar a 0 saca la entrada de la tabla,
            // libera su slot y pasa una COPIA de los handles a `destroy` — el
            // slot puede reutilizarse en el mismo frame mientras la destrucción
            // real sigue diferida. No-op si el índice no está vivo.
            void release(int index, const Destroyer& destroy);

            // Fuerza la destrucción de todo lo vivo, ignorando refcounts. SOLO
            // desde Renderer::shutdown, donde ya no queda nadie que dibuje.
            void destroyAll(const Destroyer& destroy);

            SharedGpuMesh*       get(int index);
            const SharedGpuMesh* get(int index) const;

            // 0 si el índice no está vivo.
            int    refCount(int index) const;
            size_t liveCount() const;
            // Índices vivos, en orden creciente. Lo usa createDescriptorSets pa
            // recorrer entradas en vez de objetos.
            std::vector<int> liveIndices() const;

        private:
            struct Entry
            {
                SharedGpuMesh gpu;
                std::string   key;
                int           refs = 0;
                bool          live = false;
            };

            std::vector<Entry>                   m_entries;
            std::unordered_map<std::string, int> m_byKey;
            std::vector<int>                     m_freeSlots;
    };
}
