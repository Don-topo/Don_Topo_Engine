#include "DonTopo/Renderer.h"
#include "DonTopo/GameObject.h"
#include <GLFW/glfw3.h>
#include <stdexcept>
#include "DonTopo/Window.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <algorithm>
#include <fstream>
#include "DonTopo/Vertex.h"
#include <glm/gtc/matrix_transform.hpp>
#include "DonTopo/UniformBufferObject.h"
#include <limits>
#include <cmath>

namespace DonTopo {

    Renderer::~Renderer()
    {
        shutdown();
    }

    void Renderer::init(Window& window, const std::vector<Mesh>& meshes)
    {
        // Gizmos::kFramesInFlight se usa para dimensionar buffers por frame en vuelo
        // dentro de Gizmos; debe coincidir siempre con Renderer::MAX_FRAMES. MAX_FRAMES
        // es private, así que este static_assert vive aquí (contexto de miembro) en vez
        // de a nivel de archivo.
        static_assert(Gizmos::kFramesInFlight == MAX_FRAMES,
            "Gizmos::kFramesInFlight debe coincidir con Renderer::MAX_FRAMES");

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

        m_gpu.init(window.getNativeWindow());
        createSwapChain(window);

        createImageViews();
        createDepthResources();
        createOffscreenRenderPass();
        Gizmos::init(m_gpu, m_offscreenRenderPass, m_swapChainFormat);
        createRenderPass();
        createDescriptorSetLayout();
        createPipeline();
        createShadowResources();
        createFramebuffers();
        createComputePipelines();
        initImGui(window.getNativeWindow()); // necesita m_renderPass + m_swapChainImages.size()
        createOffscreenImages();  // necesita ImGui inicializado (llama AddTexture)

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
        // Limpiamos los vértices de gizmos acumulados el frame anterior antes de
        // que Gizmos::drawX(...) empiece a añadir los de este frame. Se hace al
        // principio (en vez de al final) para que también cubra los early-return
        // (p.ej. VK_ERROR_OUT_OF_DATE_KHR) que ocurren más abajo en esta función.
        Gizmos::clear();

        // 1. Espera a que el frame anterior terminó
        vkWaitForFences(m_gpu.device(), 1, &m_inFlight[m_currentFrame], VK_TRUE, UINT64_MAX);

        // 2. Pide la siguiente imagen del swapchain
        uint32_t imageIndex;
        VkResult result;

        result = vkAcquireNextImageKHR(m_gpu.device(), m_swapChain, UINT64_MAX, m_imageAvailable[m_currentFrame], VK_NULL_HANDLE, &imageIndex);
        if(result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            recreateSwapChain(window);
            return;
        }

        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            throw std::runtime_error("failed to acquire next image!");
        }        

        vkResetFences(m_gpu.device(), 1, &m_inFlight[m_currentFrame]);

        // 3. Graba el command buffer
        if(vkResetCommandBuffer(m_commandBuffers[m_currentFrame], 0) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to reset command buffer!");
        }

        updateUniformBuffer(m_currentFrame);

        // ── Construir frame ImGui (antes de grabar el command buffer) ─────────────
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        m_editorUI.draw(m_offscreenDescSet[m_currentFrame], m_sceneRoot, m_viewMatrix);

        ImGui::Render();

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
        if(vkQueueSubmit(m_gpu.graphicsQueue(), 1, &submitInfo, m_inFlight[m_currentFrame]) != VK_SUCCESS)
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
        result = vkQueuePresentKHR(m_gpu.presentQueue(), &presentInfo);
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
        if (m_gpu.device() == VK_NULL_HANDLE) return;
        vkDeviceWaitIdle(m_gpu.device());

        destroyOffscreenImages();
        shutdownImGui();
        vkDestroyRenderPass(m_gpu.device(), m_offscreenRenderPass, nullptr);
        m_offscreenRenderPass = VK_NULL_HANDLE;

        for(auto sem : m_renderFinished){
            vkDestroySemaphore(m_gpu.device(), sem, nullptr);
        }

        for(int i = 0; i < MAX_FRAMES; i++)
        {            
            vkDestroySemaphore(m_gpu.device(), m_imageAvailable[i], nullptr);
            vkDestroyFence(m_gpu.device(), m_inFlight[i], nullptr);
        }
        for(auto framebuffer : m_swapChainFramebuffers)
        {
            vkDestroyFramebuffer(m_gpu.device(), framebuffer, nullptr);
        }
        vkDestroyPipeline(m_gpu.device(), m_pipeline, nullptr);
        vkDestroyPipelineLayout(m_gpu.device(), m_pipelineLayout, nullptr);
        vkDestroyRenderPass(m_gpu.device(), m_renderPass, nullptr);
        for(VkImageView imageView : m_swapChainImageViews)
        {
            vkDestroyImageView(m_gpu.device(), imageView, nullptr);
        }                        
        vkDestroySwapchainKHR(m_gpu.device(), m_swapChain, nullptr);
        vkDestroyDescriptorPool(m_gpu.device(), m_descriptorPool, nullptr);
        for(auto& obj : m_objects)
            destroyRenderObject(obj);
        m_objects.clear();
        for(int i = 0; i < MAX_FRAMES; i++)
        {
            vkDestroyBuffer(m_gpu.device(), m_uniformBuffers[i], nullptr);
            vkFreeMemory(m_gpu.device(), m_uniformBuffersMemory[i], nullptr);
        }
        vkDestroyDescriptorSetLayout(m_gpu.device(), m_descriptorSetLayout, nullptr);
        vkDestroyImageView(m_gpu.device(), m_depthImageView, nullptr);
        vkDestroyImage(m_gpu.device(), m_depthImage, nullptr);
        vkFreeMemory(m_gpu.device(), m_depthImageMemory, nullptr);
        // Shadow Map
        vkDestroySampler(m_gpu.device(), m_shadowSampler, nullptr);
        vkDestroyImageView(m_gpu.device(), m_shadowView, nullptr);
        vkDestroyImage(m_gpu.device(), m_shadowImage, nullptr);
        vkFreeMemory(m_gpu.device(), m_shadowMemory, nullptr);
        vkDestroyFramebuffer(m_gpu.device(), m_shadowFramebuffer, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_skinnedGfxPipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_shadowPipeline, nullptr);
        vkDestroyPipelineLayout(m_gpu.device(), m_shadowPipelineLayout, nullptr);
        vkDestroyRenderPass(m_gpu.device(), m_shadowRenderPass, nullptr);
        for (auto& obj : m_skinnedObjects)
        {
            destroySkinnedRenderObject(obj);
        }
        
        m_skinnedObjects.clear();
        if (m_computeDescPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(m_gpu.device(), m_computeDescPool, nullptr);
        }
        vkDestroyPipeline(m_gpu.device(), m_boneEvalPipeline,      nullptr);
        vkDestroyPipeline(m_gpu.device(), m_boneHierarchyPipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_skinningPipeline,       nullptr);
        vkDestroyPipelineLayout(m_gpu.device(), m_computePipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(m_gpu.device(), m_computeDescLayout, nullptr);
        m_skybox.shutdown(m_gpu);
        Gizmos::shutdown(m_gpu);
        printf("destroy render items OK\n"); fflush(stdout);
        m_gpu.shutdown();
    }

    void Renderer::initSkybox(const std::array<std::string, 6>& facePaths)
    {
        m_skybox.init(m_gpu, m_offscreenRenderPass, m_swapChainFormat, facePaths);
    }

    void Renderer::setCamera(const Camera& camera)
    {
        m_viewMatrix = camera.getViewMatrix();
        m_camera = camera;
    }

    void Renderer::createSwapChain(Window& window)
    {
        VkSurfaceCapabilitiesKHR surfaceCapabilities;
        if(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_gpu.physicalDevice(), m_gpu.surface(), &surfaceCapabilities) != VK_SUCCESS) {
            throw std::runtime_error("failed to get surface capabilities!");
        }

        uint32_t formatCount;
        if(vkGetPhysicalDeviceSurfaceFormatsKHR(m_gpu.physicalDevice(), m_gpu.surface(), &formatCount, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("failed to get surface formats!");
        }

        std::vector<VkSurfaceFormatKHR> surfaceFormats(formatCount);
        if(vkGetPhysicalDeviceSurfaceFormatsKHR(m_gpu.physicalDevice(), m_gpu.surface(), &formatCount, surfaceFormats.data()) != VK_SUCCESS) {
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
        createInfo.surface = m_gpu.surface();
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

        if(vkCreateSwapchainKHR(m_gpu.device(), &createInfo, nullptr, &m_swapChain) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create swap chain!");
        }
        
        if(vkGetSwapchainImagesKHR(m_gpu.device(), m_swapChain, &imageCount, nullptr) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to get swap chain images!");
        }
        m_swapChainImages.resize(imageCount);
        if(vkGetSwapchainImagesKHR(m_gpu.device(), m_swapChain, &imageCount, m_swapChainImages.data()) != VK_SUCCESS)
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

            if(vkCreateImageView(m_gpu.device(), &createInfo, nullptr, &m_swapChainImageViews[i]) != VK_SUCCESS)
            {
                throw std::runtime_error("failed to create image views!");
            }
        }

        printf("Image View OK\n"); fflush(stdout);
    }

    // Pass de escena 3D → offscreen (finalLayout=SHADER_READ para que ImGui lo muestree)
    void Renderer::createOffscreenRenderPass()
    {
        VkAttachmentDescription colorAtt{};
        colorAtt.format         = m_swapChainFormat;
        colorAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
        colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAtt.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentDescription depthAtt{};
        depthAtt.format         = VK_FORMAT_D32_SFLOAT;
        depthAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
        depthAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAtt.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAtt.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthRef{};
        depthRef.attachment = 1;
        depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount    = 1;
        subpass.pColorAttachments       = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;

        // Dependencias: garantizan que ImGui puede leer la textura cuando el pass acaba
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        VkAttachmentDescription attachments[] = { colorAtt, depthAtt };
        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 2;
        rpInfo.pAttachments    = attachments;
        rpInfo.subpassCount    = 1;
        rpInfo.pSubpasses      = &subpass;
        rpInfo.dependencyCount = 2;
        rpInfo.pDependencies   = deps;

        if (vkCreateRenderPass(m_gpu.device(), &rpInfo, nullptr, &m_offscreenRenderPass) != VK_SUCCESS)
            throw std::runtime_error("failed to create offscreen render pass!");

        printf("offscreen render pass OK\n"); fflush(stdout);
    }

    // Pass ImGui → swapchain (solo color, sin depth)
    void Renderer::createRenderPass()
    {
        VkAttachmentDescription colorAtt{};
        colorAtt.format         = m_swapChainFormat;
        colorAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
        colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAtt.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &colorRef;

        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = 0;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments    = &colorAtt;
        rpInfo.subpassCount    = 1;
        rpInfo.pSubpasses      = &subpass;
        rpInfo.dependencyCount = 1;
        rpInfo.pDependencies   = &dep;

        if (vkCreateRenderPass(m_gpu.device(), &rpInfo, nullptr, &m_renderPass) != VK_SUCCESS)
            throw std::runtime_error("failed to create ImGui render pass!");

        printf("ImGui render pass OK\n"); fflush(stdout);
    }

    // Framebuffers del swapchain: solo color, usados por el pass ImGui
    void Renderer::createFramebuffers()
    {
        m_swapChainFramebuffers.resize(m_swapChainImageViews.size());
        for (size_t i = 0; i < m_swapChainImageViews.size(); i++)
        {
            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass      = m_renderPass;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments    = &m_swapChainImageViews[i];
            fbInfo.width           = m_swapChainExtent.width;
            fbInfo.height          = m_swapChainExtent.height;
            fbInfo.layers          = 1;

            if (vkCreateFramebuffer(m_gpu.device(), &fbInfo, nullptr, &m_swapChainFramebuffers[i]) != VK_SUCCESS)
                throw std::runtime_error("failed to create swapchain framebuffer!");
        }
        printf("swapchain framebuffers OK\n"); fflush(stdout);
    }



    void Renderer::createCommandBuffers()
    {
        m_commandBuffers.resize(2);
        VkCommandBufferAllocateInfo commandBufferAllocateInfo{};
        commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandBufferAllocateInfo.commandPool = m_gpu.commandPool();
        commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandBufferAllocateInfo.commandBufferCount = 2;

        if(vkAllocateCommandBuffers(m_gpu.device(), &commandBufferAllocateInfo, m_commandBuffers.data()) != VK_SUCCESS)
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
            if(vkCreateSemaphore(m_gpu.device(), &semaphoreInfo, nullptr, &m_imageAvailable[i]) != VK_SUCCESS                
                || vkCreateFence(m_gpu.device(), &fenceCreateInfo, nullptr, &m_inFlight[i]) != VK_SUCCESS)
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
            if (vkCreateSemaphore(m_gpu.device(), &semInfo, nullptr, &sem) != VK_SUCCESS)
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

        recordComputePass(m_commandBuffers[m_currentFrame]);
        recordShadowPass(m_commandBuffers[m_currentFrame]);

        // ── Pass 1: escena 3D → offscreen ────────────────────────────────────────
        {
            VkClearValue clearValues[2];
            clearValues[0].color        = {0.0f, 0.0f, 0.0f, 1.0f};
            clearValues[1].depthStencil = {1.0f, 0};

            VkRenderPassBeginInfo rpInfo{};
            rpInfo.sType               = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpInfo.renderPass          = m_offscreenRenderPass;
            rpInfo.framebuffer         = m_offscreenFramebuffer[m_currentFrame];
            rpInfo.renderArea.extent   = m_swapChainExtent;
            rpInfo.renderArea.offset   = {0, 0};
            rpInfo.clearValueCount     = 2;
            rpInfo.pClearValues        = clearValues;

            vkCmdBeginRenderPass(m_commandBuffers[m_currentFrame], &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

            VkViewport viewport{};
            viewport.width    = (float)m_swapChainExtent.width;
            viewport.height   = (float)m_swapChainExtent.height;
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(m_commandBuffers[m_currentFrame], 0, 1, &viewport);

            VkRect2D scissor{};
            scissor.extent = m_swapChainExtent;
            vkCmdSetScissor(m_commandBuffers[m_currentFrame], 0, 1, &scissor);

            vkCmdBindPipeline(m_commandBuffers[m_currentFrame], VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
            for (auto& obj : m_objects)
            {
                if (obj.vertexBuffer == VK_NULL_HANDLE) continue; // borrado desde el editor
                vkCmdBindDescriptorSets(m_commandBuffers[m_currentFrame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_pipelineLayout, 0, 1, &obj.descriptorSets[m_currentFrame], 0, nullptr);
                PushData push;
                push.transform = obj.transform;
                push.metallic  = obj.metallic;
                push.roughness = obj.roughness;
                vkCmdPushConstants(m_commandBuffers[m_currentFrame], m_pipelineLayout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(PushData), &push);
                VkBuffer vbs[]      = { obj.vertexBuffer };
                VkDeviceSize offs[] = { 0 };
                vkCmdBindVertexBuffers(m_commandBuffers[m_currentFrame], 0, 1, vbs, offs);
                vkCmdBindIndexBuffer(m_commandBuffers[m_currentFrame], obj.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(m_commandBuffers[m_currentFrame], obj.indexCount, 1, 0, 0, 0);
            }

            if (!m_skinnedObjects.empty())
            {
                vkCmdBindPipeline(m_commandBuffers[m_currentFrame],
                    VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinnedGfxPipeline);

                for (auto& sobj : m_skinnedObjects)
                {
                    if (sobj.outputVertexBuffer == VK_NULL_HANDLE) continue; // borrado desde el editor
                    VkBuffer     vbs[]  = { sobj.outputVertexBuffer };
                    VkDeviceSize offs[] = { 0 };
                    vkCmdBindVertexBuffers(m_commandBuffers[m_currentFrame], 0, 1, vbs, offs);
                    vkCmdBindIndexBuffer(m_commandBuffers[m_currentFrame],
                        sobj.indexBuffer, 0, VK_INDEX_TYPE_UINT32);

                    for (auto& sm : sobj.subMeshes)
                    {
                        SkinnedMatGfx& mgfx = sobj.matGfx[sm.materialIndex];
                        PushData push;
                        push.transform = sobj.transform;
                        push.metallic  = mgfx.metallic;
                        push.roughness = mgfx.roughness;
                        vkCmdBindDescriptorSets(m_commandBuffers[m_currentFrame],
                            VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                            0, 1, &mgfx.descSets[m_currentFrame], 0, nullptr);
                        vkCmdPushConstants(m_commandBuffers[m_currentFrame], m_pipelineLayout,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(PushData), &push);
                        vkCmdDrawIndexed(m_commandBuffers[m_currentFrame],
                            sm.indexCount, 1, sm.indexStart, 0, 0);
                    }
                }
            }

            // Proyección compartida por skybox y gizmos (mismo pass, misma cámara).
            glm::mat4 proj = glm::perspective(
                glm::radians(45.0f),
                (float)m_swapChainExtent.width / (float)m_swapChainExtent.height,
                m_cameraDistance * 0.001f, m_cameraDistance * 3.0f);
            proj[1][1] *= -1.0f;

            // Skybox — fullscreen quad, depth LEQUAL sin escritura (al final del pass)
            if (m_skybox.isInitialized()) {
                glm::mat4 rotView    = glm::mat4(glm::mat3(m_viewMatrix)); // sin traslación
                glm::mat4 invViewProj = glm::inverse(proj * rotView);
                m_skybox.draw(m_commandBuffers[m_currentFrame], invViewProj);
            }

            // Gizmos — mismo pass, tras el skybox, respetando el depth test de la escena.
            {
                Gizmos::draw(m_commandBuffers[m_currentFrame], proj * m_viewMatrix, m_currentFrame);
            }

            vkCmdEndRenderPass(m_commandBuffers[m_currentFrame]);
        }

        // ── Pass 2: ImGui → swapchain ─────────────────────────────────────────────
        {
            VkClearValue clearColor{};
            clearColor.color = {0.12f, 0.12f, 0.12f, 1.0f};

            VkRenderPassBeginInfo rpInfo{};
            rpInfo.sType               = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpInfo.renderPass          = m_renderPass;
            rpInfo.framebuffer         = m_swapChainFramebuffers[imageIndex];
            rpInfo.renderArea.extent   = m_swapChainExtent;
            rpInfo.renderArea.offset   = {0, 0};
            rpInfo.clearValueCount     = 1;
            rpInfo.pClearValues        = &clearColor;

            vkCmdBeginRenderPass(m_commandBuffers[m_currentFrame], &rpInfo, VK_SUBPASS_CONTENTS_INLINE);
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), m_commandBuffers[m_currentFrame]);
            vkCmdEndRenderPass(m_commandBuffers[m_currentFrame]);
        }

        vkEndCommandBuffer(m_commandBuffers[m_currentFrame]);
    }

    void Renderer::createPipeline()
    {
        auto vertCode = loadShaderFile("shaders/triangle.vert.spv");
        auto fragCode = loadShaderFile("shaders/pbr.frag.spv");

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
        pushRange.stageFlags    = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset        = 0;
        pushRange.size          = sizeof(PushData);   // 80 bytes

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType                    = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount           = 1;
        layoutInfo.pSetLayouts              = &m_descriptorSetLayout;
        layoutInfo.pushConstantRangeCount   = 1;
        layoutInfo.pPushConstantRanges      = &pushRange;
        if(vkCreatePipelineLayout(m_gpu.device(), &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
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
        pipelineInfo.renderPass             = m_offscreenRenderPass;
        pipelineInfo.subpass                = 0;
        pipelineInfo.pDepthStencilState     = &depthStencil;

        if(vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create graphics pipeline!");
        }

        // los módulos se destruyen al final de esta función — solo los necesita el pipeline
        vkDestroyShaderModule(m_gpu.device(), vertModule, nullptr);
        vkDestroyShaderModule(m_gpu.device(), fragModule, nullptr);
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
        if(vkCreateShaderModule(m_gpu.device(), &createInfo, nullptr, &shaderModule) != VK_SUCCESS)
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

        vkDeviceWaitIdle(m_gpu.device());

        // Teardown offscreen primero (sus FBs usan m_depthImageView)
        destroyOffscreenImages();

        // Teardown swapchain
        for (auto semaphore : m_renderFinished)
            vkDestroySemaphore(m_gpu.device(), semaphore, nullptr);
        m_renderFinished.clear();

        for (auto fb : m_swapChainFramebuffers)
            vkDestroyFramebuffer(m_gpu.device(), fb, nullptr);
        m_swapChainFramebuffers.clear();

        for (auto iv : m_swapChainImageViews)
            vkDestroyImageView(m_gpu.device(), iv, nullptr);
        m_swapChainImageViews.clear();

        vkDestroyImageView(m_gpu.device(), m_depthImageView, nullptr);
        vkDestroyImage(m_gpu.device(), m_depthImage, nullptr);
        vkFreeMemory(m_gpu.device(), m_depthImageMemory, nullptr);
        vkDestroySwapchainKHR(m_gpu.device(), m_swapChain, nullptr);
        m_swapChain = VK_NULL_HANDLE;

        // Recreate
        createSwapChain(window);
        createImageViews();
        createDepthResources();
        createFramebuffers();
        createOffscreenImages(); // recrea con el nuevo tamaño

        // Solo recrear los semáforos que dependen del image count
        m_renderFinished.resize(m_swapChainImages.size());
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (auto& sem : m_renderFinished)
            if (vkCreateSemaphore(m_gpu.device(), &semInfo, nullptr, &sem) != VK_SUCCESS)
                throw std::runtime_error("failed to create renderFinished semaphore!");
    }

    void Renderer::createVertexBuffer(const std::vector<Vertex>& vertices, VkBuffer& buf, VkDeviceMemory& mem)
    {
        VkDeviceSize size = sizeof(vertices[0]) * vertices.size();

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        m_res.createBuffer(size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stagingBuffer, stagingMemory);

        void* data;
        vkMapMemory(m_gpu.device(), stagingMemory, 0, size, 0, &data);
        memcpy(data, vertices.data(), (size_t)size);
        vkUnmapMemory(m_gpu.device(), stagingMemory);

        m_res.createBuffer(size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            buf, mem);

        m_res.copyBuffer(stagingBuffer, buf, size);

        vkDestroyBuffer(m_gpu.device(), stagingBuffer, nullptr);
        vkFreeMemory(m_gpu.device(), stagingMemory, nullptr);
    }

    void Renderer::createIndexBuffer(const std::vector<uint32_t>& idx, VkBuffer& buf, VkDeviceMemory& mem)
    {
        VkDeviceSize size = sizeof(idx[0]) * idx.size();

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        m_res.createBuffer(size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stagingBuffer, stagingMemory);

        void* data;
        vkMapMemory(m_gpu.device(), stagingMemory, 0, size, 0, &data);
        memcpy(data, idx.data(), (size_t)size);
        vkUnmapMemory(m_gpu.device(), stagingMemory);

        m_res.createBuffer(size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            buf, mem);

        m_res.copyBuffer(stagingBuffer, buf, size);

        vkDestroyBuffer(m_gpu.device(), stagingBuffer, nullptr);
        vkFreeMemory(m_gpu.device(), stagingMemory, nullptr);

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

        VkDescriptorSetLayoutBinding ormBinding{};
        ormBinding.binding         = 4;
        ormBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ormBinding.descriptorCount = 1;
        ormBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding bindings[] = { uboBinding, samplerBinding, samplerNormal, shadowBinding, ormBinding };

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType            = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount     = 5;
        layoutInfo.pBindings        = bindings;

        if(vkCreateDescriptorSetLayout(m_gpu.device(), &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS)
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
            vkCreateBuffer(m_gpu.device(), &bufferInfo, nullptr, &m_uniformBuffers[i]);

            VkMemoryRequirements memoryRequirements;
            vkGetBufferMemoryRequirements(m_gpu.device(), m_uniformBuffers[i], &memoryRequirements);

            VkMemoryAllocateInfo allocInfo{};
            allocInfo.sType             = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize    = memoryRequirements.size;
            allocInfo.memoryTypeIndex   = m_gpu.findMemoryType(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkAllocateMemory(m_gpu.device(), &allocInfo, NULL, &m_uniformBuffersMemory[i]);
            vkBindBufferMemory(m_gpu.device(), m_uniformBuffers[i], m_uniformBuffersMemory[i], 0);

            // Mapeo persistente — nunca llamamos unmap
            vkMapMemory(m_gpu.device(), m_uniformBuffersMemory[i], 0, size, 0, &m_uniformBuffersMapped[i]);

        }
    }

    void Renderer::createDescriptorPool()
    {
        uint32_t n = (uint32_t)((m_objects.size() + 128) * MAX_FRAMES);
        VkDescriptorPoolSize poolSizes[2]{};
        poolSizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = n;
        poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[1].descriptorCount = n * 4;   // diffuse + normal map + shadow + orm

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes    = poolSizes;
        poolInfo.maxSets       = n;

        if(vkCreateDescriptorPool(m_gpu.device(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
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

            if(vkAllocateDescriptorSets(m_gpu.device(), &allocInfo, obj.descriptorSets) != VK_SUCCESS)
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

                VkDescriptorImageInfo ormInfo{};
                ormInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                ormInfo.imageView   = obj.ormView;
                ormInfo.sampler     = obj.ormSampler;

                VkWriteDescriptorSet writes[5]{};
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

                writes[4].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[4].dstSet          = obj.descriptorSets[i];
                writes[4].dstBinding      = 4;
                writes[4].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[4].descriptorCount = 1;
                writes[4].pImageInfo      = &ormInfo;

                vkUpdateDescriptorSets(m_gpu.device(), 5, writes, 0, nullptr);
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

        if(vkCreateImage(m_gpu.device(), &imageInfo, nullptr, &m_depthImage) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create depth image!");
        }

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(m_gpu.device(), m_depthImage, &memReq);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType             = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize    = memReq.size;
        allocInfo.memoryTypeIndex   = m_gpu.findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if(vkAllocateMemory(m_gpu.device(), &allocInfo, nullptr, &m_depthImageMemory) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to allocate depth image memory!");
        }

        vkBindImageMemory(m_gpu.device(), m_depthImage, m_depthImageMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_depthImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = depthFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if(vkCreateImageView(m_gpu.device(), &viewInfo, nullptr, &m_depthImageView) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create depth image view!");
        }
    }

    void Renderer::buildRenderObject(const Mesh& mesh, RenderObject& obj)
    {
        obj.name       = mesh.name;
        obj.indexCount = (uint32_t)mesh.indices.size();
        createVertexBuffer(mesh.vertices,                               obj.vertexBuffer, obj.vertexMemory);
        createIndexBuffer(mesh.indices,                                 obj.indexBuffer,  obj.indexMemory);
        m_res.createTextureImage(mesh.material.texturePath, mesh.material.embeddedTexture,         obj.textureImage, obj.textureMem);
        m_res.createTextureImageView(obj.textureImage,                           obj.textureView);
        m_res.createTextureSampler(obj.sampler);
        m_res.createNormalMapImage(mesh.material.normalMapPath, mesh.material.embeddedNormalMap,   obj.normalImage,  obj.normalMem);
        m_res.createTextureImageView(obj.normalImage,                            obj.normalView, VK_FORMAT_R8G8B8A8_UNORM);
        m_res.createTextureSampler(obj.normalSampler);

        if (!mesh.material.metallicRoughnessPath.empty() || !mesh.material.embeddedMetallicRoughness.empty())
        {
            m_res.createNormalMapImage(mesh.material.metallicRoughnessPath, mesh.material.embeddedMetallicRoughness,
                                 obj.ormImage, obj.ormMem);
            obj.metallic  = 1.0f;
            obj.roughness = 1.0f;
        }
        else
        {
            constexpr uint8_t white[4] = {255, 255, 255, 255};
            m_res.createSolidColorImage(white, obj.ormImage, obj.ormMem);
            obj.metallic  = mesh.material.metallic;
            obj.roughness = mesh.material.roughness;
        }
        m_res.createTextureImageView(obj.ormImage, obj.ormView, VK_FORMAT_R8G8B8A8_UNORM);
        m_res.createTextureSampler(obj.ormSampler);
    }

    void Renderer::destroyRenderObject(RenderObject& obj)
    {
        vkDestroySampler(m_gpu.device(),   obj.ormSampler,    nullptr);
        vkDestroyImageView(m_gpu.device(), obj.ormView,       nullptr);
        vkDestroyImage(m_gpu.device(),     obj.ormImage,      nullptr);
        vkFreeMemory(m_gpu.device(),       obj.ormMem,        nullptr);
        vkDestroySampler(m_gpu.device(),   obj.normalSampler, nullptr);
        vkDestroyImageView(m_gpu.device(), obj.normalView,    nullptr);
        vkDestroyImage(m_gpu.device(),     obj.normalImage,   nullptr);
        vkFreeMemory(m_gpu.device(),       obj.normalMem,     nullptr);
        vkDestroySampler(m_gpu.device(),   obj.sampler,       nullptr);
        vkDestroyImageView(m_gpu.device(), obj.textureView,   nullptr);
        vkDestroyImage(m_gpu.device(),     obj.textureImage,  nullptr);
        vkFreeMemory(m_gpu.device(),       obj.textureMem,    nullptr);
        vkDestroyBuffer(m_gpu.device(),    obj.indexBuffer,   nullptr);
        vkFreeMemory(m_gpu.device(),       obj.indexMemory,   nullptr);
        vkDestroyBuffer(m_gpu.device(),    obj.vertexBuffer,  nullptr);
        vkFreeMemory(m_gpu.device(),       obj.vertexMemory,  nullptr);
    }

    void Renderer::createShadowResources()
    {
        // 1. Imagen depth para shadow map
        m_res.createImage(SHADOW_SIZE, SHADOW_SIZE, VK_FORMAT_D32_SFLOAT, VK_IMAGE_TILING_OPTIMAL, 
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
        if(vkCreateImageView(m_gpu.device(), &viewInfo, nullptr, &m_shadowView) != VK_SUCCESS)
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
        if(vkCreateSampler(m_gpu.device(), &samplerInfo, nullptr, &m_shadowSampler) != VK_SUCCESS)
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
        if(vkCreateRenderPass(m_gpu.device(), &renderPassInfo, nullptr, &m_shadowRenderPass) != VK_SUCCESS)
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
        if (vkCreateFramebuffer(m_gpu.device(), &fbInfo, nullptr, &m_shadowFramebuffer) != VK_SUCCESS)
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
        if (vkCreatePipelineLayout(m_gpu.device(), &layoutInfo, nullptr, &m_shadowPipelineLayout) != VK_SUCCESS)
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
        if (vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_shadowPipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create shadow pipeline!");
        }            

        vkDestroyShaderModule(m_gpu.device(), vertModule, nullptr);
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
            if (obj.vertexBuffer == VK_NULL_HANDLE) continue; // borrado desde el editor
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

    void Renderer::createComputePipelines()
    {
        // --- Descriptor set layout: 8 storage buffers ---
        VkDescriptorSetLayoutBinding bindings[8]{};
        for (uint32_t i = 0; i < 8; i++)
        {
            bindings[i].binding         = i;
            bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        }

        VkDescriptorSetLayoutCreateInfo dslInfo{};
        dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslInfo.bindingCount = 8;
        dslInfo.pBindings    = bindings;
        if (vkCreateDescriptorSetLayout(m_gpu.device(), &dslInfo, nullptr, &m_computeDescLayout) != VK_SUCCESS)
            throw std::runtime_error("failed to create compute descriptor set layout!");

        // --- Pipeline layout (1 set + push constant) ---
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.offset     = 0;
        pcr.size       = sizeof(ComputePush);

        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount         = 1;
        plInfo.pSetLayouts            = &m_computeDescLayout;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges    = &pcr;
        if (vkCreatePipelineLayout(m_gpu.device(), &plInfo, nullptr, &m_computePipelineLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create compute pipeline layout!");
        }

        // --- Descriptor pool: 8 SSBOs * 16 objetos max ---
        VkDescriptorPoolSize ps{};
        ps.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ps.descriptorCount = 8 * 16;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &ps;
        poolInfo.maxSets       = 16;
        if (vkCreateDescriptorPool(m_gpu.device(), &poolInfo, nullptr, &m_computeDescPool) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create compute descriptor pool!");
        }        

        // --- Crear los tres pipelines ---
        auto makePipeline = [&](const std::string& spv, VkPipeline& pipeline)
        {
            auto code   = loadShaderFile(spv);
            auto module = createShaderModule(code);

            VkComputePipelineCreateInfo info{};
            info.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            info.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            info.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
            info.stage.module = module;
            info.stage.pName  = "main";
            info.layout       = m_computePipelineLayout;

            if (vkCreateComputePipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
                throw std::runtime_error("failed to create compute pipeline: " + spv);

            vkDestroyShaderModule(m_gpu.device(), module, nullptr);
        };

        makePipeline("shaders/bone_eval.comp.spv",      m_boneEvalPipeline);
        makePipeline("shaders/bone_hierarchy.comp.spv", m_boneHierarchyPipeline);
        makePipeline("shaders/skinning.comp.spv",       m_skinningPipeline);

         // --- Skinned graphics pipeline (stride=80, mismos shaders) ---
        {
            auto vertCode = loadShaderFile("shaders/triangle.vert.spv");
            auto fragCode = loadShaderFile("shaders/pbr.frag.spv");
            auto vertMod  = createShaderModule(vertCode);
            auto fragMod  = createShaderModule(fragCode);

            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vertMod; stages[0].pName = "main";
            stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = fragMod; stages[1].pName = "main";

            VkVertexInputBindingDescription binding{};
            binding.binding   = 0;
            binding.stride    = 5 * (uint32_t)sizeof(glm::vec4);  // 80 bytes
            binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

            // OutputVertex: pos@0, color@16, uv@32, normal@48, tangent@64
            VkVertexInputAttributeDescription attrs[5]{};
            attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,  0 };
            attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 16 };
            attrs[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,    32 };
            attrs[3] = { 3, 0, VK_FORMAT_R32G32B32_SFLOAT, 48 };
            attrs[4] = { 4, 0, VK_FORMAT_R32G32B32_SFLOAT, 64 };

            VkPipelineVertexInputStateCreateInfo vi{};
            vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            vi.vertexBindingDescriptionCount   = 1;  vi.pVertexBindingDescriptions  = &binding;
            vi.vertexAttributeDescriptionCount = 5;  vi.pVertexAttributeDescriptions = attrs;

            VkPipelineInputAssemblyStateCreateInfo ia{};
            ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

            VkPipelineViewportStateCreateInfo vp{};
            vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            vp.viewportCount = 1; vp.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rs{};
            rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode    = VK_CULL_MODE_BACK_BIT;
            rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.lineWidth   = 1.0f;

            VkPipelineMultisampleStateCreateInfo ms{};
            ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkPipelineDepthStencilStateCreateInfo ds{};
            ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            ds.depthTestEnable  = VK_TRUE;
            ds.depthWriteEnable = VK_TRUE;
            ds.depthCompareOp   = VK_COMPARE_OP_LESS;

            VkPipelineColorBlendAttachmentState blend{};
            blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

            VkPipelineColorBlendStateCreateInfo cb{};
            cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            cb.attachmentCount = 1; cb.pAttachments = &blend;

            VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo dyn{};
            dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;

            VkGraphicsPipelineCreateInfo pci{};
            pci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pci.stageCount          = 2;  pci.pStages             = stages;
            pci.pVertexInputState   = &vi; pci.pInputAssemblyState = &ia;
            pci.pViewportState      = &vp; pci.pRasterizationState = &rs;
            pci.pMultisampleState   = &ms; pci.pDepthStencilState  = &ds;
            pci.pColorBlendState    = &cb; pci.pDynamicState       = &dyn;
            pci.layout              = m_pipelineLayout;
            pci.renderPass          = m_offscreenRenderPass;
            pci.subpass             = 0;

            if (vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &pci, nullptr, &m_skinnedGfxPipeline) != VK_SUCCESS)
                throw std::runtime_error("failed to create skinned graphics pipeline!");

            vkDestroyShaderModule(m_gpu.device(), vertMod, nullptr);
            vkDestroyShaderModule(m_gpu.device(), fragMod, nullptr);
        }
    }

    void Renderer::destroySkinnedRenderObject(SkinnedRenderObject& obj)
    {
        auto destroy = [&](VkBuffer& b, VkDeviceMemory& m)
        {
            if (b != VK_NULL_HANDLE) { vkDestroyBuffer(m_gpu.device(), b, nullptr); b = VK_NULL_HANDLE; }
            if (m != VK_NULL_HANDLE) { vkFreeMemory(m_gpu.device(), m, nullptr);    m = VK_NULL_HANDLE; }
        };
        destroy(obj.keyframePosBuffer,    obj.keyframePosMemory);
        destroy(obj.keyframeRotBuffer,    obj.keyframeRotMemory);
        destroy(obj.keyframeScaleBuffer,  obj.keyframeScaleMemory);
        destroy(obj.boneInfoBuffer,       obj.boneInfoMemory);
        destroy(obj.inputVertexBuffer,    obj.inputVertexMemory);
        destroy(obj.localTransformBuffer, obj.localTransformMemory);
        destroy(obj.finalBoneBuffer,      obj.finalBoneMemory);
        destroy(obj.outputVertexBuffer,   obj.outputVertexMemory);
        destroy(obj.indexBuffer,          obj.indexMemory);

        for (auto& mgfx : obj.matGfx)
        {
            if (mgfx.ormSampler    != VK_NULL_HANDLE) { vkDestroySampler  (m_gpu.device(), mgfx.ormSampler,    nullptr); }
            if (mgfx.ormView       != VK_NULL_HANDLE) { vkDestroyImageView(m_gpu.device(), mgfx.ormView,       nullptr); }
            if (mgfx.ormImage      != VK_NULL_HANDLE) { vkDestroyImage    (m_gpu.device(), mgfx.ormImage,      nullptr); }
            if (mgfx.ormMem        != VK_NULL_HANDLE) { vkFreeMemory      (m_gpu.device(), mgfx.ormMem,        nullptr); }
            if (mgfx.normalSampler != VK_NULL_HANDLE) { vkDestroySampler  (m_gpu.device(), mgfx.normalSampler, nullptr); }
            if (mgfx.normalView    != VK_NULL_HANDLE) { vkDestroyImageView(m_gpu.device(), mgfx.normalView,    nullptr); }
            if (mgfx.normalImage   != VK_NULL_HANDLE) { vkDestroyImage    (m_gpu.device(), mgfx.normalImage,   nullptr); }
            if (mgfx.normalMem     != VK_NULL_HANDLE) { vkFreeMemory      (m_gpu.device(), mgfx.normalMem,     nullptr); }
            if (mgfx.sampler       != VK_NULL_HANDLE) { vkDestroySampler  (m_gpu.device(), mgfx.sampler,       nullptr); }
            if (mgfx.textureView   != VK_NULL_HANDLE) { vkDestroyImageView(m_gpu.device(), mgfx.textureView,   nullptr); }
            if (mgfx.textureImage  != VK_NULL_HANDLE) { vkDestroyImage    (m_gpu.device(), mgfx.textureImage,  nullptr); }
            if (mgfx.textureMem    != VK_NULL_HANDLE) { vkFreeMemory      (m_gpu.device(), mgfx.textureMem,    nullptr); }
        }
        obj.matGfx.clear();
        obj.subMeshes.clear();
    }

    int Renderer::addSkinnedMesh(const SkinnedMesh& mesh)
    {
        m_skinnedObjects.emplace_back();
        SkinnedRenderObject& obj = m_skinnedObjects.back();

        const Skeleton&      skel = mesh.skeleton;
        const AnimationClip& clip = mesh.animationClip;
        int boneCount   = (int)skel.names.size();
        int vertexCount = (int)mesh.skinnedVertices.size();

        obj.name           = mesh.name;
        obj.boneCount      = (uint32_t)boneCount;
        obj.vertexCount    = (uint32_t)vertexCount;
        obj.indexCount     = (uint32_t)mesh.indices.size();
        obj.duration       = clip.duration;
        obj.ticksPerSecond = (clip.ticksPerSecond > 0.0f) ? clip.ticksPerSecond : 24.0f;

        // --- Flatten keyframes a GPU format ---
        std::vector<GpuPosKey>   allPos;
        std::vector<GpuRotKey>   allRot;
        std::vector<GpuPosKey>   allScale;
        std::vector<GpuBoneInfo> boneInfos(boneCount);

        for (int b = 0; b < boneCount; b++)
        {
            GpuBoneInfo& bi    = boneInfos[b];
            bi.parentIndex     = skel.parentIndex[b];
            bi.inverseBindPose = skel.inverseBindPose[b];
            bi.pad             = 0;

            const BoneChannel* ch = nullptr;
            for (auto& c : clip.channels)
                if (c.boneIndex == b) { ch = &c; break; }

            bi.posOffset = (int)allPos.size();
            bi.posCount  = ch ? (int)ch->posKeys.size() : 0;
            for (int k = 0; k < bi.posCount; k++)
            {
                GpuPosKey pk{};
                pk.timePad = { ch->posKeys[k].time, 0, 0, 0 };
                pk.value   = { ch->posKeys[k].value.x, ch->posKeys[k].value.y, ch->posKeys[k].value.z, 0 };
                allPos.push_back(pk);
            }

            bi.rotOffset = (int)allRot.size();
            bi.rotCount  = ch ? (int)ch->rotKeys.size() : 0;
            for (int k = 0; k < bi.rotCount; k++)
            {
                const glm::quat& q = ch->rotKeys[k].value;
                GpuRotKey rk{};
                rk.timePad = { ch->rotKeys[k].time, 0, 0, 0 };
                rk.value   = { q.x, q.y, q.z, q.w };
                allRot.push_back(rk);
            }

            bi.scaleOffset = (int)allScale.size();
            bi.scaleCount  = ch ? (int)ch->scaleKeys.size() : 0;
            for (int k = 0; k < bi.scaleCount; k++)
            {
                GpuPosKey sk{};
                sk.timePad = { ch->scaleKeys[k].time, 0, 0, 0 };
                sk.value   = { ch->scaleKeys[k].value.x, ch->scaleKeys[k].value.y, ch->scaleKeys[k].value.z, 0 };
                allScale.push_back(sk);
            }
        }

        // Vulkan no acepta buffers de tamaño 0
        if (allPos.empty())   allPos.push_back({});
        if (allRot.empty())   allRot.push_back({});
        if (allScale.empty()) allScale.push_back({});

        // --- Upload SSBOs estáticos ---
        m_res.uploadBuffer(allPos.data(),   allPos.size()   * sizeof(GpuPosKey),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, obj.keyframePosBuffer,   obj.keyframePosMemory);
        m_res.uploadBuffer(allRot.data(),   allRot.size()   * sizeof(GpuRotKey),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, obj.keyframeRotBuffer,   obj.keyframeRotMemory);
        m_res.uploadBuffer(allScale.data(), allScale.size() * sizeof(GpuPosKey),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, obj.keyframeScaleBuffer, obj.keyframeScaleMemory);
        m_res.uploadBuffer(boneInfos.data(), boneInfos.size() * sizeof(GpuBoneInfo),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, obj.boneInfoBuffer,      obj.boneInfoMemory);
        m_res.uploadBuffer(mesh.skinnedVertices.data(), mesh.skinnedVertices.size() * sizeof(SkinnedVertex),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, obj.inputVertexBuffer,   obj.inputVertexMemory);

        // --- Index buffer ---
        createIndexBuffer(mesh.indices, obj.indexBuffer, obj.indexMemory);

        // --- SSBOs dinámicos (device local, sin datos iniciales) ---
        m_res.createBuffer((uint32_t)boneCount * sizeof(glm::mat4),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            obj.localTransformBuffer, obj.localTransformMemory);

        m_res.createBuffer((uint32_t)boneCount * sizeof(glm::mat4),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            obj.finalBoneBuffer, obj.finalBoneMemory);

        // --- Output vertex buffer: SSBO + VB, stride 80 bytes (5×vec4) ---
        constexpr VkDeviceSize OUT_VERT = 5 * sizeof(glm::vec4);
        m_res.createBuffer((uint32_t)vertexCount * OUT_VERT,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            obj.outputVertexBuffer, obj.outputVertexMemory);

        // --- Compute descriptor set ---
        VkDescriptorSetAllocateInfo dsAlloc{};
        dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsAlloc.descriptorPool     = m_computeDescPool;
        dsAlloc.descriptorSetCount = 1;
        dsAlloc.pSetLayouts        = &m_computeDescLayout;
        if (vkAllocateDescriptorSets(m_gpu.device(), &dsAlloc, &obj.computeDescSet) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate compute descriptor set!");

        VkDescriptorBufferInfo bufInfos[8]{};
        bufInfos[0] = { obj.keyframePosBuffer,    0, VK_WHOLE_SIZE };
        bufInfos[1] = { obj.keyframeRotBuffer,    0, VK_WHOLE_SIZE };
        bufInfos[2] = { obj.keyframeScaleBuffer,  0, VK_WHOLE_SIZE };
        bufInfos[3] = { obj.boneInfoBuffer,       0, VK_WHOLE_SIZE };
        bufInfos[4] = { obj.localTransformBuffer, 0, VK_WHOLE_SIZE };
        bufInfos[5] = { obj.finalBoneBuffer,      0, VK_WHOLE_SIZE };
        bufInfos[6] = { obj.inputVertexBuffer,    0, VK_WHOLE_SIZE };
        bufInfos[7] = { obj.outputVertexBuffer,   0, VK_WHOLE_SIZE };

        VkWriteDescriptorSet writes[8]{};
        for (int i = 0; i < 8; i++)
        {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = obj.computeDescSet;
            writes[i].dstBinding      = (uint32_t)i;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].descriptorCount = 1;
            writes[i].pBufferInfo     = &bufInfos[i];
        }
        vkUpdateDescriptorSets(m_gpu.device(), 8, writes, 0, nullptr);

        // --- Texturas y descriptor sets por material ---
        constexpr uint8_t white[4] = {255, 255, 255, 255};
        obj.matGfx.resize(mesh.materials.size());

        for (size_t mi = 0; mi < mesh.materials.size(); mi++)
        {
            const Material& smat = mesh.materials[mi];
            SkinnedMatGfx& mgfx = obj.matGfx[mi];

            // Diffuse
            m_res.createTextureImage(smat.texturePath, smat.embeddedTexture, mgfx.textureImage, mgfx.textureMem);
            m_res.createTextureImageView(mgfx.textureImage, mgfx.textureView);
            m_res.createTextureSampler(mgfx.sampler);

            // Normal map
            m_res.createNormalMapImage(smat.normalMapPath, smat.embeddedNormalMap, mgfx.normalImage, mgfx.normalMem);
            m_res.createTextureImageView(mgfx.normalImage, mgfx.normalView, VK_FORMAT_R8G8B8A8_UNORM);
            m_res.createTextureSampler(mgfx.normalSampler);

            // ORM
            if (!smat.metallicRoughnessPath.empty() || !smat.embeddedMetallicRoughness.empty())
            {
                m_res.createNormalMapImage(smat.metallicRoughnessPath, smat.embeddedMetallicRoughness,
                                     mgfx.ormImage, mgfx.ormMem);
                mgfx.metallic  = 1.0f;
                mgfx.roughness = 1.0f;
            }
            else
            {
                m_res.createSolidColorImage(white, mgfx.ormImage, mgfx.ormMem);
                mgfx.metallic  = smat.metallic;
                mgfx.roughness = smat.roughness;
            }
            m_res.createTextureImageView(mgfx.ormImage, mgfx.ormView, VK_FORMAT_R8G8B8A8_UNORM);
            m_res.createTextureSampler(mgfx.ormSampler);

            // Descriptor sets
            VkDescriptorSetLayout layouts[MAX_FRAMES] = { m_descriptorSetLayout, m_descriptorSetLayout };
            VkDescriptorSetAllocateInfo gfxAlloc{};
            gfxAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            gfxAlloc.descriptorPool     = m_descriptorPool;
            gfxAlloc.descriptorSetCount = MAX_FRAMES;
            gfxAlloc.pSetLayouts        = layouts;
            if (vkAllocateDescriptorSets(m_gpu.device(), &gfxAlloc, mgfx.descSets) != VK_SUCCESS)
                throw std::runtime_error("failed to allocate skinned graphics descriptor sets!");

            for (int fi = 0; fi < MAX_FRAMES; fi++)
            {
                VkDescriptorBufferInfo uboInfo{};
                uboInfo.buffer = m_uniformBuffers[fi];
                uboInfo.offset = 0;
                uboInfo.range  = sizeof(UniformBufferObject);

                VkDescriptorImageInfo texInfo{};
                texInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                texInfo.imageView   = mgfx.textureView;
                texInfo.sampler     = mgfx.sampler;

                VkDescriptorImageInfo nrmInfo{};
                nrmInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                nrmInfo.imageView   = mgfx.normalView;
                nrmInfo.sampler     = mgfx.normalSampler;

                VkDescriptorImageInfo shdInfo{};
                shdInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                shdInfo.imageView   = m_shadowView;
                shdInfo.sampler     = m_shadowSampler;

                VkDescriptorImageInfo ormInfo{};
                ormInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                ormInfo.imageView   = mgfx.ormView;
                ormInfo.sampler     = mgfx.ormSampler;

                VkWriteDescriptorSet gw[5]{};
                gw[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                gw[0].dstSet = mgfx.descSets[fi]; gw[0].dstBinding = 0;
                gw[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                gw[0].descriptorCount = 1; gw[0].pBufferInfo = &uboInfo;

                gw[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                gw[1].dstSet = mgfx.descSets[fi]; gw[1].dstBinding = 1;
                gw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                gw[1].descriptorCount = 1; gw[1].pImageInfo = &texInfo;

                gw[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                gw[2].dstSet = mgfx.descSets[fi]; gw[2].dstBinding = 2;
                gw[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                gw[2].descriptorCount = 1; gw[2].pImageInfo = &nrmInfo;

                gw[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                gw[3].dstSet = mgfx.descSets[fi]; gw[3].dstBinding = 3;
                gw[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                gw[3].descriptorCount = 1; gw[3].pImageInfo = &shdInfo;

                gw[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                gw[4].dstSet = mgfx.descSets[fi]; gw[4].dstBinding = 4;
                gw[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                gw[4].descriptorCount = 1; gw[4].pImageInfo = &ormInfo;

                vkUpdateDescriptorSets(m_gpu.device(), 5, gw, 0, nullptr);
            }
        }

        // --- SubMesh draw list ---
        obj.subMeshes.resize(mesh.subMeshRanges.size());
        for (size_t si = 0; si < mesh.subMeshRanges.size(); si++)
        {
            obj.subMeshes[si].indexStart   = mesh.subMeshRanges[si].indexStart;
            obj.subMeshes[si].indexCount   = mesh.subMeshRanges[si].indexCount;
            obj.subMeshes[si].materialIndex = mesh.subMeshRanges[si].materialIndex;
        }

        return (int)m_skinnedObjects.size() - 1;
    }

    void Renderer::updateAnimation(int index, float deltaTime)
    {
        if (index < 0 || index >= (int)m_skinnedObjects.size()) return;
        auto& obj = m_skinnedObjects[index];
        if (obj.ticksPerSecond <= 0.0f || obj.duration <= 0.0f) return;
        obj.animTime += deltaTime * obj.ticksPerSecond;
        if (obj.animTime > obj.duration)
            obj.animTime = std::fmod(obj.animTime, obj.duration);
    }

    void Renderer::setSkinnedTransform(int index, const glm::mat4& t)
    {
        if (index >= 0 && index < (int)m_skinnedObjects.size())
            m_skinnedObjects[index].transform = t;
    }

    void Renderer::setSceneRoot(GameObject* root)
    {
        m_sceneRoot = root;
        m_editorUI.setOnDelete([this](GameObject* node) { removeGameObject(node); });
    }

    void Renderer::removeStaticObject(int index)
    {
        if (index < 0 || index >= (int)m_objects.size()) return;
        RenderObject& obj = m_objects[index];
        if (obj.vertexBuffer == VK_NULL_HANDLE) return; // ya liberado
        destroyRenderObject(obj);
        obj = RenderObject{};
    }

    void Renderer::removeSkinnedObject(int index)
    {
        if (index < 0 || index >= (int)m_skinnedObjects.size()) return;
        SkinnedRenderObject& obj = m_skinnedObjects[index];
        if (obj.outputVertexBuffer == VK_NULL_HANDLE) return; // ya liberado
        destroySkinnedRenderObject(obj);
        obj = SkinnedRenderObject{};
    }

    void Renderer::removeGameObject(GameObject* node)
    {
        if (!node) return;
        // Espera a que la GPU termine antes de destruir buffers/texturas que
        // un command buffer en vuelo (double buffering) pudiera seguir usando.
        vkDeviceWaitIdle(m_gpu.device());
        node->traverse([this](GameObject* go) {
            if (go->staticRenderIndex >= 0)
                removeStaticObject(go->staticRenderIndex);
            if (go->skinnedRenderIndex >= 0)
                removeSkinnedObject(go->skinnedRenderIndex);
        });
    }

    void Renderer::recordComputePass(VkCommandBuffer cmd)
    {
        if (m_skinnedObjects.empty()) return;

        auto ssboBarrier = [](VkBuffer buf) {
            VkBufferMemoryBarrier b{};
            b.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b.buffer        = buf;
            b.offset        = 0;
            b.size          = VK_WHOLE_SIZE;
            return b;
        };

        for (auto& obj : m_skinnedObjects)
        {
            if (obj.outputVertexBuffer == VK_NULL_HANDLE) continue; // borrado desde el editor
            ComputePush push{};
            push.animTime    = obj.animTime;
            push.boneCount   = obj.boneCount;
            push.vertexCount = obj.vertexCount;
            push.pad         = 0;

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                m_computePipelineLayout, 0, 1, &obj.computeDescSet, 0, nullptr);

            // --- Pass 1: bone_eval (local transforms) ---
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_boneEvalPipeline);
            vkCmdPushConstants(cmd, m_computePipelineLayout,
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePush), &push);
            vkCmdDispatch(cmd, (obj.boneCount + 63) / 64, 1, 1);

            // Barrier: localTransform escrito → leído por bone_hierarchy
            VkBufferMemoryBarrier b1 = ssboBarrier(obj.localTransformBuffer);
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 1, &b1, 0, nullptr);

            // --- Pass 2: bone_hierarchy (world transforms + inverse bind pose) ---
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_boneHierarchyPipeline);
            vkCmdPushConstants(cmd, m_computePipelineLayout,
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePush), &push);
            vkCmdDispatch(cmd, 1, 1, 1);

            // Barrier: finalBone escrito → leído por skinning
            VkBufferMemoryBarrier b2 = ssboBarrier(obj.finalBoneBuffer);
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 1, &b2, 0, nullptr);

            // --- Pass 3: skinning (output vertex buffer) ---
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_skinningPipeline);
            vkCmdPushConstants(cmd, m_computePipelineLayout,
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePush), &push);
            vkCmdDispatch(cmd, (obj.vertexCount + 63) / 64, 1, 1);

            // Barrier: outputVertexBuffer escrito por compute → leído como VB en vertex shader
            VkBufferMemoryBarrier b3{};
            b3.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            b3.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            b3.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
            b3.buffer        = obj.outputVertexBuffer;
            b3.offset        = 0;
            b3.size          = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                0, 0, nullptr, 1, &b3, 0, nullptr);
        }
    }

    // ─── Offscreen images ────────────────────────────────────────────────────────

    void Renderer::createOffscreenImages()
    {
        // Sampler compartido entre los dos frames offscreen
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter    = VK_FILTER_LINEAR;
        samplerInfo.minFilter    = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.borderColor  = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        if (vkCreateSampler(m_gpu.device(), &samplerInfo, nullptr, &m_offscreenSampler) != VK_SUCCESS)
            throw std::runtime_error("failed to create offscreen sampler!");

        for (int i = 0; i < MAX_FRAMES; i++)
        {
            // Imagen color offscreen
            m_res.createImage(
                m_swapChainExtent.width, m_swapChainExtent.height,
                m_swapChainFormat,
                VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                m_offscreenImage[i], m_offscreenMemory[i]);

            m_res.createTextureImageView(m_offscreenImage[i], m_offscreenView[i], m_swapChainFormat);

            // Framebuffer offscreen: color + depth compartido
            VkImageView atts[] = { m_offscreenView[i], m_depthImageView };
            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass      = m_offscreenRenderPass;
            fbInfo.attachmentCount = 2;
            fbInfo.pAttachments    = atts;
            fbInfo.width           = m_swapChainExtent.width;
            fbInfo.height          = m_swapChainExtent.height;
            fbInfo.layers          = 1;
            if (vkCreateFramebuffer(m_gpu.device(), &fbInfo, nullptr, &m_offscreenFramebuffer[i]) != VK_SUCCESS)
                throw std::runtime_error("failed to create offscreen framebuffer!");

            // Registrar la textura en ImGui para obtener el VkDescriptorSet
            m_offscreenDescSet[i] = ImGui_ImplVulkan_AddTexture(
                m_offscreenSampler, m_offscreenView[i],
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        printf("offscreen images OK\n"); fflush(stdout);
    }

    void Renderer::destroyOffscreenImages()
    {
        for (int i = 0; i < MAX_FRAMES; i++)
        {
            if (m_offscreenDescSet[i])
            {
                ImGui_ImplVulkan_RemoveTexture(m_offscreenDescSet[i]);
                m_offscreenDescSet[i] = VK_NULL_HANDLE;
            }
            if (m_offscreenFramebuffer[i])
            {
                vkDestroyFramebuffer(m_gpu.device(), m_offscreenFramebuffer[i], nullptr);
                m_offscreenFramebuffer[i] = VK_NULL_HANDLE;
            }
            if (m_offscreenView[i])
            {
                vkDestroyImageView(m_gpu.device(), m_offscreenView[i], nullptr);
                m_offscreenView[i] = VK_NULL_HANDLE;
            }
            if (m_offscreenImage[i])
            {
                vkDestroyImage(m_gpu.device(), m_offscreenImage[i], nullptr);
                m_offscreenImage[i] = VK_NULL_HANDLE;
            }
            if (m_offscreenMemory[i])
            {
                vkFreeMemory(m_gpu.device(), m_offscreenMemory[i], nullptr);
                m_offscreenMemory[i] = VK_NULL_HANDLE;
            }
        }
        if (m_offscreenSampler)
        {
            vkDestroySampler(m_gpu.device(), m_offscreenSampler, nullptr);
            m_offscreenSampler = VK_NULL_HANDLE;
        }
    }

    // ─── ImGui init / shutdown ───────────────────────────────────────────────────

    void Renderer::initImGui(GLFWwindow* window)
    {
        // Pool dedicado para ImGui (necesita FREE_DESCRIPTOR_SET_BIT).
        // La API nueva (sept 2025) usa SAMPLER + SAMPLED_IMAGE separados en AddTexture().
        VkDescriptorPoolSize poolSizes[3]{};
        poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[0].descriptorCount = 16;
        poolSizes[1].type            = VK_DESCRIPTOR_TYPE_SAMPLER;
        poolSizes[1].descriptorCount = 16;
        poolSizes[2].type            = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        poolSizes[2].descriptorCount = 16;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets       = 48;
        poolInfo.poolSizeCount = 3;
        poolInfo.pPoolSizes    = poolSizes;
        if (vkCreateDescriptorPool(m_gpu.device(), &poolInfo, nullptr, &m_imguiDescPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create ImGui descriptor pool!");

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

        ImGui::StyleColorsDark();

        // Sin instalar callbacks GLFW propios: ImGui los sondea en NewFrame
        ImGui_ImplGlfw_InitForVulkan(window, false);

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.ApiVersion                       = VK_API_VERSION_1_0;
        initInfo.Instance                         = m_gpu.instance();
        initInfo.PhysicalDevice                   = m_gpu.physicalDevice();
        initInfo.Device                           = m_gpu.device();
        initInfo.QueueFamily                      = m_gpu.graphicsFamily();
        initInfo.Queue                            = m_gpu.graphicsQueue();
        initInfo.DescriptorPool                   = m_imguiDescPool;
        initInfo.MinImageCount                    = 2;
        initInfo.ImageCount                       = (uint32_t)m_swapChainImages.size();
        initInfo.PipelineInfoMain.RenderPass      = m_renderPass;
        initInfo.PipelineInfoMain.MSAASamples     = VK_SAMPLE_COUNT_1_BIT;
        ImGui_ImplVulkan_Init(&initInfo);

        printf("ImGui init OK\n"); fflush(stdout);
    }

    void Renderer::shutdownImGui()
    {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        if (m_imguiDescPool)
        {
            vkDestroyDescriptorPool(m_gpu.device(), m_imguiDescPool, nullptr);
            m_imguiDescPool = VK_NULL_HANDLE;
        }
    }

}
