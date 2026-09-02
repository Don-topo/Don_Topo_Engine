// Test headless del estado de pipeline compartido (sin GUI, sin device).
// GraphicsPipelineState no llama a Vulkan: solo rellena structs, asi que los
// valores con los que se compilan TODOS los pipelines del pass de escena se
// pueden afirmar aqui. Plain main + asserts, sin framework, coherente con
// frustum_tests.cpp.
//
// Lo que se prueba son las dos cosas que antes no protegia nada:
//
//  1. Los VALORES. Estaban escritos dos veces (H10) y cambiar uno solo de los
//     dos sitios no rompia ningun test: la escena se veia distinta segun la
//     malla fuera estatica o con huesos, y habia que dar con ello mirando.
//  2. Que fill() deje los punteros apuntando DENTRO del propio objeto. Es la
//     trampa del struct: pAttachments y pDynamicStates apuntan a miembros, y
//     un puntero a memoria muerta ahi no da error de validacion ninguno.
#include "DonTopo/Renderer/PipelineDefaults.h"

#include <cstdio>
#include <type_traits>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Los valores con los que se dibuja la escena. Cada uno tiene un sintoma
// concreto si cambia, y por eso se afirman de uno en uno y no en bloque.
static void test_valores()
{
    const GraphicsPipelineState st(VK_SAMPLE_COUNT_4_BIT);

    CHECK(st.inputAssembly.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

    // Viewport y scissor son DINAMICOS: se cuentan aqui pero se fijan al
    // grabar. Un pipeline que declare 1 y no los liste en dynamic exige
    // punteros a estructuras que el motor no rellena.
    CHECK(st.viewport.viewportCount == 1);
    CHECK(st.viewport.scissorCount  == 1);
    CHECK(st.dynamic.dynamicStateCount == 2);
    CHECK(st.dynamicStates[0] == VK_DYNAMIC_STATE_VIEWPORT);
    CHECK(st.dynamicStates[1] == VK_DYNAMIC_STATE_SCISSOR);

    // Relleno solido, caras traseras fuera y winding CCW. El frontFace es el
    // que se descuadro entre backends en su dia: con el contrario, la escena
    // se ve del reves (se dibuja el interior de las mallas).
    CHECK(st.rasterization.polygonMode == VK_POLYGON_MODE_FILL);
    CHECK(st.rasterization.cullMode    == VK_CULL_MODE_BACK_BIT);
    CHECK(st.rasterization.frontFace   == VK_FRONT_FACE_COUNTER_CLOCKWISE);
    CHECK(st.rasterization.lineWidth   == 1.0f);

    // Profundidad: test Y escritura, con LESS. Sin escritura, lo que se dibuja
    // despues tapa lo de delante.
    CHECK(st.depthStencil.depthTestEnable  == VK_TRUE);
    CHECK(st.depthStencil.depthWriteEnable == VK_TRUE);
    CHECK(st.depthStencil.depthCompareOp   == VK_COMPARE_OP_LESS);
    CHECK(st.depthStencil.depthBoundsTestEnable == VK_FALSE);
    CHECK(st.depthStencil.stencilTestEnable     == VK_FALSE);

    // Opaco: los cuatro canales se escriben y el blending esta APAGADO.
    // Encenderlo sin querer no da error en ningun sitio — se ve como objetos
    // que se transparentan entre si.
    CHECK(st.blendAttachment.blendEnable == VK_FALSE);
    CHECK(st.blendAttachment.colorWriteMask ==
          (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT));
    CHECK(st.colorBlend.attachmentCount == 1);
}

// Las muestras entran por parametro porque las fija el modo de AA, y tienen que
// coincidir con las del render pass o el pipeline es invalido.
static void test_muestras()
{
    const GraphicsPipelineState una(VK_SAMPLE_COUNT_1_BIT);
    const GraphicsPipelineState ocho(VK_SAMPLE_COUNT_8_BIT);
    CHECK(una.multisample.rasterizationSamples  == VK_SAMPLE_COUNT_1_BIT);
    CHECK(ocho.multisample.rasterizationSamples == VK_SAMPLE_COUNT_8_BIT);
}

// La trampa del struct: los punteros tienen que apuntar a los miembros de ESTE
// objeto, no a ningun temporal. Se comprueba por identidad de direccion, que es
// lo unico que lo demuestra.
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

    // Lo que fill() NO toca, porque es justo lo que distingue a cada pipeline:
    // shaders, vertex input, layout y render pass. Si los tocara, sobrescribiria
    // lo que el llamante ya hubiera puesto.
    CHECK(pci.pStages           == nullptr);
    CHECK(pci.pVertexInputState == nullptr);
    CHECK(pci.layout            == VK_NULL_HANDLE);
    CHECK(pci.renderPass        == VK_NULL_HANDLE);
}

// Copiar el objeto se llevaria los punteros apuntando al original, y si ese
// muere antes de crear el pipeline, Vulkan lee memoria muerta sin que la capa de
// validacion diga una palabra. Que no compile es la unica forma de impedirlo.
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
