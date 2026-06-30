#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <glm/glm.hpp>
#include "DonTopo/Mesh.h"
#include "DonTopo/Camera.h"
#include "DonTopo/UniformBufferObject.h"

namespace DonTopo {

    class Window;
    
    class Renderer {
        public:                
            Renderer()                              = default;
            ~Renderer();
            Renderer(const Renderer&)               = delete;
            Renderer& operator=(const Renderer&)    = delete;
            void init(Window& window, const std::vector<Mesh>& meshes);
            void drawFrame(Window& window);
            void shutdown();
            void setCamera(const Camera& camera);
            void notifyResize() { m_framebufferResized = true; }
            void setTransform(size_t objectIndex, const glm::mat4& transform)
            {
                if (objectIndex < m_objects.size())
                    m_objects[objectIndex].transform = transform;
            }
            void setLights(const std::vector<Light>& lights){ m_lights = lights; }

        private:

            struct RenderObject {
                VkBuffer        vertexBuffer        = VK_NULL_HANDLE;
                VkDeviceMemory  vertexMemory        = VK_NULL_HANDLE;
                VkBuffer        indexBuffer         = VK_NULL_HANDLE;
                VkDeviceMemory  indexMemory         = VK_NULL_HANDLE;
                uint32_t        indexCount          = 0;
                // first texture
                VkImage         textureImage        = VK_NULL_HANDLE;
                VkDeviceMemory  textureMem          = VK_NULL_HANDLE;
                VkImageView     textureView         = VK_NULL_HANDLE;
                VkSampler       sampler             = VK_NULL_HANDLE;
                // Second texture
                VkImage         normalImage         = VK_NULL_HANDLE;
                VkDeviceMemory  normalMem           = VK_NULL_HANDLE;
                VkImageView     normalView          = VK_NULL_HANDLE;
                VkSampler       normalSampler       = VK_NULL_HANDLE;
                VkDescriptorSet descriptorSets[2]   = {};
                glm::mat4       transform{1.0f};
            };

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
            void createVertexBuffer(const std::vector<Vertex>& v, VkBuffer& buf, VkDeviceMemory& mem);
            void createIndexBuffer(const std::vector<uint32_t>& idx, VkBuffer& buf, VkDeviceMemory& mem);
            uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags props);
            void createDescriptorSetLayout();
            void createUniformBuffers();
            void createDescriptorPool();
            void createDescriptorSets();
            void updateUniformBuffer(uint32_t frameIndex);
            void createDepthResources();
            void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer& buffer, VkDeviceMemory& memory);
            void copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size);
            VkCommandBuffer beginOneTimeCommands();
            void endOneTimeCommands(VkCommandBuffer comandBuffer);
            void createTextureImage(const std::string& path, const std::vector<uint8_t>& embedded, VkImage& img, VkDeviceMemory& mem);
            void createNormalMapImage(const std::string& path, const std::vector<uint8_t>& embedded, VkImage& img, VkDeviceMemory& mem);
            void createImage(uint32_t w, uint32_t h, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags props, VkImage& image, VkDeviceMemory& memory);
            void transitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout);
            void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t w, uint32_t h);
            void buildRenderObject(const Mesh& mesh, RenderObject& obj);
            void destroyRenderObject(RenderObject& obj);
            void createTextureImageView(VkImage img, VkImageView& view, VkFormat format = VK_FORMAT_R8G8B8A8_SRGB);
            void createTextureSampler(VkSampler& out);

            
            VkDebugUtilsMessengerEXT        m_debugMessenger                    = VK_NULL_HANDLE;
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
            VkDescriptorSetLayout           m_descriptorSetLayout               = VK_NULL_HANDLE;
            VkBuffer                        m_uniformBuffers[MAX_FRAMES]        = {};
            VkDeviceMemory                  m_uniformBuffersMemory[MAX_FRAMES]  = {};
            void*                           m_uniformBuffersMapped[MAX_FRAMES]  = {};
            VkDescriptorPool                m_descriptorPool                    = VK_NULL_HANDLE;
            VkImage                         m_depthImage                        = VK_NULL_HANDLE;
            VkDeviceMemory                  m_depthImageMemory                  = VK_NULL_HANDLE;
            VkImageView                     m_depthImageView                    = VK_NULL_HANDLE;
            glm::vec3                       m_cameraTarget{0.0f};
            float                           m_cameraDistance{5.0f};
            glm::mat4                       m_viewMatrix{1.0f};
            Camera                          m_camera;
            std::vector<Light>              m_lights;
            std::vector<RenderObject> m_objects;
    };
}