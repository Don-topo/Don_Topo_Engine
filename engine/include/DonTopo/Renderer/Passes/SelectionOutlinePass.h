#pragma once
#include <vulkan/vulkan.h>

namespace DonTopo {

class GpuDevice;

// Contorno de seleccion: el casco extruido que el EDITOR pinta alrededor del
// objeto seleccionado. Nadie mas lo usa — `setOutlineTarget` solo lo llama
// ViewportPanel—, y aun asi sus cuatro pipelines vivian sueltos dentro del
// Renderer, que es el unico efecto del motor que seguia sin pase propio (H15).
//
// Que conste el numero, porque el motivo que daba la ficha no se sostiene: el
// runtime se ahorra 1,88 ms de arranque, no mas. Esto se saca por el
// acoplamiento —codigo que solo usa el editor dentro del backend—, igual que
// paso con H8 cuando el reloj no se movio.
//
// Reparto, el mismo que DepthPrepassPass y ShadowPass: esta clase posee los
// PIPELINES y el objetivo seleccionado. Los DRAWS se quedan en el Renderer,
// porque salen de sus listas de objetos, de su cache de mallas y de su
// pipeline layout, que no son de este pase.
class SelectionOutlinePass {
public:
    struct Context {
        GpuDevice&       gpu;
        // Prestado del Renderer: el contorno usa EXACTAMENTE el mismo layout
        // que las mallas (set 0 del objeto y las mismas push constants), asi
        // que no crea uno propio.
        VkPipelineLayout pipelineLayout;
        // El de COMPOSICION, no el de escena: el contorno se pinta ya en LDR
        // para que el tonemap no le cambie el naranja plano.
        VkRenderPass     compositeRenderPass;
    };

    SelectionOutlinePass()                                       = default;
    SelectionOutlinePass(const SelectionOutlinePass&)            = delete;
    SelectionOutlinePass& operator=(const SelectionOutlinePass&) = delete;

    // Los dos pipelines de mallas estaticas (relleno y wireframe). Se crean
    // junto al pipeline principal porque comparten casi todo su estado, que
    // llega en `plantilla` ya rellena.
    //
    // `plantilla` tiene que traer el vertex input del Vertex del motor: aqui
    // solo se le cambian los shaders, el culling y los atributos.
    void createStaticPipelines(const Context& ctx,
                               const VkGraphicsPipelineCreateInfo& plantilla,
                               const VkPipelineRasterizationStateCreateInfo& rasterizacion,
                               const VkPipelineVertexInputStateCreateInfo& vertexInput,
                               uint32_t posOffset, uint32_t normalOffset);
    // Los dos de mallas con huesos. Van aparte porque su vertex input es la
    // SALIDA del compute de skinning (stride 80), no el Vertex empaquetado: el
    // casco se extruye sobre la pose ya deformada de ESTE frame.
    void createSkinnedPipelines(const Context& ctx,
                                const VkGraphicsPipelineCreateInfo& plantilla,
                                const VkPipelineRasterizationStateCreateInfo& rasterizacion,
                                const VkPipelineVertexInputStateCreateInfo& vertexInput,
                                uint32_t posOffset, uint32_t normalOffset);
    void destroyResources(const Context& ctx);

    // Lo llama el editor una vez por frame. Con el default (-1, -1) no se
    // dibuja nada, que es lo que ve el runtime siempre.
    void setTarget(int staticIndex, int skinnedIndex)
    {
        m_staticIndex  = staticIndex;
        m_skinnedIndex = skinnedIndex;
    }
    int  staticTarget()  const { return m_staticIndex; }
    int  skinnedTarget() const { return m_skinnedIndex; }
    bool hasTarget()     const { return m_staticIndex >= 0 || m_skinnedIndex >= 0; }

    // El pipeline que toca segun el modo de relleno. El Renderer lo bindea
    // justo antes de su draw.
    VkPipeline staticPipeline(bool wireframe) const
    {
        return wireframe ? m_staticWire : m_static;
    }
    VkPipeline skinnedPipeline(bool wireframe) const
    {
        return wireframe ? m_skinnedWire : m_skinned;
    }

private:
    // Estado comun a los cuatro: descartar las caras FRONTALES. Las traseras
    // del casco extruido quedan por detras de la superficie del objeto, asi
    // que el depth test (LESS) solo deja pasar el reborde que sobresale de su
    // silueta.
    static constexpr VkCullModeFlags kCullMode = VK_CULL_MODE_FRONT_BIT;

    void crearPar(const Context& ctx,
                  const VkGraphicsPipelineCreateInfo& plantilla,
                  const VkPipelineRasterizationStateCreateInfo& rasterizacion,
                  const VkPipelineVertexInputStateCreateInfo& vertexInput,
                  uint32_t posOffset, uint32_t normalOffset,
                  VkPipeline& relleno, VkPipeline& wireframe,
                  const char* queSon);

    VkPipeline m_static      = VK_NULL_HANDLE;
    VkPipeline m_staticWire  = VK_NULL_HANDLE;
    VkPipeline m_skinned     = VK_NULL_HANDLE;
    VkPipeline m_skinnedWire = VK_NULL_HANDLE;

    int        m_staticIndex  = -1;
    int        m_skinnedIndex = -1;
};

} // namespace DonTopo
