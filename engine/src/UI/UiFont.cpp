#include "DonTopo/UI/UiFont.h"

#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/GpuResources.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <msdfgen.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace DonTopo
{
    namespace
    {
        uint64_t kerningKey(uint32_t left, uint32_t right)
        {
            return ((uint64_t)left << 32) | (uint64_t)right;
        }

        // FreeType da las coordenadas en 26.6 (1/64 de píxel) una vez fijado el
        // tamaño en píxeles, así que dividir por 64 las deja YA en píxeles de
        // horneado: las métricas y el contorno salen en la misma unidad.
        constexpr double kFt26_6 = 1.0 / 64.0;

        // ── Contorno de FreeType -> Shape de msdfgen ────────────────────────
        // Es lo único que aportaba la mitad "ext" de msdfgen, que aquí no se
        // compila porque busca FreeType en el sistema.
        struct OutlineSink
        {
            msdfgen::Shape*   shape   = nullptr;
            msdfgen::Contour* contour = nullptr;
            msdfgen::Point2   current{};
        };

        msdfgen::Point2 toPoint(const FT_Vector* v)
        {
            return msdfgen::Point2((double)v->x * kFt26_6, (double)v->y * kFt26_6);
        }

        int outlineMoveTo(const FT_Vector* to, void* user)
        {
            OutlineSink* sink = (OutlineSink*)user;
            // Un contorno de un solo punto no aporta ni una arista: msdfgen lo
            // trataría como un contorno vacío y edgeColoringSimple lo cuenta.
            if (!(sink->contour && sink->contour->edges.empty()))
                sink->contour = &sink->shape->addContour();
            sink->current = toPoint(to);
            return 0;
        }

        int outlineLineTo(const FT_Vector* to, void* user)
        {
            OutlineSink* sink = (OutlineSink*)user;
            const msdfgen::Point2 next = toPoint(to);
            if (next != sink->current)
            {
                sink->contour->addEdge(msdfgen::EdgeHolder(sink->current, next));
                sink->current = next;
            }
            return 0;
        }

        int outlineConicTo(const FT_Vector* control, const FT_Vector* to, void* user)
        {
            OutlineSink* sink = (OutlineSink*)user;
            const msdfgen::Point2 next = toPoint(to);
            if (next != sink->current)
            {
                sink->contour->addEdge(msdfgen::EdgeHolder(sink->current, toPoint(control), next));
                sink->current = next;
            }
            return 0;
        }

        int outlineCubicTo(const FT_Vector* c1, const FT_Vector* c2, const FT_Vector* to, void* user)
        {
            OutlineSink* sink = (OutlineSink*)user;
            const msdfgen::Point2 next = toPoint(to);
            if (next != sink->current)
            {
                sink->contour->addEdge(
                    msdfgen::EdgeHolder(sink->current, toPoint(c1), toPoint(c2), next));
                sink->current = next;
            }
            return 0;
        }

        // El mismo redondeo que usa msdfgen al volcar a 8 bits.
        uint8_t toByte(float v)
        {
            const int i = (int)(v * 256.0f);
            return (uint8_t)std::min(std::max(i, 0), 255);
        }

        struct BakedGlyph
        {
            uint32_t       codepoint = 0;
            msdfgen::Shape shape;
            bool     hasShape = false;
            uint32_t width    = 0;   // tamaño del bitmap MSDF, ya con el margen
            uint32_t height   = 0;
            double   translateX = 0.0;   // lleva el contorno dentro del bitmap
            double   translateY = 0.0;
            UiGlyph  metrics{};
        };
    }

    void UiFont::setMetrics(float ascent, float descent, float lineHeight)
    {
        m_ascent     = ascent;
        m_descent    = descent;
        m_lineHeight = lineHeight;
    }

    const UiGlyph* UiFont::findGlyph(uint32_t codepoint) const
    {
        auto it = m_glyphs.find(codepoint);
        return it == m_glyphs.end() ? nullptr : &it->second;
    }

    void UiFont::setKerning(uint32_t left, uint32_t right, float amount)
    {
        m_kerning[kerningKey(left, right)] = amount;
    }

    float UiFont::kerning(uint32_t left, uint32_t right) const
    {
        auto it = m_kerning.find(kerningKey(left, right));
        return it == m_kerning.end() ? 0.0f : it->second;
    }

    UiUvRect UiFont::glyphUv(const UiGlyph& glyph) const
    {
        const uint32_t w = m_atlas.width();
        const uint32_t h = m_atlas.height();
        if (w == 0 || h == 0) return {};

        const float invW = 1.0f / (float)w;
        const float invH = 1.0f / (float)h;

        UiUvRect uv{};
        uv.u0 = glyph.rect.x * invW;
        uv.v0 = glyph.rect.y * invH;
        uv.u1 = (glyph.rect.x + glyph.rect.width)  * invW;
        uv.v1 = (glyph.rect.y + glyph.rect.height) * invH;
        return uv;
    }

    float UiFont::scaleFor(float fontSize) const
    {
        if (m_bakeSize <= 0.0f) return 0.0f;
        return fontSize / m_bakeSize;
    }

    void UiFont::decodeUtf8(const std::string& text, std::vector<uint32_t>& out)
    {
        // El vector es de quien llama y se REUTILIZA: el batcher decodifica cada
        // texto en cada frame y no puede permitirse una asignacion por etiqueta.
        out.clear();
        out.reserve(text.size());

        const unsigned char* p   = (const unsigned char*)text.data();
        const unsigned char* end = p + text.size();

        while (p < end)
        {
            const unsigned char c = *p;
            uint32_t cp    = 0;
            int      extra = 0;

            if (c < 0x80)            { cp = c;        extra = 0; }
            else if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; extra = 1; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; extra = 2; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; extra = 3; }
            else
            {
                // Byte de continuación suelto o cabecera inválida: se consume
                // UNO y se sigue, que es lo que evita que el resto de la cadena
                // se desplace.
                out.push_back(0xFFFDu);
                ++p;
                continue;
            }

            if (p + extra >= end)
            {
                out.push_back(0xFFFDu);
                break;
            }

            bool ok = true;
            for (int i = 1; i <= extra; ++i)
            {
                const unsigned char cc = p[i];
                if ((cc & 0xC0) != 0x80) { ok = false; break; }
                cp = (cp << 6) | (uint32_t)(cc & 0x3Fu);
            }

            if (!ok)
            {
                out.push_back(0xFFFDu);
                ++p;
                continue;
            }

            out.push_back(cp);
            p += extra + 1;
        }
    }

    std::vector<uint32_t> UiFont::decodeUtf8(const std::string& text)
    {
        std::vector<uint32_t> out;
        decodeUtf8(text, out);
        return out;
    }

    bool UiFont::loadFromFile(GpuDevice& gpu, GpuResources& res, const std::string& path,
                              float bakePx, uint32_t firstCodepoint, uint32_t lastCodepoint)
    {
        if (bakePx <= 0.0f || lastCodepoint < firstCodepoint) return false;

        FT_Library library = nullptr;
        if (FT_Init_FreeType(&library) != 0)
        {
            std::printf("[UI] FreeType no arranca\n");
            return false;
        }

        FT_Face face = nullptr;
        if (FT_New_Face(library, path.c_str(), 0, &face) != 0)
        {
            std::printf("[UI] fuente ilegible: %s\n", path.c_str());
            FT_Done_FreeType(library);
            return false;
        }

        FT_Set_Pixel_Sizes(face, 0, (FT_UInt)std::lround(bakePx));

        // Margen a cada lado del contorno para que quepa la banda del campo de
        // distancia entera. Sin él los bordes se cortan y el outline del shader
        // se come el glyph.
        const int padding = (int)std::ceil(m_pixelRange) + 1;

        std::vector<BakedGlyph> baked;
        baked.reserve(lastCodepoint - firstCodepoint + 1);

        for (uint32_t cp = firstCodepoint; cp <= lastCodepoint; ++cp)
        {
            if (FT_Load_Char(face, cp, FT_LOAD_NO_BITMAP) != 0) continue;

            BakedGlyph entry;
            entry.codepoint       = cp;
            entry.metrics.advance = (float)((double)face->glyph->advance.x * kFt26_6);

            OutlineSink sink;
            sink.shape = &entry.shape;

            FT_Outline_Funcs funcs{};
            funcs.move_to  = &outlineMoveTo;
            funcs.line_to  = &outlineLineTo;
            funcs.conic_to = &outlineConicTo;
            funcs.cubic_to = &outlineCubicTo;
            FT_Outline_Decompose(&face->glyph->outline, &funcs, &sink);

            if (!entry.shape.contours.empty())
            {
                entry.shape.normalize();
                msdfgen::edgeColoringSimple(entry.shape, 3.0);

                const msdfgen::Shape::Bounds b = entry.shape.getBounds();
                const int left   = (int)std::floor(b.l) - padding;
                const int bottom = (int)std::floor(b.b) - padding;
                const int right  = (int)std::ceil(b.r)  + padding;
                const int top    = (int)std::ceil(b.t)  + padding;

                entry.hasShape   = true;
                entry.width      = (uint32_t)std::max(1, right - left);
                entry.height     = (uint32_t)std::max(1, top - bottom);
                entry.translateX = -(double)left;
                entry.translateY = -(double)bottom;

                entry.metrics.rect.width  = (float)entry.width;
                entry.metrics.rect.height = (float)entry.height;
                entry.metrics.bearingX    = (float)left;
                // El bitmap llega hasta `top` por encima de la línea base, y el
                // canvas mide hacia abajo: por eso el batcher lo resta.
                entry.metrics.bearingY    = (float)top;
            }

            baked.push_back(std::move(entry));
        }

        if (baked.empty())
        {
            FT_Done_Face(face);
            FT_Done_FreeType(library);
            return false;
        }

        // ── Empaquetado por estanterías ──────────────────────────────────────
        // Todos los glyphs de un cuerpo miden parecido, así que una estantería
        // simple desperdicia poco y no hace falta nada más elaborado.
        uint32_t atlasSide = 64;
        bool packed = false;

        while (atlasSide <= 4096 && !packed)
        {
            uint32_t penX = 0, penY = 0, shelfH = 0;
            packed = true;

            for (BakedGlyph& g : baked)
            {
                if (!g.hasShape) continue;
                if (g.width + 1 > atlasSide || g.height + 1 > atlasSide) { packed = false; break; }

                if (penX + g.width + 1 > atlasSide)
                {
                    penX    = 0;
                    penY   += shelfH + 1;
                    shelfH  = 0;
                }
                if (penY + g.height + 1 > atlasSide) { packed = false; break; }

                g.metrics.rect.x = (float)penX;
                g.metrics.rect.y = (float)penY;

                penX  += g.width + 1;
                shelfH = std::max(shelfH, g.height);
            }

            if (!packed) atlasSide *= 2;
        }

        if (!packed)
        {
            std::printf("[UI] la fuente no cabe en un atlas de 4096: %s\n", path.c_str());
            FT_Done_Face(face);
            FT_Done_FreeType(library);
            return false;
        }

        // ── Horneado ─────────────────────────────────────────────────────────
        std::vector<uint8_t> pixels((size_t)atlasSide * atlasSide * 4, 0);

        for (const BakedGlyph& g : baked)
        {
            if (!g.hasShape) continue;

            msdfgen::Bitmap<float, 3> msdf((int)g.width, (int)g.height);
            const msdfgen::SDFTransformation transform(
                msdfgen::Projection(msdfgen::Vector2(1.0),
                                    msdfgen::Vector2(g.translateX, g.translateY)),
                msdfgen::Range((double)m_pixelRange));
            msdfgen::generateMSDF(msdf, g.shape, transform);

            const uint32_t ox = (uint32_t)g.metrics.rect.x;
            const uint32_t oy = (uint32_t)g.metrics.rect.y;

            for (uint32_t y = 0; y < g.height; ++y)
            {
                // msdfgen tiene el origen ABAJO y el atlas arriba: la fila se
                // voltea al copiar, que es lo que hace que las UVs de v0 = borde
                // superior sigan valiendo.
                const float* row = msdf((int)0, (int)(g.height - 1 - y));
                uint8_t* dst = &pixels[(((size_t)(oy + y) * atlasSide) + ox) * 4];
                for (uint32_t x = 0; x < g.width; ++x)
                {
                    dst[x * 4 + 0] = toByte(row[x * 3 + 0]);
                    dst[x * 4 + 1] = toByte(row[x * 3 + 1]);
                    dst[x * 4 + 2] = toByte(row[x * 3 + 2]);
                    dst[x * 4 + 3] = 255;
                }
            }
        }

        // ── Métricas ─────────────────────────────────────────────────────────
        m_glyphs.clear();
        m_kerning.clear();
        m_bakeSize = bakePx;
        setMetrics((float)((double)face->size->metrics.ascender  * kFt26_6),
                   (float)(-(double)face->size->metrics.descender * kFt26_6),
                   (float)((double)face->size->metrics.height     * kFt26_6));

        for (const BakedGlyph& g : baked)
            m_glyphs[g.codepoint] = g.metrics;

        if (FT_HAS_KERNING(face))
        {
            for (const BakedGlyph& l : baked)
            {
                const FT_UInt li = FT_Get_Char_Index(face, l.codepoint);
                if (li == 0) continue;
                for (const BakedGlyph& r : baked)
                {
                    const FT_UInt ri = FT_Get_Char_Index(face, r.codepoint);
                    if (ri == 0) continue;

                    FT_Vector delta{};
                    if (FT_Get_Kerning(face, li, ri, FT_KERNING_DEFAULT, &delta) != 0) continue;
                    if (delta.x == 0) continue;   // el par ausente ya vale 0

                    setKerning(l.codepoint, r.codepoint, (float)((double)delta.x * kFt26_6));
                }
            }
        }

        FT_Done_Face(face);
        FT_Done_FreeType(library);

        // UNORM, NUNCA SRGB: el MSDF son distancias y el sampler tiene que
        // devolverlas tal cual.
        return m_atlas.loadFromPixels(gpu, res, pixels.data(), atlasSide, atlasSide,
                                      VK_FORMAT_R8G8B8A8_UNORM);
    }
}
