#pragma once
#include <vulkan/vulkan.h>

namespace DonTopo {

class GpuDevice;
class RendererState;

// Bloom: half-resolution mip chain (down pass with threshold + cumulative
// up pass). What is NOT its own and therefore stays in the Renderer: the
// COMPOSITION pass, which adds mip 0 onto the HDR, tonemaps and also hosts the
// outline, the gizmos and the game UI.
//
// Two ties with that neighbor, and that is why they come out in the public interface:
//  - sampler() and mipView0(): the composition's descriptor set needs them.
//  - queryPool(): the timestamp pair measures "bloom + composition", so the
//    closing one is written AFTER the composition render pass, which the
//    Renderer records.
class BloomPass {
public:
    // It must match Renderer::MAX_FRAMES (checked with static_assert in Renderer.cpp).
    static constexpr int      kFramesInFlight = 2;
    static constexpr uint32_t kMaxMips        = 5;

    struct Context {
        GpuDevice&           gpu;
        const RendererState& state;
        // INTERNAL render resolution: the chain starts at half of this.
        const VkExtent2D&    renderExtent;
        // The swapchain's, only for the measurement report.
        const VkExtent2D&    swapChainExtent;
        int                  currentFrame;
        // The scene target: mip 0 reads it with a threshold.
        VkFormat             hdrFormat;
        const VkImageView*   hdrView;   // [kFramesInFlight]
        // They were resolved by the Renderer right before creating the pipelines.
        bool                 timestampsSupported;
        float                timestampPeriod;
    };

    BloomPass()                            = default;
    BloomPass(const BloomPass&)            = delete;
    BloomPass& operator=(const BloomPass&) = delete;

    // What does not depend on the size: sampler, layout, pool, the two pipelines and
    // the query pool. Only once, in init.
    void createPipelines(const Context& ctx);
    void destroyPipelines(const Context& ctx);
    // The mip chain and its sets: they go with the swapchain.
    void createImages(const Context& ctx);
    void destroyImages(const Context& ctx);

    // Down + up. It records nothing without a chain (tiny viewport).
    void record(const Context& ctx, VkCommandBuffer cmd);
    // With the bloom off the composition keeps sampling the chain, so
    // it has to be left black and in GENERAL. It happens ONCE per image (when
    // creating it and when turning the effect off), not every frame.
    void recordClear(const Context& ctx, VkCommandBuffer cmd);
    // Opens the frame's timestamp pair and reads the one from two frames ago. It goes before
    // record(); the closing one is written by the Renderer after the composition.
    void beginQuery(const Context& ctx, VkCommandBuffer cmd);
    // With the bloom off: the measurement is voided and the pair is not opened.
    void skipQuery(const Context& ctx);

    // Levels actually used: a small viewport does not allow kMaxMips. With 0
    // there is nothing to add and the composition forces the intensity to zero.
    uint32_t    mipCount() const { return m_mipCount; }
    // What the composition's descriptor set needs.
    VkImageView mipView0(int frame) const { return m_mipView[frame][0]; }
    VkSampler   sampler()           const { return m_sampler; }
    VkQueryPool queryPool()         const { return m_queryPool; }
    float       gpuMs()             const { return m_gpuMs; }
    // When turning the effect off the chain has to be left black again.
    void markClearPending();

private:
    VkImage               m_image[kFramesInFlight]  = {};
    VkDeviceMemory        m_memory[kFramesInFlight] = {};
    // One 2D view per level: imageStore does not choose the mip, as in the
    // IBL prefilter. The same view acts as storage image and as
    // sampled texture: the image stays in GENERAL for the whole chain,
    // which is a layout valid for both and saves the ping-pong.
    VkImageView           m_mipView[kFramesInFlight][kMaxMips] = {};
    VkExtent2D            m_mipExtent[kMaxMips]                = {};
    uint32_t              m_mipCount                           = 0;
    VkSampler             m_sampler                            = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descLayout                         = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool                           = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout                     = VK_NULL_HANDLE;
    VkPipeline            m_downPipeline                       = VK_NULL_HANDLE;
    VkPipeline            m_upPipeline                         = VK_NULL_HANDLE;
    VkDescriptorSet       m_downSets[kFramesInFlight][kMaxMips] = {};
    VkDescriptorSet       m_upSets[kFramesInFlight][kMaxMips]   = {};
    bool                  m_clearPending[kFramesInFlight]      = {};
    VkQueryPool           m_queryPool                          = VK_NULL_HANDLE;
    bool                  m_queryPending[kFramesInFlight]      = {};
    float                 m_gpuMs                              = 0.0f;
    uint32_t              m_measuredFrames                     = 0;
};

} // namespace DonTopo
