#pragma once
#include <vulkan/vulkan.h>
#include <utility>
#include <vector>

namespace DonTopo
{
    class GpuDevice;

    // Agrupa todos los uploads de un pump en UN command buffer, UN submit y UNA
    // fence.
    //
    // Antes, cada createTextureImage encadenaba tres vkQueueWaitIdle (transición
    // → copia → transición) y cada buffer uno más: unos 11 vaciados completos de
    // la cola gráfica por mesh estático, ~440 al abrir una escena de 40 objetos.
    //
    // La corrección se mantiene porque las barreras (vkCmdPipelineBarrier)
    // ordenan dentro del command buffer igual que ordenaban entre submits. Lo
    // que desaparece es la espera, no la sincronización.
    class TransferBatch
    {
        public:
            explicit TransferBatch(const GpuDevice& gpu) : m_gpu(gpu) {}
            ~TransferBatch();
            TransferBatch(const TransferBatch&)            = delete;
            TransferBatch& operator=(const TransferBatch&) = delete;

            // Abre el command buffer la primera vez que se llama. Todas las
            // operaciones del batch comparten este.
            VkCommandBuffer cmd();

            // El staging vive hasta que la fence señala: liberarlo antes es un
            // use-after-free en la GPU que no peta de forma reproducible.
            void addStaging(VkBuffer buf, VkDeviceMemory mem);

            // Cierra y envía. No espera.
            void submit();

            // vkGetFenceStatus. NO bloquea: es lo que consulta el Renderer cada
            // frame para decidir si ya puede dibujar los objetos del batch.
            bool complete() const;

            // Destruye staging y command buffer. Exige complete() == true.
            void reclaim();

            bool empty() const { return m_cmd == VK_NULL_HANDLE; }

        private:
            const GpuDevice& m_gpu;
            VkCommandBuffer  m_cmd       = VK_NULL_HANDLE;
            VkFence          m_fence     = VK_NULL_HANDLE;
            bool             m_submitted = false;
            std::vector<std::pair<VkBuffer, VkDeviceMemory>> m_staging;
    };
}
