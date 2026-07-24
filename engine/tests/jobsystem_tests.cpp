// Tests del pool de hilos. Plain main + asserts, sin framework — coherente con
// audio_tests.cpp y camera_tests.cpp.
//
// Cada caso corre kIters veces: un race que aparece 1 de cada 20 ejecuciones no
// se caza en una sola pasada, y un test de concurrencia que solo corre una vez
// da una falsa sensación de cobertura.
#include "DonTopo/Core/JobSystem.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <vector>

namespace {

constexpr int kIters = 50;

// 1000 jobs incrementando un atomic dan exactamente 1000. Sabotaje: cambiar el
// atomic por un int normal — con -fsanitize=thread o repitiendo, la suma baja.
void testAllJobsRun()
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start();
        std::atomic<int> counter{0};
        for (int i = 0; i < 1000; ++i)
            js.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
        js.shutdown();
        assert(counter.load() == 1000 && "shutdown debe drenar la cola entera");
    }
}

// shutdown() con la cola llena ejecuta TODO lo encolado, no lo tira. Sabotaje:
// poner m_stop = true antes de drenar en shutdown() — el contador se queda corto.
void testShutdownDrains()
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start(1);   // 1 hilo garantiza que la cola se acumula de verdad
        std::atomic<int> counter{0};
        for (int i = 0; i < 200; ++i)
            js.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
        js.shutdown();
        assert(counter.load() == 200 && "un shutdown no puede descartar jobs encolados");
    }
}

// cancel() de un job todavía en cola impide que corra. Con 1 hilo y un job
// bloqueante por delante, el cancelado no ha podido arrancar. Sabotaje: ignorar
// el flag de cancelación en el worker — el contador sube a 1.
void testCancelPreventsQueuedJob()
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start(1);

        std::atomic<bool> release{false};
        std::atomic<int>  ran{0};

        js.submit([&release] { while (!release.load(std::memory_order_acquire)) {} });
        DonTopo::JobSystem::JobId victim =
            js.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });

        js.cancel(victim);
        release.store(true, std::memory_order_release);
        js.shutdown();

        assert(ran.load() == 0 && "un job cancelado antes de arrancar no debe ejecutarse");
    }
}

// shutdown() dos veces no cuelga ni peta. Sabotaje: quitar la guarda de
// idempotencia — el segundo join sobre hilos ya unidos aborta o deadlockea.
void testDoubleShutdown()
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start();
        js.submit([] {});
        js.shutdown();
        js.shutdown();
    }
}

// start(1) es válido: cubre el clamp inferior sin depender del hardware.
void testSingleThread()
{
    DonTopo::JobSystem js;
    js.start(1);
    assert(js.threadCount() == 1);
    std::atomic<int> counter{0};
    for (int i = 0; i < 50; ++i)
        js.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
    js.shutdown();
    assert(counter.load() == 50);
}

// start(0) aplica clamp(hardware_concurrency()-1, 2, 8): nunca 0, nunca >8.
void testAutoThreadCountClamped()
{
    DonTopo::JobSystem js;
    js.start(0);
    const unsigned n = js.threadCount();
    assert(n >= 2 && n <= 8 && "el clamp automatico debe caer en [2,8]");
    js.shutdown();
}

// Un job que lanza no tumba el proceso: el worker lo traga. Sabotaje: quitar el
// try/catch del worker — std::terminate y el test no llega a imprimir OK.
void testJobExceptionDoesNotTerminate()
{
    DonTopo::JobSystem js;
    js.start(2);
    js.submit([] { throw std::runtime_error("boom"); });
    std::atomic<int> after{0};
    js.submit([&after] { after.fetch_add(1, std::memory_order_relaxed); });
    js.shutdown();
    assert(after.load() == 1 && "una excepcion en un job no puede matar al worker");
}

// reserveId() da ids unicos y submitWithId() los respeta: el job ve su propio
// id sin la carrera de leerlo despues de submit(). Sabotaje: hacer que
// reserveId devuelva siempre 1 — el assert de unicidad salta.
void testReserveIdIsUniqueAndUsable()
{
    DonTopo::JobSystem js;
    js.start(2);

    const DonTopo::JobSystem::JobId a = js.reserveId();
    const DonTopo::JobSystem::JobId b = js.reserveId();
    assert(a != 0 && b != 0 && a != b && "cada reserveId da un id distinto y no nulo");

    std::atomic<uint64_t> seen{0};
    js.submitWithId(a, [&seen, a] { seen.store(a, std::memory_order_relaxed); });
    js.shutdown();

    assert(seen.load() == a && "el job debe poder capturar su propio id ya relleno");
}

// Un id reservado y cancelado antes de submitWithId no llega a ejecutarse.
// Sabotaje: ignorar m_cancelled en el worker — el contador sube.
void testCancelBeforeSubmitWithId()
{
    DonTopo::JobSystem js;
    js.start(1);

    const DonTopo::JobSystem::JobId id = js.reserveId();
    js.cancel(id);

    std::atomic<int> ran{0};
    js.submitWithId(id, [&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    js.shutdown();

    assert(ran.load() == 0 && "cancelar antes de encolar tambien debe impedir la ejecucion");
}

} // namespace

int main()
{
    testAllJobsRun();
    testShutdownDrains();
    testCancelPreventsQueuedJob();
    testDoubleShutdown();
    testSingleThread();
    testAutoThreadCountClamped();
    testJobExceptionDoesNotTerminate();
    testReserveIdIsUniqueAndUsable();
    testCancelBeforeSubmitWithId();
    std::printf("jobsystem_tests OK\n");
    return 0;
}
