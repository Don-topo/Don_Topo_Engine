#include "DonTopo/Renderer/Passes/IblPass.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include <stdexcept>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include "DonTopo/Renderer/ShaderModule.h"

namespace DonTopo {

// ── helpers ──────────────────────────────────────────────────────────────────

// ── IBL global ──────────────────────────────────────────────────────────────

void IblPass::createResources(const Context& ctx)
{
    // 1. The two images. They do not use m_res.createImage: that one fixes arrayLayers and
    // mipLevels to 1, and here 6 layers are needed (and mips in the prefilter).
    auto makeCube = [&](uint32_t size, uint32_t mips, VkImage& image, VkDeviceMemory& memory)
    {
        VkImageCreateInfo ci{};
        ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.format        = kFormat;
        ci.extent        = { size, size, 1 };
        ci.mipLevels     = mips;
        ci.arrayLayers   = 6;
        ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        // TRANSFER_DST is for the neutral clear below, not for a copy.
        ci.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
                         | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(ctx.gpu.device(), &ci, nullptr, &image) != VK_SUCCESS)
            throw std::runtime_error("failed to create IBL cubemap image!");

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(ctx.gpu.device(), image, &memReq);
        VkMemoryAllocateInfo memAlloc{};
        memAlloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memAlloc.allocationSize  = memReq.size;
        memAlloc.memoryTypeIndex = ctx.gpu.findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(ctx.gpu.device(), &memAlloc, nullptr, &memory) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate IBL cubemap memory!");
        vkBindImageMemory(ctx.gpu.device(), image, memory, 0);
    };

    makeCube(kIrradianceSize, 1,              m_irradianceImage, m_irradianceMemory);
    makeCube(kPrefilterSize,  kPrefilterMips, m_prefilterImage,  m_prefilterMemory);

    // 2. Views. The CUBE one is the one that goes in the objects' descriptor sets;
    // the 2D_ARRAY ones only exist so the compute can write them as a storage
    // image, one per mip level because imageStore does not choose the level.
    auto makeView = [&](VkImage image, VkImageViewType type, uint32_t baseMip, uint32_t mipCount, VkImageView& view)
    {
        VkImageViewCreateInfo vi{};
        vi.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image                           = image;
        vi.viewType                        = type;
        vi.format                          = kFormat;
        vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.baseMipLevel   = baseMip;
        vi.subresourceRange.levelCount     = mipCount;
        vi.subresourceRange.baseArrayLayer = 0;
        vi.subresourceRange.layerCount     = 6;
        if (vkCreateImageView(ctx.gpu.device(), &vi, nullptr, &view) != VK_SUCCESS)
            throw std::runtime_error("failed to create IBL image view!");
    };

    makeView(m_irradianceImage, VK_IMAGE_VIEW_TYPE_CUBE,     0, 1, m_irradianceView);
    makeView(m_irradianceImage, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, 1, m_irradianceStore);
    makeView(m_prefilterImage,  VK_IMAGE_VIEW_TYPE_CUBE,     0, kPrefilterMips, m_prefilterView);
    for (uint32_t m = 0; m < kPrefilterMips; m++)
        makeView(m_prefilterImage, VK_IMAGE_VIEW_TYPE_2D_ARRAY, m, 1, m_prefilterStore[m]);

    // 3. Common sampler. maxLod covers the prefilter's mips; the irradiance view
    // only has one level, so there the LOD is clamped on its own.
    VkSamplerCreateInfo si{};
    si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod       = (float)kPrefilterMips;
    if (vkCreateSampler(ctx.gpu.device(), &si, nullptr, &m_sampler) != VK_SUCCESS)
        throw std::runtime_error("failed to create IBL sampler!");

    // 4. Neutral content. It is what is seen if initSkybox is never called (or
    // if the cubemap fails to load): the same flat ambient as before, instead of
    // a descriptor pointing at garbage.
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

        // The same numbers that pbr.frag's hemispheric ambient had:
        // the average of sky and ground for the diffuse, the sky for the specular.
        const VkClearColorValue irradianceNeutral{{ 0.075f, 0.080f, 0.090f, 1.0f }};
        const VkClearColorValue prefilterNeutral {{ 0.100f, 0.120f, 0.150f, 1.0f }};
        clearTo(m_irradianceImage, 1,              irradianceNeutral);
        clearTo(m_prefilterImage,  kPrefilterMips, prefilterNeutral);

        ctx.gpu.endOneTimeCommands(cmd);
    }

    // 5. Descriptor set layout, pool and pipelines of the precomputation. Its own layout
    // and not createComputePipelines's: that one is 8 storage buffers.
    VkDescriptorSetLayoutBinding iblBindings[2]{};
    iblBindings[0].binding         = 0;
    iblBindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    iblBindings[0].descriptorCount = 1;
    iblBindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    iblBindings[1].binding         = 1;
    iblBindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    iblBindings[1].descriptorCount = 1;
    iblBindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dsl{};
    dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl.bindingCount = 2;
    dsl.pBindings    = iblBindings;
    if (vkCreateDescriptorSetLayout(ctx.gpu.device(), &dsl, nullptr, &m_descLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create IBL descriptor set layout!");

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset     = 0;
    pcr.size       = sizeof(Push);

    VkPipelineLayoutCreateInfo pli{};
    pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount         = 1;
    pli.pSetLayouts            = &m_descLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(ctx.gpu.device(), &pli, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create IBL pipeline layout!");

    // One set for the irradiance and one per prefilter mip: each one carries
    // a different storage image, so they cannot be reused.
    const uint32_t setCount = 1 + kPrefilterMips;
    VkDescriptorPoolSize iblSizes[2]{};
    iblSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    iblSizes[0].descriptorCount = setCount;
    iblSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    iblSizes[1].descriptorCount = setCount;

    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes    = iblSizes;
    dpi.maxSets       = setCount;
    if (vkCreateDescriptorPool(ctx.gpu.device(), &dpi, nullptr, &m_descPool) != VK_SUCCESS)
        throw std::runtime_error("failed to create IBL descriptor pool!");

    auto makeIblPipeline = [&](const std::string& spv, VkPipeline& pipeline)
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

    makeIblPipeline("shaders/ibl_irradiance.comp.spv", m_irradiancePipeline);
    makeIblPipeline("shaders/ibl_prefilter.comp.spv",  m_prefilterPipeline);
}

void IblPass::destroyResources(const Context& ctx)
{
    vkDestroyPipeline(ctx.gpu.device(), m_irradiancePipeline, nullptr);
    vkDestroyPipeline(ctx.gpu.device(), m_prefilterPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.gpu.device(), m_pipelineLayout, nullptr);
    vkDestroyDescriptorPool(ctx.gpu.device(), m_descPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.gpu.device(), m_descLayout, nullptr);
    vkDestroySampler(ctx.gpu.device(), m_sampler, nullptr);
    vkDestroyImageView(ctx.gpu.device(), m_irradianceView, nullptr);
    vkDestroyImageView(ctx.gpu.device(), m_irradianceStore, nullptr);
    vkDestroyImage(ctx.gpu.device(), m_irradianceImage, nullptr);
    vkFreeMemory(ctx.gpu.device(), m_irradianceMemory, nullptr);
    vkDestroyImageView(ctx.gpu.device(), m_prefilterView, nullptr);
    for (uint32_t m = 0; m < kPrefilterMips; m++)
        vkDestroyImageView(ctx.gpu.device(), m_prefilterStore[m], nullptr);
    vkDestroyImage(ctx.gpu.device(), m_prefilterImage, nullptr);
    vkFreeMemory(ctx.gpu.device(), m_prefilterMemory, nullptr);
}

void IblPass::precompute(const Context& ctx)
{
    // Without an environment cubemap there is nothing to convolve: the neutral
    // values that createResources left remain.
    if (ctx.envView == VK_NULL_HANDLE) return;

    const auto t0 = std::chrono::steady_clock::now();

    // Reset and not free: initSkybox could be called again (environment change)
    // and the sets from the previous time are no longer valid.
    vkResetDescriptorPool(ctx.gpu.device(), m_descPool, 0);

    const uint32_t setCount = 1 + kPrefilterMips;
    std::vector<VkDescriptorSetLayout> layouts(setCount, m_descLayout);
    std::vector<VkDescriptorSet>       sets(setCount, VK_NULL_HANDLE);

    VkDescriptorSetAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = m_descPool;
    ai.descriptorSetCount = setCount;
    ai.pSetLayouts        = layouts.data();
    if (vkAllocateDescriptorSets(ctx.gpu.device(), &ai, sets.data()) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate IBL descriptor sets!");

    VkDescriptorImageInfo envInfo{};
    envInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    envInfo.imageView   = ctx.envView;
    envInfo.sampler     = ctx.envSampler;

    // The VkDescriptorImageInfo have to stay alive until
    // vkUpdateDescriptorSets, so the vector is sized all at once.
    std::vector<VkDescriptorImageInfo>  storeInfos(setCount);
    std::vector<VkWriteDescriptorSet>   writes;
    writes.reserve(setCount * 2);
    for (uint32_t s = 0; s < setCount; s++)
    {
        storeInfos[s].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        storeInfos[s].imageView   = (s == 0) ? m_irradianceStore : m_prefilterStore[s - 1];

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

    VkImageMemoryBarrier barriers[2]{};
    for (int i = 0; i < 2; i++)
    {
        barriers[i].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[i].subresourceRange.layerCount = 6;
    }
    barriers[0].image = m_irradianceImage;
    barriers[0].subresourceRange.levelCount = 1;
    barriers[1].image = m_prefilterImage;
    barriers[1].subresourceRange.levelCount = kPrefilterMips;

    // To GENERAL: it is the only layout that supports imageStore.
    for (int i = 0; i < 2; i++)
    {
        barriers[i].oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[i].newLayout     = VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, barriers);

    // Irradiance: one invocation per texel, 6 layers in z.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_irradiancePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                            0, 1, &sets[0], 0, nullptr);
    // intensity 1.0: the global IBL scales nothing, so the two cubemaps
    // come out bit for bit as before the push carried that field.
    Push push{ 0.0f, kIrradianceSize, 1.0f };
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    const uint32_t irrGroups = (kIrradianceSize + 7) / 8;
    vkCmdDispatch(cmd, irrGroups, irrGroups, 6);

    // Prefilter: one dispatch per mip. They write disjoint regions and nobody
    // reads them in between, so no intermediate barriers are needed.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_prefilterPipeline);
    for (uint32_t m = 0; m < kPrefilterMips; m++)
    {
        const uint32_t mipSize = kPrefilterSize >> m;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                                0, 1, &sets[1 + m], 0, nullptr);
        Push mipPush{ (float)m / (float)(kPrefilterMips - 1), mipSize, 1.0f };
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mipPush), &mipPush);
        const uint32_t groups = (mipSize + 7) / 8;
        vkCmdDispatch(cmd, groups, groups, 6);
    }

    for (int i = 0; i < 2; i++)
    {
        barriers[i].oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, barriers);

    // Blocks until the queue finishes, so the measured ms includes the GPU.
    ctx.gpu.endOneTimeCommands(cmd);

    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    printf("IBL precompute: %.2f ms (irradiance %ux%u, prefilter %ux%u x%u mips)\n",
           ms, kIrradianceSize, kIrradianceSize,
           kPrefilterSize, kPrefilterSize, kPrefilterMips);
    fflush(stdout);
}

void IblPass::fillIblWrites(VkDescriptorSet set, VkImageView irradiance,
                            VkImageView prefilter, VkSampler sampler,
                            VkDescriptorImageInfo infos[2],
                            VkWriteDescriptorSet writes[2])
{
    const VkImageView vistas[2] = { irradiance, prefilter };
    for (int i = 0; i < 2; i++)
    {
        infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[i].imageView   = vistas[i];
        infos[i].sampler     = sampler;

        writes[i]                 = VkWriteDescriptorSet{};
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = set;
        writes[i].dstBinding      = kBindingIrradiance + (uint32_t)i;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo      = &infos[i];
    }
    static_assert(kBindingPrefilter == kBindingIrradiance + 1,
                  "the two IBL bindings are consecutive: the loop assumes that +1");
}

void IblPass::writeBindings(const Context& ctx, VkDescriptorSet set,
                            VkImageView irradiance, VkImageView prefilter) const
{
    // A standalone write on an ALREADY allocated set, like writeSsaoBinding:
    // rewriting the IBL bindings is the only thing needed for an
    // object to go from the global cubemap to a probe's. No new layout, no new
    // member in the UBO, no index in PushData (which is at exactly 80
    // bytes).
    VkDescriptorImageInfo infos[2]{};
    VkWriteDescriptorSet  w[2]{};
    fillIblWrites(set, irradiance, prefilter, m_sampler, infos, w);
    vkUpdateDescriptorSets(ctx.gpu.device(), 2, w, 0, nullptr);
}

} // namespace DonTopo
