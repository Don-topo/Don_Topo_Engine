// Headless test of the shared pipeline state (no GUI, no device).
// GraphicsPipelineState does not call Vulkan: it only fills structs, so the
// values with which ALL the scene pass pipelines are compiled can be
// asserted here. Plain main + asserts, no framework, consistent with
// frustum_tests.cpp.
//
// What is tested are the two things that nothing protected before:
//
//  1. The VALUES. They were written twice (H10) and changing only one of the two
//     places broke no test: the scene looked different depending on whether the
//     mesh was static or skinned, and you had to find it by looking.
//  2. That fill() leaves the pointers pointing INSIDE the object itself. It is the
//     trap of the struct: pAttachments and pDynamicStates point to members, and
//     a pointer to dead memory there gives no validation error at all.
#include "DonTopo/Renderer/PipelineDefaults.h"

#include <cstdio>
#include <type_traits>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// The values with which the scene is drawn. Each one has a specific symptom if it
// changes, which is why they are asserted one by one and not as a block.
static void test_valores()
{
    const GraphicsPipelineState st(VK_SAMPLE_COUNT_4_BIT);

    CHECK(st.inputAssembly.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

    // Viewport and scissor are DYNAMIC: they are counted here but set when
    // recording. A pipeline that declares 1 and does not list them in dynamic requires
    // pointers to structures that the engine does not fill.
    CHECK(st.viewport.viewportCount == 1);
    CHECK(st.viewport.scissorCount  == 1);
    CHECK(st.dynamic.dynamicStateCount == 2);
    CHECK(st.dynamicStates[0] == VK_DYNAMIC_STATE_VIEWPORT);
    CHECK(st.dynamicStates[1] == VK_DYNAMIC_STATE_SCISSOR);

    // Solid fill, back faces culled and CCW winding. The frontFace is the one that
    // went out of sync between backends back in the day: with the opposite one, the
    // scene looks inside out (the interior of the meshes is drawn).
    CHECK(st.rasterization.polygonMode == VK_POLYGON_MODE_FILL);
    CHECK(st.rasterization.cullMode    == VK_CULL_MODE_BACK_BIT);
    CHECK(st.rasterization.frontFace   == VK_FRONT_FACE_COUNTER_CLOCKWISE);
    CHECK(st.rasterization.lineWidth   == 1.0f);

    // Depth: test AND write, with LESS. Without the write, what is drawn afterwards
    // covers what is in front.
    CHECK(st.depthStencil.depthTestEnable  == VK_TRUE);
    CHECK(st.depthStencil.depthWriteEnable == VK_TRUE);
    CHECK(st.depthStencil.depthCompareOp   == VK_COMPARE_OP_LESS);
    CHECK(st.depthStencil.depthBoundsTestEnable == VK_FALSE);
    CHECK(st.depthStencil.stencilTestEnable     == VK_FALSE);

    // Opaque: all four channels are written and blending is OFF.
    // Turning it on by accident gives no error anywhere; it looks like objects
    // that become transparent to each other.
    CHECK(st.blendAttachment.blendEnable == VK_FALSE);
    CHECK(st.blendAttachment.colorWriteMask ==
          (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT));
    CHECK(st.colorBlend.attachmentCount == 1);
}

// The samples come in as a parameter because the AA mode sets them, and they have
// to match those of the render pass or the pipeline is invalid.
static void test_muestras()
{
    const GraphicsPipelineState una(VK_SAMPLE_COUNT_1_BIT);
    const GraphicsPipelineState ocho(VK_SAMPLE_COUNT_8_BIT);
    CHECK(una.multisample.rasterizationSamples  == VK_SAMPLE_COUNT_1_BIT);
    CHECK(ocho.multisample.rasterizationSamples == VK_SAMPLE_COUNT_8_BIT);
}

// The trap of the struct: the pointers have to point to the members of THIS
// object, not to any temporary. It is checked by address identity, which is
// the only thing that proves it.
static void test_punteros_dentro_del_objeto()
{
    const GraphicsPipelineState st(VK_SAMPLE_COUNT_1_BIT);

    CHECK(st.colorBlend.pAttachments  == &st.blendAttachment);
    CHECK(st.dynamic.pDynamicStates   == st.dynamicStates);

    VkGraphicsPipelineCreateInfo pci{};
    st.fill(pci);

    CHECK(pci.sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    CHECK(pci.pInputAssemblyState == &st.inputAssembly);
    CHECK(pci.pViewportState      == &st.viewport);
    CHECK(pci.pRasterizationState == &st.rasterization);
    CHECK(pci.pMultisampleState   == &st.multisample);
    CHECK(pci.pDepthStencilState  == &st.depthStencil);
    CHECK(pci.pColorBlendState    == &st.colorBlend);
    CHECK(pci.pDynamicState       == &st.dynamic);
    CHECK(pci.subpass             == 0);

    // What fill() does NOT touch, because it is exactly what distinguishes each
    // pipeline: shaders, vertex input, layout and render pass. If it touched them, it
    // would overwrite what the caller had already set.
    CHECK(pci.pStages           == nullptr);
    CHECK(pci.pVertexInputState == nullptr);
    CHECK(pci.layout            == VK_NULL_HANDLE);
    CHECK(pci.renderPass        == VK_NULL_HANDLE);
}

// Copying the object would carry the pointers along still pointing at the original,
// and if that one dies before the pipeline is created, Vulkan reads dead memory
// without the validation layer saying a word. Not compiling is the only way to
// prevent it.
static_assert(!std::is_copy_constructible<GraphicsPipelineState>::value,
              "GraphicsPipelineState no puede ser copiable: sus punteros internos "
              "apuntan a miembros propios");
static_assert(!std::is_copy_assignable<GraphicsPipelineState>::value,
              "GraphicsPipelineState no puede ser asignable por copia");

int main()
{
    test_valores();
    test_muestras();
    test_punteros_dentro_del_objeto();

    if (g_failures == 0) std::printf("pipeline_defaults_tests: OK\n");
    else                 std::printf("pipeline_defaults_tests: %d FALLOS\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
