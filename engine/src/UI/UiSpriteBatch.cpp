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
        void emitRawQuad(const UiTextureAtlas* atlas, const glm::vec2& pos, const glm::vec2& size,
                         const UiUvRect& uv, const glm::vec4& color,
                         const glm::vec4& params, const glm::vec4& effect,
                         const UiScissor& scissor, UiDrawData& out)
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

            // Sentido horario en pantalla empezando arriba a la izquierda. El
            // vértice inferior tiene la Y MAYOR: +Y va hacia abajo.
            out.vertices.push_back({{pos.x,          pos.y         }, {uv.u0, uv.v0}, color, params, effect});
            out.vertices.push_back({{pos.x + size.x, pos.y         }, {uv.u1, uv.v0}, color, params, effect});
            out.vertices.push_back({{pos.x + size.x, pos.y + size.y}, {uv.u1, uv.v1}, color, params, effect});
            out.vertices.push_back({{pos.x,          pos.y + size.y}, {uv.u0, uv.v1}, color, params, effect});

            const uint16_t quad[6] = { (uint16_t)(base + 0), (uint16_t)(base + 1), (uint16_t)(base + 2),
                                       (uint16_t)(base + 2), (uint16_t)(base + 3), (uint16_t)(base + 0) };
            out.indices.insert(out.indices.end(), quad, quad + 6);
            out.batches.back().indexCount += 6;
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

            // params.x = 0: el shader hace exactamente lo de siempre.
            emitRawQuad(node.atlas, pos, size, uv, color,
                        glm::vec4(0.0f), glm::vec4(0.0f), scissor, out);
        }

        // ── Texto ───────────────────────────────────────────────────────────
        // Una línea, sin wrap y sin alineación. El rect del nodo solo aporta la
        // esquina superior izquierda: la altura la manda la métrica de la fuente.
        void emitText(const Text& text, const glm::vec2& worldPos, const glm::vec2& worldScale,
                      const UiScissor& scissor, float opacity, UiDrawData& out)
        {
            const UiFont* font = text.font;

            // fontSize/bakeSize: el atlas se horneó a UN tamaño y el resto sale
            // de escalar el quad, que es de lo que va un MSDF.
            const float unit = font->scaleFor(text.fontSize);
            if (unit <= 0.0f) return;

            const std::vector<uint32_t> codepoints = UiFont::decodeUtf8(text.text);
            if (codepoints.empty()) return;

            const glm::vec2 scale = glm::vec2(unit) * worldScale;

            // El atlas de la fuente es la clave del lote, igual que cualquier
            // otro: dos textos de la misma fuente caen en el mismo draw.
            const UiTextureAtlas* atlas = &font->atlas();

            glm::vec4 fill = text.color;
            fill.a *= opacity;
            glm::vec4 outline = text.outlineColor;
            outline.a *= opacity;
            glm::vec4 shadow = text.shadowColor;
            shadow.a *= opacity;

            const glm::vec2 shadowOffset = text.shadowOffset * worldScale;
            const bool hasShadow = shadow.a > 0.0f &&
                                   (text.shadowOffset.x != 0.0f || text.shadowOffset.y != 0.0f);

            // screenPxRange: el rango del campo de distancia llevado al tamaño
            // al que se va a dibujar. Sin esto el borde se difumina o se aliasa
            // según el tamaño.
            const float screenPxRange = font->pixelRange() * scale.y;

            // La sombra es un pase ENTERO por delante: mismo atlas y mismo
            // scissor, así que no parte el lote ni necesita otro pass.
            for (int pass = hasShadow ? 0 : 1; pass < 2; ++pass)
            {
                const bool isShadow = (pass == 0);

                // La línea base cae a un ascent del borde superior del rect.
                glm::vec2 pen{worldPos.x, worldPos.y + font->ascent() * scale.y};
                if (isShadow) pen += shadowOffset;

                uint32_t previous = 0;

                for (uint32_t codepoint : codepoints)
                {
                    const UiGlyph* glyph = font->findGlyph(codepoint);
                    if (!glyph)
                    {
                        // Sin glyph no hay ni avance ni par de kerning que valga.
                        previous = 0;
                        continue;
                    }

                    if (previous != 0) pen.x += font->kerning(previous, codepoint) * scale.x;
                    previous = codepoint;

                    // Un espacio no tiene contorno: avanza el cursor y ya.
                    if (glyph->rect.width > 0.0f && glyph->rect.height > 0.0f)
                    {
                        const glm::vec2 pos{pen.x + glyph->bearingX * scale.x,
                                            pen.y - glyph->bearingY * scale.y};
                        const glm::vec2 size{glyph->rect.width  * scale.x,
                                             glyph->rect.height * scale.y};

                        const glm::vec4 params{1.0f, screenPxRange,
                                               isShadow ? 0.0f : text.outlineWidth, 0.0f};

                        emitRawQuad(atlas, pos, size, font->glyphUv(*glyph),
                                    isShadow ? shadow : fill, params,
                                    isShadow ? glm::vec4(0.0f) : outline, scissor, out);
                    }

                    pen.x += glyph->advance * scale.x;
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

            if (node.layoutMode != UiLayoutMode::None && (node.fitWidth || node.fitHeight))
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

        void emitNode(const UiElement& node, uint32_t index, const std::vector<MeasuredNode>& measured,
                      const glm::vec2& parentPos, const glm::vec2& parentScale,
                      const glm::vec2& parentSize, const LayoutPlacement& placement,
                      UiScissor scissor, float parentOpacity, UiDrawData& out)
        {
            // enabled NO se mira aquí: es para el input, no para el dibujado.
            if (!node.visible) return;

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

            if (node.clipChildren)
            {
                scissor = intersectScissor(scissor, scissorFromRect(worldPos, worldSize));
                // Intersección vacía: ni este nodo ni ninguno de sus hijos puede
                // verse, así que no se emite ni un draw con width/height 0.
                if (scissor.empty()) return;
            }

            // Un Text con fuente dibuja sus glyphs EN VEZ de su propio quad: si
            // no, cada texto arrastraría un rectángulo blanco detrás. Sin fuente
            // (o sin nada que decir) vuelve a comportarse como su base, que es lo
            // que hace que un Text a medio configurar no desaparezca en silencio.
            const Text* text = node.asText();
            const bool  drawsText = text && text->font && text->font->hasGlyphs() && !text->text.empty();

            if (drawsText)
                emitText(*text, worldPos, worldScale, scissor, opacity, out);
            else if (node.drawable && worldSize.x > 0.0f && worldSize.y > 0.0f)
                emitQuad(node, worldPos, worldSize, scissor, opacity, out);

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
        // Canvas vacío: ni se mide, ni se reserva el vector, ni se recorre nada.
        if (canvas.root().children().empty()) return;

        UiScissor full{};
        full.x = 0;
        full.y = 0;
        full.width  = width;
        full.height = height;

        // El "padre" de la raíz es el render entero: es contra ese rect contra
        // el que anclan los elementos de primer nivel.
        const glm::vec2 screen{(float)width, (float)height};

        // Medida bottom-up primero (resuelve los fitters), colocación después.
        std::vector<MeasuredNode> measured;
        measureNode(canvas.root(), measured);

        emitNode(canvas.root(), 0, measured, glm::vec2(0.0f), glm::vec2(1.0f), screen,
                 LayoutPlacement{}, full, 1.0f, out);
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

        // top=0 y bottom=alto: (0,0) cae ARRIBA a la izquierda. RH_ZO porque
        // Vulkan clipea z fuera de [0,1] y glm::ortho a secas da [-1,1].
        const glm::mat4 proj = glm::orthoRH_ZO(0.0f, (float)extent.width,
                                               (float)extent.height, 0.0f,
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
