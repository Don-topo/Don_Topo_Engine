#include "DonTopo/Renderer/Passes/FogPass.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/RendererState.h"
#include <stdexcept>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstdio>
#include "DonTopo/Renderer/ShaderModule.h"

namespace DonTopo {

// ── helpers ──────────────────────────────────────────────────────────────────

// Exactly 128 bytes (the minimum Vulkan guarantees): the same
// fields and in the same order as the fog.comp block.
struct FogPush {
    glm::mat4 invViewProj;
    glm::vec4 camPosDensity;
    glm::vec4 lightDirFalloff;
    glm::vec4 scatterBaseHeight;
    glm::vec4 gStepsRes;
};
static_assert(sizeof(FogPush) == 128, "FogPush must stay at 128 bytes: fog.comp declares this layout");

// ── Volumetric fog ──────────────────────────────────────────────────────────
void FogPass::createPipelines(const Context& ctx)
{
    // Four bindings: HDR as storage (read + write in place), the
    // pre-pass depth, the frame's UBO (view matrix, cuts and
    // cascade matrices) and the key light's shadow map.
    VkDescriptorSetLayoutBinding bindings[4]{};
    const VkDescriptorType types[4] = {
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    };
    for (int i = 0; i < 4; i++)
    {
        bindings[i].binding         = (uint32_t)i;
        bindings[i].descriptorType  = types[i];
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo dsl{};
    dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl.bindingCount = 4;
    dsl.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(ctx.gpu.device(), &dsl, nullptr, &m_descLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create fog descriptor set layout!");

    VkDescriptorPoolSize sizes[3]{};
    sizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[0].descriptorCount = kFramesInFlight;
    sizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[1].descriptorCount = kFramesInFlight * 2;
    sizes[2].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[2].descriptorCount = kFramesInFlight;

    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes    = sizes;
    dpi.maxSets       = kFramesInFlight;
    if (vkCreateDescriptorPool(ctx.gpu.device(), &dpi, nullptr, &m_descPool) != VK_SUCCESS)
        throw std::runtime_error("failed to create fog descriptor pool!");

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset     = 0;
    pcr.size       = sizeof(FogPush);

    VkPipelineLayoutCreateInfo pli{};
    pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount         = 1;
    pli.pSetLayouts            = &m_descLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(ctx.gpu.device(), &pli, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create fog pipeline layout!");

    auto module = loadShaderModule(ctx.gpu.device(), "shaders/fog.comp.spv");

    VkComputePipelineCreateInfo ci{};
    ci.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName  = "main";
    ci.layout       = m_pipelineLayout;
    if (vkCreateComputePipelines(ctx.gpu.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &m_pipeline) != VK_SUCCESS)
        throw std::runtime_error("failed to create compute pipeline: shaders/fog.comp.spv");

    vkDestroyShaderModule(ctx.gpu.device(), module, nullptr);

    // Two per frame, the ones that bound the dispatch. timestampsSupported was already
    // resolved by the bloom.
    if (ctx.timestampsSupported)
    {
        VkQueryPoolCreateInfo qpi{};
        qpi.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = kFramesInFlight * 2;
        if (vkCreateQueryPool(ctx.gpu.device(), &qpi, nullptr, &m_queryPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create fog query pool!");
    }

    printf("fog pipeline OK\n"); fflush(stdout);
}

void FogPass::destroyPipelines(const Context& ctx)
{
    vkDestroyPipeline(ctx.gpu.device(), m_pipeline, nullptr);
    vkDestroyPipelineLayout(ctx.gpu.device(), m_pipelineLayout, nullptr);
    vkDestroyDescriptorPool(ctx.gpu.device(), m_descPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.gpu.device(), m_descLayout, nullptr);
    if (m_queryPool != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(ctx.gpu.device(), m_queryPool, nullptr);
        m_queryPool = VK_NULL_HANDLE;
    }
}

void FogPass::createSets(const Context& ctx)
{
    // The frame's UBO is one of the four bindings and on the first init
    // it does not exist yet when createOffscreenImages runs: it exits here without
    // doing anything and the end of init calls it again.
    if (ctx.uniformBuffers[0] == VK_NULL_HANDLE) return;

    // The fog has no image of its own: it writes inside the HDR. The only thing that
    // has to be rebuilt with the swapchain is the sets, which reference
    // hdrView and ssaoDepthView. Reset and not free, as in SSR.
    vkResetDescriptorPool(ctx.gpu.device(), m_descPool, 0);

    for (int f = 0; f < kFramesInFlight; f++)
    {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = m_descPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &m_descLayout;
        if (vkAllocateDescriptorSets(ctx.gpu.device(), &ai, &m_sets[f]) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate fog descriptor sets!");

        VkDescriptorImageInfo hdrInfo{};
        hdrInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        hdrInfo.imageView   = ctx.hdrView[f];

        VkDescriptorImageInfo depthInfo{};
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        depthInfo.imageView   = ctx.ssaoDepthView[f];
        depthInfo.sampler     = ctx.ssaoSampler;

        VkDescriptorBufferInfo uboInfo{};
        uboInfo.buffer = ctx.uniformBuffers[f];
        uboInfo.offset = 0;
        uboInfo.range  = sizeof(UniformBufferObject);

        // The same view+sampler pair that pbr.frag samples: depth comparator
        // included, which is what sampler2DArrayShadow expects.
        VkDescriptorImageInfo shadowInfo{};
        shadowInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        shadowInfo.imageView   = ctx.shadowView;
        shadowInfo.sampler     = ctx.shadowSampler;

        VkWriteDescriptorSet writes[4]{};
        for (int i = 0; i < 4; i++)
        {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = m_sets[f];
            writes[i].dstBinding      = (uint32_t)i;
            writes[i].descriptorCount = 1;
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo     = &hdrInfo;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo     = &depthInfo;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[2].pBufferInfo    = &uboInfo;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[3].pImageInfo     = &shadowInfo;

        vkUpdateDescriptorSets(ctx.gpu.device(), 4, writes, 0, nullptr);
    }
}

void FogPass::record(const Context& ctx, VkCommandBuffer cmd, const glm::mat4& view, const glm::mat4& proj)
{
    // Off (or sets not yet allocated, degenerate viewport): no dispatch, no
    // barriers, no timestamps. The HDR stays exactly as the scene pass
    // and the SSR left it, in SHADER_READ_ONLY, which is precisely what the
    // bloom and the composition expect. Image identical to the one before the feature.
    if (!ctx.state.fogEnabled() || m_sets[ctx.currentFrame] == VK_NULL_HANDLE)
    {
        m_gpuMs = 0.0f;
        m_queryPending[ctx.currentFrame] = false;
        return;
    }

    // Timestamps from two frames ago in this same slot, whose fence was already awaited by
    // drawFrame, so they do not block anyone.
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
                printf("fog (ray-marching): %.3f ms (%ux%u, %d steps)\n",
                       m_gpuMs, ctx.renderExtent.width, ctx.renderExtent.height, ctx.state.fogSteps());
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

    FogPush push{};
    // The frame's EFFECTIVE projection (with Vulkan's Y-flip) times the
    // view: it is the one the depth was recorded with, so unprojecting is consistent.
    push.invViewProj = glm::inverse(proj * view);
    // The camera in world comes from the view itself: the fourth column of its
    // inverse. That way there is no need to drag one more parameter down to here.
    const glm::vec3 camPos = glm::vec3(glm::inverse(view)[3]);
    push.camPosDensity = glm::vec4(camPos, ctx.state.fogDensity());

    // Key light = the same one that feeds the cascades (m_lights[0]) and with its SAME
    // criterion, which lives in keyLightDirection.
    // Without lights, neutral direction and black color: the fog only absorbs, which
    // is right when there is nothing to scatter.
    glm::vec3 lightDir(0.0f, -1.0f, 0.0f);
    glm::vec3 lightColor(0.0f);
    if (!ctx.lights.empty())
    {
        // The SAME criterion as the cascades, not a copy: when this derived
        // the direction on its own and the shadow pass changed its own, the
        // scattering pointed to one side and the shadow map was built towards
        // another. That is why a point light's aim point is not computed here:
        // it arrives through the Context, already resolved, and it is the same object that the
        // cascades received in this frame.
        keyLightDirection(ctx.lights[0].position, ctx.lights[0].direction,
                          ctx.sceneCenter, lightDir);
        lightColor = glm::vec3(ctx.lights[0].color) * ctx.lights[0].color.a;
    }
    push.lightDirFalloff = glm::vec4(lightDir, ctx.state.fogHeightFalloff());
    // The key light's color is folded here into the fog's tint: the
    // push constant is at the exact 128 bytes that Vulkan guarantees and
    // one more vec4 does not fit.
    push.scatterBaseHeight = glm::vec4(ctx.state.fogScatter() * lightColor, ctx.state.fogBaseHeight());
    push.gStepsRes = glm::vec4(ctx.state.fogAnisotropy(), (float)ctx.state.fogSteps(),
                               (float)ctx.renderExtent.width, (float)ctx.renderExtent.height);

    // The HDR goes to GENERAL, the only valid layout for imageLoad/imageStore.
    VkImageMemoryBarrier toFog{};
    toFog.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toFog.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toFog.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toFog.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    toFog.subresourceRange.baseMipLevel   = 0;
    toFog.subresourceRange.levelCount     = 1;
    toFog.subresourceRange.baseArrayLayer = 0;
    toFog.subresourceRange.layerCount     = 1;
    toFog.image                           = ctx.hdrImage[ctx.currentFrame];
    toFog.oldLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toFog.newLayout                       = VK_IMAGE_LAYOUT_GENERAL;
    toFog.srcAccessMask                   = VK_ACCESS_SHADER_READ_BIT;
    toFog.dstAccessMask                   = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    // And the pre-pass depth and the shadow map are read from compute:
    // they were written by the rasterizer, so that write has to be
    // made visible. They go through a memory barrier and not an image barrier because their
    // layout does NOT change (both stay in DEPTH_STENCIL_READ_ONLY).
    VkMemoryBarrier mem{};
    mem.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mem.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    mem.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                         | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                         | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &mem, 0, nullptr, 1, &toFog);

    const uint32_t gx = (ctx.renderExtent.width  + 7) / 8;
    const uint32_t gy = (ctx.renderExtent.height + 7) / 8;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                            0, 1, &m_sets[ctx.currentFrame], 0, nullptr);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, gx, gy, 1);

    // And the HDR goes back to SHADER_READ_ONLY, the layout declared by the
    // descriptor sets of the bloom (compute) and the composition (fragment).
    toFog.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    toFog.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toFog.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toFog.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toFog);

    if (ctx.timestampsSupported && m_queryPending[ctx.currentFrame])
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool, ctx.currentFrame * 2 + 1);
}

void FogPass::destroySets()
{
    // The sets die with the pool reset done by createSets; here
    // the handles are only nulled so that nobody ties them to already
    // destroyed views.
    for (int f = 0; f < kFramesInFlight; f++) m_sets[f] = VK_NULL_HANDLE;
}

} // namespace DonTopo
