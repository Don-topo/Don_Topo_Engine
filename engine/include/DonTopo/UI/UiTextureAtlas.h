#pragma once

// Sprite atlas for the game UI. The CPU part (atlas size and
// sub-rects in pixels) does not touch Vulkan: it is what the tests exercise and what
// converts a sub-rect into normalized UVs. The GPU part (image, view and
// descriptor set) is filled by the Renderer and UiSpriteBatch and stays at VK_NULL_HANDLE
// as long as nobody loads anything from disk.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace DonTopo
{
    class GpuDevice;
    class GpuResources;

    // Sub-rect of a sprite INSIDE the atlas, in pixels and with the origin at the top
    // left (same convention as the canvas).
    struct UiSpriteRect
    {
        float x = 0.0f;
        float y = 0.0f;
        float width  = 0.0f;
        float height = 0.0f;
    };

    // The same rect already normalized. v0 is the TOP edge: the texture is
    // sampled with V growing downward, just like the screen.
    struct UiUvRect
    {
        float u0 = 0.0f;
        float v0 = 0.0f;
        float u1 = 1.0f;
        float v1 = 1.0f;
    };

    class UiTextureAtlas
    {
    public:
        UiTextureAtlas() = default;

        // --- CPU ---------------------------------------------------------------
        void setSize(uint32_t width, uint32_t height) { m_width = width; m_height = height; }
        uint32_t width()  const { return m_width; }
        uint32_t height() const { return m_height; }

        void addSprite(const std::string& name, const UiSpriteRect& rect) { m_sprites[name] = rect; }
        bool hasSprite(const std::string& name) const { return m_sprites.count(name) != 0; }
        const UiSpriteRect* findSprite(const std::string& name) const;
        bool removeSprite(const std::string& name) { return m_sprites.erase(name) != 0; }
        void clearSprites() { m_sprites.clear(); }
        size_t spriteCount() const { return m_sprites.size(); }

        // The registered names, SORTED alphabetically. The map has no
        // order and the editor draws them in a combo: a list that dances from one
        // frame to the next is unusable, and the selected index would stop
        // meaning the same thing between two frames.
        std::vector<std::string> spriteNames() const;

        // --- Sprite sidecar ----------------------------------------------------
        // The sub-rects live NEXT TO the image and not inside the scene: an
        // atlas sliced once serves every widget that uses it, and two different
        // scenes cannot disagree about where each sprite is.
        //
        // "assets/ui/botones.png" -> "assets/ui/botones.sprites.json".
        static std::string spriteSheetPathFor(const std::string& imagePath);

        // Loads (and REPLACES) the atlas sprites. Without a file, with broken JSON
        // or with a root that is not the expected one it returns false and does NOT touch those
        // that already existed: half a list is worse than none, because the widget draws
        // the whole atlas instead of its sprite and it looks like an art problem.
        // Loose entries that are not valid (no name, or of area 0) are skipped.
        bool loadSprites(const std::string& jsonPath);
        // Writes the whole sidecar, creating any missing directories.
        bool saveSprites(const std::string& jsonPath) const;

        // UVs of the sprite. Without a sprite (or with an atlas of size 0) it returns the
        // full rect: a loose texture is used the same as a one-sprite atlas,
        // without registering anything.
        UiUvRect uvRect(const std::string& name) const;

        // --- GPU ---------------------------------------------------------------
        // The size in pixels comes from the image itself (stbi_info), so the
        // UVs do not depend on anyone declaring it by hand. Sprites are added
        // afterwards with addSprite.
        bool loadFromFile(GpuDevice& gpu, GpuResources& res, const std::string& path);

        // The same atlas but from RGBA8 pixels ALREADY in memory: this is what
        // a font needs, whose atlas is baked on the fly and never exists
        // as a file. The format is explicit and there is NO default value
        // that is safe for everyone: an MSDF is distances and in SRGB they come out distorted
        // without validation saying a word.
        bool loadFromPixels(GpuDevice& gpu, GpuResources& res, const uint8_t* rgba,
                            uint32_t width, uint32_t height, VkFormat format);

        void destroy(GpuDevice& gpu);

        // --- Source data, without a graphics API -------------------------------
        // The pixels it was loaded with, so that ANY backend can
        // upload them: the Vulkan one does it in loadFromFile/loadFromPixels and the
        // DirectX 12 one reads them from here. They are always stored because a font's atlas
        // is baked on the fly and does not exist as a file to go back to.
        const std::vector<uint8_t>& sourcePixels() const { return m_pixels; }
        // true = the content is COLOR and goes in an sRGB view. false = it is
        // distances (MSDF) and any conversion distorts them without warning.
        bool sourceIsSrgb() const { return m_srgb; }

        // Loads the file's pixels and the size, without touching the GPU. It is what
        // a backend that uploads on its own uses.
        bool loadPixelsFromFile(const std::string& path);
        // The already-baked pixels (a font). It keeps a copy.
        void setSourcePixels(const uint8_t* rgba, uint32_t width, uint32_t height, bool srgb);

        VkImageView     view()          const { return m_view; }
        VkDescriptorSet descriptorSet() const { return m_descriptorSet; }
        void setDescriptorSet(VkDescriptorSet set) { m_descriptorSet = set; }
        bool loaded() const { return m_view != VK_NULL_HANDLE; }

    private:
        uint32_t m_width  = 0;
        uint32_t m_height = 0;
        std::unordered_map<std::string, UiSpriteRect> m_sprites;

        // RGBA8, as loaded or baked. See sourcePixels().
        std::vector<uint8_t> m_pixels;
        bool                 m_srgb = true;

        VkImage         m_image         = VK_NULL_HANDLE;
        VkDeviceMemory  m_memory        = VK_NULL_HANDLE;
        VkImageView     m_view          = VK_NULL_HANDLE;
        // The owner is the UiSpriteBatch pool; here only the handle is stored.
        VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    };
}
