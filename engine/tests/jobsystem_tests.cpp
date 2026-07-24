// Tests del pool de hilos. Plain main + asserts, sin framework — coherente con
// audio_tests.cpp y camera_tests.cpp.
//
// Cada caso corre kIters veces: un race que aparece 1 de cada 20 ejecuciones no
// se caza en una sola pasada, y un test de concurrencia que solo corre una vez
// da una falsa sensación de cobertura.
#include "DonTopo/Core/JobSystem.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

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

// shutdown() llamado dos veces EN PARALELO (dos hilos que llaman shutdown() a
// mano — no el destructor: destruir el objeto mientras otro código sigue
// llamando a uno de sus miembros es UB del lenguaje, no un caso que esta
// clase pueda soportar; lo que la clase SÍ garantiza es que dos llamadas
// explícitas a shutdown() convivan) no debe perder jobs ya encolados.
//
// Con 1 solo worker y un primer job bloqueado con "release", el worker está
// atascado y la cola real (50 jobs) sigue sin drenar cuando llegan los dos
// shutdown(). El mutex serializa quién "gana": el ganador mueve m_threads
// (con hilos de verdad) y se queda bloqueado en join() esperando a que el
// worker despierte. El perdedor ve m_shuttingDown a true y ESPERA en
// m_shutdownCv a que el ganador termine, en vez de retornar ya. Sabotaje:
// quitar la guarda de idempotencia ("if (m_threads.empty()) return;") — sin
// ella, el perdedor no espera ni retorna por ahí: sigue de largo, no tiene
// nada que unir (join instantáneo sobre un vector vacío) y llega en
// microsegundos al m_queue.clear() final, que se ejecuta MIENTRAS el worker
// sigue bloqueado sin haber tocado la cola real. Esos 50 jobs se descartan
// antes de correr — silenciosamente, como en el finding 2 pero por la puerta
// de atrás. Con la guarda puesta, el perdedor espera a que el ganador drene
// y una los hilos antes de retornar, y nunca llega a ese clear() por su
// cuenta.
void testDoubleShutdown()
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start(1);   // 1 hilo: no hay ambigüedad sobre quién drena qué.

        std::atomic<bool> release{false};
        std::atomic<int>  ran{0};

        js.submit([&release] { while (!release.load(std::memory_order_acquire)) {} });
        for (int i = 0; i < 50; ++i)
            js.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });

        std::thread t1([&js] { js.shutdown(); });
        std::thread t2([&js] { js.shutdown(); });

        // Dar tiempo a que el "perdedor" complete su camino corto -incluido
        // el clear() final- mientras el "ganador" sigue bloqueado en join().
        // Sin este margen la carrera existe igual pero con ventana estrecha.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        release.store(true, std::memory_order_release);

        t1.join();
        t2.join();

        assert(ran.load() == 50 && "shutdown() concurrente no debe perder jobs ya encolados");
        assert(js.threadCount() == 0 && "tras shutdown no deben quedar hilos");
        assert(js.idle() && "tras shutdown el pool debe estar idle");
    }
}

// shutdown() concurrente debe significar "todo ha terminado" para AMBOS
// llamadores, no solo para el que gana la carrera por el mutex. Un job
// deliberadamente lento (20ms) deja una ventana amplia: si el perdedor
// retornase ya (en vez de esperar en m_shutdownCv a que el ganador drene y
// una los hilos), su shutdown() volvería con el job todavía sin terminar.
// Cada hilo mira el flag justo al volver de SU PROPIA llamada a shutdown();
// con la espera puesta, los dos deben verlo ya en true.
//
// Sabotaje: en la rama "if (m_shuttingDown)" de shutdown(), cambiar el
// m_shutdownCv.wait(...) por un return inmediato (el perdedor ya no espera
// al ganador). Resultado esperado: seenByLoser da false en varias de las
// 50 iteraciones porque el hilo que pierde la carrera por el mutex retorna
// en microsegundos, mucho antes de que el job de 20ms haya podido terminar.
void testConcurrentShutdownWaitsForWinner()
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start(1);

        std::atomic<bool> finished{false};
        js.submit([&finished]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            finished.store(true, std::memory_order_release);
        });

        std::atomic<bool> seenByT1{false};
        std::atomic<bool> seenByT2{false};

        std::thread t1([&js, &finished, &seenByT1]
        {
            js.shutdown();
            seenByT1.store(finished.load(std::memory_order_acquire));
        });
        std::thread t2([&js, &finished, &seenByT2]
        {
            js.shutdown();
            seenByT2.store(finished.load(std::memory_order_acquire));
        });

        t1.join();
        t2.join();

        assert(seenByT1.load() && seenByT2.load() &&
               "shutdown() debe retornar solo cuando el job ya ha terminado, para las dos llamadas concurrentes");
    }
}

// start(1) es válido: cubre el clamp inferior sin depender del hardware.
void testSingleThread()
{
    for (int it = 0; it < kIters; ++it)
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
}

// start(0) aplica clamp(hardware_concurrency()-1, 2, 8): nunca 0, nunca >8.
void testAutoThreadCountClamped()
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start(0);
        const unsigned n = js.threadCount();
        assert(n >= 2 && n <= 8 && "el clamp automatico debe caer en [2,8]");
        js.shutdown();
    }
}

// idle() es lo que AsyncAssetLoader (Task 2) va a hacer polling para saber si
// terminó de cargar: falso mientras hay un job corriendo, verdadero cuando ya
// no. Sabotaje: quitar el "--m_inFlight" en workerLoop() tras job.fn() — idle()
// se queda en false para siempre y el assert final salta.
void testIdleReflectsInFlightJob()
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start(1);

        std::atomic<bool> release{false};
        std::atomic<bool> jobStarted{false};

        js.submit([&release, &jobStarted]
        {
            jobStarted.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {}
        });

        while (!jobStarted.load(std::memory_order_acquire)) {}
        assert(!js.idle() && "un job en ejecucion no puede reportar idle()");

        release.store(true, std::memory_order_release);
        js.shutdown();

        assert(js.idle() && "tras shutdown() sin jobs pendientes idle() debe ser true");
    }
}

// Un job que lanza no tumba el proceso: el worker lo traga. Sabotaje: quitar el
// try/catch del worker — std::terminate y el test no llega a imprimir OK.
void testJobExceptionDoesNotTerminate()
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start(2);
        js.submit([] { throw std::runtime_error("boom"); });
        std::atomic<int> after{0};
        js.submit([&after] { after.fetch_add(1, std::memory_order_relaxed); });
        js.shutdown();
        assert(after.load() == 1 && "una excepcion en un job no puede matar al worker");
    }
}

// reserveId() da ids unicos y submitWithId() los respeta: el job ve su propio
// id sin la carrera de leerlo despues de submit(). Sabotaje: hacer que
// reserveId devuelva siempre 1 — el assert de unicidad salta.
//
// También se comprueba el bool que devuelve submitWithId(): true con el pool
// arrancado (si no, "return true;" a secas pasaría el test igual, dejando
// el fix del finding 2 de la primera review sin verificar), y false con un
// pool sin arrancar, donde el id ya se reservó pero el job nunca se encola.
void testReserveIdIsUniqueAndUsable()
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start(2);

        const DonTopo::JobSystem::JobId a = js.reserveId();
        const DonTopo::JobSystem::JobId b = js.reserveId();
        assert(a != 0 && b != 0 && a != b && "cada reserveId da un id distinto y no nulo");

        std::atomic<uint64_t> seen{0};
        assert(js.submitWithId(a, [&seen, a] { seen.store(a, std::memory_order_relaxed); }) &&
               "submitWithId debe devolver true con el pool arrancado");
        js.shutdown();

        assert(seen.load() == a && "el job debe poder capturar su propio id ya relleno");
    }

    // Pool nunca arrancado: submit() y submitWithId() deben rechazar el
    // trabajo (0 / false) en vez de encolarlo silenciosamente.
    {
        DonTopo::JobSystem js;
        assert(js.submit([] {}) == 0 && "submit() en un pool no arrancado debe devolver 0");
        assert(!js.submitWithId(js.reserveId(), [] {}) &&
               "submitWithId() en un pool no arrancado debe devolver false");
    }
}

// Un id reservado y cancelado antes de submitWithId no llega a ejecutarse.
// Sabotaje: ignorar m_cancelled en el worker — el contador sube.
void testCancelBeforeSubmitWithId()
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start(1);

        const DonTopo::JobSystem::JobId id = js.reserveId();
        js.cancel(id);

        std::atomic<int> ran{0};
        assert(js.submitWithId(id, [&ran] { ran.fetch_add(1, std::memory_order_relaxed); }) &&
               "submitWithId debe devolver true: el pool sigue arrancado, solo el job esta cancelado");
        js.shutdown();

        assert(ran.load() == 0 && "cancelar antes de encolar tambien debe impedir la ejecucion");
    }
}

} // namespace

int main()
{
    testAllJobsRun();
    testShutdownDrains();
    testCancelPreventsQueuedJob();
    testDoubleShutdown();
    testConcurrentShutdownWaitsForWinner();
    testIdleReflectsInFlightJob();
    testSingleThread();
    testAutoThreadCountClamped();
    testJobExceptionDoesNotTerminate();
    testReserveIdIsUniqueAndUsable();
    testCancelBeforeSubmitWithId();
    std::printf("jobsystem_tests OK\n");
    return 0;
}
