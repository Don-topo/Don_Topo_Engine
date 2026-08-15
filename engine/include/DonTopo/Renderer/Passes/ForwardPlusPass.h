#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include "DonTopo/Renderer/RendererState.h"
#include "DonTopo/Renderer/UniformBufferObject.h"

namespace DonTopo {

class GpuDevice;
class GpuResources;

// Forward+ : el dispatch de culling de luces y los buffers que consume
// pbr.frag. A diferencia de los demas pases, este NO escribe una imagen: llena
// una rejilla que lee el pass de ESCENA, asi que su descriptor set y su layout
// salen a la interfaz publica.
//
// Ataduras con codigo que no es suyo:
//  - descLayout(): es el set 2 del pipeline layout de escena.
//  - set(frame): lo bindea el pass de escena y tambien el bakeo de sondas.
//  - overrideModeOff()/restoreParams(): el bakeo de sondas apaga el Forward+
//    mientras captura las seis caras, porque la rejilla se culleo contra el
//    frustum de la camara del frame y no contra esas caras.
class ForwardPlusPass {
public:
    // Debe coincidir con Renderer::MAX_FRAMES (comprobado con static_assert en Renderer.cpp).
    static constexpr int kFramesInFlight = 2;

    // Tope de luces que entran en el culling y, a la vez, ancho de la
    // mascara de bits de light_cull_tiled.comp (256 / 32 = 8 palabras).
    static constexpr uint32_t kMaxLights     = 256;
    // Tope de luces por celda. Una celda que se pase PIERDE luces: por
    // eso se cuentan aparte y se enseñan en la UI.
    static constexpr uint32_t kMaxPerCell    = 64;
    static constexpr uint32_t kTileSize      = 16;   // tiled
    static constexpr uint32_t kClusterTile   = 64;   // clustered, XY
    static constexpr uint32_t kClusterSlices = 24;   // clustered, Z

    // Bloque de parametros tal cual lo declaran los dos .comp y pbr.frag.
    // std430 con puros escalares de 4 bytes: los offsets son secuenciales.
    struct ParamsGpu {
        uint32_t mode;
        uint32_t gridX;
        uint32_t gridY;
        uint32_t gridZ;
        uint32_t tileSize;
        uint32_t maxPerCell;
        uint32_t numLights;
        uint32_t pad0;
        float    zNear;
        float    zFar;
        float    sliceScale;
        float    sliceBias;
    };
    static_assert(sizeof(ParamsGpu) == 48, "ParamsGpu debe seguir en 48 bytes: los dos .comp y pbr.frag declaran este layout");

    struct Context {
        GpuDevice&        gpu;
        GpuResources&     res;
        // Resolucion INTERNA del render y NO la del swapchain: con SSAA el
        // render es mayor que la ventana, y dimensionar con el de la ventana
        // dejaria a pbr.frag leyendo celdas fuera del buffer.
        const VkExtent2D& renderExtent;
        int               currentFrame;
        // Modo CONGELADO del frame, no el que pide la UI.
        RendererState::FpMode activeMode;
        // La profundidad del depth pre-pass y su sampler: el culling tiled
        // reduce el maximo de profundidad de cada tile a partir de ella.
        const VkImageView*    depthView;   // [kFramesInFlight]
        VkSampler             depthSampler;
        bool                  timestampsSupported;
        float                 timestampPeriod;
    };

    ForwardPlusPass()                                  = default;
    ForwardPlusPass(const ForwardPlusPass&)            = delete;
    ForwardPlusPass& operator=(const ForwardPlusPass&) = delete;

    // Layout, pool, los dos pipelines, los buffers que NO dependen del tamano
    // (parametros, luces y contadores, todos con mapeo persistente) y el pool
    // de queries. Una sola vez, en el init.
    void createPipelines(const Context& ctx);
    void destroyPipelines(const Context& ctx);
    // La rejilla y la lista de indices, mas los descriptor sets: dependen del
    // tamano, asi que van con el swapchain.
    void createBuffers(const Context& ctx);
    void destroyBuffers(const Context& ctx);

    // El dispatch de culling del modo activo. Va DESPUES del depth pre-pass
    // (el tiled lo necesita) y ANTES del pass de escena.
    void record(const Context& ctx, VkCommandBuffer cmd, const glm::mat4& proj);
    // Bloque de parametros y lista de luces del frame. Se escribe SIEMPRE,
    // tambien en Off: pbr.frag lee el modo de aqui para decidir por que rama
    // va.
    void uploadFrameData(const Context& ctx, const glm::mat4& view, const glm::mat4& proj,
                         const std::vector<Light>& lights,
                         const std::vector<float>& lightRadii, float defaultRadius);

    // Dimensiones de la rejilla del modo dado con el extent del contexto.
    void gridDims(const Context& ctx, RendererState::FpMode mode,
                  uint32_t& gridX, uint32_t& gridY, uint32_t& gridZ, uint32_t& tileSize) const;

    // El set 2 del pipeline de escena y su layout.
    VkDescriptorSetLayout descLayout()    const { return m_descLayout; }
    VkDescriptorSet       set(int frame)  const { return m_sets[frame]; }

    // Deja el modo en Off sin tocar el que pide la UI, y devuelve lo que habia
    // para restaurarlo. false si el buffer aun no existe.
    bool overrideModeOff(ParamsGpu& saved);
    void restoreParams(const ParamsGpu& saved);

    float    gpuMs()         const { return m_gpuMs; }
    float    avgPerCell()    const { return m_avgPerCell; }
    uint32_t overflowCells() const { return m_overflowCells; }

private:
    VkDescriptorSetLayout m_descLayout                      = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool                        = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout                  = VK_NULL_HANDLE;
    VkPipeline            m_tiledPipeline                   = VK_NULL_HANDLE;
    VkPipeline            m_clusteredPipeline               = VK_NULL_HANDLE;
    VkDescriptorSet       m_sets[kFramesInFlight]           = {};
    // Mapeo persistente, igual que el UBO.
    VkBuffer              m_paramsBuffer[kFramesInFlight]   = {};
    VkDeviceMemory        m_paramsMemory[kFramesInFlight]   = {};
    void*                 m_paramsMapped[kFramesInFlight]   = {};
    VkBuffer              m_lightBuffer[kFramesInFlight]    = {};
    VkDeviceMemory        m_lightMemory[kFramesInFlight]    = {};
    void*                 m_lightMapped[kFramesInFlight]    = {};
    VkBuffer              m_statsBuffer[kFramesInFlight]    = {};
    VkDeviceMemory        m_statsMemory[kFramesInFlight]    = {};
    void*                 m_statsMapped[kFramesInFlight]    = {};
    // Dependen del tamano de la rejilla.
    VkBuffer              m_gridBuffer[kFramesInFlight]     = {};
    VkDeviceMemory        m_gridMemory[kFramesInFlight]     = {};
    VkBuffer              m_indexBuffer[kFramesInFlight]    = {};
    VkDeviceMemory        m_indexMemory[kFramesInFlight]    = {};
    VkQueryPool           m_queryPool                       = VK_NULL_HANDLE;
    bool                  m_queryPending[kFramesInFlight]   = {};
    float                 m_gpuMs                           = 0.0f;
    float                 m_avgPerCell                      = 0.0f;
    uint32_t              m_overflowCells                   = 0;
    uint32_t              m_measuredFrames                  = 0;
};

} // namespace DonTopo
