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

// Compartida por ssr.comp y ssr_resolve.comp, que comparten pipeline
// layout. 48 bytes: los mismos campos y en el mismo orden que el
// bloque de los dos .comp.
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
static_assert(sizeof(SsrPush) == 48, "SsrPush debe seguir en 48 bytes: los dos .comp declaran este layout");

// ── SSR ─────────────────────────────────────────────────────────────────────
bool SsrPass::active(const Context& ctx) const
{
    if (!ctx.state.ssrEnabled()) return false;
    // Recursos aún sin crear (viewport degenerado): nada que grabar.
    if (m_image[ctx.currentFrame] == VK_NULL_HANDLE) return false;
    // Interruptor puesto pero ningún objeto marcado = ningún píxel con
    // máscara: se salta el pass entero en vez de despachar y multiplicar por
    // cero. Es un float por objeto, mucho menos que el culling que ya se hace
    // en este mismo frame.
    return ctx.anyObjectWithSsr;
}

void SsrPass::createPipelines(const Context& ctx)
{
    // LINEAR: el impacto del rayo cae entre texeles y R16G16B16A16_SFLOAT sí
    // tiene garantizado el filtrado lineal. La profundidad NO se muestrea con
    // este sampler sino con el del SSAO (NEAREST), que es el que le
    // corresponde a D32_SFLOAT. CLAMP_TO_EDGE para que un tap del borde no
    // traiga color del lado opuesto de la pantalla.
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

    // Un solo layout para los dos pipelines: color muestreado, profundidad
    // muestreada y destino como storage image. ssr_resolve.comp declara el
    // binding 1 y no lo lee.
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

    // Dos sets por frame: marcha (HDR + depth → reflejo) y suma (reflejo →
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

    makeSsrPipeline("shaders/ssr.comp.spv",         m_pipeline);
    makeSsrPipeline("shaders/ssr_resolve.comp.spv", m_resolvePipeline);

    // Cuatro por frame: [0,1] el depth pre-pass cuando lo pide el SSR, [2,3]
    // los dos dispatches. timestampsSupported ya lo resolvió el bloom.
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
    // Las imagenes y los sets ya se fueron con destroyImages; aqui solo queda
    // lo que es independiente del tamano.
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
        // Mismo formato que el HDR: ssr_resolve.comp declara los dos con el
        // qualifier rgba16f. Resolución completa, como el SSAO.
        ctx.res.createImage(
            ctx.renderExtent.width, ctx.renderExtent.height,
            ctx.hdrFormat, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            m_image[f], m_memory[f]);
        ctx.res.createTextureImageView(m_image[f], m_view[f], ctx.hdrFormat);
    }

    // Los sets de la vez anterior apuntan a vistas ya destruidas: reset y no
    // free, igual que en el bloom y en el SSAO.
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
        // Marcha: color de la escena (sale del render pass en SHADER_READ_ONLY)
        // + profundidad del pre-pass → reflejo.
        infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[0].imageView   = ctx.hdrView[f];
        infos[0].sampler     = m_sampler;
        infos[1].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        infos[1].imageView   = ctx.ssaoDepthView[f];
        infos[1].sampler     = ctx.ssaoSampler;
        infos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        infos[2].imageView   = m_view[f];
        // Suma: el reflejo (ya en GENERAL) → el HDR como storage. El binding 1
        // se rellena con la misma profundidad aunque el shader no lo lea: un
        // descriptor set no puede quedarse con un binding sin escribir.
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
        // Ni dispatches ni barreras: el HDR se queda tal y como lo dejó el
        // pass de escena, en SHADER_READ_ONLY, que es justo lo que esperan el
        // bloom y la composición. Imagen idéntica a la de antes del SSR.
        return;
    }

    // Timestamps de hace dos frames en este mismo slot, ya señalados.
    if (ctx.timestampsSupported && m_queryPending[ctx.currentFrame])
    {
        uint64_t stamps[4] = {};
        if (vkGetQueryPoolResults(ctx.gpu.device(), m_queryPool, ctx.currentFrame * 4, 4,
                                  sizeof(stamps), stamps, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        {
            // El pre-pass solo cuenta como coste del SSR cuando es el SSR
            // quien lo pide: con el SSAO encendido ya sale en ssaoGpuMs y
            // sumarlo aquí lo contaría dos veces.
            const uint64_t prepass = ctx.state.ssaoEnabled() ? 0 : (stamps[1] - stamps[0]);
            m_gpuMs = (float)((double)(prepass + (stamps[3] - stamps[2]))
                              * ctx.timestampPeriod * 1e-6);
            if (++m_measuredFrames == 300)
            {
                printf("ssr (marcha + suma%s): %.3f ms (%ux%u, %d pasos)\n",
                       ctx.state.ssaoEnabled() ? "" : " + depth pre-pass",
                       m_gpuMs, ctx.swapChainExtent.width, ctx.swapChainExtent.height,
                       ctx.state.ssrMaxSteps());
                fflush(stdout);
            }
        }
    }
    // Solo se da por bueno el frame en el que recordSsaoPass dejó escrito el
    // par [0,1]: sin eso la lectura de los cuatro daría NOT_READY.
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
    // Los mismos cuatro coeficientes que usa el SSAO, de la proyección que
    // grabó el depth. El signo de p11 se cancela igual que allí: ver ssao.comp.
    push.projP00     = proj[0][0];
    push.projP11     = proj[1][1];
    push.projP22     = proj[2][2];
    push.projP32     = proj[3][2];
    push.invResX     = 1.0f / (float)ctx.renderExtent.width;
    push.invResY     = 1.0f / (float)ctx.renderExtent.height;
    push.maxDistance = ctx.state.ssrMaxDistance();
    push.thickness   = ctx.state.ssrThickness();
    push.maxSteps    = (int32_t)ctx.state.ssrMaxSteps();
    // Fijo y no configurable: cuatro bisecciones ya sitúan el impacto dentro
    // de 1/16 de paso, y subirlo no cambia nada visible.
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

    // El reflejo entra desde UNDEFINED: se reescribe entero (ssr.comp empieza
    // por poner el píxel a 0) y el contenido del frame anterior no se
    // reutiliza.
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

    // Dos transiciones antes de la suma: el reflejo que se acaba de escribir
    // pasa a leerse, y el HDR sale de SHADER_READ_ONLY (donde lo dejó el
    // render pass, y desde donde acaba de leerlo la marcha) a GENERAL, que es
    // el único layout válido para imageLoad/imageStore.
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

    // Y el HDR vuelve a SHADER_READ_ONLY, que es el layout que declaran los
    // descriptor sets del bloom (compute) y de la composición (fragment).
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
