#include "DonTopo/Renderer/Passes/SsrPass.h"
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

// Shared by ssr.comp and ssr_resolve.comp, which share a pipeline
// layout. 48 bytes: the same fields and in the same order as the
// block of the two .comp files.
struct SsrPush {
    float   projP00;
    float   projP11;
    float   projP22;
    float   projP32;
    float   invResX;
    float   invResY;
    float   maxDistance;
    float   thickness;
    int32_t maxSteps;
    int32_t refineSteps;
    float   edgeFade;
    float   intensity;
};
static_assert(sizeof(SsrPush) == 48, "SsrPush must stay at 48 bytes: both .comp shaders declare this layout");

// ── SSR ─────────────────────────────────────────────────────────────────────
bool SsrPass::active(const Context& ctx) const
{
    if (!ctx.state.ssrEnabled()) return false;
    // Resources not created yet (degenerate viewport): nothing to record.
    if (m_image[ctx.currentFrame] == VK_NULL_HANDLE) return false;
    // Switch on but no object marked = no pixel with a
    // mask: the whole pass is skipped instead of dispatching and multiplying by
    // zero. It is one float per object, much less than the culling that is already done
    // in this same frame.
    return ctx.anyObjectWithSsr;
}

void SsrPass::createPipelines(const Context& ctx)
{
    // LINEAR: the ray hit falls between texels and R16G16B16A16_SFLOAT does
    // guarantee linear filtering. The depth is NOT sampled with
    // this sampler but with SSAO's (NEAREST), which is the one that
    // corresponds to D32_SFLOAT. CLAMP_TO_EDGE so that an edge tap does not
    // bring in color from the opposite side of the screen.
    VkSamplerCreateInfo si{};
    si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.borderColor  = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    if (vkCreateSampler(ctx.gpu.device(), &si, nullptr, &m_sampler) != VK_SUCCESS)
        throw std::runtime_error("failed to create ssr sampler!");

    // A single layout for the two pipelines: sampled color, sampled depth
    // and destination as a storage image. ssr_resolve.comp declares
    // binding 1 and does not read it.
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (int i = 0; i < 3; i++)
    {
        bindings[i].binding         = (uint32_t)i;
        bindings[i].descriptorType  = (i < 2) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                              : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo dsl{};
    dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl.bindingCount = 3;
    dsl.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(ctx.gpu.device(), &dsl, nullptr, &m_descLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create ssr descriptor set layout!");

    // Two sets per frame: march (HDR + depth → reflection) and sum (reflection →
    // HDR).
    const uint32_t ssrSets = kFramesInFlight * 2;
    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[0].descriptorCount = ssrSets * 2;
    sizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[1].descriptorCount = ssrSets;

    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes    = sizes;
    dpi.maxSets       = ssrSets;
    if (vkCreateDescriptorPool(ctx.gpu.device(), &dpi, nullptr, &m_descPool) != VK_SUCCESS)
        throw std::runtime_error("failed to create ssr descriptor pool!");

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset     = 0;
    pcr.size       = sizeof(SsrPush);

    VkPipelineLayoutCreateInfo pli{};
    pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount         = 1;
    pli.pSetLayouts            = &m_descLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(ctx.gpu.device(), &pli, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create ssr pipeline layout!");

    auto makeSsrPipeline = [&](const std::string& spv, VkPipeline& pipeline)
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

    makeSsrPipeline("shaders/ssr.comp.spv",         m_pipeline);
    makeSsrPipeline("shaders/ssr_resolve.comp.spv", m_resolvePipeline);

    // Four per frame: [0,1] the depth pre-pass when SSR requests it, [2,3]
    // the two dispatches. timestampsSupported was already resolved by the bloom.
    if (ctx.timestampsSupported)
    {
        VkQueryPoolCreateInfo qpi{};
        qpi.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = kFramesInFlight * 4;
        if (vkCreateQueryPool(ctx.gpu.device(), &qpi, nullptr, &m_queryPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create ssr query pool!");
    }

    printf("ssr pipelines OK\n"); fflush(stdout);
}

void SsrPass::destroyPipelines(const Context& ctx)
{
    // The images and the sets are already gone with destroyImages; here only
    // what is independent of the size remains.
    vkDestroyPipeline(ctx.gpu.device(), m_pipeline, nullptr);
    vkDestroyPipeline(ctx.gpu.device(), m_resolvePipeline, nullptr);
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

void SsrPass::createImages(const Context& ctx)
{
    for (int f = 0; f < kFramesInFlight; f++)
    {
        // Same format as the HDR: ssr_resolve.comp declares both with the
        // rgba16f qualifier. Full resolution, like SSAO.
        ctx.res.createImage(
            ctx.renderExtent.width, ctx.renderExtent.height,
            ctx.hdrFormat, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            m_image[f], m_memory[f]);
        ctx.res.createTextureImageView(m_image[f], m_view[f], ctx.hdrFormat);
    }

    // The sets from the previous time point to already destroyed views: reset and not
    // free, as in the bloom and the SSAO.
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
            throw std::runtime_error("failed to allocate ssr descriptor sets!");

        m_sets[f]        = sets[0];
        m_resolveSets[f] = sets[1];

        VkDescriptorImageInfo infos[6]{};
        // March: scene color (comes out of the render pass in SHADER_READ_ONLY)
        // + pre-pass depth → reflection.
        infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[0].imageView   = ctx.hdrView[f];
        infos[0].sampler     = m_sampler;
        infos[1].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        infos[1].imageView   = ctx.ssaoDepthView[f];
        infos[1].sampler     = ctx.ssaoSampler;
        infos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        infos[2].imageView   = m_view[f];
        // Sum: the reflection (already in GENERAL) → the HDR as storage. Binding 1
        // is filled with the same depth even though the shader does not read it: a
        // descriptor set cannot be left with an unwritten binding.
        infos[3].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        infos[3].imageView   = m_view[f];
        infos[3].sampler     = m_sampler;
        infos[4]             = infos[1];
        infos[5].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        infos[5].imageView   = ctx.hdrView[f];

        VkWriteDescriptorSet writes[6]{};
        for (int i = 0; i < 6; i++)
        {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = (i < 3) ? m_sets[f] : m_resolveSets[f];
            writes[i].dstBinding      = (uint32_t)(i % 3);
            writes[i].descriptorType  = (i % 3 == 2) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                     : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].descriptorCount = 1;
            writes[i].pImageInfo      = &infos[i];
        }
        vkUpdateDescriptorSets(ctx.gpu.device(), 6, writes, 0, nullptr);
    }
}

void SsrPass::destroyImages(const Context& ctx)
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
        m_sets[f]        = VK_NULL_HANDLE;
        m_resolveSets[f] = VK_NULL_HANDLE;
    }
}

void SsrPass::record(const Context& ctx, VkCommandBuffer cmd, const glm::mat4& proj)
{
    if (!active(ctx))
    {
        m_gpuMs = 0.0f;
        // Neither dispatches nor barriers: the HDR stays exactly as the
        // scene pass left it, in SHADER_READ_ONLY, which is precisely what the
        // bloom and the composition expect. Image identical to the one before SSR.
        return;
    }

    // Timestamps from two frames ago in this same slot, already signaled.
    if (ctx.timestampsSupported && m_queryPending[ctx.currentFrame])
    {
        uint64_t stamps[4] = {};
        if (vkGetQueryPoolResults(ctx.gpu.device(), m_queryPool, ctx.currentFrame * 4, 4,
                                  sizeof(stamps), stamps, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        {
            // The pre-pass only counts as SSR cost when SSR is the one
            // requesting it: with SSAO on it already shows up in ssaoGpuMs and
            // adding it here would count it twice.
            const uint64_t prepass = ctx.state.ssaoEnabled() ? 0 : (stamps[1] - stamps[0]);
            m_gpuMs = (float)((double)(prepass + (stamps[3] - stamps[2]))
                              * ctx.timestampPeriod * 1e-6);
            if (++m_measuredFrames == 300)
            {
                printf("ssr (march + sum%s): %.3f ms (%ux%u, %d steps)\n",
                       ctx.state.ssaoEnabled() ? "" : " + depth pre-pass",
                       m_gpuMs, ctx.swapChainExtent.width, ctx.swapChainExtent.height,
                       ctx.state.ssrMaxSteps());
                fflush(stdout);
            }
        }
    }
    // The frame is only accepted when recordSsaoPass left the
    // pair [0,1] written: without that, reading all four would give NOT_READY.
    if (ctx.timestampsSupported && ctx.stampedPrepass)
    {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_queryPool, ctx.currentFrame * 4 + 2);
        m_queryPending[ctx.currentFrame] = true;
    }
    else
    {
        m_queryPending[ctx.currentFrame] = false;
    }

    SsrPush push{};
    // The same four coefficients SSAO uses, from the projection that
    // recorded the depth. The sign of p11 cancels out just as there: see ssao.comp.
    push.projP00     = proj[0][0];
    push.projP11     = proj[1][1];
    push.projP22     = proj[2][2];
    push.projP32     = proj[3][2];
    push.invResX     = 1.0f / (float)ctx.renderExtent.width;
    push.invResY     = 1.0f / (float)ctx.renderExtent.height;
    push.maxDistance = ctx.state.ssrMaxDistance();
    push.thickness   = ctx.state.ssrThickness();
    push.maxSteps    = (int32_t)ctx.state.ssrMaxSteps();
    // Fixed and not configurable: four bisections already place the hit within
    // 1/16 of a step, and raising it changes nothing visible.
    push.refineSteps = 4;
    push.edgeFade    = ctx.state.ssrEdgeFade();
    push.intensity   = ctx.state.ssrIntensity();

    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel   = 0;
    b.subresourceRange.levelCount     = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount     = 1;

    // The reflection enters from UNDEFINED: it is rewritten entirely (ssr.comp starts
    // by setting the pixel to 0) and the previous frame's content is not
    // reused.
    b.image         = m_image[ctx.currentFrame];
    b.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);

    const uint32_t gx = (ctx.renderExtent.width  + 7) / 8;
    const uint32_t gy = (ctx.renderExtent.height + 7) / 8;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                            0, 1, &m_sets[ctx.currentFrame], 0, nullptr);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, gx, gy, 1);

    // Two transitions before the sum: the reflection that was just written
    // becomes readable, and the HDR goes from SHADER_READ_ONLY (where the
    // render pass left it, and from where the march just read it) to GENERAL, which is
    // the only valid layout for imageLoad/imageStore.
    VkImageMemoryBarrier toResolve[2] = { b, b };
    toResolve[0].image         = m_image[ctx.currentFrame];
    toResolve[0].oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    toResolve[0].newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    toResolve[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toResolve[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toResolve[1].image         = ctx.hdrImage[ctx.currentFrame];
    toResolve[1].oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toResolve[1].newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    toResolve[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toResolve[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, toResolve);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_resolvePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                            0, 1, &m_resolveSets[ctx.currentFrame], 0, nullptr);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, gx, gy, 1);

    // And the HDR goes back to SHADER_READ_ONLY, which is the layout declared by the
    // descriptor sets of the bloom (compute) and the composition (fragment).
    b.image         = ctx.hdrImage[ctx.currentFrame];
    b.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);

    if (ctx.timestampsSupported && m_queryPending[ctx.currentFrame])
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool, ctx.currentFrame * 4 + 3);
}

} // namespace DonTopo
