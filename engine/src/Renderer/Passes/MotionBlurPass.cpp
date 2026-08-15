#include "DonTopo/Renderer/Passes/MotionBlurPass.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/GpuResources.h"
#include "DonTopo/Renderer/RendererState.h"
#include <stdexcept>
#include <fstream>
#include <vector>
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

// Los mismos campos y en el mismo orden que el bloque de
// motion_blur.comp.
struct MotionBlurPush {
    glm::mat4 reproject;
    float     invResX;
    float     invResY;
    float     intensity;
    float     maxRadius;
    int32_t   samples;
};
static_assert(sizeof(MotionBlurPush) == 84,
              "MotionBlurPush debe seguir en 84 bytes: motion_blur.comp declara este layout");

// ── Motion blur ─────────────────────────────────────────────────────────────
bool MotionBlurPass::active(const Context& ctx) const
{
    if (!ctx.state.motionBlurEnabled()) return false;
    // Recursos aún sin crear (viewport degenerado): nada que grabar.
    if (m_image[ctx.currentFrame] == VK_NULL_HANDLE) return false;
    // Menos de dos taps no promedia nada: el resultado sería el píxel central
    // y la copia de vuelta escribiría la misma imagen con el coste de un
    // dispatch entero.
    return ctx.state.motionBlurSamples() >= 2;
}

void MotionBlurPass::createPipeline(const Context& ctx)
{
    // Tres bindings: la escena muestreada, la profundidad muestreada y la
    // imagen intermedia como storage. No hay sampler propio: el color va con
    // el del SSR (LINEAR + CLAMP_TO_EDGE, que es lo que quieren unos taps
    // entre texeles y que no traigan color del borde opuesto) y la
    // profundidad con el del SSAO (NEAREST, el que le toca a D32_SFLOAT).
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
        throw std::runtime_error("failed to create motion blur descriptor set layout!");

    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[0].descriptorCount = kFramesInFlight * 2;
    sizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[1].descriptorCount = kFramesInFlight;

    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes    = sizes;
    dpi.maxSets       = kFramesInFlight;
    if (vkCreateDescriptorPool(ctx.gpu.device(), &dpi, nullptr, &m_descPool) != VK_SUCCESS)
        throw std::runtime_error("failed to create motion blur descriptor pool!");

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset     = 0;
    pcr.size       = sizeof(MotionBlurPush);

    VkPipelineLayoutCreateInfo pli{};
    pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount         = 1;
    pli.pSetLayouts            = &m_descLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(ctx.gpu.device(), &pli, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create motion blur pipeline layout!");

    auto code   = loadSpv("shaders/motion_blur.comp.spv");
    auto module = makeModule(ctx.gpu.device(), code);

    VkComputePipelineCreateInfo ci{};
    ci.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName  = "main";
    ci.layout       = m_pipelineLayout;
    if (vkCreateComputePipelines(ctx.gpu.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &m_pipeline) != VK_SUCCESS)
        throw std::runtime_error("failed to create compute pipeline: shaders/motion_blur.comp.spv");

    vkDestroyShaderModule(ctx.gpu.device(), module, nullptr);

    printf("motion blur pipeline OK\n"); fflush(stdout);
}

void MotionBlurPass::destroyPipeline(const Context& ctx)
{
    // Las imagenes y los sets se fueron con destroyImages.
    vkDestroyPipeline(ctx.gpu.device(), m_pipeline, nullptr);
    vkDestroyPipelineLayout(ctx.gpu.device(), m_pipelineLayout, nullptr);
    vkDestroyDescriptorPool(ctx.gpu.device(), m_descPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.gpu.device(), m_descLayout, nullptr);
}

void MotionBlurPass::createImages(const Context& ctx)
{
    for (int f = 0; f < kFramesInFlight; f++)
    {
        // Mismo formato y tamaño que el HDR: es la copia emborronada de esa
        // misma imagen, y vkCmdCopyImage exige formatos compatibles.
        // TRANSFER_SRC porque de aquí sale esa copia.
        ctx.res.createImage(
            ctx.renderExtent.width, ctx.renderExtent.height,
            ctx.hdrFormat, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            m_image[f], m_memory[f]);
        ctx.res.createTextureImageView(m_image[f], m_view[f], ctx.hdrFormat);
    }

    // Los sets de la vez anterior apuntan a vistas ya destruidas: reset y no
    // free, igual que en el bloom, el SSAO y el SSR.
    vkResetDescriptorPool(ctx.gpu.device(), m_descPool, 0);

    for (int f = 0; f < kFramesInFlight; f++)
    {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = m_descPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &m_descLayout;
        if (vkAllocateDescriptorSets(ctx.gpu.device(), &ai, &m_sets[f]) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate motion blur descriptor sets!");

        VkDescriptorImageInfo infos[3]{};
        // El HDR entra en SHADER_READ_ONLY: es donde lo dejan el render pass,
        // el SSR y la niebla.
        infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[0].imageView   = ctx.hdrView[f];
        infos[0].sampler     = ctx.ssrSampler;
        infos[1].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        infos[1].imageView   = ctx.ssaoDepthView[f];
        infos[1].sampler     = ctx.ssaoSampler;
        infos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        infos[2].imageView   = m_view[f];

        VkWriteDescriptorSet writes[3]{};
        for (int i = 0; i < 3; i++)
        {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = m_sets[f];
            writes[i].dstBinding      = (uint32_t)i;
            writes[i].descriptorType  = (i == 2) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                 : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].descriptorCount = 1;
            writes[i].pImageInfo      = &infos[i];
        }
        vkUpdateDescriptorSets(ctx.gpu.device(), 3, writes, 0, nullptr);
    }
}

void MotionBlurPass::destroyImages(const Context& ctx)
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
        m_sets[f] = VK_NULL_HANDLE;
    }
}

void MotionBlurPass::record(const Context& ctx, VkCommandBuffer cmd)
{
    if (!active(ctx) || m_sets[ctx.currentFrame] == VK_NULL_HANDLE)
    {
        // Ni dispatch ni copia ni barreras: el HDR se queda tal y como lo
        // dejaron el pass de escena, el SSR y la niebla, en SHADER_READ_ONLY,
        // que es justo lo que esperan el bloom y la composición. Imagen
        // idéntica a la de antes de esta feature.
        return;
    }

    MotionBlurPush push{};
    // La MISMA matriz que reproyecta el TAA: clip de este frame (sin jitter)
    // → clip del anterior. taaCurrViewProj y taaPrevViewProj se
    // actualizan todos los frames, esté el TAA activo o no.
    push.reproject = ctx.taaPrevViewProj * glm::inverse(ctx.taaCurrViewProj);
    push.invResX   = 1.0f / (float)ctx.renderExtent.width;
    push.invResY   = 1.0f / (float)ctx.renderExtent.height;
    push.intensity = ctx.state.motionBlurIntensity();
    push.maxRadius = ctx.state.motionBlurMaxRadius();
    push.samples   = (int32_t)ctx.state.motionBlurSamples();

    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel   = 0;
    b.subresourceRange.levelCount     = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount     = 1;

    // La intermedia entra desde UNDEFINED: se reescribe entera (el shader
    // escribe TODOS los píxeles, también los que no emborrona) y el contenido
    // del frame anterior no se reutiliza.
    b.image         = m_image[ctx.currentFrame];
    b.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                            0, 1, &m_sets[ctx.currentFrame], 0, nullptr);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (ctx.renderExtent.width + 7) / 8, (ctx.renderExtent.height + 7) / 8, 1);

    // La copia de vuelta, y no un segundo dispatch: es una copia 1:1 de la
    // imagen entera, que es lo que mejor hace el hardware. Las dos imágenes
    // pasan a sus layouts de transferencia.
    VkImageMemoryBarrier toCopy[2] = { b, b };
    toCopy[0].image         = m_image[ctx.currentFrame];
    toCopy[0].oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    toCopy[0].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toCopy[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toCopy[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toCopy[1].image         = ctx.hdrImage[ctx.currentFrame];
    toCopy[1].oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toCopy[1].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toCopy[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toCopy[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, toCopy);

    VkImageCopy region{};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.layerCount = 1;
    region.dstSubresource            = region.srcSubresource;
    region.extent                    = { ctx.renderExtent.width, ctx.renderExtent.height, 1 };
    vkCmdCopyImage(cmd,
                   m_image[ctx.currentFrame],      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   ctx.hdrImage[ctx.currentFrame], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region);

    // Y el HDR vuelve a SHADER_READ_ONLY, que es el layout que declaran los
    // descriptor sets del bloom (compute) y de la composición (fragment).
    b.image         = ctx.hdrImage[ctx.currentFrame];
    b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
}

} // namespace DonTopo
