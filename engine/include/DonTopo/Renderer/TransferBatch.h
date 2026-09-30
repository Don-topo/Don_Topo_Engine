#pragma once
#include <vulkan/vulkan.h>
#include <utility>
#include <vector>

namespace DonTopo
{
    class GpuDevice;

    // Groups all the uploads of a pump into ONE command buffer, ONE submit and ONE
    // fence.
    //
    // Before, each createTextureImage chained three vkQueueWaitIdle (transition
    // -> copy -> transition) and each buffer one more: about 11 full drains of the
    // graphics queue per static mesh, ~440 when opening a scene of 40 objects.
    //
    // Correctness is kept because the barriers (vkCmdPipelineBarrier)
    // order within the command buffer just as they used to order between submits. What
    // disappears is the wait, not the synchronization.
    class TransferBatch
    {
        public:
            explicit TransferBatch(const GpuDevice& gpu) : m_gpu(gpu) {}
            ~TransferBatch();
            TransferBatch(const TransferBatch&)            = delete;
            TransferBatch& operator=(const TransferBatch&) = delete;

            // Opens the command buffer the first time it is called. All the
            // operations of the batch share this one.
            VkCommandBuffer cmd();

            // The staging lives until the fence signals: freeing it earlier is a
            // use-after-free on the GPU that does not crash reproducibly.
            void addStaging(VkBuffer buf, VkDeviceMemory mem);

            // Closes and submits. Does not wait.
            void submit();

            // vkGetFenceStatus. Does NOT block: it is what the Renderer queries every
            // frame to decide whether it can already draw the objects of the batch.
            bool complete() const;

            // Destroys staging and command buffer. Requires complete() == true.
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
