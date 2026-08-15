#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace DonTopo {

class GpuDevice;
class GpuResources;
class RendererState;

// Oclusion ambiental en espacio de pantalla: los dos dispatches (oclusion +
// blur) y sus imagenes. La PROFUNDIDAD que lee no es suya: la graba
// DepthPrepassPass, que la comparte con el SSR, el TAA, el Forward+, la niebla
// y el motion blur.
//
// La grabacion va en dos trozos porque el depth pre-pass se cuela en medio:
//   recordPreDepth()  → limpieza del mapa (apagado) o timestamps (encendido)
//   [el Renderer graba el depth pre-pass]
//   record()          → oclusion + blur
class SsaoPass {
public:
    // Debe coincidir con Renderer::MAX_FRAMES (comprobado con static_assert en Renderer.cpp).
    static constexpr int kFramesInFlight = 2;

    struct Context {
        GpuDevice&           gpu;
        GpuResources&        res;
        const RendererState& state;
        // Resolucion INTERNA del render, no la del swapchain.
        const VkExtent2D&    renderExtent;
        // La del swapchain, solo para el informe de medida.
        const VkExtent2D&    swapChainExtent;
        int                  currentFrame;
        // La profundidad del pre-pass y su sampler, los dos de
        // DepthPrepassPass.
        const VkImageView*   depthView;      // [kFramesInFlight]
        VkSampler            depthSampler;
        // Los resolvio el bloom; aqui solo se leen.
        bool                 timestampsSupported;
        float                timestampPeriod;
    };

    SsaoPass()                           = default;
    SsaoPass(const SsaoPass&)            = delete;
    SsaoPass& operator=(const SsaoPass&) = delete;

    // Lo que no depende del tamano: layout, pool, los dos pipelines compute y
    // el pool de queries. Una sola vez, en el init.
    void createPipelines(const Context& ctx);
    void destroyPipelines(const Context& ctx);
    // Las dos imagenes (AO crudo y AO emborronado) y sus sets: van con el
    // swapchain, porque los sets referencian la profundidad del pre-pass.
    void createImages(const Context& ctx);
    void destroyImages(const Context& ctx);

    // Primer trozo: con el efecto apagado deja el mapa en la identidad (solo
    // cuando hay algo que limpiar); encendido, lee los timestamps del slot y
    // abre el par de este frame. Va ANTES del depth pre-pass.
    void recordPreDepth(const Context& ctx, VkCommandBuffer cmd);
    // Segundo trozo: oclusion + blur. Va DESPUES del depth pre-pass, y solo
    // con el efecto encendido.
    void record(const Context& ctx, VkCommandBuffer cmd, const glm::mat4& proj);

    // Viewport degenerado o recursos aun sin crear.
    bool ready(int frame) const { return m_blurImage[frame] != VK_NULL_HANDLE; }
    // Coste GPU de los dos dispatches en ms.
    float gpuMs() const { return m_gpuMs; }
    // El mapa que consume pbr.frag por el binding 7: lo escribe el Renderer en
    // los descriptor sets de cada objeto, que son suyos.
    const VkImageView* blurViews()      const { return m_blurView; }   // [kFramesInFlight]
    VkImage            blurImage(int f) const { return m_blurImage[f]; }
    // Al apagar el efecto hay que volver a dejar el mapa en 1.0.
    void markClearPending();

private:
    static constexpr VkFormat kSsaoFormat = VK_FORMAT_R32_SFLOAT;

    VkImage               m_image[kFramesInFlight]      = {};
    VkDeviceMemory        m_memory[kFramesInFlight]     = {};
    VkImageView           m_view[kFramesInFlight]       = {};
    VkImage               m_blurImage[kFramesInFlight]  = {};
    VkDeviceMemory        m_blurMemory[kFramesInFlight] = {};
    VkImageView           m_blurView[kFramesInFlight]   = {};
    VkDescriptorSetLayout m_descLayout                  = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool                    = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout              = VK_NULL_HANDLE;
    VkPipeline            m_pipeline                    = VK_NULL_HANDLE;
    VkPipeline            m_blurPipeline                = VK_NULL_HANDLE;
    VkDescriptorSet       m_sets[kFramesInFlight]       = {};
    VkDescriptorSet       m_blurSets[kFramesInFlight]   = {};
    // Con el efecto apagado el mapa tiene que valer 1.0 (identidad) y
    // ademas estar en GENERAL, que es el layout que declaran los
    // descriptor sets. Un clear resuelve las dos cosas de golpe, y solo se
    // graba cuando hay algo que limpiar: al crear las imagenes y al
    // apagar el efecto. Fuera de eso, apagado = cero trabajo por frame.
    bool                  m_clearPending[kFramesInFlight] = {};
    VkQueryPool           m_queryPool                     = VK_NULL_HANDLE;
    bool                  m_queryPending[kFramesInFlight] = {};
    float                 m_gpuMs                         = 0.0f;
    uint32_t              m_measuredFrames                = 0;
};

} // namespace DonTopo
