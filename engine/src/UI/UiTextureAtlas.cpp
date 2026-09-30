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
        // The dot has to come AFTER the last separator: in
        // "v1.2/hoja.png" the first dot belongs to a directory, and cutting there
        // would leave the sidecar somewhere else.
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
            std::printf("[UI] unreadable sprite sidecar: %s\n", jsonPath.c_str());
            return false;
        }

        if (!j.is_object() || !j.contains("sprites") || !j["sprites"].is_object())
        {
            std::printf("[UI] sidecar without a 'sprites' block: %s\n", jsonPath.c_str());
            return false;
        }

        // It is built separately and swapped in at the end: if something blows up midway, the
        // atlas keeps the sprites it already had instead of a truncated
        // list, which would draw the whole image and look like an art bug.
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

            // A rect with zero or negative area gives degenerate UVs and an invisible
            // quad, without a single error anywhere: out.
            if (!(rect.width > 0.0f) || !(rect.height > 0.0f))
            {
                std::printf("[UI] sprite '%s' with an invalid size in %s: ignored\n",
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
        // Sorted by name: this way the file does not change order between
        // saves and the diff shows what was really touched.
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
            std::printf("[UI] unreadable atlas: %s\n", path.c_str());
            if (data) stbi_image_free(data);
            return false;
        }

        // A sprite atlas is COLOR: it goes in sRGB. A font's atlas does not go through
        // here; UiFont bakes it and declares it UNORM.
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
        // The size is read BEFORE uploading anything: without it the UVs would come out of an
        // atlas of 0x0 and every sprite would degenerate to the full rect, which is
        // exactly the failure that gives no error.
        int w = 0, h = 0, channels = 0;
        if (!stbi_info(path.c_str(), &w, &h, &channels) || w <= 0 || h <= 0)
        {
            std::printf("[UI] unreadable atlas: %s\n", path.c_str());
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

        // No batch: this path is deliberately synchronous (the atlas has to be
        // ready before drawing the first glyph), and with the helper it is ONE
        // wait instead of the three it cost to write it here by hand.
        res.uploadPixelsToImage(rgba, width, height, format, m_image, m_memory);

        // The view is declared with the SAME format it was uploaded with: this is where
        // a font atlas stays UNORM.
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
