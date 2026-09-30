#pragma once

// 2D sprite batch of the game UI.
//
// Two well-separated halves:
//   - build(): pure CPU. It walks the canvas hierarchy and produces vertices,
//     indices and batches. It does not touch Vulkan, and is what the tests exercise.
//   - the rest: uploads that data to the in-flight frame's buffers and records the
//     draws INSIDE the Renderer's composition pass (LDR, already tonemapped).
//
// The batch breaks on a change of atlas or scissor, which are the only two
// states that the draw cannot carry per vertex.

#include "DonTopo/UI/UiTextureAtlas.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstddef>   // offsetof: the safety net of the push constant layout
#include <cstdint>
#include <vector>

namespace DonTopo
{
    class GpuDevice;
    class GpuResources;
    class UiCanvas;

    // Position in screen PIXELS, with (0,0) at the top left: the vertex shader's
    // orthographic projection is what takes it to NDC.
    struct UiVertex
    {
        glm::vec2 pos{0.0f};
        glm::vec2 uv{0.0f};
        glm::vec4 color{1.0f};

        // Everything that tells a text quad apart from a sprite one travels PER
        // VERTEX, not per pipeline or per descriptor: this way the text falls in the
        // same batch as the panel behind it.
        //   params.x = mode: 0 = sprite/flat color, 1 = MSDF
        //   params.y = screenPxRange ALREADY scaled to this quad's size
        //   params.z = outline thickness in screen pixels
        //   effect   = outline color (with the tree's opacity already applied)
        // With params.x = 0 the shader does literally what it always did.
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

    // Pure bump allocator: sets aside `count` elements starting at `cursor`,
    // advances the cursor and returns WHERE it started. It is the arithmetic that separates
    // a canvas's write offset inside the frame's SHARED buffer
    // (N canvases, a single VkBuffer). Without this (or by always binding at
    // offset 0, which is what it did with a single canvas) each canvas's draw
    // overwrites the previous one, and since the GPU reads the buffer at EXECUTE and not at
    // RECORD, the frame's N draws all come out with the LAST one's geometry,
    // without any validation layer saying so. It is extracted separately and free of
    // Vulkan precisely so this arithmetic can be tested without a GPU (ui_batch_tests.cpp).
    inline uint32_t bumpUiCursor(uint32_t& cursor, uint32_t count)
    {
        const uint32_t at = cursor;
        cursor += count;
        return at;
    }

    // Capacity guard: does [base, base+count) fit inside `capacity`?
    // record() checks it before EVERY memcpy. The invariant "beginFrame()
    // was called this frame, exactly once, with the exact total" is not
    // enforced by the type; today it is upheld by there being a single caller (Renderer).
    // As soon as there is a second one (the world canvases, in the scene pass,
    // in another loop), a record() without its beginFrame, one called twice, or
    // a total that came up short, would write OUTSIDE the mapped memory:
    // a HOST write that no validation layer sees: there is no
    // device lost, no Vulkan error, only silent corruption. Better
    // not to draw that canvas than to corrupt the buffer. It sums in 64 bits because
    // `base + count` in 32 bits could overflow near UINT32_MAX (it never
    // happens with real UI sizes, but the guard does not depend on anyone
    // remembering that).
    inline bool uiCursorFits(uint32_t base, uint32_t count, uint32_t capacity)
    {
        return (uint64_t)base + (uint64_t)count <= (uint64_t)capacity;
    }

    // The push constants block of ui.vert/ui.frag, in C++. It has to say
    // EXACTLY the same as the `layout(push_constant) uniform Push` of the
    // two shaders: an offset mismatch between CPU and GPU gives no compile
    // error, no link error, and no warning from any validation layer,
    // only a flag with garbage and wrong colors. The static_asserts below are the
    // only safety net on the CPU side; on the GPU side, `spirv-dis
    // shaders/ui.frag.spv | grep MemberDecorate` has to show Offset 0
    // for the mat4 and Offset 64 for the int.
    struct UiPushConstants
    {
        glm::mat4 transform{1.0f};
        // 0 = the target is SRGB and the hardware encodes on write (the UI
        // pass). 1 = the target is LINEAR HDR (the scene pass) and ui.frag
        // undoes the gamma by hand, or the color comes out washed out.
        int32_t   linearOutput = 0;
    };
    static_assert(offsetof(UiPushConstants, transform)    == 0,  "ui.vert expects the mat4 at offset 0");
    static_assert(offsetof(UiPushConstants, linearOutput) == 64, "ui.frag expects the flag at offset 64");

    // What is really pushed: up to the last useful byte, without the alignment
    // padding that sizeof(UiPushConstants) adds at the end (glm::mat4 aligns to 16,
    // so sizeof would be 80 and the last 12 bytes would be uninitialized
    // garbage).
    constexpr uint32_t kUiPushConstantSize = 68;

    struct UiBatch
    {
        // nullptr = UiSpriteBatch's own 1x1 white texture (flat-color panels).
        // It is also the grouping key: two nodes with the same
        // pointer and the same scissor fall in the same batch.
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
        static constexpr int kFrames = 2;   // the same MAX_FRAMES as the Renderer

        // --- CPU ---------------------------------------------------------------
        static void build(const UiCanvas& canvas, uint32_t width, uint32_t height, UiDrawData& out);

        // --- GPU ---------------------------------------------------------------
        void init(GpuDevice& gpu, GpuResources& res, VkRenderPass renderPass, VkSampleCountFlagBits samples);
        void recreatePipeline(GpuDevice& gpu, VkRenderPass renderPass, VkSampleCountFlagBits samples);

        // The TWO variants of WORLD canvas, compiled against the SCENE's
        // renderpass (not the UI one): one with a depth test (so that a
        // wall covers the sign) and another without it (for what always goes on top,
        // like a health bar).
        //
        // It also serves as "recreate": if there were already world pipelines, it
        // destroys them before compiling the new ones. And it has to be called EVERY TIME
        // the Renderer recreates `scenePass` or changes `samples` (the AA change):
        // a pipeline compiled against an already destroyed VkRenderPass gives no validation
        // error, it shows up as DEVICE LOST when used.
        //
        // Without a prior init() (headless without UI, or without a pipeline layout) it does
        // nothing: there is no layout to compile against.
        void initWorldPipelines(GpuDevice& gpu, VkRenderPass scenePass, VkSampleCountFlagBits samples);

        void shutdown(GpuDevice& gpu);

        // Reserves and writes the atlas's descriptor set. Without this the atlas would be
        // drawn with another's set, which is a silent failure.
        bool registerAtlas(GpuDevice& gpu, UiTextureAtlas& atlas);

        // The sampler the atlases are sampled with. The editor needs it
        // to show one in an ImGui::Image: the atlas has the view, but
        // the sampler is from here.
        VkSampler sampler() const { return m_sampler; }

        // ONCE per FRAME, before the FIRST record/recordWorld of that frame,
        // and since WORLD canvases are recorded in the SCENE pass, which runs
        // BEFORE the UI pass, "before the first" means before the scene
        // pass, not inside the UI one.
        //
        // The totals are the ACCUMULATED amount of ALL the frame's canvases, world
        // AND screen, in a single shared buffer. Why sizing
        // per pass or per canvas does not work:
        //   - If the buffer grew in the middle of a frame, the already-recorded bind of an
        //     earlier canvas would point to a destroyed VkBuffer (ensureBuffers
        //     recreates the handle when growing).
        //   - If it were called a second time for the UI pass, it would reset the
        //     cursors and the screen canvases would OVERWRITE the world ones' vertices,
        //     which the GPU has not read yet: it reads the buffer at EXECUTE,
        //     not at RECORD.
        //   - If it were not called before the scene pass, the world canvases
        //     would reach record() with the PREVIOUS frame's capacity (or 0) and
        //     the uiCursorFits guard would discard them SILENTLY: not an error,
        //     not a validation warning, not a canvas on screen.
        // The uiCursorFits guard prevents memory corruption; it does not replace
        // calling this correctly. With totals at 0 it touches no buffer and only
        // resets the cursors.
        void beginFrame(GpuDevice& gpu, int frame, uint32_t totalVertices, uint32_t totalIndices);

        // With empty data it records not a single command and touches no buffer.
        //
        // canvasExtent is the space in which the UiDrawData was BUILT (the output
        // pixels, the same ones the mouse arrives in) and fbExtent that of the
        // framebuffer being recorded. With SSAA they do not match: the orthographic
        // comes from the first and the scissors are scaled to the second, which is the only
        // space a VkRect2D understands. When equal, it comes out as usual.
        //
        // It writes at the NEXT free slot of the frame's buffer (advancing
        // the cursor that beginFrame left) and not always at offset 0: with a
        // single canvas per frame it made no difference, but with N, always binding at 0
        // would make each call overwrite the previous one's vertices.
        //
        // transform is proj*view*model already multiplied: for a screen
        // canvas it is the usual orthographic, and for a world one (later
        // task) it will also carry the camera and the canvas matrix. The caller
        // decides which it is; record() no longer computes any.
        void record(GpuDevice& gpu, VkCommandBuffer cmd, const UiDrawData& data,
                    const glm::mat4& transform,
                    VkExtent2D canvasExtent, VkExtent2D fbExtent, int frame);

        // Same as record(), but INSIDE the scene pass and with the world
        // variant of the pipeline. Three differences, all mandatory:
        //
        //   1. `transform` is proj*view*model, not an orthographic: the canvas
        //      comes out with perspective and geometry in front of it covers it.
        //   2. `depthTest` chooses the pipeline: true = a wall covers it; false =
        //      always on top. Depth WRITING is off in
        //      both (see createPipeline).
        //   3. The scissor is set to the WHOLE framebuffer and is NOT clipped per
        //      batch. KNOWN LIMITATION: `clipChildren` does not clip in a world
        //      canvas. The batcher's scissor is in canvas pixels and a
        //      VkRect2D only understands framebuffer pixels; on screen the
        //      mapping is a scale, but a world canvas is PROJECTED
        //      (it can come out rotated, in perspective or split by the edge) and there is
        //      no axis-aligned rectangle that represents it. Clipping
        //      with the unprojected rect would cover pieces that are visible.
        //
        // It shares the frame's buffers and cursors with record(): the same
        // beginFrame() sizes for both.
        void recordWorld(GpuDevice& gpu, VkCommandBuffer cmd, const UiDrawData& data,
                         const glm::mat4& transform, bool depthTest,
                         VkExtent2D canvasExtent, VkExtent2D fbExtent, int frame);

    private:
        // `depthTest` is only turned on by the occluded world variant; the
        // screen one and the world-always-on-top one both go to false.
        void createPipeline(GpuDevice& gpu, VkRenderPass renderPass, VkSampleCountFlagBits samples,
                            bool depthTest, VkPipeline& out);

        // The common body of record() and recordWorld(): the sub-allocation, the
        // capacity guard, the memcpy and the batch loop. A single one so that
        // the uiCursorFits guard cannot be left on only one of the two paths.
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

        // The two WORLD variants. They share m_layout and m_descLayout with the
        // screen one; the only thing that changes is the renderpass they are
        // compiled against (the scene's), their samples and the depth test.
        VkPipeline m_worldPipelineDepth   = VK_NULL_HANDLE;   // geometry covers it
        VkPipeline m_worldPipelineNoDepth = VK_NULL_HANDLE;   // always on top

        // 1x1 white for nodes without an atlas: multiplying by (1,1,1,1) leaves
        // the vertex color as is, so a flat panel needs neither a separate
        // pipeline nor a branch in the shader.
        VkImage         m_whiteImage  = VK_NULL_HANDLE;
        VkDeviceMemory  m_whiteMemory = VK_NULL_HANDLE;
        VkImageView     m_whiteView   = VK_NULL_HANDLE;
        VkDescriptorSet m_whiteSet    = VK_NULL_HANDLE;

        // A pair of buffers per in-flight frame, with persistent mapping and
        // growth by doubling, just like the instance SSBO. They are created
        // on the FIRST frame with something to draw, not in init.
        VkBuffer       m_vertexBuffers[kFrames]  = {};
        VkDeviceMemory m_vertexMemory[kFrames]   = {};
        void*          m_vertexMapped[kFrames]   = {};
        uint32_t       m_vertexCapacity[kFrames] = {};

        VkBuffer       m_indexBuffers[kFrames]  = {};
        VkDeviceMemory m_indexMemory[kFrames]   = {};
        void*          m_indexMapped[kFrames]   = {};
        uint32_t       m_indexCapacity[kFrames] = {};

        // Sub-allocation cursors INSIDE the current frame's buffer.
        // beginFrame() sets them to 0; each record() advances its own with
        // bumpUiCursor and writes starting where the previous one left off.
        uint32_t m_frameVertexCursor[kFrames] = {};
        uint32_t m_frameIndexCursor[kFrames]  = {};
    };
}
