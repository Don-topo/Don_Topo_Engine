#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include <array>

namespace DonTopo {

class GpuDevice;

struct GizmoVertex {
    glm::vec3 pos;
    glm::vec3 color;
};

// Sistema de dibujo de depuración sin iluminación (líneas), estilo Unity
// Gizmos/Debug.DrawLine. API estática respaldada por un singleton interno;
// Renderer controla init/draw/clear (ciclo de vida), el resto del engine
// solo llama a Gizmos::drawX(...) durante su update, antes de que
// Renderer::drawFrame() se invoque ese mismo ciclo — las líneas dibujadas
// no persisten al frame siguiente salvo que se vuelvan a llamar.
class Gizmos {
public:
    static void setEnabled(bool enabled);
    static bool isEnabled();

    static void drawLine(const glm::vec3& a, const glm::vec3& b, const glm::vec3& color);
    static void drawRay(const glm::vec3& origin, const glm::vec3& dir,
                         float length, const glm::vec3& color);
    static void drawVector(const glm::vec3& origin, const glm::vec3& v,
                            const glm::vec3& color, float headSize = 0.1f);
    static void drawWireBox(const glm::mat4& transform, const glm::vec3& center,
                            const glm::vec3& halfExtents, const glm::vec3& color);
    static void drawWirePlane(const glm::mat4& transform, const glm::vec3& center,
                              const glm::vec3& color);
    static void drawWireSphere(const glm::mat4& transform, const glm::vec3& center,
                               float radius, const glm::vec3& color);
    static void drawWireCapsule(const glm::mat4& transform, const glm::vec3& center,
                                float radius, float halfHeight, const glm::vec3& color);
    static void drawAxes(const glm::mat4& transform, float scale = 1.0f);
    // depthZeroToOne: la matriz viewProj puede venir de dos convenciones de
    // profundidad distintas. glm::perspective/ortho por defecto (sin
    // GLM_FORCE_DEPTH_ZERO_TO_ONE) son NO: near -> z_ndc=-1, pensadas pa
    // OpenGL. CameraComponent::projectionMatrix usa *_ZO (near -> z_ndc=0)
    // porque Vulkan clipea 0<=z<=w. Reconstruir corners con el z_ndc
    // equivocado descoloca la cara cercana: en ortográfica se va detrás del
    // ojo, en perspectiva se queda por delante del near plane. El default
    // false mantiene intactos los callers existentes (p.ej. sandbox/main.cpp,
    // que usa una matriz NO de glm sin tocar).
    static void drawFrustum(const glm::mat4& viewProj, const glm::vec3& color,
                            bool depthZeroToOne = false);

    // Uso exclusivo de Renderer.
    // colorFormat: no usado (el renderPass ya lo lleva), se mantiene por simetría con Skybox::init.
    // samples: muestras del render pass en el que se dibujan (el de composición).
    // Lo impone el modo de anti-aliasing del Renderer.
    static void init(GpuDevice& gpu, VkRenderPass renderPass, VkFormat colorFormat,
                     VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
    // Rehace SOLO el pipeline, para cuando el MSAA cambia el número de muestras:
    // en Vulkan 1.0 rasterizationSamples no es estado dinámico.
    static void recreatePipeline(GpuDevice& gpu, VkRenderPass renderPass, VkSampleCountFlagBits samples);
    static void shutdown(GpuDevice& gpu);

    // ── Consumir VACÍA, y por eso no hay un clear() que se pueda olvidar ─────
    //
    // Antes esto era un `vertices()` que solo miraba, más un `clear()` aparte
    // que el consumidor tenía que acordarse de llamar. Se olvidó: el camino de
    // DirectX 12 leía los vértices y no vaciaba, así que el vector crecía frame
    // tras frame hasta agotar kMaxGizmoVertices y lo único que se veía era el
    // aviso de capacidad (H16). Se arregló añadiendo un clear() a mano, que es
    // exactamente lo mismo que se puede volver a olvidar en el siguiente
    // backend.
    //
    // Sube y dibuja los vértices de este ciclo, y deja el buffer vacío. Vacía
    // SIEMPRE, aunque no llegue a dibujar —gizmos apagados, o init() todavía no
    // llamado—: si el vaciado dependiera de haber dibujado, el caso de no
    // dibujar volvería a acumular para siempre.
    static void draw(VkCommandBuffer cmd, const glm::mat4& viewProj, int frameIndex);

    // Se lleva los vértices de este ciclo y deja el buffer vacío. Para los
    // backends que no son Vulkan, que los suben por su cuenta
    // (D3D12Renderer::submitDebugLines).
    static std::vector<GizmoVertex> takeVertices();

    // Tira lo acumulado SIN dibujarlo. Solo para el frame que no se llega a
    // dibujar —swapchain obsoleto, o el selector de proyectos delante—: si no,
    // esas líneas se arrastrarían duplicadas al siguiente frame que sí dibuje.
    // Se llama discard y no clear a propósito: obliga a justificar por qué se
    // tira el trabajo, en vez de parecerse a "ya lo he consumido".
    static void discard();

    // Debe coincidir con Renderer::MAX_FRAMES (comprobado con static_assert en Renderer.cpp).
    static constexpr int kFramesInFlight = 2;

private:
    Gizmos()                         = default;
    Gizmos(const Gizmos&)            = delete;
    Gizmos& operator=(const Gizmos&) = delete;

    static Gizmos& get();

    void addLine(const glm::vec3& a, const glm::vec3& b, const glm::vec3& color);
    void addArc(const glm::mat4& transform, const glm::vec3& center,
                const glm::vec3& axisA, const glm::vec3& axisB, float radius,
                float angleStart, float angleEnd, int segments, const glm::vec3& color);
    void addBoxEdges(const std::array<glm::vec3, 8>& corners, const glm::vec3& color);
    void createBuffer(GpuDevice& gpu);
    void createPipeline(GpuDevice& gpu, VkRenderPass renderPass, VkSampleCountFlagBits samples);

    static constexpr uint32_t kMaxGizmoVertices = 65536;

    bool m_enabled        = true;
    bool m_capacityWarned = false;
    std::vector<GizmoVertex> m_vertices;

    VkBuffer         m_vertexBuffer[kFramesInFlight] = {};
    VkDeviceMemory   m_vertexMemory[kFramesInFlight] = {};
    void*            m_mapped[kFramesInFlight]       = {};
    VkPipelineLayout m_pipeLayout   = VK_NULL_HANDLE;
    VkPipeline       m_pipeline     = VK_NULL_HANDLE;
};

} // namespace DonTopo
