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
        // Walk with an index and swap-erase: a destroyer cannot enqueue more
        // work (it destroys, it does not create), so there is no need to guard against
        // invalidation by reentrancy, but there is against reordering while iterating.
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
        // The caller guarantees a prior vkDeviceWaitIdle. Without it, this is
        // exactly the use-after-free the queue exists to avoid.
        for (auto& e : m_entries)
            e.destroyer(device);
        m_entries.clear();
    }
}
