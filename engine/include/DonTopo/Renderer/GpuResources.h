#pragma once
#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <cstdint>

namespace DonTopo {

class GpuDevice;

class GpuResources {
public:
    explicit GpuResources(const GpuDevice& gpu) : m_gpu(gpu) {}
    GpuResources(const GpuResources&)            = delete;
    GpuResources& operator=(const GpuResources&) = delete;

    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags props,
                      VkBuffer& buf, VkDeviceMemory& mem);
    void copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size);
    void uploadBuffer(const void* data, VkDeviceSize size,
                      VkBufferUsageFlags usage,
                      VkBuffer& buf, VkDeviceMemory& mem);

    void createImage(uint32_t w, uint32_t h, VkFormat fmt,
                     VkImageTiling tiling, VkImageUsageFlags usage,
                     VkMemoryPropertyFlags props,
                     VkImage& img, VkDeviceMemory& mem);
    void transitionImageLayout(VkImage img,
                               VkImageLayout from, VkImageLayout to);
    void copyBufferToImage(VkBuffer buf, VkImage img,
                           uint32_t w, uint32_t h);

    void createTextureImage(const std::string& path,
                            const std::vector<uint8_t>& embedded,
                            VkImage& img, VkDeviceMemory& mem);
    void createNormalMapImage(const std::string& path,
                              const std::vector<uint8_t>& embedded,
                              VkImage& img, VkDeviceMemory& mem);
    void createSolidColorImage(const uint8_t rgba[4],
                               VkImage& img, VkDeviceMemory& mem);
    void createTextureImageView(VkImage img, VkImageView& view,
                                VkFormat fmt = VK_FORMAT_R8G8B8A8_SRGB);
    void createTextureSampler(VkSampler& out);

private:
    const GpuDevice& m_gpu;
};

} // namespace DonTopo
