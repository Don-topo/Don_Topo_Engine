#include "DonTopo/UI/UiTextureAtlas.h"

#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/GpuResources.h"

#include <stb_image.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace DonTopo
{
    const UiSpriteRect* UiTextureAtlas::findSprite(const std::string& name) const
    {
        auto it = m_sprites.find(name);
        return it == m_sprites.end() ? nullptr : &it->second;
    }

    std::vector<std::string> UiTextureAtlas::spriteNames() const
    {
        std::vector<std::string> names;
        names.reserve(m_sprites.size());
        for (const auto& entry : m_sprites) names.push_back(entry.first);
        std::sort(names.begin(), names.end());
        return names;
    }

    std::string UiTextureAtlas::spriteSheetPathFor(const std::string& imagePath)
    {
        // El punto tiene que ser POSTERIOR al último separador: en
        // "v1.2/hoja.png" el primer punto es de un directorio, y cortar por él
        // dejaría el sidecar en otro sitio.
        const size_t slash = imagePath.find_last_of("/\\");
        const size_t dot   = imagePath.find_last_of('.');

        const bool tieneExtension = dot != std::string::npos &&
                                    (slash == std::string::npos || dot > slash);

        const std::string base = tieneExtension ? imagePath.substr(0, dot) : imagePath;
        return base + ".sprites.json";
    }

    bool UiTextureAtlas::loadSprites(const std::string& jsonPath)
    {
        std::ifstream in(jsonPath);
        if (!in) return false;

        nlohmann::json j;
        try
        {
            in >> j;
        }
        catch (const std::exception&)
        {
            std::printf("[UI] sidecar de sprites ilegible: %s\n", jsonPath.c_str());
            return false;
        }

        if (!j.is_object() || !j.contains("sprites") || !j["sprites"].is_object())
        {
            std::printf("[UI] sidecar sin bloque 'sprites': %s\n", jsonPath.c_str());
            return false;
        }

        // Se monta aparte y se cambia al final: si algo revienta a mitad, el
        // atlas se queda con los sprites que ya tenía en vez de con una lista
        // trunca, que dibujaría la imagen entera y parecería un fallo de arte.
        std::unordered_map<std::string, UiSpriteRect> leidos;

        for (const auto& entry : j["sprites"].items())
        {
            const std::string& name = entry.key();
            const auto&        v    = entry.value();
            if (name.empty() || !v.is_object()) continue;

            UiSpriteRect rect{};
            rect.x      = v.value("x", 0.0f);
            rect.y      = v.value("y", 0.0f);
            rect.width  = v.value("w", 0.0f);
            rect.height = v.value("h", 0.0f);

            // Un rect de área nula o negativa da UVs degeneradas y un quad
            // invisible, sin un solo error por ningún lado: fuera.
            if (!(rect.width > 0.0f) || !(rect.height > 0.0f))
            {
                std::printf("[UI] sprite '%s' con tamano invalido en %s: se ignora\n",
                            name.c_str(), jsonPath.c_str());
                continue;
            }

            leidos[name] = rect;
        }

        m_sprites = std::move(leidos);
        return true;
    }

    bool UiTextureAtlas::saveSprites(const std::string& jsonPath) const
    {
        std::error_code ec;
        const std::filesystem::path file(jsonPath);
        if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);

        nlohmann::json sprites = nlohmann::json::object();
        // Ordenados por nombre: así el fichero no cambia de orden entre
        // guardados y el diff enseña lo que de verdad se ha tocado.
        for (const std::string& name : spriteNames())
        {
            const UiSpriteRect& r = m_sprites.at(name);
            sprites[name] = { {"x", r.x}, {"y", r.y}, {"w", r.width}, {"h", r.height} };
        }

        nlohmann::json j;
        j["version"] = 1;
        j["sprites"] = std::move(sprites);

        std::ofstream out(jsonPath, std::ios::trunc);
        if (!out) return false;
        out << j.dump(2) << "\n";
        return (bool)out;
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
