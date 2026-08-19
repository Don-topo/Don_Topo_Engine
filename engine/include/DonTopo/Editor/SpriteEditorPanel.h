#pragma once

// Editor de sprites: trocea una imagen en sub-rects con nombre y los guarda en
// el sidecar que lee UiTextureAtlas (<imagen>.sprites.json).
//
// Existe porque sin sub-rects registrados TODO nombre de sprite cae al rect
// completo del atlas: el Sprite Swap de un botón no cambiaba nada visible y el
// 9-slice solo servía con un fichero por trozo. El motor sabía hacerlo desde
// siempre; lo que faltaba era por dónde decírselo.
//
// Edita un ASSET, no la escena, así que NO pasa por el stack de undo del editor
// (ese deshace cambios de escena). El botón de recargar es el "deshacer": tira
// lo que haya en pantalla y vuelve a lo que dice el fichero.

#include "DonTopo/UI/UiTextureAtlas.h"

#include <imgui.h>

#include <cstdint>
#include <string>
#include <vector>

namespace DonTopo
{
    struct EditorContext;

    class SpriteEditorPanel
    {
    public:
        void  draw(EditorContext& ctx);
        bool* GetOpenPtr() { return &m_open; }

        // Abre la imagen y trae sus sprites del sidecar, si lo hay. Volver a
        // abrir la MISMA imagen no descarta lo que se esté editando: sería
        // perder trabajo por pulsar dos veces el mismo botón.
        void open(EditorContext& ctx, const std::string& imagePath);

    private:
        struct Entry
        {
            std::string  name;
            UiSpriteRect rect{};
        };

        // Qué se está arrastrando. El redimensionado va por esquina: son las
        // cuatro que un rect puede mover sin invertirse.
        enum class Drag { None, Move, TopLeft, TopRight, BottomLeft, BottomRight, Creating };

        void loadFrom(EditorContext& ctx, const std::string& imagePath);
        void save(EditorContext& ctx);
        void drawToolbar(EditorContext& ctx);
        void drawSidebar(EditorContext& ctx);
        void drawImage(EditorContext& ctx);
        void sliceGrid();
        // Nombre libre con el prefijo dado: "sprite_3" si sprite_0..2 están.
        std::string freeName(const std::string& prefix) const;
        int  indexOfName(const std::string& name) const;

        bool        m_open = false;
        std::string m_path;                    // imagen abierta; vacío = ninguna
        std::string m_error;                   // por qué no se pudo abrir
        uint64_t    m_textureId = 0;           // handle de ImGui, 0 = sin imagen
        uint32_t    m_imageW    = 0;
        uint32_t    m_imageH    = 0;

        std::vector<Entry> m_entries;
        int   m_selected = -1;
        bool  m_dirty    = false;
        float m_zoom     = 1.0f;

        // Rejilla uniforme: cubre las hojas de sprites regulares, que son la
        // mayoría de la UI. Reemplaza TODO, y por eso el botón lo dice.
        int  m_gridCols = 4;
        int  m_gridRows = 2;
        int  m_gridOffsetX = 0;
        int  m_gridOffsetY = 0;
        int  m_gridSpacingX = 0;
        int  m_gridSpacingY = 0;
        char m_gridPrefix[64] = "sprite_";

        // Arrastre en curso. m_dragRect es el rect ANTES de empezar: así el
        // arrastre siempre se calcula contra el original y no acumula error.
        Drag         m_drag = Drag::None;
        int          m_dragIndex = -1;
        ImVec2       m_dragStartImg{0.0f, 0.0f};
        UiSpriteRect m_dragRect{};

        char m_nameBuf[128] = {};
    };
}
