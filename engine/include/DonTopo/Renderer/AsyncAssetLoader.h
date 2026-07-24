#pragma once
#include "DonTopo/Core/JobSystem.h"
#include "DonTopo/Renderer/Mesh.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
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

        private:
            void runJob(JobSystem::JobId id, const std::string& path, uint64_t targetId);

            JobSystem&              m_jobs;
            mutable std::mutex      m_mutex;
            std::vector<LoadedMesh> m_inbox;
            int                     m_pending = 0;
    };
}
