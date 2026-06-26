#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <glm/glm.hpp>

namespace DonTopo {

    class Window;
    
    class Renderer {
        public:                
            Renderer()                              = default;
            ~Renderer();
            Renderer(const Renderer&)               = delete;
            Renderer& operator=(const Renderer&)    = delete;
            void init(Window& window);
            void drawFrame(Window& window);
            void shutdown();

        private:
            void setupDebugMessenger();
            static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
                VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                VkDebugUtilsMessageTypeFlagsEXT types,
                const VkDebugUtilsMessengerCallbackDataEXT* data,
                void* userData);
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
            void recreateSwapChain(Window& window);
            void createVertexBuffer();
            uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags props);
            void createDescriptorSetLayout();
            void createUniformBuffers();
            void createDescriptorPool();
            void createDescriptorSets();
            void updateUniformBuffer(uint32_t frameIndex);

            VkDebugUtilsMessengerEXT        m_debugMessenger   = VK_NULL_HANDLE;
            VkInstance                      m_instance                          = VK_NULL_HANDLE;
            VkSurfaceKHR                    m_surface                           = VK_NULL_HANDLE;
            VkPhysicalDevice                m_physicalDevice                    = VK_NULL_HANDLE;
            uint32_t                        m_graphicsFamily                    = 0;
            uint32_t                        m_presentFamily                     = 0;
            VkDevice                        m_device                            = VK_NULL_HANDLE;
            VkQueue                         m_graphicsQueue                     = VK_NULL_HANDLE;
            VkQueue                         m_presentQueue                      = VK_NULL_HANDLE;
            VkSwapchainKHR                  m_swapChain                         = VK_NULL_HANDLE;
            VkFormat                        m_swapChainFormat                   = VK_FORMAT_UNDEFINED;
            VkExtent2D                      m_swapChainExtent                   = {};
            std::vector<VkImage>            m_swapChainImages;
            VkColorSpaceKHR                 m_swapChainColorSpace               = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            std::vector<VkImageView>        m_swapChainImageViews;
            VkRenderPass                    m_renderPass                        = VK_NULL_HANDLE;
            std::vector<VkFramebuffer>      m_swapChainFramebuffers;
            VkCommandPool                   m_commandPool                       = VK_NULL_HANDLE;
            std::vector<VkCommandBuffer>    m_commandBuffers;
            static constexpr int            MAX_FRAMES                          = 2;
            VkSemaphore                     m_imageAvailable[MAX_FRAMES]        = {};
            std::vector<VkSemaphore>        m_renderFinished;
            VkFence                         m_inFlight[MAX_FRAMES]              = {};
            int                             m_currentFrame                      = 0;
            VkPipelineLayout                m_pipelineLayout                    = VK_NULL_HANDLE;
            VkPipeline                      m_pipeline                          = VK_NULL_HANDLE;
            bool                            m_framebufferResized                = false;
            VkBuffer                        m_vertexBuffer                      = VK_NULL_HANDLE;
            VkDeviceMemory                  m_vertexBufferMemory                = VK_NULL_HANDLE;
            glm::mat4                       m_transform{1.0f};
            VkDescriptorSetLayout           m_descriptorSetLayout               = VK_NULL_HANDLE;
            VkBuffer                        m_uniformBuffers[MAX_FRAMES]        = {};
            VkDeviceMemory                  m_uniformBuffersMemory[MAX_FRAMES]  = {};
            void*                           m_uniformBuffersMapped[MAX_FRAMES]  = {};
            VkDescriptorPool                m_descriptorPool                    = VK_NULL_HANDLE;
            VkDescriptorSet                 m_descriptorSets[MAX_FRAMES]        = {};
    };
}