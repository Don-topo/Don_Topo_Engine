#include "DonTopo/Renderer/Passes/DepthPrepassPass.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/GpuResources.h"
#include "DonTopo/Renderer/Vertex.h"
#include <stdexcept>
#include <fstream>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>

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

// ── Depth pre-pass ──────────────────────────────────────────────────────────
void DepthPrepassPass::createRenderPassAndPipeline(const Context& ctx)
{
    // NEAREST: ni D32_SFLOAT ni R32_SFLOAT tienen garantizado el filtrado
    // lineal, y los dos shaders muestrean a texel exacto. CLAMP_TO_EDGE para
    // que los taps del borde no traigan profundidad del lado opuesto.
    VkSamplerCreateInfo si{};
    si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter    = VK_FILTER_NEAREST;
    si.minFilter    = VK_FILTER_NEAREST;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(ctx.gpu.device(), &si, nullptr, &m_sampler) != VK_SUCCESS)
        throw std::runtime_error("failed to create ssao sampler!");

    // --- Render pass del depth pre-pass (solo profundidad) ---------------
    VkAttachmentDescription depthAtt{};
    depthAtt.format         = VK_FORMAT_D32_SFLOAT;
    depthAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
    depthAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    depthAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    // READ_ONLY: en cuanto acaba el pass, el compute del SSAO la muestrea.
    depthAtt.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 0;
    depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency deps[2]{};
    // Entrada: el compute del frame anterior en este mismo slot pudo estar
    // leyendo esta imagen.
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    // Salida: ssao.comp muestrea la profundidad recién escrita.
    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments    = &depthAtt;
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = 2;
    rpInfo.pDependencies   = deps;
    if (vkCreateRenderPass(ctx.gpu.device(), &rpInfo, nullptr, &m_renderPass) != VK_SUCCESS)
        throw std::runtime_error("failed to create ssao depth render pass!");

    // --- Pipeline del pre-pass (vertex-only, como el de sombras) ---------
    auto vertCode = loadSpv("shaders/depth_prepass.vert.spv");
    VkShaderModule vertModule = makeModule(ctx.gpu.device(), vertCode);

    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    vertStage.pName  = "main";

    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding   = 0;
    bindingDesc.stride    = sizeof(Vertex);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrDesc{};
    attrDesc.binding  = 0;
    attrDesc.location = 0;
    attrDesc.format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrDesc.offset   = offsetof(Vertex, pos);

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions    = &attrDesc;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode    = VK_CULL_MODE_NONE;
    rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth   = 1.0f;
    // Sin depthBias, al reves que el pass de sombras: esta profundidad no se
    // compara contra nada, se reconstruye a posicion. Un sesgo aqui movería
    // la geometría en Z y el AO saldría despegado del contacto.

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 0;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynStates;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount          = 1;
    pipelineInfo.pStages             = &vertStage;
    pipelineInfo.pVertexInputState   = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState   = &multisampling;
    pipelineInfo.pDepthStencilState  = &depthStencil;
    pipelineInfo.pColorBlendState    = &colorBlend;
    pipelineInfo.pDynamicState       = &dynamicState;
    // Prestado del pass de sombras: mismos dos sets (objeto + SSBO de
    // instancias) y mismo rango de push constants, que este shader no usa.
    pipelineInfo.layout              = ctx.shadowPipelineLayout;
    pipelineInfo.renderPass          = m_renderPass;
    if (vkCreateGraphicsPipelines(ctx.gpu.device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS)
        throw std::runtime_error("failed to create ssao depth pipeline!");
    vkDestroyShaderModule(ctx.gpu.device(), vertModule, nullptr);
}

void DepthPrepassPass::destroyRenderPassAndPipeline(const Context& ctx)
{
    vkDestroyPipeline(ctx.gpu.device(), m_pipeline, nullptr);
    vkDestroySampler(ctx.gpu.device(), m_sampler, nullptr);
    vkDestroyRenderPass(ctx.gpu.device(), m_renderPass, nullptr);
    m_renderPass = VK_NULL_HANDLE;
}

void DepthPrepassPass::createImages(const Context& ctx)
{
    for (int f = 0; f < kFramesInFlight; f++)
    {
        // Depth del pre-pass. SAMPLED ademas de ATTACHMENT: lo muestrea
        // ssao.comp.
        ctx.res.createImage(
            ctx.renderExtent.width, ctx.renderExtent.height,
            VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            m_image[f], m_memory[f]);

        // Inline y no createTextureImageView: esa fija el aspecto a COLOR.
        VkImageViewCreateInfo dvi{};
        dvi.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        dvi.image                           = m_image[f];
        dvi.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        dvi.format                          = VK_FORMAT_D32_SFLOAT;
        dvi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        dvi.subresourceRange.levelCount     = 1;
        dvi.subresourceRange.layerCount     = 1;
        if (vkCreateImageView(ctx.gpu.device(), &dvi, nullptr, &m_view[f]) != VK_SUCCESS)
            throw std::runtime_error("failed to create ssao depth view!");

        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass      = m_renderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments    = &m_view[f];
        fbInfo.width           = ctx.renderExtent.width;
        fbInfo.height          = ctx.renderExtent.height;
        fbInfo.layers          = 1;
        if (vkCreateFramebuffer(ctx.gpu.device(), &fbInfo, nullptr, &m_fb[f]) != VK_SUCCESS)
            throw std::runtime_error("failed to create ssao depth framebuffer!");
    }
}

void DepthPrepassPass::destroyImages(const Context& ctx)
{
    for (int f = 0; f < kFramesInFlight; f++)
    {
        if (m_fb[f])
        {
            vkDestroyFramebuffer(ctx.gpu.device(), m_fb[f], nullptr);
            m_fb[f] = VK_NULL_HANDLE;
        }
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
    }
}

void DepthPrepassPass::begin(const Context& ctx, VkCommandBuffer cmd)
{
    VkClearValue clearDepth{};
    clearDepth.depthStencil = { 1.0f, 0 };

    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass        = m_renderPass;
    rpInfo.framebuffer       = m_fb[ctx.currentFrame];
    rpInfo.renderArea.extent = ctx.renderExtent;
    rpInfo.renderArea.offset = { 0, 0 };
    rpInfo.clearValueCount   = 1;
    rpInfo.pClearValues      = &clearDepth;

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{};
    vp.width    = (float)ctx.renderExtent.width;
    vp.height   = (float)ctx.renderExtent.height;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = ctx.renderExtent;
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
}

void DepthPrepassPass::end(VkCommandBuffer cmd)
{
    vkCmdEndRenderPass(cmd);
}

} // namespace DonTopo
