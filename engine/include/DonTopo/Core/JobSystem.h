#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace DonTopo
{
    // Generic thread pool: it knows nothing about assets, Vulkan or the scene. It is used by
    // AsyncAssetLoader, but it is tested on its own.
    //
    // It is NOT a singleton on purpose: the app creates its own in main() and passes it by
    // reference, so tests set one up per case with no global state between
    // them.
    class JobSystem
    {
        public:
            using JobId = uint64_t;

            // The destructor DRAINS (calls shutdown(), which runs what is pending),
            // it does not discard. Anything a job captures by reference has
            // to outlive the JobSystem: if it is declared after it in the same
            // scope, the JobSystem destructor runs first (reverse order of
            // declaration) and there is no problem; if it lives elsewhere, it is up to the
            // caller to guarantee the order.
            JobSystem() = default;
            ~JobSystem() { shutdown(); }
            JobSystem(const JobSystem&)            = delete;
            JobSystem& operator=(const JobSystem&) = delete;

            // threads == 0 → clamp(hardware_concurrency() - 1, 2, 8). The -1 leaves
            // one core for the main thread, which is the one that draws. Calling twice
            // without a shutdown() in between is a no-op.
            void start(unsigned threads = 0);

            // Drains the queue (runs what is pending), stops the threads and
            // joins. When shutdown() returns, ALL the workers are
            // stopped and joined — this holds for any caller, not only
            // for the one that "wins" the race if there are two concurrent explicit
            // calls (e.g. two threads calling shutdown() by hand).
            // The one that loses the race for the mutex WAITS for the
            // winner to finish draining and joining before returning, instead
            // of returning right away with the winner's workers still
            // alive. That matters because the first step of an orderly
            // teardown (e.g. before vkDeviceWaitIdle) is "no worker is
            // alive any more", and that guarantee has to hold for
            // any of the calls, not only for the one that reached the
            // mutex first.
            //
            // This does NOT cover destroying the JobSystem while another thread is still
            // calling one of its members: that is undefined behavior of the
            // language (the object ceases to exist under the feet of that
            // call) and no internal synchronization can defend against
            // that. shutdown() resolves the race between concurrent MEMBER calls, not
            // destruction concurrent with a call.
            void shutdown();

            // Returns 0 if the pool is not started — the job is NOT run.
            JobId submit(std::function<void()> fn);

            // Reserves a JobId without enqueuing anything, and enqueues with an already
            // reserved id. They exist so that a job can know its OWN id
            // from the inside: with plain submit(), the id is only known on
            // return, and a fast worker may have started already. Reading it
            // from the lambda then is a race.
            JobId reserveId();

            // Returns false if the pool is not started — the job is NOT
            // enqueued and fn is discarded. The caller already has the id (from
            // reserveId()): with false it knows that id will never complete
            // or fail, and can react (e.g. not add it to a progress counter
            // that would otherwise never reach zero).
            bool submitWithId(JobId id, std::function<void()> fn);

            // Marks id as cancelled. A job already started is NOT interrupted (an
            // Assimp::ReadFile cannot be stopped halfway): it finishes and the
            // consumer is the one who discards its result.
            void cancel(JobId id);

            // How many cancellation marks are still alive. Diagnostic: in the normal regime
            // it drops to 0 by itself -the worker removes the mark when discarding the job,
            // and also when finishing it if the cancel arrived with it already in flight-, so
            // a number that only goes up says someone cancels ids that this
            // pool never got to see.
            size_t   pendingCancellations() const;

            bool     idle() const;
            unsigned threadCount() const;

        private:
            struct Job
            {
                JobId                 id;
                std::function<void()> fn;
            };

            void workerLoop();

            mutable std::mutex       m_mutex;
            std::condition_variable  m_cv;          // queue signal: used by the workers.
            // Handshake of concurrent shutdown(). Deliberately SEPARATE from
            // m_cv: reusing m_cv risks a lost wakeup (a notify_all()
            // from shutdown() could be "spent" on a worker that was waiting for the
            // queue, not on the caller waiting for the winning shutdown
            // to finish). start() also waits here before starting, so as
            // not to launch a new pool while a shutdown() in progress still
            // has its final cleanup pending (see JobSystem.cpp).
            std::condition_variable  m_shutdownCv;
            std::deque<Job>          m_queue;
            std::unordered_set<JobId> m_cancelled;
            std::vector<std::thread> m_threads;
            JobId                    m_nextId       = 1;
            int                      m_inFlight     = 0;
            bool                     m_stop         = false;
            bool                     m_shuttingDown = false;
    };
}
