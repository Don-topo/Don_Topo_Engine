#pragma once
#include <vulkan/vulkan.h>
#include <cstddef>
#include <functional>
#include <vector>

namespace DonTopo
{
    // Delays the destruction of Vulkan resources until no frame in flight
    // can be using them, instead of draining the whole device.
    //
    // It replaces the vkDeviceWaitIdle calls in the resource destruction paths
    // of Renderer.cpp (removeGameObject, removeMeshComponent and
    // replaceStaticTextureWithMissing). That was slow but impossible to get
    // wrong; this is fast and its failure mode is worse: destroying one frame
    // too early is a use-after-free on the GPU that does not reproduce
    // reliably.
    //
    // That is why the delay is kDelayFrames = MAX_FRAMES + 1, one frame more than
    // strictly necessary, and why flushAll() requires a prior vkDeviceWaitIdle
    // from the caller.
    class DeferredDeleteQueue
    {
        public:
            // Renderer's MAX_FRAMES is 2 (Renderer.h:337). The +1 is a deliberate
            // margin, not an off-by-one.
            static constexpr int kDelayFrames = 3;

            void push(std::function<void(VkDevice)> destroyer);

            // Once per frame, at the start of drawFrame.
            void tick(VkDevice device);

            // ONLY from Renderer::shutdown, and ONLY after a
            // vkDeviceWaitIdle. Runs everything pending without looking at the counter.
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
