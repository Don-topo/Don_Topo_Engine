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

            // El destructor DRENA (llama a shutdown(), que ejecuta lo pendiente),
            // no descarta. Cualquier cosa que un job capture por referencia tiene
            // que vivir más que el JobSystem: si se declara después en el mismo
            // scope, el destructor del JobSystem corre primero (orden inverso de
            // declaración) y no hay problema; si vive en otro sitio, es cosa del
            // llamador garantizar el orden.
            JobSystem() = default;
            ~JobSystem() { shutdown(); }
            JobSystem(const JobSystem&)            = delete;
            JobSystem& operator=(const JobSystem&) = delete;

            // threads == 0 → clamp(hardware_concurrency() - 1, 2, 8). El -1 deja
            // un core para el hilo principal, que es quien pinta. Llamar dos
            // veces sin shutdown() entre medias es un no-op.
            void start(unsigned threads = 0);

            // Drena la cola (ejecuta lo pendiente), para los hilos y hace
            // join. Cuando shutdown() retorna, TODOS los workers están
            // parados y unidos — esto vale para cualquier llamador, no solo
            // para el que "gana" la carrera si hay dos llamadas explícitas
            // concurrentes (p.ej. dos hilos que llaman shutdown() a mano).
            // La que pierde la carrera por el mutex ESPERA a que la
            // ganadora termine de drenar y hacer join antes de retornar, en
            // vez de retornar ya con los workers de la ganadora todavía
            // vivos. Eso importa porque el primer paso de un teardown
            // ordenado (p.ej. antes de vkDeviceWaitIdle) es "ya no queda
            // ningún worker vivo", y esa garantía tiene que valer para
            // cualquiera de las llamadas, no solo para la que llegó primero
            // al mutex.
            //
            // Esto NO cubre destruir el JobSystem mientras otro hilo sigue
            // llamando a uno de sus miembros: eso es undefined behavior del
            // lenguaje (el objeto deja de existir bajo los pies de esa
            // llamada) y ninguna sincronización interna puede defenderse de
            // eso. shutdown() resuelve la carrera entre llamadas a MIEMBROS
            // concurrentes, no la destrucción concurrente con una llamada.
            void shutdown();

            // Devuelve 0 si el pool no está arrancado — el job NO se ejecuta.
            JobId submit(std::function<void()> fn);

            // Reserva un JobId sin encolar nada, y encola con un id ya
            // reservado. Existen para que un job pueda conocer su PROPIO id
            // desde dentro: con submit() a secas, el id solo se conoce al
            // retornar, y un worker rápido puede haber arrancado ya. Leerlo
            // entonces desde el lambda es una carrera.
            JobId reserveId();

            // Devuelve false si el pool no está arrancado — el job NO se
            // encola y fn se descarta. El llamador ya tiene el id (de
            // reserveId()): con false sabe que ese id nunca va a completarse
            // ni a fallar, y puede reaccionar (p.ej. no sumarlo a un contador
            // de progreso que si no, nunca llegaría a cero).
            bool submitWithId(JobId id, std::function<void()> fn);

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
            std::condition_variable  m_cv;          // señal de cola: la usan los workers.
            // Handshake de shutdown() concurrente. Deliberadamente SEPARADA de
            // m_cv: reutilizar m_cv arriesga un lost wakeup (un notify_all()
            // de shutdown() podría "gastarse" en un worker que esperaba por
            // cola, no en el llamador que espera a que termine el shutdown
            // ganador). start() también espera aquí antes de arrancar, para
            // no lanzar un pool nuevo mientras un shutdown() en curso todavía
            // tiene pendiente su limpieza final (ver JobSystem.cpp).
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
