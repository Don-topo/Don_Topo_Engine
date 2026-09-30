#include "DonTopo/Renderer/GpuDevice.h"
#include <GLFW/glfw3.h>
#include <stdexcept>
#include <vector>
#include <cstdio>
#include <cstring>
#include <algorithm>

#ifdef NDEBUG
    static constexpr bool ENABLE_VALIDATION = false;
#else
    static constexpr bool ENABLE_VALIDATION = true;
#endif

namespace DonTopo {

void GpuDevice::init(GLFWwindow* window)
{
    createInstance();
    setupDebugMessenger();
    createSurface(window);
    pickPhysicalDevice();
    createDevice();
    createCommandPool();
}

void GpuDevice::shutdown()
{
    if (m_device == VK_NULL_HANDLE) return;
    vkDestroyCommandPool(m_device, m_commandPool, nullptr);
    vkDestroyDevice(m_device, nullptr);
    m_device = VK_NULL_HANDLE;
    vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (ENABLE_VALIDATION && m_debugMessenger != VK_NULL_HANDLE) {
        auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT");
        if (func) func(m_instance, m_debugMessenger, nullptr);
    }
    vkDestroyInstance(m_instance, nullptr);
}

void GpuDevice::createInstance()
{
    VkApplicationInfo appInfo{};
    appInfo.sType               = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName    = "DonTopo";
    appInfo.applicationVersion  = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName         = "DonTopo Engine";
    appInfo.engineVersion       = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion          = VK_API_VERSION_1_0;

    uint32_t extensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&extensionCount);
    std::vector<const char*> extensions(glfwExtensions, glfwExtensions + extensionCount);

    if (ENABLE_VALIDATION)
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    const char* validationLayer = "VK_LAYER_KHRONOS_validation";

    // Is it installed? The layer ships with the Vulkan SDK, not with the driver, so
    // a machine with perfectly working Vulkan may not have it. Requesting it
    // blindly made vkCreateInstance return VK_ERROR_LAYER_NOT_PRESENT and
    // the Debug build died with "failed to create Vulkan instance!", which points
    // at the wrong place: it looks like the GPU is not good enough (H27).
    //
    // Without the layer it starts anyway and says so. Losing the validation messages is
    // far less bad than not being able to run.
    bool validationAvailable = false;
    if (ENABLE_VALIDATION) {
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> layers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
        for (const VkLayerProperties& layer : layers)
            if (std::strcmp(layer.layerName, validationLayer) == 0) {
                validationAvailable = true;
                break;
            }
        if (!validationAvailable) {
            printf("WARNING: %s is not installed (the Vulkan SDK is missing). Starting\n"
                   "       without the validation layer: there will be no misuse messages.\n",
                   validationLayer);
            fflush(stdout);
            // The messenger extension comes with the layer: requesting it without the layer is the
            // same failure one step later.
            extensions.erase(std::remove_if(extensions.begin(), extensions.end(),
                                            [](const char* e) {
                                                return std::strcmp(
                                                           e, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
                                            }),
                             extensions.end());
        }
    }

    VkInstanceCreateInfo createInfo{};
    createInfo.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo        = &appInfo;
    createInfo.enabledExtensionCount   = (uint32_t)extensions.size();
    createInfo.ppEnabledExtensionNames = extensions.data();
    if (validationAvailable) {
        createInfo.enabledLayerCount   = 1;
        createInfo.ppEnabledLayerNames = &validationLayer;
    }

    if (vkCreateInstance(&createInfo, nullptr, &m_instance) != VK_SUCCESS)
        throw std::runtime_error("failed to create Vulkan instance!");
    printf("instance OK\n"); fflush(stdout);
}

void GpuDevice::setupDebugMessenger()
{
    if (!ENABLE_VALIDATION) return;

    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    createInfo.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugCallback;

    auto func = (PFN_vkCreateDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT");
    // With no layer installed there is no pointer to resolve, and that is NOT an error: the
    // instance was created without it on purpose and a warning was already given at creation.
    // Throwing here was the same H27 failure one step later, killing startup
    // for lacking a diagnostic tool.
    if (!func) return;
    if (func(m_instance, &createInfo, nullptr, &m_debugMessenger) != VK_SUCCESS)
        throw std::runtime_error("failed to set up debug messenger!");
}

VKAPI_ATTR VkBool32 VKAPI_CALL GpuDevice::debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        fprintf(stderr, "[Vulkan] %s\n", data->pMessage);
    return VK_FALSE;
}

void GpuDevice::createSurface(GLFWwindow* window)
{
    if (glfwCreateWindowSurface(m_instance, window, nullptr, &m_surface) != VK_SUCCESS)
        throw std::runtime_error("failed to create window surface!");
    printf("surface OK\n"); fflush(stdout);
}

void GpuDevice::pickPhysicalDevice()
{
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
    if (deviceCount == 0)
        throw std::runtime_error("failed to find GPUs with Vulkan support!");

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

    for (VkPhysicalDevice device : devices) {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());

        bool hasGraphics = false, hasPresent = false;
        for (uint32_t i = 0; i < familyCount; i++) {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                hasGraphics = true; m_graphicsFamily = i;
            }
            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &presentSupport);
            if (presentSupport) { hasPresent = true; m_presentFamily = i; }
            if (hasGraphics && hasPresent) { m_physicalDevice = device; break; }
        }
        if (m_physicalDevice != VK_NULL_HANDLE) break;
    }

    if (m_physicalDevice == VK_NULL_HANDLE)
        throw std::runtime_error("failed to find a suitable GPU!");

    // Device memory allocation limit. It is read here, once, because it
    // varies enormously between implementations: the spec guarantees 4096 and a
    // desktop NVIDIA reports 4,189,151. With two live allocations per mesh,
    // that is 2,048 meshes in the worst case and two million in the other.
    //
    // A warning is only given when the limit is NARROW: on a generous GPU the message
    // would be noise on every startup, and warning about what cannot happen trains
    // people not to read warnings.
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
        m_maxMemoryAllocations   = props.limits.maxMemoryAllocationCount;
        m_maxUniformBufferRange  = props.limits.maxUniformBufferRange;
        if (allocationLimitIsTight(m_maxMemoryAllocations))
            printf("WARNING: this GPU allows %u memory allocations at a time. The engine requests\n"
                   "       one per resource, so two per mesh, which means the scene runs out\n"
                   "       of memory at about %u meshes even with VRAM to spare.\n",
                   m_maxMemoryAllocations,
                   meshesWithinAllocationLimit(m_maxMemoryAllocations));
    }
    printf("device OK\n"); fflush(stdout);
}

void GpuDevice::createDevice()
{
    float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueInfos;

    uint32_t uniqueFamilies[] = { m_graphicsFamily, m_presentFamily };
    for (uint32_t family : uniqueFamilies) {
        bool alreadyAdded = false;
        for (auto& q : queueInfos)
            if (q.queueFamilyIndex == family) { alreadyAdded = true; break; }
        if (alreadyAdded) continue;

        VkDeviceQueueCreateInfo qi{};
        qi.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = family;
        qi.queueCount       = 1;
        qi.pQueuePriorities = &priority;
        queueInfos.push_back(qi);
    }

    const char* extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    // fillModeNonSolid: required for VK_POLYGON_MODE_LINE (wireframe mode).
    VkPhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.fillModeNonSolid = VK_TRUE;

    VkDeviceCreateInfo createInfo{};
    createInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount    = (uint32_t)queueInfos.size();
    createInfo.pQueueCreateInfos       = queueInfos.data();
    createInfo.enabledExtensionCount   = 1;
    createInfo.ppEnabledExtensionNames = extensions;
    createInfo.pEnabledFeatures        = &deviceFeatures;

    if (vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device) != VK_SUCCESS)
        throw std::runtime_error("failed to create logical device!");

    vkGetDeviceQueue(m_device, m_graphicsFamily, 0, &m_graphicsQueue);
    vkGetDeviceQueue(m_device, m_presentFamily,  0, &m_presentQueue);
    printf("logical device OK\n"); fflush(stdout);
}

void GpuDevice::createCommandPool()
{
    VkCommandPoolCreateInfo info{};
    info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = m_graphicsFamily;

    if (vkCreateCommandPool(m_device, &info, nullptr, &m_commandPool) != VK_SUCCESS)
        throw std::runtime_error("failed to create command pool!");
    printf("command pool OK\n"); fflush(stdout);
}

uint32_t GpuDevice::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags props) const
{
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props)
            return i;

    throw std::runtime_error("failed to find suitable memory type!");
}

VkCommandBuffer GpuDevice::beginOneTimeCommands() const
{
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool        = m_commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(m_device, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);
    return cmd;
}

void GpuDevice::endOneTimeCommands(VkCommandBuffer cmd) const
{
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
}

} // namespace DonTopo
