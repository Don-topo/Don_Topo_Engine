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
    // Los dos shaders son los MISMOS para estaticas y con huesos: lo unico que
    // cambia entre los dos pares es de donde salen las posiciones y las
    // normales, y eso viaja en el vertex input.
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

    // Solo posicion y normal: outline.vert no consume color, uv ni tangent, y
    // declararlos hace saltar el aviso "Vertex attribute at location N not
    // consumed by vertex shader" de la capa de validacion. Mismo binding y
    // mismos offsets que el pipeline del que sale la plantilla — solo se
    // describen menos atributos, el buffer que se bindea es el mismo.
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
    // El contorno se dibuja en el pass de composicion (ya en LDR) y no en el de
    // escena: por el pass HDR, el tonemap le cambiaria el naranja plano. Alli el
    // skybox ya esta dibujado, asi que el depthWrite del casco deja de hacer
    // falta para taparlo — se queda como venga en la plantilla, que no molesta.
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
    // Los modulos ya no hacen falta: los pipelines se quedan con lo suyo. Se
    // sueltan ANTES de lanzar para no fugarlos en el camino de error.
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
