#include "DonTopo/Renderer/DeferredDelete.h"

#include <utility>

namespace DonTopo
{
    void DeferredDeleteQueue::push(std::function<void(VkDevice)> destroyer)
    {
        m_entries.push_back(Entry{kDelayFrames, std::move(destroyer)});
    }

    void DeferredDeleteQueue::tick(VkDevice device)
    {
        // Recorrido con índice y swap-erase: un destroyer no puede encolar más
        // trabajo (destruye, no crea), así que no hace falta protegerse de una
        // invalidación por reentrada, pero sí de reordenar mientras se itera.
        for (size_t i = 0; i < m_entries.size();)
        {
            if (--m_entries[i].framesLeft > 0) { ++i; continue; }

            m_entries[i].destroyer(device);
            m_entries[i] = std::move(m_entries.back());
            m_entries.pop_back();
        }
    }

    void DeferredDeleteQueue::flushAll(VkDevice device)
    {
        // El caller garantiza vkDeviceWaitIdle previo. Sin él, esto es
        // exactamente el use-after-free que la cola existe para evitar.
        for (auto& e : m_entries)
            e.destroyer(device);
        m_entries.clear();
    }
}
