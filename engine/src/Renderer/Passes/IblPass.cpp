#include "DonTopo/Renderer/Passes/IblPass.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include <stdexcept>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace DonTopo {

// ── helpers ──────────────────────────────────────────────────────────────────

static std::vector<char> loadSpv(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("failed to open shader: " + path);
    size_t sz = (size_t)f.tellg();
    std::vector<char> buf(sz);
    f.seekg(0);
    f.read(buf.data(), (std::streamsize)sz);
    return buf;
}

static VkShaderModule makeModule(VkDevice dev, const std::vector<char>& code)
{
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS)
        throw std::runtime_error("failed to create shader module!");
    return m;
}

// ── IBL global ──────────────────────────────────────────────────────────────

void IblPass::createResources(const Context& ctx)
{
    // 1. Las dos imagenes. No usan m_res.createImage: esa fija arrayLayers y
    // mipLevels a 1, y aqui hacen falta 6 capas (y mips en el prefiltrado).
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
        // TRANSFER_DST es pa el clear neutro de mas abajo, no pa una copia.
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

    // 2. Vistas. La CUBE es la que va en los descriptor sets de los objetos;
    // las 2D_ARRAY solo existen pa que el compute las escriba como storage
    // image, una por nivel de mip porque imageStore no elige nivel.
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

    // 3. Sampler comun. maxLod cubre los mips del prefiltrado; la vista de
    // irradiancia solo tiene un nivel, asi que ahi el LOD se recorta solo.
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

    // 4. Contenido neutro. Es lo que se ve si nunca se llama a initSkybox (o
    // si el cubemap no carga): el mismo ambiente plano de antes, en vez de
    // un descriptor apuntando a basura.
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

        // Los mismos numeros que tenia el ambiente hemisferico de pbr.frag:
        // la media de cielo y suelo pal difuso, el cielo pal especular.
        const VkClearColorValue irradianceNeutral{{ 0.075f, 0.080f, 0.090f, 1.0f }};
        const VkClearColorValue prefilterNeutral {{ 0.100f, 0.120f, 0.150f, 1.0f }};
        clearTo(m_irradianceImage, 1,              irradianceNeutral);
        clearTo(m_prefilterImage,  kPrefilterMips, prefilterNeutral);

        ctx.gpu.endOneTimeCommands(cmd);
    }

    // 5. Descriptor set layout, pool y pipelines de la precomputacion. Layout
    // propio y no el de createComputePipelines: ese son 8 storage buffers.
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

    // Un set pa la irradiancia y uno por mip del prefiltrado: cada uno lleva
    // una storage image distinta, asi que no se pueden reutilizar.
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
        auto code   = loadSpv(spv);
        auto module = makeModule(ctx.gpu.device(), code);

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
    // Sin cubemap de entorno no hay nada que convolucionar: se quedan los
    // valores neutros que dejo createResources.
    if (ctx.envView == VK_NULL_HANDLE) return;

    const auto t0 = std::chrono::steady_clock::now();

    // Reset y no free: initSkybox podria llamarse otra vez (cambio de
    // entorno) y los sets de la vez anterior ya no valen.
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

    // Los VkDescriptorImageInfo tienen que seguir vivos hasta el
    // vkUpdateDescriptorSets, asi que el vector se dimensiona de golpe.
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

    // A GENERAL: es el unico layout que admite imageStore.
    for (int i = 0; i < 2; i++)
    {
        barriers[i].oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[i].newLayout     = VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, barriers);

    // Irradiancia: una invocacion por texel, 6 capas en z.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_irradiancePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                            0, 1, &sets[0], 0, nullptr);
    // intensity 1.0: el IBL global no escala nada, asi que los dos cubemaps
    // salen bit a bit como antes de que el push llevara ese campo.
    Push push{ 0.0f, kIrradianceSize, 1.0f };
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    const uint32_t irrGroups = (kIrradianceSize + 7) / 8;
    vkCmdDispatch(cmd, irrGroups, irrGroups, 6);

    // Prefiltrado: un dispatch por mip. Escriben regiones disjuntas y nadie
    // las lee entre medias, asi que no hacen falta barreras intermedias.
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

    // Bloquea hasta que la cola termina, asi que el ms medido incluye la GPU.
    ctx.gpu.endOneTimeCommands(cmd);

    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    printf("IBL precompute: %.2f ms (irradiance %ux%u, prefilter %ux%u x%u mips)\n",
           ms, kIrradianceSize, kIrradianceSize,
           kPrefilterSize, kPrefilterSize, kPrefilterMips);
    fflush(stdout);
}

void IblPass::writeBindings(const Context& ctx, VkDescriptorSet set,
                            VkImageView irradiance, VkImageView prefilter) const
{
    // Un write suelto sobre un set YA alojado, igual que writeSsaoBinding:
    // reescribir los bindings 5 y 6 es lo unico que hace falta para que un
    // objeto pase del IBL global a una sonda. Ni layout nuevo, ni miembro
    // nuevo en el UBO, ni un indice en PushData (que esta a 80 bytes justos).
    VkDescriptorImageInfo infos[2]{};
    infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infos[0].imageView   = irradiance;
    infos[0].sampler     = m_sampler;
    infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infos[1].imageView   = prefilter;
    infos[1].sampler     = m_sampler;

    VkWriteDescriptorSet w[2]{};
    for (int i = 0; i < 2; i++)
    {
        w[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet          = set;
        w[i].dstBinding      = 5 + i;
        w[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].descriptorCount = 1;
        w[i].pImageInfo      = &infos[i];
    }
    vkUpdateDescriptorSets(ctx.gpu.device(), 2, w, 0, nullptr);
}

} // namespace DonTopo
