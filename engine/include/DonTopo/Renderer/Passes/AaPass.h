#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include "DonTopo/Renderer/RendererState.h"

namespace DonTopo {

class GpuDevice;
class GpuResources;

// Anti-aliasing: the RESOLVE pass (FXAA, SSAA and TAA), its intermediate
// image, the TAA history and the projection jitter.
//
// What is NOT its own and stays in the Renderer, because it governs others:
//  - the MSAA sample count and the scene and composition render passes
//    that depend on it (the MSAA resolve happens inside those passes,
//    not here),
//  - m_renderExtent and its recomputation,
//  - the query pool, which also times the whole frame without UI: this
//    pass only writes the pair [0,1].
//
// The resolve pass is graphics and not compute: the swapchain is
// B8G8R8A8_SRGB and Vulkan forbids storage images in sRGB formats.
class AaPass {
public:
    // It must match Renderer::MAX_FRAMES (checked with static_assert in Renderer.cpp).
    static constexpr int kFramesInFlight = 2;

    using AaMode = RendererState::AaMode;

    struct Context {
        GpuDevice&           gpu;
        GpuResources&        res;
        const RendererState& state;
        // INTERNAL render resolution (larger than the window with SSAA) and
        // PRESENTATION size: this pass is precisely the one that goes down from one to the other.
        const VkExtent2D&    renderExtent;
        VkExtent2D           viewport;
        int                  currentFrame;
        // BUILT mode, the one that corresponds to the resources that exist now.
        AaMode               activeMode;
        VkFormat             swapChainFormat;
        // Final destination: the image that the UI samples and that the runtime blits.
        const VkImageView*   offscreenView;      // [kFramesInFlight]
        // The scene's depth, which the composition framebuffer shares.
        VkImageView          sceneDepthView;
        // The composition render pass: this pass builds an alternative framebuffer
        // for it that writes into the intermediate image.
        VkRenderPass         compositeRenderPass;
        // The depth from the depth pre-pass and its sampler: TAA reprojects with
        // it (without jitter, which is the geometric one).
        const VkImageView*   prepassDepthView;   // [kFramesInFlight]
        VkSampler            prepassDepthSampler;
        // The pool is owned by the Renderer because it also measures the whole frame.
        VkQueryPool          queryPool;
        bool                 timestampsSupported;
        float                ssaaFactor;
    };

    AaPass()                         = default;
    AaPass(const AaPass&)            = delete;
    AaPass& operator=(const AaPass&) = delete;

    // The two resolve render passes (a one-attachment one, and TAA's two-attachment one)
    // and the three pipelines with their sampler, layouts and pools. Independent
    // of the size and the mode: only once in init.
    void createRenderPasses(const Context& ctx);
    void createPipelines(const Context& ctx);
    void destroyPipelinesAndRenderPasses(const Context& ctx);
    // Intermediate image, TAA history, framebuffers and sets. Everything depends
    // on the size and the mode, so it hangs from
    // createOffscreenImages/destroyOffscreenImages.
    void createImages(const Context& ctx);
    void destroyImages(const Context& ctx);

    // Reads the intermediate image (what the composition wrote) and writes the
    // offscreen with the active mode's pipeline. In None and in MSAA it records
    // NOTHING: the composition will have already written directly to the offscreen.
    void record(const Context& ctx, VkCommandBuffer cmd);

    // prev<-curr handover and frame projection jitter. It is ALWAYS called,
    // also with TAA off: motion blur reprojects with the same two
    // matrices.
    void updateFrameMatrices(const Context& ctx, const glm::mat4& view, const glm::mat4& proj);

    // The projection used by the scene pass: jittered only in TAA.
    const glm::mat4& jitteredProj() const { return m_jitteredProj; }
    // The two consumed by TAA and motion blur.
    const glm::mat4& currViewProj() const { return m_currViewProj; }
    const glm::mat4& prevViewProj() const { return m_prevViewProj; }
    // The framebuffer the composition has to write to when there is a
    // resolve pass behind it. VK_NULL_HANDLE if there is none.
    VkFramebuffer compositeFramebuffer(int frame) const { return m_srcFramebuffer[frame]; }
    // Whether this frame got to write the pair [0,1] of the pool.
    bool passStamped(int frame) const { return m_passStamped[frame]; }

private:
    // Alternative destination of the composition, at INTERNAL resolution.
    VkImage        m_srcImage[kFramesInFlight]       = {};
    VkDeviceMemory m_srcMemory[kFramesInFlight]      = {};
    VkImageView    m_srcView[kFramesInFlight]        = {};
    VkFramebuffer  m_srcFramebuffer[kFramesInFlight] = {};
    // Resolve pass: writes into the offscreen at window size.
    VkRenderPass   m_renderPass                      = VK_NULL_HANDLE;
    VkFramebuffer  m_framebuffer[kFramesInFlight]    = {};
    VkSampler      m_sampler                         = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descLayout               = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool                 = VK_NULL_HANDLE;
    VkDescriptorSet       m_sets[kFramesInFlight]    = {};
    VkPipelineLayout m_fxaaPipelineLayout            = VK_NULL_HANDLE;
    VkPipeline       m_fxaaPipeline                  = VK_NULL_HANDLE;
    VkPipelineLayout m_ssaaPipelineLayout            = VK_NULL_HANDLE;
    VkPipeline       m_ssaaPipeline                  = VK_NULL_HANDLE;
    // TAA: history, its two-attachment render pass and its three-binding sets.
    VkImage        m_historyImage[kFramesInFlight]       = {};
    VkDeviceMemory m_historyMemory[kFramesInFlight]      = {};
    VkImageView    m_historyView[kFramesInFlight]        = {};
    VkFramebuffer  m_historyFramebuffer[kFramesInFlight] = {};
    VkRenderPass   m_historyRenderPass                   = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_taaDescLayout                = VK_NULL_HANDLE;
    VkDescriptorPool      m_taaDescPool                  = VK_NULL_HANDLE;
    VkDescriptorSet       m_taaSets[kFramesInFlight]     = {};
    VkPipelineLayout      m_taaPipelineLayout            = VK_NULL_HANDLE;
    VkPipeline            m_taaPipeline                  = VK_NULL_HANDLE;
    bool                  m_historyValid                 = false;
    // Jitter and matrices of the frame.
    uint32_t   m_jitterIndex  = 0;
    glm::vec2  m_jitter       = glm::vec2(0.0f);
    glm::mat4  m_jitteredProj = glm::mat4(1.0f);
    glm::mat4  m_prevViewProj = glm::mat4(1.0f);
    glm::mat4  m_currViewProj = glm::mat4(1.0f);
    // Whether the frame got to write the pair [0,1]; the pool and the measurement belong
    // to the Renderer, which with the same pool times the whole frame.
    bool m_passStamped[kFramesInFlight] = {};
};

} // namespace DonTopo
