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
        std::unique_lock<std::mutex> lock(m_mutex);

        // Esperar a que un shutdown() concurrente termine DEL TODO (incluida
        // su limpieza final de m_queue/m_cancelled) antes de mirar si hay
        // que arrancar. Sin esto: un shutdown() ganador mueve m_threads y lo
        // deja vacío, suelta el mutex para hacer join (lento) de los hilos
        // viejos, y en esa ventana un start() vería m_threads vacío,
        // lanzaría un pool nuevo y ya arrancado — para que el shutdown()
        // viejo, al re-adquirir el mutex, le vaciara la cola al pool NUEVO
        // con su m_queue.clear() de cierre. m_shuttingDown cierra esa
        // ventana: mientras siga puesto, start() no toca nada.
        m_shutdownCv.wait(lock, [this] { return !m_shuttingDown; });

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
            std::unique_lock<std::mutex> lock(m_mutex);

            // Otra llamada a shutdown() ya está en marcha (desde otro hilo:
            // dos shutdown() manuales, o uno manual junto al del destructor
            // al final del mismo scope). En vez de retornar ya -que es lo
            // que hacía la guarda original-, ESPERAMOS a que la ganadora
            // termine de drenar, notificar a los workers y hacer join, y
            // retornamos justo después: para esta llamada ya no queda nada
            // que hacer, todo lo que había que parar ya está parado. Si no
            // esperáramos, "shutdown() ha retornado" dejaría de significar
            // "no queda ningún worker vivo" para el que pierde la carrera:
            // podría retornar con la ganadora todavía bloqueada en join(), y
            // si ese que pierde es el destructor, destruye
            // m_mutex/m_cv/m_queue con workers reales todavía usándolos.
            if (m_shuttingDown)
            {
                m_shutdownCv.wait(lock, [this] { return !m_shuttingDown; });
                return;
            }

            // Idempotencia para el caso puramente secuencial (nadie más
            // llamando a la vez): si no hay hilos que parar es que ya se
            // hizo shutdown del todo antes y nadie lo está haciendo ahora
            // -si lo estuviera, la rama de arriba ya nos habría hecho
            // esperar y retornar-. Nótese que esta guarda YA NO es la que
            // evita perder jobs en la carrera concurrente -eso ahora lo
            // hace la rama de arriba esperando en vez de retornar ya-; sin
            // ella, una llamada secuencial redundante (p.ej. el destructor
            // tras un shutdown() manual) repetiría el "vaciar, notificar,
            // unir nada, limpiar nada" entero sin hacer daño -m_threads y
            // m_queue ya están vacíos-, solo trabajo de más.
            if (m_threads.empty()) return;

            m_shuttingDown = true;
            m_stop         = true;
            // Vaciar el vector aquí, bajo el lock, es lo que hace que
            // m_threads.empty() sea una lectura fiable para submit()/
            // threadCount()/start()/otro shutdown() mientras el join (lento,
            // fuera del lock) está en marcha.
            threadsToJoin = std::move(m_threads);
            // move-asignación deja la fuente "válida pero no especificada",
            // no garantiza vacío por norma — en la práctica todas las
            // implementaciones la dejan vacía, pero la guarda de arriba
            // (m_threads.empty()) depende de que lo esté SIEMPRE. clear()
            // convierte ese detalle de implementación en garantía real.
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
        // Fuera del lock: a quien esperaba (otro shutdown() perdedor, o un
        // start() que aguardaba su turno) le basta con despertar y volver a
        // adquirir el mutex, no hace falta tenerlo cogido para notificar.
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
