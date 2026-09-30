#include "DonTopo/Renderer/Passes/ForwardPlusPass.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/GpuResources.h"
#include <stdexcept>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include "DonTopo/Renderer/ShaderModule.h"

namespace DonTopo {

// ── helpers ──────────────────────────────────────────────────────────────────

// A light from the SSBO. viewPosR is the SAME light in view space: the CPU computes it
// so that the culling does not need the view matrix.
struct FpLightGpu {
    glm::vec4 posRadius;
    glm::vec4 color;
    glm::vec4 viewPosR;
    // The two type fields of DonTopo::Light. Without them the
    // fragment shader would not know how to evaluate a spot or a directional
    // through the Forward+ path, and the binning could not leave the
    // directional always visible.
    glm::vec4 direction;    // xyz dir, w type
    glm::vec4 params;       // range, inner cos, outer cos, width
};
static_assert(sizeof(FpLightGpu) == 80, "FpLightGpu must stay at 80 bytes: it is the std430 stride of the light array");

// Push constant shared by the two .comp files.
struct FpPush {
    float    p00;
    float    p11;
    float    p22;
    float    p32;
    uint32_t screenW;
    uint32_t screenH;
    uint32_t pad0;
    uint32_t pad1;
};
static_assert(sizeof(FpPush) == 32, "FpPush must stay at 32 bytes: both .comp shaders declare this layout");

// ── Forward+ ────────────────────────────────────────────────────────────────
void ForwardPlusPass::gridDims(const Context& ctx, RendererState::FpMode mode,
                               uint32_t& gridX, uint32_t& gridY,
                               uint32_t& gridZ, uint32_t& tileSize) const
{
    // With renderExtent and NOT the swapchain's: with SSAA the render is
    // larger than the window, and sizing with the window's would leave
    // pbr.frag reading cells outside the buffer without the validation saying
    // anything (gl_FragCoord is in target pixels).
    if (mode == RendererState::FpMode::Clustered)
    {
        tileSize = kClusterTile;
        gridZ    = kClusterSlices;
    }
    else
    {
        tileSize = kTileSize;
        gridZ    = 1;
    }
    gridX = (ctx.renderExtent.width  + tileSize - 1) / tileSize;
    gridY = (ctx.renderExtent.height + tileSize - 1) / tileSize;
}

void ForwardPlusPass::createPipelines(const Context& ctx)
{
    // Six bindings. The first four are also seen by pbr.frag (set 2); the
    // depth and the counters are only the compute's, and it is legal for the fragment
    // shader not to declare them.
    VkDescriptorSetLayoutBinding bindings[6]{};
    for (uint32_t i = 0; i < 6; i++)
    {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = (i == 4) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                               : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = (i < 4) ? (VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                                              : VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo dsl{};
    dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl.bindingCount = 6;
    dsl.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(ctx.gpu.device(), &dsl, nullptr, &m_descLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create forward+ descriptor set layout!");

    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[0].descriptorCount = kFramesInFlight * 5;
    sizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[1].descriptorCount = kFramesInFlight;

    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes    = sizes;
    dpi.maxSets       = kFramesInFlight;
    if (vkCreateDescriptorPool(ctx.gpu.device(), &dpi, nullptr, &m_descPool) != VK_SUCCESS)
        throw std::runtime_error("failed to create forward+ descriptor pool!");

    // The culling pipeline declares the set at index 0; the scene one
    // declares it at 2. It is the same VkDescriptorSet: a set fits at
    // any index as long as the VkDescriptorSetLayout matches.
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset     = 0;
    pcr.size       = sizeof(FpPush);

    VkPipelineLayoutCreateInfo pli{};
    pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount         = 1;
    pli.pSetLayouts            = &m_descLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(ctx.gpu.device(), &pli, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create forward+ pipeline layout!");

    auto makePipeline = [&](const std::string& spv, VkPipeline& pipeline)
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

    makePipeline("shaders/light_cull_tiled.comp.spv",     m_tiledPipeline);
    makePipeline("shaders/light_cull_clustered.comp.spv", m_clusteredPipeline);

    // Parameters, lights and counters: they do not depend on the size, live the whole
    // process and are written from the CPU every frame (persistent mapping, like
    // the UBO). The counters are also READ: the GPU writes them with
    // atomics and the CPU collects them two frames later.
    const VkMemoryPropertyFlags hostFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (int f = 0; f < kFramesInFlight; f++)
    {
        ctx.res.createBuffer(sizeof(ParamsGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostFlags,
                             m_paramsBuffer[f], m_paramsMemory[f]);
        vkMapMemory(ctx.gpu.device(), m_paramsMemory[f], 0, sizeof(ParamsGpu), 0, &m_paramsMapped[f]);
        memset(m_paramsMapped[f], 0, sizeof(ParamsGpu));

        const VkDeviceSize lightSize = sizeof(FpLightGpu) * kMaxLights;
        ctx.res.createBuffer(lightSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostFlags,
                             m_lightBuffer[f], m_lightMemory[f]);
        vkMapMemory(ctx.gpu.device(), m_lightMemory[f], 0, lightSize, 0, &m_lightMapped[f]);
        memset(m_lightMapped[f], 0, (size_t)lightSize);

        // 4 uint: [0] sum of assigned lights, [1] non-empty cells,
        // [2] overflowed cells, [3] unused (alignment).
        ctx.res.createBuffer(sizeof(uint32_t) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostFlags,
                             m_statsBuffer[f], m_statsMemory[f]);
        vkMapMemory(ctx.gpu.device(), m_statsMemory[f], 0, sizeof(uint32_t) * 4, 0, &m_statsMapped[f]);
        memset(m_statsMapped[f], 0, sizeof(uint32_t) * 4);
    }

    // Own queries: mixing them with the SSAO's or the AA's would lump two
    // measurements together.
    if (ctx.timestampsSupported)
    {
        VkQueryPoolCreateInfo qpi{};
        qpi.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = kFramesInFlight * 2;
        if (vkCreateQueryPool(ctx.gpu.device(), &qpi, nullptr, &m_queryPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create forward+ query pool!");
    }

    printf("forward+ pipelines OK\n"); fflush(stdout);
}

void ForwardPlusPass::destroyPipelines(const Context& ctx)
{
    vkDestroyPipeline(ctx.gpu.device(), m_tiledPipeline, nullptr);
    vkDestroyPipeline(ctx.gpu.device(), m_clusteredPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.gpu.device(), m_pipelineLayout, nullptr);
    vkDestroyDescriptorPool(ctx.gpu.device(), m_descPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.gpu.device(), m_descLayout, nullptr);
    // The three persistently mapped buffers do not need unmap: the mapping
    // dies with the memory, as with the UBO and the instance SSBO.
    for (int f = 0; f < kFramesInFlight; f++)
    {
        vkDestroyBuffer(ctx.gpu.device(), m_paramsBuffer[f], nullptr);
        vkFreeMemory(ctx.gpu.device(), m_paramsMemory[f], nullptr);
        vkDestroyBuffer(ctx.gpu.device(), m_lightBuffer[f], nullptr);
        vkFreeMemory(ctx.gpu.device(), m_lightMemory[f], nullptr);
        vkDestroyBuffer(ctx.gpu.device(), m_statsBuffer[f], nullptr);
        vkFreeMemory(ctx.gpu.device(), m_statsMemory[f], nullptr);
    }
    if (m_queryPool != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(ctx.gpu.device(), m_queryPool, nullptr);
        m_queryPool = VK_NULL_HANDLE;
    }
}

void ForwardPlusPass::createBuffers(const Context& ctx)
{
    // To the LARGER of the two grids: that way switching mode at runtime does not
    // recreate anything and a frame cannot be recorded with the buffers of the
    // previous mode. The memory difference between one and the other is negligible
    // next to having two sets of buffers.
    uint32_t gx = 0, gy = 0, gz = 0, ts = 0;
    gridDims(ctx, RendererState::FpMode::Tiled, gx, gy, gz, ts);
    uint32_t maxCells = gx * gy * gz;
    gridDims(ctx, RendererState::FpMode::Clustered, gx, gy, gz, ts);
    maxCells = std::max(maxCells, gx * gy * gz);
    // Degenerate viewport: nothing to size. The rest of the frame already skips
    // the whole pass.
    if (maxCells == 0) return;

    for (int f = 0; f < kFramesInFlight; f++)
    {
        ctx.res.createBuffer((VkDeviceSize)maxCells * sizeof(uint32_t) * 2,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                             m_gridBuffer[f], m_gridMemory[f]);
        ctx.res.createBuffer((VkDeviceSize)maxCells * kMaxPerCell * sizeof(uint32_t),
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                             m_indexBuffer[f], m_indexMemory[f]);
    }

    // The sets from the previous time point to already destroyed buffers: reset and not
    // free, as in the bloom and the SSAO.
    vkResetDescriptorPool(ctx.gpu.device(), m_descPool, 0);

    for (int f = 0; f < kFramesInFlight; f++)
    {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = m_descPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &m_descLayout;
        if (vkAllocateDescriptorSets(ctx.gpu.device(), &ai, &m_sets[f]) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate forward+ descriptor set!");

        VkDescriptorBufferInfo bufs[5]{};
        bufs[0].buffer = m_paramsBuffer[f];
        bufs[1].buffer = m_lightBuffer[f];
        bufs[2].buffer = m_gridBuffer[f];
        bufs[3].buffer = m_indexBuffer[f];
        bufs[4].buffer = m_statsBuffer[f];
        for (int i = 0; i < 5; i++) bufs[i].range = VK_WHOLE_SIZE;

        // The depth from the depth pre-pass, the same one SSAO samples, and
        // with the same NEAREST sampler: it is D32_SFLOAT and the culling reads it at
        // exact texel.
        VkDescriptorImageInfo depthInfo{};
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        depthInfo.imageView   = ctx.depthView[f];
        depthInfo.sampler     = ctx.depthSampler;

        VkWriteDescriptorSet writes[6]{};
        for (int i = 0; i < 6; i++)
        {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = m_sets[f];
            writes[i].dstBinding      = (uint32_t)i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = (i == 4) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                 : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        writes[0].pBufferInfo = &bufs[0];
        writes[1].pBufferInfo = &bufs[1];
        writes[2].pBufferInfo = &bufs[2];
        writes[3].pBufferInfo = &bufs[3];
        writes[4].pImageInfo  = &depthInfo;
        writes[5].pBufferInfo = &bufs[4];

        vkUpdateDescriptorSets(ctx.gpu.device(), 6, writes, 0, nullptr);
    }
}

void ForwardPlusPass::destroyBuffers(const Context& ctx)
{
    for (int f = 0; f < kFramesInFlight; f++)
    {
        if (m_gridBuffer[f])
        {
            vkDestroyBuffer(ctx.gpu.device(), m_gridBuffer[f], nullptr);
            m_gridBuffer[f] = VK_NULL_HANDLE;
        }
        if (m_gridMemory[f])
        {
            vkFreeMemory(ctx.gpu.device(), m_gridMemory[f], nullptr);
            m_gridMemory[f] = VK_NULL_HANDLE;
        }
        if (m_indexBuffer[f])
        {
            vkDestroyBuffer(ctx.gpu.device(), m_indexBuffer[f], nullptr);
            m_indexBuffer[f] = VK_NULL_HANDLE;
        }
        if (m_indexMemory[f])
        {
            vkFreeMemory(ctx.gpu.device(), m_indexMemory[f], nullptr);
            m_indexMemory[f] = VK_NULL_HANDLE;
        }
        m_sets[f] = VK_NULL_HANDLE;
    }
}

void ForwardPlusPass::uploadFrameData(const Context& ctx, const glm::mat4& view, const glm::mat4& proj,
                                      const std::vector<Light>& lights,
                                      const std::vector<float>& lightRadii, float defaultRadius)
{
    // It is ALWAYS written, also in Off: pbr.frag reads fp.mode from here to
    // decide which branch to take, and with 0 it does not touch a single extra buffer.
    if (!m_paramsMapped[ctx.currentFrame]) return;

    uint32_t gx = 0, gy = 0, gz = 0, ts = 0;
    gridDims(ctx, ctx.activeMode, gx, gy, gz, ts);

    // zNear/zFar come from the projection itself (RH_ZO): p22 = f/(n-f) and
    // p32 = f*n/(n-f), so n = p32/p22 and f = p32/(p22+1). It is the only
    // way for the grid to follow the CameraComponent's camera in Play
    // without duplicating here the editor camera's planes.
    const float p22 = proj[2][2];
    const float p32 = proj[3][2];
    const float zNear = (p22 != 0.0f) ? p32 / p22 : 0.1f;
    const float zFar  = (p22 != -1.0f) ? p32 / (p22 + 1.0f) : 1000.0f;

    const uint32_t count = (uint32_t)std::min<size_t>(lights.size(), kMaxLights);

    ParamsGpu fp{};
    fp.mode       = (uint32_t)ctx.activeMode;
    fp.gridX      = gx;
    fp.gridY      = gy;
    fp.gridZ      = gz;
    fp.tileSize   = ts;
    fp.maxPerCell = kMaxPerCell;
    fp.numLights  = count;
    fp.zNear      = zNear;
    fp.zFar       = zFar;
    // Inverse of the logarithmic split of light_cull_clustered.comp:
    // slice = log2(z)*scale + bias.
    const float logRatio = std::log2(std::max(zFar / zNear, 1.0001f));
    fp.sliceScale = (float)gz / logRatio;
    fp.sliceBias  = -std::log2(zNear) * fp.sliceScale;
    memcpy(m_paramsMapped[ctx.currentFrame], &fp, sizeof(fp));

    if (m_lightMapped[ctx.currentFrame] && count > 0)
    {
        FpLightGpu* dst = (FpLightGpu*)m_lightMapped[ctx.currentFrame];
        for (uint32_t i = 0; i < count; i++)
        {
            // The radius is the only piece of data that Light does not carry: per light if the
            // user has given it, and if not the global one. It does not fit in the UBO without
            // moving the std140 layout that 5 shaders declare.
            const float radius = (i < lightRadii.size()) ? lightRadii[i] : defaultRadius;
            const glm::vec3 wp = glm::vec3(lights[i].position);
            // The same light in view space, so that the culling does not need
            // the view matrix or recompute it per cell.
            const glm::vec3 vp = glm::vec3(view * glm::vec4(wp, 1.0f));
            dst[i].posRadius = glm::vec4(wp, radius);
            dst[i].color     = lights[i].color;
            dst[i].viewPosR  = glm::vec4(vp, radius);
            dst[i].direction = lights[i].direction;
            dst[i].params    = lights[i].params;
        }
    }
}

bool ForwardPlusPass::overrideModeOff(ParamsGpu& saved)
{
    if (!m_paramsMapped[0]) return false;
    memcpy(&saved, m_paramsMapped[0], sizeof(ParamsGpu));
    ParamsGpu off = saved;
    off.mode = 0;
    memcpy(m_paramsMapped[0], &off, sizeof(off));
    return true;
}

void ForwardPlusPass::restoreParams(const ParamsGpu& saved)
{
    memcpy(m_paramsMapped[0], &saved, sizeof(saved));
}

void ForwardPlusPass::record(const Context& ctx, VkCommandBuffer cmd, const glm::mat4& proj)
{
    // Off: not a single command. It is what makes the image and the cost be
    // exactly those from before the feature.
    if (ctx.activeMode == RendererState::FpMode::Off) { m_gpuMs = 0.0f; return; }
    if (m_sets[ctx.currentFrame] == VK_NULL_HANDLE) return;

    // Counters from two frames ago in this same slot: its fence was already awaited by
    // drawFrame, so the read does not block. They are read BEFORE zeroing
    // them for this frame.
    if (m_statsMapped[ctx.currentFrame])
    {
        const uint32_t* s = (const uint32_t*)m_statsMapped[ctx.currentFrame];
        m_avgPerCell    = (s[1] > 0) ? (float)s[0] / (float)s[1] : 0.0f;
        m_overflowCells = s[2];
        memset(m_statsMapped[ctx.currentFrame], 0, sizeof(uint32_t) * 4);
    }

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
                printf("forward+ (%s): culling %.3f ms, %.1f lights/cell, %u overflowing cells (%ux%u internal)\n",
                       ctx.activeMode == RendererState::FpMode::Tiled ? "tiled" : "clustered",
                       m_gpuMs, m_avgPerCell, m_overflowCells,
                       ctx.renderExtent.width, ctx.renderExtent.height);
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

    uint32_t gx = 0, gy = 0, gz = 0, ts = 0;
    gridDims(ctx, ctx.activeMode, gx, gy, gz, ts);

    FpPush push{};
    // The frame's EFFECTIVE projection, with Vulkan's Y-flip inside: it is the
    // same one the depth pre-pass was recorded with, so reconstructing
    // depth and lifting the tile's planes is consistent.
    push.p00     = proj[0][0];
    push.p11     = proj[1][1];
    push.p22     = proj[2][2];
    push.p32     = proj[3][2];
    push.screenW = ctx.renderExtent.width;
    push.screenH = ctx.renderExtent.height;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      ctx.activeMode == RendererState::FpMode::Tiled ? m_tiledPipeline : m_clusteredPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                            0, 1, &m_sets[ctx.currentFrame], 0, nullptr);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

    if (ctx.activeMode == RendererState::FpMode::Tiled)
    {
        // One 16x16 workgroup PER TILE: the shader reads one texel per
        // invocation to reduce the tile's maximum depth.
        vkCmdDispatch(cmd, gx, gy, 1);
    }
    else
    {
        // One invocation per cluster, in groups of 4x4x4.
        vkCmdDispatch(cmd, (gx + 3) / 4, (gy + 3) / 4, (gz + 3) / 4);
    }

    // The grid and the index list are read by pbr.frag in the scene pass.
    VkMemoryBarrier mb{};
    mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);

    // And the counters are read by the CPU two frames later.
    VkMemoryBarrier hostMb{};
    hostMb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hostMb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    hostMb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &hostMb, 0, nullptr, 0, nullptr);

    if (ctx.timestampsSupported)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool, ctx.currentFrame * 2 + 1);
}

} // namespace DonTopo
