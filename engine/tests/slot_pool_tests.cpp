// Headless test of SlotPool, the free-slot list with which the two backends
// recycle the object slots instead of growing endlessly (P11/P13,
// H19/H32/H43).
//
// The pool does not touch the GPU, so it is tested in full without a device. What
// is verified here is the only thing that can corrupt the render: that a slot is
// NOT handed out twice. If that happens, two different GameObjects write to the
// same slot and the same descriptor block, and nobody reports it.
#include "DonTopo/Renderer/SlotPool.h"

#include <cstdio>
#include <set>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// With nothing freed there is no slot to give: the caller has to grow the
// vector, which is today's behavior.
static void test_pool_vacio_no_da_hueco()
{
    SlotPool pool;
    CHECK(pool.acquire() == -1);
    CHECK(pool.freeCount() == 0);
}

static void test_lo_liberado_se_reutiliza()
{
    SlotPool pool;
    pool.release(7);
    CHECK(pool.freeCount() == 1);
    CHECK(pool.acquire() == 7);
    CHECK(pool.freeCount() == 0);
    CHECK(pool.acquire() == -1);
}

// LIFO: the last one freed is the first to come back. Reusing the most recent
// slot keeps hot the entries that the previous frame already touched.
static void test_orden_lifo()
{
    SlotPool pool;
    pool.release(1);
    pool.release(2);
    pool.release(3);
    CHECK(pool.acquire() == 3);
    CHECK(pool.acquire() == 2);
    CHECK(pool.acquire() == 1);
}

// THE test of this file. Freeing the same slot twice (a removeGameObject
// on a subtree that had already been removed, or a Ctrl+Z that redoes a delete)
// would leave the index twice in the list, and two NEW objects would end up
// sharing a slot and a descriptor block. The second free is ignored.
static void test_liberar_dos_veces_no_duplica_el_hueco()
{
    SlotPool pool;
    pool.release(4);
    pool.release(4);
    CHECK(pool.freeCount() == 1);
    CHECK(pool.acquire() == 4);
    CHECK(pool.acquire() == -1);
}

// And after being handed out again, that same slot can be freed once more:
// the block is "already free", not "was freed at some point".
static void test_un_hueco_reentregado_se_puede_liberar_de_nuevo()
{
    SlotPool pool;
    pool.release(4);
    CHECK(pool.acquire() == 4);
    pool.release(4);
    CHECK(pool.acquire() == 4);
}

// Negative index: produced by any unassigned *RenderIndex. Ignoring it
// here avoids repeating the guard in every caller.
static void test_indice_negativo_se_ignora()
{
    SlotPool pool;
    pool.release(-1);
    CHECK(pool.freeCount() == 0);
    CHECK(pool.acquire() == -1);
}

// clear() is called by clearStaticMeshes and the shutdown: the object vectors are
// emptied entirely, so no old slot is still valid.
static void test_clear_olvida_los_huecos()
{
    SlotPool pool;
    pool.release(0);
    pool.release(1);
    pool.clear();
    CHECK(pool.freeCount() == 0);
    CHECK(pool.acquire() == -1);
}

// The reason to exist: a repeated Play/Stop cycle cannot keep raising the
// number of slots. With the pool, N create/delete cycles always reuse the same one.
static void test_ciclos_repetidos_no_crecen()
{
    SlotPool pool;
    std::set<int> vistos;
    int siguienteNuevo = 0;

    for (int ciclo = 0; ciclo < 100; ++ciclo)
    {
        int slot = pool.acquire();
        if (slot < 0) slot = siguienteNuevo++;   // what the caller does: grow
        vistos.insert(slot);
        pool.release(slot);
    }

    CHECK(siguienteNuevo == 1);      // only the first cycle had to grow
    CHECK(vistos.size() == 1u);
}

// Several alive at once: the pool never hands out an occupied slot.
static void test_nunca_entrega_dos_veces_el_mismo_hueco()
{
    SlotPool pool;
    for (int i = 0; i < 8; ++i) pool.release(i);

    std::set<int> entregados;
    for (int i = 0; i < 8; ++i)
    {
        const int slot = pool.acquire();
        CHECK(slot >= 0);
        CHECK(entregados.insert(slot).second);   // false = duplicate
    }
    CHECK(pool.acquire() == -1);
}

int main()
{
    test_pool_vacio_no_da_hueco();
    test_lo_liberado_se_reutiliza();
    test_orden_lifo();
    test_liberar_dos_veces_no_duplica_el_hueco();
    test_un_hueco_reentregado_se_puede_liberar_de_nuevo();
    test_indice_negativo_se_ignora();
    test_clear_olvida_los_huecos();
    test_ciclos_repetidos_no_crecen();
    test_nunca_entrega_dos_veces_el_mismo_hueco();

    if (g_failures == 0) std::printf("ALL SLOT POOL TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
