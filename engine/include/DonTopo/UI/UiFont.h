#pragma once

// Fuente de la UI de juego: un atlas MSDF horneado desde un TTF.
//
// Un MSDF guarda la DISTANCIA al contorno en tres canales, no el color del
// glyph. Por eso el texto se ve nítido a cualquier tamaño sin rehornear nada:
// el atlas se hornea UNA vez a bakeSize() y el tamaño final sale de escalar el
// quad; el shader reconstruye el borde con la mediana de los tres canales.
//
// Dos mitades, igual que UiTextureAtlas:
//   - CPU pura: métricas por glyph, kerning y el escalado a un fontSize dado.
//     No toca ni FreeType ni Vulkan, se puede rellenar a mano y es lo que
//     ejercitan los tests.
//   - GPU: loadFromFile() abre el TTF, hornea el atlas y lo sube.
//
// La fuente CONTIENE su UiTextureAtlas en vez de heredar de él: así el
// registro de descriptor (UiSpriteBatch::registerAtlas) y el agrupado por
// atlas del batcher funcionan tal cual, sin una segunda ruta para el texto.

#include "DonTopo/UI/UiTextureAtlas.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace DonTopo
{
    class GpuDevice;
    class GpuResources;

    // Todo en PÍXELES DEL TAMAÑO DE HORNEADO (bakeSize). Quien dibuja escala
    // por fontSize/bakeSize.
    struct UiGlyph
    {
        // Área del glyph DENTRO del atlas, en píxeles. Incluye el margen que
        // necesita el campo de distancia: el quad se dibuja con este tamaño,
        // no con el del contorno.
        UiSpriteRect rect{};

        // Del cursor (pluma) al borde IZQUIERDO del quad, +X a la derecha.
        float bearingX = 0.0f;
        // De la línea base al borde SUPERIOR del quad, +Y hacia ARRIBA. Ojo:
        // el canvas tiene +Y hacia abajo, así que el batcher lo RESTA.
        float bearingY = 0.0f;
        // Cuánto avanza el cursor tras este glyph. No tiene por qué parecerse
        // ni al ancho del rect ni al bearing.
        float advance = 0.0f;
    };

    class UiFont
    {
    public:
        UiFont() = default;

        // --- CPU ---------------------------------------------------------------

        // Tamaño al que se horneó el atlas. Es el denominador de todo escalado:
        // a 0 la fuente no dibuja nada en vez de dividir por cero.
        void  setBakeSize(float px) { m_bakeSize = px; }
        float bakeSize() const { return m_bakeSize; }

        // Anchura, en píxeles del atlas, de la banda donde el campo de
        // distancia es válido. Es lo que el shader necesita para saber cuántos
        // píxeles de pantalla cubre el borde.
        void  setPixelRange(float px) { m_pixelRange = px; }
        float pixelRange() const { return m_pixelRange; }

        void  setMetrics(float ascent, float descent, float lineHeight);
        float ascent() const { return m_ascent; }
        float descent() const { return m_descent; }
        float lineHeight() const { return m_lineHeight; }

        void addGlyph(uint32_t codepoint, const UiGlyph& glyph) { m_glyphs[codepoint] = glyph; }
        const UiGlyph* findGlyph(uint32_t codepoint) const;

        // Corrección ENTRE dos glyphs consecutivos, en píxeles de horneado.
        // Suele ser negativa (acerca el par). Un par sin entrada vale 0.
        void  setKerning(uint32_t left, uint32_t right, float amount);
        float kerning(uint32_t left, uint32_t right) const;

        // UVs del glyph a partir de su rect y del tamaño del atlas. No pasa por
        // los sprites con nombre de UiTextureAtlas: un glyph no tiene nombre.
        UiUvRect glyphUv(const UiGlyph& glyph) const;

        // Factor por el que hay que multiplicar TODAS las métricas para dibujar
        // a fontSize. Con bakeSize a 0 devuelve 0.
        float scaleFor(float fontSize) const;

        UiTextureAtlas&       atlas()       { return m_atlas; }
        const UiTextureAtlas& atlas() const { return m_atlas; }

        // Una fuente sin glyphs no dibuja: el batcher sale antes de recorrer
        // nada. Que el atlas esté o no subido a la GPU es otra cosa (los tests
        // van sin Vulkan).
        bool hasGlyphs() const { return !m_glyphs.empty(); }

        // UTF-8 -> codepoints. Los bytes inválidos dan U+FFFD en vez de
        // desincronizar el resto de la cadena.
        static std::vector<uint32_t> decodeUtf8(const std::string& text);
        // Misma decodificacion sobre un vector REUTILIZADO: el batcher pasa por
        // aqui cada frame, y la version que devuelve por valor asignaria uno
        // nuevo por cada texto del canvas.
        static void decodeUtf8(const std::string& text, std::vector<uint32_t>& out);

        // --- GPU ---------------------------------------------------------------

        // Hornea [firstCodepoint, lastCodepoint] del TTF a un atlas MSDF y lo
        // sube. El atlas queda UNORM: un MSDF son distancias, y una vista SRGB
        // las deforma sin dar ni un error de validación.
        bool loadFromFile(GpuDevice& gpu, GpuResources& res, const std::string& path,
                          float bakePx = 48.0f,
                          uint32_t firstCodepoint = 32, uint32_t lastCodepoint = 126);

        void destroy(GpuDevice& gpu) { m_atlas.destroy(gpu); }

    private:
        UiTextureAtlas m_atlas;

        float m_bakeSize   = 0.0f;
        float m_pixelRange = 4.0f;
        float m_ascent     = 0.0f;
        float m_descent    = 0.0f;
        float m_lineHeight = 0.0f;

        std::unordered_map<uint32_t, UiGlyph> m_glyphs;
        // Clave = (izquierdo << 32) | derecho: un solo mapa en vez de un mapa
        // de mapas, y el par ausente ni se guarda.
        std::unordered_map<uint64_t, float> m_kerning;
    };
}
