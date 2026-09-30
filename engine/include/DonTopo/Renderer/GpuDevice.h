#pragma once
#include <vulkan/vulkan.h>

struct GLFWwindow;

namespace DonTopo {

class GpuDevice {
public:
    GpuDevice() = default;
    ~GpuDevice() { shutdown(); }
    GpuDevice(const GpuDevice&)            = delete;
    GpuDevice& operator=(const GpuDevice&) = delete;

    void init(GLFWwindow* window);
    void shutdown();

    // ── Memory allocation limit ─────────────────────────────────────────────
    //
    // Vulkan asks the driver for one allocation PER RESOURCE and the device imposes a
    // maximum of LIVE allocations at a time. Each mesh takes two (the vertex
    // buffer and the index buffer), so the limit translates directly into
    // a number of meshes.
    //
    // The number varies A LOT between implementations: the specification guarantees
    // 4096 as a minimum, and a desktop NVIDIA returns 4,189,151 (measured).
    // So the ceiling is a real wall on some GPUs and does not exist on others, and
    // that is why it is read from the device instead of assumed.
    //
    // These two are pure and live here so they can be tested without a device.
    static uint32_t meshesWithinAllocationLimit(uint32_t maxAllocations)
    {
        return maxAllocations / 2;   // vertices + indices per mesh
    }
    // Is a warning worthwhile? Only on implementations near the spec minimum:
    // above this, exhausting the limit requires a scene that would not fit in VRAM
    // long before.
    static bool allocationLimitIsTight(uint32_t maxAllocations)
    {
        return maxAllocations < 100000u;
    }
    // What THIS device said. 0 before the GPU is chosen.
    uint32_t maxMemoryAllocations() const { return m_maxMemoryAllocations; }
    // The largest UBO that THIS device accepts. The spec guarantees 16 KB as a
    // minimum, but the spec minimum is not the real limit (see H72): it is read from the
    // device and not assumed. createUniformBuffers looks at it, since it is the one
    // that knows the size of the block.
    uint32_t maxUniformBufferRange() const { return m_maxUniformBufferRange; }

    VkDevice         device()         const { return m_device; }
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    VkQueue          graphicsQueue()  const { return m_graphicsQueue; }
    VkQueue          presentQueue()   const { return m_presentQueue; }
    VkCommandPool    commandPool()    const { return m_commandPool; }
    VkSurfaceKHR     surface()        const { return m_surface; }
    VkInstance       instance()       const { return m_instance; }
    uint32_t         graphicsFamily() const { return m_graphicsFamily; }
    uint32_t         presentFamily()  const { return m_presentFamily; }

    uint32_t        findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags props) const;
    VkCommandBuffer beginOneTimeCommands() const;
    void            endOneTimeCommands(VkCommandBuffer cmd) const;

private:
    void createInstance();
    void setupDebugMessenger();
    void createSurface(GLFWwindow* window);
    void pickPhysicalDevice();
    void createDevice();
    void createCommandPool();

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT,
        VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT*,
        void*);

    VkInstance               m_instance       = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR             m_surface        = VK_NULL_HANDLE;
    VkPhysicalDevice         m_physicalDevice = VK_NULL_HANDLE;
    // Read from the device when it is chosen, in pickPhysicalDevice.
    uint32_t                 m_maxMemoryAllocations  = 0;
    uint32_t                 m_maxUniformBufferRange = 0;
    VkDevice                 m_device         = VK_NULL_HANDLE;
    VkQueue                  m_graphicsQueue  = VK_NULL_HANDLE;
    VkQueue                  m_presentQueue   = VK_NULL_HANDLE;
    VkCommandPool            m_commandPool    = VK_NULL_HANDLE;
    uint32_t                 m_graphicsFamily = 0;
    uint32_t                 m_presentFamily  = 0;
};

} // namespace DonTopo
