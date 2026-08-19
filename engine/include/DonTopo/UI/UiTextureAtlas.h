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
#include <vector>

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
        bool removeSprite(const std::string& name) { return m_sprites.erase(name) != 0; }
        void clearSprites() { m_sprites.clear(); }
        size_t spriteCount() const { return m_sprites.size(); }

        // Los nombres registrados, ORDENADOS alfabéticamente. El mapa no tiene
        // orden y el editor los pinta en un combo: una lista que baila de un
        // frame a otro es inusable, y el índice seleccionado dejaría de
        // significar lo mismo entre dos frames.
        std::vector<std::string> spriteNames() const;

        // --- Sidecar de sprites ------------------------------------------------
        // Los sub-rects viven JUNTO a la imagen y no dentro de la escena: un
        // atlas troceado una vez sirve para todos los widgets que lo usen, y dos
        // escenas distintas no pueden discrepar sobre dónde está cada sprite.
        //
        // "assets/ui/botones.png" -> "assets/ui/botones.sprites.json".
        static std::string spriteSheetPathFor(const std::string& imagePath);

        // Carga (y REEMPLAZA) los sprites del atlas. Sin fichero, con JSON roto
        // o con una raíz que no es la esperada devuelve false y NO toca los que
        // ya hubiera: media lista es peor que ninguna, porque el widget dibuja
        // el atlas entero en vez de su sprite y parece un problema de arte.
        // Las entradas sueltas que no valen (sin nombre, o de área 0) se saltan.
        bool loadSprites(const std::string& jsonPath);
        // Escribe el sidecar entero, creando los directorios que falten.
        bool saveSprites(const std::string& jsonPath) const;

        // UVs del sprite. Sin sprite (o con un atlas de tamaño 0) devuelve el
        // rect completo: una textura suelta se usa igual que un atlas de un
        // sprite, sin registrar nada.
        UiUvRect uvRect(const std::string& name) const;

        // --- GPU ---------------------------------------------------------------
        // El tamaño en píxeles sale de la propia imagen (stbi_info), así que las
        // UVs no dependen de que nadie lo declare a mano. Los sprites se añaden
        // después con addSprite.
        bool loadFromFile(GpuDevice& gpu, GpuResources& res, const std::string& path);

        // El mismo atlas pero desde píxeles RGBA8 YA en memoria: es lo que
        // necesita una fuente, cuyo atlas se hornea en caliente y nunca existe
        // como fichero. El formato es explícito y NO tiene un valor por defecto
        // seguro para todos: un MSDF son distancias y en SRGB salen deformadas
        // sin que la validación diga una palabra.
        bool loadFromPixels(GpuDevice& gpu, GpuResources& res, const uint8_t* rgba,
                            uint32_t width, uint32_t height, VkFormat format);

        void destroy(GpuDevice& gpu);

        // --- Datos de origen, sin API gráfica ----------------------------------
        // Los píxeles con los que se cargó, para que CUALQUIER backend pueda
        // subirlos: el de Vulkan lo hace en loadFromFile/loadFromPixels y el de
        // DirectX 12 los lee de aquí. Se guardan siempre porque el atlas de una
        // fuente se hornea en caliente y no existe como fichero al que volver.
        const std::vector<uint8_t>& sourcePixels() const { return m_pixels; }
        // true = el contenido es COLOR y va en una vista sRGB. false = son
        // distancias (MSDF) y cualquier conversión las deforma sin avisar.
        bool sourceIsSrgb() const { return m_srgb; }

        // Carga los píxeles del fichero y el tamaño, sin tocar la GPU. Es lo que
        // usa un backend que sube por su cuenta.
        bool loadPixelsFromFile(const std::string& path);
        // Los píxeles ya horneados (una fuente). Se queda con una copia.
        void setSourcePixels(const uint8_t* rgba, uint32_t width, uint32_t height, bool srgb);

        VkImageView     view()          const { return m_view; }
        VkDescriptorSet descriptorSet() const { return m_descriptorSet; }
        void setDescriptorSet(VkDescriptorSet set) { m_descriptorSet = set; }
        bool loaded() const { return m_view != VK_NULL_HANDLE; }

    private:
        uint32_t m_width  = 0;
        uint32_t m_height = 0;
        std::unordered_map<std::string, UiSpriteRect> m_sprites;

        // RGBA8, tal cual se cargó o se horneó. Ver sourcePixels().
        std::vector<uint8_t> m_pixels;
        bool                 m_srgb = true;

        VkImage         m_image         = VK_NULL_HANDLE;
        VkDeviceMemory  m_memory        = VK_NULL_HANDLE;
        VkImageView     m_view          = VK_NULL_HANDLE;
        // Lo dueño es el pool de UiSpriteBatch; aquí solo se guarda el handle.
        VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    };
}
