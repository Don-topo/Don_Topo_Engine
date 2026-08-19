#include "DonTopo/Editor/SpriteEditorPanel.h"

#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Editor/ProjectContext.h"
#include "DonTopo/Renderer/EditorRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace DonTopo
{
    namespace
    {
        // Radio en píxeles DE PANTALLA para agarrar una esquina. Fijo y no
        // proporcional al zoom: es una distancia de ratón, no de imagen.
        constexpr float kHandle = 6.0f;

        ImU32 colorRect(bool selected)
        {
            return selected ? IM_COL32(255, 190, 60, 255) : IM_COL32(90, 200, 255, 190);
        }

        void normalize(UiSpriteRect& r)
        {
            if (r.width < 0.0f)  { r.x += r.width;  r.width  = -r.width; }
            if (r.height < 0.0f) { r.y += r.height; r.height = -r.height; }
        }

        // Dentro de la imagen y con área: un rect que se sale daría UVs fuera
        // de [0,1] y el sampler repetiría o estiraría el borde sin avisar.
        void clampToImage(UiSpriteRect& r, uint32_t w, uint32_t h)
        {
            const float fw = (float)w;
            const float fh = (float)h;
            r.x = std::max(0.0f, std::min(r.x, fw));
            r.y = std::max(0.0f, std::min(r.y, fh));
            r.width  = std::max(1.0f, std::min(r.width,  fw - r.x));
            r.height = std::max(1.0f, std::min(r.height, fh - r.y));
        }

        bool contains(const UiSpriteRect& r, const ImVec2& p)
        {
            return p.x >= r.x && p.x <= r.x + r.width &&
                   p.y >= r.y && p.y <= r.y + r.height;
        }
    }

    void SpriteEditorPanel::open(EditorContext& ctx, const std::string& imagePath)
    {
        m_open = true;
        // La misma imagen que ya está abierta no se recarga: perderían los
        // cambios sin guardar por pulsar dos veces el mismo botón.
        if (imagePath == m_path && !m_path.empty()) return;
        loadFrom(ctx, imagePath);
    }

    void SpriteEditorPanel::loadFrom(EditorContext& ctx, const std::string& imagePath)
    {
        m_path.clear();
        m_error.clear();
        m_entries.clear();
        m_selected  = -1;
        m_dirty     = false;
        m_textureId = 0;
        m_imageW = m_imageH = 0;
        m_drag = Drag::None;

        if (imagePath.empty()) return;
        if (!ctx.renderer) { m_error = "Sin renderer"; return; }

        // El atlas VIVO del renderer (cacheado por ruta): así lo que se guarda
        // aquí se ve en el viewport sin recargar la escena.
        UiTextureAtlas* atlas = ctx.renderer->loadUiAtlas(imagePath);
        if (!atlas)
        {
            m_error = "No se pudo abrir la imagen: " + imagePath;
            return;
        }

        m_path      = imagePath;
        m_imageW    = atlas->width();
        m_imageH    = atlas->height();
        m_textureId = ctx.renderer->uiAtlasTextureId(atlas);

        // Los sprites salen del atlas vivo, que ya cargó el sidecar al abrirse.
        for (const std::string& name : atlas->spriteNames())
            if (const UiSpriteRect* r = atlas->findSprite(name))
                m_entries.push_back(Entry{name, *r});

        if (m_textureId == 0)
            m_error = "La imagen está cargada pero el backend no da handle para "
                      "enseñarla; los rects se pueden editar a mano.";
    }

    void SpriteEditorPanel::save(EditorContext& ctx)
    {
        if (m_path.empty() || !ctx.renderer) return;

        UiTextureAtlas* vivo = ctx.renderer->loadUiAtlas(m_path);
        if (!vivo)
        {
            m_error = "El atlas ya no está cargado: no se guarda nada";
            return;
        }

        // El atlas vivo ES el que se serializa: así lo que queda en disco y lo
        // que se dibuja en el viewport no pueden discrepar.
        vivo->clearSprites();
        for (const Entry& e : m_entries)
            if (!e.name.empty() && e.rect.width > 0.0f && e.rect.height > 0.0f)
                vivo->addSprite(e.name, e.rect);

        const std::string sidecar = UiTextureAtlas::spriteSheetPathFor(m_path);

        // Mismo criterio que el resto del editor: se puede escribir junto a un
        // asset compartido del repo, pero NUNCA dentro del proyecto de otro. El
        // sidecar acompaña a la imagen, así que hereda su sitio y su permiso.
        if (ctx.project && ctx.project->valid() && !ctx.project->contains(sidecar))
        {
            const ProjectContext workspace(ProjectContext::workspaceDir());
            if (workspace.contains(sidecar))
            {
                m_error = "El atlas es de otro proyecto: no se escribe nada";
                ctx.logModule("Project", "Sidecar de sprites rechazado: " + sidecar);
                return;
            }
        }

        if (!vivo->saveSprites(sidecar))
        {
            m_error = "No se pudo escribir " + sidecar;
            if (ctx.pushLog) ctx.pushLog(m_error);
            return;
        }

        m_dirty = false;
        m_error.clear();
        if (ctx.pushLog)
            ctx.pushLog("Sprites guardados en " + sidecar + " (" +
                        std::to_string(m_entries.size()) + ")");
        // Properties cachea los nombres por ruta: sin este aviso, los combos
        // seguirían enseñando la lista de antes hasta cambiar de atlas.
        if (ctx.onSpritesChanged) ctx.onSpritesChanged();
    }

    std::string SpriteEditorPanel::freeName(const std::string& prefix) const
    {
        for (int i = 0; ; ++i)
        {
            const std::string candidato = prefix + std::to_string(i);
            if (indexOfName(candidato) < 0) return candidato;
        }
    }

    int SpriteEditorPanel::indexOfName(const std::string& name) const
    {
        for (size_t i = 0; i < m_entries.size(); ++i)
            if (m_entries[i].name == name) return (int)i;
        return -1;
    }

    void SpriteEditorPanel::sliceGrid()
    {
        if (m_imageW == 0 || m_imageH == 0) return;
        if (m_gridCols <= 0 || m_gridRows <= 0) return;

        // El sobrante tras los offsets y los huecos, repartido entre las celdas.
        const float dispX = (float)m_imageW - (float)m_gridOffsetX -
                            (float)m_gridSpacingX * (float)(m_gridCols - 1);
        const float dispY = (float)m_imageH - (float)m_gridOffsetY -
                            (float)m_gridSpacingY * (float)(m_gridRows - 1);
        if (dispX <= 0.0f || dispY <= 0.0f)
        {
            m_error = "La rejilla no cabe en la imagen con esos márgenes";
            return;
        }

        const float cw = std::floor(dispX / (float)m_gridCols);
        const float ch = std::floor(dispY / (float)m_gridRows);
        if (cw < 1.0f || ch < 1.0f)
        {
            m_error = "Celdas de menos de un píxel";
            return;
        }

        m_entries.clear();
        m_selected = -1;

        // Fila a fila y de izquierda a derecha: es el orden en el que se leen
        // las hojas de sprites, así que los números salen donde uno los espera.
        int n = 0;
        for (int fila = 0; fila < m_gridRows; ++fila)
        {
            for (int col = 0; col < m_gridCols; ++col, ++n)
            {
                UiSpriteRect r{};
                r.x      = (float)m_gridOffsetX + (float)col * (cw + (float)m_gridSpacingX);
                r.y      = (float)m_gridOffsetY + (float)fila * (ch + (float)m_gridSpacingY);
                r.width  = cw;
                r.height = ch;
                m_entries.push_back(Entry{std::string(m_gridPrefix) + std::to_string(n), r});
            }
        }
        m_dirty = true;
        m_error.clear();
    }

    void SpriteEditorPanel::drawToolbar(EditorContext& ctx)
    {
        if (ImGui::Button("Guardar")) save(ctx);
        ImGui::SameLine();
        if (ImGui::Button("Recargar"))
        {
            // El "deshacer" de este panel: vuelve a lo que dice el fichero.
            const std::string path = m_path;
            m_path.clear();
            loadFrom(ctx, path);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s", m_path.empty() ? "(sin imagen)" : m_path.c_str(),
                            m_dirty ? "  *sin guardar" : "");

        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::SliderFloat("Zoom", &m_zoom, 0.25f, 8.0f, "%.2fx");
        ImGui::SameLine();
        if (ImGui::Button("1:1")) m_zoom = 1.0f;

        if (!m_error.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 120, 120, 255));
            ImGui::TextWrapped("%s", m_error.c_str());
            ImGui::PopStyleColor();
        }
    }

    void SpriteEditorPanel::drawSidebar(EditorContext& ctx)
    {
        (void)ctx;
        // 280 y no 240: con cuatro números en una línea (el rect del sprite) y
        // los pares de la rejilla, por debajo de esto los campos se quedan sin
        // sitio para el propio número.
        ImGui::BeginChild("sprite_list", ImVec2(280.0f, 0.0f), true);

        ImGui::TextDisabled("Sprites (%d)", (int)m_entries.size());
        if (ImGui::Button("Nuevo"))
        {
            UiSpriteRect r{};
            r.x = 0.0f; r.y = 0.0f;
            r.width  = (float)std::min<uint32_t>(m_imageW, 32u);
            r.height = (float)std::min<uint32_t>(m_imageH, 32u);
            m_entries.push_back(Entry{freeName("sprite_"), r});
            m_selected = (int)m_entries.size() - 1;
            m_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Borrar") && m_selected >= 0 && m_selected < (int)m_entries.size())
        {
            m_entries.erase(m_entries.begin() + m_selected);
            m_selected = -1;
            m_dirty = true;
        }

        ImGui::Separator();
        for (int i = 0; i < (int)m_entries.size(); ++i)
        {
            ImGui::PushID(i);
            if (ImGui::Selectable(m_entries[i].name.c_str(), m_selected == i))
            {
                m_selected = i;
                std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", m_entries[i].name.c_str());
            }
            ImGui::PopID();
        }

        ImGui::Separator();
        if (m_selected >= 0 && m_selected < (int)m_entries.size())
        {
            Entry& e = m_entries[m_selected];
            ImGui::TextDisabled("Seleccionado");

            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##nombre", m_nameBuf, sizeof(m_nameBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
            {
                const std::string nuevo = m_nameBuf;
                const int choque = indexOfName(nuevo);
                // Dos sprites con el mismo nombre serían uno solo al guardar (el
                // mapa se queda con el último) y el otro desaparecería sin más.
                if (nuevo.empty())
                    m_error = "Un sprite sin nombre no se puede referenciar";
                else if (choque >= 0 && choque != m_selected)
                    m_error = "Ya hay un sprite llamado '" + nuevo + "'";
                else
                {
                    e.name  = nuevo;
                    m_dirty = true;
                    m_error.clear();
                }
            }

            int v[4] = { (int)e.rect.x, (int)e.rect.y, (int)e.rect.width, (int)e.rect.height };
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputInt4("##rect", v))
            {
                e.rect.x = (float)v[0]; e.rect.y = (float)v[1];
                e.rect.width = (float)v[2]; e.rect.height = (float)v[3];
                normalize(e.rect);
                clampToImage(e.rect, m_imageW, m_imageH);
                m_dirty = true;
            }
            ImGui::TextDisabled("x  y  w  h");
        }

        ImGui::Separator();
        ImGui::TextDisabled("Rejilla uniforme");

        // InputInt2 y no dos InputInt: el InputInt de uno en uno dibuja los
        // botones -/+ DENTRO del ancho pedido, así que con la columna estrecha
        // del panel no quedaba sitio ni para ver el número. Las etiquetas van
        // encima por lo mismo: a la derecha se comían el ancho útil.
        int colsFilas[2] = { m_gridCols, m_gridRows };
        ImGui::TextDisabled("Columnas / Filas");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputInt2("##colsfilas", colsFilas))
        {
            m_gridCols = std::max(1, colsFilas[0]);
            m_gridRows = std::max(1, colsFilas[1]);
        }

        int offset[2] = { m_gridOffsetX, m_gridOffsetY };
        ImGui::TextDisabled("Margen X / Y");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputInt2("##offset", offset))
        {
            m_gridOffsetX = std::max(0, offset[0]);
            m_gridOffsetY = std::max(0, offset[1]);
        }

        int gap[2] = { m_gridSpacingX, m_gridSpacingY };
        ImGui::TextDisabled("Hueco X / Y");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputInt2("##gap", gap))
        {
            m_gridSpacingX = std::max(0, gap[0]);
            m_gridSpacingY = std::max(0, gap[1]);
        }

        ImGui::TextDisabled("Prefijo de los nombres");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##prefijo", m_gridPrefix, sizeof(m_gridPrefix));

        // Y el tamaño de celda que sale de todo eso, para no tener que
        // calcularlo a ojo antes de pulsar.
        if (m_imageW > 0 && m_gridCols > 0 && m_gridRows > 0)
        {
            const float cw = std::floor(((float)m_imageW - (float)m_gridOffsetX -
                                         (float)m_gridSpacingX * (float)(m_gridCols - 1)) /
                                        (float)m_gridCols);
            const float ch = std::floor(((float)m_imageH - (float)m_gridOffsetY -
                                         (float)m_gridSpacingY * (float)(m_gridRows - 1)) /
                                        (float)m_gridRows);
            ImGui::TextDisabled("Celda: %.0f x %.0f px", cw, ch);
        }

        if (ImGui::Button("Generar (reemplaza todo)", ImVec2(-1.0f, 0.0f))) sliceGrid();

        ImGui::EndChild();
    }

    void SpriteEditorPanel::drawImage(EditorContext& ctx)
    {
        (void)ctx;
        ImGui::BeginChild("sprite_canvas", ImVec2(0.0f, 0.0f), true,
                          ImGuiWindowFlags_HorizontalScrollbar);

        if (m_imageW == 0 || m_imageH == 0)
        {
            ImGui::TextDisabled("Abre una imagen desde el campo Atlas de un Button.");
            ImGui::EndChild();
            return;
        }

        const ImVec2 origen = ImGui::GetCursorScreenPos();
        const ImVec2 tamano{ (float)m_imageW * m_zoom, (float)m_imageH * m_zoom };

        if (m_textureId != 0)
            ImGui::Image((ImTextureID)m_textureId, tamano);
        else
            ImGui::Dummy(tamano);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        // El borde de la imagen, para ver dónde acaba cuando el fondo es
        // transparente y el atlas está casi vacío.
        dl->AddRect(origen, ImVec2(origen.x + tamano.x, origen.y + tamano.y),
                    IM_COL32(255, 255, 255, 60));

        // De pantalla a píxeles de la imagen y al revés. Todo el resto de la
        // función trabaja en píxeles de IMAGEN: es lo que se guarda.
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const ImVec2 raton{ (mouse.x - origen.x) / m_zoom, (mouse.y - origen.y) / m_zoom };
        auto aPantalla = [&](float x, float y) {
            return ImVec2(origen.x + x * m_zoom, origen.y + y * m_zoom);
        };

        for (int i = 0; i < (int)m_entries.size(); ++i)
        {
            const UiSpriteRect& r = m_entries[i].rect;
            const ImVec2 p0 = aPantalla(r.x, r.y);
            const ImVec2 p1 = aPantalla(r.x + r.width, r.y + r.height);
            dl->AddRect(p0, p1, colorRect(i == m_selected), 0.0f, 0, i == m_selected ? 2.0f : 1.0f);
            dl->AddText(ImVec2(p0.x + 2.0f, p0.y + 2.0f), colorRect(i == m_selected),
                        m_entries[i].name.c_str());

            if (i == m_selected)
            {
                // Manejadores de las cuatro esquinas.
                const ImVec2 esquinas[4] = { p0, ImVec2(p1.x, p0.y), ImVec2(p0.x, p1.y), p1 };
                for (const ImVec2& c : esquinas)
                    dl->AddRectFilled(ImVec2(c.x - 3.0f, c.y - 3.0f),
                                      ImVec2(c.x + 3.0f, c.y + 3.0f), colorRect(true));
            }
        }

        const bool sobreImagen = ImGui::IsWindowHovered() &&
                                 mouse.x >= origen.x && mouse.x <= origen.x + tamano.x &&
                                 mouse.y >= origen.y && mouse.y <= origen.y + tamano.y;

        if (sobreImagen && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_drag == Drag::None)
        {
            m_dragStartImg = raton;

            // Primero las esquinas del seleccionado: si no, agarrar una esquina
            // que cae dentro del propio rect movería el rect entero.
            if (m_selected >= 0 && m_selected < (int)m_entries.size())
            {
                const UiSpriteRect& r = m_entries[m_selected].rect;
                const ImVec2 esquinas[4] = {
                    aPantalla(r.x, r.y), aPantalla(r.x + r.width, r.y),
                    aPantalla(r.x, r.y + r.height), aPantalla(r.x + r.width, r.y + r.height)
                };
                const Drag modos[4] = { Drag::TopLeft, Drag::TopRight,
                                        Drag::BottomLeft, Drag::BottomRight };
                for (int c = 0; c < 4; ++c)
                {
                    const float dx = mouse.x - esquinas[c].x;
                    const float dy = mouse.y - esquinas[c].y;
                    if (dx * dx + dy * dy <= kHandle * kHandle)
                    {
                        m_drag      = modos[c];
                        m_dragIndex = m_selected;
                        m_dragRect  = r;
                        break;
                    }
                }
            }

            if (m_drag == Drag::None)
            {
                // Al revés: gana el último dibujado, que es el que se ve encima.
                int golpe = -1;
                for (int i = (int)m_entries.size() - 1; i >= 0; --i)
                    if (contains(m_entries[i].rect, raton)) { golpe = i; break; }

                if (golpe >= 0)
                {
                    m_selected  = golpe;
                    std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", m_entries[golpe].name.c_str());
                    m_drag      = Drag::Move;
                    m_dragIndex = golpe;
                    m_dragRect  = m_entries[golpe].rect;
                }
                else
                {
                    // En vacío se crea uno nuevo arrastrando.
                    UiSpriteRect nuevo{};
                    nuevo.x = std::floor(raton.x);
                    nuevo.y = std::floor(raton.y);
                    nuevo.width = nuevo.height = 1.0f;
                    m_entries.push_back(Entry{freeName("sprite_"), nuevo});
                    m_selected  = (int)m_entries.size() - 1;
                    std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s",
                                  m_entries[m_selected].name.c_str());
                    m_drag      = Drag::Creating;
                    m_dragIndex = m_selected;
                    m_dragRect  = nuevo;
                    m_dirty     = true;
                }
            }
        }

        if (m_drag != Drag::None && m_dragIndex >= 0 && m_dragIndex < (int)m_entries.size())
        {
            UiSpriteRect r = m_dragRect;
            const float dx = raton.x - m_dragStartImg.x;
            const float dy = raton.y - m_dragStartImg.y;

            switch (m_drag)
            {
                case Drag::Move:
                    r.x = std::floor(m_dragRect.x + dx);
                    r.y = std::floor(m_dragRect.y + dy);
                    break;
                case Drag::TopLeft:
                    r.x = std::floor(m_dragRect.x + dx);       r.width  = m_dragRect.width  - dx;
                    r.y = std::floor(m_dragRect.y + dy);       r.height = m_dragRect.height - dy;
                    break;
                case Drag::TopRight:
                    r.width  = std::floor(m_dragRect.width + dx);
                    r.y = std::floor(m_dragRect.y + dy);       r.height = m_dragRect.height - dy;
                    break;
                case Drag::BottomLeft:
                    r.x = std::floor(m_dragRect.x + dx);       r.width  = m_dragRect.width - dx;
                    r.height = std::floor(m_dragRect.height + dy);
                    break;
                case Drag::BottomRight:
                    r.width  = std::floor(m_dragRect.width + dx);
                    r.height = std::floor(m_dragRect.height + dy);
                    break;
                case Drag::Creating:
                    r.x = std::floor(std::min(m_dragStartImg.x, raton.x));
                    r.y = std::floor(std::min(m_dragStartImg.y, raton.y));
                    r.width  = std::floor(std::fabs(dx));
                    r.height = std::floor(std::fabs(dy));
                    break;
                default: break;
            }

            normalize(r);
            clampToImage(r, m_imageW, m_imageH);
            m_entries[m_dragIndex].rect = r;
            m_dirty = true;

            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                // Un click sin arrastre al crear deja un rect de 1x1 que no
                // sirve para nada: se descarta en vez de dejar basura.
                if (m_drag == Drag::Creating && (r.width < 2.0f || r.height < 2.0f))
                {
                    m_entries.erase(m_entries.begin() + m_dragIndex);
                    m_selected = -1;
                }
                m_drag = Drag::None;
                m_dragIndex = -1;
            }
        }

        ImGui::EndChild();
    }

    void SpriteEditorPanel::draw(EditorContext& ctx)
    {
        if (!m_open) return;

        ImGui::SetNextWindowSize(ImVec2(900.0f, 560.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Sprite Editor", &m_open))
        {
            ImGui::End();
            return;
        }

        drawToolbar(ctx);
        ImGui::Separator();
        drawSidebar(ctx);
        ImGui::SameLine();
        drawImage(ctx);

        ImGui::End();
    }
}
