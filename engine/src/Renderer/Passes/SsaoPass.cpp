#include "DonTopo/Renderer/Passes/SsaoPass.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/GpuResources.h"
#include "DonTopo/Renderer/RendererState.h"
#include <stdexcept>
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include "DonTopo/Renderer/ShaderModule.h"

namespace DonTopo {

// ── helpers ──────────────────────────────────────────────────────────────────

// Shared by ssao.comp and ssao_blur.comp, which share a pipeline
// layout (the blur only reads invRes).
struct SsaoPush {
    float projP00;
    float projP11;
    float projP22;
    float projP32;
    float invResX;
    float invResY;
    float radius;
    float bias;
    float intensity;
    float power;
};

// ── SSAO ────────────────────────────────────────────────────────────────────
void SsaoPass::markClearPending()
{
    for (int i = 0; i < kFramesInFlight; i++) m_clearPending[i] = true;
}

void SsaoPass::createPipelines(const Context& ctx)
{
    // --- Compute: sampled source + destination as a storage image ---------
    VkDescriptorSetLayoutBinding ssaoBindings[2]{};
    ssaoBindings[0].binding         = 0;
    ssaoBindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ssaoBindings[0].descriptorCount = 1;
    ssaoBindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    ssaoBindings[1].binding         = 1;
    ssaoBindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    ssaoBindings[1].descriptorCount = 1;
    ssaoBindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dsl{};
    dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl.bindingCount = 2;
    dsl.pBindings    = ssaoBindings;
    if (vkCreateDescriptorSetLayout(ctx.gpu.device(), &dsl, nullptr, &m_descLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create ssao descriptor set layout!");

    // Two sets per frame: occlusion (depth → AO) and blur (AO → smoothed AO).
    const uint32_t ssaoSets = kFramesInFlight * 2;
    VkDescriptorPoolSize ssaoSizes[2]{};
    ssaoSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ssaoSizes[0].descriptorCount = ssaoSets;
    ssaoSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    ssaoSizes[1].descriptorCount = ssaoSets;

    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes    = ssaoSizes;
    dpi.maxSets       = ssaoSets;
    if (vkCreateDescriptorPool(ctx.gpu.device(), &dpi, nullptr, &m_descPool) != VK_SUCCESS)
        throw std::runtime_error("failed to create ssao descriptor pool!");

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset     = 0;
    pcr.size       = sizeof(SsaoPush);

    VkPipelineLayoutCreateInfo pli{};
    pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount         = 1;
    pli.pSetLayouts            = &m_descLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(ctx.gpu.device(), &pli, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create ssao pipeline layout!");

    auto makeSsaoPipeline = [&](const std::string& spv, VkPipeline& pipeline)
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

    makeSsaoPipeline("shaders/ssao.comp.spv",      m_pipeline);
    makeSsaoPipeline("shaders/ssao_blur.comp.spv", m_blurPipeline);

    // Own queries: timestampsSupported and timestampPeriod were already
    // resolved by createBloomPipelines, which runs earlier.
    if (ctx.timestampsSupported)
    {
        VkQueryPoolCreateInfo qpi{};
        qpi.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = kFramesInFlight * 2;
        if (vkCreateQueryPool(ctx.gpu.device(), &qpi, nullptr, &m_queryPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create ssao query pool!");
    }

    printf("ssao pipelines OK\n"); fflush(stdout);
}

void SsaoPass::destroyPipelines(const Context& ctx)
{
    vkDestroyPipeline(ctx.gpu.device(), m_pipeline, nullptr);
    vkDestroyPipeline(ctx.gpu.device(), m_blurPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.gpu.device(), m_pipelineLayout, nullptr);
    vkDestroyDescriptorPool(ctx.gpu.device(), m_descPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.gpu.device(), m_descLayout, nullptr);
    if (m_queryPool != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(ctx.gpu.device(), m_queryPool, nullptr);
        m_queryPool = VK_NULL_HANDLE;
    }
}

void SsaoPass::createImages(const Context& ctx)
{
    for (int f = 0; f < kFramesInFlight; f++)
    {
        // Raw AO and blurred AO. TRANSFER_DST on the second one: with the
        // effect off it is cleared to 1.0 instead of being computed.
        ctx.res.createImage(
            ctx.renderExtent.width, ctx.renderExtent.height,
            kSsaoFormat, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            m_image[f], m_memory[f]);
        ctx.res.createTextureImageView(m_image[f], m_view[f], kSsaoFormat);

        ctx.res.createImage(
            ctx.renderExtent.width, ctx.renderExtent.height,
            kSsaoFormat, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            m_blurImage[f], m_blurMemory[f]);
        ctx.res.createTextureImageView(m_blurImage[f], m_blurView[f], kSsaoFormat);

        // Freshly created: undefined content and UNDEFINED layout. The clear leaves it
        // at 1.0 and in GENERAL, which is what binding 7 declares.
        m_clearPending[f] = true;
    }

    // The sets from the previous time point to already destroyed views: reset and not
    // free, as in the bloom.
    vkResetDescriptorPool(ctx.gpu.device(), m_descPool, 0);

    for (int f = 0; f < kFramesInFlight; f++)
    {
        VkDescriptorSetLayout layouts[2] = { m_descLayout, m_descLayout };
        VkDescriptorSet       sets[2]    = {};

        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = m_descPool;
        ai.descriptorSetCount = 2;
        ai.pSetLayouts        = layouts;
        if (vkAllocateDescriptorSets(ctx.gpu.device(), &ai, sets) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate ssao descriptor sets!");

        m_sets[f]     = sets[0];
        m_blurSets[f] = sets[1];

        VkDescriptorImageInfo infos[4]{};
        // Occlusion: reads the pre-pass depth, writes the raw AO.
        infos[0].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        infos[0].imageView   = ctx.depthView[f];
        infos[0].sampler     = ctx.depthSampler;
        infos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        infos[1].imageView   = m_view[f];
        // Blur: reads the raw AO, writes the one that pbr.frag consumes.
        infos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        infos[2].imageView   = m_view[f];
        infos[2].sampler     = ctx.depthSampler;
        infos[3].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        infos[3].imageView   = m_blurView[f];

        VkWriteDescriptorSet writes[4]{};
        for (int i = 0; i < 4; i++)
        {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = (i < 2) ? m_sets[f] : m_blurSets[f];
            writes[i].dstBinding      = (uint32_t)(i % 2);
            writes[i].descriptorType  = (i % 2 == 0) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                     : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].descriptorCount = 1;
            writes[i].pImageInfo      = &infos[i];
        }
        vkUpdateDescriptorSets(ctx.gpu.device(), 4, writes, 0, nullptr);
    }
}

void SsaoPass::destroyImages(const Context& ctx)
{
    for (int f = 0; f < kFramesInFlight; f++)
    {
        if (m_view[f])
        {
            vkDestroyImageView(ctx.gpu.device(), m_view[f], nullptr);
            m_view[f] = VK_NULL_HANDLE;
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
        if (m_blurView[f])
        {
            vkDestroyImageView(ctx.gpu.device(), m_blurView[f], nullptr);
            m_blurView[f] = VK_NULL_HANDLE;
        }
        if (m_blurImage[f])
        {
            vkDestroyImage(ctx.gpu.device(), m_blurImage[f], nullptr);
            m_blurImage[f] = VK_NULL_HANDLE;
        }
        if (m_blurMemory[f])
        {
            vkFreeMemory(ctx.gpu.device(), m_blurMemory[f], nullptr);
            m_blurMemory[f] = VK_NULL_HANDLE;
        }
        m_sets[f]     = VK_NULL_HANDLE;
        m_blurSets[f] = VK_NULL_HANDLE;
    }
}

void SsaoPass::recordPreDepth(const Context& ctx, VkCommandBuffer cmd)
{
    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel   = 0;
    b.subresourceRange.levelCount     = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount     = 1;

    if (!ctx.state.ssaoEnabled())
    {
        m_gpuMs = 0.0f;
        // Off: neither occlusion nor blur. All that is left is to put the map at
        // identity, and that happens ONCE per image (when creating it and when turning the
        // effect off), not every frame.
        if (m_clearPending[ctx.currentFrame])
        {
            b.image         = m_blurImage[ctx.currentFrame];
            b.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);

            VkClearColorValue white{};
            white.float32[0] = 1.0f;
            vkCmdClearColorImage(cmd, m_blurImage[ctx.currentFrame], VK_IMAGE_LAYOUT_GENERAL,
                                 &white, 1, &b.subresourceRange);

            b.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);

            m_clearPending[ctx.currentFrame] = false;
        }
        return;
    }

    // Slot's timestamps: the ones from two frames ago are read, whose fence was already
    // awaited by drawFrame, so they do not block anyone.
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
                printf("ssao (depth pre-pass + 2 dispatches): %.3f ms (%ux%u)\n",
                       m_gpuMs, ctx.swapChainExtent.width, ctx.swapChainExtent.height);
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

void SsaoPass::record(const Context& ctx, VkCommandBuffer cmd, const glm::mat4& proj)
{
    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel   = 0;
    b.subresourceRange.levelCount     = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount     = 1;

    // ── Occlusion + blur ──────────────────────────────────────────────────
    SsaoPush push{};
    // The four coefficients of the frame's projection: the same one the
    // depth was just recorded with, so reconstructing and reprojecting is
    // consistent.
    //
    // Here it goes with Vulkan's Y-flip inside and the DirectX 12 backend sends the
    // opposite sign, and the two images come out equal: the pass does not leave
    // screen space and the sign cancels out. It is explained in ssao.comp.
    push.projP00   = proj[0][0];
    push.projP11   = proj[1][1];
    push.projP22   = proj[2][2];
    push.projP32   = proj[3][2];
    push.invResX   = 1.0f / (float)ctx.renderExtent.width;
    push.invResY   = 1.0f / (float)ctx.renderExtent.height;
    push.radius    = ctx.state.ssaoRadius();
    push.bias      = ctx.state.ssaoBias();
    push.intensity = ctx.state.ssaoIntensity();
    push.power     = ctx.state.ssaoPower();

    // The two images enter from UNDEFINED: they are rewritten entirely and the
    // previous frame's content is not reused. GENERAL for both,
    // which is the only layout valid at once for imageStore and for
    // sampling, as in the bloom chain.
    VkImageMemoryBarrier toGeneral[2] = { b, b };
    toGeneral[0].image         = m_image[ctx.currentFrame];
    toGeneral[1].image         = m_blurImage[ctx.currentFrame];
    for (int i = 0; i < 2; i++)
    {
        toGeneral[i].oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        toGeneral[i].newLayout     = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral[i].srcAccessMask = 0;
        toGeneral[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, toGeneral);

    const uint32_t gx = (ctx.renderExtent.width  + 7) / 8;
    const uint32_t gy = (ctx.renderExtent.height + 7) / 8;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                            0, 1, &m_sets[ctx.currentFrame], 0, nullptr);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, gx, gy, 1);

    // What the occlusion has just written is read by the blur.
    b.image         = m_image[ctx.currentFrame];
    b.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_blurPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                            0, 1, &m_blurSets[ctx.currentFrame], 0, nullptr);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, gx, gy, 1);

    // And the result is read by pbr.frag in the scene pass.
    b.image         = m_blurImage[ctx.currentFrame];
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);

    if (ctx.timestampsSupported)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool, ctx.currentFrame * 2 + 1);
}

} // namespace DonTopo
