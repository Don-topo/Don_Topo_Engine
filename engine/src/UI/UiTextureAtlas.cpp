#include "DonTopo/UI/UiTextureAtlas.h"

#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/GpuResources.h"

#include <stb_image.h>

#include <cstdio>
#include <cstring>

namespace DonTopo
{
    const UiSpriteRect* UiTextureAtlas::findSprite(const std::string& name) const
    {
        auto it = m_sprites.find(name);
        return it == m_sprites.end() ? nullptr : &it->second;
    }

    UiUvRect UiTextureAtlas::uvRect(const std::string& name) const
    {
        if (m_width == 0 || m_height == 0) return {};

        const UiSpriteRect* rect = findSprite(name);
        if (!rect) return {};

        const float invW = 1.0f / (float)m_width;
        const float invH = 1.0f / (float)m_height;

        UiUvRect uv{};
        uv.u0 = rect->x * invW;
        uv.v0 = rect->y * invH;
        uv.u1 = (rect->x + rect->width)  * invW;
        uv.v1 = (rect->y + rect->height) * invH;
        return uv;
    }

    bool UiTextureAtlas::loadPixelsFromFile(const std::string& path)
    {
        int      w = 0, h = 0, channels = 0;
        stbi_uc* data = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
        if (!data || w <= 0 || h <= 0)
        {
            std::printf("[UI] atlas ilegible: %s\n", path.c_str());
            if (data) stbi_image_free(data);
            return false;
        }

        // Un atlas de sprites es COLOR: va en sRGB. El de una fuente no pasa por
        // aquí, lo hornea UiFont y lo declara UNORM.
        setSourcePixels(data, (uint32_t)w, (uint32_t)h, /*srgb=*/true);
        stbi_image_free(data);
        return true;
    }

    void UiTextureAtlas::setSourcePixels(const uint8_t* rgba, uint32_t width, uint32_t height,
                                         bool srgb)
    {
        if (!rgba || width == 0 || height == 0) return;
        m_pixels.assign(rgba, rgba + (size_t)width * height * 4);
        m_srgb = srgb;
        setSize(width, height);
    }

    bool UiTextureAtlas::loadFromFile(GpuDevice& gpu, GpuResources& res, const std::string& path)
    {
        // El tamaño se lee ANTES de subir nada: sin él las UVs saldrían de un
        // atlas de 0x0 y todos los sprites degenerarían al rect completo, que es
        // exactamente el fallo que no da ningún error.
        int w = 0, h = 0, channels = 0;
        if (!stbi_info(path.c_str(), &w, &h, &channels) || w <= 0 || h <= 0)
        {
            std::printf("[UI] atlas ilegible: %s\n", path.c_str());
            return false;
        }

        destroy(gpu);

        res.createTextureImage(path, {}, m_image, m_memory);
        res.createTextureImageView(m_image, m_view);
        setSize((uint32_t)w, (uint32_t)h);
        return true;
    }

    bool UiTextureAtlas::loadFromPixels(GpuDevice& gpu, GpuResources& res, const uint8_t* rgba,
                                        uint32_t width, uint32_t height, VkFormat format)
    {
        if (!rgba || width == 0 || height == 0) return false;

        destroy(gpu);

        const VkDeviceSize bytes = (VkDeviceSize)width * height * 4;

        VkBuffer       staging       = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        res.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         staging, stagingMemory);

        void* mapped = nullptr;
        vkMapMemory(gpu.device(), stagingMemory, 0, bytes, 0, &mapped);
        std::memcpy(mapped, rgba, (size_t)bytes);
        vkUnmapMemory(gpu.device(), stagingMemory);

        res.createImage(width, height, format, VK_IMAGE_TILING_OPTIMAL,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_image, m_memory);

        res.transitionImageLayout(m_image, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        res.copyBufferToImage(staging, m_image, width, height);
        res.transitionImageLayout(m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        vkDestroyBuffer(gpu.device(), staging, nullptr);
        vkFreeMemory(gpu.device(), stagingMemory, nullptr);

        // La vista se declara con el MISMO formato con el que se subió: es aquí
        // donde un atlas de fuente se queda en UNORM.
        res.createTextureImageView(m_image, m_view, format);
        setSize(width, height);
        return true;
    }

    void UiTextureAtlas::destroy(GpuDevice& gpu)
    {
        if (m_view != VK_NULL_HANDLE)   vkDestroyImageView(gpu.device(), m_view, nullptr);
        if (m_image != VK_NULL_HANDLE)  vkDestroyImage(gpu.device(), m_image, nullptr);
        if (m_memory != VK_NULL_HANDLE) vkFreeMemory(gpu.device(), m_memory, nullptr);
        m_view          = VK_NULL_HANDLE;
        m_image         = VK_NULL_HANDLE;
        m_memory        = VK_NULL_HANDLE;
        m_descriptorSet = VK_NULL_HANDLE;
    }
}
