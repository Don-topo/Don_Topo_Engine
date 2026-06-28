#include "DonTopo/Renderer.h"
#include <GLFW/glfw3.h>
#include <stdexcept>
#include "DonTopo/Window.h"
#include <algorithm>
#include <fstream>
#include "DonTopo/Vertex.h"
#include <chrono>
#include <glm/gtc/matrix_transform.hpp>
#include "DonTopo/UniformBufferObject.h"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#ifdef NDEBUG
    static constexpr bool ENABLE_VALIDATION = false;
#else
    static constexpr bool ENABLE_VALIDATION = true;
#endif

namespace DonTopo {

    Renderer::~Renderer()
    {
        shutdown();
    }

    static const std::vector<DonTopo::Vertex> s_vertices = {
    // Frente (z=+0.5) — rojo
    {{-0.5f, -0.5f,  0.5f}, {1,0,0}, {0,0}}, {{ 0.5f, -0.5f,  0.5f}, {1,0,0}, {1,0}},
    {{ 0.5f,  0.5f,  0.5f}, {1,0,0}, {1,1}}, {{-0.5f,  0.5f,  0.5f}, {1,0,0}, {0,1}},
    // Atrás (z=-0.5) — verde
    {{ 0.5f, -0.5f, -0.5f}, {0,1,0}, {0,0}}, {{-0.5f, -0.5f, -0.5f}, {0,1,0}, {1,0}},
    {{-0.5f,  0.5f, -0.5f}, {0,1,0}, {1,1}}, {{ 0.5f,  0.5f, -0.5f}, {0,1,0}, {0,1}},
    // Izquierda (x=-0.5) — azul
    {{-0.5f, -0.5f, -0.5f}, {0,0,1}, {0,0}}, {{-0.5f, -0.5f,  0.5f}, {0,0,1}, {1,0}},
    {{-0.5f,  0.5f,  0.5f}, {0,0,1}, {1,1}}, {{-0.5f,  0.5f, -0.5f}, {0,0,1}, {0,1}},
    // Derecha (x=+0.5) — amarillo
    {{ 0.5f, -0.5f,  0.5f}, {1,1,0}, {0,0}}, {{ 0.5f, -0.5f, -0.5f}, {1,1,0}, {1,0}},
    {{ 0.5f,  0.5f, -0.5f}, {1,1,0}, {1,1}}, {{ 0.5f,  0.5f,  0.5f}, {1,1,0}, {0,1}},
    // Arriba (y=+0.5) — cian
    {{-0.5f,  0.5f,  0.5f}, {0,1,1}, {0,0}}, {{ 0.5f,  0.5f,  0.5f}, {0,1,1}, {1,0}},
    {{ 0.5f,  0.5f, -0.5f}, {0,1,1}, {1,1}}, {{-0.5f,  0.5f, -0.5f}, {0,1,1}, {0,1}},
    // Abajo (y=-0.5) — magenta
    {{-0.5f, -0.5f, -0.5f}, {1,0,1}, {0,0}}, {{ 0.5f, -0.5f, -0.5f}, {1,0,1}, {1,0}},
    {{ 0.5f, -0.5f,  0.5f}, {1,0,1}, {1,1}}, {{-0.5f, -0.5f,  0.5f}, {1,0,1}, {0,1}},
    };

    static const std::vector<uint16_t> s_indices = {
     0, 1, 2,  0, 2, 3,   // frente
     4, 5, 6,  4, 6, 7,   // atrás
     8, 9,10,  8,10,11,   // izquierda
    12,13,14, 12,14,15,   // derecha
    16,17,18, 16,18,19,   // arriba
    20,21,22, 20,22,23,   // abajo
    };

    void Renderer::init(Window& window)
    {
        createInstance();
        setupDebugMessenger();
        createSurface(window);
        pickPhysicalDevice();
        createDevice();
        createSwapChain(window);

        glfwSetWindowUserPointer(window.getNativeWindow(), &m_framebufferResized);
        glfwSetFramebufferSizeCallback(window.getNativeWindow(), [](GLFWwindow* w, int, int) {
            auto* flag = static_cast<bool*>(glfwGetWindowUserPointer(w));
            *flag = true;
        });

        createImageViews();
        createDepthResources();
        createRenderPass();
        createDescriptorSetLayout();
        createPipeline();
        createFramebuffers();
        createCommandPool();
        createVertexBuffer();
        createIndexBuffer();
        createTextureImage();
        createTextureImageView();
        createTextureSampler();
        createUniformBuffers();
        createDescriptorPool();
        createDescriptorSets();
        createCommandBuffers();
        createSyncObjects();        
    }

    void Renderer::drawFrame(Window& window)
    {
        // 1. Espera a que el frame anterior terminó
        vkWaitForFences(m_device, 1, &m_inFlight[m_currentFrame], VK_TRUE, UINT64_MAX);        

        // 2. Pide la siguiente imagen del swapchain
        uint32_t imageIndex;
        VkResult result;

        result = vkAcquireNextImageKHR(m_device, m_swapChain, UINT64_MAX, m_imageAvailable[m_currentFrame], VK_NULL_HANDLE, &imageIndex);
        if(result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            recreateSwapChain(window);
            return;
        }

        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            throw std::runtime_error("failed to acquire next image!");
        }        

        vkResetFences(m_device, 1, &m_inFlight[m_currentFrame]);

        // 3. Graba el command buffer
        if(vkResetCommandBuffer(m_commandBuffers[m_currentFrame], 0) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to reset command buffer!");
        }

        static auto startTime = std::chrono::high_resolution_clock::now();
        auto now = std::chrono::high_resolution_clock::now();
        float elapsed = std::chrono::duration<float>(now - startTime).count();

        m_transform = glm::rotate(glm::mat4(1.0f), 
            elapsed * glm::radians(90.0f), 
            glm::vec3(0.0f,1.0f,0.0f));
            
        updateUniformBuffer(m_currentFrame);
        recordCommandBuffer(imageIndex);

        // 4. Envía a la GPU
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount       = 1;
        submitInfo.pWaitSemaphores          = &m_imageAvailable[m_currentFrame];
        submitInfo.pWaitDstStageMask        = &waitStage;
        submitInfo.commandBufferCount       = 1;
        submitInfo.pCommandBuffers          = &m_commandBuffers[m_currentFrame];
        submitInfo.signalSemaphoreCount     = 1;
        submitInfo.pSignalSemaphores        = &m_renderFinished[imageIndex];
        if(vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_inFlight[m_currentFrame]) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to submit graphics queue!");
        }

        // 5. Presenta
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType               = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount  = 1;
        presentInfo.pWaitSemaphores     = &m_renderFinished[imageIndex];
        presentInfo.swapchainCount      = 1;
        presentInfo.pSwapchains         = &m_swapChain;
        presentInfo.pImageIndices       = &imageIndex;
        result = vkQueuePresentKHR(m_presentQueue, &presentInfo);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || m_framebufferResized) {
            m_framebufferResized = false;
            recreateSwapChain(window);
        } else if (result != VK_SUCCESS) {
            throw std::runtime_error("failed to present!");
        }

        m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES;
    }

    void Renderer::shutdown()
    {
        if (m_device == VK_NULL_HANDLE) return;
        vkDeviceWaitIdle(m_device);

        for(auto sem : m_renderFinished){
            vkDestroySemaphore(m_device, sem, nullptr);
        }

        for(int i = 0; i < MAX_FRAMES; i++)
        {            
            vkDestroySemaphore(m_device, m_imageAvailable[i], nullptr);
            vkDestroyFence(m_device, m_inFlight[i], nullptr);
        }
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        for(auto framebuffer : m_swapChainFramebuffers)
        {
            vkDestroyFramebuffer(m_device, framebuffer, nullptr);
        }
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        for(VkImageView imageView : m_swapChainImageViews)
        {
            vkDestroyImageView(m_device, imageView, nullptr);
        }                        
        vkDestroySwapchainKHR(m_device, m_swapChain, nullptr);
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        vkDestroyBuffer(m_device, m_vertexBuffer, nullptr);
        vkFreeMemory(m_device, m_vertexBufferMemory, nullptr);
        for(int i = 0; i < MAX_FRAMES; i++)
        {
            vkDestroyBuffer(m_device, m_uniformBuffers[i], nullptr);
            vkFreeMemory(m_device, m_uniformBuffersMemory[i], nullptr);
        }
        vkDestroyBuffer(m_device, m_indexBuffer, nullptr);
        vkFreeMemory(m_device, m_indexBufferMemory, nullptr);
        vkDestroySampler(m_device, m_textureSampler, nullptr);
        vkDestroyImageView(m_device, m_textureImageView, nullptr);
        vkDestroyImage(m_device, m_textureImage, nullptr);
        vkFreeMemory(m_device, m_textureImageMemory, nullptr);
        vkDestroySampler(m_device, m_textureSampler, nullptr);
        vkDestroyImageView(m_device, m_textureImageView, nullptr);
        vkDestroyImage(m_device, m_textureImage, nullptr);
        vkFreeMemory(m_device, m_textureImageMemory, nullptr);
        vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
        vkDestroyImageView(m_device, m_depthImageView, nullptr);
        vkDestroyImage(m_device, m_depthImage, nullptr);
        vkFreeMemory(m_device, m_depthImageMemory, nullptr);
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        if(ENABLE_VALIDATION && m_debugMessenger != VK_NULL_HANDLE)
        {
            auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)
                vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT");
            if(func){
                func(m_instance, m_debugMessenger, nullptr);
            }
        }
        vkDestroyInstance(m_instance, nullptr);
        printf("destroy render items OK\n"); fflush(stdout);
    }

    void Renderer::createInstance()
    {
        VkApplicationInfo appInfo{};
        appInfo.sType               = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName    = "DonTopo";
        appInfo.applicationVersion  = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName         = "DonTopo Engine";
        appInfo.engineVersion       = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion          = VK_API_VERSION_1_0;

        uint32_t extensionCount = 0;
        const char **glfwExtensions = glfwGetRequiredInstanceExtensions(&extensionCount);
        // Copia a vector para poder añadir extensiones extra
        std::vector<const char*> extensions(glfwExtensions, glfwExtensions + extensionCount);

        if(ENABLE_VALIDATION)
        {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        
        const char* validationLayer = "VK_LAYER_KHRONOS_validation";
            
        VkInstanceCreateInfo createInfo{};
        createInfo.sType                    = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo         = &appInfo;
        createInfo.enabledExtensionCount    = (uint32_t)extensions.size();
        createInfo.ppEnabledExtensionNames  = extensions.data();
        if(ENABLE_VALIDATION)
        {
            createInfo.enabledLayerCount    = 1;
            createInfo.ppEnabledLayerNames  = &validationLayer;
        }

        if(vkCreateInstance(&createInfo, nullptr, &m_instance) != VK_SUCCESS) {
            throw std::runtime_error("failed to create Vulkan instance!");
        }   
        printf("instance OK\n");  fflush(stdout);     
    }
    
    void Renderer::setupDebugMessenger()
    {
        if(!ENABLE_VALIDATION) return;

        VkDebugUtilsMessengerCreateInfoEXT createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        createInfo.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.pfnUserCallback = debugCallback;

        auto func = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT");
        if (!func || func(m_instance, &createInfo, nullptr, &m_debugMessenger) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to set up debug messenger!");
        }            
    }

    VKAPI_ATTR VkBool32 VKAPI_CALL Renderer::debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
    {
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            fprintf(stderr, "[Vulkan] %s\n", data->pMessage);
        return VK_FALSE;
    }

    void Renderer::createSurface(Window& window)
    {
        if(glfwCreateWindowSurface(m_instance, window.getNativeWindow(), nullptr, &m_surface) != VK_SUCCESS) {
            throw std::runtime_error("failed to create window surface!");
        }
        printf("surface OK\n");   fflush(stdout);
    }

    void Renderer::pickPhysicalDevice()
    {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(m_instance, &deviceCount,nullptr);
        if(deviceCount == 0)
        {
            throw std::runtime_error("failed to find GPUs with Vulkan support!");
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());
    
        for(VkPhysicalDevice device : devices)
        {
            uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);

            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());

            bool hasGraphics = false;
            bool hasPresent = false;

            for(uint32_t i = 0; i < familyCount; i++)
            {
                // Check if the queue family supports graphics
                if(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                {
                    hasGraphics = true;
                    m_graphicsFamily = i;
                }
                // Check if the queue family supports presentation to the surface
                VkBool32 presentSupport = false;
                vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &presentSupport);
                if(presentSupport)
                {
                    hasPresent = true;
                    m_presentFamily = i;
                }
                if(hasGraphics && hasPresent)
                {
                    m_physicalDevice = device;
                    printf("device OK\n"); fflush(stdout);
                    break;
                }
            }

            if(m_physicalDevice != VK_NULL_HANDLE)
            {
                break;
            }
        }
        
        if(m_physicalDevice == VK_NULL_HANDLE)
        {
            throw std::runtime_error("failed to find a suitable GPU!");
        }

    }

    void Renderer::createDevice()
    {
        float priority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queueInfos;

        // Si son la misma family, crea solo 1 entrada
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
        VkDeviceCreateInfo createInfo{};
        createInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount    = (uint32_t)queueInfos.size();
        createInfo.pQueueCreateInfos       = queueInfos.data();
        createInfo.enabledExtensionCount   = 1;
        createInfo.ppEnabledExtensionNames = extensions;

        if (vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device) != VK_SUCCESS)
            throw std::runtime_error("failed to create logical device!");

        vkGetDeviceQueue(m_device, m_graphicsFamily, 0, &m_graphicsQueue);
        vkGetDeviceQueue(m_device, m_presentFamily,  0, &m_presentQueue);
        printf("logical device OK\n"); fflush(stdout);
    }

    void Renderer::createSwapChain(Window& window)
    {
        VkSurfaceCapabilitiesKHR surfaceCapabilities;
        if(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &surfaceCapabilities) != VK_SUCCESS) {
            throw std::runtime_error("failed to get surface capabilities!");
        }

        uint32_t formatCount;
        if(vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("failed to get surface formats!");
        }

        std::vector<VkSurfaceFormatKHR> surfaceFormats(formatCount);
        if(vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, surfaceFormats.data()) != VK_SUCCESS) {
            throw std::runtime_error("failed to get surface formats!");
        }

        VkSurfaceFormatKHR chosenFormat = surfaceFormats[0];
        for(auto& surfaceFormat : surfaceFormats)
        {
            if(surfaceFormat.format == VK_FORMAT_B8G8R8A8_SRGB && surfaceFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                m_swapChainFormat = surfaceFormat.format;
                chosenFormat = surfaceFormat;
                break;
            }
        }
        
        m_swapChainFormat = chosenFormat.format;
        m_swapChainColorSpace = chosenFormat.colorSpace;

        VkExtent2D extent;
        if(surfaceCapabilities.currentExtent.width != UINT32_MAX)
        {
            extent = surfaceCapabilities.currentExtent;
        }
        else
        {
            int width, height;
            glfwGetFramebufferSize(window.getNativeWindow(), &width, &height);
            
            extent.width = std::clamp((uint32_t)width, surfaceCapabilities.minImageExtent.width, surfaceCapabilities.maxImageExtent.width);
            extent.height = std::clamp((uint32_t)height, surfaceCapabilities.minImageExtent.height, surfaceCapabilities.maxImageExtent.height);            
        }
        
        m_swapChainExtent = extent;

        uint32_t imageCount = surfaceCapabilities.minImageCount + 1;
        if(surfaceCapabilities.maxImageCount > 0 && imageCount > surfaceCapabilities.maxImageCount)
        {
            imageCount = surfaceCapabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = m_surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = m_swapChainFormat;
        createInfo.imageColorSpace = m_swapChainColorSpace;
        createInfo.imageExtent = m_swapChainExtent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.preTransform = surfaceCapabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR; // vsync
        createInfo.clipped = VK_TRUE;

        if(vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &m_swapChain) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create swap chain!");
        }
        
        if(vkGetSwapchainImagesKHR(m_device, m_swapChain, &imageCount, nullptr) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to get swap chain images!");
        }
        m_swapChainImages.resize(imageCount);
        if(vkGetSwapchainImagesKHR(m_device, m_swapChain, &imageCount, m_swapChainImages.data()) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to get swap chain images!");
        }
        printf("SwapChain OK\n"); fflush(stdout);
    }

    void Renderer::createImageViews()
    {
        m_swapChainImageViews.resize((m_swapChainImages.size()));

        for(size_t i = 0; i < m_swapChainImages.size(); i++)
        {
            VkImageViewCreateInfo createInfo{};
            createInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            createInfo.image    = m_swapChainImages[i];
            createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            createInfo.format   = m_swapChainFormat;
            // Mapeo de canales (identidad = sin cambios)
            createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            // Qué parte de la imagen usamos
            createInfo.subresourceRange.aspectMask      = VK_IMAGE_ASPECT_COLOR_BIT;
            createInfo.subresourceRange.baseMipLevel    = 0;
            createInfo.subresourceRange.levelCount      = 1;
            createInfo.subresourceRange.baseArrayLayer  = 0;
            createInfo.subresourceRange.layerCount      = 1;

            if(vkCreateImageView(m_device, &createInfo, nullptr, &m_swapChainImageViews[i]) != VK_SUCCESS)
            {
                throw std::runtime_error("failed to create image views!");
            }
        }

        printf("Image View OK\n"); fflush(stdout);
    }

    void Renderer::createRenderPass()
    {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format          = m_swapChainFormat;
        colorAttachment.samples         = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp          = VK_ATTACHMENT_LOAD_OP_CLEAR;   // Clean at start
        colorAttachment.storeOp         = VK_ATTACHMENT_STORE_OP_STORE; // Save result
        colorAttachment.stencilLoadOp   = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp  = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout   = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorReference{};
        colorReference.attachment   = 0; // Array attachment index
        colorReference.layout       = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentDescription depthAttachment{};
        depthAttachment.format          = VK_FORMAT_D32_SFLOAT;
        depthAttachment.samples         = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp          = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp         = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.stencilLoadOp   = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp  = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout   = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthRef{};
        depthRef.attachment = 1;
        depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount    = 1;
        subpass.pColorAttachments       = &colorReference;
        subpass.pDepthStencilAttachment = &depthRef;

        VkSubpassDependency subpassDependency{};
        subpassDependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
        subpassDependency.dstSubpass    = 0;
        subpassDependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        subpassDependency.srcAccessMask = 0;
        subpassDependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        subpassDependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkAttachmentDescription attachments[]= { colorAttachment, depthAttachment};
        VkRenderPassCreateInfo renderPassCreateInfo {};
        renderPassCreateInfo.sType              = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassCreateInfo.attachmentCount    = 2;
        renderPassCreateInfo.pAttachments       = attachments;
        renderPassCreateInfo.subpassCount       = 1;
        renderPassCreateInfo.pSubpasses         = &subpass;
        renderPassCreateInfo.dependencyCount    = 1;
        renderPassCreateInfo.pDependencies      = &subpassDependency;

        if(vkCreateRenderPass(m_device, &renderPassCreateInfo, nullptr, &m_renderPass) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create render pass!");
        }

        printf("render pass OK\n"); fflush(stdout);
    }

    void Renderer::createFramebuffers()
    {
        m_swapChainFramebuffers.resize(m_swapChainImageViews.size());
        for(size_t i = 0; i < m_swapChainImageViews.size(); i++)
        {
            VkImageView attachments[] = { m_swapChainImageViews[i], m_depthImageView };
            VkFramebufferCreateInfo framebufferCreateInfo{};
            framebufferCreateInfo.sType             = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferCreateInfo.renderPass        = m_renderPass;
            framebufferCreateInfo.attachmentCount   = 2;
            framebufferCreateInfo.pAttachments      = attachments;
            framebufferCreateInfo.width             = m_swapChainExtent.width;
            framebufferCreateInfo.height            = m_swapChainExtent.height;   
            framebufferCreateInfo.layers            = 1;         

            if(vkCreateFramebuffer(m_device, &framebufferCreateInfo, nullptr, &m_swapChainFramebuffers[i]) != VK_SUCCESS)
            {
                throw std::runtime_error("failed to create framebuffer!");
            }            
        }
        printf("framebuffers OK\n"); fflush(stdout);
    }

    void Renderer::createCommandPool()
    {
        VkCommandPoolCreateInfo commandPoolCreateInfo{};
        commandPoolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        commandPoolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        commandPoolCreateInfo.queueFamilyIndex = m_graphicsFamily;
        
        if(vkCreateCommandPool(m_device, &commandPoolCreateInfo, nullptr, &m_commandPool) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create command pool!");
        }

        printf("command pool OK\n"); fflush(stdout);
    }

    void Renderer::createCommandBuffers()
    {
        m_commandBuffers.resize(2);
        VkCommandBufferAllocateInfo commandBufferAllocateInfo{};
        commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandBufferAllocateInfo.commandPool = m_commandPool;
        commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandBufferAllocateInfo.commandBufferCount = 2;

        if(vkAllocateCommandBuffers(m_device, &commandBufferAllocateInfo, m_commandBuffers.data()) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to allocate command buffers!");
        }

        printf("command buffer allocate OK\n"); fflush(stdout);
    }

    void Renderer::createSyncObjects()
    {
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        
        VkFenceCreateInfo fenceCreateInfo{};
        fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;   // First frame

        for(int i = 0; i < MAX_FRAMES; i++)
        {
            if(vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_imageAvailable[i]) != VK_SUCCESS                
                || vkCreateFence(m_device, &fenceCreateInfo, nullptr, &m_inFlight[i]) != VK_SUCCESS)
            {
                // m_imageAvailable — señala que hay imagen disponible del swapchain
                // m_renderFinished — señala que el render terminó, listo para presentar
                // m_inFlight — fence que bloquea la CPU hasta que la GPU terminó ese frame
                throw std::runtime_error("failed to create sync objects!");
            }
        }

        m_renderFinished.resize(m_swapChainImages.size());
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (auto& sem : m_renderFinished)
        {
            if (vkCreateSemaphore(m_device, &semInfo, nullptr, &sem) != VK_SUCCESS)
            {
                throw std::runtime_error("failed to create renderFinished semaphore!");
            }                
            printf("sync objects OK\n"); fflush(stdout);
        }            
    }

    void Renderer::recordCommandBuffer(uint32_t imageIndex)
    {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if(vkBeginCommandBuffer(m_commandBuffers[m_currentFrame], &beginInfo) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to begin command buffer!");
        }

        VkClearValue clearValues[2];
        clearValues[0].color        = {0.0f, 0.0f, 0.0f, 1.0f};
        clearValues[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo renderPassBeginInfo{};
        renderPassBeginInfo.sType               = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassBeginInfo.renderPass          = m_renderPass;
        renderPassBeginInfo.framebuffer         = m_swapChainFramebuffers[imageIndex];
        renderPassBeginInfo.renderArea.extent   = m_swapChainExtent;
        renderPassBeginInfo.renderArea.offset   = {0,0};
        renderPassBeginInfo.clearValueCount     = 2;
        renderPassBeginInfo.pClearValues        = clearValues;

        vkCmdBeginRenderPass(m_commandBuffers[m_currentFrame], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(m_commandBuffers[m_currentFrame], VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
        vkCmdBindDescriptorSets(m_commandBuffers[m_currentFrame], VK_PIPELINE_BIND_POINT_GRAPHICS,m_pipelineLayout,0,1,&m_descriptorSets[m_currentFrame], 0, nullptr);
        vkCmdPushConstants(m_commandBuffers[m_currentFrame], m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &m_transform);

        VkViewport viewport{};
        viewport.x          = 0.0f;
        viewport.y          = 0.0f;
        viewport.width      = (float)m_swapChainExtent.width;
        viewport.height     = (float) m_swapChainExtent.height;
        viewport.minDepth   = 0.0f;
        viewport.maxDepth   = 1.0f;
        vkCmdSetViewport(m_commandBuffers[m_currentFrame], 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {0,0};
        scissor.extent = m_swapChainExtent;
        vkCmdSetScissor(m_commandBuffers[m_currentFrame], 0, 1, &scissor);

        VkBuffer vertexBuffers[] = { m_vertexBuffer};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(m_commandBuffers[m_currentFrame], 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(m_commandBuffers[m_currentFrame], m_indexBuffer, 0, VK_INDEX_TYPE_UINT16);

        vkCmdDrawIndexed(m_commandBuffers[m_currentFrame], (uint32_t)s_indices.size(), 1, 0, 0, 0);

        vkCmdEndRenderPass(m_commandBuffers[m_currentFrame]);
        vkEndCommandBuffer(m_commandBuffers[m_currentFrame]);
    }

    void Renderer::createPipeline()
    {
        auto vertCode = loadShaderFile("shaders/triangle.vert.spv");
        auto fragCode = loadShaderFile("shaders/triangle.frag.spv");

        VkShaderModule vertModule = createShaderModule(vertCode);
        VkShaderModule fragModule = createShaderModule(fragCode);

        // 1. Shader stages — qué shader va en cada etapa
        VkPipelineShaderStageCreateInfo vertStage{};
        vertStage.sType     = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertStage.stage     = VK_SHADER_STAGE_VERTEX_BIT;
        vertStage.module    = vertModule;
        vertStage.pName     = "main";

        VkPipelineShaderStageCreateInfo fragStage{};
        fragStage.sType     = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragStage.stage     = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragStage.module    = fragModule;
        fragStage.pName     = "main";

        VkPipelineShaderStageCreateInfo stages[] = {vertStage,fragStage};

        // 2. Vertex input — sin vertex buffer, las posiciones van en el shader
        VkVertexInputBindingDescription bindingDesc{};
        bindingDesc.binding     = 0;
        bindingDesc.stride      = sizeof(Vertex);
        bindingDesc.inputRate   = VK_VERTEX_INPUT_RATE_VERTEX;

        // Atributo 0: pos (vec2, offset 0)
        VkVertexInputAttributeDescription attrDescs[3]{};
        attrDescs[0].binding    = 0;
        attrDescs[0].location   = 0;
        attrDescs[0].format     = VK_FORMAT_R32G32B32_SFLOAT;
        attrDescs[0].offset     = offsetof(Vertex, pos);

        // Atributo 1: color (vec3, offset después de pos)
        attrDescs[1].binding    = 0;
        attrDescs[1].location   = 1;
        attrDescs[1].format     = VK_FORMAT_R32G32B32_SFLOAT;
        attrDescs[1].offset     = offsetof(Vertex, color);

        // UV
        attrDescs[2].binding    = 0;
        attrDescs[2].location   = 2;
        attrDescs[2].format     = VK_FORMAT_R32G32_SFLOAT;
        attrDescs[2].offset     = offsetof(Vertex, uv);

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType                               = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount       = 1;
        vertexInput.pVertexBindingDescriptions          = &bindingDesc;
        vertexInput.vertexAttributeDescriptionCount     = 3;
        vertexInput.pVertexAttributeDescriptions        = attrDescs;

        // 3. Input assembly — qué primitivo forman los vértices
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType     = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology  = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        // 4. Viewport y scissor — dinámicos, los seteamos en recordCommandBuffer
        VkPipelineViewportStateCreateInfo viewportInfo{};
        viewportInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportInfo.viewportCount  = 1;
        viewportInfo.scissorCount   = 1;

        // 5. Rasterizer — cómo se rellena el triángulo
        VkPipelineRasterizationStateCreateInfo rasterizationInfo{};
        rasterizationInfo.sType         = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizationInfo.polygonMode   = VK_POLYGON_MODE_FILL;
        rasterizationInfo.cullMode      = VK_CULL_MODE_BACK_BIT;
        rasterizationInfo.frontFace     = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizationInfo.lineWidth     = 1.0f;

        // 6. Multisampling — sin MSAA por ahora
        VkPipelineMultisampleStateCreateInfo multisampleInfo{};
        multisampleInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampleInfo.rasterizationSamples    = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable       = VK_TRUE;
        depthStencil.depthWriteEnable      = VK_TRUE;
        depthStencil.depthCompareOp        = VK_COMPARE_OP_LESS;
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.stencilTestEnable     = VK_FALSE;

        // 7. Color blending — opaco, sin transparencia
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlendInfo{};
        colorBlendInfo.sType            = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlendInfo.attachmentCount  = 1;
        colorBlendInfo.pAttachments     = &blendAttachment;

        // 8. Dynamic state — viewport y scissor los cambiamos en runtime (no hardcoded aquí)
        VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamicInfo{};
        dynamicInfo.sType               = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicInfo.dynamicStateCount   = 2;
        dynamicInfo.pDynamicStates      = dynamicStates;

        // 9. Pipeline layout — sin descriptors ni push constants por ahora
        VkPushConstantRange pushRange{};
        pushRange.stageFlags    = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.offset        = 0;
        pushRange.size          = sizeof(glm::mat4);

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType                    = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount           = 1;
        layoutInfo.pSetLayouts              = &m_descriptorSetLayout;
        layoutInfo.pushConstantRangeCount   = 1;
        layoutInfo.pPushConstantRanges      = &pushRange;
        if(vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create pipeline layout!");
        }

        // 10. Pipeline completo
        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType                  = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount             = 2;
        pipelineInfo.pStages                = stages;
        pipelineInfo.pVertexInputState      = &vertexInput;
        pipelineInfo.pInputAssemblyState    = &inputAssembly;
        pipelineInfo.pViewportState         = &viewportInfo;
        pipelineInfo.pRasterizationState    = &rasterizationInfo;
        pipelineInfo.pMultisampleState      = &multisampleInfo;
        pipelineInfo.pColorBlendState       = &colorBlendInfo;
        pipelineInfo.pDynamicState          = &dynamicInfo;
        pipelineInfo.layout                 = m_pipelineLayout;
        pipelineInfo.renderPass             = m_renderPass;
        pipelineInfo.subpass                = 0;
        pipelineInfo.pDepthStencilState     = &depthStencil;

        if(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create graphics pipeline!");
        }

        // los módulos se destruyen al final de esta función — solo los necesita el pipeline
        vkDestroyShaderModule(m_device, vertModule, nullptr);
        vkDestroyShaderModule(m_device, fragModule, nullptr);
        printf("pipeline OK\n"); fflush(stdout);
    }

    std::vector<char> Renderer::loadShaderFile(const std::string& path)
    {
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if(!file.is_open())
        {
            throw std::runtime_error("failed to open shader: " + path);
        }
        size_t size = (size_t)file.tellg();
        std::vector<char> buffer(size);
        file.seekg(0);
        file.read(buffer.data(), (std::streamsize)size);
        return buffer;
    }

    VkShaderModule Renderer::createShaderModule(const std::vector<char>& code)
    {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t *>(code.data());
        VkShaderModule shaderModule;
        if(vkCreateShaderModule(m_device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create shader module!");
        }
        return shaderModule;
    }

    void Renderer::recreateSwapChain(Window& window)
    {
        int width = 0;
        int height = 0;

        glfwGetFramebufferSize(window.getNativeWindow(), &width, &height);
        // Espera si la ventana está minimizada (0x0)
        while(width == 0 || height == 0)
        {
            glfwWaitEvents();
            glfwGetFramebufferSize(window.getNativeWindow(), &width, &height);
        }

        vkDeviceWaitIdle(m_device);

        // Teardown
        for(auto semaphore: m_renderFinished)
        {
            vkDestroySemaphore(m_device, semaphore, nullptr);
        }
        m_renderFinished.clear();

        for(auto frameBuffer : m_swapChainFramebuffers)
        {
            vkDestroyFramebuffer(m_device, frameBuffer, nullptr);
        }
        m_swapChainFramebuffers.clear();

        for(auto imageView : m_swapChainImageViews)
        {
            vkDestroyImageView(m_device, imageView, nullptr);
        }
        m_swapChainImageViews.clear();

        vkDestroyImageView(m_device, m_depthImageView, nullptr);
        vkDestroyImage(m_device, m_depthImage, nullptr);
        vkFreeMemory(m_device, m_depthImageMemory, nullptr);
        vkDestroySwapchainKHR(m_device, m_swapChain, nullptr);
        m_swapChain = VK_NULL_HANDLE;

        // Recreate
        createSwapChain(window);
        createImageViews();
        createDepthResources();
        createFramebuffers();

        // Solo recrear los semáforos que dependen del image count
        m_renderFinished.resize(m_swapChainImages.size());
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (auto& sem : m_renderFinished)
            if (vkCreateSemaphore(m_device, &semInfo, nullptr, &sem) != VK_SUCCESS)
                throw std::runtime_error("failed to create renderFinished semaphore!");
    }

    uint32_t Renderer::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags props)
    {
       VkPhysicalDeviceMemoryProperties memProps;
       vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);

       for(uint32_t i = 0; i < memProps.memoryTypeCount; i++)
       {
        if((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props)
            return i;
       }

       throw std::runtime_error("failed to find suitable memory type!");
    }

    void Renderer::createVertexBuffer()
    {
        VkDeviceSize size = sizeof(s_vertices[0]) * s_vertices.size();

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        createBuffer(size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stagingBuffer, stagingMemory);

        void* data;
        vkMapMemory(m_device, stagingMemory, 0, size, 0, &data);
        memcpy(data, s_vertices.data(), (size_t)size);
        vkUnmapMemory(m_device, stagingMemory);

        createBuffer(size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            m_vertexBuffer, m_vertexBufferMemory);

        copyBuffer(stagingBuffer, m_vertexBuffer, size);

        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
    }

    void Renderer::createIndexBuffer()
    {
        VkDeviceSize size = sizeof(s_indices[0]) * s_indices.size();

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        createBuffer(size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stagingBuffer, stagingMemory);

        void* data;
        vkMapMemory(m_device, stagingMemory, 0, size, 0, &data);
        memcpy(data, s_indices.data(), (size_t)size);
        vkUnmapMemory(m_device, stagingMemory);

        createBuffer(size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            m_indexBuffer, m_indexBufferMemory);

        copyBuffer(stagingBuffer, m_indexBuffer, size);

        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);

    }

    void Renderer::createDescriptorSetLayout()
    {
        VkDescriptorSetLayoutBinding uboBinding{};
        uboBinding.binding          = 0;
        uboBinding.descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uboBinding.descriptorCount  = 1;
        uboBinding.stageFlags       = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutBinding samplerBinding{};
        samplerBinding.binding         = 1;
        samplerBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerBinding.descriptorCount = 1;
        samplerBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding bindings[] = { uboBinding, samplerBinding};

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType            = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount     = 2;
        layoutInfo.pBindings        = bindings;

        if(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create descriptor set layout!");
        }
    }

    void Renderer::createUniformBuffers()
    {
        VkDeviceSize size = sizeof(UniformBufferObject);
        for (int i = 0; i < MAX_FRAMES; i++) 
        {
            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType        = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size         = size;
            bufferInfo.usage        = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            bufferInfo.sharingMode  = VK_SHARING_MODE_EXCLUSIVE;
            vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_uniformBuffers[i]);

            VkMemoryRequirements memoryRequirements;
            vkGetBufferMemoryRequirements(m_device, m_uniformBuffers[i], &memoryRequirements);

            VkMemoryAllocateInfo allocInfo{};
            allocInfo.sType             = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize    = memoryRequirements.size;
            allocInfo.memoryTypeIndex   = findMemoryType(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkAllocateMemory(m_device, &allocInfo, NULL, &m_uniformBuffersMemory[i]);
            vkBindBufferMemory(m_device, m_uniformBuffers[i], m_uniformBuffersMemory[i], 0);

            // Mapeo persistente — nunca llamamos unmap
            vkMapMemory(m_device, m_uniformBuffersMemory[i], 0, size, 0, &m_uniformBuffersMapped[i]);

        }
    }

    void Renderer::createDescriptorPool()
    {
        VkDescriptorPoolSize poolSizes[2]{};
        poolSizes[0].type               = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount    = (uint32_t)MAX_FRAMES;
        poolSizes[1].type               = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[1].descriptorCount    = (uint32_t)MAX_FRAMES;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType          = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount  = 2;
        poolInfo.pPoolSizes     = poolSizes;
        poolInfo.maxSets        = (uint32_t)MAX_FRAMES;

        if(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create descriptor pool!");
        }
    }

    void Renderer::createDescriptorSets()
    {
        VkDescriptorSetLayout layouts[MAX_FRAMES];
        for(int i = 0; i < MAX_FRAMES; i++)
        {
            layouts[i] = m_descriptorSetLayout;
        }

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType                 = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool        = m_descriptorPool;
        allocInfo.descriptorSetCount    = (uint32_t)MAX_FRAMES;
        allocInfo.pSetLayouts           = layouts;

        if(vkAllocateDescriptorSets(m_device, &allocInfo, m_descriptorSets) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to allocate descriptor sets!");
        }

        for(int i = 0; i < MAX_FRAMES; i++)
        {
            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer   = m_uniformBuffers[i];
            bufferInfo.offset   = 0;
            bufferInfo.range    = sizeof(UniformBufferObject);

            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageLayout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView     = m_textureImageView;
            imageInfo.sampler       = m_textureSampler;

            VkWriteDescriptorSet write{};
            write.sType             = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet            = m_descriptorSets[i];
            write.dstBinding        = 0;
            write.dstArrayElement   = 0;
            write.descriptorType    = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write.descriptorCount   = 1;
            write.pBufferInfo       = &bufferInfo;

            VkWriteDescriptorSet writes[2]{};
            writes[0] = write;

            writes[1].sType             = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet            = m_descriptorSets[i];
            writes[1].dstBinding        = 1;
            writes[1].dstArrayElement   = 0;
            writes[1].descriptorType    = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].descriptorCount   = 1;
            writes[1].pImageInfo        = &imageInfo;

            vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);
        }
        
    }

    void Renderer::updateUniformBuffer(uint32_t frameIndex)
    {
        UniformBufferObject ubo{};
        ubo.view = glm::lookAt(
            glm::vec3(0.0f, 0.0f, 2.0f),        // camera
            glm::vec3(0.0f, 0.0f, 0.0f),     // origin        
            glm::vec3(0.0f, 1.0f, 0.0f)          // up Y+
        );

        ubo.proj = glm::perspective(
        glm::radians(45.0f),
        (float)m_swapChainExtent.width / (float)m_swapChainExtent.height,
        0.1f, 10.0f);
        ubo.proj[1][1] *= -1.0f;    // Vulkan Y flip

        memcpy(m_uniformBuffersMapped[frameIndex], &ubo, sizeof(ubo));        
    }

    void Renderer::createDepthResources()
    {
        VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType             = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType         = VK_IMAGE_TYPE_2D;
        imageInfo.format            = depthFormat;
        imageInfo.extent            = { m_swapChainExtent.width, m_swapChainExtent.height, 1 };
        imageInfo.mipLevels         = 1;
        imageInfo.arrayLayers       = 1;
        imageInfo.samples           = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling            = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage             = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        imageInfo.sharingMode       = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout     = VK_IMAGE_LAYOUT_UNDEFINED;

        if(vkCreateImage(m_device, &imageInfo, nullptr, &m_depthImage) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create depth image!");
        }

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(m_device, m_depthImage, &memReq);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType             = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize    = memReq.size;
        allocInfo.memoryTypeIndex   = findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if(vkAllocateMemory(m_device, &allocInfo, nullptr, &m_depthImageMemory) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to allocate depth image memory!");
        }

        vkBindImageMemory(m_device, m_depthImage, m_depthImageMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_depthImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = depthFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if(vkCreateImageView(m_device, &viewInfo, nullptr, &m_depthImageView) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create depth image view!");
        }
    }

    void Renderer::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer& buffer, VkDeviceMemory& memory)
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType        = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size         = size;
        bufferInfo.usage        = usage;
        bufferInfo.sharingMode  = VK_SHARING_MODE_EXCLUSIVE;

        if(vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create buffer!");
        }

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(m_device, buffer, &req);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType             = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize    = req.size;
        allocInfo.memoryTypeIndex   = findMemoryType(req.memoryTypeBits, props);

        if(vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to allocate buffer memory!");
        }

        vkBindBufferMemory(m_device, buffer, memory, 0);
    }

    void Renderer::copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size)
    {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType                 = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level                 = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool           = m_commandPool;
        allocInfo.commandBufferCount    = 1;

        VkCommandBuffer commandBuffer;
        vkAllocateCommandBuffers(m_device, &allocInfo, &commandBuffer);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commandBuffer, &beginInfo);

        VkBufferCopy region{};
        region.size = size;
        vkCmdCopyBuffer(commandBuffer, src, dst, 1, &region);

        vkEndCommandBuffer(commandBuffer);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);

        vkFreeCommandBuffers(m_device, m_commandPool, 1, &commandBuffer);
    }

    VkCommandBuffer Renderer::beginOneTimeCommands()
    {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType                 = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level                 = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool           = m_commandPool;
        allocInfo.commandBufferCount    = 1;

        VkCommandBuffer cmd;
        vkAllocateCommandBuffers(m_device, &allocInfo, &cmd);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);
        return cmd;
    } 

    void Renderer::endOneTimeCommands(VkCommandBuffer cmd)
    {
        vkEndCommandBuffer(cmd);

        VkSubmitInfo submitInfo{};
        submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount   = 1;
        submitInfo.pCommandBuffers      = &cmd;
        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);

        vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    }

    void Renderer::createImage(uint32_t w, uint32_t h, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags props, VkImage& image, VkDeviceMemory& memory)
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType             = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType         = VK_IMAGE_TYPE_2D;
        imageInfo.format            = format;
        imageInfo.extent            = { w, h, 1};
        imageInfo.mipLevels         = 1,
        imageInfo.arrayLayers       = 1;
        imageInfo.samples           = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling            = tiling;
        imageInfo.usage             = usage;
        imageInfo.sharingMode       = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout     = VK_IMAGE_LAYOUT_UNDEFINED;

        if(vkCreateImage(m_device, &imageInfo, nullptr, &image) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create image!");
        }

        VkMemoryRequirements memoryRequirement;
        vkGetImageMemoryRequirements(m_device, image, &memoryRequirement);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType             = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize    = memoryRequirement.size;
        allocInfo.memoryTypeIndex   = findMemoryType(memoryRequirement.memoryTypeBits, props);

        if(vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to allocate image memory!");
        }

        vkBindImageMemory(m_device, image, memory, 0);
    }

    void Renderer::transitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout)
    {
        VkCommandBuffer cmd = beginOneTimeCommands();

        VkImageMemoryBarrier barrier{};
        barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout                       = oldLayout;
        barrier.newLayout                       = newLayout;
        barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                           = image;
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel   = 0;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;

        VkPipelineStageFlags srcStage, dstStage;

        if(oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
        {
            barrier.srcAccessMask   = 0;
            barrier.dstAccessMask   = VK_ACCESS_TRANSFER_WRITE_BIT;
            srcStage                = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            dstStage                = VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        else if( oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            barrier.srcAccessMask   = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask   = VK_ACCESS_SHADER_READ_BIT;
            srcStage                = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dstStage                = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        else 
        {
            throw std::runtime_error("unsupported layout transition!");
        }
        
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        endOneTimeCommands(cmd);
    }

    void Renderer::copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t w, uint32_t h)
    {
        VkCommandBuffer cmd = beginOneTimeCommands();

        VkBufferImageCopy region{};
        region.bufferOffset                     = 0;
        region.bufferRowLength                  = 0;
        region.bufferImageHeight                = 0;
        region.imageSubresource.aspectMask      = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel        = 0;
        region.imageSubresource.baseArrayLayer  = 0;
        region.imageSubresource.layerCount      = 1;
        region.imageOffset                      = {0,0,0};
        region.imageExtent                      = {w,h,1};

        vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        endOneTimeCommands(cmd);
    }

    void Renderer::createTextureImage()
    {
        int w, h, channels;
        stbi_uc* pixels = stbi_load("assets/texture.png", &w, &h, &channels, STBI_rgb_alpha);
        if (!pixels)
        {
            
            throw std::runtime_error(stbi_failure_reason());            
        }
            
        VkDeviceSize imageSize = w * h * 4;

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        createBuffer(imageSize,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stagingBuffer, stagingMemory);

        void* data;
        vkMapMemory(m_device, stagingMemory, 0, imageSize, 0, &data);
        memcpy(data, pixels, (size_t)imageSize);
        vkUnmapMemory(m_device, stagingMemory);
        stbi_image_free(pixels);

        createImage((uint32_t)w, (uint32_t)h,
            VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            m_textureImage, m_textureImageMemory);

        transitionImageLayout(m_textureImage,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        copyBufferToImage(stagingBuffer, m_textureImage, (uint32_t)w, (uint32_t)h);

        transitionImageLayout(m_textureImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
    }

    void Renderer::createTextureImageView()
    {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                              = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                              = m_textureImage;
        viewInfo.viewType                           = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                             = VK_FORMAT_R8G8B8A8_SRGB;
        viewInfo.subresourceRange.aspectMask        = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel      = 0,
        viewInfo.subresourceRange.levelCount        = 1;
        viewInfo.subresourceRange.baseArrayLayer    = 0;
        viewInfo.subresourceRange.layerCount        = 1;

        if(vkCreateImageView(m_device, &viewInfo, nullptr, &m_textureImageView) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create texture image view!");
        }
    }

    void Renderer::createTextureSampler()
    {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter               = VK_FILTER_LINEAR;
        samplerInfo.minFilter               = VK_FILTER_LINEAR;
        samplerInfo.addressModeU            = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV            = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW            = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.anisotropyEnable        = VK_FALSE;
        samplerInfo.borderColor             = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        samplerInfo.compareEnable           = VK_FALSE;
        samplerInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;

        if(vkCreateSampler(m_device, &samplerInfo, nullptr, &m_textureSampler) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create texture sampler!");
        }
    }
}
