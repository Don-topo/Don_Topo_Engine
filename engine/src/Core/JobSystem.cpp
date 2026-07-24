#include "DonTopo/Core/JobSystem.h"

#include <algorithm>

namespace DonTopo
{
    void JobSystem::start(unsigned threads)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_threads.empty()) return;   // ya arrancado
            m_stop = false;
        }

        if (threads == 0)
        {
            const unsigned hw = std::thread::hardware_concurrency();
            // hw puede devolver 0 si el SO no lo sabe; el clamp lo cubre.
            const unsigned avail = (hw > 1) ? (hw - 1) : 1;
            threads = std::clamp(avail, 2u, 8u);
        }

        m_threads.reserve(threads);
        for (unsigned i = 0; i < threads; ++i)
            m_threads.emplace_back([this] { workerLoop(); });
    }

    void JobSystem::shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // Idempotencia: sin esto, el segundo shutdown() haría join sobre
            // hilos ya unidos y abortaría. El destructor llama a shutdown(), y
            // el usuario también: los dos caminos tienen que convivir.
            if (m_threads.empty()) return;
            m_stop = true;
        }
        m_cv.notify_all();

        for (auto& t : m_threads)
            if (t.joinable()) t.join();

        m_threads.clear();

        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.clear();
        m_cancelled.clear();
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

    void JobSystem::submitWithId(JobId id, std::function<void()> fn)
    {
        if (id == 0) return;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_threads.empty() || m_stop) return;
            m_queue.push_back(Job{id, std::move(fn)});
        }
        m_cv.notify_one();
    }

    void JobSystem::cancel(JobId id)
    {
        if (id == 0) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cancelled.insert(id);
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

                // Con m_stop y cola vacía se sale. Con m_stop y cola llena NO se
                // sale: shutdown() promete drenar lo encolado. Al revés, un
                // Load Scene cancelado a medias dejaría GameObjects sin mesh y
                // sin nadie que lo reporte.
                if (m_queue.empty()) return;

                job = std::move(m_queue.front());
                m_queue.pop_front();

                if (m_cancelled.erase(job.id) > 0)
                    continue;   // cancelado antes de arrancar: ni se ejecuta

                ++m_inFlight;
            }

            // Una excepción escapando de aquí es std::terminate: el hilo no
            // tiene a nadie por encima que la capture. Cada job de verdad ya
            // convierte sus fallos en un string de error, pero el catch(...) es
            // la red de seguridad de que ninguno se olvide.
            try { job.fn(); }
            catch (...) { }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                --m_inFlight;
            }
        }
    }
}
