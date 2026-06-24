#include "DonTopo/Renderer.h"
#include <GLFW/glfw3.h>
#include <stdexcept>
#include "DonTopo/Window.h"
#include <algorithm>

namespace DonTopo {

    Renderer::~Renderer()
    {
        shutdown();
    }

    void Renderer::init(Window& window)
    {
        createInstance();
        createSurface(window);
        pickPhysicalDevice();
        createDevice();
        createSwapChain(window);
        createImageViews();
        createRenderPass();
        createFramebuffers();
        createCommandPool();
        createCommandBuffers();
        createSyncObjects();
    }

    void Renderer::drawFrame()
    {
        // 1. Espera a que el frame anterior terminó
        vkWaitForFences(m_device, 1, &m_inFlight[m_currentFrame], VK_TRUE, UINT64_MAX);
        vkResetFences(m_device, 1, &m_inFlight[m_currentFrame]);

        // 2. Pide la siguiente imagen del swapchain
        uint32_t imageIndex;
        if(vkAcquireNextImageKHR(m_device, m_swapChain, UINT64_MAX, m_imageAvailable[m_currentFrame], VK_NULL_HANDLE, &imageIndex) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to acquire next image!");
        }

        // 3. Graba el command buffer
        if(vkResetCommandBuffer(m_commandBuffers[m_currentFrame], 0) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to reset command buffer!");
        }

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
        submitInfo.pSignalSemaphores        = &m_renderFinished[m_currentFrame];
        if(vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_inFlight[m_currentFrame]) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to submit graphics queue!");
        }

        // 5. Presenta
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType               = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount  = 1;
        presentInfo.pWaitSemaphores     = &m_renderFinished[m_currentFrame];
        presentInfo.swapchainCount      = 1;
        presentInfo.pSwapchains         = &m_swapChain;
        presentInfo.pImageIndices       = &imageIndex;
        if(vkQueuePresentKHR(m_presentQueue, &presentInfo) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to present!");
        }

        m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES;
    }

    void Renderer::shutdown()
    {
        vkDeviceWaitIdle(m_device);
        for(int i = 0; i < MAX_FRAMES; i++)
        {
            vkDestroySemaphore(m_device, m_renderFinished[i], nullptr);
            vkDestroySemaphore(m_device, m_imageAvailable[i], nullptr);
            vkDestroyFence(m_device, m_inFlight[i], nullptr);
        }
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        for(auto framebuffer : m_swapChainFramebuffers)
        {
            vkDestroyFramebuffer(m_device, framebuffer, nullptr);
        }
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        for(VkImageView imageView : m_swapChainImageViews)
        {
            vkDestroyImageView(m_device, imageView, nullptr);
        }                        
        vkDestroySwapchainKHR(m_device, m_swapChain, nullptr);
        vkDestroyDevice(m_device, nullptr);
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
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
        const char **extensions = glfwGetRequiredInstanceExtensions(&extensionCount);

        VkInstanceCreateInfo createInfo{};
        createInfo.sType                    = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo         = &appInfo;
        createInfo.enabledExtensionCount    = extensionCount;
        createInfo.ppEnabledExtensionNames  = extensions;

        if(vkCreateInstance(&createInfo, nullptr, &m_instance) != VK_SUCCESS) {
            throw std::runtime_error("failed to create Vulkan instance!");
        }   
        printf("instance OK\n");  fflush(stdout);     
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

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount    = 1;
        subpass.pColorAttachments       = &colorReference;

        VkSubpassDependency subpassDependency{};
        subpassDependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
        subpassDependency.dstSubpass    = 0;
        subpassDependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        subpassDependency.srcAccessMask = 0;
        subpassDependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        subpassDependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo renderPassCreateInfo {};
        renderPassCreateInfo.sType              = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassCreateInfo.attachmentCount    = 1;
        renderPassCreateInfo.pAttachments       = &colorAttachment;
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
            VkFramebufferCreateInfo framebufferCreateInfo{};
            framebufferCreateInfo.sType             = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferCreateInfo.renderPass        = m_renderPass;
            framebufferCreateInfo.attachmentCount   = 1;
            framebufferCreateInfo.pAttachments      = &m_swapChainImageViews[i];
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
                || vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_renderFinished[i]) != VK_SUCCESS
                || vkCreateFence(m_device, &fenceCreateInfo, nullptr, &m_inFlight[i]) != VK_SUCCESS)
            {
                // m_imageAvailable — señala que hay imagen disponible del swapchain
                // m_renderFinished — señala que el render terminó, listo para presentar
                // m_inFlight — fence que bloquea la CPU hasta que la GPU terminó ese frame
                throw std::runtime_error("failed to create sync objects!");
            }
        }
        printf("sync objects OK\n"); fflush(stdout);
    }

    void Renderer::recordCommandBuffer(uint32_t imageIndex)
    {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if(vkBeginCommandBuffer(m_commandBuffers[m_currentFrame], &beginInfo) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to begin command buffer!");
        }

        VkClearValue clearBlack = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
        VkRenderPassBeginInfo renderPassBeginInfo{};
        renderPassBeginInfo.sType               = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassBeginInfo.renderPass          = m_renderPass;
        renderPassBeginInfo.framebuffer         = m_swapChainFramebuffers[imageIndex];
        renderPassBeginInfo.renderArea.extent   = m_swapChainExtent;
        renderPassBeginInfo.renderArea.offset   = {0,0};
        renderPassBeginInfo.clearValueCount     = 1;
        renderPassBeginInfo.pClearValues        = &clearBlack;

        vkCmdBeginRenderPass(m_commandBuffers[m_currentFrame], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(m_commandBuffers[m_currentFrame]);
        vkEndCommandBuffer(m_commandBuffers[m_currentFrame]);
    }
}
