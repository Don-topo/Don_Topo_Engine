// Headless test of the gizmo buffer (no GUI, no Vulkan device). The accumulation
// part (drawX, the enabled flag and the flush) does not touch the graphics API,
// so it can be exercised here in full. Plain main + asserts, no framework,
// consistent with frustum_tests.cpp.
//
// What is tested is the invariant that H16 makes impossible to break:
// CONSUMING EMPTIES. The bug already happened once: the DirectX 12 backend read
// the vertices and did not call clear(), so the vector grew frame by frame until
// it blew past 65536 and the only thing seen was the capacity warning. It had
// been fixed by hand by adding a fourth clear() that can also be forgotten. Here
// it is asserted that it is no longer necessary to remember.
#include "DonTopo/Renderer/Gizmos.h"

#include <cstdio>
#include <vector>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// The singleton is shared between cases: each one starts by emptying it, which is
// also the first check that discard() does its job.
static void empezarLimpio()
{
    Gizmos::setEnabled(true);
    Gizmos::discard();
}

static void test_acumula_y_take_vacia()
{
    empezarLimpio();

    Gizmos::drawLine(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(1.0f));
    Gizmos::drawLine(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f));

    // Two lines = four vertices: the buffer is a list of segments.
    std::vector<GizmoVertex> tomados = Gizmos::takeVertices();
    CHECK(tomados.size() == 4);
    CHECK(tomados[1].pos.x == 1.0f);
    CHECK(tomados[3].pos.y == 1.0f);

    // And this is what used to have to be remembered by hand: after consuming, empty.
    std::vector<GizmoVertex> segunda = Gizmos::takeVertices();
    CHECK(segunda.empty());
}

// draw() is the consumer of the Vulkan path, and it has to empty JUST like
// takeVertices. With the pipeline not created it returns before touching the API
// (which is what allows calling it here without a device), and that early return
// is exactly the one that left the vertices inside: if the buffer were not emptied
// there, an engine on which init() has not been called would accumulate forever.
static void test_draw_sin_pipeline_tambien_vacia()
{
    empezarLimpio();

    Gizmos::drawLine(glm::vec3(0.0f), glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(1.0f));
    CHECK(Gizmos::takeVertices().size() == 2);   // they were there

    Gizmos::drawLine(glm::vec3(0.0f), glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(1.0f));
    Gizmos::draw(VK_NULL_HANDLE, glm::mat4(1.0f), 0);
    CHECK(Gizmos::takeVertices().empty());
}

static void test_discard_vacia_sin_consumir()
{
    empezarLimpio();

    Gizmos::drawLine(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(1.0f));
    Gizmos::discard();
    CHECK(Gizmos::takeVertices().empty());
}

// When off it does not accumulate: it is what makes the editor checkbox not cost
// memory instead of merely not drawing.
static void test_apagado_no_acumula()
{
    empezarLimpio();

    Gizmos::setEnabled(false);
    CHECK(!Gizmos::isEnabled());
    Gizmos::drawLine(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(1.0f));
    Gizmos::drawWireSphere(glm::mat4(1.0f), glm::vec3(0.0f), 1.0f, glm::vec3(1.0f));
    CHECK(Gizmos::takeVertices().empty());

    Gizmos::setEnabled(true);
    CHECK(Gizmos::isEnabled());
    Gizmos::drawLine(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(1.0f));
    CHECK(Gizmos::takeVertices().size() == 2);
}

// The cap exists so that a runaway loop does not eat the memory. It is checked
// that it cuts off, and above all that it does NOT grow beyond: that was the
// symptom in which the leak showed up.
static void test_tope_de_capacidad()
{
    empezarLimpio();

    for (int i = 0; i < 40000; i++)
        Gizmos::drawLine(glm::vec3((float)i), glm::vec3((float)i + 1.0f), glm::vec3(1.0f));

    const size_t total = Gizmos::takeVertices().size();
    CHECK(total <= 65536);
    // And that it really reached the cap, not that it fell short for another reason:
    // 40000 lines are 80000 vertices, more than double the limit.
    CHECK(total > 60000);
}

int main()
{
    test_acumula_y_take_vacia();
    test_draw_sin_pipeline_tambien_vacia();
    test_discard_vacia_sin_consumir();
    test_apagado_no_acumula();
    test_tope_de_capacidad();

    if (g_failures == 0) std::printf("gizmos_tests: OK\n");
    else                 std::printf("gizmos_tests: %d FALLOS\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
