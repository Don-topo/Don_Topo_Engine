#include "DonTopo/Renderer/TransferBatch.h"
#include "DonTopo/Renderer/GpuDevice.h"

#include <stdexcept>

namespace DonTopo
{
    TransferBatch::~TransferBatch()
    {
        // A batch that was submitted and not reclaimed when destroyed would leak staging and
        // fence. We cannot wait here without risking a block in the
        // destructor, so it is drained explicitly: if this fires, there is a
        // path that submits without calling reclaim().
        if (m_submitted && m_fence != VK_NULL_HANDLE)
        {
            vkWaitForFences(m_gpu.device(), 1, &m_fence, VK_TRUE, UINT64_MAX);
            reclaim();
        }
        else if (m_cmd != VK_NULL_HANDLE)
        {
            // Opened but never submitted: nothing ran on the GPU, it is released without
            // waiting.
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
            throw std::runtime_error("TransferBatch: failed to allocate the command buffer");

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
            throw std::runtime_error("TransferBatch: failed to create the fence");

        VkSubmitInfo submitInfo{};
        submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers    = &m_cmd;
        vkQueueSubmit(m_gpu.graphicsQueue(), 1, &submitInfo, m_fence);
        m_submitted = true;
    }

    bool TransferBatch::complete() const
    {
        if (!m_submitted) return m_cmd == VK_NULL_HANDLE;   // empty batch = nothing to wait for
        return vkGetFenceStatus(m_gpu.device(), m_fence) == VK_SUCCESS;
    }

    void TransferBatch::reclaim()
    {
        if (!m_submitted) return;
        // Inverted guard: instead of enumerating where it is safe to call from,
        // it asks about the real state of the GPU. Reclaiming too early is
        // a use-after-free that the validation layers catch, but that in
        // release corrupts silently.
        if (vkGetFenceStatus(m_gpu.device(), m_fence) != VK_SUCCESS)
            throw std::runtime_error("TransferBatch::reclaim with the fence not signaled");

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
