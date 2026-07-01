#include "DonTopo/Renderer.h"
#include <GLFW/glfw3.h>
#include <stdexcept>
#include "DonTopo/Window.h"
#include <algorithm>
#include <fstream>
#include "DonTopo/Vertex.h"
#include <glm/gtc/matrix_transform.hpp>
#include "DonTopo/UniformBufferObject.h"
#include <limits>
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

    void Renderer::init(Window& window, const std::vector<Mesh>& meshes)
    {

        // Auto-fit camera to mesh bounding box
        glm::vec3 bMin( std::numeric_limits<float>::max());
        glm::vec3 bMax(-std::numeric_limits<float>::max());

        for(auto& mesh : meshes)
        {
            for (auto& v : mesh.vertices) 
            {
                bMin = glm::min(bMin, v.pos);
                bMax = glm::max(bMax, v.pos);
            }
        }
        
        m_cameraTarget   = (bMin + bMax) * 0.5f;
        float maxDim     = glm::max(bMax.x - bMin.x, glm::max(bMax.y - bMin.y, bMax.z - bMin.z));
        m_cameraDistance = maxDim * 1.2f;

        createInstance();
        setupDebugMessenger();
        createSurface(window);
        pickPhysicalDevice();
        createDevice();
        createSwapChain(window);

        createImageViews();
        createDepthResources();
        createRenderPass();
        createDescriptorSetLayout();
        createPipeline();
        createShadowResources();
        createFramebuffers();
        createCommandPool();

        m_objects.resize(meshes.size());
        for(size_t i = 0; i < meshes.size(); i++)
        {
            buildRenderObject(meshes[i], m_objects[i]);
        }
       
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
        for(auto& obj : m_objects)
            destroyRenderObject(obj);
        m_objects.clear();
        for(int i = 0; i < MAX_FRAMES; i++)
        {
            vkDestroyBuffer(m_device, m_uniformBuffers[i], nullptr);
            vkFreeMemory(m_device, m_uniformBuffersMemory[i], nullptr);
        }
        vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
        vkDestroyImageView(m_device, m_depthImageView, nullptr);
        vkDestroyImage(m_device, m_depthImage, nullptr);
        vkFreeMemory(m_device, m_depthImageMemory, nullptr);
        // Shadow Map
        vkDestroySampler(m_device, m_shadowSampler, nullptr);
        vkDestroyImageView(m_device, m_shadowView, nullptr);
        vkDestroyImage(m_device, m_shadowImage, nullptr);
        vkFreeMemory(m_device, m_shadowMemory, nullptr);
        vkDestroyFramebuffer(m_device, m_shadowFramebuffer, nullptr);
        vkDestroyPipeline(m_device, m_shadowPipeline, nullptr);
        vkDestroyPipelineLayout(m_device, m_shadowPipelineLayout, nullptr);
        vkDestroyRenderPass(m_device, m_shadowRenderPass, nullptr);
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

    void Renderer::setCamera(const Camera& camera)
    {
        m_viewMatrix = camera.getViewMatrix();
        m_camera = camera;
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

        recordShadowPass(m_commandBuffers[m_currentFrame]);

        vkCmdBeginRenderPass(m_commandBuffers[m_currentFrame], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(m_commandBuffers[m_currentFrame], VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
        VkViewport viewport{};
        viewport.x          = 0.0f;
        viewport.y          = 0.0f;
        viewport.width      = (float)m_swapChainExtent.width;
        viewport.height     = (float)m_swapChainExtent.height;
        viewport.minDepth   = 0.0f;
        viewport.maxDepth   = 1.0f;
        vkCmdSetViewport(m_commandBuffers[m_currentFrame], 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {0,0};
        scissor.extent = m_swapChainExtent;
        vkCmdSetScissor(m_commandBuffers[m_currentFrame], 0, 1, &scissor);

        for (auto& obj : m_objects)
        {
            vkCmdBindDescriptorSets(m_commandBuffers[m_currentFrame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_pipelineLayout, 0, 1, &obj.descriptorSets[m_currentFrame], 0, nullptr);
            vkCmdPushConstants(m_commandBuffers[m_currentFrame], m_pipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &obj.transform);

            VkBuffer vbs[]        = { obj.vertexBuffer };
            VkDeviceSize offs[]   = { 0 };
            vkCmdBindVertexBuffers(m_commandBuffers[m_currentFrame], 0, 1, vbs, offs);
            vkCmdBindIndexBuffer(m_commandBuffers[m_currentFrame], obj.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(m_commandBuffers[m_currentFrame], obj.indexCount, 1, 0, 0, 0);
        }

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
        VkVertexInputAttributeDescription attrDescs[5]{};
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

        // normals
        attrDescs[3].binding    = 0;
        attrDescs[3].location   = 3;
        attrDescs[3].format     = VK_FORMAT_R32G32B32_SFLOAT;
        attrDescs[3].offset     = offsetof(Vertex, normal);

        // tangents
        attrDescs[4].binding    = 0;
        attrDescs[4].location   = 4;
        attrDescs[4].format     = VK_FORMAT_R32G32B32_SFLOAT;
        attrDescs[4].offset     = offsetof(Vertex, tangent);

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType                               = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount       = 1;
        vertexInput.pVertexBindingDescriptions          = &bindingDesc;
        vertexInput.vertexAttributeDescriptionCount     = 5;
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

    void Renderer::createVertexBuffer(const std::vector<Vertex>& vertices, VkBuffer& buf, VkDeviceMemory& mem)
    {
        VkDeviceSize size = sizeof(vertices[0]) * vertices.size();

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        createBuffer(size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stagingBuffer, stagingMemory);

        void* data;
        vkMapMemory(m_device, stagingMemory, 0, size, 0, &data);
        memcpy(data, vertices.data(), (size_t)size);
        vkUnmapMemory(m_device, stagingMemory);

        createBuffer(size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            buf, mem);

        copyBuffer(stagingBuffer, buf, size);

        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
    }

    void Renderer::createIndexBuffer(const std::vector<uint32_t>& idx, VkBuffer& buf, VkDeviceMemory& mem)
    {
        VkDeviceSize size = sizeof(idx[0]) * idx.size();

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        createBuffer(size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stagingBuffer, stagingMemory);

        void* data;
        vkMapMemory(m_device, stagingMemory, 0, size, 0, &data);
        memcpy(data, idx.data(), (size_t)size);
        vkUnmapMemory(m_device, stagingMemory);

        createBuffer(size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            buf, mem);

        copyBuffer(stagingBuffer, buf, size);

        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);

    }

    void Renderer::createDescriptorSetLayout()
    {
        VkDescriptorSetLayoutBinding uboBinding{};
        uboBinding.binding          = 0;
        uboBinding.descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uboBinding.descriptorCount  = 1;
        uboBinding.stageFlags       = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding samplerBinding{};
        samplerBinding.binding         = 1;
        samplerBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerBinding.descriptorCount = 1;
        samplerBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding samplerNormal{};
        samplerNormal.binding         = 2;
        samplerNormal.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerNormal.descriptorCount = 1;
        samplerNormal.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding shadowBinding{};
        shadowBinding.binding         = 3;
        shadowBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadowBinding.descriptorCount = 1;
        shadowBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding bindings[] = { uboBinding, samplerBinding, samplerNormal, shadowBinding };

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType            = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount     = 4;
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
        uint32_t n = (uint32_t)(m_objects.size() * MAX_FRAMES);
        VkDescriptorPoolSize poolSizes[2]{};
        poolSizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = n;
        poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[1].descriptorCount = n * 3;   // diffuse + normal map + shadow

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes    = poolSizes;
        poolInfo.maxSets       = n;

        if(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create descriptor pool!");
    }

    void Renderer::createDescriptorSets()
    {
        VkDescriptorSetLayout layouts[MAX_FRAMES] = { m_descriptorSetLayout, m_descriptorSetLayout };

        for(auto& obj : m_objects)
        {
            VkDescriptorSetAllocateInfo allocInfo{};
            allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool     = m_descriptorPool;
            allocInfo.descriptorSetCount = MAX_FRAMES;
            allocInfo.pSetLayouts        = layouts;

            if(vkAllocateDescriptorSets(m_device, &allocInfo, obj.descriptorSets) != VK_SUCCESS)
                throw std::runtime_error("failed to allocate descriptor sets!");

            for(int i = 0; i < MAX_FRAMES; i++)
            {
                VkDescriptorBufferInfo bufferInfo{};
                bufferInfo.buffer = m_uniformBuffers[i];
                bufferInfo.offset = 0;
                bufferInfo.range  = sizeof(UniformBufferObject);

                VkDescriptorImageInfo imageInfo{};
                imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                imageInfo.imageView   = obj.textureView;
                imageInfo.sampler     = obj.sampler;

                VkDescriptorImageInfo normalInfo{};
                normalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                normalInfo.imageView   = obj.normalView;
                normalInfo.sampler     = obj.normalSampler;

                VkDescriptorImageInfo shadowInfo{};
                shadowInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                shadowInfo.imageView   = m_shadowView;
                shadowInfo.sampler     = m_shadowSampler;

                VkWriteDescriptorSet writes[4]{};
                writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[0].dstSet          = obj.descriptorSets[i];
                writes[0].dstBinding      = 0;
                writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                writes[0].descriptorCount = 1;
                writes[0].pBufferInfo     = &bufferInfo;

                writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[1].dstSet          = obj.descriptorSets[i];
                writes[1].dstBinding      = 1;
                writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[1].descriptorCount = 1;
                writes[1].pImageInfo      = &imageInfo;

                writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[2].dstSet          = obj.descriptorSets[i];
                writes[2].dstBinding      = 2;
                writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[2].descriptorCount = 1;
                writes[2].pImageInfo      = &normalInfo;

                writes[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[3].dstSet          = obj.descriptorSets[i];
                writes[3].dstBinding      = 3;
                writes[3].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[3].descriptorCount = 1;
                writes[3].pImageInfo      = &shadowInfo;

                vkUpdateDescriptorSets(m_device, 4, writes, 0, nullptr);
            }
        }
    }

    void Renderer::updateUniformBuffer(uint32_t frameIndex)
    {
        UniformBufferObject ubo{};
        ubo.view = m_viewMatrix;        

        ubo.proj = glm::perspective(
            glm::radians(45.0f),
            (float)m_swapChainExtent.width / (float)m_swapChainExtent.height,
            m_cameraDistance * 0.001f,
            m_cameraDistance * 3.0f);
        ubo.proj[1][1] *= -1.0f;    // Vulkan Y flip
        ubo.numLights = std::min((int)m_lights.size(), MAX_LIGHTS);
        for(int i = 0; i < ubo.numLights; i++)
        {
            ubo.lights[i] = m_lights[i];
        }
        
        ubo.viewPos  = glm::vec4(m_camera.getPos(), 1.0f);
        if (!m_lights.empty())
        {
            glm::vec3 lpos     = glm::vec3(m_lights[0].position);
            glm::mat4 lightView = glm::lookAt(lpos, glm::vec3(0.0f), glm::vec3(0.0f,1.0f,0.0f));
            glm::mat4 lightProj = glm::orthoRH_ZO(-350.0f, 350.0f, -350.0f, 350.0f, 1.0f, 2000.0f);
            lightProj[1][1] *= -1.0f;
            ubo.lightSpaceMatrix = lightProj * lightView;
        }

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

    void Renderer::createTextureImage(const std::string& path, const std::vector<uint8_t>& embedded, VkImage& img, VkDeviceMemory& mem)
    {
        int w, h, channels;
        stbi_uc* pixels = nullptr;
        bool fromStb = false;

        if (!embedded.empty())
        {
            pixels = stbi_load_from_memory(embedded.data(), (int)embedded.size(), &w, &h, &channels, STBI_rgb_alpha);
            fromStb = (pixels != nullptr);
        }
        else if (!path.empty())
        {
            pixels = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
            fromStb = (pixels != nullptr);
        }

        // Fallback: purple/black checkerboard when no texture or file not found
        std::vector<uint8_t> placeholder;
        if (!pixels)
        {
            constexpr int SIZE = 64, TILE = 8;
            placeholder.resize(SIZE * SIZE * 4);
            for (int py = 0; py < SIZE; py++) {
                for (int px = 0; px < SIZE; px++) {
                    bool check = ((px / TILE) + (py / TILE)) % 2 == 0;
                    uint8_t* p = placeholder.data() + (py * SIZE + px) * 4;
                    p[0] = check ? 0xCC : 0x88;
                    p[1] = check ? 0xCC : 0x88;
                    p[2] = check ? 0xCC : 0x88;
                    p[3] = 0xFF;
                }
            }
            pixels = placeholder.data();
            w = h = SIZE;
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
        if (fromStb) stbi_image_free(pixels);

        createImage((uint32_t)w, (uint32_t)h,
            VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            img, mem);

        transitionImageLayout(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        copyBufferToImage(stagingBuffer, img, (uint32_t)w, (uint32_t)h);
        transitionImageLayout(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
    }

    void Renderer::createNormalMapImage(const std::string& path, const std::vector<uint8_t>& embedded, VkImage& img, VkDeviceMemory& mem)
    {
        int w, h, channels;
        stbi_uc* pixels = nullptr;
        bool fromStb = false;

        if (!embedded.empty())
        {
            pixels = stbi_load_from_memory(embedded.data(), (int)embedded.size(), &w, &h, &channels, STBI_rgb_alpha);
            fromStb = (pixels != nullptr);
        }
        else if (!path.empty())
        {
            pixels = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
            fromStb = (pixels != nullptr);
        }

        // Fallback: flat normal (0,0,1) en tangent space = (128,128,255)
        uint8_t flatNormal[4] = { 0x80, 0x80, 0xFF, 0xFF };
        if (!pixels)
        {
            pixels = flatNormal;
            w = h = 1;
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
        if (fromStb) stbi_image_free(pixels);

        createImage((uint32_t)w, (uint32_t)h,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            img, mem);

        transitionImageLayout(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        copyBufferToImage(stagingBuffer, img, (uint32_t)w, (uint32_t)h);
        transitionImageLayout(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
    }

    void Renderer::createTextureImageView(VkImage image, VkImageView& view, VkFormat format)
    {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                              = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                              = image;
        viewInfo.viewType                           = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                             = format;
        viewInfo.subresourceRange.aspectMask        = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel      = 0;
        viewInfo.subresourceRange.levelCount        = 1;
        viewInfo.subresourceRange.baseArrayLayer    = 0;
        viewInfo.subresourceRange.layerCount        = 1;

        if(vkCreateImageView(m_device, &viewInfo, nullptr, &view) != VK_SUCCESS)
            throw std::runtime_error("failed to create texture image view!");
    }

    void Renderer::createTextureSampler(VkSampler& outSampler)
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

        if(vkCreateSampler(m_device, &samplerInfo, nullptr, &outSampler) != VK_SUCCESS)
            throw std::runtime_error("failed to create texture sampler!");
    }

    void Renderer::buildRenderObject(const Mesh& mesh, RenderObject& obj)
    {
        obj.indexCount = (uint32_t)mesh.indices.size();
        createVertexBuffer(mesh.vertices,                               obj.vertexBuffer, obj.vertexMemory);
        createIndexBuffer(mesh.indices,                                 obj.indexBuffer,  obj.indexMemory);
        createTextureImage(mesh.texturePath, mesh.embeddedTexture,         obj.textureImage, obj.textureMem);
        createTextureImageView(obj.textureImage,                           obj.textureView);
        createTextureSampler(obj.sampler);
        createNormalMapImage(mesh.normalMapPath, mesh.embeddedNormalMap,   obj.normalImage,  obj.normalMem);
        createTextureImageView(obj.normalImage,                            obj.normalView, VK_FORMAT_R8G8B8A8_UNORM);
        createTextureSampler(obj.normalSampler);
    }

    void Renderer::destroyRenderObject(RenderObject& obj)
    {
        vkDestroySampler(m_device,   obj.normalSampler, nullptr);
        vkDestroyImageView(m_device, obj.normalView,    nullptr);
        vkDestroyImage(m_device,     obj.normalImage,   nullptr);
        vkFreeMemory(m_device,       obj.normalMem,     nullptr);
        vkDestroySampler(m_device,   obj.sampler,       nullptr);
        vkDestroyImageView(m_device, obj.textureView,   nullptr);
        vkDestroyImage(m_device,     obj.textureImage,  nullptr);
        vkFreeMemory(m_device,       obj.textureMem,    nullptr);
        vkDestroyBuffer(m_device,    obj.indexBuffer,   nullptr);
        vkFreeMemory(m_device,       obj.indexMemory,   nullptr);
        vkDestroyBuffer(m_device,    obj.vertexBuffer,  nullptr);
        vkFreeMemory(m_device,       obj.vertexMemory,  nullptr);
    }

    void Renderer::createShadowResources()
    {
        // 1. Imagen depth para shadow map
        createImage(SHADOW_SIZE, SHADOW_SIZE, VK_FORMAT_D32_SFLOAT, VK_IMAGE_TILING_OPTIMAL, 
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_shadowImage, m_shadowMemory);

        // 2. Image view (depth aspect)
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                          = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                          = m_shadowImage;
        viewInfo.viewType                       = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                         = VK_FORMAT_D32_SFLOAT;
        viewInfo.subresourceRange.aspectMask    = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.layerCount    = 1;
        viewInfo.subresourceRange.levelCount    = 1;    
        if(vkCreateImageView(m_device, &viewInfo, nullptr, &m_shadowView) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create shadow image view!");
        }

        // 3. Sampler de comparación (PCF listo)
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter               = VK_FILTER_LINEAR;
        samplerInfo.minFilter               = VK_FILTER_LINEAR;
        samplerInfo.addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.borderColor             = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        samplerInfo.compareEnable           = VK_TRUE;
        samplerInfo.compareOp               = VK_COMPARE_OP_LESS_OR_EQUAL;
        samplerInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        if(vkCreateSampler(m_device, &samplerInfo, nullptr, &m_shadowSampler) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create shadow sampler!");
        }

        // 4. Render pass depth-only
        VkAttachmentDescription depthAttachment{};
        depthAttachment.format         = VK_FORMAT_D32_SFLOAT;
        depthAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        
        VkAttachmentReference depthAttachmentRef{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.pDepthStencilAttachment = &depthAttachmentRef;

        VkSubpassDependency dependencies[2]{};
        dependencies[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass      = 0;
        dependencies[0].srcStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[0].dstStageMask    = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependencies[0].srcAccessMask   = VK_ACCESS_SHADER_READ_BIT;
        dependencies[0].dstAccessMask   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[0].dependencyFlags  = VK_DEPENDENCY_BY_REGION_BIT;
        dependencies[1].srcSubpass      = 0;
        dependencies[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask    = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[1].dstStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].srcAccessMask   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask   = VK_ACCESS_SHADER_READ_BIT;
        dependencies[1].dependencyFlags  = VK_DEPENDENCY_BY_REGION_BIT;

        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType            = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount   = 1;
        renderPassInfo.pAttachments      = &depthAttachment;
        renderPassInfo.subpassCount      = 1;
        renderPassInfo.pSubpasses        = &subpass;
        renderPassInfo.dependencyCount   = 2;
        renderPassInfo.pDependencies     = dependencies;
        if(vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_shadowRenderPass) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create shadow render pass!");
        }

         // 5. Framebuffer
         VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass      = m_shadowRenderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments    = &m_shadowView;
        fbInfo.width           = SHADOW_SIZE;
        fbInfo.height          = SHADOW_SIZE;
        fbInfo.layers          = 1;
        if (vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_shadowFramebuffer) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create shadow framebuffer!");
        }            

        // 6. Pipeline (vertex-only, sin color attachments)
        auto vertCode = loadShaderFile("shaders/shadow.vert.spv");
        VkShaderModule vertModule = createShaderModule(vertCode);

        VkPipelineShaderStageCreateInfo vertStage{};
        vertStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertStage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
        vertStage.module = vertModule;
        vertStage.pName  = "main";

        VkVertexInputBindingDescription bindingDesc{};
        bindingDesc.binding   = 0;
        bindingDesc.stride    = sizeof(Vertex);
        bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attrDesc{};
        attrDesc.binding  = 0;
        attrDesc.location = 0;
        attrDesc.format   = VK_FORMAT_R32G32B32_SFLOAT;
        attrDesc.offset   = offsetof(Vertex, pos);

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount   = 1;
        vertexInput.pVertexBindingDescriptions      = &bindingDesc;
        vertexInput.vertexAttributeDescriptionCount = 1;
        vertexInput.pVertexAttributeDescriptions    = &attrDesc;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode             = VK_POLYGON_MODE_FILL;
        rasterizer.cullMode                = VK_CULL_MODE_NONE;
        rasterizer.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.lineWidth               = 1.0f;
        rasterizer.depthBiasEnable         = VK_TRUE;
        rasterizer.depthBiasConstantFactor = 1.25f;
        rasterizer.depthBiasSlopeFactor    = 1.75f;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable  = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 0; // sin color attachments

        VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = 2;
        dynamicState.pDynamicStates    = dynStates;

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.size       = sizeof(glm::mat4);

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount         = 1;
        layoutInfo.pSetLayouts            = &m_descriptorSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges    = &pushRange;
        if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_shadowPipelineLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create shadow pipeline layout!");
        }            

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount          = 1;
        pipelineInfo.pStages             = &vertStage;
        pipelineInfo.pVertexInputState   = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState      = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState   = &multisampling;
        pipelineInfo.pDepthStencilState  = &depthStencil;
        pipelineInfo.pColorBlendState    = &colorBlend;
        pipelineInfo.pDynamicState       = &dynamicState;
        pipelineInfo.layout              = m_shadowPipelineLayout;
        pipelineInfo.renderPass          = m_shadowRenderPass;
        if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_shadowPipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create shadow pipeline!");
        }            

        vkDestroyShaderModule(m_device, vertModule, nullptr);
    }

    void Renderer::recordShadowPass(VkCommandBuffer cmd)
    {
        VkClearValue clearDepth{};
        clearDepth.depthStencil = { 1.0f, 0 };

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass        = m_shadowRenderPass;
        renderPassInfo.framebuffer       = m_shadowFramebuffer;
        renderPassInfo.renderArea.extent = { SHADOW_SIZE, SHADOW_SIZE };
        renderPassInfo.clearValueCount   = 1;
        renderPassInfo.pClearValues      = &clearDepth;

        vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);

        
        VkViewport vp {0.0f, 0.0f, (float)SHADOW_SIZE, (float)SHADOW_SIZE, 0.0f, 1.0f};
        VkRect2D sc {{0,0}, {SHADOW_SIZE, SHADOW_SIZE}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);

        for(auto& obj : m_objects)
        {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipelineLayout, 0, 1, &obj.descriptorSets[m_currentFrame], 0, nullptr);
            vkCmdPushConstants(cmd, m_shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &obj.transform);

            VkBuffer vb[] = { obj.vertexBuffer };
            VkDeviceSize offsets[] = { 0 };
            vkCmdBindVertexBuffers(cmd, 0, 1, vb, offsets);
            vkCmdBindIndexBuffer(cmd, obj.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, obj.indexCount, 1, 0, 0, 0);
        }

        vkCmdEndRenderPass(cmd);
    }

}
