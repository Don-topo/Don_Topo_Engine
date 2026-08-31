#include "DonTopo/Renderer/Passes/AaPass.h"
#include "DonTopo/Renderer/TaaJitter.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/GpuResources.h"
#include <stdexcept>
#include <fstream>
#include <string>
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include "DonTopo/Renderer/ShaderModule.h"

namespace DonTopo {

// ── helpers ──────────────────────────────────────────────────────────────────

// Los tres modos con pass propio. None y MSAA no lo tienen: en MSAA el resolve
// ocurre dentro del pass de composicion.
static bool needsIntermediate(RendererState::AaMode m)
{
    return m == RendererState::AaMode::Fxaa || m == RendererState::AaMode::Ssaa
        || m == RendererState::AaMode::Taa;
}

// FXAA: mismo layout que declara fxaa.frag.
struct FxaaPush {
    float invResX;
    float invResY;
    float subpix;
    float edgeThreshold;
    float edgeThresholdMin;
};
static_assert(sizeof(FxaaPush) == 20, "FxaaPush debe seguir en 20 bytes: fxaa.frag declara este layout");

// SSAA: mismo layout que declara ssaa_resolve.frag.
struct SsaaPush {
    float invSrcX;      // 1/ancho de la imagen intermedia (la grande)
    float invSrcY;
    int32_t taps;       // muestras por eje del filtro de bajada
};
static_assert(sizeof(SsaaPush) == 12, "SsaaPush debe seguir en 12 bytes: ssaa_resolve.frag declara este layout");

// TAA: mismo layout que declara taa.frag.
struct TaaPush {
    glm::mat4 reproject;
    float     invResX;
    float     invResY;
    float     feedback;
    int32_t   historyValid;
};
static_assert(sizeof(TaaPush) == 80, "TaaPush debe seguir en 80 bytes: taa.frag declara este layout");

// ── Anti-aliasing ───────────────────────────────────────────────────────────
void AaPass::createRenderPasses(const Context& ctx)
{
    // Un solo attachment: la offscreen de siempre. El triangulo la
    // cubre entera, asi que no hay nada que cargar. Sin depth: el contorno y
    // los gizmos ya se dibujaron en el pass de composicion, aguas arriba.
    VkAttachmentDescription colorAtt{};
    colorAtt.format         = ctx.swapChainFormat;
    colorAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    // El MISMO finalLayout que deja el pass de composicion cuando el FXAA
    // esta apagado: encender o apagar el efecto en caliente no deja a
    // la offscreen en un layout distinto del que espera la UI o el blit.
    colorAtt.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    VkSubpassDependency deps[2]{};
    // Entrada: espera a que el pass de composicion haya terminado de escribir
    // la imagen intermedia, que es lo unico que muestrea este pass.
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                          | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    // Salida: la UI (o el blit headless) lee la imagen ya suavizada.
    deps[1].srcSubpass      = 0;
    deps[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask   = VK_ACCESS_SHADER_READ_BIT;
    deps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments    = &colorAtt;
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = 2;
    rpInfo.pDependencies   = deps;

    if (vkCreateRenderPass(ctx.gpu.device(), &rpInfo, nullptr, &m_renderPass) != VK_SUCCESS)
        throw std::runtime_error("failed to create aa render pass!");

    // --- Variante del TAA: dos attachments ------------------------------
    // El mismo color va a la vez a la offscreen (que se presenta) y al
    // historial (que se muestrea el frame siguiente). Escribirlo una vez con
    // dos targets ahorra un segundo pass entero sobre toda la pantalla.
    VkAttachmentDescription taaAtts[2] = { colorAtt, colorAtt };
    // El historial no se presenta: sale listo para que lo lea taa.frag.
    taaAtts[1].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference taaRefs[2]{};
    taaRefs[0].attachment = 0;
    taaRefs[0].layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    taaRefs[1].attachment = 1;
    taaRefs[1].layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription taaSubpass{};
    taaSubpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    taaSubpass.colorAttachmentCount = 2;
    taaSubpass.pColorAttachments    = taaRefs;

    VkRenderPassCreateInfo taaRpInfo = rpInfo;
    taaRpInfo.attachmentCount = 2;
    taaRpInfo.pAttachments    = taaAtts;
    taaRpInfo.pSubpasses      = &taaSubpass;

    if (vkCreateRenderPass(ctx.gpu.device(), &taaRpInfo, nullptr, &m_historyRenderPass) != VK_SUCCESS)
        throw std::runtime_error("failed to create taa render pass!");

    printf("aa render passes OK\n"); fflush(stdout);
}

void AaPass::createPipelines(const Context& ctx)
{
    // Filtrado LINEAL: los tres modos muestrean entre texeles (FXAA a media
    // distancia, SSAA en la rejilla de bajada, TAA en la uv reproyectada) y
    // es de ahi de donde sale el suavizado. Con NEAREST no harian nada.
    VkSamplerCreateInfo si{};
    si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.borderColor  = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    if (vkCreateSampler(ctx.gpu.device(), &si, nullptr, &m_sampler) != VK_SUCCESS)
        throw std::runtime_error("failed to create aa sampler!");

    // --- Layout de un binding: la imagen intermedia. FXAA y SSAA -------
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dsl{};
    dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl.bindingCount = 1;
    dsl.pBindings    = &binding;
    if (vkCreateDescriptorSetLayout(ctx.gpu.device(), &dsl, nullptr, &m_descLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create aa descriptor set layout!");

    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = kFramesInFlight;

    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes    = &poolSize;
    dpi.maxSets       = kFramesInFlight;
    if (vkCreateDescriptorPool(ctx.gpu.device(), &dpi, nullptr, &m_descPool) != VK_SUCCESS)
        throw std::runtime_error("failed to create aa descriptor pool!");

    // --- Layout de tres bindings: color, historial y profundidad. TAA ---
    VkDescriptorSetLayoutBinding taaBindings[3]{};
    for (int i = 0; i < 3; i++)
    {
        taaBindings[i].binding         = (uint32_t)i;
        taaBindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        taaBindings[i].descriptorCount = 1;
        taaBindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo taaDsl{};
    taaDsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    taaDsl.bindingCount = 3;
    taaDsl.pBindings    = taaBindings;
    if (vkCreateDescriptorSetLayout(ctx.gpu.device(), &taaDsl, nullptr, &m_taaDescLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create taa descriptor set layout!");

    VkDescriptorPoolSize taaPoolSize{};
    taaPoolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    taaPoolSize.descriptorCount = kFramesInFlight * 3;

    VkDescriptorPoolCreateInfo taaDpi{};
    taaDpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    taaDpi.poolSizeCount = 1;
    taaDpi.pPoolSizes    = &taaPoolSize;
    taaDpi.maxSets       = kFramesInFlight;
    if (vkCreateDescriptorPool(ctx.gpu.device(), &taaDpi, nullptr, &m_taaDescPool) != VK_SUCCESS)
        throw std::runtime_error("failed to create taa descriptor pool!");

    // Un pipeline layout por modo: las push constants no tienen el mismo
    // tamano y el TAA ademas usa otro descriptor set layout.
    auto makeLayout = [&](VkDescriptorSetLayout setLayout, uint32_t pushSize, VkPipelineLayout& out)
    {
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pcr.offset     = 0;
        pcr.size       = pushSize;

        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &setLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &pcr;
        if (vkCreatePipelineLayout(ctx.gpu.device(), &pli, nullptr, &out) != VK_SUCCESS)
            throw std::runtime_error("failed to create aa pipeline layout!");
    };
    makeLayout(m_descLayout,    (uint32_t)sizeof(FxaaPush), m_fxaaPipelineLayout);
    makeLayout(m_descLayout,    (uint32_t)sizeof(SsaaPush), m_ssaaPipelineLayout);
    makeLayout(m_taaDescLayout, (uint32_t)sizeof(TaaPush),  m_taaPipelineLayout);

    // Mismo vertex shader que la composicion: el triangulo sale de
    // gl_VertexIndex y saca la UV en location 0, que es justo lo que esperan
    // los tres fragment shaders.
    VkShaderModule vertModule = loadShaderModule(ctx.gpu.device(), "shaders/fullscreen.vert.spv");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // El TAA escribe en DOS attachments (pantalla + historial) y el estado de
    // blending tiene que declarar uno por attachment o el pipeline es
    // invalido, aunque los dos sean identicos.
    VkPipelineColorBlendAttachmentState blend[2]{};
    for (int i = 0; i < 2; i++)
        blend[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = blend;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    VkGraphicsPipelineCreateInfo pci{};
    pci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.stageCount          = 2;
    pci.pStages             = stages;
    pci.pVertexInputState   = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState      = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState   = &ms;
    // Sin pDepthStencilState: ningun subpass de resolucion declara attachment
    // de profundidad, asi que Vulkan permite (y espera) un puntero nulo.
    pci.pDepthStencilState  = nullptr;
    pci.pColorBlendState    = &cb;
    pci.pDynamicState       = &dyn;
    pci.subpass             = 0;

    // Los tres pipelines comparten TODO el estado fijo: solo cambian el
    // fragment shader, el pipeline layout y (en el TAA) el render pass y el
    // numero de attachments.
    auto makePipeline = [&](const char* spv, VkPipelineLayout layout,
                            VkRenderPass pass, uint32_t attachments, VkPipeline& out)
    {
        VkShaderModule module = loadShaderModule(ctx.gpu.device(), spv);
        stages[1].module   = module;
        cb.attachmentCount = attachments;
        pci.layout         = layout;
        pci.renderPass     = pass;
        if (vkCreateGraphicsPipelines(ctx.gpu.device(), VK_NULL_HANDLE, 1, &pci, nullptr, &out) != VK_SUCCESS)
            throw std::runtime_error(std::string("failed to create graphics pipeline: ") + spv);
        vkDestroyShaderModule(ctx.gpu.device(), module, nullptr);
    };

    makePipeline("shaders/fxaa.frag.spv",         m_fxaaPipelineLayout, m_renderPass,        1, m_fxaaPipeline);
    makePipeline("shaders/ssaa_resolve.frag.spv", m_ssaaPipelineLayout, m_renderPass,        1, m_ssaaPipeline);
    makePipeline("shaders/taa.frag.spv",          m_taaPipelineLayout,  m_historyRenderPass, 2, m_taaPipeline);

    vkDestroyShaderModule(ctx.gpu.device(), vertModule, nullptr);
}

void AaPass::destroyPipelinesAndRenderPasses(const Context& ctx)
{
    vkDestroyPipeline(ctx.gpu.device(), m_fxaaPipeline, nullptr);
    vkDestroyPipeline(ctx.gpu.device(), m_ssaaPipeline, nullptr);
    vkDestroyPipeline(ctx.gpu.device(), m_taaPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.gpu.device(), m_fxaaPipelineLayout, nullptr);
    vkDestroyPipelineLayout(ctx.gpu.device(), m_ssaaPipelineLayout, nullptr);
    vkDestroyPipelineLayout(ctx.gpu.device(), m_taaPipelineLayout, nullptr);
    vkDestroyDescriptorPool(ctx.gpu.device(), m_descPool, nullptr);
    vkDestroyDescriptorPool(ctx.gpu.device(), m_taaDescPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.gpu.device(), m_descLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.gpu.device(), m_taaDescLayout, nullptr);
    vkDestroySampler(ctx.gpu.device(), m_sampler, nullptr);
    vkDestroyRenderPass(ctx.gpu.device(), m_renderPass, nullptr);
    vkDestroyRenderPass(ctx.gpu.device(), m_historyRenderPass, nullptr);
    m_renderPass        = VK_NULL_HANDLE;
    m_historyRenderPass = VK_NULL_HANDLE;
}

void AaPass::createImages(const Context& ctx)
{
    if (!needsIntermediate(ctx.activeMode)) return;

    const bool taa = (ctx.activeMode == AaMode::Taa);

    for (int f = 0; f < kFramesInFlight; f++)
    {
        // Destino alternativo de la composicion. Tiene el tamano INTERNO del
        // render, que en SSAA no es el de la ventana. COLOR_ATTACHMENT porque
        // es un target de render, SAMPLED porque el pass de resolucion lo lee.
        ctx.res.createImage(
            ctx.renderExtent.width, ctx.renderExtent.height,
            ctx.swapChainFormat, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            m_srcImage[f], m_srcMemory[f]);
        ctx.res.createTextureImageView(m_srcImage[f], m_srcView[f], ctx.swapChainFormat);

        // Framebuffer del pass de COMPOSICION apuntando aqui, con el mismo
        // depth compartido que el framebuffer de siempre: el contorno y los
        // gizmos siguen cargando la profundidad de la escena.
        VkImageView compAtts[] = { m_srcView[f], ctx.sceneDepthView };
        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass      = ctx.compositeRenderPass;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments    = compAtts;
        fbInfo.width           = ctx.renderExtent.width;
        fbInfo.height          = ctx.renderExtent.height;
        fbInfo.layers          = 1;
        if (vkCreateFramebuffer(ctx.gpu.device(), &fbInfo, nullptr, &m_srcFramebuffer[f]) != VK_SUCCESS)
            throw std::runtime_error("failed to create aa source framebuffer!");

        if (taa)
        {
            // Historial: mismo formato y tamano que la imagen que se
            // presenta. TRANSFER_DST no se usa para copiar nada: es el
            // requisito de la transicion inicial de layout de aqui abajo.
            ctx.res.createImage(
                ctx.viewport.width, ctx.viewport.height,
                ctx.swapChainFormat, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                m_historyImage[f], m_historyMemory[f]);
            ctx.res.createTextureImageView(m_historyImage[f], m_historyView[f], ctx.swapChainFormat);

            // El historial se MUESTREA antes de escribirse: el primer frame
            // de cada slot (y el primero tras cada resize) lo lee todavia
            // recien creado. taa.frag descarta ese contenido por
            // historyValid, pero el descriptor lo declara en
            // SHADER_READ_ONLY y la capa de validacion exige que la imagen
            // este de verdad en ese layout, no en UNDEFINED. Se pasa por
            // TRANSFER_DST porque es la unica cadena que admite
            // transitionImageLayout; no se copia nada.
            ctx.res.transitionImageLayout(m_historyImage[f],
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            ctx.res.transitionImageLayout(m_historyImage[f],
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

            // Un solo pass escribe los dos: la imagen que se presenta y el
            // historial que se leera el frame siguiente.
            VkImageView taaAtts[] = { ctx.offscreenView[f], m_historyView[f] };
            VkFramebufferCreateInfo taaFb = fbInfo;
            taaFb.renderPass      = m_historyRenderPass;
            taaFb.attachmentCount = 2;
            taaFb.pAttachments    = taaAtts;
            taaFb.width           = ctx.viewport.width;
            taaFb.height          = ctx.viewport.height;
            if (vkCreateFramebuffer(ctx.gpu.device(), &taaFb, nullptr, &m_historyFramebuffer[f]) != VK_SUCCESS)
                throw std::runtime_error("failed to create taa framebuffer!");
        }
        else
        {
            // Framebuffer del pass de resolucion: escribe en la offscreen de
            // siempre, que es la que muestrea la UI y la que blitea el
            // runtime headless. Va a tamano de VENTANA aunque la fuente sea
            // mayor: eso es exactamente el downsample del SSAA.
            VkFramebufferCreateInfo outFb = fbInfo;
            outFb.renderPass      = m_renderPass;
            outFb.attachmentCount = 1;
            outFb.pAttachments    = &ctx.offscreenView[f];
            outFb.width           = ctx.viewport.width;
            outFb.height          = ctx.viewport.height;
            if (vkCreateFramebuffer(ctx.gpu.device(), &outFb, nullptr, &m_framebuffer[f]) != VK_SUCCESS)
                throw std::runtime_error("failed to create aa framebuffer!");
        }
    }

    // Los sets de la vez anterior apuntan a vistas ya destruidas: reset y no
    // free, igual que en el bloom, el SSAO y el SSR.
    vkResetDescriptorPool(ctx.gpu.device(), m_descPool, 0);
    if (taa) vkResetDescriptorPool(ctx.gpu.device(), m_taaDescPool, 0);

    for (int f = 0; f < kFramesInFlight; f++)
    {
        if (taa)
        {
            VkDescriptorSetAllocateInfo ai{};
            ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool     = m_taaDescPool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts        = &m_taaDescLayout;
            if (vkAllocateDescriptorSets(ctx.gpu.device(), &ai, &m_taaSets[f]) != VK_SUCCESS)
                throw std::runtime_error("failed to allocate taa descriptor set!");

            VkDescriptorImageInfo infos[3]{};
            infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            infos[0].imageView   = m_srcView[f];
            infos[0].sampler     = m_sampler;
            // El historial que se LEE es el del otro slot: el que escribio el
            // frame anterior. Con kFramesInFlight = 2 alternan solos.
            infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            infos[1].imageView   = m_historyView[(f + 1) % kFramesInFlight];
            infos[1].sampler     = m_sampler;
            // Profundidad del depth pre-pass, que ya sale en el layout de
            // lectura y se graba sin jitter (es la geometrica).
            infos[2].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            infos[2].imageView   = ctx.prepassDepthView[f];
            infos[2].sampler     = ctx.prepassDepthSampler;

            VkWriteDescriptorSet writes[3]{};
            for (int i = 0; i < 3; i++)
            {
                writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet          = m_taaSets[f];
                writes[i].dstBinding      = (uint32_t)i;
                writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].descriptorCount = 1;
                writes[i].pImageInfo      = &infos[i];
            }
            vkUpdateDescriptorSets(ctx.gpu.device(), 3, writes, 0, nullptr);
            continue;
        }

        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = m_descPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &m_descLayout;
        if (vkAllocateDescriptorSets(ctx.gpu.device(), &ai, &m_sets[f]) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate aa descriptor set!");

        VkDescriptorImageInfo info{};
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        info.imageView   = m_srcView[f];
        info.sampler     = m_sampler;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = m_sets[f];
        write.dstBinding      = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo      = &info;
        vkUpdateDescriptorSets(ctx.gpu.device(), 1, &write, 0, nullptr);
    }
}

void AaPass::destroyImages(const Context& ctx)
{
    auto destroyImage = [&](VkImage& image, VkDeviceMemory& memory, VkImageView& view)
    {
        if (view)   { vkDestroyImageView(ctx.gpu.device(), view, nullptr);  view   = VK_NULL_HANDLE; }
        if (image)  { vkDestroyImage(ctx.gpu.device(), image, nullptr);     image  = VK_NULL_HANDLE; }
        if (memory) { vkFreeMemory(ctx.gpu.device(), memory, nullptr);      memory = VK_NULL_HANDLE; }
    };

    for (int f = 0; f < kFramesInFlight; f++)
    {
        if (m_framebuffer[f])
        {
            vkDestroyFramebuffer(ctx.gpu.device(), m_framebuffer[f], nullptr);
            m_framebuffer[f] = VK_NULL_HANDLE;
        }
        if (m_srcFramebuffer[f])
        {
            vkDestroyFramebuffer(ctx.gpu.device(), m_srcFramebuffer[f], nullptr);
            m_srcFramebuffer[f] = VK_NULL_HANDLE;
        }
        if (m_historyFramebuffer[f])
        {
            vkDestroyFramebuffer(ctx.gpu.device(), m_historyFramebuffer[f], nullptr);
            m_historyFramebuffer[f] = VK_NULL_HANDLE;
        }
        destroyImage(m_srcImage[f],     m_srcMemory[f],     m_srcView[f]);
        destroyImage(m_historyImage[f], m_historyMemory[f], m_historyView[f]);
        m_sets[f]    = VK_NULL_HANDLE;
        m_taaSets[f] = VK_NULL_HANDLE;
    }
    // El historial que quede es de un tamano o un modo que ya no existe.
    m_historyValid = false;
}

void AaPass::updateFrameMatrices(const Context& ctx, const glm::mat4& view, const glm::mat4& proj)
{
    // El relevo prev<-curr se hace aqui y TODOS los frames porque el motion
    // blur tambien reproyecta con estas dos, y corre con el TAA apagado. La
    // linea equivalente del final del pass del TAA escribe exactamente el
    // mismo valor: con el TAA activo esto es redundante, no un cambio.
    m_prevViewProj = m_currViewProj;
    m_currViewProj = proj * view;
    m_jitteredProj = proj;
    if (ctx.activeMode != AaMode::Taa) return;

    // Secuencia y aplicacion en TaaJitter.h, compartidas con D3D12: estaban
    // escritas dos veces y descuadrarlas no da error, solo hace converger el
    // TAA a una imagen distinta segun el backend.
    m_jitter = taaJitterPixels(m_jitterIndex, ctx.state.taaJitterScale());
    applyTaaJitter(m_jitteredProj, m_jitter,
                   (float)ctx.renderExtent.width, (float)ctx.renderExtent.height);
}

void AaPass::record(const Context& ctx, VkCommandBuffer cmd)
{
    if (!needsIntermediate(ctx.activeMode))
    {
        // None y MSAA no tienen pass propio. En MSAA el resolve ocurre dentro
        // del pass de composicion y su coste sale en renderGpuMs(); en None no
        // hay ni un comando de mas: la composicion ya escribio directamente en
        // la offscreen y la dejo en SHADER_READ_ONLY, que es exactamente
        // lo que esperan la UI y el blit headless.
        m_passStamped[ctx.currentFrame] = false;
        return;
    }

    const bool taa = (ctx.activeMode == AaMode::Taa);
    const VkFramebuffer fb = taa ? m_historyFramebuffer[ctx.currentFrame]
                                 : m_framebuffer[ctx.currentFrame];
    // Red de seguridad: el modo activo y los recursos construidos van
    // siempre a la par (activeMode solo cambia dentro de
    // rebuildAaResources), pero grabar un render pass con un framebuffer
    // nulo mata el proceso. Si algun dia se vuelven a desincronizar, se
    // pierde el anti-aliasing de un frame en vez de la aplicacion entera.
    if (fb == VK_NULL_HANDLE)
    {
        m_passStamped[ctx.currentFrame] = false;
        return;
    }

    if (ctx.timestampsSupported)
    {
        // El pool ya lo reseteo el arranque del frame: aqui solo se escribe.
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, ctx.queryPool, ctx.currentFrame * 4);
        m_passStamped[ctx.currentFrame] = true;
    }

    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass        = taa ? m_historyRenderPass : m_renderPass;
    rpInfo.framebuffer       = fb;
    // Tamano de VENTANA, no de render: este pass es justo el que baja de la
    // resolucion interna a la de presentacion.
    rpInfo.renderArea.extent = ctx.viewport;
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.clearValueCount   = 0;   // los attachments son DONT_CARE

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width    = (float)ctx.viewport.width;
    viewport.height   = (float)ctx.viewport.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = ctx.viewport;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    switch (ctx.activeMode)
    {
    case AaMode::Fxaa:
    {
        FxaaPush push{};
        // invRes de la imagen que se MUESTREA. En FXAA la intermedia tiene el
        // tamano de la ventana, pero se toma del extent interno igualmente
        // para que el shader no dependa de que ambos coincidan.
        push.invResX          = 1.0f / (float)ctx.renderExtent.width;
        push.invResY          = 1.0f / (float)ctx.renderExtent.height;
        push.subpix           = ctx.state.fxaaSubpix();
        push.edgeThreshold    = ctx.state.fxaaEdgeThreshold();
        push.edgeThresholdMin = ctx.state.fxaaEdgeThresholdMin();

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_fxaaPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_fxaaPipelineLayout,
                                0, 1, &m_sets[ctx.currentFrame], 0, nullptr);
        vkCmdPushConstants(cmd, m_fxaaPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
        break;
    }
    case AaMode::Ssaa:
    {
        SsaaPush push{};
        push.invSrcX = 1.0f / (float)ctx.renderExtent.width;
        push.invSrcY = 1.0f / (float)ctx.renderExtent.height;
        // Una muestra por texel de origen y por eje: a factor 2 son los 4
        // texeles que caen dentro del pixel de destino, que es exactamente el
        // promedio que define el supersampling.
        push.taps    = (int32_t)std::lround((double)ctx.ssaaFactor);
        if (push.taps < 1) push.taps = 1;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ssaaPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ssaaPipelineLayout,
                                0, 1, &m_sets[ctx.currentFrame], 0, nullptr);
        vkCmdPushConstants(cmd, m_ssaaPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
        break;
    }
    default:   // AaMode::Taa
    {
        TaaPush push{};
        // De clip de este frame a clip del anterior, los dos SIN jitter: es
        // la transformacion geometrica pura, el jitter es ruido de muestreo y
        // meterlo aqui desplazaria el historial medio pixel cada frame.
        push.reproject    = m_prevViewProj * glm::inverse(m_currViewProj);
        push.invResX      = 1.0f / (float)ctx.viewport.width;
        push.invResY      = 1.0f / (float)ctx.viewport.height;
        push.feedback     = ctx.state.taaFeedback();
        push.historyValid = m_historyValid ? 1 : 0;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_taaPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_taaPipelineLayout,
                                0, 1, &m_taaSets[ctx.currentFrame], 0, nullptr);
        vkCmdPushConstants(cmd, m_taaPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
        break;
    }
    }

    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    if (ctx.timestampsSupported)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx.queryPool, ctx.currentFrame * 4 + 1);

    if (taa)
    {
        // A partir del segundo frame ya hay historial que acumular, y la
        // view-proj de este frame pasa a ser la "anterior" del siguiente.
        m_historyValid = true;
        m_prevViewProj = m_currViewProj;
    }
}

} // namespace DonTopo
