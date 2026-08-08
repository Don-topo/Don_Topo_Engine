#include "DonTopo/UI/UiSpriteBatch.h"

#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiFont.h"
#include "DonTopo/UI/UiWidgets.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/GpuResources.h"

#include <glm/ext/matrix_clip_space.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace DonTopo
{
    namespace
    {
        // uint16 para los índices: 4 vértices por quad, así que el techo son
        // 16383 quads por frame. Pasado ese punto se deja de emitir en vez de
        // desbordar el índice en silencio.
        constexpr size_t kMaxVertices = 65532;

        constexpr uint32_t kInitialVertexCapacity = 1024;   // 256 quads
        constexpr uint32_t kInitialIndexCapacity  = 1536;

        UiScissor scissorFromRect(const glm::vec2& pos, const glm::vec2& size)
        {
            if (size.x <= 0.0f || size.y <= 0.0f) return {};

            // Hacia fuera (floor/ceil): recortar de menos deja el borde del
            // sprite; recortar de más se come una fila de píxeles.
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

        // Intersección, NO reemplazo: un hijo nunca puede pintar fuera de lo que
        // su padre ya había recortado.
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

        // Un quad con TODO explícito: atlas, UVs, color y los dos vec4 de
        // parámetros. El de texto no puede salir de node.sprite porque un glyph
        // no tiene nombre, así que la regla de romper el lote vive aquí y solo
        // aquí.
        // dxTop/dxBottom desplazan en X el borde superior y el inferior: es la
        // cizalla de la cursiva, que así no necesita ni matriz ni un vértice más
        // ancho. A 0 el quad sale exactamente igual que siempre.
        // ── Rotación del quad ───────────────────────────────────────────────
        // Estado de módulo y no un parámetro más: la rotación tiene que llegar
        // a los N quads que emite un Image y a cada glyph de un Text sin
        // ensuciar seis firmas. El build de la UI es de un solo hilo y
        // determinista, y quien la enciende (emitNode) la deja como estaba
        // antes de bajar a los hijos.
        struct QuadRotation
        {
            bool      activa = false;
            float     sen    = 0.0f;
            float     cs     = 1.0f;
            glm::vec2 centro{0.0f, 0.0f};
        };

        QuadRotation g_rot;

        // ── Resolución del canvas ───────────────────────────────────────────
        // El árbol entero (layout, anclas, texto, animaciones) se resuelve en
        // unidades de REFERENCIA y no sabe que esto existe. La escala y el
        // origen entran UNA sola vez, al pasar el rect ya resuelto de cada nodo
        // a píxeles del render. Estado de módulo por lo mismo que g_rot: el
        // build de la UI es de un solo hilo y determinista.
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

        // ── Caché por nodo ──────────────────────────────────────────────────
        // Mientras g_rec apunta a un nodo, cada quad que salga se copia TAMBIÉN
        // a su caché, troceado por (atlas, scissor) igual que los lotes. Nulo
        // fuera de la emisión de un nodo, o sea siempre que la caché sirve.
        const UiElement* g_rec = nullptr;

        // Nodos que han reemitido en el build en curso.
        uint32_t g_rebuilt = 0;

        // Un quad recién emitido, tal cual, dentro de la caché del nodo activo.
        // Los índices se guardan RELATIVOS al primer vértice del nodo.
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

        // Vuelca la caché de un nodo limpio. Reabre lote con EL MISMO criterio
        // que emitRawQuad y rebasa los índices sobre la base de destino: salen
        // los mismos uint16 y el mismo corte de lotes que si se hubiera emitido.
        void reproduceCache(const UiElement& node, UiDrawData& out)
        {
            size_t vRead = 0;
            size_t iRead = 0;

            for (const UiElement::CacheSegment& seg : node.cacheSegments)
            {
                // El mismo tope que emitRawQuad. Con la misma escena y el mismo
                // orden no salta nunca; está para que no pueda salir un índice
                // por encima de 65535 si algún día salta.
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

            // Un lote solo puede llevar UN atlas y UN scissor: cualquiera de los
            // dos que cambie obliga a cerrar el actual y abrir otro. El modo, el
            // outline y el color van por vértice justamente para NO aparecer
            // aquí.
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

            // Sin rotación NO se toca ni una coordenada: girar por 0 pasaría
            // igualmente por centro + (p - centro), y eso en coma flotante NO
            // devuelve p exacto. Con la rama, un árbol sin rotation sale bit a
            // bit como salía.
            glm::vec2 v0 = esquina[0], v1 = esquina[1], v2 = esquina[2], v3 = esquina[3];
            if (g_rot.activa)
            {
                v0 = rotaQuad(esquina[0]);
                v1 = rotaQuad(esquina[1]);
                v2 = rotaQuad(esquina[2]);
                v3 = rotaQuad(esquina[3]);
            }

            // Sentido horario en pantalla empezando arriba a la izquierda. El
            // vértice inferior tiene la Y MAYOR: +Y va hacia abajo.
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

        // ── Modos de dibujo del Image ───────────────────────────────────────
        // Los cuatro se resuelven AQUÍ, en CPU, emitiendo N quads con el mismo
        // atlas y el mismo scissor: por eso ninguno parte el lote y ninguno
        // necesita una rama en el shader ni un campo en el vértice.

        // Tamaño del sprite EN PÍXELES DEL ATLAS. Una textura suelta (un atlas
        // sin ninguna entrada con ese nombre) mide lo que mide el atlas entero:
        // es la misma regla que ya usa uvRect al caer a 0..1.
        glm::vec2 spriteNativeSize(const UiElement& node)
        {
            if (!node.atlas) return {0.0f, 0.0f};
            if (const UiSpriteRect* r = node.atlas->findSprite(node.sprite))
                return {r->width, r->height};
            return {(float)node.atlas->width(), (float)node.atlas->height()};
        }

        // Repite el sprite a su tamaño nativo. La fila y la columna del final NO
        // se escalan: se recortan por UV, que es lo que distingue un tiling de un
        // stretch con más pasos.
        bool emitTiled(const UiElement& node, const Image& img,
                       const glm::vec2& pos, const glm::vec2& size, const UiUvRect& uv,
                       const glm::vec4& color, const UiScissor& scissor, UiDrawData& out)
        {
            const glm::vec2 tile = spriteNativeSize(node);
            if (tile.x <= 0.0f || tile.y <= 0.0f) return false;

            const double cols = std::ceil((double)size.x / (double)tile.x);
            const double rows = std::ceil((double)size.y / (double)tile.y);
            if (cols <= 0.0 || rows <= 0.0) return false;

            // El tope se comprueba en double y ANTES de convertir: con un sprite
            // de 2 px y un rect grande el producto se sale de un uint32.
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

        // 9-slice. Las esquinas salen SIEMPRE a su tamaño nativo, los bordes se
        // estiran solo en su eje y el centro rellena el hueco. Si los bordes de
        // un eje no caben en el rect se escalan los dos proporcionalmente: es la
        // única forma de que no se solapen, y encoger es lo contrario de
        // estirar una esquina.
        bool emitSliced(const UiElement& node, const Image& img,
                        const glm::vec2& pos, const glm::vec2& size, const UiUvRect& uv,
                        const glm::vec4& color, const UiScissor& scissor, UiDrawData& out)
        {
            const glm::vec2 native = spriteNativeSize(node);
            if (native.x <= 0.0f || native.y <= 0.0f) return false;

            // Bordes en píxeles del sprite, acotados al propio sprite: unos
            // bordes mayores que la textura darían UVs cruzadas.
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

            // Y ahora en píxeles de pantalla: el mismo valor, salvo que no quepa
            // en el rect.
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

        // Recorta posición y UV A LA VEZ: el trozo visible enseña SU parte del
        // sprite, no el sprite entero comprimido. A 1 devuelve false para que
        // salga por el camino Normal y dé vértice a vértice lo mismo que antes.
        bool emitFilled(const UiElement& node, const Image& img,
                        const glm::vec2& pos, const glm::vec2& size, const UiUvRect& uv,
                        const glm::vec4& color, const UiScissor& scissor, UiDrawData& out)
        {
            const float amount = std::min(1.0f, std::max(0.0f, img.fillAmount));
            if (amount <= 0.0f) return true;    // manejado: ni un quad
            if (amount >= 1.0f) return false;   // idéntico a Normal

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

            // La opacidad acumulada del árbol viaja POR VÉRTICE: así no parte el
            // lote, que solo puede cambiar por atlas o por scissor.
            glm::vec4 color = node.color;
            color.a *= opacity;

            // Los modos del Image son N quads del mismo lote. El que no puede
            // resolverse (sin tamaño nativo, o con más tiles que el tope) cae a
            // Normal en vez de desaparecer.
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

            // params.x = 0: el shader hace exactamente lo de siempre.
            emitRawQuad(node.atlas, pos, size, uv, color,
                        glm::vec4(0.0f), glm::vec4(0.0f), scissor, out);
        }

        // ── Texto ───────────────────────────────────────────────────────────
        // Todo el rich text se resuelve AQUÍ, en CPU: ni un shader ni un
        // pipeline ni una fuente más. El parseo produce glyphs ya "planchados"
        // (con su color, su escala y su avance en píxeles de mundo) y el corte
        // de líneas trabaja sobre ese array, no sobre la cadena.

        // Estilo vigente en un punto del texto. Es lo que la pila apila.
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

        // Cada apertura guarda el estilo de FUERA: el cierre no "deshace" campo
        // a campo, restaura el de antes entero. Así anidan sin sorpresas.
        struct StyleEntry
        {
            TagKind   kind = TagKind::None;
            TextStyle previous{};
        };

        // Un glyph ya resuelto. A partir de aquí no se vuelve a mirar ni la
        // cadena ni la pila de estilos.
        struct ShapedGlyph
        {
            const UiGlyph* glyph = nullptr;
            glm::vec2 scale{1.0f, 1.0f};
            glm::vec4 color{1.0f};
            float     kern    = 0.0f;   // corrección ANTES de este glyph, en px de mundo
            float     advance = 0.0f;   // en px de mundo
            float     sizePx  = 0.0f;   // tamaño del tramo, para el grosor de la negrita
            bool      bold    = false;
            bool      italic  = false;
            bool      space   = false;
            bool      newline = false;  // '\n': ni se dibuja ni avanza, solo corta
        };

        struct TextLine
        {
            uint32_t first = 0;
            uint32_t count = 0;   // ya SIN los espacios del final: no se dibujan ni se alinean
            uint32_t spaces = 0;  // espacios interiores: los que reparte Justify
            float    width = 0.0f;
            bool     hardBreak = false;   // la cortó un '\n', así que Justify no la toca

            // Los puntos suspensivos van al final de s.glyphs, no dentro de la
            // línea: recortar la línea es mover un contador, no mover glyphs.
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

        // El buffer REUTILIZADO entre frames. build() es estática, así que el
        // "miembro" es este bloque por hilo: tras el primer frame ni el parseo
        // ni el corte de líneas asignan nada.
        TextScratch& textScratch()
        {
            static thread_local TextScratch s;
            return s;
        }

        // Comparación ASCII sin distinguir mayúsculas contra un literal. El
        // nombre del tag tiene que casar ENTERO: "colorr" no es "color".
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

        // #RRGGBB o #RRGGBBAA. Cualquier otra longitud o un dígito que no sea
        // hexadecimal = no es un color.
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

        // Decimal sin signo, con parte fraccionaria opcional. Sin dígitos o con
        // basura detrás no hay número: "<size=>" es texto, no un tamaño 0 que
        // haría desaparecer el tramo en silencio.
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

        // Intenta leer un tag en 'at' (que apunta a un '<'). Devuelve cuántos
        // codepoints consume, INCLUIDOS '<' y '>', o 0 si ahí no había un tag
        // válido. Ese 0 es toda la regla: lo que no se entiende se dibuja.
        size_t applyTag(const std::vector<uint32_t>& cps, size_t at,
                        TextStyle& style, std::vector<StyleEntry>& stack)
        {
            // Un tag no puede ser infinito: sin '>' cerca, o con otro '<' por
            // medio, esto era texto.
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

                // Un cierre huérfano o cruzado NO desapila a ciegas: sale como
                // texto, que es lo único que no puede perder información.
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
                // Se parsea ANTES de apilar: un color inválido no deja la pila
                // tocada, así que el '</color>' de después tampoco casa.
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

        // Cadena -> array de glyphs con estilo, ya en píxeles de mundo.
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
                    // No era un tag: el '<' sigue siendo un carácter como otro.
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
                    // Sin glyph no hay ni avance ni par de kerning que valga.
                    previous = 0;
                    continue;
                }

                // fontSize/bakeSize: el atlas se horneó a UN tamaño y el resto
                // sale de escalar el quad, que es de lo que va un MSDF.
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

        // Corta en líneas. Con availWidth <= 0 (un Text sin rect) no hay contra
        // qué cortar: queda una sola línea por cada '\n', que es exactamente lo
        // de la fase anterior.
        void breakLines(const Text& text, float availWidth, TextScratch& s)
        {
            s.lines.clear();

            const bool   wrap = text.wordWrap && availWidth > 0.0f;
            const size_t n    = s.glyphs.size();

            size_t   lineFirst     = 0;
            float    lineWidth     = 0.0f;   // con los espacios del final incluidos
            uint32_t spaces        = 0;
            uint32_t trailing      = 0;      // espacios seguidos al final de la línea
            float    trailingWidth = 0.0f;

            // El kerning del PRIMER glyph de una línea no cuenta: su par se
            // quedó en la línea de arriba.
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

                // La palabra se mide ENTERA antes de decidir dónde va: es lo que
                // distingue un wrap por palabras de uno por caracteres.
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
                    // La palabra entera baja. Se reevalúa sin avanzar: ya en
                    // cabeza de línea puede seguir sin caber.
                    closeLine(i, i, false);
                    continue;
                }

                if (wrap && atLineStart && wordWidth > availWidth)
                {
                    // No cabe ni sola: se parte por glyph, con al menos uno por
                    // línea (si no, un rect más estrecho que un glyph no
                    // terminaría nunca).
                    for (size_t k = i; k < wordEnd; ++k)
                    {
                        if (k > lineFirst && lineWidth + glyphWidth(k) > availWidth)
                            closeLine(k, k, false);
                        lineWidth += glyphWidth(k);   // recalculado: lineFirst pudo cambiar
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

            // La última línea (o la única, aunque el texto esté vacío de glyphs
            // dibujables) se cierra igual.
            if (lineFirst < n || s.lines.empty()) closeLine(n, n, false);
        }

        // Recorta a las líneas que caben de alto y le pone '…' al final de la
        // última. Sin ese glyph en el atlas se cae a "...", y sin ninguno de los
        // dos se deja el texto tal cual antes que dibujar un hueco.
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

            // Se quitan glyphs del final hasta que quepan los puntos. Puede
            // quedarse en cero: más vale solo '…' que pasarse del rect.
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

        // Parseo + corte + puntos suspensivos, y de paso el tamaño del bloque:
        // ancho de la línea más larga y alto por lineHeight. Es lo mismo que
        // consume el pase de medida y lo que emite el de dibujo.
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

            // Clip reutiliza el scissor de siempre, así que lo único que cuesta
            // es partir el lote; Overflow no toca nada y no lo parte.
            if (text.overflow == UiTextOverflow::Clip)
            {
                scissor = intersectScissor(scissor, scissorFromRect(worldPos, worldSize));
                if (scissor.empty()) return;
            }

            // El atlas de la fuente es la clave del lote, igual que cualquier
            // otro: dos textos de la misma fuente caen en el mismo draw.
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

            // La sombra es un pase ENTERO por delante: mismo atlas y mismo
            // scissor, así que no parte el lote ni necesita otro pass.
            for (int pass = hasShadow ? 0 : 1; pass < 2; ++pass)
            {
                const bool isShadow = (pass == 0);

                // La línea base cae a un ascent del borde superior del rect.
                float baseline = worldPos.y + font->ascent() * unit * worldScale.y;
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

                    // Parte 0 = la línea; parte 1 = los puntos suspensivos, que
                    // viven al final del array.
                    for (int part = 0; part < 2; ++part)
                    {
                        const uint32_t first = part == 0 ? line.first : line.ellipsisFirst;
                        const uint32_t count = part == 0 ? line.count : line.ellipsisCount;

                        for (uint32_t k = 0; k < count; ++k)
                        {
                            const ShapedGlyph& g = s.glyphs[first + k];
                            if (part > 0 || k > 0) pen += g.kern;

                            // Un espacio no tiene contorno: avanza el cursor y ya.
                            if (g.glyph->rect.width > 0.0f && g.glyph->rect.height > 0.0f)
                            {
                                const glm::vec2 pos{pen + g.glyph->bearingX * g.scale.x,
                                                    baseline - g.glyph->bearingY * g.scale.y};
                                const glm::vec2 size{g.glyph->rect.width  * g.scale.x,
                                                     g.glyph->rect.height * g.scale.y};

                                // screenPxRange: el rango del campo de distancia
                                // llevado al tamaño al que se va a dibujar.
                                const float screenPxRange = font->pixelRange() * g.scale.y;

                                // <b> engorda por el MISMO canal que el outline:
                                // sin outline propio, el "borde" se pinta del
                                // color del relleno y el glyph sale más gordo.
                                const float bold = g.bold ? text.boldStrength * g.sizePx * worldScale.y : 0.0f;

                                glm::vec4 fill = g.color;
                                fill.a *= opacity;

                                const glm::vec4 params{1.0f, screenPxRange,
                                                       (isShadow ? 0.0f : text.outlineWidth) + bold, 0.0f};

                                glm::vec4 effect = isShadow ? glm::vec4(0.0f) : outline;
                                if (bold > 0.0f && (isShadow || text.outlineWidth <= 0.0f))
                                    effect = isShadow ? shadow : fill;

                                // <i> es una cizalla sobre la línea base: el
                                // borde de arriba se va a la derecha y el de
                                // abajo a la izquierda. Ni una UV cambia.
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

        // ── Pase de medida ──────────────────────────────────────────────────
        // Los content size fitters van de abajo arriba (el tamaño del padre sale
        // de los hijos) y la colocación de arriba abajo, así que hacen falta dos
        // pases. El árbol NO se muta: la medida vive en un vector local en
        // pre-orden y la colocación lo indexa.
        struct MeasuredNode
        {
            glm::vec2 size{0.0f, 0.0f};   // tamaño local ya resuelto (fitters aplicados)
            // Nodos que ocupa este subárbol, este incluido. Es lo que permite
            // saltar de un hijo al siguiente sin recorrerlo: emitNode sale antes
            // por !visible y por scissor vacío, y con un cursor que solo avanza
            // de uno en uno esas salidas desincronizarían todas las medidas.
            uint32_t subtree = 1;
        };

        bool participatesInLayout(const UiElement& node)
        {
            return node.visible && !node.ignoreLayout;
        }

        // El hueco que ocupa un hijo dentro del layout de su padre, en unidades
        // LOCALES del padre. Grid impone la celda y con ella se come el scale
        // del hijo (si no, un scale distinto rompería la rejilla); Horizontal y
        // Vertical respetan el tamaño del hijo ya escalado.
        glm::vec2 layoutSlotSize(const UiElement& parent, const UiElement& child, const glm::vec2& childSize)
        {
            if (parent.layoutMode == UiLayoutMode::Grid) return parent.cellSize;
            return childSize * child.scale;
        }

        // columns == 0 = las que quepan a lo ancho. Se mide contra node.size.x y
        // NO contra el tamaño ya ajustado: con fitWidth serían mutuamente
        // recursivos. Medida y colocación llaman a esto con los mismos datos.
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

            // El contenido se acumula DURANTE la recursión: así no hace falta ni
            // un vector de hijos por nodo, solo el de medidas.
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

            // El bloque de texto ES el contenido de un Text: el fitter crece
            // hasta él igual que un contenedor crece hasta sus hijos, y desde ahí
            // el layout del padre ya suma la medida como la de cualquier otro.
            // Se mide contra node.size (no contra el ya ajustado) por lo mismo
            // que gridColumns: con fitWidth serían mutuamente recursivos.
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

        // ── Pase de colocación ──────────────────────────────────────────────

        // Rect ya resuelto por el layout del padre. Sin él, el nodo se coloca
        // por sus anclas como siempre.
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

        // Un subárbol que el emisor NO recorre (invisible, o recortado a cero) se
        // queda sin rect resuelto. Marcarlo es lo que impide que el hit test del
        // input siga usando el rect del frame anterior, que ya no significa nada.
        void invalidateRects(const UiElement& node)
        {
            node.rectValid = false;
            for (const auto& child : node.children()) invalidateRects(*child);
        }

        // Resuelve la colocación del nodo y la deja EN SU CACHÉ. Es exactamente
        // el cálculo que hacía emitNode en línea; sale aparte solo para poder
        // saltárselo entero cuando ni Transform ni Layout están sucios.
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
                // Lo colocó el layout del padre: sus anclas, sus márgenes y su
                // position no se leen.
                worldPos  = placement.worldPos;
                worldSize = placement.worldSize;
            }
            else
            {
                // Eje a eje. Con anchorMin == anchorMax sale exactamente la
                // fórmula de siempre: ancla sobre el rect DEL PADRE, pivot sobre
                // el PROPIO, y con todo a {0,0}, parentPos + position*parentScale.
                // Con anchorMin != anchorMax el eje se ESTIRA y mandan los
                // márgenes: size y pivot de ese eje no se leen.
                // node.rotation se sigue ignorando a propósito.
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

            // Un contenedor sin tamaño (la raíz, o un grupo que solo agrupa) no
            // define área de anclaje: sus hijos siguen anclando contra la del
            // padre en vez de colapsar todos contra su esquina.
            const glm::vec2 childArea = (worldSize.x > 0.0f && worldSize.y > 0.0f) ? worldSize : parentSize;

            // La máscara: el rect del elemento metido hacia dentro por sus
            // insets, INTERSECADO con lo que venía del padre (nunca un
            // reemplazo, así que una máscara anidada solo puede recortar más).
            // Con maskSelf el propio elemento entra en ella; sin él solo sus
            // descendientes.
            // ÚNICO punto donde entra la escala del canvas: de aquí para abajo
            // se trabaja en píxeles del render, y de aquí para arriba (medida,
            // layout, anclas, márgenes, padding) en unidades de referencia. Con
            // escala 1 y origen {0,0} el float que sale es el MISMO bit a bit.
            const glm::vec2 screenPos  = g_xf.origen + worldPos * g_xf.escala;
            const glm::vec2 screenSize = worldSize * g_xf.escala;

            UiScissor selfScissor  = scissor;
            UiScissor childScissor = scissor;
            bool      culled       = false;

            if (node.clipChildren && node.maskEnabled)
            {
                const glm::vec2 maskPos {screenPos.x + node.maskInsetLeft * g_xf.escala,
                                         screenPos.y + node.maskInsetTop  * g_xf.escala};
                // Insets que se cruzan dejan tamaño <= 0 y scissorFromRect
                // devuelve vacío: JAMÁS un width/height negativo, que en un
                // VkRect2D es un crash.
                const glm::vec2 maskSize{screenSize.x - node.maskInsetLeft * g_xf.escala - node.maskInsetRight  * g_xf.escala,
                                         screenSize.y - node.maskInsetTop  * g_xf.escala - node.maskInsetBottom * g_xf.escala};

                childScissor = intersectScissor(scissor, scissorFromRect(maskPos, maskSize));

                if (node.maskSelf)
                {
                    selfScissor = childScissor;
                    // Intersección vacía: ni este nodo ni ninguno de sus hijos
                    // puede verse, así que no se emite ni un draw con
                    // width/height 0.
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
            // enabled NO se mira aquí: es para el input, no para el dibujado.
            if (!node.visible) { invalidateRects(node); return; }

            // Ni Transform ni Layout sucios quiere decir que tampoco lo están en
            // ningún ancestro (los dos SUBEN y BAJAN), o sea que las entradas de
            // la colocación son las MISMAS y volvería a salir bit a bit igual.
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

            // El rect ya está resuelto: se GUARDA para que el input lo reutilice
            // sin recorrer el árbol otra vez. No altera ni un vértice ni un lote.
            // Es el MISMO scissor con el que se dibuja el nodo, que es lo que
            // hace que el hit test respete la máscara sin código propio.
            node.screenPos     = screenPos;
            node.screenSize    = screenSize;
            node.screenScissor = selfScissor;
            node.rectValid     = true;

            // Un Text con fuente dibuja sus glyphs EN VEZ de su propio quad: si
            // no, cada texto arrastraría un rectángulo blanco detrás. Sin fuente
            // (o sin nada que decir) vuelve a comportarse como su base, que es lo
            // que hace que un Text a medio configurar no desaparezca en silencio.
            // Aquí está TODO el ahorro. Un nodo sin ni un bit sucio emitió sus
            // vértices desde las mismas entradas que ahora, así que se vuelcan
            // tal cual en vez de medir glyphs o trocear un sliced otra vez. El
            // recorrido del árbol no cambia: se salta el trabajo, no el nodo.
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

                // La rotación vale para lo que emite ESTE nodo (su quad, sus N
                // quads de Image o sus glyphs) y para nada más: los hijos vuelven
                // al estado de antes, y el scissor de arriba es el AABB sin rotar.
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

            // Máscara vacía con maskSelf a false: el elemento ya se ha dibujado
            // entero, pero por su máscara no pasa ni un vértice de sus hijos.
            if (childScissor.empty())
            {
                for (const auto& child : node.children()) invalidateRects(*child);
                return;
            }

            scissor = childScissor;

            // El índice del primer hijo es el siguiente en pre-orden, y cada
            // hermano está a un subárbol entero del anterior.
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

            // El layout trabaja ya en píxeles de mundo: así un contenedor
            // estirado o colocado por otro layout reparte sobre su rect real.
            const glm::vec2 padMin{node.paddingLeft * worldScale.x, node.paddingTop * worldScale.y};
            const glm::vec2 padMax{node.paddingRight * worldScale.x, node.paddingBottom * worldScale.y};
            const glm::vec2 gap = node.spacing * worldScale;
            const glm::vec2 origin = worldPos + padMin;
            const glm::vec2 inner  = worldSize - padMin - padMax;

            uint32_t laidCount = 0;
            for (const auto& child : node.children())
                if (participatesInLayout(*child)) ++laidCount;

            const uint32_t cols = node.layoutMode == UiLayoutMode::Grid ? gridColumns(node, laidCount) : 1;

            float    cursor    = 0.0f;   // avance en el eje principal, en píxeles de mundo
            uint32_t laidIndex = 0;

            for (const auto& child : node.children())
            {
                const uint32_t ci = childIndex;
                childIndex += measured[ci].subtree;

                if (!participatesInLayout(*child))
                {
                    // ignoreLayout se dibuja anclado como si el padre no tuviera
                    // layout; el invisible ni se visita (pero su hueco en el
                    // vector de medidas ya se ha saltado arriba).
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
                    // Grid: la celda es uniforme, así que la posición sale de la
                    // fila y la columna, no de un cursor acumulado.
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

        std::vector<char> readSpv(const std::string& path)
        {
            std::ifstream file(path, std::ios::ate | std::ios::binary);
            if (!file.is_open()) throw std::runtime_error("failed to open shader file: " + path);
            const size_t size = (size_t)file.tellg();
            std::vector<char> buffer(size);
            file.seekg(0);
            file.read(buffer.data(), (std::streamsize)size);
            return buffer;
        }

        VkShaderModule makeModule(VkDevice device, const std::vector<char>& code)
        {
            VkShaderModuleCreateInfo info{};
            info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            info.codeSize = code.size();
            info.pCode    = reinterpret_cast<const uint32_t*>(code.data());
            VkShaderModule module = VK_NULL_HANDLE;
            if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS)
                throw std::runtime_error("failed to create ui shader module!");
            return module;
        }
    }

    // ── CPU ─────────────────────────────────────────────────────────────────

    void UiSpriteBatch::build(const UiCanvas& canvas, uint32_t width, uint32_t height, UiDrawData& out)
    {
        // ── Área útil, en este orden y no en otro ───────────────────────────
        // (a) el render entero.
        float x0 = 0.0f;
        float y0 = 0.0f;
        float x1 = (float)width;
        float y1 = (float)height;

        // (b) los insets del safe area, en píxeles reales. Negativos se ignoran
        // (agrandar el área útil por encima del render no significa nada) y unos
        // insets que se cruzan dejan área 0, nunca un rect del revés.
        if (canvas.safeArea.left   > 0.0f) x0 += canvas.safeArea.left;
        if (canvas.safeArea.top    > 0.0f) y0 += canvas.safeArea.top;
        if (canvas.safeArea.right  > 0.0f) x1 -= canvas.safeArea.right;
        if (canvas.safeArea.bottom > 0.0f) y1 -= canvas.safeArea.bottom;
        if (x1 < x0) x1 = x0;
        if (y1 < y0) y1 = y0;

        float uw = x1 - x0;
        float uh = y1 - y0;

        // (c) el aspect ratio, recortando CENTRADO. Lo que sobra son barras
        // (letterbox si el área es más ancha de la cuenta, pillarbox si es más
        // alta) y la UI no las ocupa.
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

        // (d) una escala ÚNICA y UNIFORME, del área útil.
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
                if (!(m >= 0.0f)) m = 0.0f;    // pilla también el NaN
                if (m > 1.0f)     m = 1.0f;

                switch (canvas.screenMatch)
                {
                case UiScreenMatch::Expand: escala = (rx < ry) ? rx : ry; break;
                case UiScreenMatch::Shrink: escala = (rx > ry) ? rx : ry; break;
                case UiScreenMatch::MatchWidthOrHeight:
                default:
                    // Lerp LOGARÍTMICO: con m = 0 sigue al ancho, con m = 1 al
                    // alto, y en medio cae entre los dos sin que un lado se
                    // coma al otro (que es lo que pasa con la media aritmética).
                    escala = std::pow(rx, 1.0f - m) * std::pow(ry, m);
                    break;
                }
                escala *= canvas.scaleFactor;
            }
            break;
        }
        case UiScaleMode::ConstantPhysicalSize:
        {
            // screenDpi <= 0 es "no se sabe": el fallback evita que un SO que no
            // lo reporte deje la UI a escala 0.
            const float dpi = (canvas.screenDpi > 0.0f) ? canvas.screenDpi : canvas.fallbackDpi;
            if (canvas.referenceDpi > 0.0f) escala = (dpi / canvas.referenceDpi) * canvas.scaleFactor;
            break;
        }
        case UiScaleMode::ConstantPixelSize:
        default:
            escala = canvas.scaleFactor;
            break;
        }

        // Una escala <= 0 o no finita no encoge la UI: la hace desaparecer o la
        // llena de NaN. Cae a 1 y sigue.
        if (!std::isfinite(escala) || escala <= 0.0f) escala = 1.0f;

        const glm::vec2 nuevoOrigen{x0, y0};
        const glm::vec2 nuevaRef{uw / escala, uh / escala};

        // Si cambia el tamaño del render, la escala, el origen o el área útil,
        // se mueve la colocación de TODOS los nodos: no hay ni una caché que
        // siga valiendo, así que se ensucia el árbol entero antes de recorrerlo.
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

        // Canvas vacío: ni se mide, ni se reserva el vector, ni se recorre nada.
        // La resolución ya está resuelta, así que los getters valen igual.
        if (canvas.root().children().empty()) return;

        // Área útil de 0 píxeles (safe area que se come el render entero): no
        // hay dónde dibujar y los rects del frame anterior no pueden quedarse.
        if (uw <= 0.0f || uh <= 0.0f) { invalidateRects(canvas.root()); return; }

        // El scissor raíz ES el área útil: lo que cae en las barras del aspect
        // ratio o fuera del safe area no se dibuja. El hit test lee este mismo
        // scissor, así que los eventos salen coherentes sin código propio.
        const UiScissor full = scissorFromRect({x0, y0}, {uw, uh});

        // El "padre" de la raíz es el área útil en unidades de REFERENCIA: los
        // elementos de primer nivel anclan contra ese rect y no ven la escala.
        const glm::vec2 screen = canvas.m_referenceSize;

        g_xf.origen = {x0, y0};
        g_xf.escala = escala;

        // Medida bottom-up primero (resuelve los fitters), colocación después.
        std::vector<MeasuredNode> measured;
        measureNode(canvas.root(), measured);

        emitNode(canvas.root(), 0, measured, glm::vec2(0.0f), glm::vec2(1.0f), screen,
                 LayoutPlacement{}, full, 1.0f, out);

        canvas.m_rebuiltNodes = g_rebuilt;

        // Que no se quede encendida para el siguiente canvas ni para el hit test.
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

        // Pool propio: un set por atlas (más el blanco). 32 cubre de sobra la
        // UI de un juego y los sets viven todo el proceso.
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
        // UNORM y no el SRGB por defecto de createTextureImageView: la imagen la
        // crea createSolidColorImage como R8G8B8A8_UNORM, y la vista tiene que
        // declarar EXACTAMENTE ese formato (la imagen no es MUTABLE_FORMAT).
        // Da igual visualmente — 255 es 1.0 en los dos — pero es un error de
        // validacion y comportamiento indefinido.
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

        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pcr.offset     = 0;
        pcr.size       = sizeof(glm::mat4);

        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &m_descLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &pcr;
        if (vkCreatePipelineLayout(gpu.device(), &pli, nullptr, &m_layout) != VK_SUCCESS)
            throw std::runtime_error("failed to create ui pipeline layout!");

        createPipeline(gpu, renderPass, samples);
    }

    void UiSpriteBatch::recreatePipeline(GpuDevice& gpu, VkRenderPass renderPass,
                                         VkSampleCountFlagBits samples)
    {
        if (m_layout == VK_NULL_HANDLE) return;   // sin init (headless sin UI)
        if (m_pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(gpu.device(), m_pipeline, nullptr);
            m_pipeline = VK_NULL_HANDLE;
        }
        createPipeline(gpu, renderPass, samples);
    }

    void UiSpriteBatch::createPipeline(GpuDevice& gpu, VkRenderPass renderPass,
                                       VkSampleCountFlagBits samples)
    {
        VkShaderModule vert = makeModule(gpu.device(), readSpv("shaders/ui.vert.spv"));
        VkShaderModule frag = makeModule(gpu.device(), readSpv("shaders/ui.frag.spv"));

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

        // Las cinco localizaciones tienen que decir LO MISMO que ui.vert: un
        // desajuste de offset o de formato no da ni error ni aviso, solo
        // basura en pantalla.
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
        // NONE: los quads salen en el orden en que los emite el batcher y su
        // orientación no depende del frontFace del resto del motor.
        rs.cullMode    = VK_CULL_MODE_NONE;
        rs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        // Las mismas muestras que sus compañeros del pass de composición: con
        // MSAA ese pass es multisample y un pipeline que declare 1 sample no
        // sería compatible.
        ms.rasterizationSamples = samples;

        // El pass de composición trae la profundidad de la escena cargada. La UI
        // ni la lee ni la escribe: va siempre encima.
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable  = VK_FALSE;
        ds.depthWriteEnable = VK_FALSE;
        ds.depthCompareOp   = VK_COMPARE_OP_ALWAYS;

        // Alpha recto (SRC_ALPHA / ONE_MINUS_SRC_ALPHA): el color del sprite NO
        // viene premultiplicado.
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

        if (vkCreateGraphicsPipelines(gpu.device(), VK_NULL_HANDLE, 1, &pci, nullptr, &m_pipeline) != VK_SUCCESS)
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

            // Mapeo persistente: se reescribe entero cada frame.
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

    void UiSpriteBatch::record(GpuDevice& gpu, VkCommandBuffer cmd, const UiDrawData& data,
                               VkExtent2D extent, int frame)
    {
        // Canvas vacío = ni un comando, ni un buffer creado, ni un mapeo. Es la
        // condición que hace que la escena 3D salga EXACTAMENTE igual que antes.
        if (data.empty() || m_pipeline == VK_NULL_HANDLE) return;
        if (extent.width == 0 || extent.height == 0) return;

        ensureBuffers(gpu, frame, (uint32_t)data.vertices.size(), (uint32_t)data.indices.size());

        std::memcpy(m_vertexMapped[frame], data.vertices.data(), data.vertices.size() * sizeof(UiVertex));
        std::memcpy(m_indexMapped[frame],  data.indices.data(),  data.indices.size()  * sizeof(uint16_t));

        // bottom=0 y top=alto: (0,0) cae ARRIBA a la izquierda. Parece del revés
        // y es justo lo contrario: en Vulkan el +Y de NDC va hacia ABAJO, así
        // que la receta de OpenGL (top=0, bottom=alto) deja [1][1] negativo y
        // dibuja la UI ENTERA espejada — invisible mientras solo hubo quads de
        // color, evidente en cuanto se dibujó la primera letra. RH_ZO porque
        // Vulkan clipea z fuera de [0,1] y glm::ortho a secas da [-1,1].
        const glm::mat4 proj = glm::orthoRH_ZO(0.0f, (float)extent.width,
                                               0.0f, (float)extent.height,
                                               0.0f, 1.0f);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
        vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &proj);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertexBuffers[frame], &offset);
        vkCmdBindIndexBuffer(cmd, m_indexBuffers[frame], 0, VK_INDEX_TYPE_UINT16);

        for (const UiBatch& batch : data.batches)
        {
            if (batch.indexCount == 0 || batch.scissor.empty()) continue;

            VkRect2D rect{};
            rect.offset.x      = batch.scissor.x;
            rect.offset.y      = batch.scissor.y;
            rect.extent.width  = batch.scissor.width;
            rect.extent.height = batch.scissor.height;
            vkCmdSetScissor(cmd, 0, 1, &rect);

            VkDescriptorSet set = (batch.atlas && batch.atlas->descriptorSet() != VK_NULL_HANDLE)
                                ? batch.atlas->descriptorSet() : m_whiteSet;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &set, 0, nullptr);
            vkCmdDrawIndexed(cmd, batch.indexCount, 1, batch.firstIndex, 0, 0);
        }

        // El scissor es estado dinámico del command buffer: dejarlo recortado
        // afectaría a lo que se grabe después en este mismo buffer.
        VkRect2D full{};
        full.offset = {0, 0};
        full.extent = extent;
        vkCmdSetScissor(cmd, 0, 1, &full);
    }

    void UiSpriteBatch::shutdown(GpuDevice& gpu)
    {
        for (int i = 0; i < kFrames; ++i) destroyBuffers(gpu, i);

        if (m_pipeline != VK_NULL_HANDLE)   vkDestroyPipeline(gpu.device(), m_pipeline, nullptr);
        if (m_layout != VK_NULL_HANDLE)     vkDestroyPipelineLayout(gpu.device(), m_layout, nullptr);
        if (m_whiteView != VK_NULL_HANDLE)  vkDestroyImageView(gpu.device(), m_whiteView, nullptr);
        if (m_whiteImage != VK_NULL_HANDLE) vkDestroyImage(gpu.device(), m_whiteImage, nullptr);
        if (m_whiteMemory != VK_NULL_HANDLE)vkFreeMemory(gpu.device(), m_whiteMemory, nullptr);
        if (m_sampler != VK_NULL_HANDLE)    vkDestroySampler(gpu.device(), m_sampler, nullptr);
        // El pool se lleva por delante todos los sets (el blanco y los de los
        // atlas), así que no hay que liberarlos uno a uno.
        if (m_descPool != VK_NULL_HANDLE)   vkDestroyDescriptorPool(gpu.device(), m_descPool, nullptr);
        if (m_descLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(gpu.device(), m_descLayout, nullptr);

        m_pipeline    = VK_NULL_HANDLE;
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
