#pragma once
#include <vulkan/vulkan.h>

namespace DonTopo {

// El estado de pipeline que comparten TODOS los pipelines graficos del pass de
// escena: los dos de mallas estaticas, los dos de mallas con huesos y, a traves
// de la plantilla, los cuatro del contorno de seleccion.
//
// Estaba escrito dos veces —createPipeline y createSkinnedGraphicsPipelines—
// con los mismos valores y distintos nombres de variable (H10). Se compararon
// antes de unificar, mismo criterio que en H3 y H75, y solo habia UNA diferencia
// aparente: el bloque estatico ponia depthBoundsTestEnable y stencilTestEnable a
// VK_FALSE explicitamente y el de huesos no. Los dos se declaran con `{}`, que
// los deja a 0 = VK_FALSE, asi que son equivalentes: no habia ninguna
// divergencia deliberada que preservar.
//
// Lo que NO entra aqui es lo que de verdad distingue a los dos: el vertex input
// (stride del Vertex del motor contra los 80 bytes de la salida del compute de
// skinning, con sus cinco atributos en offsets distintos) y los shaders.
struct GraphicsPipelineState {
    // Triangulos, viewport y scissor dinamicos (se fijan al grabar), relleno
    // solido con culling de caras traseras y winding CCW, profundidad LESS con
    // escritura, y opaco sin blending.
    explicit GraphicsPipelineState(VkSampleCountFlagBits samples)
    {
        inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount  = 1;

        rasterization.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode    = VK_CULL_MODE_BACK_BIT;
        rasterization.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth   = 1.0f;

        // Lo fija el modo de AA, y TIENE que coincidir con el numero de muestras
        // del render pass contra el que se compile el pipeline (escena para las
        // mallas, composicion para el contorno) o el pipeline es invalido.
        multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = samples;

        depthStencil.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable       = VK_TRUE;
        depthStencil.depthWriteEnable      = VK_TRUE;
        depthStencil.depthCompareOp        = VK_COMPARE_OP_LESS;
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.stencilTestEnable     = VK_FALSE;

        blendAttachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments    = &blendAttachment;

        dynamicStates[0] = VK_DYNAMIC_STATE_VIEWPORT;
        dynamicStates[1] = VK_DYNAMIC_STATE_SCISSOR;
        dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates    = dynamicStates;
    }

    // NO copiable, y no es un capricho: colorBlend.pAttachments apunta a
    // blendAttachment y dynamic.pDynamicStates a dynamicStates, o sea a
    // miembros de ESTE objeto. Una copia se llevaria los punteros apuntando al
    // original, y si el original muere antes de crear el pipeline, Vulkan lee
    // memoria muerta. Eso NO da error de validacion: se manifiesta mas tarde y
    // en otro sitio, igual que un pipeline compilado contra un render pass ya
    // destruido. Prohibirlo lo hace imposible en vez de documentarlo.
    GraphicsPipelineState(const GraphicsPipelineState&)            = delete;
    GraphicsPipelineState& operator=(const GraphicsPipelineState&) = delete;

    // Engancha en `pci` los punteros a los miembros de este objeto. Lo que el
    // llamante pone despues —shaders, vertex input, layout y render pass— es
    // justo lo que NO es comun.
    void fill(VkGraphicsPipelineCreateInfo& pci) const
    {
        pci.sType                = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pci.pInputAssemblyState  = &inputAssembly;
        pci.pViewportState       = &viewport;
        pci.pRasterizationState  = &rasterization;
        pci.pMultisampleState    = &multisample;
        pci.pDepthStencilState   = &depthStencil;
        pci.pColorBlendState     = &colorBlend;
        pci.pDynamicState        = &dynamic;
        pci.subpass              = 0;
    }

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    VkPipelineViewportStateCreateInfo      viewport{};
    VkPipelineRasterizationStateCreateInfo rasterization{};
    VkPipelineMultisampleStateCreateInfo   multisample{};
    VkPipelineDepthStencilStateCreateInfo  depthStencil{};
    VkPipelineColorBlendAttachmentState    blendAttachment{};
    VkPipelineColorBlendStateCreateInfo    colorBlend{};
    VkDynamicState                         dynamicStates[2]{};
    VkPipelineDynamicStateCreateInfo       dynamic{};
};

} // namespace DonTopo
