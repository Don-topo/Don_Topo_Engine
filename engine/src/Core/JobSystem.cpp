#include "DonTopo/Core/JobSystem.h"

#include <algorithm>

namespace DonTopo
{
    void JobSystem::start(unsigned threads)
    {
        if (threads == 0)
        {
            const unsigned hw = std::thread::hardware_concurrency();
            // hw may return 0 if the OS does not know; the clamp covers it.
            const unsigned avail = (hw > 1) ? (hw - 1) : 1;
            threads = std::clamp(avail, 2u, 8u);
        }

        // The lock is held for the whole spawn: otherwise two concurrent start()
        // calls could both pass the "if (!m_threads.empty())"
        // (unprotected check-then-act) and start twice the threads. It is
        // safe to hold it here: a freshly created worker simply
        // blocks in m_cv.wait until this scope releases the mutex, there is no
        // deadlock.
        std::unique_lock<std::mutex> lock(m_mutex);

        // Wait for a concurrent shutdown() to finish COMPLETELY (including
        // its final m_queue/m_cancelled cleanup) before checking whether we have
        // to start. Without this: a winning shutdown() moves m_threads and leaves it
        // empty, releases the mutex to join (slow) the old threads, and in that
        // window a start() would see m_threads empty and
        // launch a new, already running pool — so that the old shutdown(),
        // when re-acquiring the mutex, would empty the queue of the NEW pool
        // with its closing m_queue.clear(). m_shuttingDown closes that
        // window: while it is set, start() touches nothing.
        m_shutdownCv.wait(lock, [this] { return !m_shuttingDown; });

        if (!m_threads.empty()) return;   // already started
        m_stop = false;

        m_threads.reserve(threads);
        for (unsigned i = 0; i < threads; ++i)
            m_threads.emplace_back([this] { workerLoop(); });
    }

    void JobSystem::shutdown()
    {
        std::vector<std::thread> threadsToJoin;
        {
            std::unique_lock<std::mutex> lock(m_mutex);

            // Another shutdown() call is already in progress (from another thread:
            // two manual shutdown() calls, or a manual one next to the destructor's
            // at the end of the same scope). Instead of returning right away -which is
            // what the original guard did-, we WAIT for the winner to
            // finish draining, notifying the workers and joining, and we
            // return right after: for this call there is nothing left
            // to do, everything that had to be stopped is already stopped. If we
            // did not wait, "shutdown() has returned" would stop meaning
            // "no worker is alive" for the one that loses the race:
            // it could return with the winner still blocked in join(), and
            // if the loser is the destructor, it destroys
            // m_mutex/m_cv/m_queue with real workers still using them.
            if (m_shuttingDown)
            {
                m_shutdownCv.wait(lock, [this] { return !m_shuttingDown; });
                return;
            }

            // Idempotence for the purely sequential case (nobody else
            // calling at the same time): if there are no threads to stop, a full
            // shutdown was already done before and nobody is doing it now
            // -if someone were, the branch above would already have made us
            // wait and return-. Note that this guard is NO LONGER the one that
            // avoids losing jobs in the concurrent race -that is now done by
            // the branch above by waiting instead of returning right away-; without
            // it, a redundant sequential call (e.g. the destructor
            // after a manual shutdown()) would repeat the whole "empty, notify,
            // join nothing, clean nothing" without harm -m_threads and
            // m_queue are already empty-, just extra work.
            if (m_threads.empty()) return;

            m_shuttingDown = true;
            m_stop         = true;
            // Emptying the vector here, under the lock, is what makes
            // m_threads.empty() a reliable read for submit()/
            // threadCount()/start()/another shutdown() while the join (slow,
            // outside the lock) is in progress.
            threadsToJoin = std::move(m_threads);
            // move-assignment leaves the source "valid but unspecified",
            // it does not guarantee empty by the standard — in practice all
            // implementations leave it empty, but the guard above
            // (m_threads.empty()) depends on it being so ALWAYS. clear()
            // turns that implementation detail into a real guarantee.
            m_threads.clear();
        }
        m_cv.notify_all();

        for (auto& t : threadsToJoin)
            if (t.joinable()) t.join();

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.clear();
            m_cancelled.clear();
            m_shuttingDown = false;
        }
        // Outside the lock: whoever was waiting (another losing shutdown(), or a
        // start() waiting for its turn) only needs to wake up and re-acquire
        // the mutex, it does not need to be held to notify.
        m_shutdownCv.notify_all();
    }

    JobSystem::JobId JobSystem::submit(std::function<void()> fn)
    {
        JobId id;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_threads.empty() || m_stop) return 0;
            id = m_nextId++;
            m_queue.push_back(Job{id, std::move(fn)});
        }
        m_cv.notify_one();
        return id;
    }

    JobSystem::JobId JobSystem::reserveId()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_nextId++;
    }

    bool JobSystem::submitWithId(JobId id, std::function<void()> fn)
    {
        if (id == 0) return false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_threads.empty() || m_stop) return false;
            m_queue.push_back(Job{id, std::move(fn)});
        }
        m_cv.notify_one();
        return true;
    }

    void JobSystem::cancel(JobId id)
    {
        if (id == 0) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cancelled.insert(id);
    }

    size_t JobSystem::pendingCancellations() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cancelled.size();
    }

    bool JobSystem::idle() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.empty() && m_inFlight == 0;
    }

    unsigned JobSystem::threadCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<unsigned>(m_threads.size());
    }

    void JobSystem::workerLoop()
    {
        for (;;)
        {
            Job job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });

                // With m_stop and an empty queue it exits. With m_stop and a full queue it does NOT
                // exit: shutdown() promises to drain what was enqueued. Otherwise, a
                // Load Scene cancelled halfway would leave GameObjects without a mesh and
                // with nobody to report it.
                if (m_queue.empty()) return;

                job = std::move(m_queue.front());
                m_queue.pop_front();

                if (m_cancelled.erase(job.id) > 0)
                    continue;   // cancelled before starting: it is not even run

                ++m_inFlight;
            }

            // An exception escaping from here is std::terminate: the thread has
            // nobody above it to catch it. Every real job already
            // converts its failures into an error string, but the catch(...) is
            // the safety net so that none is forgotten.
            try { job.fn(); }
            catch (...) { }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                --m_inFlight;
                // The cancellation mark is removed HERE TOO, not only when
                // dequeuing. A cancel() that arrives with the job already running cannot
                // stop it —the header already says so: an Assimp::ReadFile
                // is not interrupted halfway— but its id stayed in the set
                // for the rest of the pool's life, because the worker had already
                // dequeued it and nobody was going to look at it again. And that is
                // precisely the case that really happens: AsyncAssetLoader cancels
                // in-flight loads, so a long editor session with
                // many Load Scenes made the set grow and never shrink.
                //
                // Removing it here changes nothing of what was already decided: the
                // job has finished, and whoever discards its result is the
                // consumer (pumpCompleted), which does not consult this set.
                m_cancelled.erase(job.id);
            }
        }
    }
}
