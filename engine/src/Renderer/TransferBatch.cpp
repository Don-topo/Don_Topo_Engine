#include "DonTopo/Renderer/TransferBatch.h"
#include "DonTopo/Renderer/GpuDevice.h"

#include <stdexcept>

namespace DonTopo
{
    TransferBatch::~TransferBatch()
    {
        // Un batch enviado y no reclamado al destruirse filtraría staging y
        // fence. No se puede esperar aquí sin arriesgar un bloqueo en el
        // destructor, así que se drena de forma explícita: si esto salta, hay un
        // camino que envía sin llamar a reclaim().
        if (m_submitted && m_fence != VK_NULL_HANDLE)
        {
            vkWaitForFences(m_gpu.device(), 1, &m_fence, VK_TRUE, UINT64_MAX);
            reclaim();
        }
        else if (m_cmd != VK_NULL_HANDLE)
        {
            // Abierto pero nunca enviado: nada corrió en la GPU, se libera sin
            // esperar.
            vkFreeCommandBuffers(m_gpu.device(), m_gpu.commandPool(), 1, &m_cmd);
            m_cmd = VK_NULL_HANDLE;
            for (auto& [buf, mem] : m_staging)
            {
                vkDestroyBuffer(m_gpu.device(), buf, nullptr);
                vkFreeMemory(m_gpu.device(), mem, nullptr);
            }
            m_staging.clear();
        }
    }

    VkCommandBuffer TransferBatch::cmd()
    {
        if (m_cmd != VK_NULL_HANDLE) return m_cmd;

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool        = m_gpu.commandPool();
        allocInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(m_gpu.device(), &allocInfo, &m_cmd) != VK_SUCCESS)
            throw std::runtime_error("TransferBatch: fallo al reservar command buffer");

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(m_cmd, &beginInfo);
        return m_cmd;
    }

    void TransferBatch::addStaging(VkBuffer buf, VkDeviceMemory mem)
    {
        m_staging.emplace_back(buf, mem);
    }

    void TransferBatch::submit()
    {
        if (m_cmd == VK_NULL_HANDLE || m_submitted) return;

        vkEndCommandBuffer(m_cmd);

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(m_gpu.device(), &fenceInfo, nullptr, &m_fence) != VK_SUCCESS)
            throw std::runtime_error("TransferBatch: fallo al crear la fence");

        VkSubmitInfo submitInfo{};
        submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers    = &m_cmd;
        vkQueueSubmit(m_gpu.graphicsQueue(), 1, &submitInfo, m_fence);
        m_submitted = true;
    }

    bool TransferBatch::complete() const
    {
        if (!m_submitted) return m_cmd == VK_NULL_HANDLE;   // batch vacío = nada que esperar
        return vkGetFenceStatus(m_gpu.device(), m_fence) == VK_SUCCESS;
    }

    void TransferBatch::reclaim()
    {
        if (!m_submitted) return;
        // Guarda invertida: en vez de enumerar desde dónde es seguro llamar,
        // se pregunta por el estado real de la GPU. Reclamar antes de tiempo es
        // un use-after-free que las capas de validación cazan, pero que en
        // release corrompe en silencio.
        if (vkGetFenceStatus(m_gpu.device(), m_fence) != VK_SUCCESS)
            throw std::runtime_error("TransferBatch::reclaim con la fence sin senalar");

        for (auto& [buf, mem] : m_staging)
        {
            vkDestroyBuffer(m_gpu.device(), buf, nullptr);
            vkFreeMemory(m_gpu.device(), mem, nullptr);
        }
        m_staging.clear();

        vkFreeCommandBuffers(m_gpu.device(), m_gpu.commandPool(), 1, &m_cmd);
        m_cmd = VK_NULL_HANDLE;

        vkDestroyFence(m_gpu.device(), m_fence, nullptr);
        m_fence     = VK_NULL_HANDLE;
        m_submitted = false;
    }
}
