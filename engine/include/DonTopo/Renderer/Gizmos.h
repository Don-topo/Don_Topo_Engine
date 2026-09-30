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

// Unlit debug drawing system (lines), Unity-style
// Gizmos/Debug.DrawLine. Static API backed by an internal singleton;
// Renderer controls init/draw/clear (lifecycle), the rest of the engine
// only calls Gizmos::drawX(...) during its update, before
// Renderer::drawFrame() is invoked in that same cycle. The lines drawn
// do not persist to the next frame unless they are called again.
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
    // depthZeroToOne: the viewProj matrix can come from two different depth
    // conventions. glm::perspective/ortho by default (without
    // GLM_FORCE_DEPTH_ZERO_TO_ONE) are NO: near -> z_ndc=-1, meant for
    // OpenGL. CameraComponent::projectionMatrix uses *_ZO (near -> z_ndc=0)
    // because Vulkan clips 0<=z<=w. Reconstructing corners with the wrong
    // z_ndc misplaces the near face: in orthographic it goes behind the
    // eye, in perspective it stays in front of the near plane. The default
    // false keeps existing callers intact (e.g. sandbox/main.cpp,
    // which uses a NO glm matrix untouched).
    static void drawFrustum(const glm::mat4& viewProj, const glm::vec3& color,
                            bool depthZeroToOne = false);

    // For Renderer's exclusive use.
    // colorFormat: unused (the renderPass already carries it), kept for symmetry with Skybox::init.
    // samples: samples of the render pass they are drawn in (the composition one).
    // Imposed by the Renderer's anti-aliasing mode.
    static void init(GpuDevice& gpu, VkRenderPass renderPass, VkFormat colorFormat,
                     VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
    // Rebuilds ONLY the pipeline, for when MSAA changes the sample count:
    // in Vulkan 1.0 rasterizationSamples is not dynamic state.
    static void recreatePipeline(GpuDevice& gpu, VkRenderPass renderPass, VkSampleCountFlagBits samples);
    static void shutdown(GpuDevice& gpu);

    // ── Consuming EMPTIES, and that is why there is no clear() that can be forgotten ─────
    //
    // This used to be a `vertices()` that only looked, plus a separate `clear()`
    // that the consumer had to remember to call. It was forgotten: the DirectX 12
    // path read the vertices and did not empty them, so the vector grew frame
    // after frame until kMaxGizmoVertices was exhausted and the only thing seen was the
    // capacity warning (H16). It was fixed by adding a clear() by hand, which is
    // exactly the same thing that can be forgotten again in the next
    // backend.
    //
    // Uploads and draws this cycle's vertices, and leaves the buffer empty. It empties
    // ALWAYS, even if it does not get to draw (gizmos off, or init() not yet
    // called): if the emptying depended on having drawn, the not-drawing case
    // would go back to accumulating forever.
    static void draw(VkCommandBuffer cmd, const glm::mat4& viewProj, int frameIndex);

    // Takes this cycle's vertices and leaves the buffer empty. For the
    // non-Vulkan backends, which upload them on their own
    // (D3D12Renderer::submitDebugLines).
    static std::vector<GizmoVertex> takeVertices();

    // Throws away what has accumulated WITHOUT drawing it. Only for a frame that does not get
    // drawn (obsolete swapchain, or the project selector in front): otherwise
    // those lines would be carried over duplicated to the next frame that does draw.
    // It is called discard and not clear on purpose: it forces you to justify why the
    // work is thrown away, instead of looking like "I already consumed it".
    static void discard();

    // Must match Renderer::MAX_FRAMES (checked with static_assert in Renderer.cpp).
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
