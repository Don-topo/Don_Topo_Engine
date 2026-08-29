#include "DonTopo/Renderer/Passes/FogPass.h"
#include "DonTopo/Renderer/GpuDevice.h"
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

// 128 bytes exactos -el minimo que Vulkan garantiza-: los mismos
// campos y en el mismo orden que el bloque de fog.comp.
struct FogPush {
    glm::mat4 invViewProj;
    glm::vec4 camPosDensity;
    glm::vec4 lightDirFalloff;
    glm::vec4 scatterBaseHeight;
    glm::vec4 gStepsRes;
};
static_assert(sizeof(FogPush) == 128, "FogPush debe seguir en 128 bytes: fog.comp declara este layout");

// ── Niebla volumetrica ──────────────────────────────────────────────────────
void FogPass::createPipelines(const Context& ctx)
{
    // Cuatro bindings: HDR como storage (lectura + escritura in situ), la
    // profundidad del pre-pass, el UBO del frame (matriz de vista, cortes y
    // matrices de cascada) y el shadow map de la luz key.
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

    auto code   = loadSpv("shaders/fog.comp.spv");
    auto module = makeModule(ctx.gpu.device(), code);

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

    // Dos por frame, las que acotan el dispatch. timestampsSupported ya lo
    // resolvio el bloom.
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
    // El UBO del frame es uno de los cuatro bindings y en el primer init
    // todavia no existe cuando corre createOffscreenImages: ahi se sale sin
    // hacer nada y el final de init vuelve a llamar.
    if (ctx.uniformBuffers[0] == VK_NULL_HANDLE) return;

    // La niebla no tiene imagen propia: escribe dentro del HDR. Lo unico que
    // hay que rehacer con el swapchain son los sets, que referencian
    // hdrView y ssaoDepthView. Reset y no free, igual que en el SSR.
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

        // El mismo par vista+sampler que muestrea pbr.frag: comparador de
        // profundidad incluido, que es lo que espera sampler2DArrayShadow.
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
    // Apagada (o sets aun sin alojar, viewport degenerado): ni dispatch, ni
    // barreras, ni timestamps. El HDR se queda tal y como lo dejaron el pass
    // de escena y el SSR, en SHADER_READ_ONLY, que es justo lo que esperan el
    // bloom y la composicion. Imagen identica a la de antes de la feature.
    if (!ctx.state.fogEnabled() || m_sets[ctx.currentFrame] == VK_NULL_HANDLE)
    {
        m_gpuMs = 0.0f;
        m_queryPending[ctx.currentFrame] = false;
        return;
    }

    // Timestamps de hace dos frames en este mismo slot, cuya fence ya esperó
    // drawFrame, así que no bloquean a nadie.
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
                printf("fog (ray-marching): %.3f ms (%ux%u, %d pasos)\n",
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
    // La proyeccion EFECTIVA del frame (Y-flip de Vulkan dentro) por la
    // vista: es la que grabo el depth, asi que desproyectar es consistente.
    push.invViewProj = glm::inverse(proj * view);
    // La cámara en mundo sale de la propia vista: la cuarta columna de su
    // inversa. Así no hay que arrastrar un parámetro más hasta aquí.
    const glm::vec3 camPos = glm::vec3(glm::inverse(view)[3]);
    push.camPosDensity = glm::vec4(camPos, ctx.state.fogDensity());

    // Luz key = la misma que alimenta las cascadas (m_lights[0]) y con su MISMO
    // criterio, que vive en keyLightDirection.
    // Sin luces, dirección neutra y color negro: la niebla solo absorbe, que
    // es lo correcto cuando no hay nada que disperse.
    glm::vec3 lightDir(0.0f, -1.0f, 0.0f);
    glm::vec3 lightColor(0.0f);
    if (!ctx.lights.empty())
    {
        // El MISMO criterio que las cascadas, no una copia: cuando esto derivaba
        // la direccion por su cuenta y el shadow pass cambio el suyo, el
        // scattering apuntaba a un lado y el shadow map estaba construido hacia
        // otro.
        keyLightDirection(ctx.lights[0].position, ctx.lights[0].direction, lightDir);
        lightColor = glm::vec3(ctx.lights[0].color) * ctx.lights[0].color.a;
    }
    push.lightDirFalloff = glm::vec4(lightDir, ctx.state.fogHeightFalloff());
    // El color de la luz key se pliega aquí sobre el tinte de la niebla: la
    // push constant está en los 128 bytes exactos que Vulkan garantiza y no
    // cabe un vec4 más.
    push.scatterBaseHeight = glm::vec4(ctx.state.fogScatter() * lightColor, ctx.state.fogBaseHeight());
    push.gStepsRes = glm::vec4(ctx.state.fogAnisotropy(), (float)ctx.state.fogSteps(),
                               (float)ctx.renderExtent.width, (float)ctx.renderExtent.height);

    // El HDR pasa a GENERAL, el único layout válido para imageLoad/imageStore.
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

    // Y la profundidad del pre-pass y el shadow map se leen desde compute:
    // los escribió el rasterizador, así que hace falta hacer visible esa
    // escritura. Van por memory barrier y no por image barrier porque su
    // layout NO cambia (los dos siguen en DEPTH_STENCIL_READ_ONLY).
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

    // Y el HDR vuelve a SHADER_READ_ONLY, el layout que declaran los
    // descriptor sets del bloom (compute) y de la composición (fragment).
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
    // Los sets mueren con el reset del pool que hace createSets; aqui
    // solo se anulan los handles para que nadie los ate a vistas ya
    // destruidas.
    for (int f = 0; f < kFramesInFlight; f++) m_sets[f] = VK_NULL_HANDLE;
}

} // namespace DonTopo
