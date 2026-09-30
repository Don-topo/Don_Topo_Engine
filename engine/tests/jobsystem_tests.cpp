// Tests of the thread pool. Plain main + asserts, no framework, consistent with
// audio_tests.cpp and camera_tests.cpp.
//
// Each case runs kIters times: a race that shows up 1 out of 20 runs is not
// caught in a single pass, and a concurrency test that only runs once gives a
// false sense of coverage.
#include "DonTopo/Core/JobSystem.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace {

constexpr int kIters = 50;

// 1000 jobs incrementing an atomic give exactly 1000. Sabotage: replace the
// atomic with a plain int; with -fsanitize=thread or by repeating, the sum drops.
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

// shutdown() with a full queue runs EVERYTHING that was enqueued, it does not drop it.
// Sabotage: set m_stop = true before draining in shutdown(); the counter falls short.
void testShutdownDrains()
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start(1);   // 1 thread guarantees that the queue really builds up
        std::atomic<int> counter{0};
        for (int i = 0; i < 200; ++i)
            js.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
        js.shutdown();
        assert(counter.load() == 200 && "un shutdown no puede descartar jobs encolados");
    }
}

// cancel() of a job still in the queue prevents it from running. With 1 thread and a
// blocking job ahead of it, the cancelled one cannot have started. Sabotage: ignore
// the cancellation flag in the worker; the counter goes up to 1.
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

// shutdown() called twice IN PARALLEL (two threads that call shutdown() by
// hand, not the destructor: destroying the object while other code keeps
// calling one of its members is language UB, not a case that this
// class can support; what the class DOES guarantee is that two
// explicit calls to shutdown() coexist) must not lose jobs already enqueued.
//
// With only 1 worker and a first job blocked with "release", the worker is
// stuck and the real queue (50 jobs) is still undrained when the two
// shutdown() calls arrive. The mutex serializes who "wins": the winner moves m_threads
// (with real threads) and stays blocked in join() waiting for the
// worker to wake up. The loser sees m_shuttingDown as true and WAITS on
// m_shutdownCv for the winner to finish, instead of returning right away. Sabotage:
// remove the idempotence guard ("if (m_threads.empty()) return;"); without
// it, the loser neither waits nor returns there: it carries on, has
// nothing to join (instant join on an empty vector) and reaches in
// microseconds the final m_queue.clear(), which runs WHILE the worker
// is still blocked without having touched the real queue. Those 50 jobs are discarded
// before running, silently, as in finding 2 but through the back
// door. With the guard in place, the loser waits for the winner to drain
// and join the threads before returning, and never reaches that clear() on its
// own.
void testDoubleShutdown()
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start(1);   // 1 thread: no ambiguity about who drains what.

        std::atomic<bool> release{false};
        std::atomic<int>  ran{0};

        js.submit([&release] { while (!release.load(std::memory_order_acquire)) {} });
        for (int i = 0; i < 50; ++i)
            js.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });

        std::thread t1([&js] { js.shutdown(); });
        std::thread t2([&js] { js.shutdown(); });

        // Give time for the "loser" to complete its short path, including
        // the final clear(), while the "winner" is still blocked in join().
        // Without this margin the race exists all the same but with a narrow window.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        release.store(true, std::memory_order_release);

        t1.join();
        t2.join();

        assert(ran.load() == 50 && "shutdown() concurrente no debe perder jobs ya encolados");
        assert(js.threadCount() == 0 && "tras shutdown no deben quedar hilos");
        assert(js.idle() && "tras shutdown el pool debe estar idle");
    }
}

// Concurrent shutdown() must mean "everything has finished" for BOTH
// callers, not only for the one that wins the race for the mutex. A
// deliberately slow job (20ms) leaves a wide window: if the loser
// returned right away (instead of waiting on m_shutdownCv for the winner to drain
// and join the threads), its shutdown() would come back with the job still unfinished.
// Each thread looks at the flag right on returning from ITS OWN call to shutdown();
// with the wait in place, both must already see it as true.
//
// Sabotage: in the "if (m_shuttingDown)" branch of shutdown(), replace
// m_shutdownCv.wait(...) with an immediate return (the loser no longer waits
// for the winner). Expected result: seenByLoser gives false in several of the
// 50 iterations because the thread that loses the race for the mutex returns
// in microseconds, long before the 20ms job could have finished.
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

// start(1) is valid: it covers the lower clamp without depending on the hardware.
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

// start(0) applies clamp(hardware_concurrency()-1, 2, 8): never 0, never >8.
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

// idle() is what AsyncAssetLoader (Task 2) will poll to know whether it
// finished loading: false while a job is running, true once none is.
// Sabotage: remove the "--m_inFlight" in workerLoop() after job.fn(); idle()
// stays false forever and the final assert fires.
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

// A job that throws does not take down the process: the worker swallows it. Sabotage:
// remove the worker's try/catch; std::terminate and the test never gets to print OK.
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

// reserveId() gives unique ids and submitWithId() honors them: the job sees its own
// id without the race of reading it after submit(). Sabotage: make
// reserveId always return 1; the uniqueness assert fires.
//
// The bool returned by submitWithId() is also checked: true with the pool
// started (otherwise a bare "return true;" would pass the test all the same, leaving
// the fix for finding 2 of the first review unverified), and false with a
// pool not started, where the id was already reserved but the job is never enqueued.
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

    // Pool never started: submit() and submitWithId() must reject the
    // work (0 / false) instead of silently enqueuing it.
    {
        DonTopo::JobSystem js;
        assert(js.submit([] {}) == 0 && "submit() en un pool no arrancado debe devolver 0");
        assert(!js.submitWithId(js.reserveId(), [] {}) &&
               "submitWithId() en un pool no arrancado debe devolver false");
    }
}

// An id reserved and cancelled before submitWithId never gets to run.
// Sabotage: ignore m_cancelled in the worker; the counter goes up.
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

// H11 of docs/core-audit.md. m_cancelled was only emptied when the WORKER
// dequeued the cancelled job, so a cancel() that arrives when the job is ALREADY
// RUNNING leaves its id inside for the rest of the pool's life: the worker
// already dequeued it, nobody will look at it again.
//
// It is not a correctness bug (ids are never reused, m_nextId only goes up),
// but AsyncAssetLoader cancels in-flight loads, so in a long editor session with
// many Load Scene the set grows and never shrinks.
//
// The case of "cancelling something already finished" is not covered by this and does not need to
// be: the loader already detects that the result is in the mailbox and does NOT call cancel().
void testCancelOfRunningJobDoesNotLeak()
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start(1);

        std::atomic<bool> arrancado{false};
        std::atomic<bool> release{false};

        // The job signals that it is already INSIDE and keeps waiting: that guarantees
        // that the cancel below arrives with the job in flight, not in the queue.
        DonTopo::JobSystem::JobId enVuelo = js.submit([&arrancado, &release] {
            arrancado.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {}
        });

        while (!arrancado.load(std::memory_order_acquire)) {}
        js.cancel(enVuelo);
        assert(js.pendingCancellations() == 1 &&
               "el cancel de un job en vuelo se anota");

        release.store(true, std::memory_order_release);

        // Wait for the pool to run out of work: when the job finishes, its
        // mark has to have gone with it.
        while (!js.idle()) {}
        assert(js.pendingCancellations() == 0 &&
               "al terminar el job, su marca de cancelacion no puede quedarse");

        js.shutdown();
    }
}

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
    testCancelOfRunningJobDoesNotLeak();
    std::printf("jobsystem_tests OK\n");
    return 0;
}
