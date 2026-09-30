#pragma once
#include <vulkan/vulkan.h>

namespace DonTopo {

class GpuDevice;

// Selection outline: the extruded hull that the EDITOR paints around the
// selected object. Nobody else uses it (`setOutlineTarget` is only called by
// ViewportPanel), and even so its four pipelines lived loose inside the
// Renderer, which is the only engine effect that still had no pass of its own (H15).
//
// Let the number be on record, because the reason the ticket gave does not hold up: the
// runtime saves 1.88 ms of startup, no more. This is pulled out because of the
// coupling (code that only the editor uses inside the backend), just as
// happened with H8 when the clock did not move.
//
// Split, the same as DepthPrepassPass and ShadowPass: this class owns the
// PIPELINES and the selected target. The DRAWS stay in the Renderer,
// because they come from its object lists, its mesh cache and its
// pipeline layout, which are not this pass's.
class SelectionOutlinePass {
public:
    struct Context {
        GpuDevice&       gpu;
        // Borrowed from the Renderer: the outline uses EXACTLY the same layout
        // as the meshes (the object's set 0 and the same push constants), so
        // it does not create one of its own.
        VkPipelineLayout pipelineLayout;
        // The COMPOSITION one, not the scene one: the outline is painted already in LDR
        // so that the tonemap does not change its flat orange.
        VkRenderPass     compositeRenderPass;
    };

    SelectionOutlinePass()                                       = default;
    SelectionOutlinePass(const SelectionOutlinePass&)            = delete;
    SelectionOutlinePass& operator=(const SelectionOutlinePass&) = delete;

    // The two static mesh pipelines (fill and wireframe). They are created
    // together with the main pipeline because they share almost all their state, which
    // arrives in `plantilla` already filled in.
    //
    // `plantilla` has to carry the engine's Vertex vertex input: here only
    // the shaders, the culling and the attributes are changed.
    void createStaticPipelines(const Context& ctx,
                               const VkGraphicsPipelineCreateInfo& plantilla,
                               const VkPipelineRasterizationStateCreateInfo& rasterizacion,
                               const VkPipelineVertexInputStateCreateInfo& vertexInput,
                               uint32_t posOffset, uint32_t normalOffset);
    // The two for meshes with bones. They go apart because their vertex input is the
    // OUTPUT of the skinning compute (stride 80), not the packed Vertex: the
    // hull is extruded over THIS frame's already deformed pose.
    void createSkinnedPipelines(const Context& ctx,
                                const VkGraphicsPipelineCreateInfo& plantilla,
                                const VkPipelineRasterizationStateCreateInfo& rasterizacion,
                                const VkPipelineVertexInputStateCreateInfo& vertexInput,
                                uint32_t posOffset, uint32_t normalOffset);
    void destroyResources(const Context& ctx);

    // Called by the editor once per frame. With the default (-1, -1) nothing is
    // drawn, which is what the runtime always sees.
    void setTarget(int staticIndex, int skinnedIndex)
    {
        m_staticIndex  = staticIndex;
        m_skinnedIndex = skinnedIndex;
    }
    int  staticTarget()  const { return m_staticIndex; }
    int  skinnedTarget() const { return m_skinnedIndex; }
    bool hasTarget()     const { return m_staticIndex >= 0 || m_skinnedIndex >= 0; }

    // The pipeline that applies according to the fill mode. The Renderer binds it
    // right before its draw.
    VkPipeline staticPipeline(bool wireframe) const
    {
        return wireframe ? m_staticWire : m_static;
    }
    VkPipeline skinnedPipeline(bool wireframe) const
    {
        return wireframe ? m_skinnedWire : m_skinned;
    }

private:
    // State common to the four: discard the FRONT faces. The back faces
    // of the extruded hull end up behind the object's surface, so
    // the depth test (LESS) only lets through the rim that sticks out of its
    // silhouette.
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
