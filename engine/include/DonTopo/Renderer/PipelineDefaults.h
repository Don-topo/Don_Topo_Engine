#pragma once
#include <vulkan/vulkan.h>

namespace DonTopo {

// The pipeline state shared by ALL the graphics pipelines of the scene
// pass: the two for static meshes, the two for bone meshes and, through
// the template, the four of the selection outline.
//
// It was written twice (createPipeline and createSkinnedGraphicsPipelines)
// with the same values and different variable names (H10). They were compared
// before unifying, same criterion as in H3 and H75, and there was only ONE
// apparent difference: the static block set depthBoundsTestEnable and stencilTestEnable to
// VK_FALSE explicitly and the bone one did not. Both are declared with `{}`, which
// leaves them at 0 = VK_FALSE, so they are equivalent: there was no
// deliberate divergence to preserve.
//
// What does NOT go in here is what really distinguishes the two: the vertex input
// (the engine Vertex stride against the 80 bytes of the skinning compute
// output, with its five attributes at different offsets) and the shaders.
struct GraphicsPipelineState {
    // Triangles, dynamic viewport and scissor (set when recording), solid
    // fill with back-face culling and CCW winding, LESS depth with
    // write, and opaque without blending.
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

        // Set by the AA mode, and it MUST match the number of samples
        // of the render pass against which the pipeline is compiled (scene for the
        // meshes, composition for the outline) or the pipeline is invalid.
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

    // NOT copyable, and it is not a whim: colorBlend.pAttachments points to
    // blendAttachment and dynamic.pDynamicStates to dynamicStates, that is, to
    // members of THIS object. A copy would carry the pointers still pointing to the
    // original, and if the original dies before the pipeline is created, Vulkan reads
    // dead memory. That does NOT give a validation error: it shows up later and
    // elsewhere, just like a pipeline compiled against an already
    // destroyed render pass. Forbidding it makes it impossible instead of documenting it.
    GraphicsPipelineState(const GraphicsPipelineState&)            = delete;
    GraphicsPipelineState& operator=(const GraphicsPipelineState&) = delete;

    // Hooks into `pci` the pointers to the members of this object. What the
    // caller sets afterwards (shaders, vertex input, layout and render pass)
    // is precisely what is NOT common.
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
