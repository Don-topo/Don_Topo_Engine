#include "DonTopo/UI/UiFont.h"

#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/GpuResources.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <msdfgen.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

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

        // ── Caché en disco ───────────────────────────────────────────────────
        // Formato propio y sin comprimir: lo que se guarda es exactamente lo que
        // deja el horneado, y leerlo tiene que costar lo que cuesta leer 4 MB.
        // La versión sube cada vez que cambie cualquier campo: una entrada de
        // otra versión se descarta y se vuelve a hornear, nunca se interpreta.
        constexpr uint32_t kCacheMagic   = 0x544E4644u;   // "DFNT"
        constexpr uint32_t kCacheVersion = 1u;

        // Topes de cordura al leer. Un fichero corrupto puede traer cualquier
        // número, y sin esto una reserva de gigabytes tumbaría el editor antes
        // de que llegara a fallar la comprobación de tamaño.
        constexpr uint32_t kMaxAtlasSide  = 8192u;
        constexpr uint32_t kMaxGlyphs     = 200000u;
        constexpr uint32_t kMaxKernPairs  = 4000000u;

        uint64_t fnv1a(const void* data, size_t bytes, uint64_t hash)
        {
            const uint8_t* p = (const uint8_t*)data;
            for (size_t i = 0; i < bytes; ++i)
            {
                hash ^= p[i];
                hash *= 1099511628211ull;
            }
            return hash;
        }

        // Nombre del fichero: hash de TODO lo que define el horneado. Dos
        // configuraciones distintas de la misma fuente son dos entradas, así que
        // cambiar el tamaño de horneado no invalida la que ya había.
        std::string cacheFileName(const std::string& path, float bakePx, float pixelRange,
                                  const std::vector<UiCodepointRange>& ranges)
        {
            uint64_t h = 14695981039346656037ull;
            h = fnv1a(path.data(), path.size(), h);
            h = fnv1a(&bakePx, sizeof(bakePx), h);
            h = fnv1a(&pixelRange, sizeof(pixelRange), h);
            for (const UiCodepointRange& r : ranges)
            {
                h = fnv1a(&r.first, sizeof(r.first), h);
                h = fnv1a(&r.last,  sizeof(r.last),  h);
            }

            char buf[32] = {};
            std::snprintf(buf, sizeof(buf), "%016llx.dtfont", (unsigned long long)h);
            return buf;
        }

        // Tamaño y fecha del TTF: es lo que distingue "la misma fuente" de "otra
        // fuente en la misma ruta". Sin fichero devuelve false y entonces no hay
        // nada que cachear ni que cargar.
        bool fontStamp(const std::string& path, uint64_t& size, uint64_t& mtime)
        {
            std::error_code ec;
            const auto bytes = std::filesystem::file_size(path, ec);
            if (ec) return false;
            const auto when = std::filesystem::last_write_time(path, ec);
            if (ec) return false;

            size  = (uint64_t)bytes;
            mtime = (uint64_t)when.time_since_epoch().count();
            return true;
        }

        template <class T>
        void putPod(std::ostream& out, const T& value)
        {
            out.write((const char*)&value, sizeof(T));
        }

        // Campo a campo y con tipos de tamaño fijo: nada de volcar structs, que
        // arrastrarían el padding del compilador al fichero.
        template <class T>
        bool getPod(std::istream& in, T& value)
        {
            return (bool)in.read((char*)&value, sizeof(T));
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

    // Relativo al directorio de trabajo, que es de donde ya cuelgan los assets.
    // Un solo sitio para todas las fuentes del proyecto.
    static std::string& cacheDirStorage()
    {
        static std::string dir = ".dt-cache/fonts";
        return dir;
    }

    void UiFont::setCacheDirectory(std::string dir) { cacheDirStorage() = std::move(dir); }
    const std::string& UiFont::cacheDirectory() { return cacheDirStorage(); }

    bool UiFont::loadFromCache(const std::string& path, float bakePx,
                               const std::vector<UiCodepointRange>& ranges)
    {
        if (cacheDirStorage().empty()) return false;

        uint64_t ttfSize = 0, ttfMtime = 0;
        if (!fontStamp(path, ttfSize, ttfMtime)) return false;

        const std::filesystem::path file =
            std::filesystem::path(cacheDirStorage()) /
            cacheFileName(path, bakePx, m_pixelRange, ranges);

        std::ifstream in(file, std::ios::binary);
        if (!in) return false;

        uint32_t magic = 0, version = 0;
        if (!getPod(in, magic) || !getPod(in, version)) return false;
        if (magic != kCacheMagic || version != kCacheVersion) return false;

        // El TTF de la firma tiene que ser EL MISMO fichero: cambiarlo por otro
        // en la misma ruta deja el hash igual y el atlas viejo sería de otra
        // fuente. Aquí es donde se caza.
        uint64_t size = 0, mtime = 0;
        float    px = 0.0f, range = 0.0f;
        uint32_t rangeCount = 0;
        if (!getPod(in, size) || !getPod(in, mtime) || !getPod(in, px) ||
            !getPod(in, range) || !getPod(in, rangeCount)) return false;
        if (size != ttfSize || mtime != ttfMtime) return false;
        if (px != bakePx || range != m_pixelRange) return false;
        if (rangeCount != (uint32_t)ranges.size()) return false;
        for (uint32_t i = 0; i < rangeCount; ++i)
        {
            uint32_t first = 0, last = 0;
            if (!getPod(in, first) || !getPod(in, last)) return false;
            if (first != ranges[i].first || last != ranges[i].last) return false;
        }

        float    bakeSize = 0.0f, ascent = 0.0f, descent = 0.0f, lineHeight = 0.0f;
        uint32_t atlasW = 0, atlasH = 0, glyphCount = 0;
        if (!getPod(in, bakeSize) || !getPod(in, ascent) || !getPod(in, descent) ||
            !getPod(in, lineHeight) || !getPod(in, atlasW) || !getPod(in, atlasH) ||
            !getPod(in, glyphCount)) return false;
        if (atlasW == 0 || atlasH == 0 || atlasW > kMaxAtlasSide || atlasH > kMaxAtlasSide) return false;
        if (glyphCount == 0 || glyphCount > kMaxGlyphs) return false;

        // Nada se escribe en la fuente hasta que TODO se ha leído bien: una
        // entrada a medias dejaría glyphs sin atlas y el texto saldría con
        // cuadros de basura en vez de rehornearse.
        std::unordered_map<uint32_t, UiGlyph> glyphs;
        glyphs.reserve(glyphCount);
        for (uint32_t i = 0; i < glyphCount; ++i)
        {
            uint32_t cp = 0;
            UiGlyph  g{};
            if (!getPod(in, cp) || !getPod(in, g.rect.x) || !getPod(in, g.rect.y) ||
                !getPod(in, g.rect.width) || !getPod(in, g.rect.height) ||
                !getPod(in, g.bearingX) || !getPod(in, g.bearingY) || !getPod(in, g.advance))
                return false;
            glyphs.emplace(cp, g);
        }

        uint32_t kernCount = 0;
        if (!getPod(in, kernCount) || kernCount > kMaxKernPairs) return false;
        std::unordered_map<uint64_t, float> kerning;
        kerning.reserve(kernCount);
        for (uint32_t i = 0; i < kernCount; ++i)
        {
            uint64_t key = 0;
            float    amount = 0.0f;
            if (!getPod(in, key) || !getPod(in, amount)) return false;
            kerning.emplace(key, amount);
        }

        uint64_t pixelBytes = 0;
        if (!getPod(in, pixelBytes)) return false;
        if (pixelBytes != (uint64_t)atlasW * atlasH * 4ull) return false;

        std::vector<uint8_t> pixels((size_t)pixelBytes);
        if (!in.read((char*)pixels.data(), (std::streamsize)pixelBytes)) return false;

        m_glyphs     = std::move(glyphs);
        m_kerning    = std::move(kerning);
        m_bakeSize   = bakeSize;
        m_ascent     = ascent;
        m_descent    = descent;
        m_lineHeight = lineHeight;
        // UNORM igual que al hornear: son distancias, no color.
        m_atlas.setSourcePixels(pixels.data(), atlasW, atlasH, /*srgb=*/false);
        return true;
    }

    bool UiFont::saveToCache(const std::string& path, float bakePx,
                             const std::vector<UiCodepointRange>& ranges) const
    {
        if (cacheDirStorage().empty()) return false;
        if (m_glyphs.empty() || m_atlas.sourcePixels().empty()) return false;

        uint64_t ttfSize = 0, ttfMtime = 0;
        if (!fontStamp(path, ttfSize, ttfMtime)) return false;

        std::error_code ec;
        std::filesystem::create_directories(cacheDirStorage(), ec);
        if (ec) return false;

        const std::filesystem::path file =
            std::filesystem::path(cacheDirStorage()) /
            cacheFileName(path, bakePx, m_pixelRange, ranges);
        // Temporal + rename: si el proceso muere a media escritura, lo que queda
        // es un .tmp que nadie lee, no media entrada con el magic correcto.
        const std::filesystem::path temp = file.string() + ".tmp";

        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) return false;

            putPod(out, kCacheMagic);
            putPod(out, kCacheVersion);
            putPod(out, ttfSize);
            putPod(out, ttfMtime);
            putPod(out, bakePx);
            putPod(out, m_pixelRange);
            putPod(out, (uint32_t)ranges.size());
            for (const UiCodepointRange& r : ranges)
            {
                putPod(out, r.first);
                putPod(out, r.last);
            }

            putPod(out, m_bakeSize);
            putPod(out, m_ascent);
            putPod(out, m_descent);
            putPod(out, m_lineHeight);
            putPod(out, m_atlas.width());
            putPod(out, m_atlas.height());
            putPod(out, (uint32_t)m_glyphs.size());
            for (const auto& entry : m_glyphs)
            {
                putPod(out, entry.first);
                putPod(out, entry.second.rect.x);
                putPod(out, entry.second.rect.y);
                putPod(out, entry.second.rect.width);
                putPod(out, entry.second.rect.height);
                putPod(out, entry.second.bearingX);
                putPod(out, entry.second.bearingY);
                putPod(out, entry.second.advance);
            }

            putPod(out, (uint32_t)m_kerning.size());
            for (const auto& entry : m_kerning)
            {
                putPod(out, entry.first);
                putPod(out, entry.second);
            }

            const std::vector<uint8_t>& pixels = m_atlas.sourcePixels();
            putPod(out, (uint64_t)pixels.size());
            out.write((const char*)pixels.data(), (std::streamsize)pixels.size());
            if (!out) { out.close(); std::filesystem::remove(temp, ec); return false; }
        }

        std::filesystem::rename(temp, file, ec);
        if (ec)
        {
            // Windows no renombra encima de un fichero abierto por otro: se
            // borra el temporal y se sigue con el atlas ya horneado en memoria.
            std::filesystem::remove(temp, ec);
            return false;
        }
        return true;
    }

    bool UiFont::bakeFromFileCached(const std::string& path, float bakePx,
                                    const std::vector<UiCodepointRange>& ranges, unsigned threads)
    {
        if (loadFromCache(path, bakePx, ranges)) return true;
        if (!bakeFromFile(path, bakePx, ranges, threads)) return false;
        // Guardar es best-effort: un disco lleno o de solo lectura no puede
        // convertir un horneado bueno en un fallo.
        saveToCache(path, bakePx, ranges);
        return true;
    }

    bool UiFont::bakeFromFile(const std::string& path, float bakePx,
                              const std::vector<UiCodepointRange>& ranges, unsigned threads)
    {
        if (bakePx <= 0.0f || ranges.empty()) return false;

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

        size_t total = 0;
        for (const UiCodepointRange& r : ranges)
            if (r.last >= r.first) total += (size_t)(r.last - r.first) + 1;

        std::vector<BakedGlyph> baked;
        baked.reserve(total);

        for (const UiCodepointRange& range : ranges)
        for (uint32_t cp = range.first; cp <= range.last; ++cp)
        {
            // El codepoint que la fuente NO trae se salta aquí: FT_Load_Char
            // cargaría el .notdef (índice 0) y meteríamos una cajita vacía en el
            // atlas por cada hueco del rango.
            if (FT_Get_Char_Index(face, cp) == 0) continue;
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

        // Cada glyph es independiente: su Shape ya está coloreado, su bitmap es
        // local y el empaquetado le dio un rect DISJUNTO del atlas, así que dos
        // hilos nunca escriben el mismo byte de `pixels`. El reparto va por
        // índice atómico y no por bloques porque los glyphs no miden lo mismo:
        // con bloques, el hilo que pilla las mayúsculas acaba el último.
        //
        // El default NO es "todos los hilos": con el CRT de depuración cada
        // asignación pasa por un heap con cerrojo global, y msdfgen asigna por
        // glyph, así que ahí el paralelismo va HACIA ATRÁS. Medido con la fuente
        // por defecto a 48 px (20 hilos lógicos), en ms:
        //
        //   hilos      1      2      4      8     16
        //   Debug   2829   2463   6959  14339  17835
        //   Release  808    409    237    144    114
        //
        // De ahí los dos defaults: 2 en Debug (lo único que gana algo) y los del
        // hardware en Release. Un número explícito manda sobre esto.
        unsigned nThreads = threads;
        if (nThreads == 0)
        {
#ifdef _DEBUG
            nThreads = 2;
#else
            nThreads = std::thread::hardware_concurrency();
            if (nThreads > 16) nThreads = 16;   // pasado ahí la curva ya es plana
#endif
            if (nThreads == 0) nThreads = 1;
        }
        if (nThreads > baked.size()) nThreads = (unsigned)baked.size();
        if (nThreads == 0) nThreads = 1;

        std::atomic<size_t> siguiente{0};

        auto horneaUno = [&](const BakedGlyph& g)
        {
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
        };

        auto trabaja = [&]()
        {
            for (;;)
            {
                const size_t i = siguiente.fetch_add(1);
                if (i >= baked.size()) return;
                // El glyph sin contorno (el espacio) no tiene bitmap que generar,
                // solo avance: se cuenta igual para no descuadrar el reparto.
                if (baked[i].hasShape) horneaUno(baked[i]);
            }
        };

        if (nThreads <= 1)
        {
            trabaja();
        }
        else
        {
            // El hilo que llama también trabaja: con N hilos se lanzan N-1.
            std::vector<std::thread> pool;
            pool.reserve(nThreads - 1);
            for (unsigned t = 1; t < nThreads; ++t) pool.emplace_back(trabaja);
            trabaja();
            for (std::thread& th : pool) th.join();
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
        // devolverlas tal cual. Aqui solo se guardan; sube quien pueda.
        m_atlas.setSourcePixels(pixels.data(), atlasSide, atlasSide, /*srgb=*/false);
        return true;
    }

    bool UiFont::loadFromFile(GpuDevice& gpu, GpuResources& res, const std::string& path,
                              float bakePx, const std::vector<UiCodepointRange>& ranges)
    {
        // Con caché: el horneado solo ocurre la primera vez de todas.
        if (!bakeFromFileCached(path, bakePx, ranges))
            return false;

        return m_atlas.loadFromPixels(gpu, res, m_atlas.sourcePixels().data(), m_atlas.width(),
                                      m_atlas.height(), VK_FORMAT_R8G8B8A8_UNORM);
    }
}
