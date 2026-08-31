// Test headless de SlotPool, la lista de huecos libres con la que los dos
// backends reciclan las ranuras de objeto en vez de crecer sin parar (P11/P13,
// H19/H32/H43).
//
// El pool no toca la GPU, asi que se prueba entero sin device. Lo que se
// verifica aqui es lo unico que puede corromper el render: que un hueco NO se
// entregue dos veces. Si eso pasa, dos GameObject distintos escriben en la
// misma ranura y en el mismo bloque de descriptores, y no lo avisa nadie.
#include "DonTopo/Renderer/SlotPool.h"

#include <cstdio>
#include <set>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Sin nada liberado no hay hueco que dar: el llamante tiene que crecer el
// vector, que es el comportamiento de hoy.
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

// LIFO: el ultimo en liberarse es el primero en volver. Reutilizar el hueco
// mas reciente mantiene calientes las entradas que el frame anterior ya tocó.
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

// EL test de este fichero. Liberar dos veces el mismo hueco —un removeGameObject
// sobre un subarbol que ya se habia quitado, o un Ctrl+Z que rehace un borrado—
// dejaria el indice dos veces en la lista, y dos objetos NUEVOS acabarian
// compartiendo ranura y bloque de descriptores. La segunda liberacion se ignora.
static void test_liberar_dos_veces_no_duplica_el_hueco()
{
    SlotPool pool;
    pool.release(4);
    pool.release(4);
    CHECK(pool.freeCount() == 1);
    CHECK(pool.acquire() == 4);
    CHECK(pool.acquire() == -1);
}

// Y despues de volver a entregarse, ese mismo hueco se puede liberar otra vez:
// el bloqueo es "ya esta libre", no "ya se libero alguna vez".
static void test_un_hueco_reentregado_se_puede_liberar_de_nuevo()
{
    SlotPool pool;
    pool.release(4);
    CHECK(pool.acquire() == 4);
    pool.release(4);
    CHECK(pool.acquire() == 4);
}

// Indice negativo: lo produce cualquier *RenderIndex sin asignar. Ignorarlo
// aqui evita repetir la guarda en cada llamante.
static void test_indice_negativo_se_ignora()
{
    SlotPool pool;
    pool.release(-1);
    CHECK(pool.freeCount() == 0);
    CHECK(pool.acquire() == -1);
}

// clear() lo llaman clearStaticMeshes y el apagado: los vectores de objetos se
// vacian enteros, asi que ningun hueco viejo sigue siendo valido.
static void test_clear_olvida_los_huecos()
{
    SlotPool pool;
    pool.release(0);
    pool.release(1);
    pool.clear();
    CHECK(pool.freeCount() == 0);
    CHECK(pool.acquire() == -1);
}

// El motivo de existir: un ciclo Play/Stop repetido no puede seguir subiendo el
// numero de ranuras. Con el pool, N crear/borrar reutilizan siempre la misma.
static void test_ciclos_repetidos_no_crecen()
{
    SlotPool pool;
    std::set<int> vistos;
    int siguienteNuevo = 0;

    for (int ciclo = 0; ciclo < 100; ++ciclo)
    {
        int slot = pool.acquire();
        if (slot < 0) slot = siguienteNuevo++;   // lo que hace el llamante: crecer
        vistos.insert(slot);
        pool.release(slot);
    }

    CHECK(siguienteNuevo == 1);      // solo el primer ciclo tuvo que crecer
    CHECK(vistos.size() == 1u);
}

// Varios vivos a la vez: el pool nunca entrega un hueco ocupado.
static void test_nunca_entrega_dos_veces_el_mismo_hueco()
{
    SlotPool pool;
    for (int i = 0; i < 8; ++i) pool.release(i);

    std::set<int> entregados;
    for (int i = 0; i < 8; ++i)
    {
        const int slot = pool.acquire();
        CHECK(slot >= 0);
        CHECK(entregados.insert(slot).second);   // false = repetido
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
