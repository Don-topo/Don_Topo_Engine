#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace DonTopo {

class GpuDevice;
class GpuResources;
class RendererState;

// Camera motion blur. The whole pass (pipeline, intermediate images,
// descriptor sets and recording) lives here; the Renderer remains the owner of
// the instance and the one that decides WHEN each thing is called.
class MotionBlurPass {
public:
    // It must match Renderer::MAX_FRAMES (checked with static_assert in Renderer.cpp).
    static constexpr int kFramesInFlight = 2;

    // What the pass needs from the Renderer and is NOT its own. It is built at the
    // call site and passed by reference: do not store it, since the
    // handles are recreated with the swapchain.
    struct Context {
        GpuDevice&           gpu;
        GpuResources&        res;
        const RendererState& state;
        // INTERNAL render resolution (the HDR's), not the swapchain's.
        const VkExtent2D&    renderExtent;
        int                  currentFrame;
        // Scene target format: the intermediate image copies it as is.
        VkFormat             hdrFormat;
        const VkImage*       hdrImage;       // [kFramesInFlight]
        const VkImageView*   hdrView;        // [kFramesInFlight]
        const VkImageView*   ssaoDepthView;  // [kFramesInFlight]
        // No sampler of its own: the color goes with SSR's and the depth with
        // SSAO's.
        VkSampler            ssrSampler;
        VkSampler            ssaoSampler;
        // The SAME matrices that TAA reprojects with. They are updated every
        // frame, whether TAA is active or not.
        const glm::mat4&     taaCurrViewProj;
        const glm::mat4&     taaPrevViewProj;
        // GPU cost measurement, as in the bloom. Without this the pass
        // was the ONLY one that was not measured, that is, motion blur was turned on
        // blindly despite being one of the expensive ones.
        bool                 timestampsSupported;
        float                timestampPeriod;
    };

    MotionBlurPass()                                 = default;
    MotionBlurPass(const MotionBlurPass&)            = delete;
    MotionBlurPass& operator=(const MotionBlurPass&) = delete;

    // What does not depend on the size: layout, pool, pipeline layout and pipeline.
    // Only once, in init.
    void createPipeline(const Context& ctx);
    // Counterpart of createPipeline, in cleanup.
    void destroyPipeline(const Context& ctx);
    // Intermediate images and descriptor sets: they go with the swapchain, because
    // they reference hdrView and ssaoDepthView, which are recreated with it.
    void createImages(const Context& ctx);
    void destroyImages(const Context& ctx);
    // A dispatch to a separate image plus the copy back. It goes AFTER
    // the fog and BEFORE the bloom: the trail drags the highlights
    // and blooms with them. Off, it records not a single command.
    void record(const Context& ctx, VkCommandBuffer cmd);
    bool active(const Context& ctx) const;
    // GPU ms of the last measured frame. 0 if there are no timestamps or if the
    // effect is off.
    float gpuMs() const { return m_gpuMs; }

private:
    // Intermediate image of the same format and size as the HDR: the shader
    // reads arbitrary pixels along the velocity, so it cannot
    // write onto the image it samples. The copy back is done
    // by a vkCmdCopyImage, not a second dispatch.
    VkQueryPool           m_queryPool               = VK_NULL_HANDLE;
    bool                  m_queryPending[kFramesInFlight] = {};
    float                 m_gpuMs                   = 0.0f;

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
