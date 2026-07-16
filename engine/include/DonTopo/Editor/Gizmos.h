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
    // equivocado deja los 4 de la cara cercana detrás del ojo. El default
    // false mantiene intactos los callers existentes (p.ej. sandbox/main.cpp,
    // que usa una matriz NO de glm sin tocar).
    static void drawFrustum(const glm::mat4& viewProj, const glm::vec3& color,
                            bool depthZeroToOne = false);

    // Uso exclusivo de Renderer.
    // colorFormat: no usado (el renderPass ya lo lleva), se mantiene por simetría con Skybox::init.
    static void init(GpuDevice& gpu, VkRenderPass renderPass, VkFormat colorFormat);
    static void shutdown(GpuDevice& gpu);
    static void draw(VkCommandBuffer cmd, const glm::mat4& viewProj, int frameIndex);
    static void clear();

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
    void createPipeline(GpuDevice& gpu, VkRenderPass renderPass);

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
