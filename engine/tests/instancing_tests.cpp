// Headless test of instancing grouping (no GUI, no Vulkan).
// Renderer::buildInstanceBatches is static precisely so it can be exercised without an
// initialized Renderer, same as frustumFromViewProj. Plain main + asserts,
// no framework, consistent with frustum_tests.cpp.
//
// What is tested is not "does it group?" but three things that fail silently: that
// each instance ends up in the slot its draw is going to read (a wrongly set
// firstInstance gives no error anywhere: it draws the wrong object in the wrong
// place), that what is invisible does NOT spend a slot, and that without capacity it truncates
// instead of writing outside the SSBO. That is why each transform carries a unique
// translation and the matrix is checked slot by slot, not just the counters.
#include "DonTopo/Renderer/Renderer.h"
#include "DonTopo/Renderer/InstanceBuffers.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <vector>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Translation in X: it identifies the object unambiguously and is not the neutral value,
// so a slot that nobody writes is distinguishable from a well-written one.
static glm::mat4 markerAt(float x)
{
    return glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.0f, 0.0f));
}

static float markerOf(const glm::mat4& m) { return m[3][0]; }

// Recognizable fill of the output buffer: any slot that is left
// unwritten comes out as -999 and not as an identity that could slip through.
static constexpr float kUnwritten = -999.0f;

static std::vector<glm::mat4> makeOut(size_t slots)
{
    return std::vector<glm::mat4>(slots, markerAt(kUnwritten));
}

// N objects of the same shared entry = ONE draw with N instances, and the
// transforms in the order in which they came.
static void test_misma_entrada_un_solo_grupo()
{
    std::vector<glm::mat4> tf = { markerAt(10.0f), markerAt(20.0f), markerAt(30.0f) };
    std::vector<Renderer::BatchCandidate> cands;
    for (auto& m : tf) cands.push_back({ 7, true, &m });

    std::vector<glm::mat4> out = makeOut(8);
    std::vector<Renderer::InstanceBatch> batches;
    const uint32_t written = Renderer::buildInstanceBatches(cands.data(), cands.size(),
        out.data(), (uint32_t)out.size(), 0, batches);

    CHECK(written == 3);
    CHECK(batches.size() == 1);
    if (batches.size() == 1)
    {
        CHECK(batches[0].sharedIndex   == 7);
        CHECK(batches[0].firstInstance == 0);
        CHECK(batches[0].instanceCount == 3);
    }
    CHECK(markerOf(out[0]) == 10.0f);
    CHECK(markerOf(out[1]) == 20.0f);
    CHECK(markerOf(out[2]) == 30.0f);
    // Nothing written beyond what was said.
    CHECK(markerOf(out[3]) == kUnwritten);
}

// Interleaved entries: each group has to end up CONTIGUOUS (an instanced draw
// reads a range, not a list) and the order of groups is that of first
// appearance, which is what makes the result stable between frames.
static void test_mezcla_de_entradas_agrupa_contiguo()
{
    std::vector<glm::mat4> tf = {
        markerAt(1.0f),  // A
        markerAt(2.0f),  // B
        markerAt(3.0f),  // A
        markerAt(4.0f),  // C
        markerAt(5.0f),  // B
        markerAt(6.0f),  // A
    };
    const int shared[] = { 4, 9, 4, 2, 9, 4 };
    std::vector<Renderer::BatchCandidate> cands;
    for (int i = 0; i < 6; i++) cands.push_back({ shared[i], true, &tf[(size_t)i] });

    std::vector<glm::mat4> out = makeOut(8);
    std::vector<Renderer::InstanceBatch> batches;
    const uint32_t written = Renderer::buildInstanceBatches(cands.data(), cands.size(),
        out.data(), (uint32_t)out.size(), 0, batches);

    CHECK(written == 6);
    CHECK(batches.size() == 3);
    if (batches.size() == 3)
    {
        CHECK(batches[0].sharedIndex == 4); // first appearance: A
        CHECK(batches[1].sharedIndex == 9); // then B
        CHECK(batches[2].sharedIndex == 2); // and lastly C
        CHECK(batches[0].firstInstance == 0); CHECK(batches[0].instanceCount == 3);
        CHECK(batches[1].firstInstance == 3); CHECK(batches[1].instanceCount == 2);
        CHECK(batches[2].firstInstance == 5); CHECK(batches[2].instanceCount == 1);
    }
    // A: 1,3,6 together; B: 2,5; C: 4. If the grouping wrote in candidate order
    // instead of by group, these slots would come out mixed.
    CHECK(markerOf(out[0]) == 1.0f);
    CHECK(markerOf(out[1]) == 3.0f);
    CHECK(markerOf(out[2]) == 6.0f);
    CHECK(markerOf(out[3]) == 2.0f);
    CHECK(markerOf(out[4]) == 5.0f);
    CHECK(markerOf(out[5]) == 4.0f);
}

// What the pass already discarded (culled, upload in flight, deleted entry) arrives
// with visible = false: it must not spend a slot or appear in any group. A
// grouping that ignores the flag would draw objects outside the camera and, in the case of
// the deleted entry, would dereference a dead entry when looking up its buffers.
static void test_no_visibles_excluidos()
{
    std::vector<glm::mat4> tf = {
        markerAt(1.0f),  // culled
        markerAt(2.0f),  // visible
        markerAt(3.0f),  // in flight
        markerAt(4.0f),  // visible
        markerAt(5.0f),  // deleted from the editor: sharedIndex -1
    };
    std::vector<Renderer::BatchCandidate> cands = {
        { 3, false, &tf[0] },
        { 3, true,  &tf[1] },
        { 3, false, &tf[2] },
        { 3, true,  &tf[3] },
        { -1, false, &tf[4] },
    };

    std::vector<glm::mat4> out = makeOut(8);
    std::vector<Renderer::InstanceBatch> batches;
    const uint32_t written = Renderer::buildInstanceBatches(cands.data(), cands.size(),
        out.data(), (uint32_t)out.size(), 0, batches);

    CHECK(written == 2);
    CHECK(batches.size() == 1);
    if (batches.size() == 1)
    {
        CHECK(batches[0].sharedIndex   == 3);
        CHECK(batches[0].instanceCount == 2);
    }
    CHECK(markerOf(out[0]) == 2.0f);
    CHECK(markerOf(out[1]) == 4.0f);
    CHECK(markerOf(out[2]) == kUnwritten);

    // All invisible: neither groups nor writes. It is the case of a scene loading
    // (all uploads in flight), which before the grouping was simply
    // "no draw".
    for (auto& c : cands) c.visible = false;
    std::vector<glm::mat4> out2 = makeOut(4);
    CHECK(Renderer::buildInstanceBatches(cands.data(), cands.size(),
              out2.data(), (uint32_t)out2.size(), 0, batches) == 0);
    CHECK(batches.empty());
    CHECK(markerOf(out2[0]) == kUnwritten);
}

// The two passes share the SSBO: the shadow one writes in front and the scene
// one behind, with firstInstanceBase = what is already written. The firstInstance values have
// to be ABSOLUTE (indices inside the buffer) while the writes are
// RELATIVE to the pointer that is passed, which already comes offset.
static void test_base_de_first_instance()
{
    std::vector<glm::mat4> tf = { markerAt(11.0f), markerAt(22.0f), markerAt(33.0f) };
    std::vector<Renderer::BatchCandidate> cands = {
        { 1, true, &tf[0] },
        { 5, true, &tf[1] },
        { 1, true, &tf[2] },
    };

    // The full buffer with 4 slots already occupied by the previous pass; the offset
    // pointer is passed, as the Renderer does.
    std::vector<glm::mat4> full = makeOut(12);
    const uint32_t base = 4;
    for (uint32_t i = 0; i < base; i++) full[i] = markerAt(100.0f + (float)i);

    std::vector<Renderer::InstanceBatch> batches;
    const uint32_t written = Renderer::buildInstanceBatches(cands.data(), cands.size(),
        full.data() + base, (uint32_t)full.size() - base, base, batches);

    CHECK(written == 3);
    CHECK(batches.size() == 2);
    if (batches.size() == 2)
    {
        CHECK(batches[0].firstInstance == base);      // no 0
        CHECK(batches[0].instanceCount == 2);
        CHECK(batches[1].firstInstance == base + 2);
        CHECK(batches[1].instanceCount == 1);
    }
    // The range of the previous pass is still intact.
    CHECK(markerOf(full[0]) == 100.0f);
    CHECK(markerOf(full[3]) == 103.0f);
    // And the new one starts right at base.
    CHECK(markerOf(full[base + 0]) == 11.0f);
    CHECK(markerOf(full[base + 1]) == 33.0f);
    CHECK(markerOf(full[base + 2]) == 22.0f);
}

// More objects than capacity: it truncates by groups and does NOT write a single slot
// outside the promised range. Writing outside here is corrupting mapped device
// memory, which gives no error anywhere.
static void test_trunca_sin_desbordar()
{
    std::vector<glm::mat4> tf;
    for (int i = 0; i < 6; i++) tf.push_back(markerAt((float)(i + 1)));
    const int shared[] = { 1, 1, 1, 2, 2, 3 };
    std::vector<Renderer::BatchCandidate> cands;
    for (int i = 0; i < 6; i++) cands.push_back({ shared[i], true, &tf[(size_t)i] });

    // Capacity 4 for 6 instances: group 1 (3) fits and only one of group 2.
    std::vector<glm::mat4> out = makeOut(10);
    const uint32_t capacity = 4;
    std::vector<Renderer::InstanceBatch> batches;
    const uint32_t written = Renderer::buildInstanceBatches(cands.data(), cands.size(),
        out.data(), capacity, 0, batches);

    CHECK(written == capacity);
    // Group 3 does not fit: it cannot arrive as a draw of 0 instances.
    CHECK(batches.size() == 2);
    uint32_t sum = 0;
    for (const auto& b : batches)
    {
        CHECK(b.instanceCount > 0);
        CHECK(b.firstInstance + b.instanceCount <= capacity);
        sum += b.instanceCount;
    }
    CHECK(sum == capacity);
    // Not a single slot touched beyond the capacity.
    for (size_t i = capacity; i < out.size(); i++)
        CHECK(markerOf(out[i]) == kUnwritten);
}


// --- Distribution within the instance SSBO -----------------------------------

// InstanceCursor is the Vulkan-FREE half of InstanceBuffers, and that is why it can be
// asserted here in full. What it protects is the guard that used to be copied by
// hand in three places of the Renderer: checking the cap, advancing the cursor and doing
// the pointer arithmetic. Skipping the check in any of the three
// wrote outside the buffer without anything warning.
static void test_cursor_reparte_contiguo()
{
    glm::mat4 buffer[8];
    for (auto& m : buffer) m = markerAt(kUnwritten);

    InstanceCursor cur;
    cur.reset(buffer, 8);
    CHECK(cur.cursor() == 0);
    CHECK(cur.capacity() == 8);

    glm::mat4* a = cur.alloc(3);
    CHECK(a == buffer);          // the first one starts at the beginning
    CHECK(cur.cursor() == 3);

    glm::mat4* b = cur.alloc(2);
    CHECK(b == buffer + 3);      // the next one goes BEHIND, without overwriting
    CHECK(cur.cursor() == 5);

    // What is written through the pointer ends up in the slot its draw is going to read.
    a[0] = markerAt(10.0f);
    b[0] = markerAt(20.0f);
    CHECK(markerOf(buffer[0]) == 10.0f);
    CHECK(markerOf(buffer[3]) == 20.0f);
}

// It does not fit: nullptr and the cursor does NOT move. Both halves matter: returning
// nullptr but having advanced would leave a dead gap in the buffer.
static void test_cursor_no_cabe_devuelve_nulo()
{
    glm::mat4 buffer[4];
    InstanceCursor cur;
    cur.reset(buffer, 4);

    CHECK(cur.alloc(3) != nullptr);
    CHECK(cur.cursor() == 3);

    CHECK(cur.alloc(2) == nullptr);   // asks for 2, 1 left
    CHECK(cur.cursor() == 3);         // and it has not moved

    CHECK(cur.alloc(1) != nullptr);   // the last slot does fit
    CHECK(cur.cursor() == 4);
    CHECK(cur.alloc(1) == nullptr);   // full
    CHECK(cur.cursor() == 4);
}

// The sum cursor + n is done in 64 bits: in 32 it would wrap around and pass the
// check, which is the silent way of writing outside the buffer.
static void test_cursor_no_desborda_el_uint32()
{
    glm::mat4 buffer[4];
    InstanceCursor cur;
    cur.reset(buffer, 4);
    CHECK(cur.alloc(3) != nullptr);

    // 3 + 0xFFFFFFFF wraps around to 2, which "fits" in 4 if the sum is 32-bit.
    CHECK(cur.alloc(0xFFFFFFFFu) == nullptr);
    CHECK(cur.cursor() == 3);
}

// An unmapped buffer (the frame whose buffer does not exist yet) accepts nothing, instead
// of handing out a null pointer with an offset.
static void test_cursor_sin_buffer_no_reparte()
{
    InstanceCursor cur;
    cur.reset(nullptr, 1024);         // capacity lied about on purpose
    CHECK(cur.capacity() == 0);       // it does not believe it
    CHECK(cur.alloc(1) == nullptr);
    CHECK(cur.rest().data == nullptr);
    CHECK(cur.rest().capacity == 0);
}

// rest() gives the free tail for whoever does not know how much they are going to write until
// they finish, and commit() closes it. It is the path of batch grouping.
static void test_cursor_rest_y_commit()
{
    glm::mat4 buffer[10];
    InstanceCursor cur;
    cur.reset(buffer, 10);
    CHECK(cur.alloc(4) != nullptr);

    InstanceCursor::Span s = cur.rest();
    CHECK(s.data == buffer + 4);
    CHECK(s.capacity == 6);
    CHECK(s.base == 4);               // the base of the batch's firstInstance values

    cur.commit(2);
    CHECK(cur.cursor() == 6);
    CHECK(cur.rest().base == 6);
    CHECK(cur.rest().capacity == 4);

    // A commit that overshoots is trimmed: moving the cursor outside the buffer would leave
    // the next pass writing in no man's land.
    cur.commit(999);
    CHECK(cur.cursor() == 10);
    CHECK(cur.rest().data == nullptr);   // full: no tail left
    CHECK(cur.rest().capacity == 0);
    CHECK(cur.alloc(1) == nullptr);
}

// The buffer is shared between the two passes of the frame (shadows first, scene
// behind). reset() is what separates one frame from the next.
static void test_cursor_reset_entre_frames()
{
    glm::mat4 buffer[4];
    InstanceCursor cur;
    cur.reset(buffer, 4);
    CHECK(cur.alloc(4) != nullptr);
    CHECK(cur.alloc(1) == nullptr);   // exhausted

    cur.reset(buffer, 4);             // new frame
    CHECK(cur.cursor() == 0);
    CHECK(cur.alloc(4) != nullptr);   // it fits whole again
}

// Two objects of the SAME mesh with different PBR factors cannot share a
// draw: metallic and roughness travel by push constant, which is per group. Before
// taking them out of the dedup key the case did not exist (the shared mesh was
// already a different one), and that is why this test is born with the live dragging feature.
static void test_factores_distintos_parten_el_grupo()
{
    std::vector<glm::mat4> tf = { markerAt(10.0f), markerAt(20.0f), markerAt(30.0f) };
    std::vector<Renderer::BatchCandidate> cands;
    // Same shared entry (7) and same ssr in all three: the only thing that separates
    // the one in the middle is the metallic.
    cands.push_back({ 7, true, &tf[0], 0.0f, 0.0f, 0.5f });
    cands.push_back({ 7, true, &tf[1], 0.0f, 1.0f, 0.5f });
    cands.push_back({ 7, true, &tf[2], 0.0f, 0.0f, 0.5f });

    std::vector<glm::mat4> out = makeOut(8);
    std::vector<Renderer::InstanceBatch> batches;
    const uint32_t written = Renderer::buildInstanceBatches(cands.data(), cands.size(),
        out.data(), (uint32_t)out.size(), 0, batches);

    CHECK(written == 3);
    CHECK(batches.size() == 2);
    if (batches.size() == 2)
    {
        // The group of the two with metallic 0 comes out first (order of first
        // appearance) and CONTIGUOUS, even though in the entry they were separated.
        CHECK(batches[0].instanceCount == 2);
        CHECK(batches[0].metallic      == 0.0f);
        CHECK(batches[1].instanceCount == 1);
        CHECK(batches[1].metallic      == 1.0f);
    }
    CHECK(markerOf(out[0]) == 10.0f);
    CHECK(markerOf(out[1]) == 30.0f);
    CHECK(markerOf(out[2]) == 20.0f);
}

// The roughness splits the same as the metallic: the key is BOTH. With only the
// metallic compared, two objects with the same metal and different roughness
// would share a push constant and one of the two would be drawn with the other's.
static void test_roughness_tambien_parte_el_grupo()
{
    std::vector<glm::mat4> tf = { markerAt(10.0f), markerAt(20.0f) };
    std::vector<Renderer::BatchCandidate> cands;
    cands.push_back({ 7, true, &tf[0], 0.0f, 1.0f, 0.2f });
    cands.push_back({ 7, true, &tf[1], 0.0f, 1.0f, 0.8f });

    std::vector<glm::mat4> out = makeOut(4);
    std::vector<Renderer::InstanceBatch> batches;
    Renderer::buildInstanceBatches(cands.data(), cands.size(), out.data(),
                                   (uint32_t)out.size(), 0, batches);

    CHECK(batches.size() == 2);
    if (batches.size() == 2)
    {
        CHECK(batches[0].roughness == 0.2f);
        CHECK(batches[1].roughness == 0.8f);
    }
}

// And the case that pays the bill: SAME factors are still a single draw.
// Without this, the feature could have split all the groups and nobody would
// find out until looking at the draw counter of the Performance panel.
static void test_mismos_factores_no_parten_el_grupo()
{
    std::vector<glm::mat4> tf = { markerAt(10.0f), markerAt(20.0f), markerAt(30.0f) };
    std::vector<Renderer::BatchCandidate> cands;
    for (auto& m : tf) cands.push_back({ 7, true, &m, 0.0f, 0.75f, 0.25f });

    std::vector<glm::mat4> out = makeOut(8);
    std::vector<Renderer::InstanceBatch> batches;
    Renderer::buildInstanceBatches(cands.data(), cands.size(), out.data(),
                                   (uint32_t)out.size(), 0, batches);

    CHECK(batches.size() == 1);
    if (batches.size() == 1)
    {
        CHECK(batches[0].instanceCount == 3);
        CHECK(batches[0].metallic      == 0.75f);
        CHECK(batches[0].roughness     == 0.25f);
    }
}

int main()
{
    test_misma_entrada_un_solo_grupo();
    test_mezcla_de_entradas_agrupa_contiguo();
    test_no_visibles_excluidos();
    test_base_de_first_instance();
    test_trunca_sin_desbordar();
    test_factores_distintos_parten_el_grupo();
    test_roughness_tambien_parte_el_grupo();
    test_mismos_factores_no_parten_el_grupo();

    test_cursor_reparte_contiguo();
    test_cursor_no_cabe_devuelve_nulo();
    test_cursor_no_desborda_el_uint32();
    test_cursor_sin_buffer_no_reparte();
    test_cursor_rest_y_commit();
    test_cursor_reset_entre_frames();

    if (g_failures == 0) std::printf("instancing_tests: OK\n");
    else                 std::printf("instancing_tests: %d FALLOS\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
