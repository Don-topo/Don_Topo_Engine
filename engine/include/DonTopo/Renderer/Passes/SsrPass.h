#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace DonTopo {

class GpuDevice;
class GpuResources;
class RendererState;

// Reflejos en espacio de pantalla. El pase entero -pipelines, imagen del
// reflejo, sampler, descriptor sets, queries de tiempo y grabacion- vive aqui;
// el Renderer sigue siendo el dueno de la instancia y el que decide CUANDO se
// llama a cada cosa.
//
// Dos ataduras con codigo que NO es de este pase, y por eso salen a la
// interfaz publica:
//  - queryPool(): el par de timestamps [0,1] lo escribe el depth PRE-PASS, que
//    graba el Renderer (recordSsaoPass), no este pase.
//  - sampler(): lo creo el SSR pero tambien lo usa el motion blur.
class SsrPass {
public:
    // Debe coincidir con Renderer::MAX_FRAMES (comprobado con static_assert en Renderer.cpp).
    static constexpr int kFramesInFlight = 2;

    // Lo que el pase necesita del Renderer y NO es suyo. Se construye en el
    // sitio de llamada y se pasa por referencia: nada de guardarlo, que los
    // handles se recrean con el swapchain.
    struct Context {
        GpuDevice&           gpu;
        GpuResources&        res;
        const RendererState& state;
        // Resolucion INTERNA del render (la del HDR), no la del swapchain.
        const VkExtent2D&    renderExtent;
        // La del swapchain, solo para el informe de medida.
        const VkExtent2D&    swapChainExtent;
        int                  currentFrame;
        // Formato del target de escena: el reflejo comparte formato con el HDR.
        VkFormat             hdrFormat;
        const VkImage*       hdrImage;       // [kFramesInFlight]
        const VkImageView*   hdrView;        // [kFramesInFlight]
        const VkImageView*   ssaoDepthView;  // [kFramesInFlight]
        // La profundidad se muestrea con el del SSAO (NEAREST), no con el suyo.
        VkSampler            ssaoSampler;
        // Los resolvio el bloom; aqui solo se leen.
        bool                 timestampsSupported;
        float                timestampPeriod;
        // Hay al menos un objeto visible con ssrStrength > 0. Lo calcula el
        // Renderer: recorre sus propias listas de objetos, que no son de este
        // pase.
        bool                 anyObjectWithSsr;
        // recordSsaoPass dejo escritos los timestamps [0,1] de este frame. Sin
        // eso la lectura de los cuatro daria NOT_READY.
        bool                 stampedPrepass;
    };

    SsrPass()                          = default;
    SsrPass(const SsrPass&)            = delete;
    SsrPass& operator=(const SsrPass&) = delete;

    // Lo que no depende del tamano: sampler, layout, pool, los dos pipelines y
    // el pool de queries. Una sola vez, en el init.
    void createPipelines(const Context& ctx);
    // Contrapartida de createPipelines, en el cleanup.
    void destroyPipelines(const Context& ctx);
    // La imagen del reflejo y los dos sets por frame: van con el swapchain,
    // porque referencian hdrView y ssaoDepthView, que se recrean con el.
    void createImages(const Context& ctx);
    void destroyImages(const Context& ctx);
    // Los dos dispatches (marcha + suma sobre el HDR). Va DESPUES del pass de
    // escena -necesita el color ya iluminado- y ANTES del bloom, para que el
    // reflejo pase por el umbral del bloom y por el tonemap como el resto de
    // la imagen.
    void record(const Context& ctx, VkCommandBuffer cmd, const glm::mat4& proj);
    // true si hay algo que grabar: interruptor global puesto Y al menos un
    // objeto visible con fuerza > 0. Con cualquiera de las dos cosas en falso
    // no se graba ni un dispatch (ni se calcula multiplicando por cero), asi
    // que el coste GPU cae a cero.
    bool active(const Context& ctx) const;

    // Coste GPU del SSR en ms: los dos dispatches, mas el depth pre-pass
    // cuando es el SSR quien lo pide.
    float gpuMs() const { return m_gpuMs; }
    // Las dos ataduras de arriba.
    VkQueryPool queryPool() const { return m_queryPool; }
    VkSampler   sampler()   const { return m_sampler; }

private:
    // Reflejo aislado, a resolucion completa y en el MISMO formato que el
    // HDR: ssr_resolve.comp lo suma sobre el HDR y los dos son storage images
    // con el mismo qualifier rgba16f.
    VkImage               m_image[kFramesInFlight]  = {};
    VkDeviceMemory        m_memory[kFramesInFlight] = {};
    VkImageView           m_view[kFramesInFlight]   = {};
    // LINEAR: a diferencia del SSAO, el impacto del rayo cae entre texeles y
    // el color de la escena si tiene garantizado el filtrado lineal en
    // R16G16B16A16_SFLOAT.
    VkSampler             m_sampler                 = VK_NULL_HANDLE;
    // Un unico layout para los dos pipelines: ssr_resolve.comp declara el
    // binding 1 y simplemente no lo lee.
    VkDescriptorSetLayout m_descLayout              = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool                = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout          = VK_NULL_HANDLE;
    VkPipeline            m_pipeline                = VK_NULL_HANDLE;
    VkPipeline            m_resolvePipeline         = VK_NULL_HANDLE;
    VkDescriptorSet       m_sets[kFramesInFlight]        = {};
    VkDescriptorSet       m_resolveSets[kFramesInFlight] = {};
    // Cuatro queries por frame: [0,1] el depth pre-pass cuando es el SSR
    // quien lo pide, [2,3] los dos dispatches. Reutilizar las del SSAO o
    // las del bloom mezclaria dos medidas.
    VkQueryPool           m_queryPool                     = VK_NULL_HANDLE;
    bool                  m_queryPending[kFramesInFlight] = {};
    float                 m_gpuMs                         = 0.0f;
    uint32_t              m_measuredFrames                = 0;
};

} // namespace DonTopo
