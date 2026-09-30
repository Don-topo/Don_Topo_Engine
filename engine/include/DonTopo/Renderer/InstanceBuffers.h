#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <glm/glm.hpp>

namespace DonTopo {

class GpuDevice;

// Bump allocation inside an already mapped buffer: whoever asks for space says how many
// matrices it wants and receives where to write them, or **nullptr** if they do not fit.
//
// Not a single line of Vulkan on purpose. The only logic of the
// matter lives here, so all of it can be asserted without a GPU, and it was needed,
// because this guard was copied by hand in THREE places in the Renderer:
//
//     if (m_instanceCursor >= m_instanceCapacity[m_currentFrame]) { ... break; }
//     const uint32_t i = m_instanceCursor++;
//     ((glm::mat4*)m_instanceMapped[m_currentFrame])[i] = sobj.transform;
//
// With `alloc` there is no way to write without having looked: whoever does not check the
// null does not write outside the buffer, it blows up on the spot. The pointer arithmetic
// and the cursor are no longer within the caller's reach.
class InstanceCursor {
public:
    // Buffer of the frame and matrices that fit. Null `mapped` = nothing fits, which
    // is the valid state of a frame whose buffer does not exist yet.
    //
    // **This is the ONLY place that writes the buffer and its capacity**, and from that
    // comes the invariant everything else lives on: no buffer, capacity
    // ZERO. That is why neither `alloc` nor `rest` ask about the pointer again:
    // checking the limit already covers the case, and a second guard that cannot
    // fire is one that nobody knows whether it is still needed (it was sabotaged:
    // removing it did not turn a single test red).
    void reset(glm::mat4* mapped, uint32_t capacity)
    {
        m_mapped   = mapped;
        m_capacity = mapped ? capacity : 0;
        m_cursor   = 0;
    }

    // `n` contiguous matrices, or nullptr if they do not fit.
    //
    // `outBase` receives the INDEX of the first one, which is what the draw puts in
    // `firstInstance` so that the shader finds it by `gl_InstanceIndex`.
    // It goes here and not in a separately read `cursor()` because they are the same act: reading
    // the index before reserving and having the reservation fail leaves an index that
    // points to whatever the next one writes.
    glm::mat4* alloc(uint32_t n, uint32_t* outBase = nullptr)
    {
        // The sum in 64 bits on purpose: with the cursor near the limit,
        // `m_cursor + n` in 32 bits could wrap around and pass the comparison.
        //
        // Without a buffer the capacity is 0 (reset invariant), so this
        // check also covers that case and there is no need to look at the pointer.
        if ((uint64_t)m_cursor + n > (uint64_t)m_capacity) return nullptr;
        glm::mat4* dst = m_mapped + m_cursor;
        if (outBase) *outBase = m_cursor;
        m_cursor += n;
        return dst;
    }

    // What is left free, for whoever does not know how many matrices it will write
    // until it finishes (batch grouping). Closed with commit().
    struct Span {
        glm::mat4* data     = nullptr;  // where the free gap starts
        uint32_t   capacity = 0;        // matrices that fit there
        uint32_t   base     = 0;        // index of the first one, for firstInstance
    };

    Span rest() const
    {
        Span s;
        // The subtraction is guarded because it is UNSIGNED: a cursor past the limit
        // would give a huge capacity instead of zero, and that is writing outside the
        // buffer with nothing warning. With capacity 0 (unmapped buffer) it also
        // exits through here, so there is no need to look at the pointer separately.
        if (m_cursor >= m_capacity) return s;
        s.base     = m_cursor;
        s.data     = m_mapped + m_cursor;
        s.capacity = m_capacity - m_cursor;
        return s;
    }

    // Closes a `rest()`: `used` is what was actually written. It is clamped to the
    // limit in case the caller lies; going over here moved the cursor outside the
    // buffer and the next pass wrote into no man's land.
    void commit(uint32_t used)
    {
        const uint32_t libre = m_capacity - m_cursor;
        m_cursor += (used < libre) ? used : libre;
    }

    // Matrices already written in the frame. It is the base of the `firstInstance` of the
    // next pass, which shares the buffer with this one.
    uint32_t cursor() const { return m_cursor; }
    uint32_t capacity() const { return m_capacity; }

private:
    glm::mat4* m_mapped   = nullptr;
    uint32_t   m_capacity = 0;
    uint32_t   m_cursor   = 0;
};

// Per-instance transforms SSBO (set 1, binding 0): the buffer from which
// triangle.vert and shadow.vert take the model matrix by `gl_InstanceIndex`.
//
// One per frame-in-flight and persistently mapped, because the previous frame
// may still be in flight reading its own. The passes of the frame SHARE the
// buffer: shadows writes first, the scene after, and the cursor marks where
// what has been written ends so that the next one does not overwrite it.
//
// It was loose Renderer state (eight members and three methods) and comes out for the
// same reason the thirteen passes did: the resources and their destruction travel
// together, and whoever writes no longer has to remember anything.
class InstanceBuffers {
public:
    // Frames in flight. Renderer::MAX_FRAMES has to be worth the same, and there is a
    // static_assert in Renderer.h that checks it: if someone raises one and not the
    // other, the descriptor sets of the extra frames would be born without a buffer.
    static constexpr int kFrames = 2;

    // Matrices of the initial buffer. It doubles when growing, so instancing one
    // more object per frame (Lua scripts) does not recreate the buffer on each one.
    static constexpr uint32_t kInitialCapacity = 1024;

    struct Context {
        GpuDevice& gpu;
    };

    InstanceBuffers()                                  = default;
    InstanceBuffers(const InstanceBuffers&)            = delete;
    InstanceBuffers& operator=(const InstanceBuffers&) = delete;

    // Set layout, pool, the kFrames sets and an initial buffer for EACH frame.
    // Both, not just the current one: the descriptor set of each frame has to
    // point to something valid from the first draw.
    void create(const Context& ctx);
    void destroy(const Context& ctx);

    // Start of the frame: ensures room for `matrices` and sets the cursor to 0.
    // Growing recreates the buffer, so this goes BEFORE recording anything of the frame,
    // never in the middle.
    void beginFrame(const Context& ctx, int frame, uint32_t matrices);

    // The allocator of the current frame. Everything that writes matrices goes through here.
    InstanceCursor&       cur()       { return m_cur; }
    const InstanceCursor& cur() const { return m_cur; }

    VkDescriptorSetLayout descLayout() const { return m_descLayout; }
    VkDescriptorSet       set(int frame) const { return m_descSets[frame]; }

private:
    // Grows the buffer of `frame` until `matrices` fit, if they did not already.
    void ensureCapacity(const Context& ctx, int frame, uint32_t matrices);
    void destroyBuffer(const Context& ctx, int frame);

    VkDescriptorSetLayout m_descLayout        = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool          = VK_NULL_HANDLE;
    VkDescriptorSet       m_descSets[kFrames] = {};
    VkBuffer              m_buffers[kFrames]  = {};
    VkDeviceMemory        m_memory[kFrames]   = {};
    void*                 m_mapped[kFrames]   = {};
    uint32_t              m_capacity[kFrames] = {};   // en matrices
    // The cursor is NOT per frame: there is only one alive at a time, and beginFrame
    // repoints it to that frame's buffer.
    InstanceCursor        m_cur;
};

}
