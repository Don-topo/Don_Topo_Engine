#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include "DonTopo/Renderer/RendererState.h"
#include "DonTopo/Renderer/UniformBufferObject.h"

namespace DonTopo {

class GpuDevice;
class GpuResources;

// Forward+: the light culling dispatch and the buffers that pbr.frag
// consumes. Unlike the other passes, this one does NOT write an image: it fills
// a grid read by the SCENE pass, so its descriptor set and its layout
// come out in the public interface.
//
// Ties with code that is not its own:
//  - descLayout(): it is set 2 of the scene pipeline layout.
//  - set(frame): bound by the scene pass and also by the probe bake.
//  - overrideModeOff()/restoreParams(): the probe bake turns Forward+ off
//    while capturing the six faces, because the grid was culled against the
//    frame camera's frustum and not against those faces.
class ForwardPlusPass {
public:
    // It must match Renderer::MAX_FRAMES (checked with static_assert in Renderer.cpp).
    static constexpr int kFramesInFlight = 2;

    // Cap of lights that enter the culling and, at the same time, width of the
    // bit mask of light_cull_tiled.comp (256 / 32 = 8 words).
    static constexpr uint32_t kMaxLights     = 256;
    // Cap of lights per cell. A cell that goes over it LOSES lights: that is
    // why they are counted separately and shown in the UI.
    static constexpr uint32_t kMaxPerCell    = 64;
    static constexpr uint32_t kTileSize      = 16;   // tiled
    static constexpr uint32_t kClusterTile   = 64;   // clustered, XY
    static constexpr uint32_t kClusterSlices = 24;   // clustered, Z

    // Parameter block exactly as declared by the two .comp files and pbr.frag.
    // std430 with only 4-byte scalars: the offsets are sequential.
    struct ParamsGpu {
        uint32_t mode;
        uint32_t gridX;
        uint32_t gridY;
        uint32_t gridZ;
        uint32_t tileSize;
        uint32_t maxPerCell;
        uint32_t numLights;
        uint32_t pad0;
        float    zNear;
        float    zFar;
        float    sliceScale;
        float    sliceBias;
    };
    static_assert(sizeof(ParamsGpu) == 48, "ParamsGpu must stay at 48 bytes: both .comp shaders and pbr.frag declare this layout");

    struct Context {
        GpuDevice&        gpu;
        GpuResources&     res;
        // INTERNAL render resolution and NOT the swapchain's: with SSAA the
        // render is larger than the window, and sizing with the window's
        // would leave pbr.frag reading cells outside the buffer.
        const VkExtent2D& renderExtent;
        int               currentFrame;
        // The frame's FROZEN mode, not the one the UI requests.
        RendererState::FpMode activeMode;
        // The depth from the depth pre-pass and its sampler: the tiled culling
        // reduces each tile's maximum depth from it.
        const VkImageView*    depthView;   // [kFramesInFlight]
        VkSampler             depthSampler;
        bool                  timestampsSupported;
        float                 timestampPeriod;
    };

    ForwardPlusPass()                                  = default;
    ForwardPlusPass(const ForwardPlusPass&)            = delete;
    ForwardPlusPass& operator=(const ForwardPlusPass&) = delete;

    // Layout, pool, the two pipelines, the buffers that do NOT depend on the size
    // (parameters, lights and counters, all with persistent mapping) and the query
    // pool. Only once, in init.
    void createPipelines(const Context& ctx);
    void destroyPipelines(const Context& ctx);
    // The grid and the index list, plus the descriptor sets: they depend on the
    // size, so they go with the swapchain.
    void createBuffers(const Context& ctx);
    void destroyBuffers(const Context& ctx);

    // The culling dispatch of the active mode. It goes AFTER the depth pre-pass
    // (the tiled one needs it) and BEFORE the scene pass.
    void record(const Context& ctx, VkCommandBuffer cmd, const glm::mat4& proj);
    // Parameter block and light list of the frame. It is ALWAYS written,
    // also in Off: pbr.frag reads the mode from here to decide which branch to
    // take.
    void uploadFrameData(const Context& ctx, const glm::mat4& view, const glm::mat4& proj,
                         const std::vector<Light>& lights,
                         const std::vector<float>& lightRadii, float defaultRadius);

    // Dimensions of the grid of the given mode with the context's extent.
    void gridDims(const Context& ctx, RendererState::FpMode mode,
                  uint32_t& gridX, uint32_t& gridY, uint32_t& gridZ, uint32_t& tileSize) const;

    // Set 2 of the scene pipeline and its layout.
    VkDescriptorSetLayout descLayout()    const { return m_descLayout; }
    VkDescriptorSet       set(int frame)  const { return m_sets[frame]; }

    // Leaves the mode at Off without touching the one the UI requests, and returns what was there
    // so it can be restored. false if the buffer does not exist yet.
    bool overrideModeOff(ParamsGpu& saved);
    void restoreParams(const ParamsGpu& saved);

    float    gpuMs()         const { return m_gpuMs; }
    float    avgPerCell()    const { return m_avgPerCell; }
    uint32_t overflowCells() const { return m_overflowCells; }

private:
    VkDescriptorSetLayout m_descLayout                      = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool                        = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout                  = VK_NULL_HANDLE;
    VkPipeline            m_tiledPipeline                   = VK_NULL_HANDLE;
    VkPipeline            m_clusteredPipeline               = VK_NULL_HANDLE;
    VkDescriptorSet       m_sets[kFramesInFlight]           = {};
    // Persistent mapping, like the UBO.
    VkBuffer              m_paramsBuffer[kFramesInFlight]   = {};
    VkDeviceMemory        m_paramsMemory[kFramesInFlight]   = {};
    void*                 m_paramsMapped[kFramesInFlight]   = {};
    VkBuffer              m_lightBuffer[kFramesInFlight]    = {};
    VkDeviceMemory        m_lightMemory[kFramesInFlight]    = {};
    void*                 m_lightMapped[kFramesInFlight]    = {};
    VkBuffer              m_statsBuffer[kFramesInFlight]    = {};
    VkDeviceMemory        m_statsMemory[kFramesInFlight]    = {};
    void*                 m_statsMapped[kFramesInFlight]    = {};
    // They depend on the grid's size.
    VkBuffer              m_gridBuffer[kFramesInFlight]     = {};
    VkDeviceMemory        m_gridMemory[kFramesInFlight]     = {};
    VkBuffer              m_indexBuffer[kFramesInFlight]    = {};
    VkDeviceMemory        m_indexMemory[kFramesInFlight]    = {};
    VkQueryPool           m_queryPool                       = VK_NULL_HANDLE;
    bool                  m_queryPending[kFramesInFlight]   = {};
    float                 m_gpuMs                           = 0.0f;
    float                 m_avgPerCell                      = 0.0f;
    uint32_t              m_overflowCells                   = 0;
    uint32_t              m_measuredFrames                  = 0;
};

} // namespace DonTopo
