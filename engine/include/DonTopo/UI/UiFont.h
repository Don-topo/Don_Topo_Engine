#pragma once

// Game UI font: an MSDF atlas baked from a TTF.
//
// An MSDF stores the DISTANCE to the outline in three channels, not the glyph's
// color. That is why the text looks sharp at any size without rebaking anything:
// the atlas is baked ONCE at bakeSize() and the final size comes from scaling the
// quad; the shader reconstructs the edge with the median of the three channels.
//
// Two halves, just like UiTextureAtlas:
//   - Pure CPU: per-glyph metrics, kerning and scaling to a given fontSize.
//     It touches neither FreeType nor Vulkan, can be filled in by hand and is what
//     the tests exercise.
//   - GPU: loadFromFile() opens the TTF, bakes the atlas and uploads it.
//
// The font CONTAINS its UiTextureAtlas instead of inheriting from it: this way the
// descriptor registration (UiSpriteBatch::registerAtlas) and the batcher's grouping by
// atlas work as they are, without a second path for text.

#include "DonTopo/UI/UiTextureAtlas.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace DonTopo
{
    class GpuDevice;
    class GpuResources;

    // CLOSED span of codepoints, [first, last]. A single codepoint is
    // first == last.
    struct UiCodepointRange
    {
        uint32_t first = 0;
        uint32_t last  = 0;
    };

    // What is baked when nobody asks for anything else: printable ASCII, the
    // whole Latin-1 supplement (á é í ó ú ü ñ Ñ ¿ ¡ « » º ª) and two loose ones, the
    // '…' that UiTextOverflow::Ellipsis asks for and the '€'. The controls 127..159 are
    // left out because they have no drawing.
    //
    // Widening the range is cheap: a codepoint that the font does NOT have is neither
    // baked nor takes space in the atlas. And falling short is expensive and silent:
    // shapeText skips the codepoint without a glyph without even leaving a gap, so "Año"
    // comes out "Ao" and there is not a single log to tell.
    inline const std::vector<UiCodepointRange>& defaultUiCodepointRanges()
    {
        static const std::vector<UiCodepointRange> ranges = {
            {32, 126}, {160, 255}, {0x2026, 0x2026}, {0x20AC, 0x20AC}
        };
        return ranges;
    }

    // Everything in PIXELS OF THE BAKE SIZE (bakeSize). Whoever draws scales
    // by fontSize/bakeSize.
    struct UiGlyph
    {
        // Area of the glyph INSIDE the atlas, in pixels. It includes the margin that
        // the distance field needs: the quad is drawn with this size,
        // not with the outline's.
        UiSpriteRect rect{};

        // From the cursor (pen) to the LEFT edge of the quad, +X to the right.
        float bearingX = 0.0f;
        // From the baseline to the TOP edge of the quad, +Y UPWARD. Careful:
        // the canvas has +Y downward, so the batcher SUBTRACTS it.
        float bearingY = 0.0f;
        // How far the cursor advances after this glyph. It does not have to resemble
        // either the rect's width or the bearing.
        float advance = 0.0f;
    };

    class UiFont
    {
    public:
        UiFont() = default;

        // --- CPU ---------------------------------------------------------------

        // Size the atlas was baked at. It is the denominator of all scaling:
        // at 0 the font draws nothing instead of dividing by zero.
        void  setBakeSize(float px) { m_bakeSize = px; }
        float bakeSize() const { return m_bakeSize; }

        // Width, in atlas pixels, of the band where the distance field
        // is valid. It is what the shader needs to know how many screen
        // pixels the edge covers.
        void  setPixelRange(float px) { m_pixelRange = px; }
        float pixelRange() const { return m_pixelRange; }

        void  setMetrics(float ascent, float descent, float lineHeight);
        float ascent() const { return m_ascent; }
        float descent() const { return m_descent; }
        float lineHeight() const { return m_lineHeight; }

        void addGlyph(uint32_t codepoint, const UiGlyph& glyph) { m_glyphs[codepoint] = glyph; }
        const UiGlyph* findGlyph(uint32_t codepoint) const;

        // Correction BETWEEN two consecutive glyphs, in bake pixels.
        // It is usually negative (brings the pair closer). A pair with no entry is worth 0.
        void  setKerning(uint32_t left, uint32_t right, float amount);
        float kerning(uint32_t left, uint32_t right) const;

        // UVs of the glyph from its rect and the atlas size. It does not go through
        // UiTextureAtlas's named sprites: a glyph has no name.
        UiUvRect glyphUv(const UiGlyph& glyph) const;

        // Factor by which ALL the metrics must be multiplied to draw
        // at fontSize. With bakeSize at 0 it returns 0.
        float scaleFor(float fontSize) const;

        UiTextureAtlas&       atlas()       { return m_atlas; }
        const UiTextureAtlas& atlas() const { return m_atlas; }

        // A font without glyphs does not draw: the batcher exits before walking
        // anything. Whether or not the atlas is uploaded to the GPU is another matter (the tests
        // run without Vulkan).
        bool hasGlyphs() const { return !m_glyphs.empty(); }

        // UTF-8 -> codepoints. Invalid bytes give U+FFFD instead of
        // desynchronizing the rest of the string.
        static std::vector<uint32_t> decodeUtf8(const std::string& text);
        // The same decoding over a REUSED vector: the batcher goes through
        // here every frame, and the version that returns by value would allocate a new
        // one for every text in the canvas.
        static void decodeUtf8(const std::string& text, std::vector<uint32_t>& out);

        // --- GPU ---------------------------------------------------------------

        // Bakes the TTF's codepoint ranges into an MSDF atlas and uploads it. The
        // atlas stays UNORM: an MSDF is distances, and an SRGB view distorts
        // them without giving a single validation error.
        bool loadFromFile(GpuDevice& gpu, GpuResources& res, const std::string& path,
                          float bakePx = 48.0f,
                          const std::vector<UiCodepointRange>& ranges = defaultUiCodepointRanges());

        // The bare bake: FreeType, MSDF, metrics and kerning, without touching the
        // GPU. It leaves the pixels in the atlas (UiTextureAtlas::sourcePixels) for
        // whoever knows how to upload them. loadFromFile is this plus the upload.
        //
        // threads splits the MSDF of each glyph, which is 90% of the cost and is
        // independent glyph by glyph: 0 = the hardware threads, 1 = the sequential
        // path. The result does NOT depend on the number of threads (the
        // packing is decided beforehand and each glyph writes into its own rect), and that is
        // exactly what the test compares byte by byte.
        bool bakeFromFile(const std::string& path, float bakePx = 48.0f,
                          const std::vector<UiCodepointRange>& ranges = defaultUiCodepointRanges(),
                          unsigned threads = 0);

        // The same, but going through a DISK cache: the baked atlas is
        // saved as is and the next startup reads it in milliseconds instead
        // of going through FreeType and msdfgen again. It is what both
        // backends use, because the bake is synchronous and the editor stops dead
        // the first time a text asks for its font.
        //
        // The entry is valid as long as neither the TTF (size and date), nor
        // bakePx, nor pixelRange, nor the ranges change. Any problem with the
        // file (missing, from another version, half-written or
        // corrupt) is solved by baking: the cache is NEVER the only source of
        // truth. Saving it is best-effort; if it fails the bake does not break.
        bool bakeFromFileCached(const std::string& path, float bakePx = 48.0f,
                                const std::vector<UiCodepointRange>& ranges = defaultUiCodepointRanges(),
                                unsigned threads = 0);

        // Where the cache lives, relative to the working directory. Empty = no
        // cache (the tests really bake). An atlas takes the size of the
        // RGBA bitmap, about 4 MB at 1024x1024.
        static void               setCacheDirectory(std::string dir);
        static const std::string& cacheDirectory();

        void destroy(GpuDevice& gpu) { m_atlas.destroy(gpu); }

    private:
        // The two halves of the cache. Loading leaves the font ready (metrics,
        // glyphs, kerning and atlas pixels); saving writes to a temporary and
        // renames, so a half-finished shutdown does not leave a broken entry.
        bool loadFromCache(const std::string& path, float bakePx,
                           const std::vector<UiCodepointRange>& ranges);
        bool saveToCache(const std::string& path, float bakePx,
                         const std::vector<UiCodepointRange>& ranges) const;

        UiTextureAtlas m_atlas;

        float m_bakeSize   = 0.0f;
        float m_pixelRange = 4.0f;
        float m_ascent     = 0.0f;
        float m_descent    = 0.0f;
        float m_lineHeight = 0.0f;

        std::unordered_map<uint32_t, UiGlyph> m_glyphs;
        // Key = (left << 32) | right: a single map instead of a map
        // of maps, and the absent pair is not even stored.
        std::unordered_map<uint64_t, float> m_kerning;
    };
}
