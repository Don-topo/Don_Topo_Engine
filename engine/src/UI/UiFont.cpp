#include "DonTopo/UI/UiFont.h"

#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/GpuResources.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
// FT_Load_Sfnt_Table and the TTAG_*: this is how GPOS is read raw, which is
// where modern fonts carry the kerning.
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H

#include <msdfgen.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unordered_set>

namespace DonTopo
{
    namespace
    {
        uint64_t kerningKey(uint32_t left, uint32_t right)
        {
            return ((uint64_t)left << 32) | (uint64_t)right;
        }

        // FreeType gives coordinates in 26.6 (1/64 of a pixel) once the size in
        // pixels is set, so dividing by 64 leaves them ALREADY in bake pixels:
        // the metrics and the outline come out in the same unit.
        constexpr double kFt26_6 = 1.0 / 64.0;

        // ── FreeType outline -> msdfgen Shape ───────────────────────────────
        // It is the only thing the "ext" half of msdfgen provided, which is not
        // compiled here because it looks for FreeType on the system.
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
            // A single-point contour contributes not even one edge: msdfgen would
            // treat it as an empty contour and edgeColoringSimple counts it.
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

        // The same rounding msdfgen uses when dumping to 8 bits.
        uint8_t toByte(float v)
        {
            const int i = (int)(v * 256.0f);
            return (uint8_t)std::min(std::max(i, 0), 255);
        }

        // ── Disk cache ───────────────────────────────────────────────────────
        // Own format, uncompressed: what is saved is exactly what the bake
        // leaves, and reading it has to cost what reading 4 MB costs.
        // The version goes up every time any field changes: an entry from
        // another version is discarded and rebaked, never interpreted.
        constexpr uint32_t kCacheMagic   = 0x544E4644u;   // "DFNT"
        // 2: kerning now also comes from GPOS. A v1 entry was baked when the
        // kerning of a modern font was ALWAYS zero, so reusing it would
        // leave the text without kerning forever.
        constexpr uint32_t kCacheVersion = 2u;

        // Sanity caps when reading. A corrupt file can carry any
        // number, and without this a reservation of gigabytes would take the editor down before
        // the size check got a chance to fail.
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

        // File name: hash of EVERYTHING that defines the bake. Two different
        // configurations of the same font are two entries, so
        // changing the bake size does not invalidate the one that already existed.
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

        // Size and date of the TTF: it is what tells "the same font" apart from "another
        // font at the same path". Without a file it returns false and then there is
        // nothing to cache or load.
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

        // Field by field and with fixed-size types: no dumping structs, which
        // would drag the compiler's padding into the file.
        template <class T>
        bool getPod(std::istream& in, T& value)
        {
            return (bool)in.read((char*)&value, sizeof(T));
        }

        // ── GPOS kerning ─────────────────────────────────────────────────────
        // FT_Get_Kerning only reads the CLASSIC 'kern' table, and modern
        // fonts (the project's included) carry the pairs in GPOS. Without this
        // the engine's kerning exists, is tested and is never applied: all
        // the pairs are 0 and the letters come out loose.
        //
        // The subset that really covers kerning is read: the 'kern'
        // feature, its type 2 lookups (PairPos) in both formats, and the
        // type 9 (Extension) that wraps them. The rest (marks, italics,
        // contextual) is not pair kerning and is not touched.
        //
        // All the parsing goes through a bounds-checked reader: a broken or
        // truncated font has to give zero pairs, not read outside the buffer.
        struct BeReader
        {
            const uint8_t* data = nullptr;
            size_t         size = 0;

            bool has(size_t off, size_t bytes) const { return off + bytes <= size; }
            uint16_t u16(size_t off) const
            {
                if (!has(off, 2)) return 0;
                return (uint16_t)((data[off] << 8) | data[off + 1]);
            }
            int16_t s16(size_t off) const { return (int16_t)u16(off); }
            uint32_t u32(size_t off) const
            {
                if (!has(off, 4)) return 0;
                return ((uint32_t)data[off] << 24) | ((uint32_t)data[off + 1] << 16) |
                       ((uint32_t)data[off + 2] << 8) | (uint32_t)data[off + 3];
            }
        };

        // How many bytes a ValueRecord takes according to its format: it is a bit
        // map and each present bit adds an int16.
        int valueSize(uint16_t format)
        {
            int n = 0;
            for (int bit = 0; bit < 8; ++bit)
                if (format & (1u << bit)) ++n;
            return n * 2;
        }

        // XAdvance of the ValueRecord, which is the only thing this engine models: a
        // scalar offset in X between two glyphs. Bit 0x0004 is
        // XAdvance and goes after XPlacement (0x0001) and YPlacement (0x0002).
        int16_t valueXAdvance(const BeReader& r, size_t off, uint16_t format)
        {
            if (!(format & 0x0004)) return 0;
            size_t cursor = off;
            if (format & 0x0001) cursor += 2;
            if (format & 0x0002) cursor += 2;
            return r.s16(cursor);
        }

        // Glyphs covered by a Coverage table, in coverage-index ORDER:
        // it is that order that indexes the PairSets.
        void readCoverage(const BeReader& r, size_t off, std::vector<uint16_t>& out)
        {
            out.clear();
            const uint16_t format = r.u16(off);
            if (format == 1)
            {
                const uint16_t count = r.u16(off + 2);
                out.reserve(count);
                for (uint16_t i = 0; i < count; ++i) out.push_back(r.u16(off + 4 + i * 2));
            }
            else if (format == 2)
            {
                const uint16_t ranges = r.u16(off + 2);
                for (uint16_t i = 0; i < ranges; ++i)
                {
                    const size_t   rec   = off + 4 + i * 6;
                    const uint16_t first = r.u16(rec);
                    const uint16_t last  = r.u16(rec + 2);
                    if (last < first) continue;
                    // An absurd range in a broken font could ask for 65k
                    // entries per range; the real glyph cap bounds it.
                    for (uint32_t g = first; g <= last; ++g) out.push_back((uint16_t)g);
                }
            }
        }

        // Class of a glyph inside a ClassDef. 0 is "the rest", which is
        // a class with every right to have its own kerning.
        uint16_t classOf(const BeReader& r, size_t off, uint16_t glyph)
        {
            const uint16_t format = r.u16(off);
            if (format == 1)
            {
                const uint16_t start = r.u16(off + 2);
                const uint16_t count = r.u16(off + 4);
                if (glyph < start || glyph >= (uint32_t)start + count) return 0;
                return r.u16(off + 6 + (glyph - start) * 2);
            }
            if (format == 2)
            {
                const uint16_t ranges = r.u16(off + 2);
                for (uint16_t i = 0; i < ranges; ++i)
                {
                    const size_t rec = off + 4 + i * 6;
                    if (glyph >= r.u16(rec) && glyph <= r.u16(rec + 2)) return r.u16(rec + 4);
                }
            }
            return 0;
        }

        // Key of a glyph pair. The indices are the FONT's, not
        // codepoints: the translation to codepoint is done by the caller.
        uint64_t pairKey(uint16_t left, uint16_t right)
        {
            return ((uint64_t)left << 16) | (uint64_t)right;
        }

        // A PairPos subtable, of both formats. Only the pairs in which BOTH glyphs
        // are baked are stored: the rest will never be drawn
        // and would fill the map with dead entries.
        void readPairPos(const BeReader& r, size_t off,
                         const std::unordered_set<uint16_t>& wanted,
                         std::unordered_map<uint64_t, int16_t>& out)
        {
            const uint16_t format = r.u16(off);
            const uint16_t vf1    = r.u16(off + 4);
            const uint16_t vf2    = r.u16(off + 6);
            const int      v1Size = valueSize(vf1);
            const int      v2Size = valueSize(vf2);

            std::vector<uint16_t> coverage;
            readCoverage(r, off + r.u16(off + 2), coverage);

            if (format == 1)
            {
                const uint16_t pairSets = r.u16(off + 8);
                const uint16_t total    = (uint16_t)std::min<size_t>(pairSets, coverage.size());
                for (uint16_t i = 0; i < total; ++i)
                {
                    const uint16_t left = coverage[i];
                    if (wanted.count(left) == 0) continue;

                    const size_t   setOff = off + r.u16(off + 10 + i * 2);
                    const uint16_t pairs  = r.u16(setOff);
                    for (uint16_t p = 0; p < pairs; ++p)
                    {
                        const size_t   rec   = setOff + 2 + (size_t)p * (2 + v1Size + v2Size);
                        const uint16_t right = r.u16(rec);
                        if (wanted.count(right) == 0) continue;

                        const int16_t adv = valueXAdvance(r, rec + 2, vf1);
                        if (adv != 0) out[pairKey(left, right)] = adv;
                    }
                }
                return;
            }

            if (format != 2) return;

            const size_t   class1Off  = off + r.u16(off + 8);
            const size_t   class2Off  = off + r.u16(off + 10);
            const uint16_t class1Count = r.u16(off + 12);
            const uint16_t class2Count = r.u16(off + 14);
            if (class1Count == 0 || class2Count == 0) return;

            const size_t recSize = (size_t)(v1Size + v2Size);
            const size_t matrix  = off + 16;

            // Each glyph's class is resolved ONCE: looking it up inside the
            // pair loop would walk the ranges N² times.
            std::unordered_map<uint16_t, uint16_t> claseIzq, claseDer;
            for (uint16_t g : wanted)
            {
                claseIzq[g] = classOf(r, class1Off, g);
                claseDer[g] = classOf(r, class2Off, g);
            }

            // The coverage decides WHICH glyphs take part as first of the pair;
            // class 0 does count, but only for the covered ones.
            std::unordered_set<uint16_t> cubiertos(coverage.begin(), coverage.end());

            for (uint16_t left : wanted)
            {
                if (cubiertos.count(left) == 0) continue;
                const uint16_t c1 = claseIzq[left];
                if (c1 >= class1Count) continue;

                for (uint16_t right : wanted)
                {
                    const uint16_t c2 = claseDer[right];
                    if (c2 >= class2Count) continue;

                    const size_t  rec = matrix + ((size_t)c1 * class2Count + c2) * recSize;
                    const int16_t adv = valueXAdvance(r, rec, vf1);
                    if (adv != 0) out[pairKey(left, right)] = adv;
                }
            }
        }

        // Walks the whole GPOS table and extracts the pairs of the 'kern' feature.
        void collectGposKerning(const uint8_t* data, size_t size,
                                const std::unordered_set<uint16_t>& wanted,
                                std::unordered_map<uint64_t, int16_t>& out)
        {
            if (!data || size < 10 || wanted.empty()) return;

            BeReader r{data, size};
            const size_t featureList = r.u16(6);
            const size_t lookupList  = r.u16(8);
            if (featureList == 0 || lookupList == 0) return;

            // Which lookups 'kern' uses. There can be several features with that tag
            // (one per script/language) and they can share lookups, so they are accumulated
            // in a set.
            std::unordered_set<uint16_t> lookups;
            const uint16_t featureCount = r.u16(featureList);
            for (uint16_t i = 0; i < featureCount; ++i)
            {
                const size_t rec = featureList + 2 + (size_t)i * 6;
                if (r.u32(rec) != 0x6B65726Eu) continue;   // 'kern'

                const size_t   feat  = featureList + r.u16(rec + 4);
                const uint16_t count = r.u16(feat + 2);
                for (uint16_t j = 0; j < count; ++j) lookups.insert(r.u16(feat + 4 + j * 2));
            }
            if (lookups.empty()) return;

            const uint16_t lookupCount = r.u16(lookupList);
            for (uint16_t li : lookups)
            {
                if (li >= lookupCount) continue;
                const size_t   lookup   = lookupList + r.u16(lookupList + 2 + (size_t)li * 2);
                const uint16_t type     = r.u16(lookup);
                const uint16_t subCount = r.u16(lookup + 4);

                for (uint16_t s = 0; s < subCount; ++s)
                {
                    const size_t sub = lookup + r.u16(lookup + 6 + (size_t)s * 2);

                    if (type == 2) { readPairPos(r, sub, wanted, out); continue; }

                    // Type 9: wrapper to get around the 16-bit limit of the
                    // offsets. Any type can be inside; only type 2
                    // matters, and it is NOT nested (the format forbids it).
                    if (type == 9 && r.u16(sub) == 1 && r.u16(sub + 2) == 2)
                        readPairPos(r, sub + r.u32(sub + 4), wanted, out);
                }
            }
        }

        struct BakedGlyph
        {
            uint32_t       codepoint = 0;
            msdfgen::Shape shape;
            bool     hasShape = false;
            uint32_t width    = 0;   // size of the MSDF bitmap, already with the margin
            uint32_t height   = 0;
            double   translateX = 0.0;   // brings the outline inside the bitmap
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
        // The vector belongs to the caller and is REUSED: the batcher decodes each
        // text every frame and cannot afford one allocation per label.
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
                // Stray continuation byte or invalid header: ONE is consumed
                // and decoding goes on, which is what keeps the rest of the string
                // from shifting.
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

    // Relative to the working directory, which is where the assets already hang from.
    // A single place for all the project's fonts.
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

        // The signature's TTF has to be THE SAME file: swapping it for another
        // at the same path leaves the hash the same and the old atlas would belong to another
        // font. This is where it gets caught.
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

        // Nothing is written to the font until EVERYTHING has been read correctly: a
        // half entry would leave glyphs without an atlas and the text would come out with
        // garbage boxes instead of being rebaked.
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
        // UNORM just like when baking: they are distances, not color.
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
        // Temporary + rename: if the process dies mid-write, what is left
        // is a .tmp that nobody reads, not half an entry with the right magic.
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
            // Windows does not rename over a file opened by another: the
            // temporary is deleted and we go on with the atlas already baked in memory.
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
        // Saving is best-effort: a full or read-only disk cannot
        // turn a good bake into a failure.
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
            std::printf("[UI] FreeType does not start\n");
            return false;
        }

        FT_Face face = nullptr;
        if (FT_New_Face(library, path.c_str(), 0, &face) != 0)
        {
            std::printf("[UI] unreadable font: %s\n", path.c_str());
            FT_Done_FreeType(library);
            return false;
        }

        FT_Set_Pixel_Sizes(face, 0, (FT_UInt)std::lround(bakePx));

        // Margin on each side of the outline so that the whole distance field band
        // fits. Without it the edges get cut and the shader's outline
        // eats the glyph.
        const int padding = (int)std::ceil(m_pixelRange) + 1;

        size_t total = 0;
        for (const UiCodepointRange& r : ranges)
            if (r.last >= r.first) total += (size_t)(r.last - r.first) + 1;

        std::vector<BakedGlyph> baked;
        baked.reserve(total);

        for (const UiCodepointRange& range : ranges)
        for (uint32_t cp = range.first; cp <= range.last; ++cp)
        {
            // The codepoint that the font does NOT have is skipped here: FT_Load_Char
            // would load .notdef (index 0) and we would put an empty little box in the
            // atlas for every gap in the range.
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
                // The bitmap reaches `top` above the baseline, and the
                // canvas measures downward: that is why the batcher subtracts it.
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

        // ── Shelf packing ────────────────────────────────────────────────────
        // All the glyphs of a size are similar in height, so a simple shelf
        // wastes little and nothing more elaborate is needed.
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
            std::printf("[UI] the font does not fit in a 4096 atlas: %s\n", path.c_str());
            FT_Done_Face(face);
            FT_Done_FreeType(library);
            return false;
        }

        // ── Baking ───────────────────────────────────────────────────────────
        std::vector<uint8_t> pixels((size_t)atlasSide * atlasSide * 4, 0);

        // Each glyph is independent: its Shape is already colored, its bitmap is
        // local and the packing gave it a DISJOINT rect of the atlas, so two
        // threads never write the same byte of `pixels`. The split goes by atomic
        // index and not by blocks because glyphs are not the same size:
        // with blocks, the thread that gets the capitals finishes last.
        //
        // The default is NOT "all the threads": with the debug CRT each
        // allocation goes through a heap with a global lock, and msdfgen allocates per
        // glyph, so there parallelism goes BACKWARDS. Measured with the default
        // font at 48 px (20 logical threads), in ms:
        //
        //   threads    1      2      4      8     16
        //   Debug   2829   2463   6959  14339  17835
        //   Release  808    409    237    144    114
        //
        // Hence the two defaults: 2 in Debug (the only one that gains anything) and the
        // hardware's in Release. An explicit number overrides this.
        unsigned nThreads = threads;
        if (nThreads == 0)
        {
#ifdef _DEBUG
            nThreads = 2;
#else
            nThreads = std::thread::hardware_concurrency();
            if (nThreads > 16) nThreads = 16;   // past that the curve is already flat
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
                // msdfgen has the origin at the BOTTOM and the atlas at the top: the row is
                // flipped when copying, which is what keeps the UVs with v0 = top
                // edge valid.
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
                // A glyph without an outline (the space) has no bitmap to generate,
                // only an advance: it is counted all the same so as not to throw the split off.
                if (baked[i].hasShape) horneaUno(baked[i]);
            }
        };

        if (nThreads <= 1)
        {
            trabaja();
        }
        else
        {
            // The calling thread also works: with N threads, N-1 are launched.
            std::vector<std::thread> pool;
            pool.reserve(nThreads - 1);
            for (unsigned t = 1; t < nThreads; ++t) pool.emplace_back(trabaja);
            trabaja();
            for (std::thread& th : pool) th.join();
        }

        // ── Metrics ──────────────────────────────────────────────────────────
        m_glyphs.clear();
        m_kerning.clear();
        m_bakeSize = bakePx;
        setMetrics((float)((double)face->size->metrics.ascender  * kFt26_6),
                   (float)(-(double)face->size->metrics.descender * kFt26_6),
                   (float)((double)face->size->metrics.height     * kFt26_6));

        for (const BakedGlyph& g : baked)
            m_glyphs[g.codepoint] = g.metrics;

        // Glyph index -> codepoint, to translate back what the tables say (they
        // speak of glyphs, not characters). It is computed once: the earlier
        // loop called FT_Get_Char_Index N² times.
        std::unordered_map<uint16_t, uint32_t> aCodepoint;
        std::unordered_set<uint16_t>           indices;
        aCodepoint.reserve(baked.size());
        indices.reserve(baked.size());
        for (const BakedGlyph& g : baked)
        {
            const FT_UInt gi = FT_Get_Char_Index(face, g.codepoint);
            if (gi == 0 || gi > 0xFFFFu) continue;
            // A glyph can have several codepoints (aliases): the FIRST is kept,
            // which with the ranges sorted is the one with the lowest value.
            aCodepoint.emplace((uint16_t)gi, g.codepoint);
            indices.insert((uint16_t)gi);
        }

        // (a) The classic 'kern' table, which is the only thing FreeType can read.
        if (FT_HAS_KERNING(face))
        {
            for (uint16_t li : indices)
            {
                for (uint16_t ri : indices)
                {
                    FT_Vector delta{};
                    if (FT_Get_Kerning(face, li, ri, FT_KERNING_DEFAULT, &delta) != 0) continue;
                    if (delta.x == 0) continue;   // the absent pair is already 0
                    setKerning(aCodepoint[li], aCodepoint[ri], (float)((double)delta.x * kFt26_6));
                }
            }
        }

        // (b) GPOS, which is where modern fonts carry it (the project's own
        // among them). Without this, FT_HAS_KERNING is false and the engine's
        // kerning is never applied.
        {
            FT_ULong len = 0;
            if (FT_Load_Sfnt_Table(face, TTAG_GPOS, 0, nullptr, &len) == 0 && len > 0)
            {
                std::vector<uint8_t> gpos((size_t)len);
                if (FT_Load_Sfnt_Table(face, TTAG_GPOS, 0, gpos.data(), &len) == 0)
                {
                    std::unordered_map<uint64_t, int16_t> pares;
                    collectGposKerning(gpos.data(), gpos.size(), indices, pares);

                    for (const auto& entry : pares)
                    {
                        const uint16_t li = (uint16_t)(entry.first >> 16);
                        const uint16_t ri = (uint16_t)(entry.first & 0xFFFFu);
                        const auto itL = aCodepoint.find(li);
                        const auto itR = aCodepoint.find(ri);
                        if (itL == aCodepoint.end() || itR == aCodepoint.end()) continue;

                        // From DESIGN units to bake pixels: x_scale is
                        // the 16.16 that FreeType set with FT_Set_Pixel_Sizes, and the
                        // result comes out in 26.6. Without this conversion the value
                        // would come out in the hundreds and separate half a word.
                        const FT_Pos px = FT_MulFix(entry.second, face->size->metrics.x_scale);
                        setKerning(itL->second, itR->second, (float)((double)px * kFt26_6));
                    }
                }
            }
        }

        FT_Done_Face(face);
        FT_Done_FreeType(library);

        // UNORM, NEVER SRGB: the MSDF is distances and the sampler has to
        // return them as they are. Here they are only stored; whoever can uploads them.
        m_atlas.setSourcePixels(pixels.data(), atlasSide, atlasSide, /*srgb=*/false);
        return true;
    }

    bool UiFont::loadFromFile(GpuDevice& gpu, GpuResources& res, const std::string& path,
                              float bakePx, const std::vector<UiCodepointRange>& ranges)
    {
        // With cache: the bake only happens the very first time.
        if (!bakeFromFileCached(path, bakePx, ranges))
            return false;

        return m_atlas.loadFromPixels(gpu, res, m_atlas.sourcePixels().data(), m_atlas.width(),
                                      m_atlas.height(), VK_FORMAT_R8G8B8A8_UNORM);
    }
}
