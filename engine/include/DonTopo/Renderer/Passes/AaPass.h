#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include "DonTopo/Renderer/RendererState.h"

namespace DonTopo {

class GpuDevice;
class GpuResources;

// Anti-aliasing: el pass de RESOLUCION (FXAA, SSAA y TAA), su imagen
// intermedia, el historial del TAA y el jitter de la proyeccion.
//
// Lo que NO es suyo y se queda en el Renderer, porque gobierna a otros:
//  - el numero de muestras del MSAA y los render passes de escena y
//    composicion que dependen de el (el resolve del MSAA ocurre dentro de
//    esos passes, no aqui),
//  - m_renderExtent y su recalculo,
//  - el pool de queries, que ademas cronometra el frame entero sin UI: este
//    pase solo escribe el par [0,1].
//
// El pass de resolucion es grafico y no compute: el swapchain es
// B8G8R8A8_SRGB y Vulkan prohibe las storage images en formatos sRGB.
class AaPass {
public:
    // Debe coincidir con Renderer::MAX_FRAMES (comprobado con static_assert en Renderer.cpp).
    static constexpr int kFramesInFlight = 2;

    using AaMode = RendererState::AaMode;

    struct Context {
        GpuDevice&           gpu;
        GpuResources&        res;
        const RendererState& state;
        // Resolucion INTERNA del render (mayor que la ventana con SSAA) y
        // tamano de PRESENTACION: este pase es justo el que baja de una a otra.
        const VkExtent2D&    renderExtent;
        VkExtent2D           viewport;
        int                  currentFrame;
        // Modo CONSTRUIDO, el que corresponde a los recursos que existen ahora.
        AaMode               activeMode;
        VkFormat             swapChainFormat;
        // Destino final: la imagen que muestrea la UI y que blitea el runtime.
        const VkImageView*   offscreenView;      // [kFramesInFlight]
        // El depth de la escena, que comparte el framebuffer de composicion.
        VkImageView          sceneDepthView;
        // El render pass de composicion: este pase le fabrica un framebuffer
        // alternativo que escribe en la imagen intermedia.
        VkRenderPass         compositeRenderPass;
        // La profundidad del depth pre-pass y su sampler: el TAA reproyecta con
        // ella (sin jitter, que es la geometrica).
        const VkImageView*   prepassDepthView;   // [kFramesInFlight]
        VkSampler            prepassDepthSampler;
        // El pool lo posee el Renderer porque tambien mide el frame entero.
        VkQueryPool          queryPool;
        bool                 timestampsSupported;
        float                ssaaFactor;
    };

    AaPass()                         = default;
    AaPass(const AaPass&)            = delete;
    AaPass& operator=(const AaPass&) = delete;

    // Los dos render passes de resolucion (uno de un attachment, el del TAA de
    // dos) y los tres pipelines con su sampler, layouts y pools. Independientes
    // del tamano y del modo: una sola vez en el init.
    void createRenderPasses(const Context& ctx);
    void createPipelines(const Context& ctx);
    void destroyPipelinesAndRenderPasses(const Context& ctx);
    // Imagen intermedia, historial del TAA, framebuffers y sets. Todo depende
    // del tamano y del modo, asi que va colgado de
    // createOffscreenImages/destroyOffscreenImages.
    void createImages(const Context& ctx);
    void destroyImages(const Context& ctx);

    // Lee la imagen intermedia (lo que escribio la composicion) y escribe la
    // offscreen con el pipeline del modo activo. En None y en MSAA no graba
    // NADA: la composicion ya habra escrito directamente en la offscreen.
    void record(const Context& ctx, VkCommandBuffer cmd);

    // Relevo prev<-curr y jitter de la proyeccion del frame. Se llama SIEMPRE,
    // tambien con el TAA apagado: el motion blur reproyecta con las mismas dos
    // matrices.
    void updateFrameMatrices(const Context& ctx, const glm::mat4& view, const glm::mat4& proj);

    // La proyeccion que usa el pass de escena: jittereada solo en TAA.
    const glm::mat4& jitteredProj() const { return m_jitteredProj; }
    // Las dos que consumen el TAA y el motion blur.
    const glm::mat4& currViewProj() const { return m_currViewProj; }
    const glm::mat4& prevViewProj() const { return m_prevViewProj; }
    // El framebuffer al que tiene que escribir la composicion cuando hay pass
    // de resolucion detras. VK_NULL_HANDLE si no lo hay.
    VkFramebuffer compositeFramebuffer(int frame) const { return m_srcFramebuffer[frame]; }
    // Si este frame llego a escribir el par [0,1] del pool.
    bool passStamped(int frame) const { return m_passStamped[frame]; }

private:
    // Destino alternativo de la composicion, a resolucion INTERNA.
    VkImage        m_srcImage[kFramesInFlight]       = {};
    VkDeviceMemory m_srcMemory[kFramesInFlight]      = {};
    VkImageView    m_srcView[kFramesInFlight]        = {};
    VkFramebuffer  m_srcFramebuffer[kFramesInFlight] = {};
    // Pass de resolucion: escribe en la offscreen a tamano de ventana.
    VkRenderPass   m_renderPass                      = VK_NULL_HANDLE;
    VkFramebuffer  m_framebuffer[kFramesInFlight]    = {};
    VkSampler      m_sampler                         = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descLayout               = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool                 = VK_NULL_HANDLE;
    VkDescriptorSet       m_sets[kFramesInFlight]    = {};
    VkPipelineLayout m_fxaaPipelineLayout            = VK_NULL_HANDLE;
    VkPipeline       m_fxaaPipeline                  = VK_NULL_HANDLE;
    VkPipelineLayout m_ssaaPipelineLayout            = VK_NULL_HANDLE;
    VkPipeline       m_ssaaPipeline                  = VK_NULL_HANDLE;
    // TAA: historial, su render pass de dos attachments y sus sets de tres.
    VkImage        m_historyImage[kFramesInFlight]       = {};
    VkDeviceMemory m_historyMemory[kFramesInFlight]      = {};
    VkImageView    m_historyView[kFramesInFlight]        = {};
    VkFramebuffer  m_historyFramebuffer[kFramesInFlight] = {};
    VkRenderPass   m_historyRenderPass                   = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_taaDescLayout                = VK_NULL_HANDLE;
    VkDescriptorPool      m_taaDescPool                  = VK_NULL_HANDLE;
    VkDescriptorSet       m_taaSets[kFramesInFlight]     = {};
    VkPipelineLayout      m_taaPipelineLayout            = VK_NULL_HANDLE;
    VkPipeline            m_taaPipeline                  = VK_NULL_HANDLE;
    bool                  m_historyValid                 = false;
    // Jitter y matrices del frame.
    uint32_t   m_jitterIndex  = 0;
    glm::vec2  m_jitter       = glm::vec2(0.0f);
    glm::mat4  m_jitteredProj = glm::mat4(1.0f);
    glm::mat4  m_prevViewProj = glm::mat4(1.0f);
    glm::mat4  m_currViewProj = glm::mat4(1.0f);
    // Si el frame llego a escribir el par [0,1]; el pool y la medida son
    // del Renderer, que con el mismo pool cronometra el frame entero.
    bool m_passStamped[kFramesInFlight] = {};
};

} // namespace DonTopo
