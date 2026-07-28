#pragma once
#include <vulkan/vulkan.h>
#include <cstddef>
#include <functional>
#include <vector>

namespace DonTopo
{
    // Retrasa la destrucción de recursos Vulkan hasta que ningún frame en vuelo
    // pueda estar usándolos, en lugar de vaciar el device entero.
    //
    // Sustituye a los vkDeviceWaitIdle de las rutas de destrucción de recursos
    // de Renderer.cpp (removeGameObject, removeMeshComponent y
    // replaceStaticTextureWithMissing). Aquel era lento pero imposible de
    // equivocar; esto es rápido y su modo de fallo es peor: destruir un frame
    // antes de tiempo es un use-after-free en la GPU que no se reproduce de
    // forma fiable.
    //
    // Por eso el retraso es kDelayFrames = MAX_FRAMES + 1, un frame más de lo
    // estrictamente necesario, y por eso flushAll() exige un vkDeviceWaitIdle
    // previo del caller.
    class DeferredDeleteQueue
    {
        public:
            // MAX_FRAMES de Renderer es 2 (Renderer.h:337). El +1 es margen
            // deliberado, no un off-by-one.
            static constexpr int kDelayFrames = 3;

            void push(std::function<void(VkDevice)> destroyer);

            // Una vez por frame, al principio de drawFrame.
            void tick(VkDevice device);

            // SOLO desde Renderer::shutdown, y SOLO después de un
            // vkDeviceWaitIdle. Ejecuta todo lo pendiente sin mirar el contador.
            void flushAll(VkDevice device);

            size_t pendingCount() const { return m_entries.size(); }

        private:
            struct Entry
            {
                int                            framesLeft;
                std::function<void(VkDevice)>  destroyer;
            };

            std::vector<Entry> m_entries;
    };
}
