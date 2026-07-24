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
    // Pool de hilos genérico: no sabe nada de assets, Vulkan ni escena. Lo usa
    // AsyncAssetLoader, pero se testea solo.
    //
    // NO es singleton a propósito: la app crea la suya en main() y la pasa por
    // referencia, así los tests montan una por caso sin estado global entre
    // ellos.
    class JobSystem
    {
        public:
            using JobId = uint64_t;

            JobSystem() = default;
            ~JobSystem() { shutdown(); }
            JobSystem(const JobSystem&)            = delete;
            JobSystem& operator=(const JobSystem&) = delete;

            // threads == 0 → clamp(hardware_concurrency() - 1, 2, 8). El -1 deja
            // un core para el hilo principal, que es quien pinta. Llamar dos
            // veces sin shutdown() entre medias es un no-op.
            void start(unsigned threads = 0);

            // Drena la cola (ejecuta lo pendiente), para los hilos y hace join.
            // Idempotente: el destructor lo llama y el usuario también puede.
            void shutdown();

            // Devuelve 0 si el pool no está arrancado — el job NO se ejecuta.
            JobId submit(std::function<void()> fn);

            // Reserva un JobId sin encolar nada, y encola con un id ya
            // reservado. Existen para que un job pueda conocer su PROPIO id
            // desde dentro: con submit() a secas, el id solo se conoce al
            // retornar, y un worker rápido puede haber arrancado ya. Leerlo
            // entonces desde el lambda es una carrera.
            JobId reserveId();
            void  submitWithId(JobId id, std::function<void()> fn);

            // Marca id como cancelado. Un job ya arrancado NO se interrumpe (no
            // se puede parar un Assimp::ReadFile a medias): termina y es el
            // consumidor quien descarta su resultado.
            void cancel(JobId id);

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
            std::condition_variable  m_cv;
            std::deque<Job>          m_queue;
            std::unordered_set<JobId> m_cancelled;
            std::vector<std::thread> m_threads;
            JobId                    m_nextId   = 1;
            int                      m_inFlight = 0;
            bool                     m_stop     = false;
    };
}
