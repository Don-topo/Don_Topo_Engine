#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <array>
#include <string>

namespace DonTopo {

class GpuDevice;

class Skybox {
public:
    Skybox()                           = default;
    Skybox(const Skybox&)             = delete;
    Skybox& operator=(const Skybox&)  = delete;

    // facePaths: +X, -X, +Y, -Y, +Z, -Z
    void init(GpuDevice& gpu, VkRenderPass renderPass, VkFormat colorFormat,
              const std::array<std::string, 6>& facePaths);
    void shutdown(GpuDevice& gpu);

    // invViewProj = inverse(proj * mat4(mat3(view))) — sin traslación de cámara
    void draw(VkCommandBuffer cmd, const glm::mat4& invViewProj);

    bool isInitialized() const { return m_pipeline != VK_NULL_HANDLE; }

private:
    void loadCubemap(GpuDevice& gpu, const std::array<std::string, 6>& facePaths);
    void createDescriptors(GpuDevice& gpu);
    void createPipeline(GpuDevice& gpu, VkRenderPass renderPass, VkFormat colorFormat);

    VkImage               m_image      = VK_NULL_HANDLE;
    VkDeviceMemory        m_memory     = VK_NULL_HANDLE;
    VkImageView           m_view       = VK_NULL_HANDLE;
    VkSampler             m_sampler    = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_descSet    = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipeLayout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline   = VK_NULL_HANDLE;
};

} // namespace DonTopo
