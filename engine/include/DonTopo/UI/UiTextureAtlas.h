#pragma once

// Atlas de sprites para la UI de juego. La parte CPU (tamaño del atlas y
// sub-rects en píxeles) no toca Vulkan: es la que ejercitan los tests y la que
// convierte un sub-rect en UVs normalizadas. La parte GPU (imagen, vista y
// descriptor set) la rellenan el Renderer y UiSpriteBatch y queda a VK_NULL_HANDLE
// mientras nadie cargue nada desde disco.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace DonTopo
{
    class GpuDevice;
    class GpuResources;

    // Sub-rect de un sprite DENTRO del atlas, en píxeles y con el origen arriba
    // a la izquierda (misma convención que el canvas).
    struct UiSpriteRect
    {
        float x = 0.0f;
        float y = 0.0f;
        float width  = 0.0f;
        float height = 0.0f;
    };

    // El mismo rect ya normalizado. v0 es el borde SUPERIOR: la textura se
    // muestrea con V creciendo hacia abajo, igual que la pantalla.
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

        // UVs del sprite. Sin sprite (o con un atlas de tamaño 0) devuelve el
        // rect completo: una textura suelta se usa igual que un atlas de un
        // sprite, sin registrar nada.
        UiUvRect uvRect(const std::string& name) const;

        // --- GPU ---------------------------------------------------------------
        // El tamaño en píxeles sale de la propia imagen (stbi_info), así que las
        // UVs no dependen de que nadie lo declare a mano. Los sprites se añaden
        // después con addSprite.
        bool loadFromFile(GpuDevice& gpu, GpuResources& res, const std::string& path);
        void destroy(GpuDevice& gpu);

        VkImageView     view()          const { return m_view; }
        VkDescriptorSet descriptorSet() const { return m_descriptorSet; }
        void setDescriptorSet(VkDescriptorSet set) { m_descriptorSet = set; }
        bool loaded() const { return m_view != VK_NULL_HANDLE; }

    private:
        uint32_t m_width  = 0;
        uint32_t m_height = 0;
        std::unordered_map<std::string, UiSpriteRect> m_sprites;

        VkImage         m_image         = VK_NULL_HANDLE;
        VkDeviceMemory  m_memory        = VK_NULL_HANDLE;
        VkImageView     m_view          = VK_NULL_HANDLE;
        // Lo dueño es el pool de UiSpriteBatch; aquí solo se guarda el handle.
        VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    };
}
