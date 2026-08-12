#pragma once
#include "DonTopo/Core/JobSystem.h"
#include "DonTopo/Renderer/Mesh.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace DonTopo
{
    // Una textura ya decodificada a RGBA8 por el worker. El hilo principal solo
    // hace el upload: el stbi_load, que es la mitad del coste de cargar un
    // modelo, ya ocurrió fuera.
    struct DecodedImage
    {
        enum Slot { Albedo, Normal, ORM };

        Slot                 slot = Albedo;
        int                  w    = 0;
        int                  h    = 0;
        std::vector<uint8_t> pixels;   // w*h*4, RGBA8, sin padding
    };

    // Resultado de una petición. Viaja por valor del worker al hilo principal:
    // ni un puntero compartido mutable, ni un GameObject*.
    struct LoadedMesh
    {
        JobSystem::JobId          job      = 0;
        uint64_t                  targetId = 0;   // GameObject::id, nunca un puntero
        std::string               path;
        std::shared_ptr<Mesh>     mesh;           // puede ser SkinnedMesh (loadAuto)
        std::vector<DecodedImage> images;
        std::string               error;          // no vacío = falló
    };

    // Traduce peticiones de asset a jobs y guarda los resultados en un buzón que
    // el hilo principal drena una vez por frame.
    //
    // No conoce Vulkan: produce bytes en RAM. Quien los sube es el Renderer.
    class AsyncAssetLoader
    {
        public:
            explicit AsyncAssetLoader(JobSystem& jobs) : m_jobs(jobs) {}
            AsyncAssetLoader(const AsyncAssetLoader&)            = delete;
            AsyncAssetLoader& operator=(const AsyncAssetLoader&) = delete;

            // targetId es el GameObject::id al que asignar el mesh. El pump
            // resuelve por id sobre la escena viva: si el objeto se borró
            // mientras cargaba, el resultado se descarta sin tocar memoria
            // liberada.
            JobSystem::JobId requestMesh(const std::string& path, uint64_t targetId);

            void cancel(JobSystem::JobId id);

            // Hilo principal. Devuelve los resultados listos, parando cuando se
            // agota budgetMs. Lo no devuelto sigue en el buzón para el próximo
            // frame — jamás se descarta por presupuesto.
            std::vector<LoadedMesh> pumpCompleted(float budgetMs);

            // Peticiones aún sin recoger por pumpCompleted (en cola, en vuelo o
            // en el buzón). Es lo que lee el modal de progreso.
            int pending() const;

            // Solo para tests: cuántos ReadFile de verdad se han hecho. Es la
            // única forma de comprobar el dedup desde fuera — contar resultados
            // no distingue "un ReadFile compartido" de "cuatro ReadFile".
            int readFileCount() const;

            // Cancela todas las peticiones vivas y vacía el buzón. Lo llama el
            // botón Cancelar del modal de carga (Task 9). Los jobs ya arrancados
            // terminan igual — no se puede parar un ReadFile a medias — pero sus
            // resultados se descartan.
            void cancelAllPending();

        private:
            // Peticiones agrupadas por path mientras el job está en vuelo. El
            // primero que pide un path arranca UN job (jobId); los que llegan
            // mientras sigue en vuelo se apuntan como waiters al mismo. Al
            // terminar, el worker construye un LoadedMesh por cada waiter
            // (copiando el Mesh) y vacía el grupo.
            //
            // jobId se guarda aparte de los waiters a propósito: es el id con
            // el que se encoló el job, que sigue siendo válido aunque su waiter
            // original se cancele mientras otros siguen esperando el ReadFile.
            struct PendingGroup
            {
                JobSystem::JobId jobId = 0;   // id encolado en el JobSystem (primer waiter)
                std::vector<std::pair<JobSystem::JobId, uint64_t>> waiters;  // (job, targetId)
            };

            void      runJob(const std::string& path);
            LoadedMesh buildResultFor(const LoadedMesh& src,
                                      JobSystem::JobId job, uint64_t targetId);

            JobSystem&              m_jobs;
            mutable std::mutex      m_mutex;
            std::vector<LoadedMesh> m_inbox;
            int                     m_pending = 0;

            std::unordered_map<std::string, PendingGroup> m_groups;
            int                                           m_readFileCount = 0;

            // Generación de cancelación en bloque. cancelAllPending() la
            // incrementa; un job ya arrancado (incancelable) captura la
            // generación al sacar sus waiters del grupo y, al ir a postar sus
            // resultados, los descarta si la generación cambió mientras copiaba
            // fuera del lock — esos targets se cancelaron y su m_pending ya se
            // puso a 0. Sin esto, postar tras un cancelAllPending dejaría
            // m_pending negativo para siempre (el loader es longevo) y
            // entregaría meshes de objetos ya cancelados. Ver runJob().
            uint64_t                                      m_epoch = 0;
    };

    class Scene;
    class Renderer;
    class EditorRenderer;

    // Aplica un resultado a la escena resolviendo por targetId sobre la escena
    // VIVA. Devuelve false si el GameObject ya no existe (borrado mientras
    // cargaba) o si el resultado trae error; en ese caso outError, si no es
    // nulo, recibe el mensaje para el log.
    //
    // No llama a flushPendingUploads: el caller decide cuándo cerrar el batch,
    // porque el sentido de todo esto es agrupar N resultados en UN submit.
    bool applyLoadedMesh(LoadedMesh& r, Scene& scene, EditorRenderer& renderer,
                         std::string* outError);
}
