#include "DonTopo/UI/UiTextureAtlas.h"

#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/GpuResources.h"

#include <stb_image.h>

#include <cstdio>

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
