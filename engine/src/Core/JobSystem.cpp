#include "DonTopo/Core/JobSystem.h"

#include <algorithm>

namespace DonTopo
{
    void JobSystem::start(unsigned threads)
    {
        if (threads == 0)
        {
            const unsigned hw = std::thread::hardware_concurrency();
            // hw puede devolver 0 si el SO no lo sabe; el clamp lo cubre.
            const unsigned avail = (hw > 1) ? (hw - 1) : 1;
            threads = std::clamp(avail, 2u, 8u);
        }

        // El lock se mantiene durante todo el spawn: si no, dos start()
        // concurrentes pueden pasar los dos el "if (!m_threads.empty())"
        // (check-then-act sin protección) y arrancar el doble de hilos. Es
        // seguro tenerlo cogido aquí: un worker recién creado simplemente se
        // bloquea en m_cv.wait hasta que este scope suelte el mutex, no hay
        // deadlock.
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_threads.empty()) return;   // ya arrancado
        m_stop = false;

        m_threads.reserve(threads);
        for (unsigned i = 0; i < threads; ++i)
            m_threads.emplace_back([this] { workerLoop(); });
    }

    void JobSystem::shutdown()
    {
        std::vector<std::thread> threadsToJoin;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // Idempotencia real (no solo cosmética): el destructor llama a
            // shutdown(), y el usuario también puede — los dos caminos, o dos
            // shutdown() concurrentes, tienen que convivir. Sin esta guarda,
            // la llamada que ve m_threads ya vacío (porque la otra ya hizo el
            // move de abajo) NO tiene hilos que unir -su join es instantáneo-
            // y llega en microsegundos al m_queue.clear() de más abajo. Si en
            // ese momento la OTRA llamada sigue bloqueada en join() esperando
            // a un worker que todavía está drenando la cola real, ese clear()
            // se la vacía por debajo: jobs ya encolados se descartan sin
            // correr y sin que nadie se entere. Con la guarda, la llamada que
            // pierde la carrera por el mutex retorna aquí mismo y nunca llega
            // a tocar m_queue.
            if (m_threads.empty()) return;
            m_stop = true;
            // Vaciar el vector aquí, bajo el lock, es lo que hace que
            // m_threads.empty() sea una lectura fiable para submit()/
            // threadCount()/otro shutdown() mientras el join (lento, fuera
            // del lock) está en marcha.
            threadsToJoin = std::move(m_threads);
        }
        m_cv.notify_all();

        for (auto& t : threadsToJoin)
            if (t.joinable()) t.join();

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
