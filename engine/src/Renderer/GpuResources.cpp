#include "DonTopo/Renderer/GpuResources.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include <stdexcept>
#include <cstring>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace DonTopo {

void GpuResources::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer& buffer, VkDeviceMemory& memory)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType        = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size         = size;
    bufferInfo.usage        = usage;
    bufferInfo.sharingMode  = VK_SHARING_MODE_EXCLUSIVE;

    if(vkCreateBuffer(m_gpu.device(), &bufferInfo, nullptr, &buffer) != VK_SUCCESS)
        throw std::runtime_error("failed to create buffer!");

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(m_gpu.device(), buffer, &req);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType             = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize    = req.size;
    allocInfo.memoryTypeIndex   = m_gpu.findMemoryType(req.memoryTypeBits, props);

    if(vkAllocateMemory(m_gpu.device(), &allocInfo, nullptr, &memory) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate buffer memory!");

    vkBindBufferMemory(m_gpu.device(), buffer, memory, 0);
}

void GpuResources::copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size)
{
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool        = m_gpu.commandPool();
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(m_gpu.device(), &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    VkBufferCopy region{};
    region.size = size;
    vkCmdCopyBuffer(commandBuffer, src, dst, 1, &region);

    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &commandBuffer;
    vkQueueSubmit(m_gpu.graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_gpu.graphicsQueue());

    vkFreeCommandBuffers(m_gpu.device(), m_gpu.commandPool(), 1, &commandBuffer);
}

void GpuResources::uploadBuffer(const void* data, VkDeviceSize size,
                                VkBufferUsageFlags usage,
                                VkBuffer& buf, VkDeviceMemory& mem)
{
    VkBuffer       stagingBuf;
    VkDeviceMemory stagingMem;
    createBuffer(size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuf, stagingMem);

    void* mapped;
    vkMapMemory(m_gpu.device(), stagingMem, 0, size, 0, &mapped);
    memcpy(mapped, data, size);
    vkUnmapMemory(m_gpu.device(), stagingMem);

    createBuffer(size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        buf, mem);
    copyBuffer(stagingBuf, buf, size);

    vkDestroyBuffer(m_gpu.device(), stagingBuf, nullptr);
    vkFreeMemory(m_gpu.device(), stagingMem, nullptr);
}

void GpuResources::createImage(uint32_t w, uint32_t h, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags props, VkImage& image, VkDeviceMemory& memory)
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType             = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType         = VK_IMAGE_TYPE_2D;
    imageInfo.format            = format;
    imageInfo.extent            = { w, h, 1 };
    imageInfo.mipLevels         = 1;
    imageInfo.arrayLayers       = 1;
    imageInfo.samples           = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling            = tiling;
    imageInfo.usage             = usage;
    imageInfo.sharingMode       = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout     = VK_IMAGE_LAYOUT_UNDEFINED;

    if(vkCreateImage(m_gpu.device(), &imageInfo, nullptr, &image) != VK_SUCCESS)
        throw std::runtime_error("failed to create image!");

    VkMemoryRequirements memoryRequirement;
    vkGetImageMemoryRequirements(m_gpu.device(), image, &memoryRequirement);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType             = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize    = memoryRequirement.size;
    allocInfo.memoryTypeIndex   = m_gpu.findMemoryType(memoryRequirement.memoryTypeBits, props);

    if(vkAllocateMemory(m_gpu.device(), &allocInfo, nullptr, &memory) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate image memory!");

    vkBindImageMemory(m_gpu.device(), image, memory, 0);
}

void GpuResources::transitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout)
{
    VkCommandBuffer cmd = m_gpu.beginOneTimeCommands();

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

    if(oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if(oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        throw std::runtime_error("unsupported layout transition!");
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    m_gpu.endOneTimeCommands(cmd);
}

void GpuResources::copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t w, uint32_t h)
{
    VkCommandBuffer cmd = m_gpu.beginOneTimeCommands();

    VkBufferImageCopy region{};
    region.bufferOffset                    = 0;
    region.bufferRowLength                 = 0;
    region.bufferImageHeight               = 0;
    region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel       = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount     = 1;
    region.imageOffset                     = {0, 0, 0};
    region.imageExtent                     = {w, h, 1};

    vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    m_gpu.endOneTimeCommands(cmd);
}

void GpuResources::createTextureImage(const std::string& path, const std::vector<uint8_t>& embedded, VkImage& img, VkDeviceMemory& mem)
{
    int w, h, channels;
    stbi_uc* pixels = nullptr;
    bool fromStb = false;

    if (!embedded.empty()) {
        pixels = stbi_load_from_memory(embedded.data(), (int)embedded.size(), &w, &h, &channels, STBI_rgb_alpha);
        fromStb = (pixels != nullptr);
    } else if (!path.empty()) {
        pixels = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
        fromStb = (pixels != nullptr);
    }

    // Fallback: purple/black checkerboard when no texture or file not found
    std::vector<uint8_t> placeholder;
    if (!pixels) {
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
    vkMapMemory(m_gpu.device(), stagingMemory, 0, imageSize, 0, &data);
    memcpy(data, pixels, (size_t)imageSize);
    vkUnmapMemory(m_gpu.device(), stagingMemory);
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

    vkDestroyBuffer(m_gpu.device(), stagingBuffer, nullptr);
    vkFreeMemory(m_gpu.device(), stagingMemory, nullptr);
}

void GpuResources::createNormalMapImage(const std::string& path, const std::vector<uint8_t>& embedded, VkImage& img, VkDeviceMemory& mem)
{
    int w, h, channels;
    stbi_uc* pixels = nullptr;
    bool fromStb = false;

    if (!embedded.empty()) {
        pixels = stbi_load_from_memory(embedded.data(), (int)embedded.size(), &w, &h, &channels, STBI_rgb_alpha);
        fromStb = (pixels != nullptr);
    } else if (!path.empty()) {
        pixels = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
        fromStb = (pixels != nullptr);
    }

    // Fallback: flat normal (0,0,1) en tangent space = (128,128,255)
    uint8_t flatNormal[4] = { 0x80, 0x80, 0xFF, 0xFF };
    if (!pixels) {
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
    vkMapMemory(m_gpu.device(), stagingMemory, 0, imageSize, 0, &data);
    memcpy(data, pixels, (size_t)imageSize);
    vkUnmapMemory(m_gpu.device(), stagingMemory);
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

    vkDestroyBuffer(m_gpu.device(), stagingBuffer, nullptr);
    vkFreeMemory(m_gpu.device(), stagingMemory, nullptr);
}

void GpuResources::createTextureImageView(VkImage image, VkImageView& view, VkFormat format)
{
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                           = image;
    viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                          = format;
    viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = 1;

    if(vkCreateImageView(m_gpu.device(), &viewInfo, nullptr, &view) != VK_SUCCESS)
        throw std::runtime_error("failed to create texture image view!");
}

void GpuResources::createTextureSampler(VkSampler& outSampler)
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

    if(vkCreateSampler(m_gpu.device(), &samplerInfo, nullptr, &outSampler) != VK_SUCCESS)
        throw std::runtime_error("failed to create texture sampler!");
}

void GpuResources::createSolidColorImage(const uint8_t rgba[4], VkImage& img, VkDeviceMemory& mem)
{
    VkBuffer sb; VkDeviceMemory sm;
    createBuffer(4,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        sb, sm);
    void* mapped;
    vkMapMemory(m_gpu.device(), sm, 0, 4, 0, &mapped);
    memcpy(mapped, rgba, 4);
    vkUnmapMemory(m_gpu.device(), sm);
    createImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img, mem);
    transitionImageLayout(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copyBufferToImage(sb, img, 1, 1);
    transitionImageLayout(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkDestroyBuffer(m_gpu.device(), sb, nullptr);
    vkFreeMemory(m_gpu.device(), sm, nullptr);
}

} // namespace DonTopo
