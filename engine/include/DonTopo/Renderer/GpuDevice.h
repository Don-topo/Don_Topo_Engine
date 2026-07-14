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
    VkDevice                 m_device         = VK_NULL_HANDLE;
    VkQueue                  m_graphicsQueue  = VK_NULL_HANDLE;
    VkQueue                  m_presentQueue   = VK_NULL_HANDLE;
    VkCommandPool            m_commandPool    = VK_NULL_HANDLE;
    uint32_t                 m_graphicsFamily = 0;
    uint32_t                 m_presentFamily  = 0;
};

} // namespace DonTopo
