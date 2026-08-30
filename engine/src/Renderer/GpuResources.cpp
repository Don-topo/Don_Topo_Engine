#include "DonTopo/Renderer/GpuResources.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/TransferBatch.h"
#include "DonTopo/Renderer/PlaceholderTexture.h"
#include <stdexcept>
#include <cstring>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace {
    // Devuelve el command buffer a usar. Con batch == nullptr abre uno
    // one-time igual que hasta ahora, y el caller debe cerrarlo con
    // endOneTime(). Con batch, se cuelga del command buffer compartido y NO se
    // cierra aquí.
    struct CmdScope
    {
        const DonTopo::GpuDevice& gpu;
        DonTopo::TransferBatch*   batch;
        VkCommandBuffer           cmd;

        CmdScope(const DonTopo::GpuDevice& g, DonTopo::TransferBatch* b)
            : gpu(g), batch(b), cmd(b ? b->cmd() : g.beginOneTimeCommands()) {}

        // Solo cierra y espera si NO hay batch: con batch, el submit y la fence
        // son responsabilidad de quien lo posee.
        ~CmdScope() { if (!batch) gpu.endOneTimeCommands(cmd); }
    };
}

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

void GpuResources::copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size,
                              TransferBatch* batch)
{
    CmdScope scope(m_gpu, batch);

    VkBufferCopy region{};
    region.size = size;
    vkCmdCopyBuffer(scope.cmd, src, dst, 1, &region);
}

void GpuResources::uploadBuffer(const void* data, VkDeviceSize size,
                                VkBufferUsageFlags usage,
                                VkBuffer& buf, VkDeviceMemory& mem,
                                TransferBatch* batch)
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
    copyBuffer(stagingBuf, buf, size, batch);

    if (batch)
        batch->addStaging(stagingBuf, stagingMem);   // se libera al senalar la fence
    else
    {
        vkDestroyBuffer(m_gpu.device(), stagingBuf, nullptr);
        vkFreeMemory(m_gpu.device(), stagingMem, nullptr);
    }
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

void GpuResources::transitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, TransferBatch* batch)
{
    CmdScope scope(m_gpu, batch);

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

    vkCmdPipelineBarrier(scope.cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void GpuResources::copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t w, uint32_t h, TransferBatch* batch)
{
    CmdScope scope(m_gpu, batch);

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

    vkCmdCopyBufferToImage(scope.cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void GpuResources::createTextureImage(const std::string& path, const std::vector<uint8_t>& embedded, VkImage& img, VkDeviceMemory& mem, TransferBatch* batch)
{
    // Material que NO pide textura -una primitiva procedural, por ejemplo-: la
    // blanca compartida, prestada. Antes cada malla se llevaba su propia 1x1
    // blanca, con su asignacion de memoria y la de su staging. El caso de
    // "la pide y no se pudo leer" NO entra aqui: ese sigue con su damero
    // propio, y es raro por definicion.
    if (path.empty() && embedded.empty())
    {
        static constexpr uint8_t kBlanco[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        ensurePlaceholder(m_whiteSrgb, m_whiteSrgbMem, kBlanco, VK_FORMAT_R8G8B8A8_SRGB);
        img = m_whiteSrgb;
        mem = m_whiteSrgbMem;
        return;
    }

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

    // Sin pixeles hay DOS motivos distintos y hasta ahora los dos acababan en
    // damero. Ver PlaceholderTexture.h: el material que no pide ninguna textura
    // -una primitiva procedural- se rellena de blanco, y solo el que la pide y
    // no se ha podido leer se marca con el damero.
    std::vector<uint8_t> placeholder;
    if (!pixels) {
        const bool sePidioTextura = !embedded.empty() || !path.empty();
        if (sePidioTextura) {
            placeholder = makeMissingTextureRgba();
            w = h = kMissingTextureSize;
        } else {
            placeholder.assign(4, 0xFF);  // 1x1 blanco
            w = h = 1;
        }
        pixels = placeholder.data();
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

    transitionImageLayout(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, batch);
    copyBufferToImage(stagingBuffer, img, (uint32_t)w, (uint32_t)h, batch);
    transitionImageLayout(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, batch);

    if (batch)
        batch->addStaging(stagingBuffer, stagingMemory);   // se libera al senalar la fence
    else
    {
        vkDestroyBuffer(m_gpu.device(), stagingBuffer, nullptr);
        vkFreeMemory(m_gpu.device(), stagingMemory, nullptr);
    }
}

void GpuResources::createNormalMapImage(const std::string& path, const std::vector<uint8_t>& embedded, VkImage& img, VkDeviceMemory& mem, TransferBatch* batch)
{
    // Sin normal map: la plana compartida (0,0,1 en tangent space).
    if (path.empty() && embedded.empty())
    {
        static constexpr uint8_t kNormalPlana[4] = {0x80, 0x80, 0xFF, 0xFF};
        ensurePlaceholder(m_flatNormal, m_flatNormalMem, kNormalPlana, VK_FORMAT_R8G8B8A8_UNORM);
        img = m_flatNormal;
        mem = m_flatNormalMem;
        return;
    }

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

    transitionImageLayout(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, batch);
    copyBufferToImage(stagingBuffer, img, (uint32_t)w, (uint32_t)h, batch);
    transitionImageLayout(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, batch);

    if (batch)
        batch->addStaging(stagingBuffer, stagingMemory);   // se libera al senalar la fence
    else
    {
        vkDestroyBuffer(m_gpu.device(), stagingBuffer, nullptr);
        vkFreeMemory(m_gpu.device(), stagingMemory, nullptr);
    }
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

VkSampler GpuResources::sharedMaterialSampler()
{
    // Perezoso y no en el constructor: GpuResources se construye con el
    // GpuDevice, y en ese momento el device de Vulkan todavia no existe.
    if (m_materialSampler == VK_NULL_HANDLE)
        createTextureSampler(m_materialSampler);
    return m_materialSampler;
}

void GpuResources::destroySharedSampler()
{
    if (m_materialSampler == VK_NULL_HANDLE)
        return;
    vkDestroySampler(m_gpu.device(), m_materialSampler, nullptr);
    m_materialSampler = VK_NULL_HANDLE;
}

// ── Texturas de relleno compartidas ─────────────────────────────────────────

void GpuResources::ensurePlaceholder(VkImage& img, VkDeviceMemory& mem,
                                     const uint8_t rgba[4], VkFormat fmt)
{
    if (img != VK_NULL_HANDLE)
        return;

    // Sin batch a proposito: son 4 bytes y se suben UNA vez en toda la vida del
    // proceso. Meterlas en el batch del llamante las ataria a su fence y
    // obligaria a razonar sobre quien sube primero.
    VkBuffer       staging;
    VkDeviceMemory stagingMem;
    createBuffer(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 staging, stagingMem);

    void* data = nullptr;
    vkMapMemory(m_gpu.device(), stagingMem, 0, 4, 0, &data);
    memcpy(data, rgba, 4);
    vkUnmapMemory(m_gpu.device(), stagingMem);

    createImage(1, 1, fmt, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img, mem);

    transitionImageLayout(img, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nullptr);
    copyBufferToImage(staging, img, 1, 1, nullptr);
    transitionImageLayout(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, nullptr);

    vkDestroyBuffer(m_gpu.device(), staging, nullptr);
    vkFreeMemory(m_gpu.device(), stagingMem, nullptr);
}

void GpuResources::sharedWhiteOrm(VkImage& img, VkDeviceMemory& mem)
{
    static constexpr uint8_t kBlanco[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    ensurePlaceholder(m_whiteUnorm, m_whiteUnormMem, kBlanco, VK_FORMAT_R8G8B8A8_UNORM);
    img = m_whiteUnorm;
    mem = m_whiteUnormMem;
}

bool GpuResources::isSharedPlaceholder(VkImage img) const
{
    if (img == VK_NULL_HANDLE)
        return false;
    return img == m_whiteSrgb || img == m_flatNormal || img == m_whiteUnorm;
}

void GpuResources::releaseMaterialImage(VkImage img, VkDeviceMemory mem)
{
    // Prestada: es de este GpuResources y la comparten todas las mallas sin
    // material. Destruirla con la primera dejaria a las demas muestreando una
    // imagen liberada, y eso no lo delata nada hasta que se ve basura.
    if (isSharedPlaceholder(img))
        return;

    if (img != VK_NULL_HANDLE) vkDestroyImage(m_gpu.device(), img, nullptr);
    if (mem != VK_NULL_HANDLE) vkFreeMemory(m_gpu.device(), mem, nullptr);
}

void GpuResources::destroySharedPlaceholders()
{
    auto suelta = [this](VkImage& img, VkDeviceMemory& mem) {
        if (img != VK_NULL_HANDLE) vkDestroyImage(m_gpu.device(), img, nullptr);
        if (mem != VK_NULL_HANDLE) vkFreeMemory(m_gpu.device(), mem, nullptr);
        img = VK_NULL_HANDLE;
        mem = VK_NULL_HANDLE;
    };
    suelta(m_whiteSrgb,  m_whiteSrgbMem);
    suelta(m_flatNormal, m_flatNormalMem);
    suelta(m_whiteUnorm, m_whiteUnormMem);
}

void GpuResources::createSolidColorImage(const uint8_t rgba[4], VkImage& img, VkDeviceMemory& mem, TransferBatch* batch)
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
    transitionImageLayout(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, batch);
    copyBufferToImage(sb, img, 1, 1, batch);
    transitionImageLayout(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, batch);
    if (batch)
        batch->addStaging(sb, sm);   // se libera al senalar la fence
    else
    {
        vkDestroyBuffer(m_gpu.device(), sb, nullptr);
        vkFreeMemory(m_gpu.device(), sm, nullptr);
    }
}

void GpuResources::createTextureImageFromPixels(const uint8_t* rgba, uint32_t w, uint32_t h,
                                                VkImage& img, VkDeviceMemory& mem,
                                                TransferBatch* batch)
{
    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(w) * h * 4;

    VkBuffer       stagingBuffer;
    VkDeviceMemory stagingMemory;
    createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 stagingBuffer, stagingMemory);

    void* data;
    vkMapMemory(m_gpu.device(), stagingMemory, 0, imageSize, 0, &data);
    memcpy(data, rgba, static_cast<size_t>(imageSize));
    vkUnmapMemory(m_gpu.device(), stagingMemory);

    createImage(w, h, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img, mem);

    transitionImageLayout(img, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, batch);
    copyBufferToImage(stagingBuffer, img, w, h, batch);
    transitionImageLayout(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, batch);

    if (batch)
        batch->addStaging(stagingBuffer, stagingMemory);
    else
    {
        vkDestroyBuffer(m_gpu.device(), stagingBuffer, nullptr);
        vkFreeMemory(m_gpu.device(), stagingMemory, nullptr);
    }
}

void GpuResources::createNormalMapImageFromPixels(const uint8_t* rgba, uint32_t w, uint32_t h,
                                                  VkImage& img, VkDeviceMemory& mem,
                                                  TransferBatch* batch)
{
    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(w) * h * 4;

    VkBuffer       stagingBuffer;
    VkDeviceMemory stagingMemory;
    createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 stagingBuffer, stagingMemory);

    void* data;
    vkMapMemory(m_gpu.device(), stagingMemory, 0, imageSize, 0, &data);
    memcpy(data, rgba, static_cast<size_t>(imageSize));
    vkUnmapMemory(m_gpu.device(), stagingMemory);

    createImage(w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img, mem);

    transitionImageLayout(img, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, batch);
    copyBufferToImage(stagingBuffer, img, w, h, batch);
    transitionImageLayout(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, batch);

    if (batch)
        batch->addStaging(stagingBuffer, stagingMemory);
    else
    {
        vkDestroyBuffer(m_gpu.device(), stagingBuffer, nullptr);
        vkFreeMemory(m_gpu.device(), stagingMemory, nullptr);
    }
}

} // namespace DonTopo
