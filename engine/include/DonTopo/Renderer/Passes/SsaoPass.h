#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace DonTopo {

class GpuDevice;
class GpuResources;
class RendererState;

// Screen-space ambient occlusion: the two dispatches (occlusion +
// blur) and their images. The DEPTH it reads is not its own: it is recorded by
// DepthPrepassPass, which shares it with SSR, TAA, Forward+, the fog
// and motion blur.
//
// Recording goes in two pieces because the depth pre-pass slips in between:
//   recordPreDepth()  → clearing the map (off) or timestamps (on)
//   [the Renderer records the depth pre-pass]
//   record()          → occlusion + blur
class SsaoPass {
public:
    // It must match Renderer::MAX_FRAMES (checked with static_assert in Renderer.cpp).
    static constexpr int kFramesInFlight = 2;

    struct Context {
        GpuDevice&           gpu;
        GpuResources&        res;
        const RendererState& state;
        // INTERNAL render resolution, not the swapchain's.
        const VkExtent2D&    renderExtent;
        // The swapchain's, only for the measurement report.
        const VkExtent2D&    swapChainExtent;
        int                  currentFrame;
        // The pre-pass depth and its sampler, both from
        // DepthPrepassPass.
        const VkImageView*   depthView;      // [kFramesInFlight]
        VkSampler            depthSampler;
        // They were resolved by the bloom; here they are only read.
        bool                 timestampsSupported;
        float                timestampPeriod;
    };

    SsaoPass()                           = default;
    SsaoPass(const SsaoPass&)            = delete;
    SsaoPass& operator=(const SsaoPass&) = delete;

    // What does not depend on the size: layout, pool, the two compute pipelines and
    // the query pool. Only once, in init.
    void createPipelines(const Context& ctx);
    void destroyPipelines(const Context& ctx);
    // The two images (raw AO and blurred AO) and their sets: they go with the
    // swapchain, because the sets reference the pre-pass depth.
    void createImages(const Context& ctx);
    void destroyImages(const Context& ctx);

    // First piece: with the effect off it leaves the map at identity (only
    // when there is something to clear); on, it reads the slot's timestamps and
    // opens this frame's pair. It goes BEFORE the depth pre-pass.
    void recordPreDepth(const Context& ctx, VkCommandBuffer cmd);
    // Second piece: occlusion + blur. It goes AFTER the depth pre-pass, and only
    // with the effect on.
    void record(const Context& ctx, VkCommandBuffer cmd, const glm::mat4& proj);

    // Degenerate viewport or resources not created yet.
    bool ready(int frame) const { return m_blurImage[frame] != VK_NULL_HANDLE; }
    // GPU cost of the two dispatches in ms.
    float gpuMs() const { return m_gpuMs; }
    // The map that pbr.frag consumes through binding 7: the Renderer writes it into
    // each object's descriptor sets, which are its own.
    const VkImageView* blurViews()      const { return m_blurView; }   // [kFramesInFlight]
    VkImage            blurImage(int f) const { return m_blurImage[f]; }
    // When turning the effect off the map has to be left at 1.0 again.
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
    // With the effect off the map has to be 1.0 (identity) and
    // also be in GENERAL, which is the layout the descriptor
    // sets declare. A clear solves both things at once, and it is only
    // recorded when there is something to clear: when creating the images and when
    // turning the effect off. Apart from that, off = zero work per frame.
    bool                  m_clearPending[kFramesInFlight] = {};
    VkQueryPool           m_queryPool                     = VK_NULL_HANDLE;
    bool                  m_queryPending[kFramesInFlight] = {};
    float                 m_gpuMs                         = 0.0f;
    uint32_t              m_measuredFrames                = 0;
};

} // namespace DonTopo
