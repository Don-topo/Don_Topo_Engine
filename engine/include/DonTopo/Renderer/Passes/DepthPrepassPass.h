#pragma once
#include <vulkan/vulkan.h>

namespace DonTopo {

class GpuDevice;
class GpuResources;

// Depth pre-pass: la escena entera, solo profundidad. NO es un efecto: es el
// proveedor de la imagen de profundidad que consumen el SSAO, el SSR, el TAA,
// el Forward+ tiled, la niebla y el motion blur.
//
// Reparto: esta clase posee el TARGET y el PIPELINE (imagen, vista,
// framebuffer, render pass, pipeline y el sampler con el que se muestrea esa
// profundidad). Los DRAWS se quedan en el Renderer, entre begin() y end():
// salen de sus listas de objetos, de su SSBO de instancias y del layout del
// pass de sombras, que no son de este pase.
class DepthPrepassPass {
public:
    // Debe coincidir con Renderer::MAX_FRAMES (comprobado con static_assert en Renderer.cpp).
    static constexpr int kFramesInFlight = 2;

    struct Context {
        GpuDevice&        gpu;
        GpuResources&     res;
        const VkExtent2D& renderExtent;
        int               currentFrame;
        // Prestado del pass de sombras: mismos dos sets (objeto + SSBO de
        // instancias) y mismo rango de push constants, que este shader no usa.
        VkPipelineLayout  shadowPipelineLayout;
    };

    DepthPrepassPass()                                   = default;
    DepthPrepassPass(const DepthPrepassPass&)            = delete;
    DepthPrepassPass& operator=(const DepthPrepassPass&) = delete;

    // Lo que no depende del tamano: sampler, render pass y pipeline. Una sola
    // vez, en el init.
    void createRenderPassAndPipeline(const Context& ctx);
    void destroyRenderPassAndPipeline(const Context& ctx);
    // Imagen de profundidad, vista y framebuffer: van con el swapchain.
    void createImages(const Context& ctx);
    void destroyImages(const Context& ctx);

    // Abre el render pass con el viewport, el scissor y el pipeline puestos.
    // Entre esto y end() el Renderer graba sus draws.
    void begin(const Context& ctx, VkCommandBuffer cmd);
    // Cambia al pipeline de mallas con huesos SIN cerrar el render pass: su
    // vertex input es la SALIDA del compute de skinning (5 x vec4), no el
    // Vertex empaquetado del motor. Mismo reparto que ShadowPass.
    void bindSkinnedPipeline(VkCommandBuffer cmd);
    void end(VkCommandBuffer cmd);

    // La profundidad y su sampler: los muestrean el SSAO, el SSR, el TAA, el
    // Forward+, la niebla y el motion blur.
    const VkImageView* views()   const { return m_view; }   // [kFramesInFlight]
    VkSampler          sampler() const { return m_sampler; }

private:
    VkImage        m_image[kFramesInFlight]  = {};
    VkDeviceMemory m_memory[kFramesInFlight] = {};
    VkImageView    m_view[kFramesInFlight]   = {};
    VkFramebuffer  m_fb[kFramesInFlight]     = {};
    VkPipeline     m_skinnedPipeline         = VK_NULL_HANDLE;
    VkRenderPass   m_renderPass              = VK_NULL_HANDLE;
    VkPipeline     m_pipeline                = VK_NULL_HANDLE;
    // NEAREST: ni D32_SFLOAT ni R32_SFLOAT tienen garantizado el filtrado
    // lineal, y los shaders que la leen muestrean a texel exacto.
    VkSampler      m_sampler                 = VK_NULL_HANDLE;
};

} // namespace DonTopo
