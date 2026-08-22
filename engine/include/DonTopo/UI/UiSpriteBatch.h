#pragma once

// Lote de sprites 2D de la UI de juego.
//
// Dos mitades bien separadas:
//   - build(): CPU pura. Recorre la jerarquía del canvas y saca vértices,
//     índices y lotes. No toca Vulkan, y es lo que ejercitan los tests.
//   - el resto: sube esos datos a los buffers del frame en vuelo y graba los
//     draws DENTRO del pass de composición del Renderer (LDR, ya tonemapeado).
//
// El lote rompe al cambiar de atlas o de scissor, que son los dos únicos
// estados que el draw no puede llevar por vértice.

#include "DonTopo/UI/UiTextureAtlas.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstddef>   // offsetof: la red del layout del push constant
#include <cstdint>
#include <vector>

namespace DonTopo
{
    class GpuDevice;
    class GpuResources;
    class UiCanvas;

    // Posición en PÍXELES de pantalla, con (0,0) arriba a la izquierda: la
    // proyección ortográfica del vertex shader es quien la lleva a NDC.
    struct UiVertex
    {
        glm::vec2 pos{0.0f};
        glm::vec2 uv{0.0f};
        glm::vec4 color{1.0f};

        // Todo lo que distingue un quad de texto de uno de sprite viaja POR
        // VÉRTICE, no por pipeline ni por descriptor: así el texto cae en el
        // mismo lote que el panel que tiene detrás.
        //   params.x = modo: 0 = sprite/color plano, 1 = MSDF
        //   params.y = screenPxRange YA escalado al tamaño de este quad
        //   params.z = grosor del outline en píxeles de pantalla
        //   effect   = color del outline (con la opacidad del árbol ya aplicada)
        // Con params.x = 0 el shader hace literalmente lo de siempre.
        glm::vec4 params{0.0f};
        glm::vec4 effect{0.0f};
    };

    struct UiScissor
    {
        int32_t  x = 0;
        int32_t  y = 0;
        uint32_t width  = 0;
        uint32_t height = 0;

        bool empty() const { return width == 0 || height == 0; }
        bool operator==(const UiScissor& o) const
        {
            return x == o.x && y == o.y && width == o.width && height == o.height;
        }
        bool operator!=(const UiScissor& o) const { return !(*this == o); }
    };

    // Bump allocator puro: aparta `count` elementos a partir de `cursor`,
    // avanza el cursor y devuelve DÓNDE empezaba. Es la aritmética que separa
    // el offset de escritura de un canvas dentro del buffer COMPARTIDO del
    // frame (N canvas, un solo VkBuffer). Sin esto — o bindeando siempre en el
    // offset 0, que es lo que hacía con un canvas único — el draw de cada
    // canvas pisa al anterior, y como la GPU lee el buffer al EJECUTAR y no al
    // GRABAR, los N draws del frame salen todos con la geometría del ÚLTIMO,
    // sin que ninguna capa de validación lo diga. Se extrae aparte y libre de
    // Vulkan justo para poder probar esta cuenta sin GPU (ui_batch_tests.cpp).
    inline uint32_t bumpUiCursor(uint32_t& cursor, uint32_t count)
    {
        const uint32_t at = cursor;
        cursor += count;
        return at;
    }

    // Guarda de capacidad: ¿cabe [base, base+count) dentro de `capacity`?
    // record() la comprueba antes de CADA memcpy. La invariante "beginFrame()
    // se llamó este frame, exactamente una vez, con el total exacto" no la
    // impone el tipo — hoy la sostiene que hay un único llamador (Renderer).
    // En cuanto exista un segundo (los canvas de mundo, en el pase de escena,
    // en otro bucle), un record() sin su beginFrame, uno llamado dos veces, o
    // un total que se quedó corto, escribiría FUERA de la memoria mapeada:
    // una escritura de HOST que ninguna capa de validación ve — no hay
    // device lost, no hay error de Vulkan, solo corrupción silenciosa. Mejor
    // no dibujar ese canvas que corromper el buffer. Suma en 64 bits porque
    // `base + count` en 32 bits podría desbordar cerca de UINT32_MAX (nunca
    // pasa con tamaños reales de UI, pero la guarda no depende de que nadie
    // se acuerde de eso).
    inline bool uiCursorFits(uint32_t base, uint32_t count, uint32_t capacity)
    {
        return (uint64_t)base + (uint64_t)count <= (uint64_t)capacity;
    }

    // El bloque de push constants de ui.vert/ui.frag, en C++. Tiene que decir
    // EXACTAMENTE lo mismo que el `layout(push_constant) uniform Push` de los
    // dos shaders: un desajuste de offset entre CPU y GPU no da error de
    // compilacion, ni de enlazado, ni aviso de ninguna capa de validacion —
    // solo un flag con basura y colores mal. Los static_assert de abajo son la
    // unica red que hay por el lado de la CPU; por el de la GPU, `spirv-dis
    // shaders/ui.frag.spv | grep MemberDecorate` tiene que enseñar Offset 0
    // para la mat4 y Offset 64 para el int.
    struct UiPushConstants
    {
        glm::mat4 transform{1.0f};
        // 0 = el destino es SRGB y el hardware codifica al escribir (el pase de
        // UI). 1 = el destino es HDR LINEAL (el pase de escena) y ui.frag
        // deshace la gamma a mano, o el color sale lavado.
        int32_t   linearOutput = 0;
    };
    static_assert(offsetof(UiPushConstants, transform)    == 0,  "ui.vert espera la mat4 en el offset 0");
    static_assert(offsetof(UiPushConstants, linearOutput) == 64, "ui.frag espera el flag en el offset 64");

    // Lo que se empuja de verdad: hasta el ultimo byte util, sin el relleno de
    // alineacion que sizeof(UiPushConstants) mete detras (glm::mat4 alinea a 16,
    // asi que sizeof serian 80 y los 12 ultimos bytes serian basura sin
    // inicializar).
    constexpr uint32_t kUiPushConstantSize = 68;

    struct UiBatch
    {
        // nullptr = textura blanca de 1x1 del propio UiSpriteBatch (paneles de
        // color plano). Es también la clave de agrupado: dos nodos con el mismo
        // puntero y el mismo scissor caen en el mismo lote.
        const UiTextureAtlas* atlas = nullptr;
        UiScissor scissor{};
        uint32_t  firstIndex = 0;
        uint32_t  indexCount = 0;
    };

    struct UiDrawData
    {
        std::vector<UiVertex> vertices;
        std::vector<uint16_t> indices;
        std::vector<UiBatch>  batches;

        bool empty() const { return batches.empty(); }
        void clear()
        {
            vertices.clear();
            indices.clear();
            batches.clear();
        }
    };

    class UiSpriteBatch
    {
    public:
        static constexpr int kFrames = 2;   // los mismos MAX_FRAMES del Renderer

        // --- CPU ---------------------------------------------------------------
        static void build(const UiCanvas& canvas, uint32_t width, uint32_t height, UiDrawData& out);

        // --- GPU ---------------------------------------------------------------
        void init(GpuDevice& gpu, GpuResources& res, VkRenderPass renderPass, VkSampleCountFlagBits samples);
        void recreatePipeline(GpuDevice& gpu, VkRenderPass renderPass, VkSampleCountFlagBits samples);

        // Las DOS variantes de canvas de MUNDO, compiladas contra el renderpass
        // de la ESCENA (no el de UI): una con test de profundidad —para que una
        // pared tape el cartel— y otra sin el —para lo que va siempre encima,
        // como una barra de vida—.
        //
        // Sirve tambien de "recreate": si ya habia pipelines de mundo, los
        // destruye antes de compilar los nuevos. Y hay que llamarla CADA VEZ que
        // el Renderer recrea `scenePass` o cambia `samples` (el cambio de AA):
        // un pipeline compilado contra un VkRenderPass ya destruido no da error
        // de validacion, se manifiesta como DEVICE LOST al usarlo.
        //
        // Sin init() previo (headless sin UI, o sin pipeline layout) no hace
        // nada: no hay layout contra el que compilar.
        void initWorldPipelines(GpuDevice& gpu, VkRenderPass scenePass, VkSampleCountFlagBits samples);

        void shutdown(GpuDevice& gpu);

        // Reserva y escribe el descriptor set del atlas. Sin esto el atlas se
        // dibujaría con el set de otro, que es un fallo mudo.
        bool registerAtlas(GpuDevice& gpu, UiTextureAtlas& atlas);

        // El sampler con el que se muestrean los atlas. Lo necesita el editor
        // para enseñar uno en un ImGui::Image: la vista la tiene el atlas, pero
        // el sampler es de aquí.
        VkSampler sampler() const { return m_sampler; }

        // UNA vez por FRAME, antes del PRIMER record/recordWorld de ese frame —
        // y como los canvas de MUNDO se graban en el pase de ESCENA, que corre
        // ANTES del pase de UI, "antes del primero" significa antes del pase de
        // escena, no dentro del de UI.
        //
        // Los totales son el ACUMULADO de TODOS los canvas del frame, de mundo
        // Y de pantalla, en un solo buffer compartido. Por qué no vale
        // dimensionar por pase, ni por canvas:
        //   - Si el buffer creciera a mitad de frame, el bind ya grabado de un
        //     canvas anterior apuntaría a un VkBuffer destruido (ensureBuffers
        //     recrea el handle al crecer).
        //   - Si se llamara una segunda vez para el pase de UI, reiniciaría los
        //     cursores y los canvas de pantalla PISARÍAN los vértices de los de
        //     mundo, que la GPU todavía no ha leído: lee el buffer al EJECUTAR,
        //     no al grabar.
        //   - Si no se llamara antes del pase de escena, los canvas de mundo
        //     llegarían a record() con la capacidad del frame ANTERIOR (o 0) y
        //     la guarda uiCursorFits los descartaría EN SILENCIO: ni un error,
        //     ni un aviso de validación, ni un canvas en pantalla.
        // La guarda uiCursorFits evita la corrupción de memoria; no reemplaza
        // llamar bien a esto. Con totales a 0 no toca ningún buffer y solo
        // reinicia los cursores.
        void beginFrame(GpuDevice& gpu, int frame, uint32_t totalVertices, uint32_t totalIndices);

        // Con datos vacíos no graba ni un comando ni toca ningún buffer.
        //
        // canvasExtent es el espacio en el que se CONSTRUYÓ el UiDrawData (los
        // píxeles de salida, los mismos en los que llega el ratón) y fbExtent el
        // del framebuffer que se graba. Con SSAA no coinciden: la ortográfica
        // sale del primero y los scissor se escalan al segundo, que es el único
        // espacio que entiende un VkRect2D. Iguales, sale lo de siempre.
        //
        // Escribe en el SIGUIENTE hueco libre del buffer del frame (avanzando
        // el cursor que dejó beginFrame) y no siempre en el offset 0: con un
        // solo canvas por frame daba igual, pero con N, bindear siempre en 0
        // haría que cada llamada pisara los vértices de la anterior.
        //
        // transform es proj*view*model ya multiplicada: para un canvas de
        // pantalla es la ortográfica de siempre, y para uno de mundo (tarea
        // posterior) llevará también la cámara y la matriz del canvas. Quien
        // llama decide cuál es; record() ya no calcula ninguna.
        void record(GpuDevice& gpu, VkCommandBuffer cmd, const UiDrawData& data,
                    const glm::mat4& transform,
                    VkExtent2D canvasExtent, VkExtent2D fbExtent, int frame);

        // Igual que record(), pero DENTRO del pase de escena y con la variante
        // de mundo del pipeline. Tres diferencias, todas obligatorias:
        //
        //   1. `transform` es proj*view*model, no una ortográfica: el canvas
        //      sale con perspectiva y lo tapa la geometría que tenga delante.
        //   2. `depthTest` elige pipeline: true = lo tapa una pared; false =
        //      siempre encima. La ESCRITURA de profundidad va apagada en las
        //      dos (ver createPipeline).
        //   3. El scissor se pone a TODO el framebuffer y NO se recorta por
        //      lote. LIMITACIÓN CONOCIDA: `clipChildren` no recorta en un canvas
        //      de mundo. El scissor del batcher está en píxeles de canvas y un
        //      VkRect2D solo entiende píxeles de framebuffer; en pantalla el
        //      mapeo es una escala, pero un canvas de mundo está PROYECTADO
        //      (puede salir rotado, en perspectiva o partido por el borde) y no
        //      hay rectángulo alineado a los ejes que lo represente. Recortar
        //      con el rect sin proyectar taparía trozos que sí se ven.
        //
        // Comparte los buffers y los cursores del frame con record(): el mismo
        // beginFrame() dimensiona para los dos.
        void recordWorld(GpuDevice& gpu, VkCommandBuffer cmd, const UiDrawData& data,
                         const glm::mat4& transform, bool depthTest,
                         VkExtent2D canvasExtent, VkExtent2D fbExtent, int frame);

    private:
        // `depthTest` solo lo enciende la variante de mundo ocluida; la de
        // pantalla y la de mundo-siempre-encima van las dos a false.
        void createPipeline(GpuDevice& gpu, VkRenderPass renderPass, VkSampleCountFlagBits samples,
                            bool depthTest, VkPipeline& out);

        // El cuerpo comun de record() y recordWorld(): la sub-asignacion, la
        // guarda de capacidad, los memcpy y el bucle de lotes. Uno solo para que
        // la guarda uiCursorFits no pueda quedarse en una de las dos rutas.
        void recordInto(VkCommandBuffer cmd, const UiDrawData& data, const glm::mat4& transform,
                        VkPipeline pipeline, bool linearOutput, bool scissorCompleto,
                        VkExtent2D canvasExtent, VkExtent2D fbExtent, int frame);
        void ensureBuffers(GpuDevice& gpu, int frame, uint32_t vertexCount, uint32_t indexCount);
        void destroyBuffers(GpuDevice& gpu, int frame);

        VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
        VkDescriptorPool      m_descPool   = VK_NULL_HANDLE;
        VkPipelineLayout      m_layout     = VK_NULL_HANDLE;
        VkPipeline            m_pipeline   = VK_NULL_HANDLE;
        VkSampler             m_sampler    = VK_NULL_HANDLE;

        // Las dos variantes de MUNDO. Comparten m_layout y m_descLayout con la
        // de pantalla — lo unico que cambia es el renderpass contra el que se
        // compilan (el de la escena), sus muestras y el test de profundidad.
        VkPipeline m_worldPipelineDepth   = VK_NULL_HANDLE;   // lo tapa la geometria
        VkPipeline m_worldPipelineNoDepth = VK_NULL_HANDLE;   // siempre encima

        // Blanco de 1x1 para los nodos sin atlas: multiplicar por (1,1,1,1) deja
        // el color del vértice tal cual, así que un panel plano no necesita ni
        // pipeline aparte ni rama en el shader.
        VkImage         m_whiteImage  = VK_NULL_HANDLE;
        VkDeviceMemory  m_whiteMemory = VK_NULL_HANDLE;
        VkImageView     m_whiteView   = VK_NULL_HANDLE;
        VkDescriptorSet m_whiteSet    = VK_NULL_HANDLE;

        // Un par de buffers por frame en vuelo, con mapeo persistente y
        // crecimiento por duplicación, igual que el SSBO de instancias. Se crean
        // en el PRIMER frame con algo que dibujar, no en init.
        VkBuffer       m_vertexBuffers[kFrames]  = {};
        VkDeviceMemory m_vertexMemory[kFrames]   = {};
        void*          m_vertexMapped[kFrames]   = {};
        uint32_t       m_vertexCapacity[kFrames] = {};

        VkBuffer       m_indexBuffers[kFrames]  = {};
        VkDeviceMemory m_indexMemory[kFrames]   = {};
        void*          m_indexMapped[kFrames]   = {};
        uint32_t       m_indexCapacity[kFrames] = {};

        // Cursores de sub-asignación DENTRO del buffer del frame en curso.
        // beginFrame() los pone a 0; cada record() avanza el suyo con
        // bumpUiCursor y escribe a partir de donde lo dejó el anterior.
        uint32_t m_frameVertexCursor[kFrames] = {};
        uint32_t m_frameIndexCursor[kFrames]  = {};
    };
}
