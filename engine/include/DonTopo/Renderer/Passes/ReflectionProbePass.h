#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include "DonTopo/Renderer/Passes/IblPass.h"
#include "DonTopo/Renderer/Passes/ForwardPlusPass.h"
#include "DonTopo/Renderer/RenderObjects.h"
#include "DonTopo/Renderer/SharedGpuMesh.h"

namespace DonTopo {

class GpuDevice;
class Scene;
class Skybox;

// Environment probes: they capture the scene from their position in 6 faces and
// replace the global IBL (bindings 5 and 6 of set 0) on the objects that fall
// inside their radius. The bake is an EVENT: it does not record a single command into the
// frame's command buffer, so the per-frame GPU cost with N already baked
// probes is exactly the same as with 0.
//
// Capture side: a single cubemap SHARED by all the probes (it is an
// intermediate of the bake, it does not persist), created the first time there is something to
// bake. Without probes it is not created and costs nothing.
//
// Ties with code that is not its own, all through the Context:
//  - the bake REDRAWS the scene, so it needs the Renderer's whole offscreen pass
//    (render pass, framebuffer and HDR of slot 0, the two scene pipelines
//    with their layout, the instance set and the mapped UBO) plus the object
//    lists. None of that moves: it travels by reference.
//  - the convolution pipelines and the two global views belong to IblPass.
//  - fp: the bake has to TURN OFF Forward+ while capturing (its grid was
//    culled against the frame camera's frustum, not against the 6 faces).
//    It is the only tie that does not fit in a handle, because it mutates the other
//    pass's state; it is still pass->pass and never pass->Renderer.
class ReflectionProbePass {
public:
    static constexpr uint32_t kFaceSize = 128;
    // 7 pairs: one per face plus the convolution one. The deltas are summed instead of
    // measuring from the first to the last, which would also count the host waits
    // between submits.
    static constexpr uint32_t kQueryCount = 14;

    struct GpuProbe
    {
        uint64_t  ownerId  = 0;          // GameObject::id of the probe
        glm::vec3 position { 0.0f };
        float     radius    = 0.0f;
        float     intensity = 1.0f;
        // The same two images as the global IBL, per probe: irradiance
        // (1 mip) and prefiltered environment (IblPass::kPrefilterMips).
        VkImage        irradianceImage  = VK_NULL_HANDLE;
        VkDeviceMemory irradianceMemory = VK_NULL_HANDLE;
        VkImageView    irradianceView   = VK_NULL_HANDLE;
        VkImageView    irradianceStore  = VK_NULL_HANDLE;
        VkImage        prefilterImage   = VK_NULL_HANDLE;
        VkDeviceMemory prefilterMemory  = VK_NULL_HANDLE;
        VkImageView    prefilterView    = VK_NULL_HANDLE;
        VkImageView    prefilterStore[IblPass::kPrefilterMips] {};
        bool           baked  = false;   // false: still with the neutral one
        float          bakeMs = 0.0f;    // last bake, GPU timestamps
        // Consecutive calls to sync() WITHOUT changes in the probe's settings. The
        // auto-bake waits for it to reach 1: without this, dragging the Intensity
        // slider would trigger a bake per frame (with its vkDeviceWaitIdle and its
        // 7 submits).
        int            settleFrames = 0;
    };

    struct Context {
        GpuDevice& gpu;
        // nullptr = no scene loaded: there is no tree to traverse.
        Scene*     scene;
        // The sky is drawn on each of the six faces.
        Skybox&    skybox;

        // ── IBL global (handles de IblPass) ──────────────────────────────────
        VkPipeline            iblIrradiancePipeline;
        VkPipeline            iblPrefilterPipeline;
        VkPipelineLayout      iblPipelineLayout;
        VkDescriptorPool      iblDescPool;
        VkDescriptorSetLayout iblDescLayout;
        VkSampler             iblSampler;
        VkImageView           globalIrradianceView;
        VkImageView           globalPrefilterView;

        // ── The scene pass, exactly as the frame records it ────────────────────
        // INTERNAL render resolution, not the swapchain's: the face is
        // cropped to min(kFaceSize, extent) because the framebuffer is the viewport's.
        const VkExtent2D& renderExtent;
        VkRenderPass      sceneRenderPass;
        VkFramebuffer     sceneFramebuffer;    // the one of slot 0
        VkImage           hdrImage;            // same, source of the blit
        VkPipeline        scenePipeline;
        VkPipeline        skinnedPipeline;
        VkPipelineLayout  scenePipelineLayout;
        VkDescriptorSet   instanceSet;         // set 1, slot 0
        void*             uboMapped;           // UBO of slot 0, mapped
        // false = no frame has been written yet: the UBO of slot 0 is
        // garbage (no lights or cascade matrices) and the bakes wait.
        bool              uboWritten;
        ForwardPlusPass&  fp;

        // ── What has to be drawn ───────────────────────────────────────────
        const std::vector<RenderObject>&  objects;
        SharedGpuMeshCache&               sharedMeshes;
        std::vector<SkinnedRenderObject>& skinnedObjects;
        uint64_t                          lastCompletedTicket;

        // The AO map of slot 0: it is cleared to 1.0 before capturing, because
        // the one on the GPU is the frame camera's. VK_NULL_HANDLE if
        // SSAO has never created its images.
        VkImage ssaoBlurImage;

        bool  timestampsSupported;
        float timestampPeriod;
    };

    ReflectionProbePass()                                      = default;
    ReflectionProbePass(const ReflectionProbePass&)            = delete;
    ReflectionProbePass& operator=(const ReflectionProbePass&) = delete;

    // Reconciles the probe list with the scene, launches the pending bakes and
    // reassigns probe->object. Once per frame, at the start of drawFrame: it is
    // where one can wait for the GPU to become idle without catching the command
    // buffer half recorded.
    void sync(const Context& ctx);
    void destroy(const Context& ctx);

    // The UI only QUEUES: the bake happens in the next frame's sync.
    void requestBake(uint64_t ownerId) { m_bakeQueue.push_back(ownerId); }
    void requestBakeAll()              { m_bakeAllQueued = true; }

    int   count()      const { return (int)m_probes.size(); }
    // ms of the LAST bake (one probe or the whole batch), by timestamps.
    float lastBakeMs() const { return m_lastBakeMs; }
    // ms of the last bake of ONE specific probe, or -1 if it was never baked.
    float bakeMs(uint64_t ownerId) const
    {
        for (const GpuProbe& p : m_probes)
            if (p.ownerId == ownerId) return p.baked ? p.bakeMs : -1.0f;
        return -1.0f;
    }

    // GPU memory of the persistent captures of ONE probe, in bytes.
    // It does not count the capture cubemap, which is a single one for all.
    static constexpr uint64_t probeMemoryBytes()
    {
        // rgba16f = 8 bytes/texel, 6 faces. The prefilter adds its mips
        // (the series 1 + 1/4 + 1/16 + ... truncated to kPrefilterMips).
        uint64_t pre = 0;
        for (uint32_t m = 0; m < IblPass::kPrefilterMips; m++)
        {
            const uint64_t s = IblPass::kPrefilterSize >> m;
            pre += s * s * 6ull * 8ull;
        }
        return (uint64_t)IblPass::kIrradianceSize * IblPass::kIrradianceSize * 6ull * 8ull + pre;
    }

private:
    // Intermediate bake cubemap and its query pool. The first time there is something
    // to bake.
    void createCapture(const Context& ctx);
    void createProbeImages(const Context& ctx, GpuProbe& probe);
    void destroyProbeImages(const Context& ctx, GpuProbe& probe);
    // The 6 faces + the convolution of ONE probe. Own submits, it does not touch the
    // frame's command buffer.
    void bake(const Context& ctx, GpuProbe& probe);
    // The NEAREST probe whose radius contains the point. -1 = none.
    int  pickProbeFor(const glm::vec3& worldPos) const;
    // Computes the DESIRED assignment and only touches the GPU if it differs from the one
    // already written.
    void refreshAssignment(const Context& ctx);
    // Returns ALL the objects to the global IBL. Right before a batch of
    // bakes, or the capture feeds back on itself.
    void assignAllToGlobalIbl(const Context& ctx);
    // Bindings 5 and 6 of an already allocated set, with the IBL sampler.
    void writeIblBindings(const Context& ctx, VkDescriptorSet set,
                          VkImageView irradiance, VkImageView prefilter) const;

    std::vector<GpuProbe> m_probes;
    VkImage        m_captureImage  = VK_NULL_HANDLE;
    VkDeviceMemory m_captureMemory = VK_NULL_HANDLE;
    VkImageView    m_captureView   = VK_NULL_HANDLE;
    VkQueryPool    m_queryPool     = VK_NULL_HANDLE;

    std::vector<uint64_t> m_bakeQueue;
    bool                  m_bakeAllQueued = false;
    float                 m_lastBakeMs    = 0.0f;

    // Resolved assignment: sharedIndex -> index in m_probes (-1 = global
    // IBL). It is the CACHE of what is already written in the descriptor sets; bindings are only
    // rewritten when the recomputed map differs from this one.
    std::unordered_map<int, int> m_assignShared;
    std::vector<int>             m_assignSkinned;
};

} // namespace DonTopo
