#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <string>

namespace DonTopo {

    class Window;
    
    class Renderer {
        public:                
            Renderer() = default;
            ~Renderer();
            Renderer(const Renderer&) = delete;
            Renderer& operator=(const Renderer&) = delete;
            void init(Window& window) ;
            void drawFrame();
            void shutdown();

        private:
            void createInstance();
            void createSurface(Window& window);
            void pickPhysicalDevice();
            void createDevice();
            void createSwapChain(Window& window);
            void createImageViews();
            void createRenderPass();
            void createFramebuffers();
            void createCommandPool();
            void createCommandBuffers();
            void createSyncObjects();
            void recordCommandBuffer(uint32_t imageIndex);
            void createPipeline();
            std::vector<char> loadShaderFile(const std::string& path);
            VkShaderModule createShaderModule(const std::vector<char>& code);

            VkInstance m_instance                       = VK_NULL_HANDLE;
            VkSurfaceKHR m_surface                      = VK_NULL_HANDLE;
            VkPhysicalDevice m_physicalDevice           = VK_NULL_HANDLE;
            uint32_t m_graphicsFamily                   = 0;
            uint32_t m_presentFamily                    = 0;
            VkDevice m_device                           = VK_NULL_HANDLE;
            VkQueue m_graphicsQueue                     = VK_NULL_HANDLE;
            VkQueue m_presentQueue                      = VK_NULL_HANDLE;
            VkSwapchainKHR m_swapChain                  = VK_NULL_HANDLE;
            VkFormat m_swapChainFormat                  = VK_FORMAT_UNDEFINED;
            VkExtent2D m_swapChainExtent                = {};
            std::vector<VkImage> m_swapChainImages;
            VkColorSpaceKHR m_swapChainColorSpace       = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            std::vector<VkImageView> m_swapChainImageViews;
            VkRenderPass m_renderPass                   = VK_NULL_HANDLE;
            std::vector<VkFramebuffer> m_swapChainFramebuffers;
            VkCommandPool m_commandPool                 = VK_NULL_HANDLE;
            std::vector<VkCommandBuffer> m_commandBuffers;
            static constexpr int MAX_FRAMES             = 2;
            VkSemaphore m_imageAvailable[MAX_FRAMES]    = {};
            VkSemaphore m_renderFinished[MAX_FRAMES]    = {};
            VkFence m_inFlight[MAX_FRAMES]              = {};
            int m_currentFrame                          = 0;
            VkPipelineLayout m_pipelineLayout           = VK_NULL_HANDLE;
            VkPipeline m_pipeline                       = VK_NULL_HANDLE;
    };
}