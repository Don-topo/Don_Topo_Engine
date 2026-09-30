#include "DonTopo/Renderer/Passes/BloomPass.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/RendererState.h"
#include <glm/glm.hpp>
#include <stdexcept>
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include "DonTopo/Renderer/ShaderModule.h"

namespace DonTopo {

// ── helpers ──────────────────────────────────────────────────────────────────

// Shared by bloom_down.comp and bloom_up.comp: they share a pipeline
// layout, so they declare the same block even though each one ignores
// part of the fields.
struct BloomPush {
    float    srcTexelX;
    float    srcTexelY;
    float    threshold;
    float    knee;
    float    radius;
    int32_t  prefilter;
};

// ── Bloom ───────────────────────────────────────────────────────────────────
void BloomPass::markClearPending()
{
    for (int i = 0; i < kFramesInFlight; i++) m_clearPending[i] = true;
}

void BloomPass::createPipelines(const Context& ctx)
{
    // Common sampler of the whole chain. CLAMP_TO_EDGE is mandatory: with
    // repeat, the filter's edge taps would bring in brightness from the opposite
    // side of the screen and light would be seen bleeding through the edges.
    VkSamplerCreateInfo si{};
    si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(ctx.gpu.device(), &si, nullptr, &m_sampler) != VK_SUCCESS)
        throw std::runtime_error("failed to create bloom sampler!");

    // --- Compute: sampled source + destination as a storage image ---------
    VkDescriptorSetLayoutBinding bloomBindings[2]{};
    bloomBindings[0].binding         = 0;
    bloomBindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bloomBindings[0].descriptorCount = 1;
    bloomBindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    bloomBindings[1].binding         = 1;
    bloomBindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bloomBindings[1].descriptorCount = 1;
    bloomBindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dsl{};
    dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl.bindingCount = 2;
    dsl.pBindings    = bloomBindings;
    if (vkCreateDescriptorSetLayout(ctx.gpu.device(), &dsl, nullptr, &m_descLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create bloom descriptor set layout!");

    // One set per level and per direction (down and up), per frame in flight:
    // each one carries a different source/destination pair and they cannot be reused.
    const uint32_t bloomSets = kFramesInFlight * kMaxMips * 2;
    VkDescriptorPoolSize bloomSizes[2]{};
    bloomSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bloomSizes[0].descriptorCount = bloomSets;
    bloomSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bloomSizes[1].descriptorCount = bloomSets;

    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes    = bloomSizes;
    dpi.maxSets       = bloomSets;
    if (vkCreateDescriptorPool(ctx.gpu.device(), &dpi, nullptr, &m_descPool) != VK_SUCCESS)
        throw std::runtime_error("failed to create bloom descriptor pool!");

    VkPushConstantRange bloomPcr{};
    bloomPcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bloomPcr.offset     = 0;
    bloomPcr.size       = sizeof(BloomPush);

    VkPipelineLayoutCreateInfo pli{};
    pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount         = 1;
    pli.pSetLayouts            = &m_descLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges    = &bloomPcr;
    if (vkCreatePipelineLayout(ctx.gpu.device(), &pli, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create bloom pipeline layout!");

    auto makeBloomPipeline = [&](const std::string& spv, VkPipeline& pipeline)
    {
        auto module = loadShaderModule(ctx.gpu.device(), spv);

        VkComputePipelineCreateInfo ci{};
        ci.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName  = "main";
        ci.layout       = m_pipelineLayout;
        if (vkCreateComputePipelines(ctx.gpu.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS)
            throw std::runtime_error("failed to create compute pipeline: " + spv);

        vkDestroyShaderModule(ctx.gpu.device(), module, nullptr);
    };

    makeBloomPipeline("shaders/bloom_down.comp.spv", m_downPipeline);
    makeBloomPipeline("shaders/bloom_up.comp.spv",   m_upPipeline);

    // --- GPU cost measurement -----------------------------------------
    // The support and the period were resolved by the Renderer right before calling
    // here: they are device properties and are shared by all the passes.
    if (ctx.timestampsSupported)
    {
        VkQueryPoolCreateInfo qpi{};
        qpi.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = kFramesInFlight * 2;
        if (vkCreateQueryPool(ctx.gpu.device(), &qpi, nullptr, &m_queryPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create bloom query pool!");
    }
}

void BloomPass::destroyPipelines(const Context& ctx)
{
    vkDestroyPipeline(ctx.gpu.device(), m_downPipeline, nullptr);
    vkDestroyPipeline(ctx.gpu.device(), m_upPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.gpu.device(), m_pipelineLayout, nullptr);
    vkDestroyDescriptorPool(ctx.gpu.device(), m_descPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.gpu.device(), m_descLayout, nullptr);
    vkDestroySampler(ctx.gpu.device(), m_sampler, nullptr);
    if (m_queryPool != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(ctx.gpu.device(), m_queryPool, nullptr);
        m_queryPool = VK_NULL_HANDLE;
    }
}

void BloomPass::createImages(const Context& ctx)
{
    // Chain at half resolution: the bloom is a wide blur, resolving it at full
    // size adds nothing and costs 4x.
    uint32_t w = ctx.renderExtent.width  / 2;
    uint32_t h = ctx.renderExtent.height / 2;
    m_mipCount = 0;
    for (uint32_t m = 0; m < kMaxMips && w >= 2 && h >= 2; m++)
    {
        m_mipExtent[m] = { w, h };
        m_mipCount++;
        w = (w / 2 < 1) ? 1u : w / 2;
        h = (h / 2 < 1) ? 1u : h / 2;
    }
    // Tiny viewport (window almost closed): without levels there is no bloom to
    // compute. record() and the composition check it.
    if (m_mipCount == 0) return;

    for (int f = 0; f < kFramesInFlight; f++)
    {
        // Inline and not GpuResources::createImage: that one fixes mipLevels to 1.
        VkImageCreateInfo ci{};
        ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.format        = ctx.hdrFormat;
        ci.extent        = { m_mipExtent[0].width, m_mipExtent[0].height, 1 };
        ci.mipLevels     = m_mipCount;
        ci.arrayLayers   = 1;
        ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        // TRANSFER_DST: with the effect off the chain is cleared to black instead
        // of being computed, and the composition keeps sampling it.
        ci.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(ctx.gpu.device(), &ci, nullptr, &m_image[f]) != VK_SUCCESS)
            throw std::runtime_error("failed to create bloom image!");

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(ctx.gpu.device(), m_image[f], &memReq);
        VkMemoryAllocateInfo memAlloc{};
        memAlloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memAlloc.allocationSize  = memReq.size;
        memAlloc.memoryTypeIndex = ctx.gpu.findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(ctx.gpu.device(), &memAlloc, nullptr, &m_memory[f]) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate bloom memory!");
        vkBindImageMemory(ctx.gpu.device(), m_image[f], m_memory[f], 0);

        for (uint32_t m = 0; m < m_mipCount; m++)
        {
            VkImageViewCreateInfo vi{};
            vi.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image                           = m_image[f];
            vi.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
            vi.format                          = ctx.hdrFormat;
            vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            vi.subresourceRange.baseMipLevel   = m;
            vi.subresourceRange.levelCount     = 1;
            vi.subresourceRange.baseArrayLayer = 0;
            vi.subresourceRange.layerCount     = 1;
            if (vkCreateImageView(ctx.gpu.device(), &vi, nullptr, &m_mipView[f][m]) != VK_SUCCESS)
                throw std::runtime_error("failed to create bloom mip view!");
        }

        // Freshly created: undefined content and UNDEFINED layout. With the bloom
        // on, record() fixes it; off, the clear leaves it in
        // black and in GENERAL, which is what the composition set declares.
        m_clearPending[f] = true;
    }

    // The sets from the previous time point to already destroyed views: reset and not
    // free, as precomputeIbl does when reloading the environment.
    vkResetDescriptorPool(ctx.gpu.device(), m_descPool, 0);

    for (int f = 0; f < kFramesInFlight; f++)
    {
        const uint32_t setCount = m_mipCount * 2;
        std::vector<VkDescriptorSetLayout> layouts(setCount, m_descLayout);
        std::vector<VkDescriptorSet>       sets(setCount, VK_NULL_HANDLE);

        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = m_descPool;
        ai.descriptorSetCount = setCount;
        ai.pSetLayouts        = layouts.data();
        if (vkAllocateDescriptorSets(ctx.gpu.device(), &ai, sets.data()) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate bloom descriptor sets!");

        // The VkDescriptorImageInfo have to stay alive until
        // vkUpdateDescriptorSets, so they are sized all at once.
        std::vector<VkDescriptorImageInfo> srcInfos(setCount);
        std::vector<VkDescriptorImageInfo> dstInfos(setCount);
        std::vector<VkWriteDescriptorSet>  writes;
        writes.reserve(setCount * 2);

        auto pushPair = [&](uint32_t slot, VkDescriptorSet set,
                            VkImageView srcView, VkImageLayout srcLayout, VkImageView dstView)
        {
            srcInfos[slot].imageLayout = srcLayout;
            srcInfos[slot].imageView   = srcView;
            srcInfos[slot].sampler     = m_sampler;
            dstInfos[slot].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            dstInfos[slot].imageView   = dstView;

            VkWriteDescriptorSet src{};
            src.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            src.dstSet          = set;
            src.dstBinding      = 0;
            src.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            src.descriptorCount = 1;
            src.pImageInfo      = &srcInfos[slot];
            writes.push_back(src);

            VkWriteDescriptorSet dst = src;
            dst.dstBinding     = 1;
            dst.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            dst.pImageInfo     = &dstInfos[slot];
            writes.push_back(dst);
        };

        for (uint32_t m = 0; m < m_mipCount; m++)
        {
            // Down: level 0 reads the HDR scene (which comes out of the render pass
            // in SHADER_READ_ONLY); the others read the previous mip, which lives
            // in GENERAL for the whole chain.
            m_downSets[f][m] = sets[m];
            pushPair(m, sets[m],
                     m == 0 ? ctx.hdrView[f] : m_mipView[f][m - 1],
                     m == 0 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
                     m_mipView[f][m]);

            // Up: reads level m and accumulates onto m-1. Level 0 has no
            // destination, so its set is left unused.
            m_upSets[f][m] = VK_NULL_HANDLE;
            if (m > 0)
            {
                const uint32_t slot = m_mipCount + m;
                m_upSets[f][m] = sets[slot];
                pushPair(slot, sets[slot],
                         m_mipView[f][m], VK_IMAGE_LAYOUT_GENERAL,
                         m_mipView[f][m - 1]);
            }
        }
        vkUpdateDescriptorSets(ctx.gpu.device(), (uint32_t)writes.size(), writes.data(), 0, nullptr);
    }
}

void BloomPass::destroyImages(const Context& ctx)
{
    for (int f = 0; f < kFramesInFlight; f++)
    {
        for (uint32_t m = 0; m < kMaxMips; m++)
        {
            if (m_mipView[f][m])
            {
                vkDestroyImageView(ctx.gpu.device(), m_mipView[f][m], nullptr);
                m_mipView[f][m] = VK_NULL_HANDLE;
            }
            m_downSets[f][m] = VK_NULL_HANDLE;
            m_upSets[f][m]   = VK_NULL_HANDLE;
        }
        if (m_image[f])
        {
            vkDestroyImage(ctx.gpu.device(), m_image[f], nullptr);
            m_image[f] = VK_NULL_HANDLE;
        }
        if (m_memory[f])
        {
            vkFreeMemory(ctx.gpu.device(), m_memory[f], nullptr);
            m_memory[f] = VK_NULL_HANDLE;
        }
    }
    m_mipCount = 0;
}

void BloomPass::beginQuery(const Context& ctx, VkCommandBuffer cmd)
{
    // Reading the timestamps from two frames ago in this same slot: the
    // fence of currentFrame was already awaited by drawFrame, so the
    // results are available without blocking anyone.
    if (ctx.timestampsSupported && m_queryPending[ctx.currentFrame])
    {
        uint64_t stamps[2] = {};
        if (vkGetQueryPoolResults(ctx.gpu.device(), m_queryPool, ctx.currentFrame * 2, 2,
                                  sizeof(stamps), stamps, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        {
            m_gpuMs = (float)((double)(stamps[1] - stamps[0]) * ctx.timestampPeriod * 1e-6);
            if (++m_measuredFrames == 300)
            {
                printf("bloom+composite: %.3f ms (%ux%u, %u mips)\n",
                       m_gpuMs, ctx.swapChainExtent.width, ctx.swapChainExtent.height,
                       m_mipCount);
                fflush(stdout);
            }
        }
    }

    if (ctx.timestampsSupported)
    {
        vkCmdResetQueryPool(cmd, m_queryPool, ctx.currentFrame * 2, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_queryPool, ctx.currentFrame * 2);
        m_queryPending[ctx.currentFrame] = true;
    }
}

void BloomPass::skipQuery(const Context& ctx)
{
    m_gpuMs = 0.0f;
    m_queryPending[ctx.currentFrame] = false;
}

void BloomPass::recordClear(const Context& ctx, VkCommandBuffer cmd)
{
    if (m_mipCount == 0 || !m_clearPending[ctx.currentFrame]) return;

    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = m_image[ctx.currentFrame];
    b.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel   = 0;
    // The whole chain, not just mip 0: that way the levels are left in GENERAL at
    // once and when the bloom is turned back on there are no half-done layouts.
    b.subresourceRange.levelCount     = m_mipCount;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount     = 1;

    b.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);

    VkClearColorValue black{};
    vkCmdClearColorImage(cmd, m_image[ctx.currentFrame], VK_IMAGE_LAYOUT_GENERAL,
                         &black, 1, &b.subresourceRange);

    b.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);

    m_clearPending[ctx.currentFrame] = false;
}

void BloomPass::record(const Context& ctx, VkCommandBuffer cmd)
{
    if (m_mipCount == 0) return;

    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = m_image[ctx.currentFrame];
    b.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount     = 1;

    // The whole chain lives in GENERAL: it is the only layout that supports
    // imageStore and is at the same time valid for sampling, so the layout
    // ping-pong between passes is reduced to memory barriers. It is entered from
    // UNDEFINED because the previous frame's content is not reused.
    b.oldLayout                     = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout                     = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask                 = 0;
    b.dstAccessMask                 = VK_ACCESS_SHADER_WRITE_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount   = m_mipCount;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);

    // Barrier between passes: what a dispatch has just written is read by the
    // next one. It is bounded to the mip involved so as not to serialize more than needed.
    auto writeToRead = [&](uint32_t mip)
    {
        b.oldLayout                     = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout                     = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask                 = VK_ACCESS_SHADER_WRITE_BIT;
        // SHADER_WRITE in addition to READ: the up pass does imageLoad AND imageStore
        // on the same level that the down pass wrote, so without this
        // there would be an unordered write-after-write.
        b.dstAccessMask                 = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        b.subresourceRange.baseMipLevel = mip;
        b.subresourceRange.levelCount   = 1;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
    };

    BloomPush push{};
    push.threshold = ctx.state.bloomThreshold();
    push.knee      = glm::max(ctx.state.bloomKnee(), 1e-3f);
    push.radius    = 1.0f;

    // ── Down: HDR → mip 0 (with threshold) → mip 1 → ... ───────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_downPipeline);
    for (uint32_t m = 0; m < m_mipCount; m++)
    {
        const VkExtent2D src = (m == 0) ? ctx.renderExtent : m_mipExtent[m - 1];
        const VkExtent2D dst = m_mipExtent[m];

        push.srcTexelX = 1.0f / (float)src.width;
        push.srcTexelY = 1.0f / (float)src.height;
        push.prefilter = (m == 0) ? 1 : 0;

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                                0, 1, &m_downSets[ctx.currentFrame][m], 0, nullptr);
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (dst.width + 7) / 8, (dst.height + 7) / 8, 1);

        if (m + 1 < m_mipCount) writeToRead(m);
    }

    // ── Up: each mip is added to the one above with a 3x3 tent ────────────
    push.prefilter = 0;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_upPipeline);
    for (uint32_t m = m_mipCount - 1; m > 0; m--)
    {
        // Mip m has just been written (by the down pass if it is the last one, by the
        // previous iteration of this up pass if not) and is now read.
        writeToRead(m);

        const VkExtent2D src = m_mipExtent[m];
        const VkExtent2D dst = m_mipExtent[m - 1];
        push.srcTexelX = 1.0f / (float)src.width;
        push.srcTexelY = 1.0f / (float)src.height;

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                                0, 1, &m_upSets[ctx.currentFrame][m], 0, nullptr);
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (dst.width + 7) / 8, (dst.height + 7) / 8, 1);
    }

    // Mip 0 becomes readable from the composition's fragment shader.
    b.oldLayout                     = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout                     = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask                 = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask                 = VK_ACCESS_SHADER_READ_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount   = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
}

} // namespace DonTopo
