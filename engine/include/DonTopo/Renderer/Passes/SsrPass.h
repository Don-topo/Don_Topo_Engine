#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace DonTopo {

class GpuDevice;
class GpuResources;
class RendererState;

// Screen-space reflections. The whole pass (pipelines, reflection image,
// sampler, descriptor sets, time queries and recording) lives here;
// the Renderer remains the owner of the instance and the one that decides WHEN
// each thing is called.
//
// Two ties with code that is NOT this pass's, and that is why they come out in the
// public interface:
//  - queryPool(): the timestamp pair [0,1] is written by the depth PRE-PASS, which
//    the Renderer records (recordSsaoPass), not this pass.
//  - sampler(): created by SSR but also used by motion blur.
class SsrPass {
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
        // The swapchain's, only for the measurement report.
        const VkExtent2D&    swapChainExtent;
        int                  currentFrame;
        // Scene target format: the reflection shares its format with the HDR.
        VkFormat             hdrFormat;
        const VkImage*       hdrImage;       // [kFramesInFlight]
        const VkImageView*   hdrView;        // [kFramesInFlight]
        const VkImageView*   ssaoDepthView;  // [kFramesInFlight]
        // The depth is sampled with SSAO's (NEAREST), not with its own.
        VkSampler            ssaoSampler;
        // They were resolved by the bloom; here they are only read.
        bool                 timestampsSupported;
        float                timestampPeriod;
        // There is at least one visible object with ssrStrength > 0. The
        // Renderer computes it: it traverses its own object lists, which are not this
        // pass's.
        bool                 anyObjectWithSsr;
        // recordSsaoPass left this frame's timestamps [0,1] written. Without
        // that, reading all four would give NOT_READY.
        bool                 stampedPrepass;
    };

    SsrPass()                          = default;
    SsrPass(const SsrPass&)            = delete;
    SsrPass& operator=(const SsrPass&) = delete;

    // What does not depend on the size: sampler, layout, pool, the two pipelines and
    // the query pool. Only once, in init.
    void createPipelines(const Context& ctx);
    // Counterpart of createPipelines, in cleanup.
    void destroyPipelines(const Context& ctx);
    // The reflection image and the two sets per frame: they go with the swapchain,
    // because they reference hdrView and ssaoDepthView, which are recreated with it.
    void createImages(const Context& ctx);
    void destroyImages(const Context& ctx);
    // The two dispatches (march + sum onto the HDR). It goes AFTER the scene pass
    // (it needs the already lit color) and BEFORE the bloom, so that the
    // reflection goes through the bloom threshold and the tonemap like the rest of
    // the image.
    void record(const Context& ctx, VkCommandBuffer cmd, const glm::mat4& proj);
    // true if there is something to record: global switch on AND at least one
    // visible object with strength > 0. With either of the two false
    // not a single dispatch is recorded (nor is it computed by multiplying by zero), so
    // the GPU cost drops to zero.
    bool active(const Context& ctx) const;

    // GPU cost of SSR in ms: the two dispatches, plus the depth pre-pass
    // when SSR is the one requesting it.
    float gpuMs() const { return m_gpuMs; }
    // The two ties above.
    VkQueryPool queryPool() const { return m_queryPool; }
    VkSampler   sampler()   const { return m_sampler; }

private:
    // Isolated reflection, at full resolution and in the SAME format as the
    // HDR: ssr_resolve.comp adds it onto the HDR and both are storage images
    // with the same rgba16f qualifier.
    VkImage               m_image[kFramesInFlight]  = {};
    VkDeviceMemory        m_memory[kFramesInFlight] = {};
    VkImageView           m_view[kFramesInFlight]   = {};
    // LINEAR: unlike SSAO, the ray hit falls between texels and the
    // scene color does have guaranteed linear filtering in
    // R16G16B16A16_SFLOAT.
    VkSampler             m_sampler                 = VK_NULL_HANDLE;
    // A single layout for the two pipelines: ssr_resolve.comp declares
    // binding 1 and simply does not read it.
    VkDescriptorSetLayout m_descLayout              = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool                = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout          = VK_NULL_HANDLE;
    VkPipeline            m_pipeline                = VK_NULL_HANDLE;
    VkPipeline            m_resolvePipeline         = VK_NULL_HANDLE;
    VkDescriptorSet       m_sets[kFramesInFlight]        = {};
    VkDescriptorSet       m_resolveSets[kFramesInFlight] = {};
    // Four queries per frame: [0,1] the depth pre-pass when SSR is the one
    // requesting it, [2,3] the two dispatches. Reusing SSAO's or
    // the bloom's would mix two measurements.
    VkQueryPool           m_queryPool                     = VK_NULL_HANDLE;
    bool                  m_queryPending[kFramesInFlight] = {};
    float                 m_gpuMs                         = 0.0f;
    uint32_t              m_measuredFrames                = 0;
};

} // namespace DonTopo
