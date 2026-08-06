#pragma once

// Lote de sprites 2D de la UI de juego.
//
// Dos mitades bien separadas:
//   - build(): CPU pura. Recorre la jerarquía del canvas y saca vértices,
//     índices y lotes. No toca Vulkan, y es lo que ejercitan los tests.
//   - el resto: sube esos datos a los buffers del frame en vuelo y graba los
//     draws DENTRO del pass de composición del Renderer (LDR, ya tonemapeado).
//
// El lote rompe al cambiar de atlas o de scissor, que son los dos únicos
// estados que el draw no puede llevar por vértice.

#include "DonTopo/UI/UiTextureAtlas.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace DonTopo
{
    class GpuDevice;
    class GpuResources;
    class UiCanvas;

    // Posición en PÍXELES de pantalla, con (0,0) arriba a la izquierda: la
    // proyección ortográfica del vertex shader es quien la lleva a NDC.
    struct UiVertex
    {
        glm::vec2 pos{0.0f};
        glm::vec2 uv{0.0f};
        glm::vec4 color{1.0f};

        // Todo lo que distingue un quad de texto de uno de sprite viaja POR
        // VÉRTICE, no por pipeline ni por descriptor: así el texto cae en el
        // mismo lote que el panel que tiene detrás.
        //   params.x = modo: 0 = sprite/color plano, 1 = MSDF
        //   params.y = screenPxRange YA escalado al tamaño de este quad
        //   params.z = grosor del outline en píxeles de pantalla
        //   effect   = color del outline (con la opacidad del árbol ya aplicada)
        // Con params.x = 0 el shader hace literalmente lo de siempre.
        glm::vec4 params{0.0f};
        glm::vec4 effect{0.0f};
    };

    struct UiScissor
    {
        int32_t  x = 0;
        int32_t  y = 0;
        uint32_t width  = 0;
        uint32_t height = 0;

        bool empty() const { return width == 0 || height == 0; }
        bool operator==(const UiScissor& o) const
        {
            return x == o.x && y == o.y && width == o.width && height == o.height;
        }
        bool operator!=(const UiScissor& o) const { return !(*this == o); }
    };

    struct UiBatch
    {
        // nullptr = textura blanca de 1x1 del propio UiSpriteBatch (paneles de
        // color plano). Es también la clave de agrupado: dos nodos con el mismo
        // puntero y el mismo scissor caen en el mismo lote.
        const UiTextureAtlas* atlas = nullptr;
        UiScissor scissor{};
        uint32_t  firstIndex = 0;
        uint32_t  indexCount = 0;
    };

    struct UiDrawData
    {
        std::vector<UiVertex> vertices;
        std::vector<uint16_t> indices;
        std::vector<UiBatch>  batches;

        bool empty() const { return batches.empty(); }
        void clear()
        {
            vertices.clear();
            indices.clear();
            batches.clear();
        }
    };

    class UiSpriteBatch
    {
    public:
        static constexpr int kFrames = 2;   // los mismos MAX_FRAMES del Renderer

        // --- CPU ---------------------------------------------------------------
        static void build(const UiCanvas& canvas, uint32_t width, uint32_t height, UiDrawData& out);

        // --- GPU ---------------------------------------------------------------
        void init(GpuDevice& gpu, GpuResources& res, VkRenderPass renderPass, VkSampleCountFlagBits samples);
        void recreatePipeline(GpuDevice& gpu, VkRenderPass renderPass, VkSampleCountFlagBits samples);
        void shutdown(GpuDevice& gpu);

        // Reserva y escribe el descriptor set del atlas. Sin esto el atlas se
        // dibujaría con el set de otro, que es un fallo mudo.
        bool registerAtlas(GpuDevice& gpu, UiTextureAtlas& atlas);

        // Con datos vacíos no graba ni un comando ni toca ningún buffer.
        void record(GpuDevice& gpu, VkCommandBuffer cmd, const UiDrawData& data,
                    VkExtent2D extent, int frame);

    private:
        void createPipeline(GpuDevice& gpu, VkRenderPass renderPass, VkSampleCountFlagBits samples);
        void ensureBuffers(GpuDevice& gpu, int frame, uint32_t vertexCount, uint32_t indexCount);
        void destroyBuffers(GpuDevice& gpu, int frame);

        VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
        VkDescriptorPool      m_descPool   = VK_NULL_HANDLE;
        VkPipelineLayout      m_layout     = VK_NULL_HANDLE;
        VkPipeline            m_pipeline   = VK_NULL_HANDLE;
        VkSampler             m_sampler    = VK_NULL_HANDLE;

        // Blanco de 1x1 para los nodos sin atlas: multiplicar por (1,1,1,1) deja
        // el color del vértice tal cual, así que un panel plano no necesita ni
        // pipeline aparte ni rama en el shader.
        VkImage         m_whiteImage  = VK_NULL_HANDLE;
        VkDeviceMemory  m_whiteMemory = VK_NULL_HANDLE;
        VkImageView     m_whiteView   = VK_NULL_HANDLE;
        VkDescriptorSet m_whiteSet    = VK_NULL_HANDLE;

        // Un par de buffers por frame en vuelo, con mapeo persistente y
        // crecimiento por duplicación, igual que el SSBO de instancias. Se crean
        // en el PRIMER frame con algo que dibujar, no en init.
        VkBuffer       m_vertexBuffers[kFrames]  = {};
        VkDeviceMemory m_vertexMemory[kFrames]   = {};
        void*          m_vertexMapped[kFrames]   = {};
        uint32_t       m_vertexCapacity[kFrames] = {};

        VkBuffer       m_indexBuffers[kFrames]  = {};
        VkDeviceMemory m_indexMemory[kFrames]   = {};
        void*          m_indexMapped[kFrames]   = {};
        uint32_t       m_indexCapacity[kFrames] = {};
    };
}
