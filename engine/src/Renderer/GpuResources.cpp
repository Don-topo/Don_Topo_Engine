#include "DonTopo/Renderer/GpuResources.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/TransferBatch.h"
#include "DonTopo/Renderer/PlaceholderTexture.h"
#include <stdexcept>
#include <cstring>
#include <string>
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

namespace {
    // VK_ERROR_TOO_MANY_OBJECTS es el tope de asignaciones VIVAS del device, y
    // el mensaje generico ("failed to allocate buffer memory") apunta al sitio
    // equivocado: parece falta de VRAM cuando lo que falta son ranuras. Un
    // motor que pide una asignacion por recurso lo alcanza con una escena
    // grande en una GPU que se quede en el minimo de la spec (H72).
    std::string mensajeDeAsignacion(const char* que, VkResult r, uint32_t maxAllocs)
    {
        std::string m = std::string("failed to allocate ") + que + " memory";
        if (r == VK_ERROR_TOO_MANY_OBJECTS)
            m += ": alcanzado el tope de " + std::to_string(maxAllocs) +
                 " asignaciones de memoria de esta GPU (unas " +
                 std::to_string(maxAllocs / 2) +
                 " mallas). No es falta de VRAM: es el numero de asignaciones,"
                 " y el motor pide una por recurso";
        else if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY)
            m += ": la GPU se ha quedado sin memoria";
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

    if(const VkResult r = vkAllocateMemory(m_gpu.device(), &allocInfo, nullptr, &memory);
       r != VK_SUCCESS)
        throw std::runtime_error(mensajeDeAsignacion("image", r, m_gpu.maxMemoryAllocations()));

    vkBindImageMemory(m_gpu.device(), image, memory, 0);
}

namespace {
    // Las dos operaciones de imagen, grabadas en un command buffer que ya está
    // abierto. Existen aparte de los métodos públicos porque uploadPixelsToImage
    // mete las TRES —transición, copia, transición— en el mismo buffer: por los
    // métodos serían tres submits y tres esperas para el mismo trabajo.
    void grabarTransicion(VkCommandBuffer cmd, VkImage image,
                          VkImageLayout oldLayout, VkImageLayout newLayout);
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
                                       VkImage& img, VkDeviceMemory& mem, TransferBatch* batch)
{
    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(w) * h * 4;

    VkBuffer       staging    = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 staging, stagingMem);

    void* data = nullptr;
    vkMapMemory(m_gpu.device(), stagingMem, 0, imageSize, 0, &data);
    memcpy(data, pixels, static_cast<size_t>(imageSize));
    vkUnmapMemory(m_gpu.device(), stagingMem);

    createImage(w, h, fmt, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img, mem);

    // Un solo scope para las tres: sin batch eso es un submit en vez de tres.
    {
        CmdScope scope(m_gpu, batch);
        grabarTransicion(scope.cmd, img, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        grabarCopiaABufferImagen(scope.cmd, staging, img, w, h);
        grabarTransicion(scope.cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    // El destructor del scope ya ha esperado si no habia batch, asi que el
    // staging se puede soltar. Con batch la copia sigue en vuelo y se libera al
    // senalar la fence.
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
                      VkImageLayout oldLayout, VkImageLayout newLayout)
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

    uploadPixelsToImage(pixels, (uint32_t)w, (uint32_t)h, VK_FORMAT_R8G8B8A8_SRGB, img, mem, batch);
    // Copiados ya al staging: stb puede soltarlos.
    if (fromStb) stbi_image_free(pixels);
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

    uploadPixelsToImage(pixels, (uint32_t)w, (uint32_t)h, VK_FORMAT_R8G8B8A8_UNORM, img, mem, batch);
    if (fromStb) stbi_image_free(pixels);
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
    // Prestada: es de este GpuResources y la comparten todas las mallas sin
    // material. Destruirla con la primera dejaria a las demas muestreando una
    // imagen liberada, y eso no lo delata nada hasta que se ve basura.
    // Las tres de relleno son las ULTIMAS que se sueltan, asi que llegar aqui
    // despues significa que el orden del teardown esta mal. Y no es un detalle:
    // isSharedPlaceholder decide comparando handles contra los tres miembros,
    // que destroySharedPlaceholders acaba de anular, o sea que la guarda de
    // abajo ya no los reconoce y los destruiria por segunda vez sin decir una
    // palabra (H79: eso pasaba con los personajes, que se soltaban 15 lineas
    // despues). Cerrar en falso convierte un doble free —corrupcion de estado
    // del driver— en una fuga al salir del proceso, y encima lo dice.
    if (m_placeholdersDestroyed)
    {
        fprintf(stderr, "[GpuResources] releaseMaterialImage(img=%p) despues de "
                        "destroySharedPlaceholders: el teardown esta soltando "
                        "texturas de material demasiado tarde. No se destruye "
                        "nada (ver H79).\n", (void*)img);
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
    // A partir de aqui isSharedPlaceholder ya no puede reconocer a nadie: sus
    // tres handles son VK_NULL_HANDLE. Ver releaseMaterialImage.
    m_placeholdersDestroyed = true;
}

void GpuResources::createSolidColorImage(const uint8_t rgba[4], VkImage& img, VkDeviceMemory& mem, TransferBatch* batch)
{
    uploadPixelsToImage(rgba, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, img, mem, batch);
}

void GpuResources::createTextureImageFromPixels(const uint8_t* rgba, uint32_t w, uint32_t h,
                                                VkImage& img, VkDeviceMemory& mem,
                                                TransferBatch* batch)
{
    uploadPixelsToImage(rgba, w, h, VK_FORMAT_R8G8B8A8_SRGB, img, mem, batch);
}

void GpuResources::createNormalMapImageFromPixels(const uint8_t* rgba, uint32_t w, uint32_t h,
                                                  VkImage& img, VkDeviceMemory& mem,
                                                  TransferBatch* batch)
{
    uploadPixelsToImage(rgba, w, h, VK_FORMAT_R8G8B8A8_UNORM, img, mem, batch);
}

} // namespace DonTopo
