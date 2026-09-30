#pragma once
#include <vulkan/vulkan.h>

namespace DonTopo {

class GpuDevice;
class GpuResources;

// Depth pre-pass: the whole scene, depth only. It is NOT an effect: it is the
// provider of the depth image consumed by SSAO, SSR, TAA,
// tiled Forward+, the fog and motion blur.
//
// Split: this class owns the TARGET and the PIPELINE (image, view,
// framebuffer, render pass, pipeline and the sampler with which that
// depth is sampled). The DRAWS stay in the Renderer, between begin() and end():
// they come from its object lists, its instance SSBO and the shadow
// pass's layout, which are not this pass's.
class DepthPrepassPass {
public:
    // It must match Renderer::MAX_FRAMES (checked with static_assert in Renderer.cpp).
    static constexpr int kFramesInFlight = 2;

    struct Context {
        GpuDevice&        gpu;
        GpuResources&     res;
        const VkExtent2D& renderExtent;
        int               currentFrame;
        // Borrowed from the shadow pass: same two sets (object + instance
        // SSBO) and same push constant range, which this shader does not use.
        VkPipelineLayout  shadowPipelineLayout;
    };

    DepthPrepassPass()                                   = default;
    DepthPrepassPass(const DepthPrepassPass&)            = delete;
    DepthPrepassPass& operator=(const DepthPrepassPass&) = delete;

    // What does not depend on the size: sampler, render pass and pipeline. Only
    // once, in init.
    void createRenderPassAndPipeline(const Context& ctx);
    void destroyRenderPassAndPipeline(const Context& ctx);
    // Depth image, view and framebuffer: they go with the swapchain.
    void createImages(const Context& ctx);
    void destroyImages(const Context& ctx);

    // Opens the render pass with the viewport, the scissor and the pipeline set.
    // Between this and end() the Renderer records its draws.
    void begin(const Context& ctx, VkCommandBuffer cmd);
    // Switches to the bone-mesh pipeline WITHOUT closing the render pass: its
    // vertex input is the OUTPUT of the skinning compute (5 x vec4), not the
    // engine's packed Vertex. Same split as ShadowPass.
    void bindSkinnedPipeline(VkCommandBuffer cmd);
    void end(VkCommandBuffer cmd);

    // The depth and its sampler: sampled by SSAO, SSR, TAA,
    // Forward+, the fog and motion blur.
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
    // NEAREST: neither D32_SFLOAT nor R32_SFLOAT is guaranteed to support linear
    // filtering, and the shaders that read it sample at exact texel.
    VkSampler      m_sampler                 = VK_NULL_HANDLE;
};

} // namespace DonTopo
