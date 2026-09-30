#include "DonTopo/Renderer/Passes/ReflectionProbePass.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/Skybox.h"
#include "DonTopo/Renderer/UniformBufferObject.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include "DonTopo/Renderer/Passes/IblPass.h"
#include "DonTopo/Core/ReflectionProbeComponent.h"

namespace DonTopo {

// Renderer frames in flight: the size of the descriptor set arrays of
// SharedGpuMesh and SkinnedMatGfx, which this pass rewrites.
static constexpr int kFrames = 2;
static_assert(sizeof(SharedGpuMesh::descriptorSets) / sizeof(VkDescriptorSet) == kFrames,
              "kFrames must follow the number of descriptor sets per shared mesh");

// ── Reflection probes ───────────────────────────────────────────────────────
// Nothing in here records a single command into the frame's command buffer:
// the bake uses its own submits, triggered by an event. With the probes
// already baked, the frame costs exactly the same as with none, because the
// only thing that changes is TWO descriptors (bindings 5 and 6 of set 0) that
// were already there pointing at the global IBL.

void ReflectionProbePass::createCapture(const Context& ctx)
{
    // Intermediate bake cubemap, ONE for all the probes: it only has to live
    // between the render of the 6 faces and the convolution. It is created the
    // first time there is something to bake, so a scene without probes does not
    // spend a single byte on this feature.
    VkImageCreateInfo ci{};
    ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.imageType     = VK_IMAGE_TYPE_2D;
    ci.format        = IblPass::kFormat;
    ci.extent        = { kFaceSize, kFaceSize, 1 };
    ci.mipLevels     = 1;
    ci.arrayLayers   = 6;
    ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    // TRANSFER_DST: destination of the blit from the scene HDR, one face per submit.
    ci.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(ctx.gpu.device(), &ci, nullptr, &m_captureImage) != VK_SUCCESS)
        throw std::runtime_error("failed to create probe capture cubemap!");

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(ctx.gpu.device(), m_captureImage, &memReq);
    VkMemoryAllocateInfo memAlloc{};
    memAlloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memAlloc.allocationSize  = memReq.size;
    memAlloc.memoryTypeIndex = ctx.gpu.findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(ctx.gpu.device(), &memAlloc, nullptr, &m_captureMemory) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate probe capture memory!");
    vkBindImageMemory(ctx.gpu.device(), m_captureImage, m_captureMemory, 0);

    VkImageViewCreateInfo vi{};
    vi.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image                           = m_captureImage;
    vi.viewType                        = VK_IMAGE_VIEW_TYPE_CUBE;
    vi.format                          = IblPass::kFormat;
    vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel   = 0;
    vi.subresourceRange.levelCount     = 1;
    vi.subresourceRange.baseArrayLayer = 0;
    vi.subresourceRange.layerCount     = 6;
    if (vkCreateImageView(ctx.gpu.device(), &vi, nullptr, &m_captureView) != VK_SUCCESS)
        throw std::runtime_error("failed to create probe capture view!");

    // It starts in SHADER_READ_ONLY, which is the layout the bake moves it from
    // to TRANSFER_DST and returns it to. The initial content does not matter:
    // the bake writes the 6 faces before anyone reads them.
    {
        VkCommandBuffer cmd = ctx.gpu.beginOneTimeCommands();
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = m_captureImage;
        b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask       = 0;
        b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        ctx.gpu.endOneTimeCommands(cmd);
    }

    // The bake's own query pool: 7 pairs (6 faces + convolution). It is not
    // shared with the AA one or the bloom one, which are reset every frame.
    if (ctx.timestampsSupported && m_queryPool == VK_NULL_HANDLE)
    {
        VkQueryPoolCreateInfo qi{};
        qi.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = kQueryCount;
        if (vkCreateQueryPool(ctx.gpu.device(), &qi, nullptr, &m_queryPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create probe query pool!");
    }
}

void ReflectionProbePass::createProbeImages(const Context& ctx, GpuProbe& probe)
{
    // The same two images as the global IBL (IblPass::createResources), but
    // per probe. m_res.createImage does not work: it fixes arrayLayers and mipLevels to 1.
    auto makeCube = [&](uint32_t size, uint32_t mips, VkImage& image, VkDeviceMemory& memory)
    {
        VkImageCreateInfo ci{};
        ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.format        = IblPass::kFormat;
        ci.extent        = { size, size, 1 };
        ci.mipLevels     = mips;
        ci.arrayLayers   = 6;
        ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ci.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
                         | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(ctx.gpu.device(), &ci, nullptr, &image) != VK_SUCCESS)
            throw std::runtime_error("failed to create probe cubemap image!");

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(ctx.gpu.device(), image, &memReq);
        VkMemoryAllocateInfo memAlloc{};
        memAlloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memAlloc.allocationSize  = memReq.size;
        memAlloc.memoryTypeIndex = ctx.gpu.findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(ctx.gpu.device(), &memAlloc, nullptr, &memory) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate probe cubemap memory!");
        vkBindImageMemory(ctx.gpu.device(), image, memory, 0);
    };

    makeCube(IblPass::kIrradianceSize, 1,                      probe.irradianceImage, probe.irradianceMemory);
    makeCube(IblPass::kPrefilterSize,  IblPass::kPrefilterMips, probe.prefilterImage,  probe.prefilterMemory);

    auto makeView = [&](VkImage image, VkImageViewType type, uint32_t baseMip, uint32_t mipCount, VkImageView& view)
    {
        VkImageViewCreateInfo vi{};
        vi.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image                           = image;
        vi.viewType                        = type;
        vi.format                          = IblPass::kFormat;
        vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.baseMipLevel   = baseMip;
        vi.subresourceRange.levelCount     = mipCount;
        vi.subresourceRange.baseArrayLayer = 0;
        vi.subresourceRange.layerCount     = 6;
        if (vkCreateImageView(ctx.gpu.device(), &vi, nullptr, &view) != VK_SUCCESS)
            throw std::runtime_error("failed to create probe image view!");
    };

    makeView(probe.irradianceImage, VK_IMAGE_VIEW_TYPE_CUBE,     0, 1, probe.irradianceView);
    makeView(probe.irradianceImage, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, 1, probe.irradianceStore);
    makeView(probe.prefilterImage,  VK_IMAGE_VIEW_TYPE_CUBE,     0, IblPass::kPrefilterMips, probe.prefilterView);
    for (uint32_t m = 0; m < IblPass::kPrefilterMips; m++)
        makeView(probe.prefilterImage, VK_IMAGE_VIEW_TYPE_2D_ARRAY, m, 1, probe.prefilterStore[m]);

    // Neutral content, for the same reason as in IblPass: between the probe
    // existing and someone pressing Bake, its views are already in descriptor sets and
    // cannot point at undefined memory. The same values as the neutral IBL, so a
    // freshly created, unbaked probe looks the same as the usual flat ambient.
    {
        VkCommandBuffer cmd = ctx.gpu.beginOneTimeCommands();

        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.layerCount = 6;

        auto clearTo = [&](VkImage image, uint32_t mips, const VkClearColorValue& color)
        {
            b.image                       = image;
            b.subresourceRange.levelCount = mips;

            b.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);

            vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &color, 1, &b.subresourceRange);

            b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
        };

        const VkClearColorValue irradianceNeutral{{ 0.075f, 0.080f, 0.090f, 1.0f }};
        const VkClearColorValue prefilterNeutral {{ 0.100f, 0.120f, 0.150f, 1.0f }};
        clearTo(probe.irradianceImage, 1,                       irradianceNeutral);
        clearTo(probe.prefilterImage,  IblPass::kPrefilterMips, prefilterNeutral);

        ctx.gpu.endOneTimeCommands(cmd);
    }
}

void ReflectionProbePass::destroyProbeImages(const Context& ctx, GpuProbe& probe)
{
    // The caller has already waited for the GPU to become idle and has rewritten the
    // bindings 5/6 that pointed here: by the time this function is reached, no
    // descriptor set references these views.
    vkDestroyImageView(ctx.gpu.device(), probe.irradianceView,  nullptr);
    vkDestroyImageView(ctx.gpu.device(), probe.irradianceStore, nullptr);
    vkDestroyImage(ctx.gpu.device(), probe.irradianceImage, nullptr);
    vkFreeMemory(ctx.gpu.device(), probe.irradianceMemory, nullptr);
    vkDestroyImageView(ctx.gpu.device(), probe.prefilterView, nullptr);
    for (uint32_t m = 0; m < IblPass::kPrefilterMips; m++)
        vkDestroyImageView(ctx.gpu.device(), probe.prefilterStore[m], nullptr);
    vkDestroyImage(ctx.gpu.device(), probe.prefilterImage, nullptr);
    vkFreeMemory(ctx.gpu.device(), probe.prefilterMemory, nullptr);
    probe = GpuProbe{};
}

void ReflectionProbePass::destroy(const Context& ctx)
{
    // The capture cubemap and the query pool only exist if something was ever
    // baked; the probes, if the scene had any.
    for (GpuProbe& probe : m_probes) destroyProbeImages(ctx, probe);
    m_probes.clear();
    if (m_captureView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(ctx.gpu.device(), m_captureView, nullptr);
        vkDestroyImage(ctx.gpu.device(), m_captureImage, nullptr);
        vkFreeMemory(ctx.gpu.device(), m_captureMemory, nullptr);
        m_captureView = VK_NULL_HANDLE;
    }
    if (m_queryPool != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(ctx.gpu.device(), m_queryPool, nullptr);
        m_queryPool = VK_NULL_HANDLE;
    }
}

void ReflectionProbePass::writeIblBindings(const Context& ctx, VkDescriptorSet set,
                                           VkImageView irradiance, VkImageView prefilter) const
{
    // A standalone write on an ALREADY allocated set, like writeSsaoBinding:
    // rewriting the IBL bindings is the only thing needed for an object to go
    // from the global cubemap to a probe's. No new layout, no new member in the
    // UBO, no index in PushData (which is at exactly 80 bytes).
    //
    // The writes come from IblPass, which owns those two bindings. This was the
    // FOURTH copy of the same block (with the meshes, the characters and IblPass
    // itself), and diverging here is the kind of thing nobody warns you about:
    // the object would sample another object's ambient.
    VkDescriptorImageInfo infos[2]{};
    VkWriteDescriptorSet  w[2]{};
    IblPass::fillIblWrites(set, irradiance, prefilter, ctx.iblSampler, infos, w);
    vkUpdateDescriptorSets(ctx.gpu.device(), 2, w, 0, nullptr);
}

int ReflectionProbePass::pickProbeFor(const glm::vec3& worldPos) const
{
    // The NEAREST probe whose radius contains the point. -1 = none, and then
    // the object keeps the usual global IBL.
    int   best     = -1;
    float bestDist = 0.0f;
    for (size_t i = 0; i < m_probes.size(); i++)
    {
        const float d = glm::length(worldPos - m_probes[i].position);
        if (d > m_probes[i].radius) continue;
        if (best < 0 || d < bestDist) { best = (int)i; bestDist = d; }
    }
    return best;
}

void ReflectionProbePass::refreshAssignment(const Context& ctx)
{
    // Computes the DESIRED assignment and only touches the GPU if it differs from
    // the one already written. In steady state this is a few vector subtractions
    // on the CPU and zero GPU work: no command, no write.
    std::unordered_map<int, int> wantShared;
    for (const auto& obj : ctx.objects)
    {
        const SharedGpuMesh* gpu = ctx.sharedMeshes.get(obj.sharedIndex);
        if (!gpu) continue;
        // The descriptor set is PER SHARED MESH, not per GameObject: two instances
        // of the same mesh under different probes share a probe, and the first
        // object in the traversal wins. That is the price of not duplicating the
        // sets (and with it, instancing).
        if (wantShared.find(obj.sharedIndex) != wantShared.end()) continue;
        const glm::vec3 local  = gpu->hasBounds ? (gpu->aabbMin + gpu->aabbMax) * 0.5f : glm::vec3(0.0f);
        const glm::vec3 center = glm::vec3(obj.transform * glm::vec4(local, 1.0f));
        wantShared[obj.sharedIndex] = pickProbeFor(center);
    }

    std::vector<int> wantSkinned(ctx.skinnedObjects.size(), -1);
    for (size_t i = 0; i < ctx.skinnedObjects.size(); i++)
        wantSkinned[i] = pickProbeFor(glm::vec3(ctx.skinnedObjects[i].transform[3]));

    if (wantShared == m_assignShared && wantSkinned == m_assignSkinned) return;

    // There are changes: the sets may be in use by frames in flight.
    vkDeviceWaitIdle(ctx.gpu.device());

    auto viewsFor = [&](int probeIndex, VkImageView& irr, VkImageView& pre)
    {
        if (probeIndex < 0 || probeIndex >= (int)m_probes.size())
        {
            irr = ctx.globalIrradianceView;
            pre = ctx.globalPrefilterView;
        }
        else
        {
            irr = m_probes[probeIndex].irradianceView;
            pre = m_probes[probeIndex].prefilterView;
        }
    };

    for (const auto& entry : wantShared)
    {
        auto prev = m_assignShared.find(entry.first);
        if (prev != m_assignShared.end() && prev->second == entry.second) continue;
        SharedGpuMesh* gpu = ctx.sharedMeshes.get(entry.first);
        if (!gpu) continue;
        VkImageView irr, pre;
        viewsFor(entry.second, irr, pre);
        for (int i = 0; i < kFrames; i++)
            if (gpu->descriptorSets[i]) writeIblBindings(ctx, gpu->descriptorSets[i], irr, pre);
    }
    // Meshes that are NO LONGER in the desired map (deleted object) do not need
    // to be returned to the global IBL: their sets are freed along with the mesh.

    for (size_t si = 0; si < ctx.skinnedObjects.size(); si++)
    {
        const int want = wantSkinned[si];
        if (si < m_assignSkinned.size() && m_assignSkinned[si] == want) continue;
        VkImageView irr, pre;
        viewsFor(want, irr, pre);
        for (const SkinnedMatGfx& mgfx : ctx.skinnedObjects[si].matGfx)
            for (int i = 0; i < kFrames; i++)
                if (mgfx.descSets[i]) writeIblBindings(ctx, mgfx.descSets[i], irr, pre);
    }

    m_assignShared  = std::move(wantShared);
    m_assignSkinned = std::move(wantSkinned);
}

void ReflectionProbePass::assignAllToGlobalIbl(const Context& ctx)
{
    // Returns ALL the objects to the global IBL. It is called right before a
    // batch of bakes and it is not a detail: the capture reuses the scene pass,
    // which lights each object with whatever it has in its bindings 5/6. If that
    // is the probe's own cubemap, each bake captures again the light that already
    // carried the intensity applied, and the effect is amplified from bake to bake
    // (or fades out, with low intensities). Always capturing with the global
    // IBL makes the bake idempotent and independent of the order of the probes.
    if (m_assignShared.empty() && m_assignSkinned.empty()) return;

    vkDeviceWaitIdle(ctx.gpu.device());
    for (int index : ctx.sharedMeshes.liveIndices())
    {
        SharedGpuMesh* gpu = ctx.sharedMeshes.get(index);
        for (int i = 0; i < kFrames; i++)
            if (gpu->descriptorSets[i])
                writeIblBindings(ctx, gpu->descriptorSets[i],
                                 ctx.globalIrradianceView, ctx.globalPrefilterView);
    }
    for (const SkinnedRenderObject& sobj : ctx.skinnedObjects)
        for (const SkinnedMatGfx& mgfx : sobj.matGfx)
            for (int i = 0; i < kFrames; i++)
                if (mgfx.descSets[i])
                    writeIblBindings(ctx, mgfx.descSets[i],
                                     ctx.globalIrradianceView, ctx.globalPrefilterView);

    // The caches are left empty on purpose: refreshAssignment, at the end of
    // sync(), writes the real assignment again.
    m_assignShared.clear();
    m_assignSkinned.clear();
}

void ReflectionProbePass::bake(const Context& ctx, GpuProbe& probe)
{
    // Without a scene framebuffer (early init) or without the instance SSBO
    // there is nothing to draw against: the request is retried in another frame.
    if (ctx.sceneFramebuffer == VK_NULL_HANDLE) return;
    if (ctx.instanceSet      == VK_NULL_HANDLE) return;

    if (m_captureImage == VK_NULL_HANDLE) createCapture(ctx);

    // The 6 faces draw onto the HDR of slot 0 and read the UBO of frame 0,
    // which may be in flight. This is an event, not a pass: waiting is fine.
    vkDeviceWaitIdle(ctx.gpu.device());

    const uint32_t faceRender = std::min(kFaceSize,
                                         std::min(ctx.renderExtent.width, ctx.renderExtent.height));
    if (faceRender == 0) return;

    // Base: the UBO of frame 0 AS IS. Lights, cascade matrices and splits are
    // kept on purpose: the shadow map on the GPU is the one for those
    // matrices, and recomputing them here would put it out of sync.
    UniformBufferObject ubo{};
    memcpy(&ubo, ctx.uboMapped, sizeof(ubo));
    ubo.viewPos = glm::vec4(probe.position, 1.0f);

    // Forward+ set to Off during the capture: its grid was culled against the
    // frustum of the frame's camera, not against these 6 faces. mode 0 is the
    // classic loop over the UBO lights, with all of them. It is restored on
    // exit; the mode the UI has requested is not touched.
    ForwardPlusPass::ParamsGpu savedFp{};
    const bool restoreFp = ctx.fp.overrideModeOff(savedFp);

    // Directions and "up" of the 6 faces. The ups are the OPPOSITE of those in
    // the classic OpenGL list, and the projection flips X in addition to Vulkan's
    // Y: two mirrors make a rotation, so the winding (and with it the pipeline's
    // face culling) is preserved, and the face comes out with the orientation
    // that samplerCube sampling expects.
    static const glm::vec3 kDirs[6] = {
        {  1.0f,  0.0f,  0.0f }, { -1.0f,  0.0f,  0.0f },
        {  0.0f,  1.0f,  0.0f }, {  0.0f, -1.0f,  0.0f },
        {  0.0f,  0.0f,  1.0f }, {  0.0f,  0.0f, -1.0f },
    };
    static const glm::vec3 kUps[6] = {
        {  0.0f,  1.0f,  0.0f }, {  0.0f,  1.0f,  0.0f },
        {  0.0f,  0.0f, -1.0f }, {  0.0f,  0.0f,  1.0f },
        {  0.0f,  1.0f,  0.0f }, {  0.0f,  1.0f,  0.0f },
    };

    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

    for (uint32_t face = 0; face < 6; face++)
    {
        glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f,
                                               1.0f, 20000.0f);
        proj[0][0] *= -1.0f;
        proj[1][1] *= -1.0f;
        ubo.view = glm::lookAtRH(probe.position, probe.position + kDirs[face], kUps[face]);
        ubo.proj = proj;
        memcpy(ctx.uboMapped, &ubo, sizeof(ubo));

        VkCommandBuffer cmd = ctx.gpu.beginOneTimeCommands();

        if (ctx.timestampsSupported && m_queryPool != VK_NULL_HANDLE)
        {
            // Single reset of all 14 in the first submit: the writes of the following
            // submits go behind it in the same queue.
            if (face == 0) vkCmdResetQueryPool(cmd, m_queryPool, 0, kQueryCount);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_queryPool, face * 2);
        }

        // The AO map on the GPU is the frame camera's: leaving it would bake
        // occlusion from another point of view into the cubemap. At 1.0
        // = no occlusion; the next frame recomputes it if SSAO is
        // active, and if it is not, it was already 1.0.
        if (face == 0 && ctx.ssaoBlurImage != VK_NULL_HANDLE)
        {
            // oldLayout UNDEFINED and not GENERAL: if SSAO has never run
            // on this slot the image has never been transitioned, and the
            // draws below sample it through binding 7 (declared
            // GENERAL). Discarding the content costs nothing: it is cleared.
            VkImageMemoryBarrier ao{};
            ao.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            ao.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ao.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ao.image               = ctx.ssaoBlurImage;
            ao.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            ao.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            ao.srcAccessMask       = 0;
            ao.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            ao.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &ao);

            const VkClearColorValue white{{ 1.0f, 1.0f, 1.0f, 1.0f }};
            vkCmdClearColorImage(cmd, ctx.ssaoBlurImage, VK_IMAGE_LAYOUT_GENERAL,
                                 &white, 1, &ao.subresourceRange);

            ao.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            ao.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            ao.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &ao);
        }

        VkClearValue clearValues[2];
        clearValues[0].color        = {0.0f, 0.0f, 0.0f, 1.0f};
        clearValues[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rpInfo{};
        rpInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpInfo.renderPass        = ctx.sceneRenderPass;
        rpInfo.framebuffer       = ctx.sceneFramebuffer;
        // Square and in the corner: the framebuffer is the viewport's (16:9
        // with any luck) and a cubemap face has to come out of an
        // aspect 1 projection. The rest of the framebuffer is not touched at all.
        rpInfo.renderArea.offset = {0, 0};
        rpInfo.renderArea.extent = { faceRender, faceRender };
        rpInfo.clearValueCount   = 2;
        rpInfo.pClearValues      = clearValues;
        vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.width    = (float)faceRender;
        viewport.height   = (float)faceRender;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.extent = { faceRender, faceRender };
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // The usual scene pipeline. Wireframe is NOT honored here on
        // purpose: what is captured is the lit environment, not the editing
        // aid.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx.scenePipeline);
        // Set 1: the instance SSBO is still mandatory (the vertex
        // shader declares it), even though nothing is instanced here.
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            ctx.scenePipelineLayout, 1, 1, &ctx.instanceSet, 0, nullptr);
        const VkDescriptorSet fpBakeSet = ctx.fp.set(0);
        if (fpBakeSet != VK_NULL_HANDLE)
        {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                ctx.scenePipelineLayout, 2, 1, &fpBakeSet, 0, nullptr);
        }

        for (const auto& obj : ctx.objects)
        {
            const SharedGpuMesh* gpu = ctx.sharedMeshes.get(obj.sharedIndex);
            if (!gpu || gpu->uploadTicket > ctx.lastCompletedTicket) continue;
            if (!gpu->descriptorSets[0]) continue;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                ctx.scenePipelineLayout, 0, 1, &gpu->descriptorSets[0], 0, nullptr);
            PushData push;
            // No instancing (flags.x = 0): the matrix goes in the push, which is
            // the path the skinned meshes already use. That way the bake does not touch the
            // frame's SSBO or its cursor.
            push.transform = obj.transform;
            // From the OBJECT, like the transform on the line above: the factors
            // no longer live in the shared entry. There is no grouping to consult
            // here (the bake draws object by object), so they are read
            // directly, with no intermediary.
            push.metallic  = obj.metallic;
            push.roughness = obj.roughness;
            push.flags.x   = 0.0f;
            // flags.y = 0: the HDR alpha is the SSR mask and there is no SSR
            // pass here to read it.
            push.flags.y   = 0.0f;
            vkCmdPushConstants(cmd, ctx.scenePipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(PushData), &push);
            VkBuffer vbs[]      = { gpu->vertexBuffer };
            VkDeviceSize offs[] = { 0 };
            vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offs);
            vkCmdBindIndexBuffer(cmd, gpu->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, gpu->indexCount, 1, 0, 0, 0);
        }

        if (!ctx.skinnedObjects.empty())
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx.skinnedPipeline);
            for (SkinnedRenderObject& sobj : ctx.skinnedObjects)
            {
                if (sobj.outputVertexBuffer == VK_NULL_HANDLE) continue;
                if (sobj.uploadTicket > ctx.lastCompletedTicket) continue;
                VkBuffer     vbs[]  = { sobj.outputVertexBuffer };
                VkDeviceSize offs[] = { 0 };
                vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offs);
                vkCmdBindIndexBuffer(cmd, sobj.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                for (auto& sm : sobj.subMeshes)
                {
                    SkinnedMatGfx& mgfx = sobj.matGfx[sm.materialIndex];
                    if (!mgfx.descSets[0]) continue;
                    PushData push;
                    push.transform = sobj.transform;
                    push.metallic  = mgfx.metallic;
                    push.roughness = mgfx.roughness;
                    push.flags.y   = 0.0f;
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        ctx.scenePipelineLayout, 0, 1, &mgfx.descSets[0], 0, nullptr);
                    vkCmdPushConstants(cmd, ctx.scenePipelineLayout,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(PushData), &push);
                    vkCmdDrawIndexed(cmd, sm.indexCount, 1, sm.indexStart, 0, 0);
                }
            }
        }

        if (ctx.skybox.isInitialized())
        {
            glm::mat4 rotView     = glm::mat4(glm::mat3(ubo.view));
            glm::mat4 invViewProj = glm::inverse(ubo.proj * rotView);
            ctx.skybox.draw(cmd, invViewProj);
        }

        vkCmdEndRenderPass(cmd);

        // ── Face -> capture cubemap layer ────────────────────────────────
        // The pass leaves the HDR in SHADER_READ_ONLY (finalLayout of the
        // resolve attachment, and of the color one without MSAA).
        b.image            = ctx.hdrImage;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask    = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask    = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

        VkImageMemoryBarrier toDst = b;
        toDst.image            = m_captureImage;
        toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, face, 1 };
        toDst.oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toDst.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcAccessMask    = VK_ACCESS_SHADER_READ_BIT;
        toDst.dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

        // Blit and not copy: the render goes out to faceRender (cropped to the
        // viewport size) and the cubemap face is always kFaceSize,
        // so it has to be scaled.
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[0]  = { 0, 0, 0 };
        blit.srcOffsets[1]  = { (int32_t)faceRender, (int32_t)faceRender, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, face, 1 };
        blit.dstOffsets[0]  = { 0, 0, 0 };
        blit.dstOffsets[1]  = { (int32_t)kFaceSize, (int32_t)kFaceSize, 1 };
        vkCmdBlitImage(cmd,
                       ctx.hdrImage,     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       m_captureImage,   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);

        // Back to the starting layouts: the HDR is read by the bloom and the
        // composition of the next frame, and the capture is read by the compute.
        toDst.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toDst.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDst.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

        b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

        if (ctx.timestampsSupported && m_queryPool != VK_NULL_HANDLE)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool, face * 2 + 1);

        // Blocks until the queue is empty: the UBO of frame 0 is rewritten
        // on the next iteration and a draw in flight cannot be overwritten.
        ctx.gpu.endOneTimeCommands(cmd);
    }

    // ── Convolution: the SAME two computes as the global IBL ──────────────
    {
        vkResetDescriptorPool(ctx.gpu.device(), ctx.iblDescPool, 0);

        const uint32_t setCount = 1 + IblPass::kPrefilterMips;
        std::vector<VkDescriptorSetLayout> layouts(setCount, ctx.iblDescLayout);
        std::vector<VkDescriptorSet>       sets(setCount, VK_NULL_HANDLE);

        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = ctx.iblDescPool;
        ai.descriptorSetCount = setCount;
        ai.pSetLayouts        = layouts.data();
        if (vkAllocateDescriptorSets(ctx.gpu.device(), &ai, sets.data()) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate probe descriptor sets!");

        VkDescriptorImageInfo envInfo{};
        envInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        envInfo.imageView   = m_captureView;
        envInfo.sampler     = ctx.iblSampler;

        std::vector<VkDescriptorImageInfo> storeInfos(setCount);
        std::vector<VkWriteDescriptorSet>  writes;
        writes.reserve(setCount * 2);
        for (uint32_t s = 0; s < setCount; s++)
        {
            storeInfos[s].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            storeInfos[s].imageView   = (s == 0) ? probe.irradianceStore : probe.prefilterStore[s - 1];

            VkWriteDescriptorSet src{};
            src.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            src.dstSet          = sets[s];
            src.dstBinding      = 0;
            src.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            src.descriptorCount = 1;
            src.pImageInfo      = &envInfo;
            writes.push_back(src);

            VkWriteDescriptorSet dst = src;
            dst.dstBinding     = 1;
            dst.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            dst.pImageInfo     = &storeInfos[s];
            writes.push_back(dst);
        }
        vkUpdateDescriptorSets(ctx.gpu.device(), (uint32_t)writes.size(), writes.data(), 0, nullptr);

        VkCommandBuffer cmd = ctx.gpu.beginOneTimeCommands();
        if (ctx.timestampsSupported && m_queryPool != VK_NULL_HANDLE)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_queryPool, 12);

        VkImageMemoryBarrier conv[2]{};
        for (int i = 0; i < 2; i++)
        {
            conv[i].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            conv[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            conv[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            conv[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            conv[i].subresourceRange.layerCount = 6;
        }
        conv[0].image = probe.irradianceImage;
        conv[0].subresourceRange.levelCount = 1;
        conv[1].image = probe.prefilterImage;
        conv[1].subresourceRange.levelCount = IblPass::kPrefilterMips;

        for (int i = 0; i < 2; i++)
        {
            conv[i].oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            conv[i].newLayout     = VK_IMAGE_LAYOUT_GENERAL;
            conv[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            conv[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 2, conv);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.iblIrradiancePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.iblPipelineLayout,
                                0, 1, &sets[0], 0, nullptr);
        IblPass::Push push{ 0.0f, IblPass::kIrradianceSize, probe.intensity };
        vkCmdPushConstants(cmd, ctx.iblPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        const uint32_t irrGroups = (IblPass::kIrradianceSize + 7) / 8;
        vkCmdDispatch(cmd, irrGroups, irrGroups, 6);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.iblPrefilterPipeline);
        for (uint32_t m = 0; m < IblPass::kPrefilterMips; m++)
        {
            const uint32_t mipSize = IblPass::kPrefilterSize >> m;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.iblPipelineLayout,
                                    0, 1, &sets[1 + m], 0, nullptr);
            IblPass::Push mipPush{ (float)m / (float)(IblPass::kPrefilterMips - 1), mipSize, probe.intensity };
            vkCmdPushConstants(cmd, ctx.iblPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mipPush), &mipPush);
            const uint32_t groups = (mipSize + 7) / 8;
            vkCmdDispatch(cmd, groups, groups, 6);
        }

        for (int i = 0; i < 2; i++)
        {
            conv[i].oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
            conv[i].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            conv[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            conv[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 2, conv);

        if (ctx.timestampsSupported && m_queryPool != VK_NULL_HANDLE)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool, 13);

        ctx.gpu.endOneTimeCommands(cmd);
    }

    if (restoreFp) ctx.fp.restoreParams(savedFp);

    // The UBO of frame 0 is left with the last face; updateUniformBuffer rewrites
    // it entirely before the next submit of the frame, so it does not
    // need to be restored.

    probe.baked  = true;
    probe.bakeMs = 0.0f;
    if (ctx.timestampsSupported && m_queryPool != VK_NULL_HANDLE)
    {
        uint64_t stamps[kQueryCount] = {};
        // WAIT_BIT and not polling: the queue is already empty (endOneTimeCommands
        // blocks), so all 14 results are ready.
        if (vkGetQueryPoolResults(ctx.gpu.device(), m_queryPool, 0, kQueryCount,
                                  sizeof(stamps), stamps, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
        {
            // Sum of the 7 deltas and not last-minus-first: between submits
            // there are host waits that are not GPU cost.
            double total = 0.0;
            for (uint32_t p = 0; p < kQueryCount / 2; p++)
                total += (double)(stamps[p * 2 + 1] - stamps[p * 2]) * ctx.timestampPeriod * 1e-6;
            probe.bakeMs = (float)total;
        }
    }
}

void ReflectionProbePass::sync(const Context& ctx)
{
    // Fast path: no probes in the scene and nothing to undo. It is the case of
    // ALL of today's scenes, and it leaves here without touching the GPU.
    const bool nothingToDo = m_probes.empty() && m_assignShared.empty()
                          && m_assignSkinned.empty() && m_bakeQueue.empty()
                          && !m_bakeAllQueued;
    if (!ctx.scene && nothingToDo) return;

    // 1. Reconcile the probe list with the scene. It is the only thing that runs
    //    per frame when there are probes: one tree traversal (the same one the
    //    gizmo and the physics already do) and a few float comparisons.
    struct Desc { uint64_t id; glm::vec3 pos; float radius; float intensity; };
    std::vector<Desc> descs;
    if (ctx.scene)
    {
        ctx.scene->traverse([&](GameObject* go) {
            if (!go->hasReflectionProbe()) return;
            const auto& p = go->getReflectionProbe();
            descs.push_back({ go->id, glm::vec3(go->worldTransform[3]),
                              p->getRadius(), p->getIntensity() });
        });
    }
    if (descs.empty() && nothingToDo) return;

    // Removals: probes whose GameObject is no longer there (deleted or scene change).
    for (size_t i = m_probes.size(); i-- > 0; )
    {
        bool alive = false;
        for (const Desc& d : descs) if (d.id == m_probes[i].ownerId) { alive = true; break; }
        if (alive) continue;
        // BEFORE destroying: return to the global IBL everything that pointed at
        // this probe, or descriptor sets with dead views would remain. It is
        // removed from the list first so that pickProbeFor no longer picks it.
        GpuProbe dying = m_probes[i];
        m_probes.erase(m_probes.begin() + (long)i);
        m_assignShared.clear();     // forces the rewrite of all of them
        m_assignSkinned.clear();
        refreshAssignment(ctx);
        vkDeviceWaitIdle(ctx.gpu.device());
        destroyProbeImages(ctx, dying);
    }

    // Additions and settings changes.
    bool geometryChanged = false;
    for (const Desc& d : descs)
    {
        GpuProbe* found = nullptr;
        for (GpuProbe& p : m_probes) if (p.ownerId == d.id) { found = &p; break; }
        if (!found)
        {
            GpuProbe fresh{};
            fresh.ownerId = d.id;
            createProbeImages(ctx, fresh);
            m_probes.push_back(fresh);
            found = &m_probes.back();
            geometryChanged = true;
        }
        if (found->position != d.pos || found->radius != d.radius)
            geometryChanged = true;
        // Moving the probe invalidates what was captured, and changing the intensity
        // invalidates the convolved cubemap (the intensity is baked into
        // it). The radius does NOT: it only changes whom it affects, not what is seen.
        const bool dirty = (found->position != d.pos) || (found->intensity != d.intensity);
        if (dirty) { found->baked = false; found->settleFrames = 0; }
        else if (!found->baked) found->settleFrames++;
        found->position  = d.pos;
        found->radius    = d.radius;
        found->intensity = d.intensity;
    }
    (void)geometryChanged;

    // 2. Bakes. Without a previous frame the UBO of slot 0 is garbage (it has neither
    //    lights nor the shadow map matrices): the requests wait.
    if (ctx.uboWritten)
    {
        const bool bakeAll = m_bakeAllQueued;
        m_bakeAllQueued = false;
        std::vector<uint64_t> queue;
        queue.swap(m_bakeQueue);

        std::vector<GpuProbe*> toBake;
        for (GpuProbe& p : m_probes)
        {
            if (bakeAll || std::find(queue.begin(), queue.end(), p.ownerId) != queue.end())
            {
                toBake.push_back(&p);
                continue;
            }
            // Auto-bake of the probes without a valid capture: it is what makes
            // loading a scene (or starting DonTopoRuntime) give the same
            // image as the editor without pressing anything. settleFrames waits for
            // the settings to stop moving, so dragging a slider
            // does not trigger a bake per frame: only one on release.
            if (!p.baked && p.settleFrames >= 1) toBake.push_back(&p);
        }

        float total = 0.0f;
        int   count = 0;
        if (!toBake.empty())
        {
            // BEFORE capturing anything: otherwise the scene is photographed
            // lit by the probes themselves and the effect feeds back on itself.
            assignAllToGlobalIbl(ctx);
            for (GpuProbe* p : toBake)
            {
                bake(ctx, *p);
                if (p->baked) { total += p->bakeMs; count++; }
            }
        }
        if (count > 0)
        {
            m_lastBakeMs = total;
            printf("reflection probes: bake of %d probe(s) in %.2f ms of GPU "
                   "(capture %ux%u x6, irradiance %ux%u, prefiltered %ux%u x%u mips, "
                   "%.2f MB per probe)\n",
                   count, total, kFaceSize, kFaceSize,
                   IblPass::kIrradianceSize, IblPass::kIrradianceSize,
                   IblPass::kPrefilterSize, IblPass::kPrefilterSize, IblPass::kPrefilterMips,
                   (double)probeMemoryBytes() / (1024.0 * 1024.0));
            fflush(stdout);
        }
    }

    // 3. Probe->object assignment. It returns without writing anything if nothing changed.
    refreshAssignment(ctx);
}

} // namespace DonTopo
