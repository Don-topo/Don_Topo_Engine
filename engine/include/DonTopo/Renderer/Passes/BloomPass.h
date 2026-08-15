#pragma once
#include <vulkan/vulkan.h>

namespace DonTopo {

class GpuDevice;
class RendererState;

// Bloom: cadena de mips a media resolucion (bajada con umbral + subida
// acumulativa). Lo que NO es suyo y por eso se queda en el Renderer: el pass de
// COMPOSICION, que suma el mip 0 sobre el HDR, tonemapea y ademas hospeda el
// contorno, los gizmos y la UI de juego.
//
// Dos ataduras con ese vecino, y por eso salen a la interfaz publica:
//  - sampler() y mipView0(): el descriptor set de la composicion los necesita.
//  - queryPool(): el par de timestamps mide "bloom + composicion", asi que el
//    cierre se escribe DESPUES del render pass de composicion, que graba el
//    Renderer.
class BloomPass {
public:
    // Debe coincidir con Renderer::MAX_FRAMES (comprobado con static_assert en Renderer.cpp).
    static constexpr int      kFramesInFlight = 2;
    static constexpr uint32_t kMaxMips        = 5;

    struct Context {
        GpuDevice&           gpu;
        const RendererState& state;
        // Resolucion INTERNA del render: la cadena arranca a la mitad de esta.
        const VkExtent2D&    renderExtent;
        // La del swapchain, solo para el informe de medida.
        const VkExtent2D&    swapChainExtent;
        int                  currentFrame;
        // El target de escena: el mip 0 lo lee con umbral.
        VkFormat             hdrFormat;
        const VkImageView*   hdrView;   // [kFramesInFlight]
        // Los resolvio el Renderer justo antes de crear los pipelines.
        bool                 timestampsSupported;
        float                timestampPeriod;
    };

    BloomPass()                            = default;
    BloomPass(const BloomPass&)            = delete;
    BloomPass& operator=(const BloomPass&) = delete;

    // Lo que no depende del tamano: sampler, layout, pool, los dos pipelines y
    // el pool de queries. Una sola vez, en el init.
    void createPipelines(const Context& ctx);
    void destroyPipelines(const Context& ctx);
    // La cadena de mips y sus sets: van con el swapchain.
    void createImages(const Context& ctx);
    void destroyImages(const Context& ctx);

    // Bajada + subida. No graba nada sin cadena (viewport diminuto).
    void record(const Context& ctx, VkCommandBuffer cmd);
    // Con el bloom apagado la composicion sigue muestreando la cadena, asi que
    // hay que dejarla en negro y en GENERAL. Pasa UNA vez por imagen (al
    // crearla y al apagar el efecto), no cada frame.
    void recordClear(const Context& ctx, VkCommandBuffer cmd);
    // Abre el par de timestamps del frame y lee el de hace dos. Va antes de
    // record(); el cierre lo escribe el Renderer tras la composicion.
    void beginQuery(const Context& ctx, VkCommandBuffer cmd);
    // Con el bloom apagado: la medida se anula y el par no se abre.
    void skipQuery(const Context& ctx);

    // Niveles realmente usados: un viewport pequeno no da para kMaxMips. Con 0
    // no hay nada que sumar y la composicion fuerza la intensidad a cero.
    uint32_t    mipCount() const { return m_mipCount; }
    // Lo que necesita el descriptor set de la composicion.
    VkImageView mipView0(int frame) const { return m_mipView[frame][0]; }
    VkSampler   sampler()           const { return m_sampler; }
    VkQueryPool queryPool()         const { return m_queryPool; }
    float       gpuMs()             const { return m_gpuMs; }
    // Al apagar el efecto hay que volver a dejar la cadena en negro.
    void markClearPending();

private:
    VkImage               m_image[kFramesInFlight]  = {};
    VkDeviceMemory        m_memory[kFramesInFlight] = {};
    // Una vista 2D por nivel: imageStore no elige mip, igual que en el
    // prefiltrado del IBL. La misma vista hace de storage image y de
    // textura muestreada — la imagen se queda en GENERAL toda la cadena,
    // que es un layout valido para ambas cosas y ahorra el ping-pong.
    VkImageView           m_mipView[kFramesInFlight][kMaxMips] = {};
    VkExtent2D            m_mipExtent[kMaxMips]                = {};
    uint32_t              m_mipCount                           = 0;
    VkSampler             m_sampler                            = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descLayout                         = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool                           = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout                     = VK_NULL_HANDLE;
    VkPipeline            m_downPipeline                       = VK_NULL_HANDLE;
    VkPipeline            m_upPipeline                         = VK_NULL_HANDLE;
    VkDescriptorSet       m_downSets[kFramesInFlight][kMaxMips] = {};
    VkDescriptorSet       m_upSets[kFramesInFlight][kMaxMips]   = {};
    bool                  m_clearPending[kFramesInFlight]      = {};
    VkQueryPool           m_queryPool                          = VK_NULL_HANDLE;
    bool                  m_queryPending[kFramesInFlight]      = {};
    float                 m_gpuMs                              = 0.0f;
    uint32_t              m_measuredFrames                     = 0;
};

} // namespace DonTopo
