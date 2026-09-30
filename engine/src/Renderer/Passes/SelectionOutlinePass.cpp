#include "DonTopo/Renderer/Passes/SelectionOutlinePass.h"

#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/ShaderModule.h"

#include <stdexcept>

namespace DonTopo {

void SelectionOutlinePass::crearPar(const Context& ctx,
                                    const VkGraphicsPipelineCreateInfo& plantilla,
                                    const VkPipelineRasterizationStateCreateInfo& rasterizacion,
                                    const VkPipelineVertexInputStateCreateInfo& vertexInput,
                                    uint32_t posOffset, uint32_t normalOffset,
                                    VkPipeline& relleno, VkPipeline& wireframe,
                                    const char* queSon)
{
    // The two shaders are the SAME for static and bone meshes: the only thing that
    // changes between the two pairs is where the positions and the
    // normals come from, and that travels in the vertex input.
    VkShaderModule vert = loadShaderModule(ctx.gpu.device(), "shaders/outline.vert.spv");
    VkShaderModule frag = loadShaderModule(ctx.gpu.device(), "shaders/outline.frag.spv");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName  = "main";

    VkPipelineRasterizationStateCreateInfo rs = rasterizacion;
    rs.cullMode = kCullMode;

    // Only position and normal: outline.vert does not consume color, uv or tangent, and
    // declaring them triggers the "Vertex attribute at location N not
    // consumed by vertex shader" warning of the validation layer. Same binding and
    // same offsets as the pipeline the template comes from (only fewer attributes
    // are described, the buffer that is bound is the same).
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, posOffset };
    attrs[1] = { 3, 0, VK_FORMAT_R32G32B32_SFLOAT, normalOffset };

    VkPipelineVertexInputStateCreateInfo vi = vertexInput;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions    = attrs;

    VkGraphicsPipelineCreateInfo pci = plantilla;
    pci.pStages             = stages;
    pci.pRasterizationState = &rs;
    pci.pVertexInputState   = &vi;
    pci.layout              = ctx.pipelineLayout;
    // The outline is drawn in the composition pass (already in LDR) and not in the
    // scene one: through the HDR pass, the tonemap would change its flat orange. There the
    // skybox is already drawn, so the hull's depthWrite is no longer needed
    // to cover it; it is left as it comes in the template, which does no harm.
    pci.renderPass          = ctx.compositeRenderPass;

    VkResult r = vkCreateGraphicsPipelines(ctx.gpu.device(), VK_NULL_HANDLE, 1, &pci,
                                           nullptr, &relleno);
    if (r != VK_SUCCESS)
    {
        vkDestroyShaderModule(ctx.gpu.device(), vert, nullptr);
        vkDestroyShaderModule(ctx.gpu.device(), frag, nullptr);
        throw std::runtime_error(std::string("failed to create ") + queSon +
                                 " outline graphics pipeline!");
    }

    VkPipelineRasterizationStateCreateInfo rsWire = rs;
    rsWire.polygonMode = VK_POLYGON_MODE_LINE;

    VkGraphicsPipelineCreateInfo pciWire = pci;
    pciWire.pRasterizationState = &rsWire;

    r = vkCreateGraphicsPipelines(ctx.gpu.device(), VK_NULL_HANDLE, 1, &pciWire,
                                  nullptr, &wireframe);
    // The modules are no longer needed: the pipelines keep what is theirs. They are
    // released BEFORE throwing so as not to leak them on the error path.
    vkDestroyShaderModule(ctx.gpu.device(), vert, nullptr);
    vkDestroyShaderModule(ctx.gpu.device(), frag, nullptr);
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string("failed to create ") + queSon +
                                 " outline wireframe graphics pipeline!");
}

void SelectionOutlinePass::createStaticPipelines(
    const Context& ctx,
    const VkGraphicsPipelineCreateInfo& plantilla,
    const VkPipelineRasterizationStateCreateInfo& rasterizacion,
    const VkPipelineVertexInputStateCreateInfo& vertexInput,
    uint32_t posOffset, uint32_t normalOffset)
{
    crearPar(ctx, plantilla, rasterizacion, vertexInput, posOffset, normalOffset,
             m_static, m_staticWire, "static");
}

void SelectionOutlinePass::createSkinnedPipelines(
    const Context& ctx,
    const VkGraphicsPipelineCreateInfo& plantilla,
    const VkPipelineRasterizationStateCreateInfo& rasterizacion,
    const VkPipelineVertexInputStateCreateInfo& vertexInput,
    uint32_t posOffset, uint32_t normalOffset)
{
    crearPar(ctx, plantilla, rasterizacion, vertexInput, posOffset, normalOffset,
             m_skinned, m_skinnedWire, "skinned");
}

void SelectionOutlinePass::destroyResources(const Context& ctx)
{
    auto suelta = [&](VkPipeline& p) {
        if (p != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(ctx.gpu.device(), p, nullptr);
            p = VK_NULL_HANDLE;
        }
    };
    suelta(m_static);
    suelta(m_staticWire);
    suelta(m_skinned);
    suelta(m_skinnedWire);
}

} // namespace DonTopo
