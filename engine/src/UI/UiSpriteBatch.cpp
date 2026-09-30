#include "DonTopo/UI/UiSpriteBatch.h"

#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiFont.h"
#include "DonTopo/UI/UiWidgets.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/GpuResources.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include "DonTopo/Renderer/ShaderModule.h"

namespace DonTopo
{
    namespace
    {
        // uint16 for the indices: 4 vertices per quad, so the ceiling is
        // 16383 quads per frame. Past that point emission stops instead of
        // silently overflowing the index.
        constexpr size_t kMaxVertices = 65532;

        constexpr uint32_t kInitialVertexCapacity = 1024;   // 256 quads
        constexpr uint32_t kInitialIndexCapacity  = 1536;

        UiScissor scissorFromRect(const glm::vec2& pos, const glm::vec2& size)
        {
            if (size.x <= 0.0f || size.y <= 0.0f) return {};

            // Outwards (floor/ceil): clipping too little leaves the sprite's edge;
            // clipping too much eats a row of pixels.
            const float fx0 = std::floor(pos.x);
            const float fy0 = std::floor(pos.y);
            const float fx1 = std::ceil(pos.x + size.x);
            const float fy1 = std::ceil(pos.y + size.y);

            UiScissor s{};
            s.x = (int32_t)fx0;
            s.y = (int32_t)fy0;
            s.width  = (uint32_t)std::max(0.0f, fx1 - fx0);
            s.height = (uint32_t)std::max(0.0f, fy1 - fy0);
            return s;
        }

        // Intersection, NOT replacement: a child can never paint outside what
        // its parent had already clipped.
        UiScissor intersectScissor(const UiScissor& a, const UiScissor& b)
        {
            if (a.empty() || b.empty()) return {};

            const int64_t x0 = std::max((int64_t)a.x, (int64_t)b.x);
            const int64_t y0 = std::max((int64_t)a.y, (int64_t)b.y);
            const int64_t x1 = std::min((int64_t)a.x + a.width,  (int64_t)b.x + b.width);
            const int64_t y1 = std::min((int64_t)a.y + a.height, (int64_t)b.y + b.height);

            if (x1 <= x0 || y1 <= y0) return {};

            UiScissor s{};
            s.x = (int32_t)x0;
            s.y = (int32_t)y0;
            s.width  = (uint32_t)(x1 - x0);
            s.height = (uint32_t)(y1 - y0);
            return s;
        }

        // A quad with EVERYTHING explicit: atlas, UVs, color and the two parameter
        // vec4s. The text one cannot come from node.sprite because a glyph
        // has no name, so the rule for breaking the batch lives here and only
        // here.
        // dxTop/dxBottom shift the top and bottom edges in X: it is the
        // italic shear, which this way needs neither a matrix nor a wider
        // vertex. At 0 the quad comes out exactly as it always did.
        // ── Quad rotation ───────────────────────────────────────────────────
        // Module state and not one more parameter: the rotation has to reach
        // the N quads that an Image emits and every glyph of a Text without
        // dirtying six signatures. The UI build is single-threaded and
        // deterministic, and whoever turns it on (emitNode) leaves it as it was
        // before descending into the children.
        struct QuadRotation
        {
            bool      activa = false;
            float     sen    = 0.0f;
            float     cs     = 1.0f;
            glm::vec2 centro{0.0f, 0.0f};
        };

        QuadRotation g_rot;

        // ── Canvas resolution ───────────────────────────────────────────────
        // The whole tree (layout, anchors, text, animations) is resolved in
        // REFERENCE units and does not know this exists. The scale and the
        // origin enter ONCE, when the already resolved rect of each node is
        // turned into render pixels. Module state for the same reason as g_rot: the
        // UI build is single-threaded and deterministic.
        struct CanvasXform
        {
            glm::vec2 origen{0.0f, 0.0f};
            float     escala = 1.0f;
        };

        CanvasXform g_xf;

        glm::vec2 rotaQuad(const glm::vec2& p)
        {
            const glm::vec2 d = p - g_rot.centro;
            return {g_rot.centro.x + d.x * g_rot.cs  - d.y * g_rot.sen,
                    g_rot.centro.y + d.x * g_rot.sen + d.y * g_rot.cs};
        }

        // ── Per-node cache ──────────────────────────────────────────────────
        // While g_rec points at a node, every quad that comes out is ALSO copied
        // into its cache, split by (atlas, scissor) just like the batches. Null
        // outside a node's emission, that is, whenever the cache is being used.
        const UiElement* g_rec = nullptr;

        // Nodes that have re-emitted in the current build.
        uint32_t g_rebuilt = 0;

        // A freshly emitted quad, as is, into the active node's cache.
        // The indices are stored RELATIVE to the node's first vertex.
        void grabaQuad(const UiTextureAtlas* atlas, const UiScissor& scissor,
                       const UiVertex* verts, const uint16_t* quad, uint16_t quadBase)
        {
            if (g_rec == nullptr) return;

            auto& segs = g_rec->cacheSegments;
            if (segs.empty() || segs.back().atlas != atlas || segs.back().scissor != scissor)
            {
                UiElement::CacheSegment seg{};
                seg.atlas   = atlas;
                seg.scissor = scissor;
                segs.push_back(seg);
            }

            const uint16_t base = (uint16_t)g_rec->cacheVertices.size();
            g_rec->cacheVertices.insert(g_rec->cacheVertices.end(), verts, verts + 4);
            for (int i = 0; i < 6; ++i)
                g_rec->cacheIndices.push_back((uint16_t)(base + (quad[i] - quadBase)));

            segs.back().vertexCount += 4;
            segs.back().indexCount  += 6;
        }

        // Dumps the cache of a clean node. Reopens a batch with THE SAME criterion
        // as emitRawQuad and rebases the indices onto the destination base: the
        // same uint16s and the same batch cut come out as if it had been emitted.
        void reproduceCache(const UiElement& node, UiDrawData& out)
        {
            size_t vRead = 0;
            size_t iRead = 0;

            for (const UiElement::CacheSegment& seg : node.cacheSegments)
            {
                // The same ceiling as emitRawQuad. With the same scene and the same
                // order it never triggers; it is there so that an index above
                // 65535 cannot come out if it ever does.
                if (out.vertices.size() + seg.vertexCount > kMaxVertices) return;

                if (out.batches.empty() ||
                    out.batches.back().atlas != seg.atlas ||
                    out.batches.back().scissor != seg.scissor)
                {
                    UiBatch batch{};
                    batch.atlas      = seg.atlas;
                    batch.scissor    = seg.scissor;
                    batch.firstIndex = (uint32_t)out.indices.size();
                    batch.indexCount = 0;
                    out.batches.push_back(batch);
                }

                const uint16_t base = (uint16_t)out.vertices.size();
                out.vertices.insert(out.vertices.end(),
                                    node.cacheVertices.begin() + (ptrdiff_t)vRead,
                                    node.cacheVertices.begin() + (ptrdiff_t)(vRead + seg.vertexCount));

                for (uint32_t k = 0; k < seg.indexCount; ++k)
                    out.indices.push_back((uint16_t)(base + node.cacheIndices[iRead + k]));

                out.batches.back().indexCount += seg.indexCount;

                vRead += seg.vertexCount;
                iRead += seg.indexCount;
            }
        }

        void ensuciaSubarbol(const UiElement& node)
        {
            node.dirty = UiElement::DirtyAll;
            for (const auto& child : node.children()) ensuciaSubarbol(*child);
        }

        void emitRawQuad(const UiTextureAtlas* atlas, const glm::vec2& pos, const glm::vec2& size,
                         const UiUvRect& uv, const glm::vec4& color,
                         const glm::vec4& params, const glm::vec4& effect,
                         const UiScissor& scissor, UiDrawData& out,
                         float dxTop = 0.0f, float dxBottom = 0.0f)
        {
            if (out.vertices.size() + 4 > kMaxVertices) return;

            // A batch can only carry ONE atlas and ONE scissor: whichever of the
            // two changes forces closing the current one and opening another. The mode, the
            // outline and the color go per vertex precisely so as NOT to show up
            // here.
            if (out.batches.empty() ||
                out.batches.back().atlas != atlas ||
                out.batches.back().scissor != scissor)
            {
                UiBatch batch{};
                batch.atlas      = atlas;
                batch.scissor    = scissor;
                batch.firstIndex = (uint32_t)out.indices.size();
                batch.indexCount = 0;
                out.batches.push_back(batch);
            }

            const uint16_t base = (uint16_t)out.vertices.size();

            const glm::vec2 esquina[4] = {
                {pos.x + dxTop,             pos.y         },
                {pos.x + size.x + dxTop,    pos.y         },
                {pos.x + size.x + dxBottom, pos.y + size.y},
                {pos.x + dxBottom,          pos.y + size.y}
            };

            // Without rotation not a single coordinate is touched: rotating by 0 would
            // still go through center + (p - center), and in floating point that does NOT
            // return p exactly. With the branch, a tree without rotation comes out bit for
            // bit as it did.
            glm::vec2 v0 = esquina[0], v1 = esquina[1], v2 = esquina[2], v3 = esquina[3];
            if (g_rot.activa)
            {
                v0 = rotaQuad(esquina[0]);
                v1 = rotaQuad(esquina[1]);
                v2 = rotaQuad(esquina[2]);
                v3 = rotaQuad(esquina[3]);
            }

            // Clockwise on screen starting at the top left. The
            // bottom vertex has the GREATER Y: +Y goes down.
            out.vertices.push_back({v0, {uv.u0, uv.v0}, color, params, effect});
            out.vertices.push_back({v1, {uv.u1, uv.v0}, color, params, effect});
            out.vertices.push_back({v2, {uv.u1, uv.v1}, color, params, effect});
            out.vertices.push_back({v3, {uv.u0, uv.v1}, color, params, effect});

            const uint16_t quad[6] = { (uint16_t)(base + 0), (uint16_t)(base + 1), (uint16_t)(base + 2),
                                       (uint16_t)(base + 2), (uint16_t)(base + 3), (uint16_t)(base + 0) };
            out.indices.insert(out.indices.end(), quad, quad + 6);
            out.batches.back().indexCount += 6;

            grabaQuad(atlas, scissor, out.vertices.data() + base, quad, base);
        }

        // ── Image draw modes ────────────────────────────────────────────────
        // All four are resolved HERE, on the CPU, emitting N quads with the same
        // atlas and the same scissor: that is why none of them splits the batch and none
        // needs a branch in the shader or a field in the vertex.

        // Size of the sprite IN ATLAS PIXELS. A loose texture (an atlas
        // with no entry of that name) measures what the whole atlas measures:
        // it is the same rule uvRect already uses when falling back to 0..1.
        glm::vec2 spriteNativeSize(const UiElement& node)
        {
            if (!node.atlas) return {0.0f, 0.0f};
            if (const UiSpriteRect* r = node.atlas->findSprite(node.sprite))
                return {r->width, r->height};
            return {(float)node.atlas->width(), (float)node.atlas->height()};
        }

        // Repeats the sprite at its native size. The last row and column are NOT
        // scaled: they are cut by UV, which is what distinguishes a tiling from a
        // stretch with more steps.
        bool emitTiled(const UiElement& node, const Image& img,
                       const glm::vec2& pos, const glm::vec2& size, const UiUvRect& uv,
                       const glm::vec4& color, const UiScissor& scissor, UiDrawData& out)
        {
            const glm::vec2 tile = spriteNativeSize(node);
            if (tile.x <= 0.0f || tile.y <= 0.0f) return false;

            const double cols = std::ceil((double)size.x / (double)tile.x);
            const double rows = std::ceil((double)size.y / (double)tile.y);
            if (cols <= 0.0 || rows <= 0.0) return false;

            // The cap is checked in double and BEFORE converting: with a 2 px
            // sprite and a large rect the product overflows a uint32.
            if (cols * rows > (double)img.maxTiles) return false;

            const float du = uv.u1 - uv.u0;
            const float dv = uv.v1 - uv.v0;

            for (uint32_t ry = 0; ry < (uint32_t)rows; ++ry)
            {
                const float y = (float)ry * tile.y;
                const float h = std::min(tile.y, size.y - y);
                if (h <= 0.0f) continue;

                for (uint32_t rx = 0; rx < (uint32_t)cols; ++rx)
                {
                    const float x = (float)rx * tile.x;
                    const float w = std::min(tile.x, size.x - x);
                    if (w <= 0.0f) continue;

                    UiUvRect cell{};
                    cell.u0 = uv.u0;
                    cell.v0 = uv.v0;
                    cell.u1 = uv.u0 + du * (w / tile.x);
                    cell.v1 = uv.v0 + dv * (h / tile.y);

                    emitRawQuad(node.atlas, {pos.x + x, pos.y + y}, {w, h}, cell, color,
                                glm::vec4(0.0f), glm::vec4(0.0f), scissor, out);
                }
            }
            return true;
        }

        // 9-slice. The corners ALWAYS come out at their native size, the edges
        // stretch only along their axis and the center fills the gap. If the edges of
        // one axis do not fit in the rect, both are scaled proportionally: it is the
        // only way they do not overlap, and shrinking is the opposite of
        // stretching a corner.
        bool emitSliced(const UiElement& node, const Image& img,
                        const glm::vec2& pos, const glm::vec2& size, const UiUvRect& uv,
                        const glm::vec4& color, const UiScissor& scissor, UiDrawData& out)
        {
            const glm::vec2 native = spriteNativeSize(node);
            if (native.x <= 0.0f || native.y <= 0.0f) return false;

            // Borders in sprite pixels, clamped to the sprite itself: borders
            // larger than the texture would give crossed UVs.
            float sl = std::max(0.0f, img.borderLeft);
            float sr = std::max(0.0f, img.borderRight);
            float st = std::max(0.0f, img.borderTop);
            float sb = std::max(0.0f, img.borderBottom);

            if (sl + sr > native.x && sl + sr > 0.0f)
            {
                const float k = native.x / (sl + sr);
                sl *= k; sr *= k;
            }
            if (st + sb > native.y && st + sb > 0.0f)
            {
                const float k = native.y / (st + sb);
                st *= k; sb *= k;
            }

            // And now in screen pixels: the same value, unless it does not fit
            // in the rect.
            float gl = sl, gr = sr, gt = st, gb = sb;
            if (gl + gr > size.x && gl + gr > 0.0f)
            {
                const float k = size.x / (gl + gr);
                gl *= k; gr *= k;
            }
            if (gt + gb > size.y && gt + gb > 0.0f)
            {
                const float k = size.y / (gt + gb);
                gt *= k; gb *= k;
            }

            const float du = uv.u1 - uv.u0;
            const float dv = uv.v1 - uv.v0;

            const float xs[3] = {pos.x, pos.x + gl, pos.x + size.x - gr};
            const float ws[3] = {gl, size.x - gl - gr, gr};
            const float ys[3] = {pos.y, pos.y + gt, pos.y + size.y - gb};
            const float hs[3] = {gt, size.y - gt - gb, gb};

            const float us[4] = {uv.u0, uv.u0 + du * (sl / native.x), uv.u1 - du * (sr / native.x), uv.u1};
            const float vs[4] = {uv.v0, uv.v0 + dv * (st / native.y), uv.v1 - dv * (sb / native.y), uv.v1};

            for (int row = 0; row < 3; ++row)
            {
                if (hs[row] <= 0.0f) continue;
                for (int col = 0; col < 3; ++col)
                {
                    if (ws[col] <= 0.0f) continue;
                    if (row == 1 && col == 1 && !img.fillCenter) continue;

                    const UiUvRect cell{us[col], vs[row], us[col + 1], vs[row + 1]};
                    emitRawQuad(node.atlas, {xs[col], ys[row]}, {ws[col], hs[row]}, cell, color,
                                glm::vec4(0.0f), glm::vec4(0.0f), scissor, out);
                }
            }
            return true;
        }

        // Clips position and UV AT THE SAME TIME: the visible piece shows ITS part of the
        // sprite, not the whole sprite squeezed. At 1 it returns false so that it
        // goes through the Normal path and gives vertex by vertex the same as before.
        bool emitFilled(const UiElement& node, const Image& img,
                        const glm::vec2& pos, const glm::vec2& size, const UiUvRect& uv,
                        const glm::vec4& color, const UiScissor& scissor, UiDrawData& out)
        {
            const float amount = std::min(1.0f, std::max(0.0f, img.fillAmount));
            if (amount <= 0.0f) return true;    // handled: not a single quad
            if (amount >= 1.0f) return false;   // identical to Normal

            glm::vec2 p = pos;
            glm::vec2 s = size;
            UiUvRect  r = uv;

            if (img.fillDirection == UiFillDirection::Horizontal)
            {
                s.x = size.x * amount;
                const float du = (uv.u1 - uv.u0) * amount;
                if (img.fillOrigin == UiFillOrigin::Start) { r.u1 = uv.u0 + du; }
                else                                      { p.x = pos.x + size.x - s.x; r.u0 = uv.u1 - du; }
            }
            else
            {
                s.y = size.y * amount;
                const float dv = (uv.v1 - uv.v0) * amount;
                if (img.fillOrigin == UiFillOrigin::Start) { r.v1 = uv.v0 + dv; }
                else                                      { p.y = pos.y + size.y - s.y; r.v0 = uv.v1 - dv; }
            }

            emitRawQuad(node.atlas, p, s, r, color,
                        glm::vec4(0.0f), glm::vec4(0.0f), scissor, out);
            return true;
        }

        void emitQuad(const UiElement& node, const glm::vec2& pos, const glm::vec2& size,
                      const UiScissor& scissor, float opacity, UiDrawData& out)
        {
            UiUvRect uv{};
            if (node.atlas) uv = node.atlas->uvRect(node.sprite);

            // The tree's accumulated opacity travels PER VERTEX: this way it does not split the
            // batch, which can only change by atlas or by scissor.
            glm::vec4 color = node.color;
            color.a *= opacity;

            // The Image modes are N quads of the same batch. The one that cannot
            // be resolved (without native size, or with more tiles than the limit) falls to
            // Normal instead of disappearing.
            const Image* img = node.asImage();
            if (img && img->mode != UiImageMode::Normal)
            {
                bool handled = false;
                switch (img->mode)
                {
                    case UiImageMode::Tiled:  handled = emitTiled (node, *img, pos, size, uv, color, scissor, out); break;
                    case UiImageMode::Sliced: handled = emitSliced(node, *img, pos, size, uv, color, scissor, out); break;
                    case UiImageMode::Filled: handled = emitFilled(node, *img, pos, size, uv, color, scissor, out); break;
                    default: break;
                }
                if (handled) return;
            }

            // params.x = 0: the shader does exactly what it always did.
            emitRawQuad(node.atlas, pos, size, uv, color,
                        glm::vec4(0.0f), glm::vec4(0.0f), scissor, out);
        }

        // ── Text ───────────────────────────────────────────────────────────
        // All the rich text is resolved HERE, on CPU: neither a shader nor a
        // pipeline nor another font. The parsing produces glyphs already "ironed out"
        // (with their color, their scale and their advance in world pixels) and the line
        // breaking works on that array, not on the string.

        // Current style at a point in the text. This is what the stack stacks.
        struct TextStyle
        {
            glm::vec4 color{1.0f};
            float     sizePx = 16.0f;
            bool      bold   = false;
            bool      italic = false;
        };

        enum class TagKind
        {
            None,
            Color,
            Size,
            Bold,
            Italic
        };

        // Each opening saves the style from OUTSIDE: the closing does not "undo" field
        // by field, it restores the entire previous one. So they nest without surprises.
        struct StyleEntry
        {
            TagKind   kind = TagKind::None;
            TextStyle previous{};
        };

        // A glyph already resolved. From here on neither the
        // string nor the style stack is looked at anymore.
        struct ShapedGlyph
        {
            const UiGlyph* glyph = nullptr;
            glm::vec2 scale{1.0f, 1.0f};
            glm::vec4 color{1.0f};
            float     kern    = 0.0f;   // correction BEFORE this glyph, in world px
            float     advance = 0.0f;   // in world px
            float     sizePx  = 0.0f;   // size of the segment, for the thickness of the bold
            bool      bold    = false;
            bool      italic  = false;
            bool      space   = false;
            bool      newline = false;  // '\n': neither drawn nor advances, only breaks
        };

        struct TextLine
        {
            uint32_t first = 0;
            uint32_t count = 0;   // already WITHOUT trailing spaces: they are not drawn nor aligned
            uint32_t spaces = 0;  // interior spaces: the ones that Justify distributes
            float    width = 0.0f;
            bool     hardBreak = false;   // '\n' broke it, so Justify does not touch it

            // The ellipsis goes at the end of s.glyphs, not inside the
            // line: clipping the line is moving a counter, not moving glyphs.
            uint32_t ellipsisFirst = 0;
            uint32_t ellipsisCount = 0;
        };

        struct TextScratch
        {
            std::vector<uint32_t>    cps;
            std::vector<ShapedGlyph> glyphs;
            std::vector<TextLine>    lines;
            std::vector<StyleEntry>  stack;
            glm::vec2                block{0.0f, 0.0f};
        };

        // The REUSED buffer between frames. build() is static, so the
        // "member" is this block per thread: after the first frame neither the parsing
        // nor the line breaking allocates anything.
        TextScratch& textScratch()
        {
            static thread_local TextScratch s;
            return s;
        }

        // ASCII comparison case-insensitive against a literal. The
        // tag name has to match ENTIRELY: "colorr" is not "color".
        bool tagIs(const std::vector<uint32_t>& cps, size_t first, size_t count, const char* name)
        {
            size_t i = 0;
            for (; i < count && name[i] != '\0'; ++i)
            {
                uint32_t c = cps[first + i];
                if (c >= 'A' && c <= 'Z') c += 32;
                if (c != (uint32_t)name[i]) return false;
            }
            return i == count && name[i] == '\0';
        }

        // #RRGGBB or #RRGGBBAA. Any other length or a digit that is not
        // hexadecimal = not a color.
        bool parseHexColor(const std::vector<uint32_t>& cps, size_t first, size_t count, glm::vec4& out)
        {
            if (count != 6 && count != 8) return false;

            uint32_t nib[8] = {};
            for (size_t i = 0; i < count; ++i)
            {
                const uint32_t c = cps[first + i];
                if      (c >= '0' && c <= '9') nib[i] = c - '0';
                else if (c >= 'a' && c <= 'f') nib[i] = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') nib[i] = c - 'A' + 10;
                else return false;
            }

            out.r = (float)(nib[0] * 16 + nib[1]) / 255.0f;
            out.g = (float)(nib[2] * 16 + nib[3]) / 255.0f;
            out.b = (float)(nib[4] * 16 + nib[5]) / 255.0f;
            out.a = count == 8 ? (float)(nib[6] * 16 + nib[7]) / 255.0f : 1.0f;
            return true;
        }

        // Unsigned decimal, with optional fractional part. No digits or with
        // garbage after means no number: "<size=>" is text, not a size 0 that
        // would make the segment disappear silently.
        bool parseNumber(const std::vector<uint32_t>& cps, size_t first, size_t count, float& out)
        {
            if (count == 0) return false;

            float  value  = 0.0f;
            size_t i      = 0;
            bool   digits = false;

            for (; i < count && cps[first + i] >= '0' && cps[first + i] <= '9'; ++i)
            {
                value  = value * 10.0f + (float)(cps[first + i] - '0');
                digits = true;
            }

            if (i < count && cps[first + i] == '.')
            {
                ++i;
                for (float f = 0.1f; i < count && cps[first + i] >= '0' && cps[first + i] <= '9'; ++i, f *= 0.1f)
                {
                    value += f * (float)(cps[first + i] - '0');
                    digits = true;
                }
            }

            if (!digits || i != count || value <= 0.0f) return false;
            out = value;
            return true;
        }

        // Tries to read a tag at 'at' (which points to a '<'). Returns how many
        // codepoints it consumes, INCLUDING '<' and '>', or 0 if there was no valid tag
        // there. That 0 is the entire rule: whatever is not understood is drawn.
        size_t applyTag(const std::vector<uint32_t>& cps, size_t at,
                        TextStyle& style, std::vector<StyleEntry>& stack)
        {
            // A tag cannot be infinite: without '>' nearby, or with another '<'
            // in between, this was text.
            constexpr size_t kMaxTag = 32;

            const size_t stop = std::min(cps.size(), at + 1 + kMaxTag);
            size_t end = at + 1;
            while (end < stop && cps[end] != '>')
            {
                if (cps[end] == '<') return 0;
                ++end;
            }
            if (end >= stop || cps[end] != '>') return 0;

            const size_t first    = at + 1;
            const size_t count    = end - first;
            const size_t consumed = end - at + 1;
            if (count == 0) return 0;

            if (cps[first] == '/')
            {
                TagKind kind = TagKind::None;
                if      (tagIs(cps, first, count, "/color")) kind = TagKind::Color;
                else if (tagIs(cps, first, count, "/size"))  kind = TagKind::Size;
                else if (tagIs(cps, first, count, "/b"))     kind = TagKind::Bold;
                else if (tagIs(cps, first, count, "/i"))     kind = TagKind::Italic;

                // An orphan or crossing closing does NOT pop blindly: it comes out as
                // text, which is the only thing that cannot lose information.
                if (kind == TagKind::None) return 0;
                if (stack.empty() || stack.back().kind != kind) return 0;

                style = stack.back().previous;
                stack.pop_back();
                return consumed;
            }

            if (tagIs(cps, first, count, "b"))
            {
                stack.push_back(StyleEntry{TagKind::Bold, style});
                style.bold = true;
                return consumed;
            }
            if (tagIs(cps, first, count, "i"))
            {
                stack.push_back(StyleEntry{TagKind::Italic, style});
                style.italic = true;
                return consumed;
            }
            if (count > 7 && tagIs(cps, first, 6, "color=") && cps[first + 6] == '#')
            {
                glm::vec4 color{};
                // Parsed BEFORE stacking: an invalid color does not leave the stack
                // touched, so the '</color>' after it does not match either.
                if (!parseHexColor(cps, first + 7, count - 7, color)) return 0;
                stack.push_back(StyleEntry{TagKind::Color, style});
                style.color = color;
                return consumed;
            }
            if (count > 5 && tagIs(cps, first, 5, "size="))
            {
                float sizePx = 0.0f;
                if (!parseNumber(cps, first + 5, count - 5, sizePx)) return 0;
                stack.push_back(StyleEntry{TagKind::Size, style});
                style.sizePx = sizePx;
                return consumed;
            }

            return 0;
        }

        // String -> array of glyphs with style, already in world pixels.
        void shapeText(const Text& text, const glm::vec2& worldScale, TextScratch& s)
        {
            const UiFont* font = text.font;

            UiFont::decodeUtf8(text.text, s.cps);
            s.glyphs.clear();
            s.stack.clear();

            TextStyle style{};
            style.color  = text.color;
            style.sizePx = text.fontSize;

            uint32_t previous = 0;

            for (size_t i = 0; i < s.cps.size(); )
            {
                const uint32_t cp = s.cps[i];

                if (cp == '<')
                {
                    const size_t consumed = applyTag(s.cps, i, style, s.stack);
                    if (consumed > 0)
                    {
                        i += consumed;
                        continue;
                    }
                    // It was not a tag: the '<' is still a character like any other.
                }

                ++i;

                if (cp == '\n')
                {
                    ShapedGlyph brk{};
                    brk.newline = true;
                    s.glyphs.push_back(brk);
                    previous = 0;
                    continue;
                }

                const UiGlyph* glyph = font->findGlyph(cp);
                if (!glyph)
                {
                    // Without glyph there is neither advance nor kerning pair that counts.
                    previous = 0;
                    continue;
                }

                // fontSize/bakeSize: the atlas was baked at ONE size and the rest
                // comes from scaling the quad, which is what MSDF does.
                const float unit = font->scaleFor(style.sizePx);

                ShapedGlyph g{};
                g.glyph   = glyph;
                g.scale   = glm::vec2(unit) * worldScale;
                g.color   = style.color;
                g.sizePx  = style.sizePx;
                g.bold    = style.bold;
                g.italic  = style.italic;
                g.space   = (cp == ' ' || cp == '\t');
                g.kern    = previous != 0 ? font->kerning(previous, cp) * g.scale.x : 0.0f;
                g.advance = glyph->advance * g.scale.x;
                s.glyphs.push_back(g);

                previous = cp;
            }
        }

        // Breaks into lines. With availWidth <= 0 (a Text without rect) there is nothing to
        // break against: there is one line per '\n', which is exactly what
        // the previous phase did.
        void breakLines(const Text& text, float availWidth, TextScratch& s)
        {
            s.lines.clear();

            const bool   wrap = text.wordWrap && availWidth > 0.0f;
            const size_t n    = s.glyphs.size();

            size_t   lineFirst     = 0;
            float    lineWidth     = 0.0f;   // with trailing spaces included
            uint32_t spaces        = 0;
            uint32_t trailing      = 0;      // spaces in a row at the end of the line
            float    trailingWidth = 0.0f;

            // The kerning of the FIRST glyph of a line does not count: its pair
            // is left on the line above.
            auto glyphWidth = [&](size_t idx) {
                return (idx == lineFirst ? 0.0f : s.glyphs[idx].kern) + s.glyphs[idx].advance;
            };

            auto closeLine = [&](size_t end, size_t next, bool hard) {
                TextLine line{};
                line.first     = (uint32_t)lineFirst;
                line.count     = (uint32_t)(end - lineFirst) - trailing;
                line.spaces    = spaces - trailing;
                line.width     = lineWidth - trailingWidth;
                line.hardBreak = hard;
                s.lines.push_back(line);

                lineFirst     = next;
                lineWidth     = 0.0f;
                spaces        = 0;
                trailing      = 0;
                trailingWidth = 0.0f;
            };

            size_t i = 0;
            while (i < n)
            {
                if (s.glyphs[i].newline)
                {
                    closeLine(i, i + 1, true);
                    ++i;
                    continue;
                }

                if (s.glyphs[i].space)
                {
                    const float w = glyphWidth(i);
                    lineWidth     += w;
                    trailingWidth += w;
                    ++spaces;
                    ++trailing;
                    ++i;
                    continue;
                }

                // The word is measured ENTIRELY before deciding where it goes: this is what
                // distinguishes a word wrap from a character wrap.
                size_t wordEnd   = i;
                float  wordWidth = 0.0f;
                while (wordEnd < n && !s.glyphs[wordEnd].space && !s.glyphs[wordEnd].newline)
                {
                    wordWidth += (wordEnd == i ? 0.0f : s.glyphs[wordEnd].kern) + s.glyphs[wordEnd].advance;
                    ++wordEnd;
                }

                const bool  atLineStart = (i == lineFirst);
                const float lead        = atLineStart ? 0.0f : s.glyphs[i].kern;

                if (wrap && !atLineStart && lineWidth + lead + wordWidth > availWidth)
                {
                    // The entire word goes down. Re-evaluated without advancing: already at
                    // line head it may continue without fitting.
                    closeLine(i, i, false);
                    continue;
                }

                if (wrap && atLineStart && wordWidth > availWidth)
                {
                    // It does not fit even alone: it is broken by glyph, with at least one per
                    // line (otherwise, a rect narrower than a glyph would
                    // never end).
                    for (size_t k = i; k < wordEnd; ++k)
                    {
                        if (k > lineFirst && lineWidth + glyphWidth(k) > availWidth)
                            closeLine(k, k, false);
                        lineWidth += glyphWidth(k);   // recalculated: lineFirst could have changed
                    }
                    trailing      = 0;
                    trailingWidth = 0.0f;
                    i = wordEnd;
                    continue;
                }

                lineWidth    += lead + wordWidth;
                trailing      = 0;
                trailingWidth = 0.0f;
                i = wordEnd;
            }

            // The last line (or the only one, even if the text is empty of drawable
            // glyphs) is closed the same way.
            if (lineFirst < n || s.lines.empty()) closeLine(n, n, false);
        }

        // Clips to the lines that fit in height and puts '…' at the end of the
        // last one. Without that glyph in the atlas it falls back to "...", and without any of the
        // two it leaves the text as it was before rather than draw a hole.
        void applyEllipsis(const Text& text, const glm::vec2& worldScale,
                           float availWidth, float availHeight, float lineStep, TextScratch& s)
        {
            if (text.overflow != UiTextOverflow::Ellipsis) return;
            if (s.lines.empty() || availWidth <= 0.0f) return;

            bool truncated = false;
            if (availHeight > 0.0f && lineStep > 0.0f)
            {
                const size_t maxLines = (size_t)std::max(1.0f, std::floor(availHeight / lineStep));
                if (s.lines.size() > maxLines)
                {
                    s.lines.resize(maxLines);
                    truncated = true;
                }
            }

            TextLine& last = s.lines.back();
            if (!truncated && last.width <= availWidth) return;

            const UiFont* font = text.font;
            const float   unit = font->scaleFor(text.fontSize);

            const UiGlyph* dots  = font->findGlyph(0x2026);   // '…'
            const UiGlyph* point = dots ? nullptr : font->findGlyph('.');
            if (!dots && !point) return;

            const UiGlyph* mark   = dots ? dots : point;
            const int      repeat = dots ? 1 : 3;
            const glm::vec2 scale = glm::vec2(unit) * worldScale;
            const float ellipsisWidth = mark->advance * scale.x * (float)repeat;

            // Glyphs are removed from the end until the dots fit. It can
            // stay at zero: just '…' is better than overflowing the rect.
            while (last.count > 0 && last.width + ellipsisWidth > availWidth)
            {
                const uint32_t     idx = last.first + last.count - 1;
                const ShapedGlyph& g   = s.glyphs[idx];
                last.width -= (idx == last.first ? 0.0f : g.kern) + g.advance;
                --last.count;
            }

            last.ellipsisFirst = (uint32_t)s.glyphs.size();
            last.ellipsisCount = (uint32_t)repeat;
            for (int r = 0; r < repeat; ++r)
            {
                ShapedGlyph g{};
                g.glyph   = mark;
                g.scale   = scale;
                g.color   = text.color;
                g.sizePx  = text.fontSize;
                g.advance = mark->advance * scale.x;
                s.glyphs.push_back(g);
            }
            last.width += ellipsisWidth;
        }

        // Parsing + breaking + ellipsis, and along the way the block size:
        // width of the longest line and height by lineHeight. It is the same as
        // what the measurement pass consumes and what the drawing pass emits.
        void layoutText(const Text& text, const glm::vec2& worldScale,
                        float availWidth, float availHeight, TextScratch& s)
        {
            shapeText(text, worldScale, s);
            breakLines(text, availWidth, s);

            const float lineStep = text.font->lineHeight() * text.font->scaleFor(text.fontSize) * worldScale.y;
            applyEllipsis(text, worldScale, availWidth, availHeight, lineStep, s);

            s.block = glm::vec2(0.0f);
            for (const TextLine& line : s.lines) s.block.x = std::max(s.block.x, line.width);
            s.block.y = (float)s.lines.size() * lineStep;
        }

        glm::vec2 measureTextBlock(const Text& text, float availWidth, float availHeight)
        {
            TextScratch& s = textScratch();
            layoutText(text, glm::vec2(1.0f), availWidth, availHeight, s);
            return s.block;
        }

        void emitText(const Text& text, const glm::vec2& worldPos, const glm::vec2& worldSize,
                      const glm::vec2& worldScale, UiScissor scissor, float opacity, UiDrawData& out)
        {
            const UiFont* font = text.font;

            const float unit = font->scaleFor(text.fontSize);
            if (unit <= 0.0f) return;

            TextScratch& s = textScratch();
            layoutText(text, worldScale, worldSize.x, worldSize.y, s);
            if (s.glyphs.empty() || s.lines.empty()) return;

            // Clip reuses the usual scissor, so the only cost is
            // breaking the batch; Overflow touches nothing and does not break it.
            if (text.overflow == UiTextOverflow::Clip)
            {
                scissor = intersectScissor(scissor, scissorFromRect(worldPos, worldSize));
                if (scissor.empty()) return;
            }

            // The font atlas is the batch key, just like any
            // other: two texts of the same font fall in the same draw.
            const UiTextureAtlas* atlas = &font->atlas();

            glm::vec4 outline = text.outlineColor;
            outline.a *= opacity;
            glm::vec4 shadow = text.shadowColor;
            shadow.a *= opacity;

            const glm::vec2 shadowOffset = text.shadowOffset * worldScale;
            const bool hasShadow = shadow.a > 0.0f &&
                                   (text.shadowOffset.x != 0.0f || text.shadowOffset.y != 0.0f);

            const float lineStep = font->lineHeight() * unit * worldScale.y;
            const float avail    = worldSize.x;

            // Vertical displacement of the ENTIRE BLOCK. With Top it comes out 0 and the
            // baseline is at an ascent from the top edge, which is what
            // this did before vAlign existed.
            //
            // The block height is the line height per line; it uses that and not
            // the actual box of the glyphs on purpose, so "CENTERED" and
            // "centered" are at the same height instead of bouncing according to capitals or descenders.
            float vOffset = 0.0f;
            if (text.vAlign != UiTextVAlign::Top && worldSize.y > 0.0f)
            {
                const float blockHeight = lineStep * static_cast<float>(s.lines.size());
                const float slack       = worldSize.y - blockHeight;
                vOffset = (text.vAlign == UiTextVAlign::Middle) ? slack * 0.5f : slack;
            }

            // The shadow is an ENTIRE pass in front: same atlas and same
            // scissor, so it does not break the batch nor need another pass.
            for (int pass = hasShadow ? 0 : 1; pass < 2; ++pass)
            {
                const bool isShadow = (pass == 0);

                // The baseline is at an ascent from the top edge of the rect, plus
                // what the block's vertical alignment displaces.
                float baseline = worldPos.y + vOffset + font->ascent() * unit * worldScale.y;
                if (isShadow) baseline += shadowOffset.y;

                for (size_t li = 0; li < s.lines.size(); ++li)
                {
                    const TextLine& line = s.lines[li];

                    float startX       = worldPos.x;
                    float extraPerSpace = 0.0f;

                    if (avail > 0.0f)
                    {
                        const bool isLast = (li + 1 == s.lines.size());
                        if (text.align == UiTextAlign::Center)
                            startX += (avail - line.width) * 0.5f;
                        else if (text.align == UiTextAlign::Right)
                            startX += avail - line.width;
                        else if (text.align == UiTextAlign::Justify &&
                                 !isLast && !line.hardBreak && line.spaces > 0 && line.width < avail)
                            extraPerSpace = (avail - line.width) / (float)line.spaces;
                    }
                    if (isShadow) startX += shadowOffset.x;

                    float pen = startX;

                    // Part 0 = the line; part 1 = the ellipsis,
                    // which live at the end of the array.
                    for (int part = 0; part < 2; ++part)
                    {
                        const uint32_t first = part == 0 ? line.first : line.ellipsisFirst;
                        const uint32_t count = part == 0 ? line.count : line.ellipsisCount;

                        for (uint32_t k = 0; k < count; ++k)
                        {
                            const ShapedGlyph& g = s.glyphs[first + k];
                            if (part > 0 || k > 0) pen += g.kern;

                            // A space has no outline: it advances the cursor and that is it.
                            if (g.glyph->rect.width > 0.0f && g.glyph->rect.height > 0.0f)
                            {
                                const glm::vec2 pos{pen + g.glyph->bearingX * g.scale.x,
                                                    baseline - g.glyph->bearingY * g.scale.y};
                                const glm::vec2 size{g.glyph->rect.width  * g.scale.x,
                                                     g.glyph->rect.height * g.scale.y};

                                // screenPxRange: the range of the distance field
                                // scaled to the size it will be drawn at.
                                const float screenPxRange = font->pixelRange() * g.scale.y;

                                // <b> fattens by the SAME channel as the outline:
                                // without its own outline, the "edge" is painted
                                // the fill color and the glyph comes out fatter.
                                const float bold = g.bold ? text.boldStrength * g.sizePx * worldScale.y : 0.0f;

                                glm::vec4 fill = g.color;
                                fill.a *= opacity;

                                const glm::vec4 params{1.0f, screenPxRange,
                                                       (isShadow ? 0.0f : text.outlineWidth) + bold, 0.0f};

                                glm::vec4 effect = isShadow ? glm::vec4(0.0f) : outline;
                                if (bold > 0.0f && (isShadow || text.outlineWidth <= 0.0f))
                                    effect = isShadow ? shadow : fill;

                                // <i> is a shear on the baseline: the
                                // top edge goes to the right and the
                                // bottom to the left. Not a single UV changes.
                                float dxTop    = 0.0f;
                                float dxBottom = 0.0f;
                                if (g.italic)
                                {
                                    dxTop    = text.italicSkew * (baseline - pos.y);
                                    dxBottom = text.italicSkew * (baseline - (pos.y + size.y));
                                }

                                emitRawQuad(atlas, pos, size, font->glyphUv(*g.glyph),
                                            isShadow ? shadow : fill, params,
                                            effect, scissor, out, dxTop, dxBottom);
                            }

                            pen += g.advance;
                            if (part == 0 && g.space) pen += extraPerSpace;
                        }
                    }

                    baseline += lineStep;
                }
            }
        }

        // ── Measurement Pass ──────────────────────────────────────────────────
        // Content size fitters go bottom-up (the parent's size comes
        // from the children) and placement goes top-down, so two
        // passes are needed. The tree is NOT mutated: the measurement lives in a local vector in
        // pre-order and the placement indexes it.
        struct MeasuredNode
        {
            glm::vec2 size{0.0f, 0.0f};   // local size already resolved (fitters applied)
            // Nodes occupied by this subtree, including this one. This is what allows
            // skipping from one child to the next without traversing it: emitNode exits before
            // due to !visible and empty scissor, and with a cursor that only advances
            // one by one those exits would desynchronize all measurements.
            uint32_t subtree = 1;
        };

        bool participatesInLayout(const UiElement& node)
        {
            return node.visible && !node.ignoreLayout;
        }

        // The space occupied by a child within its parent's layout, in
        // LOCAL units of the parent. Grid imposes the cell and with it consumes the scale
        // of the child (otherwise, a different scale would break the grid); Horizontal and
        // Vertical respect the size of the child already scaled.
        glm::vec2 layoutSlotSize(const UiElement& parent, const UiElement& child, const glm::vec2& childSize)
        {
            if (parent.layoutMode == UiLayoutMode::Grid) return parent.cellSize;
            return childSize * child.scale;
        }

        // columns == 0 = the ones that fit in width. It is measured against node.size.x and
        // NOT against the already-adjusted size: with fitWidth they would be mutually
        // recursive. Measurement and placement call this with the same data.
        uint32_t gridColumns(const UiElement& node, uint32_t count)
        {
            if (count == 0) return 1;
            if (node.columns > 0) return std::min(node.columns, count);

            const float inner = node.size.x - node.paddingLeft - node.paddingRight;
            const float step  = node.cellSize.x + node.spacing.x;

            uint32_t cols = 1;
            if (step > 0.0f && inner > 0.0f)
                cols = (uint32_t)std::max(1.0f, std::floor((inner + node.spacing.x) / step));
            return std::min(cols, count);
        }

        void measureNode(const UiElement& node, std::vector<MeasuredNode>& out)
        {
            const size_t self = out.size();
            out.push_back(MeasuredNode{});

            // Content accumulates DURING the recursion: so there is no need for
            // a vector of children per node, only the measurement vector.
            float    mainSum  = 0.0f;
            float    crossMax = 0.0f;
            uint32_t laid     = 0;

            for (const auto& child : node.children())
            {
                const size_t childIndex = out.size();
                measureNode(*child, out);

                if (node.layoutMode == UiLayoutMode::None) continue;
                if (!participatesInLayout(*child)) continue;

                const glm::vec2 slot = layoutSlotSize(node, *child, out[childIndex].size);
                ++laid;
                if (node.layoutMode == UiLayoutMode::Vertical)
                {
                    mainSum  += slot.y;
                    crossMax  = std::max(crossMax, slot.x);
                }
                else if (node.layoutMode == UiLayoutMode::Horizontal)
                {
                    mainSum  += slot.x;
                    crossMax  = std::max(crossMax, slot.y);
                }
            }

            glm::vec2 size = node.size;

            // The text block IS the content of a Text: the fitter grows
            // up to it just as a container grows up to its children, and from there
            // the parent's layout already sums the measurement like any other.
            // It is measured against node.size (not against the already-adjusted one) for the same
            // reason as gridColumns: with fitWidth they would be mutually recursive.
            const Text* asText = node.asText();
            const bool  fitsText = asText && asText->font && asText->font->hasGlyphs() &&
                                   !asText->text.empty() && (node.fitWidth || node.fitHeight);

            if (fitsText)
            {
                const glm::vec2 block = measureTextBlock(*asText,
                                                         node.size.x - node.paddingLeft - node.paddingRight,
                                                         node.size.y - node.paddingTop  - node.paddingBottom);

                if (node.fitWidth)  size.x = node.paddingLeft + block.x + node.paddingRight;
                if (node.fitHeight) size.y = node.paddingTop  + block.y + node.paddingBottom;
            }
            else if (node.layoutMode != UiLayoutMode::None && (node.fitWidth || node.fitHeight))
            {
                const float gaps = laid > 1 ? (float)(laid - 1) : 0.0f;

                glm::vec2 content{0.0f, 0.0f};
                if (node.layoutMode == UiLayoutMode::Grid)
                {
                    const uint32_t cols = gridColumns(node, laid);
                    const uint32_t rows = laid > 0 ? (laid + cols - 1) / cols : 0;
                    if (laid > 0)
                    {
                        content.x = (float)cols * node.cellSize.x + (float)(cols - 1) * node.spacing.x;
                        content.y = (float)rows * node.cellSize.y + (float)(rows - 1) * node.spacing.y;
                    }
                }
                else if (node.layoutMode == UiLayoutMode::Horizontal)
                {
                    content.x = mainSum + gaps * node.spacing.x;
                    content.y = crossMax;
                }
                else
                {
                    content.y = mainSum + gaps * node.spacing.y;
                    content.x = crossMax;
                }

                if (node.fitWidth)  size.x = node.paddingLeft + content.x + node.paddingRight;
                if (node.fitHeight) size.y = node.paddingTop  + content.y + node.paddingBottom;
            }

            out[self].size    = size;
            out[self].subtree = (uint32_t)(out.size() - self);
        }

        // ── Placement Pass ──────────────────────────────────────────────────

        // Rect already resolved by the parent's layout. Without it, the node is placed
        // by its anchors as always.
        struct LayoutPlacement
        {
            bool      active = false;
            glm::vec2 worldPos{0.0f, 0.0f};
            glm::vec2 worldSize{0.0f, 0.0f};
        };

        float crossOffset(float inner, float slot, UiCrossAlign align)
        {
            if (align == UiCrossAlign::Center) return (inner - slot) * 0.5f;
            if (align == UiCrossAlign::End)    return inner - slot;
            return 0.0f;
        }

        // A subtree that the emitter does NOT traverse (invisible, or clipped to zero) is
        // left without a resolved rect. Marking it is what prevents the input's hit test from
        // continuing to use the rect from the previous frame, which now means nothing.
        void invalidateRects(const UiElement& node)
        {
            node.rectValid = false;
            for (const auto& child : node.children()) invalidateRects(*child);
        }

        // Resolves the node's placement and leaves it IN ITS CACHE. It is exactly
        // the calculation that emitNode did inline; it is separated only to be able to
        // skip it entirely when neither Transform nor Layout are dirty.
        void colocaNodo(const UiElement& node, uint32_t index, const std::vector<MeasuredNode>& measured,
                        const glm::vec2& parentPos, const glm::vec2& parentScale,
                        const glm::vec2& parentSize, const LayoutPlacement& placement,
                        const UiScissor& scissor, float parentOpacity)
        {
            const glm::vec2 worldScale = parentScale * node.scale;
            const glm::vec2 localSize  = measured[index].size;

            glm::vec2 worldPos{0.0f, 0.0f};
            glm::vec2 worldSize = localSize * worldScale;

            if (placement.active)
            {
                // The layout placed it: its anchors, its margins and its
                // position are not read.
                worldPos  = placement.worldPos;
                worldSize = placement.worldSize;
            }
            else
            {
                // Axis by axis. With anchorMin == anchorMax the formula comes out exactly the
                // same as always: anchor on the PARENT's rect, pivot on
                // the OWN, and with everything at {0,0}, parentPos + position*parentScale.
                // With anchorMin != anchorMax the axis STRETCHES and the margins rule:
                // size and pivot of that axis are not read.
                // node.rotation is still intentionally ignored.
                if (node.anchorMin.x != node.anchorMax.x)
                {
                    const float x0 = parentPos.x + node.anchorMin.x * parentSize.x + node.marginLeft  * parentScale.x;
                    const float x1 = parentPos.x + node.anchorMax.x * parentSize.x - node.marginRight * parentScale.x;
                    worldPos.x  = x0;
                    worldSize.x = x1 - x0;
                }
                else
                {
                    worldPos.x = parentPos.x
                               + node.anchorMin.x * parentSize.x
                               + node.position.x * parentScale.x
                               - node.pivot.x * worldSize.x;
                }

                if (node.anchorMin.y != node.anchorMax.y)
                {
                    const float y0 = parentPos.y + node.anchorMin.y * parentSize.y + node.marginTop    * parentScale.y;
                    const float y1 = parentPos.y + node.anchorMax.y * parentSize.y - node.marginBottom * parentScale.y;
                    worldPos.y  = y0;
                    worldSize.y = y1 - y0;
                }
                else
                {
                    worldPos.y = parentPos.y
                               + node.anchorMin.y * parentSize.y
                               + node.position.y * parentScale.y
                               - node.pivot.y * worldSize.y;
                }
            }

            const float opacity = parentOpacity * node.opacity;

            // A container without size (the root, or a group that only groups) does
            // not define an anchor area: its children continue anchoring against the parent's
            // instead of collapsing all against its corner.
            const glm::vec2 childArea = (worldSize.x > 0.0f && worldSize.y > 0.0f) ? worldSize : parentSize;

            // The mask: the element's rect pushed inward by its
            // insets, INTERSECTED with what came from the parent (never a
            // replacement, so a nested mask can only clip more).
            // With maskSelf the element itself enters it; without it only its
            // descendants.
            // ONLY point where the canvas's scale enters: from here down
            // everything is in render pixels, and from here up (measurement,
            // layout, anchors, margins, padding) in reference units. With
            // scale 1 and origin {0,0} the float that comes out is the SAME bit by bit.
            const glm::vec2 screenPos  = g_xf.origen + worldPos * g_xf.escala;
            const glm::vec2 screenSize = worldSize * g_xf.escala;

            UiScissor selfScissor  = scissor;
            UiScissor childScissor = scissor;
            bool      culled       = false;

            if (node.clipChildren && node.maskEnabled)
            {
                const glm::vec2 maskPos {screenPos.x + node.maskInsetLeft * g_xf.escala,
                                         screenPos.y + node.maskInsetTop  * g_xf.escala};
                // Insets that cross leave size <= 0 and scissorFromRect
                // returns empty: NEVER a negative width/height, which in a
                // VkRect2D is a crash.
                const glm::vec2 maskSize{screenSize.x - node.maskInsetLeft * g_xf.escala - node.maskInsetRight  * g_xf.escala,
                                         screenSize.y - node.maskInsetTop  * g_xf.escala - node.maskInsetBottom * g_xf.escala};

                childScissor = intersectScissor(scissor, scissorFromRect(maskPos, maskSize));

                if (node.maskSelf)
                {
                    selfScissor = childScissor;
                    // Empty intersection: neither this node nor any of its children
                    // can be seen, so not even a draw with
                    // width/height 0 is emitted.
                    culled = selfScissor.empty();
                }
            }

            node.cacheWorldPos     = worldPos;
            node.cacheWorldSize    = worldSize;
            node.cacheWorldScale   = worldScale;
            node.cacheChildArea    = childArea;
            node.cacheScreenPos    = screenPos;
            node.cacheScreenSize   = screenSize;
            node.cacheSelfScissor  = selfScissor;
            node.cacheChildScissor = childScissor;
            node.cacheOpacity      = opacity;
            node.cacheSelfCulled   = culled;
            node.cacheGeomValid    = true;
        }

        void emitNode(const UiElement& node, uint32_t index, const std::vector<MeasuredNode>& measured,
                      const glm::vec2& parentPos, const glm::vec2& parentScale,
                      const glm::vec2& parentSize, const LayoutPlacement& placement,
                      UiScissor scissor, float parentOpacity, UiDrawData& out)
        {
            // enabled is NOT looked at here: it is for input, not for drawing.
            if (!node.visible) { invalidateRects(node); return; }

            // Neither Transform nor Layout dirty means they are not dirty either in
            // any ancestor (the two GO UP and DOWN), so the inputs of
            // the placement are the SAME and it would come out bit by bit the same.
            const bool geomFresca =
                node.cacheGeomValid &&
                (node.dirty & (UiElement::DirtyTransform | UiElement::DirtyLayout)) == 0;

            if (!geomFresca)
                colocaNodo(node, index, measured, parentPos, parentScale, parentSize,
                           placement, scissor, parentOpacity);

            const glm::vec2 worldPos    = node.cacheWorldPos;
            const glm::vec2 worldScale  = node.cacheWorldScale;
            const glm::vec2 worldSize   = node.cacheWorldSize;
            const glm::vec2 childArea   = node.cacheChildArea;
            const glm::vec2 screenPos   = node.cacheScreenPos;
            const glm::vec2 screenSize  = node.cacheScreenSize;
            const UiScissor selfScissor = node.cacheSelfScissor;
            const float     opacity     = node.cacheOpacity;
            UiScissor       childScissor = node.cacheChildScissor;

            if (node.cacheSelfCulled) { invalidateRects(node); return; }

            // The rect is already resolved: it is SAVED so the input reuses it
            // without traversing the tree again. It does not alter a vertex or a batch.
            // It is the SAME scissor with which the node is drawn, which is what
            // makes the hit test respect the mask without its own code.
            node.screenPos     = screenPos;
            node.screenSize    = screenSize;
            node.screenScissor = selfScissor;
            node.rectValid     = true;

            // A Text with font draws its glyphs INSTEAD of its own quad: if
            // not, each text would drag a white rectangle behind it. Without font
            // (or with nothing to say) it behaves like its base again, which is what
            // makes a half-configured Text not disappear silently.
            // Here is ALL the savings. A node without a single dirty bit emitted its
            // vertices from the same inputs as now, so they are dumped
            // as-is instead of measuring glyphs or cutting up a sliced again. The
            // tree traversal does not change: the work is skipped, not the node.
            if (node.cacheValid && node.dirty == 0)
            {
                reproduceCache(node, out);
            }
            else
            {
                node.cacheVertices.clear();
                node.cacheIndices.clear();
                node.cacheSegments.clear();

                const Text* text = node.asText();
                const bool  drawsText = text && text->font && text->font->hasGlyphs() && !text->text.empty();

                // The rotation applies to what THIS node emits (its quad, its N
                // quads of Image or its glyphs) and nothing else: the children return
                // to the state from before, and the scissor from above is the AABB unrotated.
                const QuadRotation rotPrevia = g_rot;
                if (node.rotation != 0.0f)
                {
                    g_rot.activa = true;
                    g_rot.sen    = std::sin(node.rotation);
                    g_rot.cs     = std::cos(node.rotation);
                    g_rot.centro = screenPos + node.pivot * screenSize;
                }

                g_rec = &node;
                if (drawsText)
                    emitText(*text, screenPos, screenSize, worldScale * g_xf.escala, selfScissor, opacity, out);
                else if (node.drawable && worldSize.x > 0.0f && worldSize.y > 0.0f)
                    emitQuad(node, screenPos, screenSize, selfScissor, opacity, out);
                g_rec = nullptr;

                g_rot = rotPrevia;

                node.cacheValid = true;
                ++node.rebuildCount;
                ++g_rebuilt;
            }

            node.dirty = 0;

            // Empty mask with maskSelf false: the element has already been drawn
            // entirely, but not a single vertex of its children passes through its mask.
            if (childScissor.empty())
            {
                for (const auto& child : node.children()) invalidateRects(*child);
                return;
            }

            scissor = childScissor;

            // The index of the first child is the next in pre-order, and each
            // sibling is an entire subtree away from the previous one.
            uint32_t childIndex = index + 1;

            if (node.layoutMode == UiLayoutMode::None)
            {
                for (const auto& child : node.children())
                {
                    emitNode(*child, childIndex, measured, worldPos, worldScale, childArea,
                             LayoutPlacement{}, scissor, opacity, out);
                    childIndex += measured[childIndex].subtree;
                }
                return;
            }

            // The layout works already in world pixels: so a container
            // stretched or placed by another layout distributes over its actual rect.
            const glm::vec2 padMin{node.paddingLeft * worldScale.x, node.paddingTop * worldScale.y};
            const glm::vec2 padMax{node.paddingRight * worldScale.x, node.paddingBottom * worldScale.y};
            const glm::vec2 gap = node.spacing * worldScale;
            const glm::vec2 origin = worldPos + padMin;
            const glm::vec2 inner  = worldSize - padMin - padMax;

            uint32_t laidCount = 0;
            for (const auto& child : node.children())
                if (participatesInLayout(*child)) ++laidCount;

            const uint32_t cols = node.layoutMode == UiLayoutMode::Grid ? gridColumns(node, laidCount) : 1;

            float    cursor    = 0.0f;   // advance on the main axis, in world pixels
            uint32_t laidIndex = 0;

            for (const auto& child : node.children())
            {
                const uint32_t ci = childIndex;
                childIndex += measured[ci].subtree;

                if (!participatesInLayout(*child))
                {
                    // ignoreLayout is drawn anchored as if the parent did not have
                    // layout; the invisible is not even visited (but its hole in the
                    // measurement vector has already been skipped above).
                    if (child->visible)
                        emitNode(*child, ci, measured, worldPos, worldScale, childArea,
                                 LayoutPlacement{}, scissor, opacity, out);
                    continue;
                }

                const glm::vec2 slot = layoutSlotSize(node, *child, measured[ci].size) * worldScale;

                LayoutPlacement placed{};
                placed.active    = true;
                placed.worldSize = slot;

                if (node.layoutMode == UiLayoutMode::Horizontal)
                {
                    placed.worldPos.x = origin.x + cursor;
                    placed.worldPos.y = origin.y + crossOffset(inner.y, slot.y, node.crossAlign);
                    cursor += slot.x + gap.x;
                }
                else if (node.layoutMode == UiLayoutMode::Vertical)
                {
                    placed.worldPos.y = origin.y + cursor;
                    placed.worldPos.x = origin.x + crossOffset(inner.x, slot.x, node.crossAlign);
                    cursor += slot.y + gap.y;
                }
                else
                {
                    // Grid: the cell is uniform, so the position comes from the
                    // row and column, not from an accumulated cursor.
                    const uint32_t col = laidIndex % cols;
                    const uint32_t row = laidIndex / cols;
                    placed.worldPos.x = origin.x + (float)col * (slot.x + gap.x);
                    placed.worldPos.y = origin.y + (float)row * (slot.y + gap.y);
                }

                ++laidIndex;
                emitNode(*child, ci, measured, worldPos, worldScale, childArea,
                         placed, scissor, opacity, out);
            }
        }

    }

    // ── CPU ─────────────────────────────────────────────────────────────────

    void UiSpriteBatch::build(const UiCanvas& canvas, uint32_t width, uint32_t height, UiDrawData& out)
    {
        // ── Useful area, in this order and no other ───────────────────────────
        // (a) the entire render.
        float x0 = 0.0f;
        float y0 = 0.0f;
        float x1 = (float)width;
        float y1 = (float)height;

        // (b) the safe area insets, in real pixels. Negative ones are ignored
        // (expanding the useful area above the render does not mean anything) and
        // insets that cross leave area 0, never a rect backwards.
        if (canvas.safeArea.left   > 0.0f) x0 += canvas.safeArea.left;
        if (canvas.safeArea.top    > 0.0f) y0 += canvas.safeArea.top;
        if (canvas.safeArea.right  > 0.0f) x1 -= canvas.safeArea.right;
        if (canvas.safeArea.bottom > 0.0f) y1 -= canvas.safeArea.bottom;
        if (x1 < x0) x1 = x0;
        if (y1 < y0) y1 = y0;

        float uw = x1 - x0;
        float uh = y1 - y0;

        // (c) the aspect ratio, clipped CENTERED. What is left over are bars
        // (letterbox if the area is wider than expected, pillarbox if it is
        // taller) and the UI does not occupy them.
        const float ar = canvas.aspectRatio;
        if (ar > 0.0f && std::isfinite(ar) && uw > 0.0f && uh > 0.0f)
        {
            if (uw > uh * ar)
            {
                const float nw = uh * ar;
                x0 += (uw - nw) * 0.5f;
                uw  = nw;
            }
            else
            {
                const float nh = uw / ar;
                y0 += (uh - nh) * 0.5f;
                uh  = nh;
            }
        }

        // (d) a UNIQUE and UNIFORM scale, of the useful area.
        float escala = 1.0f;
        switch (canvas.scaleMode)
        {
        case UiScaleMode::ScaleWithScreenSize:
        {
            const float refW = canvas.referenceResolution.x;
            const float refH = canvas.referenceResolution.y;
            if (refW > 0.0f && refH > 0.0f)
            {
                const float rx = uw / refW;
                const float ry = uh / refH;

                float m = canvas.matchWidthOrHeight;
                if (!(m >= 0.0f)) m = 0.0f;    // also catches NaN
                if (m > 1.0f)     m = 1.0f;

                switch (canvas.screenMatch)
                {
                case UiScreenMatch::Expand: escala = (rx < ry) ? rx : ry; break;
                case UiScreenMatch::Shrink: escala = (rx > ry) ? rx : ry; break;
                case UiScreenMatch::MatchWidthOrHeight:
                default:
                    // LOGARITHMIC lerp: with m = 0 it follows the width, with m = 1 the
                    // height, and in between it falls between the two without one side
                    // eating the other (which is what happens with arithmetic mean).
                    escala = std::pow(rx, 1.0f - m) * std::pow(ry, m);
                    break;
                }
                escala *= canvas.scaleFactor;
            }
            break;
        }
        case UiScaleMode::ConstantPhysicalSize:
        {
            // screenDpi <= 0 means "unknown": the fallback prevents an OS that does not
            // report it from leaving the UI at scale 0.
            const float dpi = (canvas.screenDpi > 0.0f) ? canvas.screenDpi : canvas.fallbackDpi;
            if (canvas.referenceDpi > 0.0f) escala = (dpi / canvas.referenceDpi) * canvas.scaleFactor;
            break;
        }
        case UiScaleMode::ConstantPixelSize:
        default:
            escala = canvas.scaleFactor;
            break;
        }

        // A scale <= 0 or not finite does not shrink the UI: it makes it disappear or
        // fills it with NaN. Falls back to 1 and continues.
        if (!std::isfinite(escala) || escala <= 0.0f) escala = 1.0f;

        const glm::vec2 nuevoOrigen{x0, y0};
        const glm::vec2 nuevaRef{uw / escala, uh / escala};

        // If the render size, scale, origin or useful area changes,
        // the placement of ALL nodes is moved: there is no cache that
        // stays valid, so the entire tree is dirtied before traversing it.
        if (canvas.m_lastWidth     != width  ||
            canvas.m_lastHeight    != height ||
            canvas.m_uiScale       != escala ||
            canvas.m_uiOrigin      != nuevoOrigen ||
            canvas.m_referenceSize != nuevaRef)
        {
            ensuciaSubarbol(canvas.root());
        }

        canvas.m_uiScale       = escala;
        canvas.m_uiOrigin      = nuevoOrigen;
        canvas.m_referenceSize = nuevaRef;
        canvas.m_lastWidth     = width;
        canvas.m_lastHeight    = height;
        canvas.m_rebuiltNodes  = 0;
        g_rebuilt              = 0;

        // Empty canvas: it is not measured, the vector is not reserved, nothing is traversed.
        // The resolution is already resolved, so the getters work the same.
        if (canvas.root().children().empty()) return;

        // Useful area of 0 pixels (safe area that eats the entire render): there is
        // nowhere to draw and the rects from the previous frame cannot remain.
        if (uw <= 0.0f || uh <= 0.0f) { invalidateRects(canvas.root()); return; }

        // The root scissor IS the useful area: what falls on the aspect
        // ratio bars or outside the safe area is not drawn. The hit test reads this same
        // scissor, so the events come out coherent without its own code.
        const UiScissor full = scissorFromRect({x0, y0}, {uw, uh});

        // The "parent" of the root is the useful area in REFERENCE units: the
        // first-level elements anchor against that rect and do not see the scale.
        const glm::vec2 screen = canvas.m_referenceSize;

        g_xf.origen = {x0, y0};
        g_xf.escala = escala;

        // Bottom-up measurement first (resolves the fitters), then placement.
        std::vector<MeasuredNode> measured;
        measureNode(canvas.root(), measured);

        emitNode(canvas.root(), 0, measured, glm::vec2(0.0f), glm::vec2(1.0f), screen,
                 LayoutPlacement{}, full, 1.0f, out);

        canvas.m_rebuiltNodes = g_rebuilt;

        // Leave it off for the next canvas and for the hit test.
        g_xf = CanvasXform{};
    }

    // ── GPU ─────────────────────────────────────────────────────────────────

    void UiSpriteBatch::init(GpuDevice& gpu, GpuResources& res, VkRenderPass renderPass,
                             VkSampleCountFlagBits samples)
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo dslInfo{};
        dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslInfo.bindingCount = 1;
        dslInfo.pBindings    = &binding;
        if (vkCreateDescriptorSetLayout(gpu.device(), &dslInfo, nullptr, &m_descLayout) != VK_SUCCESS)
            throw std::runtime_error("failed to create ui descriptor set layout!");

        // Own pool: one set per atlas (plus the white one). 32 is more than enough for a
        // game's UI and the sets live the entire process.
        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = 32;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;
        poolInfo.maxSets       = 32;
        if (vkCreateDescriptorPool(gpu.device(), &poolInfo, nullptr, &m_descPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create ui descriptor pool!");

        res.createTextureSampler(m_sampler);

        const uint8_t white[4] = { 255, 255, 255, 255 };
        res.createSolidColorImage(white, m_whiteImage, m_whiteMemory);
        // UNORM and not the default SRGB of createTextureImageView: the image is
        // created by createSolidColorImage as R8G8B8A8_UNORM, and the view has to
        // declare EXACTLY that format (the image is not MUTABLE_FORMAT).
        // It is the same visually — 255 is 1.0 in both — but it is a
        // validation error and undefined behavior.
        res.createTextureImageView(m_whiteImage, m_whiteView, VK_FORMAT_R8G8B8A8_UNORM);

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool     = m_descPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts        = &m_descLayout;
        if (vkAllocateDescriptorSets(gpu.device(), &allocInfo, &m_whiteSet) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate ui white descriptor set!");

        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView   = m_whiteView;
        imageInfo.sampler     = m_sampler;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = m_whiteSet;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &imageInfo;
        vkUpdateDescriptorSets(gpu.device(), 1, &write, 0, nullptr);

        // VERTEX | FRAGMENT: the mat4 is read by ui.vert and the linearOutput flag is
        // read by ui.frag, but the block is ONE ONLY and the range has to cover
        // the two members for the two stages.
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcr.offset     = 0;
        pcr.size       = kUiPushConstantSize;

        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &m_descLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &pcr;
        if (vkCreatePipelineLayout(gpu.device(), &pli, nullptr, &m_layout) != VK_SUCCESS)
            throw std::runtime_error("failed to create ui pipeline layout!");

        // The screen one never tests depth: it goes on top of everything.
        createPipeline(gpu, renderPass, samples, false, m_pipeline);
    }

    void UiSpriteBatch::recreatePipeline(GpuDevice& gpu, VkRenderPass renderPass,
                                         VkSampleCountFlagBits samples)
    {
        if (m_layout == VK_NULL_HANDLE) return;   // no init (headless without UI)
        if (m_pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(gpu.device(), m_pipeline, nullptr);
            m_pipeline = VK_NULL_HANDLE;
        }
        createPipeline(gpu, renderPass, samples, false, m_pipeline);
    }

    void UiSpriteBatch::initWorldPipelines(GpuDevice& gpu, VkRenderPass scenePass,
                                           VkSampleCountFlagBits samples)
    {
        if (m_layout == VK_NULL_HANDLE) return;   // no init (headless without UI)

        // Destroy first: this function is also the "recreate" of the AA change,
        // and the caller has already done vkDeviceWaitIdle before touching the
        // renderpass.
        if (m_worldPipelineDepth != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(gpu.device(), m_worldPipelineDepth, nullptr);
            m_worldPipelineDepth = VK_NULL_HANDLE;
        }
        if (m_worldPipelineNoDepth != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(gpu.device(), m_worldPipelineNoDepth, nullptr);
            m_worldPipelineNoDepth = VK_NULL_HANDLE;
        }

        createPipeline(gpu, scenePass, samples, true,  m_worldPipelineDepth);
        createPipeline(gpu, scenePass, samples, false, m_worldPipelineNoDepth);
    }

    void UiSpriteBatch::createPipeline(GpuDevice& gpu, VkRenderPass renderPass,
                                       VkSampleCountFlagBits samples,
                                       bool depthTest, VkPipeline& out)
    {
        VkShaderModule vert = loadShaderModule(gpu.device(), "shaders/ui.vert.spv");
        VkShaderModule frag = loadShaderModule(gpu.device(), "shaders/ui.frag.spv");

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName  = "main";

        VkVertexInputBindingDescription bindingDesc{};
        bindingDesc.binding   = 0;
        bindingDesc.stride    = sizeof(UiVertex);
        bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        // The five locations have to say THE SAME as ui.vert: a
        // mismatch of offset or format gives neither error nor warning, just
        // garbage on screen.
        VkVertexInputAttributeDescription attrs[5]{};
        attrs[0].location = 0;
        attrs[0].binding  = 0;
        attrs[0].format   = VK_FORMAT_R32G32_SFLOAT;
        attrs[0].offset   = offsetof(UiVertex, pos);
        attrs[1].location = 1;
        attrs[1].binding  = 0;
        attrs[1].format   = VK_FORMAT_R32G32_SFLOAT;
        attrs[1].offset   = offsetof(UiVertex, uv);
        attrs[2].location = 2;
        attrs[2].binding  = 0;
        attrs[2].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[2].offset   = offsetof(UiVertex, color);
        attrs[3].location = 3;
        attrs[3].binding  = 0;
        attrs[3].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[3].offset   = offsetof(UiVertex, params);
        attrs[4].location = 4;
        attrs[4].binding  = 0;
        attrs[4].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[4].offset   = offsetof(UiVertex, effect);

        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount   = 1;
        vi.pVertexBindingDescriptions      = &bindingDesc;
        vi.vertexAttributeDescriptionCount = 5;
        vi.pVertexAttributeDescriptions    = attrs;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{};
        vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        // NONE: the quads come out in the order the batcher emits them and their
        // orientation does not depend on the rest of the engine's frontFace.
        rs.cullMode    = VK_CULL_MODE_NONE;
        rs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        // The same samples as the pass it is compiled against: the UI pass always goes
        // to one, and the SCENE pass to whatever the AA mode says. A
        // pipeline that declares others is not compatible with its pass.
        ms.rasterizationSamples = samples;

        // depthTest off = the screen variant (its pass has nothing to
        // test, it goes on top of everything) and the world one "always on top".
        // On = the world one occluded, so a wall hides the sign.
        //
        // depthWrite ALWAYS off in ALL THREE: the UI goes with alpha, and writing
        // depth would make quads of the same canvas clip each other
        // according to the order they came out of the batcher (the text would hide the
        // panel behind it instead of blending with it).
        //
        // LESS_OR_EQUAL and not LESS: a canvas stuck to a wall's surface
        // has to be visible, not lose the tie against it.
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable  = depthTest ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = VK_FALSE;
        ds.depthCompareOp   = depthTest ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_ALWAYS;

        // Straight alpha (SRC_ALPHA / ONE_MINUS_SRC_ALPHA): the sprite's color is NOT
        // premultiplied.
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable         = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp        = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp        = VK_BLEND_OP_ADD;
        blend.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments    = &blend;

        VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dyn{};
        dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates    = dynStates;

        VkGraphicsPipelineCreateInfo pci{};
        pci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pci.stageCount          = 2;
        pci.pStages             = stages;
        pci.pVertexInputState   = &vi;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState      = &vp;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState   = &ms;
        pci.pDepthStencilState  = &ds;
        pci.pColorBlendState    = &cb;
        pci.pDynamicState       = &dyn;
        pci.layout              = m_layout;
        pci.renderPass          = renderPass;
        pci.subpass             = 0;

        if (vkCreateGraphicsPipelines(gpu.device(), VK_NULL_HANDLE, 1, &pci, nullptr, &out) != VK_SUCCESS)
            throw std::runtime_error("failed to create ui pipeline!");

        vkDestroyShaderModule(gpu.device(), vert, nullptr);
        vkDestroyShaderModule(gpu.device(), frag, nullptr);
    }

    bool UiSpriteBatch::registerAtlas(GpuDevice& gpu, UiTextureAtlas& atlas)
    {
        if (m_descPool == VK_NULL_HANDLE || !atlas.loaded()) return false;

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool     = m_descPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts        = &m_descLayout;

        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(gpu.device(), &allocInfo, &set) != VK_SUCCESS) return false;

        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView   = atlas.view();
        imageInfo.sampler     = m_sampler;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &imageInfo;
        vkUpdateDescriptorSets(gpu.device(), 1, &write, 0, nullptr);

        atlas.setDescriptorSet(set);
        return true;
    }

    void UiSpriteBatch::ensureBuffers(GpuDevice& gpu, int frame, uint32_t vertexCount, uint32_t indexCount)
    {
        auto grow = [&](VkBuffer& buffer, VkDeviceMemory& memory, void*& mapped, uint32_t& capacity,
                        uint32_t needed, uint32_t initial, size_t elementSize, VkBufferUsageFlags usage)
        {
            if (needed <= capacity) return;

            uint32_t next = capacity ? capacity : initial;
            while (next < needed) next *= 2;

            if (buffer != VK_NULL_HANDLE)
            {
                mapped = nullptr;
                vkDestroyBuffer(gpu.device(), buffer, nullptr);
                vkFreeMemory(gpu.device(), memory, nullptr);
                buffer = VK_NULL_HANDLE;
                memory = VK_NULL_HANDLE;
            }

            const VkDeviceSize size = (VkDeviceSize)next * elementSize;

            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size        = size;
            bufferInfo.usage       = usage;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(gpu.device(), &bufferInfo, nullptr, &buffer) != VK_SUCCESS)
                throw std::runtime_error("failed to create ui buffer!");

            VkMemoryRequirements memReq;
            vkGetBufferMemoryRequirements(gpu.device(), buffer, &memReq);

            VkMemoryAllocateInfo allocInfo{};
            allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize  = memReq.size;
            allocInfo.memoryTypeIndex = gpu.findMemoryType(memReq.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (vkAllocateMemory(gpu.device(), &allocInfo, nullptr, &memory) != VK_SUCCESS)
                throw std::runtime_error("failed to allocate ui buffer memory!");
            vkBindBufferMemory(gpu.device(), buffer, memory, 0);

            // Persistent mapping: completely rewritten each frame.
            vkMapMemory(gpu.device(), memory, 0, size, 0, &mapped);
            capacity = next;
        };

        grow(m_vertexBuffers[frame], m_vertexMemory[frame], m_vertexMapped[frame], m_vertexCapacity[frame],
             vertexCount, kInitialVertexCapacity, sizeof(UiVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        grow(m_indexBuffers[frame], m_indexMemory[frame], m_indexMapped[frame], m_indexCapacity[frame],
             indexCount, kInitialIndexCapacity, sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    }

    void UiSpriteBatch::destroyBuffers(GpuDevice& gpu, int frame)
    {
        if (m_vertexBuffers[frame] != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(gpu.device(), m_vertexBuffers[frame], nullptr);
            vkFreeMemory(gpu.device(), m_vertexMemory[frame], nullptr);
        }
        if (m_indexBuffers[frame] != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(gpu.device(), m_indexBuffers[frame], nullptr);
            vkFreeMemory(gpu.device(), m_indexMemory[frame], nullptr);
        }
        m_vertexBuffers[frame]  = VK_NULL_HANDLE;
        m_vertexMemory[frame]   = VK_NULL_HANDLE;
        m_vertexMapped[frame]   = nullptr;
        m_vertexCapacity[frame] = 0;
        m_indexBuffers[frame]   = VK_NULL_HANDLE;
        m_indexMemory[frame]    = VK_NULL_HANDLE;
        m_indexMapped[frame]    = nullptr;
        m_indexCapacity[frame]  = 0;
    }

    void UiSpriteBatch::beginFrame(GpuDevice& gpu, int frame, uint32_t totalVertices, uint32_t totalIndices)
    {
        // It is sized against the ACCUMULATED total of ALL canvas of the FRAME — the world ones,
        // which are recorded in the scene pass, and the screen ones, which are recorded in the UI
        // pass — not against the size of just one nor against the size of one pass: with either of
        // those two, the first canvas would reserve plenty but the next one would find the buffer
        // already full and ensureBuffers would recreate it MID-frame, invalidating the bind that a
        // previous canvas already left recorded in the command buffer (it would point to a
        // destroyed VkBuffer). Full contract in the header: ONE time per frame, before the SCENE
        // pass (which is where the frame's first record falls, because it runs before the UI pass).
        if (totalVertices > 0 || totalIndices > 0)
            ensureBuffers(gpu, frame, totalVertices, totalIndices);

        // A new frame starts distributing from the beginning of the buffer.
        m_frameVertexCursor[frame] = 0;
        m_frameIndexCursor[frame]  = 0;
    }

    void UiSpriteBatch::record(GpuDevice& gpu, VkCommandBuffer cmd, const UiDrawData& data,
                               const glm::mat4& transform,
                               VkExtent2D canvasExtent, VkExtent2D fbExtent, int frame)
    {
        (void)gpu;   // the buffer growth was already solved by beginFrame()

        // linearOutput = false: the UI pass writes into an SRGB attachment and the
        // hardware already encodes when writing. scissorCompleto = false: on screen
        // the batcher's scissor DOES apply, scaled to the framebuffer.
        recordInto(cmd, data, transform, m_pipeline, false, false,
                   canvasExtent, fbExtent, frame);
    }

    void UiSpriteBatch::recordWorld(GpuDevice& gpu, VkCommandBuffer cmd, const UiDrawData& data,
                                    const glm::mat4& transform, bool depthTest,
                                    VkExtent2D canvasExtent, VkExtent2D fbExtent, int frame)
    {
        (void)gpu;   // the buffer growth was already solved by beginFrame()

        const VkPipeline pipeline = depthTest ? m_worldPipelineDepth : m_worldPipelineNoDepth;

        // linearOutput = true: the scene pass is LINEAR HDR and ui.frag has to
        // undo the gamma or the color comes out washed out. scissorCompleto = true:
        // the canvas is projected and the batcher's rect no longer represents it
        // (known limitation — clipChildren does not clip in world mode).
        recordInto(cmd, data, transform, pipeline, true, true,
                   canvasExtent, fbExtent, frame);
    }

    void UiSpriteBatch::recordInto(VkCommandBuffer cmd, const UiDrawData& data,
                                   const glm::mat4& transform, VkPipeline pipeline,
                                   bool linearOutput, bool scissorCompleto,
                                   VkExtent2D canvasExtent, VkExtent2D fbExtent, int frame)
    {
        // Empty canvas = neither a command, nor a created buffer, nor a mapping. It is the
        // condition that makes the 3D scene come out EXACTLY the same as before.
        if (data.empty() || pipeline == VK_NULL_HANDLE) return;
        if (canvasExtent.width == 0 || canvasExtent.height == 0) return;
        if (fbExtent.width == 0 || fbExtent.height == 0) return;
        if (!m_vertexMapped[frame] || !m_indexMapped[frame]) return;

        // Sub-allocation INSIDE the frame's buffer: each canvas writes from
        // where the previous one left off, not always at offset 0.
        const uint32_t vertexBase = bumpUiCursor(m_frameVertexCursor[frame], (uint32_t)data.vertices.size());
        const uint32_t indexBase  = bumpUiCursor(m_frameIndexCursor[frame],  (uint32_t)data.indices.size());

        // The cursor is sized by beginFrame() with the total of the PASS. If
        // someone calls record() without it, or with a short total (or twice
        // in a row without beginFrame() reserving again), this would write
        // OUTSIDE the mapped memory: a host write that no validation
        // layer sees. Better not to draw that canvas than corrupt the
        // buffer.
        if (!uiCursorFits(vertexBase, (uint32_t)data.vertices.size(), m_vertexCapacity[frame]) ||
            !uiCursorFits(indexBase,  (uint32_t)data.indices.size(),  m_indexCapacity[frame]))
            return;

        std::memcpy(static_cast<UiVertex*>(m_vertexMapped[frame]) + vertexBase,
                    data.vertices.data(), data.vertices.size() * sizeof(UiVertex));
        std::memcpy(static_cast<uint16_t*>(m_indexMapped[frame]) + indexBase,
                    data.indices.data(), data.indices.size() * sizeof(uint16_t));

        // The scissors DO go in framebuffer pixels: a VkRect2D knows
        // no other space. Outward (floor/ceil) for the same reason as
        // scissorFromRect, and with both extents equal the entire thing comes out intact.
        const double sx = (double)fbExtent.width  / (double)canvasExtent.width;
        const double sy = (double)fbExtent.height / (double)canvasExtent.height;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

        // One block for both stages, and exactly
        // kUiPushConstantSize useful bytes are pushed: sizeof(UiPushConstants) adds 12
        // padding bytes after (glm::mat4 alignment) that would be garbage.
        UiPushConstants pc{};
        pc.transform    = transform;
        pc.linearOutput = linearOutput ? 1 : 0;
        vkCmdPushConstants(cmd, m_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, kUiPushConstantSize, &pc);

        // The bind offset is THAT of THIS canvas within the shared buffer
        // of the frame: with it already applied, the batch indices (batch.firstIndex
        // below) stay LOCAL to this canvas, without touching them.
        const VkDeviceSize vertexOffset = (VkDeviceSize)vertexBase * sizeof(UiVertex);
        const VkDeviceSize indexOffset  = (VkDeviceSize)indexBase  * sizeof(uint16_t);
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertexBuffers[frame], &vertexOffset);
        vkCmdBindIndexBuffer(cmd, m_indexBuffers[frame], indexOffset, VK_INDEX_TYPE_UINT16);

        // The complete framebuffer rectangle. It serves two things: it is the
        // scissor of ALL batches of a world canvas, and it is what
        // leaves the dynamic state when exiting in both cases.
        VkRect2D full{};
        full.offset = {0, 0};
        full.extent = fbExtent;

        // WORLD canvas: one scissor and that is it, out of the loop. The batcher's is in
        // canvas pixels and here the canvas is PROJECTED — it can come out
        // rotated, in perspective or split by the screen edge — so
        // there is no axis-aligned VkRect2D that represents it and applying the
        // rect without projecting would hide pieces that actually are visible.
        // KNOWN LIMITATION: clipChildren does not clip in a world canvas.
        if (scissorCompleto)
            vkCmdSetScissor(cmd, 0, 1, &full);

        for (const UiBatch& batch : data.batches)
        {
            if (batch.indexCount == 0 || batch.scissor.empty()) continue;

            if (scissorCompleto)
            {
                // No clipping per batch: the scissor is already set above. The
                // batch still breaks by scissor in the batcher (it is the key
                // to grouping), only here all the pieces are drawn
                // whole. Careful: `batch.scissor.empty()` from above IS respected
                // — a node whose clip stayed at zero does not emit anything on
                // screen nor in the world.
                VkDescriptorSet set = (batch.atlas && batch.atlas->descriptorSet() != VK_NULL_HANDLE)
                                    ? batch.atlas->descriptorSet() : m_whiteSet;
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &set, 0, nullptr);
                vkCmdDrawIndexed(cmd, batch.indexCount, 1, batch.firstIndex, 0, 0);
                continue;
            }

            int64_t x0 = (int64_t)std::floor(batch.scissor.x * sx);
            int64_t y0 = (int64_t)std::floor(batch.scissor.y * sy);
            int64_t x1 = (int64_t)std::ceil((batch.scissor.x + (double)batch.scissor.width)  * sx);
            int64_t y1 = (int64_t)std::ceil((batch.scissor.y + (double)batch.scissor.height) * sy);

            // Clipped to the framebuffer: a scissor that goes outside is invalid, and
            // the rounding outward can push the edge one pixel.
            x0 = std::max<int64_t>(x0, 0);
            y0 = std::max<int64_t>(y0, 0);
            x1 = std::min<int64_t>(x1, fbExtent.width);
            y1 = std::min<int64_t>(y1, fbExtent.height);
            if (x1 <= x0 || y1 <= y0) continue;

            VkRect2D rect{};
            rect.offset.x      = (int32_t)x0;
            rect.offset.y      = (int32_t)y0;
            rect.extent.width  = (uint32_t)(x1 - x0);
            rect.extent.height = (uint32_t)(y1 - y0);
            vkCmdSetScissor(cmd, 0, 1, &rect);

            VkDescriptorSet set = (batch.atlas && batch.atlas->descriptorSet() != VK_NULL_HANDLE)
                                ? batch.atlas->descriptorSet() : m_whiteSet;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &set, 0, nullptr);
            vkCmdDrawIndexed(cmd, batch.indexCount, 1, batch.firstIndex, 0, 0);
        }

        // The scissor is dynamic state of the command buffer: leaving it clipped
        // would affect what is recorded after in this same buffer. And in the
        // SCENE pass this matters MORE than in the UI pass, because behind a world
        // canvas there is still the rest of the pass.
        vkCmdSetScissor(cmd, 0, 1, &full);
    }

    void UiSpriteBatch::shutdown(GpuDevice& gpu)
    {
        for (int i = 0; i < kFrames; ++i) destroyBuffers(gpu, i);

        if (m_pipeline != VK_NULL_HANDLE)   vkDestroyPipeline(gpu.device(), m_pipeline, nullptr);
        // The two world ones: without this they are a leak that ONLY comes out through
        // vkDestroyDevice, that is when closing and the process is already half dead.
        if (m_worldPipelineDepth != VK_NULL_HANDLE)
            vkDestroyPipeline(gpu.device(), m_worldPipelineDepth, nullptr);
        if (m_worldPipelineNoDepth != VK_NULL_HANDLE)
            vkDestroyPipeline(gpu.device(), m_worldPipelineNoDepth, nullptr);
        if (m_layout != VK_NULL_HANDLE)     vkDestroyPipelineLayout(gpu.device(), m_layout, nullptr);
        if (m_whiteView != VK_NULL_HANDLE)  vkDestroyImageView(gpu.device(), m_whiteView, nullptr);
        if (m_whiteImage != VK_NULL_HANDLE) vkDestroyImage(gpu.device(), m_whiteImage, nullptr);
        if (m_whiteMemory != VK_NULL_HANDLE)vkFreeMemory(gpu.device(), m_whiteMemory, nullptr);
        if (m_sampler != VK_NULL_HANDLE)    vkDestroySampler(gpu.device(), m_sampler, nullptr);
        // The pool takes with it all the sets (the white one and the atlas ones),
        // so there is no need to free them one by one.
        if (m_descPool != VK_NULL_HANDLE)   vkDestroyDescriptorPool(gpu.device(), m_descPool, nullptr);
        if (m_descLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(gpu.device(), m_descLayout, nullptr);

        m_pipeline            = VK_NULL_HANDLE;
        m_worldPipelineDepth   = VK_NULL_HANDLE;
        m_worldPipelineNoDepth = VK_NULL_HANDLE;
        m_layout      = VK_NULL_HANDLE;
        m_whiteView   = VK_NULL_HANDLE;
        m_whiteImage  = VK_NULL_HANDLE;
        m_whiteMemory = VK_NULL_HANDLE;
        m_sampler     = VK_NULL_HANDLE;
        m_descPool    = VK_NULL_HANDLE;
        m_descLayout  = VK_NULL_HANDLE;
        m_whiteSet    = VK_NULL_HANDLE;
    }
}
