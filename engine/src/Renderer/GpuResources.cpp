#include "DonTopo/Renderer/GpuResources.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/TransferBatch.h"
#include "DonTopo/Renderer/PlaceholderTexture.h"
#include "DonTopo/Renderer/MaterialTextureSource.h"
#include <stdexcept>
#include <cstring>
#include <string>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace {
    // Returns the command buffer to use. With batch == nullptr it opens a
    // one-time one as before, and the caller must close it with
    // endOneTime(). With a batch, it hangs off the shared command buffer and is NOT
    // closed here.
    struct CmdScope
    {
        const DonTopo::GpuDevice& gpu;
        DonTopo::TransferBatch*   batch;
        VkCommandBuffer           cmd;

        CmdScope(const DonTopo::GpuDevice& g, DonTopo::TransferBatch* b)
            : gpu(g), batch(b), cmd(b ? b->cmd() : g.beginOneTimeCommands()) {}

        // Only closes and waits if there is NO batch: with a batch, the submit and the fence
        // are the responsibility of whoever owns it.
        ~CmdScope() { if (!batch) gpu.endOneTimeCommands(cmd); }
    };
}

namespace DonTopo {

namespace {
    // VK_ERROR_TOO_MANY_OBJECTS is the device's limit on LIVE allocations, and
    // the generic message ("failed to allocate buffer memory") points at the wrong
    // place: it looks like a lack of VRAM when what is lacking are slots. An
    // engine that requests one allocation per resource reaches it with a large scene
    // on a GPU that stays at the spec minimum (H72).
    std::string mensajeDeAsignacion(const char* que, VkResult r, uint32_t maxAllocs)
    {
        std::string m = std::string("failed to allocate ") + que + " memory";
        if (r == VK_ERROR_TOO_MANY_OBJECTS)
            m += ": reached this GPU's cap of " + std::to_string(maxAllocs) +
                 " memory allocations (about " +
                 std::to_string(maxAllocs / 2) +
                 " meshes). It is not a lack of VRAM: it is the number of allocations,"
                 " and the engine requests one per resource";
        else if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY)
            m += ": the GPU ran out of memory";
        return m;
    }
}

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

    if(const VkResult r = vkAllocateMemory(m_gpu.device(), &allocInfo, nullptr, &memory);
       r != VK_SUCCESS)
        throw std::runtime_error(mensajeDeAsignacion("buffer", r, m_gpu.maxMemoryAllocations()));

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
        batch->addStaging(stagingBuf, stagingMem);   // released when the fence is signaled
    else
    {
        vkDestroyBuffer(m_gpu.device(), stagingBuf, nullptr);
        vkFreeMemory(m_gpu.device(), stagingMem, nullptr);
    }
}

void GpuResources::createImage(uint32_t w, uint32_t h, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags props, VkImage& image, VkDeviceMemory& memory, uint32_t mipLevels)
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType             = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType         = VK_IMAGE_TYPE_2D;
    imageInfo.format            = format;
    imageInfo.extent            = { w, h, 1 };
    imageInfo.mipLevels         = mipLevels;
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

    if(const VkResult r = vkAllocateMemory(m_gpu.device(), &allocInfo, nullptr, &memory);
       r != VK_SUCCESS)
        throw std::runtime_error(mensajeDeAsignacion("image", r, m_gpu.maxMemoryAllocations()));

    vkBindImageMemory(m_gpu.device(), image, memory, 0);
}

namespace {
    // The two image operations, recorded into a command buffer that is already
    // open. They exist apart from the public methods because uploadPixelsToImage
    // puts all THREE (transition, copy, transition) in the same buffer: through the
    // methods it would be three submits and three waits for the same work.
    void grabarTransicion(VkCommandBuffer cmd, VkImage image,
                          VkImageLayout oldLayout, VkImageLayout newLayout,
                          uint32_t levelCount = 1);
    void grabarCopiaABufferImagen(VkCommandBuffer cmd, VkBuffer buffer, VkImage image,
                                  uint32_t w, uint32_t h);
}

void GpuResources::transitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, TransferBatch* batch)
{
    CmdScope scope(m_gpu, batch);
    grabarTransicion(scope.cmd, image, oldLayout, newLayout);
}

void GpuResources::copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t w, uint32_t h, TransferBatch* batch)
{
    CmdScope scope(m_gpu, batch);
    grabarCopiaABufferImagen(scope.cmd, buffer, image, w, h);
}

void GpuResources::uploadPixelsToImage(const void* pixels, uint32_t w, uint32_t h, VkFormat fmt,
                                       VkImage& img, VkDeviceMemory& mem, TransferBatch* batch,
                                       const TextureMip* mips, size_t mipCount)
{
    const uint32_t levels = 1 + static_cast<uint32_t>(mipCount);

    VkDeviceSize total = static_cast<VkDeviceSize>(w) * h * 4;
    for (size_t i = 0; i < mipCount; ++i)
        total += static_cast<VkDeviceSize>(mips[i].w) * mips[i].h * 4;

    VkBuffer       staging    = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    createBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 staging, stagingMem);

    void* data = nullptr;
    vkMapMemory(m_gpu.device(), stagingMem, 0, total, 0, &data);

    // All levels in ONE staging buffer and ONE copy call: level i takes up
    // w*h*4 bytes (a multiple of 4, the alignment RGBA8 requires) right after the
    // previous one.
    std::vector<VkBufferImageCopy> regions(levels);
    VkDeviceSize offset = 0;
    auto poner = [&](uint32_t level, const void* src, uint32_t lw, uint32_t lh) {
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(lw) * lh * 4;
        memcpy(static_cast<uint8_t*>(data) + offset, src, static_cast<size_t>(bytes));
        VkBufferImageCopy& r = regions[level];
        r                                 = {};
        r.bufferOffset                    = offset;
        r.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.mipLevel       = level;
        r.imageSubresource.baseArrayLayer = 0;
        r.imageSubresource.layerCount     = 1;
        r.imageExtent                     = { lw, lh, 1 };
        offset += bytes;
    };
    poner(0, pixels, w, h);
    for (size_t i = 0; i < mipCount; ++i)
        poner(static_cast<uint32_t>(i) + 1, mips[i].rgba.data(), mips[i].w, mips[i].h);
    vkUnmapMemory(m_gpu.device(), stagingMem);

    createImage(w, h, fmt, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img, mem, levels);

    // A single scope for all three: without a batch that is one submit instead of three. The
    // barrier covers ALL levels: an untransitioned level is exactly the
    // layout warning that synchronization validation catches.
    {
        CmdScope scope(m_gpu, batch);
        grabarTransicion(scope.cmd, img, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels);
        vkCmdCopyBufferToImage(scope.cmd, staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               levels, regions.data());
        grabarTransicion(scope.cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, levels);
    }
    // The scope's destructor has already waited if there was no batch, so the
    // staging buffer can be released. With a batch the copy is still in flight and it is released
    // when the fence is signaled.
    if (batch)
        batch->addStaging(staging, stagingMem);
    else
    {
        vkDestroyBuffer(m_gpu.device(), staging, nullptr);
        vkFreeMemory(m_gpu.device(), stagingMem, nullptr);
    }
}

void GpuResources::createBlankImage(uint32_t w, uint32_t h, VkFormat fmt,
                                    VkImage& img, VkDeviceMemory& mem, TransferBatch* batch)
{
    createImage(w, h, fmt, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img, mem);

    CmdScope scope(m_gpu, batch);
    grabarTransicion(scope.cmd, img, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    const VkClearColorValue clear{};   // zeros: transparent
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    vkCmdClearColorImage(scope.cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

    grabarTransicion(scope.cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void GpuResources::uploadPixelsToImageRegions(VkImage img, const ImageTileUpload* tiles,
                                              size_t count, TransferBatch* batch)
{
    if (!tiles || count == 0) return;

    VkDeviceSize total = 0;
    for (size_t i = 0; i < count; ++i)
        total += static_cast<VkDeviceSize>(tiles[i].w) * tiles[i].h * 4;

    VkBuffer       staging    = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    createBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 staging, stagingMem);

    void* data = nullptr;
    vkMapMemory(m_gpu.device(), stagingMem, 0, total, 0, &data);
    std::vector<VkBufferImageCopy> regions(count);
    VkDeviceSize offset = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(tiles[i].w) * tiles[i].h * 4;
        memcpy(static_cast<uint8_t*>(data) + offset, tiles[i].rgba, static_cast<size_t>(bytes));

        VkBufferImageCopy& r = regions[i];
        r.bufferOffset                    = offset;
        r.bufferRowLength                 = 0;
        r.bufferImageHeight               = 0;
        r.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.mipLevel       = 0;
        r.imageSubresource.baseArrayLayer = 0;
        r.imageSubresource.layerCount     = 1;
        r.imageOffset                     = { static_cast<int32_t>(tiles[i].x),
                                              static_cast<int32_t>(tiles[i].y), 0 };
        r.imageExtent                     = { tiles[i].w, tiles[i].h, 1 };
        offset += bytes;
    }
    vkUnmapMemory(m_gpu.device(), stagingMem);

    {
        // A single scope: without a batch, one submit and one wait for the N regions.
        CmdScope scope(m_gpu, batch);
        grabarTransicion(scope.cmd, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(scope.cmd, staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(regions.size()), regions.data());
        grabarTransicion(scope.cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (batch)
        batch->addStaging(staging, stagingMem);
    else
    {
        vkDestroyBuffer(m_gpu.device(), staging, nullptr);
        vkFreeMemory(m_gpu.device(), stagingMem, nullptr);
    }
}

namespace {
void grabarTransicion(VkCommandBuffer cmd, VkImage image,
                      VkImageLayout oldLayout, VkImageLayout newLayout,
                      uint32_t levelCount)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout                       = oldLayout;
    barrier.newLayout                       = newLayout;
    barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.image                           = image;
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel   = 0;
    barrier.subresourceRange.levelCount     = levelCount;
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
    } else if(oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        // Rewriting an image that the GPU may be reading in an in-flight frame:
        // the barrier waits for those reads before allowing the write.
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else {
        throw std::runtime_error("unsupported layout transition!");
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void grabarCopiaABufferImagen(VkCommandBuffer cmd, VkBuffer buffer, VkImage image,
                              uint32_t w, uint32_t h)
{
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
}
} // namespace

void GpuResources::createTextureImage(const std::string& path, const std::vector<uint8_t>& embedded, VkImage& img, VkDeviceMemory& mem, TransferBatch* batch, VkFormat* outFormat)
{
    if (outFormat) *outFormat = VK_FORMAT_R8G8B8A8_SRGB;   // fill-in and shared white
    // Material that does NOT request a texture (a procedural primitive, for example):
    // the shared white one, borrowed. Before, each mesh took its own 1x1
    // white one, with its memory allocation and that of its staging buffer. The case of
    // "it requests one and it could not be read" does NOT come in here: that one keeps its own
    // checkerboard, and it is rare by definition.
    if (path.empty() && embedded.empty())
    {
        static constexpr uint8_t kBlanco[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        ensurePlaceholder(m_whiteSrgb, m_whiteSrgbMem, kBlanco, VK_FORMAT_R8G8B8A8_SRGB);
        img = m_whiteSrgb;
        mem = m_whiteSrgbMem;
        return;
    }

    const DecodedTexture tex = decodeMaterialTexture(path, embedded);
    int w = tex.w, h = tex.h;
    const unsigned char* pixels = tex.pixels.get();

    // Without pixels there are TWO different reasons and until now both ended up as a
    // checkerboard. See PlaceholderTexture.h: a material that requests no texture
    // (a procedural primitive) is filled with white, and only one that requests it and
    // could not be read is marked with the checkerboard.
    std::vector<uint8_t> placeholder;
    if (!pixels) {
        const bool sePidioTextura = !embedded.empty() || !path.empty();
        if (sePidioTextura) {
            placeholder = makeMissingTextureRgba();
            w = h = kMissingTextureSize;
        } else {
            placeholder.assign(4, 0xFF);  // 1x1 white
            w = h = 1;
        }
        pixels = placeholder.data();
    }

    // The format is decided by resolveSrgb (slot + sidecar) ONLY when there is a decoded
    // texture; the fill-ins remain sRGB, as before.
    VkFormat fmt = VK_FORMAT_R8G8B8A8_SRGB;
    if (tex)
        fmt = resolveSrgb(TextureKind::BaseColor, tex.colorSpace) ? VK_FORMAT_R8G8B8A8_SRGB
                                                                   : VK_FORMAT_R8G8B8A8_UNORM;
    if (outFormat) *outFormat = fmt;

    // Copied into the staging buffer in here: `tex` releases the stb ones on exit.
    uploadPixelsToImage(pixels, (uint32_t)w, (uint32_t)h, fmt, img, mem, batch,
                        tex.mips.data(), tex.mips.size());
}

void GpuResources::createNormalMapImage(const std::string& path, const std::vector<uint8_t>& embedded, VkImage& img, VkDeviceMemory& mem, TransferBatch* batch, VkFormat* outFormat)
{
    if (outFormat) *outFormat = VK_FORMAT_R8G8B8A8_UNORM;   // fill-in and shared flat one
    // No normal map: the shared flat one (0,0,1 in tangent space).
    if (path.empty() && embedded.empty())
    {
        static constexpr uint8_t kNormalPlana[4] = {0x80, 0x80, 0xFF, 0xFF};
        ensurePlaceholder(m_flatNormal, m_flatNormalMem, kNormalPlana, VK_FORMAT_R8G8B8A8_UNORM);
        img = m_flatNormal;
        mem = m_flatNormalMem;
        return;
    }

    const DecodedTexture tex = decodeMaterialTexture(path, embedded);
    int w = tex.w, h = tex.h;
    const unsigned char* pixels = tex.pixels.get();

    // Fallback: flat normal (0,0,1) en tangent space = (128,128,255)
    uint8_t flatNormal[4] = { 0x80, 0x80, 0xFF, 0xFF };
    if (!pixels) {
        pixels = flatNormal;
        w = h = 1;
    }

    // The ORM also uses it: resolveSrgb(Normal, o) == resolveSrgb(Orm, o) always.
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    if (tex)
        fmt = resolveSrgb(TextureKind::Normal, tex.colorSpace) ? VK_FORMAT_R8G8B8A8_SRGB
                                                                : VK_FORMAT_R8G8B8A8_UNORM;
    if (outFormat) *outFormat = fmt;

    uploadPixelsToImage(pixels, (uint32_t)w, (uint32_t)h, fmt, img, mem, batch,
                        tex.mips.data(), tex.mips.size());
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
    viewInfo.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;   // 1 or N, whichever the image has
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
    // With maxLod at 0 (the struct's default value) the mips would never be
    // read. Single-level images keep reading level 0.
    samplerInfo.minLod                  = 0.0f;
    samplerInfo.maxLod                  = VK_LOD_CLAMP_NONE;

    if(vkCreateSampler(m_gpu.device(), &samplerInfo, nullptr, &outSampler) != VK_SUCCESS)
        throw std::runtime_error("failed to create texture sampler!");
}

VkSampler GpuResources::sharedMaterialSampler()
{
    // Lazy and not in the constructor: GpuResources is constructed together with the
    // GpuDevice, and at that moment the Vulkan device does not exist yet.
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

// ── Shared fill-in textures ─────────────────────────────────────────────────

void GpuResources::ensurePlaceholder(VkImage& img, VkDeviceMemory& mem,
                                     const uint8_t rgba[4], VkFormat fmt)
{
    if (img != VK_NULL_HANDLE)
        return;

    // Without a batch on purpose: they are 4 bytes and are uploaded ONCE in the whole life of the
    // process. Putting them in the caller's batch would tie them to its fence and
    // force reasoning about who uploads first.
    uploadPixelsToImage(rgba, 1, 1, fmt, img, mem, nullptr);
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
    // Borrowed: it belongs to this GpuResources and is shared by all meshes without a
    // material. Destroying it with the first one would leave the others sampling a
    // freed image, and nothing reveals that until garbage shows up on screen.
    // The three fill-ins are the LAST ones released, so getting here
    // afterwards means the teardown order is wrong. And it is not a detail:
    // isSharedPlaceholder decides by comparing handles against the three members,
    // which destroySharedPlaceholders has just nulled, so the guard
    // below no longer recognizes them and would destroy them a second time without saying a
    // word (H79: that happened with the characters, which were released 15 lines
    // later). Closing in the false case turns a double free (driver state
    // corruption) into a leak at process exit, and it also reports it.
    if (m_placeholdersDestroyed)
    {
        fprintf(stderr, "[GpuResources] releaseMaterialImage(img=%p) after "
                        "destroySharedPlaceholders: the teardown is releasing "
                        "material textures too late. Nothing is destroyed "
                        "(see H79).\n", (void*)img);
        return;
    }

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
    // From here on isSharedPlaceholder can no longer recognize anything: its
    // three handles are VK_NULL_HANDLE. See releaseMaterialImage.
    m_placeholdersDestroyed = true;
}

void GpuResources::createSolidColorImage(const uint8_t rgba[4], VkImage& img, VkDeviceMemory& mem, TransferBatch* batch)
{
    uploadPixelsToImage(rgba, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, img, mem, batch);
}

void GpuResources::createMaterialImageFromPixels(const uint8_t* rgba, uint32_t w, uint32_t h,
                                                 VkFormat fmt,
                                                 const TextureMip* mips, size_t mipCount,
                                                 VkImage& img, VkDeviceMemory& mem,
                                                 TransferBatch* batch)
{
    uploadPixelsToImage(rgba, w, h, fmt, img, mem, batch, mips, mipCount);
}

} // namespace DonTopo
