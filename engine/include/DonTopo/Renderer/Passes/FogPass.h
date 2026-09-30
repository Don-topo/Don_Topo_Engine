#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include "DonTopo/Renderer/UniformBufferObject.h"

namespace DonTopo {

class GpuDevice;
class RendererState;

// Volumetric fog. The whole pass (pipeline, descriptor sets, time queries
// and recording) lives here; the Renderer remains the owner of the
// instance and the one that decides WHEN each thing is called.
class FogPass {
public:
    // It must match Renderer::MAX_FRAMES (checked with static_assert in Renderer.cpp).
    static constexpr int kFramesInFlight = 2;

    // What the pass needs from the Renderer and is NOT its own. It is built at the
    // call site and passed by reference: do not store it, since the
    // handles are recreated with the swapchain.
    struct Context {
        GpuDevice&           gpu;
        const RendererState& state;
        // INTERNAL render resolution (the HDR's), not the swapchain's.
        const VkExtent2D&    renderExtent;
        int                  currentFrame;
        // The fog has no image of its own: it rewrites the HDR in place.
        const VkImage*       hdrImage;       // [kFramesInFlight]
        const VkImageView*   hdrView;        // [kFramesInFlight]
        const VkImageView*   ssaoDepthView;  // [kFramesInFlight]
        VkSampler            ssaoSampler;
        // The frame's UBO: view matrix, cuts and cascade matrices.
        const VkBuffer*      uniformBuffers; // [kFramesInFlight]
        // The same view+sampler pair that pbr.frag samples.
        VkImageView          shadowView;
        VkSampler            shadowSampler;
        // The key light is m_lights[0]; without lights the fog only absorbs.
        const std::vector<Light>& lights;
        // Where a POINT light aims. The SAME value the cascades received this
        // frame: if the two do not match, the in-scattering points to one side
        // and the shadow map is built towards another.
        glm::vec3            sceneCenter;
        // They were resolved by the bloom; here they are only read.
        bool                 timestampsSupported;
        float                timestampPeriod;
    };

    FogPass()                          = default;
    FogPass(const FogPass&)            = delete;
    FogPass& operator=(const FogPass&) = delete;

    // What does not depend on the size: layout, pool, pipeline and the query
    // pool. Only once, in init.
    void createPipelines(const Context& ctx);
    // Counterpart of createPipelines, in cleanup.
    void destroyPipelines(const Context& ctx);
    // The sets go with the swapchain: they reference hdrView and ssaoDepthView, which
    // are recreated with it.
    void createSets(const Context& ctx);
    void destroySets();
    // A single dispatch that rewrites the HDR in place. It goes AFTER the scene
    // pass and the SSR (it needs the already lit color with the reflections
    // added) and BEFORE the bloom, so that the fog blooms and goes through the
    // tonemap like the rest of the image.
    void record(const Context& ctx, VkCommandBuffer cmd, const glm::mat4& view, const glm::mat4& proj);

    // GPU cost of the dispatch in ms. 0 if it is off or the device does not
    // support timestamps.
    float gpuMs() const { return m_gpuMs; }

private:
    VkDescriptorSetLayout m_descLayout            = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool              = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout        = VK_NULL_HANDLE;
    VkPipeline            m_pipeline              = VK_NULL_HANDLE;
    VkDescriptorSet       m_sets[kFramesInFlight] = {};
    // Two queries per frame that bound the single dispatch. The depth pre-pass
    // does NOT count here: SSAO or SSR already measure it when they are the ones
    // requesting it.
    VkQueryPool           m_queryPool                  = VK_NULL_HANDLE;
    bool                  m_queryPending[kFramesInFlight] = {};
    float                 m_gpuMs                      = 0.0f;
    uint32_t              m_measuredFrames             = 0;
};

} // namespace DonTopo
