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

    // ── Tope de asignaciones de memoria ─────────────────────────────────────
    //
    // Vulkan pide una asignación al driver POR RECURSO y el device impone un
    // máximo de asignaciones VIVAS a la vez. Cada malla se lleva dos —el buffer
    // de vértices y el de índices—, así que el tope se traduce directamente a
    // un número de mallas.
    //
    // El número varía MUCHO entre implementaciones: la especificación garantiza
    // 4096 como mínimo, y una NVIDIA de escritorio devuelve 4.189.151 (medido).
    // O sea que el techo es un muro real en unas GPU y no existe en otras, y por
    // eso se lee del device en vez de darlo por sabido.
    //
    // Estas dos son puras y viven aquí para poder probarlas sin device.
    static uint32_t meshesWithinAllocationLimit(uint32_t maxAllocations)
    {
        return maxAllocations / 2;   // vértices + índices por malla
    }
    // ¿Conviene avisar? Solo en implementaciones cerca del mínimo de la spec:
    // por encima de esto, agotar el tope exige una escena que no cabría en VRAM
    // mucho antes.
    static bool allocationLimitIsTight(uint32_t maxAllocations)
    {
        return maxAllocations < 100000u;
    }
    // Lo que dijo ESTE device. 0 antes de elegir la GPU.
    uint32_t maxMemoryAllocations() const { return m_maxMemoryAllocations; }

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
    // Se lee del device al elegirlo, en pickPhysicalDevice.
    uint32_t                 m_maxMemoryAllocations = 0;
    VkDevice                 m_device         = VK_NULL_HANDLE;
    VkQueue                  m_graphicsQueue  = VK_NULL_HANDLE;
    VkQueue                  m_presentQueue   = VK_NULL_HANDLE;
    VkCommandPool            m_commandPool    = VK_NULL_HANDLE;
    uint32_t                 m_graphicsFamily = 0;
    uint32_t                 m_presentFamily  = 0;
};

} // namespace DonTopo
