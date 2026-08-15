#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace DonTopo {

class GpuDevice;
class GpuResources;
class RendererState;

// Motion blur de camara. El pase entero -pipeline, imagenes intermedias,
// descriptor sets y grabacion- vive aqui; el Renderer sigue siendo el dueno de
// la instancia y el que decide CUANDO se llama a cada cosa.
class MotionBlurPass {
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
        int                  currentFrame;
        // Formato del target de escena: la imagen intermedia lo copia tal cual.
        VkFormat             hdrFormat;
        const VkImage*       hdrImage;       // [kFramesInFlight]
        const VkImageView*   hdrView;        // [kFramesInFlight]
        const VkImageView*   ssaoDepthView;  // [kFramesInFlight]
        // Sin sampler propio: el color va con el del SSR y la profundidad con
        // el del SSAO.
        VkSampler            ssrSampler;
        VkSampler            ssaoSampler;
        // Las MISMAS matrices que reproyecta el TAA. Se actualizan todos los
        // frames, este el TAA activo o no.
        const glm::mat4&     taaCurrViewProj;
        const glm::mat4&     taaPrevViewProj;
    };

    MotionBlurPass()                                 = default;
    MotionBlurPass(const MotionBlurPass&)            = delete;
    MotionBlurPass& operator=(const MotionBlurPass&) = delete;

    // Lo que no depende del tamano: layout, pool, pipeline layout y pipeline.
    // Una sola vez, en el init.
    void createPipeline(const Context& ctx);
    // Contrapartida de createPipeline, en el cleanup.
    void destroyPipeline(const Context& ctx);
    // Imagenes intermedias y descriptor sets: van con el swapchain, porque
    // referencian hdrView y ssaoDepthView, que se recrean con el.
    void createImages(const Context& ctx);
    void destroyImages(const Context& ctx);
    // Un dispatch a una imagen aparte mas la copia de vuelta. Va DESPUES
    // de la niebla y ANTES del bloom: la estela arrastra los highlights
    // y florece con ellos. Apagado no graba ni un comando.
    void record(const Context& ctx, VkCommandBuffer cmd);
    bool active(const Context& ctx) const;

private:
    // Imagen intermedia del mismo formato y tamano que el HDR: el shader
    // lee pixeles arbitrarios a lo largo de la velocidad, asi que no
    // puede escribir sobre la imagen que muestrea. La copia de vuelta la
    // hace un vkCmdCopyImage, no un segundo dispatch.
    VkImage               m_image[kFramesInFlight]  = {};
    VkDeviceMemory        m_memory[kFramesInFlight] = {};
    VkImageView           m_view[kFramesInFlight]   = {};
    VkDescriptorSetLayout m_descLayout              = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool                = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout          = VK_NULL_HANDLE;
    VkPipeline            m_pipeline                = VK_NULL_HANDLE;
    VkDescriptorSet       m_sets[kFramesInFlight]   = {};
};

} // namespace DonTopo
