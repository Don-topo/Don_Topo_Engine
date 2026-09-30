#pragma once

// Sprite editor: slices an image into named sub-rects and saves them in
// the sidecar that UiTextureAtlas reads (<image>.sprites.json).
//
// It exists because without registered sub-rects EVERY sprite name falls back to the
// atlas's full rect: a button's Sprite Swap changed nothing visible and the
// 9-slice only worked with one file per piece. The engine always knew how to do it;
// what was missing was a way to tell it.
//
// It edits an ASSET, not the scene, so it does NOT go through the editor's undo stack
// (that one undoes scene changes). The reload button is the "undo": it throws
// away whatever is on screen and goes back to what the file says.

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

        // Opens the image and brings in its sprites from the sidecar, if there is one. Opening
        // the SAME image again does not discard what is being edited: that would be
        // losing work by pressing the same button twice.
        void open(EditorContext& ctx, const std::string& imagePath);

    private:
        struct Entry
        {
            std::string  name;
            UiSpriteRect rect{};
        };

        // What is being dragged. Resizing goes by corner: those are the
        // four that a rect can move without inverting.
        enum class Drag { None, Move, TopLeft, TopRight, BottomLeft, BottomRight, Creating };

        void loadFrom(EditorContext& ctx, const std::string& imagePath);
        void save(EditorContext& ctx);
        void drawToolbar(EditorContext& ctx);
        void drawSidebar(EditorContext& ctx);
        void drawImage(EditorContext& ctx);
        void sliceGrid();
        // Free name with the given prefix: "sprite_3" if sprite_0..2 are taken.
        std::string freeName(const std::string& prefix) const;
        int  indexOfName(const std::string& name) const;

        bool        m_open = false;
        // Request to bring the window to the front, set by open() and consumed
        // by the next draw(). m_open is not enough: docked in a tab
        // group, "open" only means the tab exists, and the
        // atlas opened behind whichever tab had focus.
        bool        m_focusRequested = false;
        std::string m_path;                    // open image; empty = none
        std::string m_error;                   // why it could not be opened
        uint64_t    m_textureId = 0;           // ImGui handle, 0 = no image
        uint32_t    m_imageW    = 0;
        uint32_t    m_imageH    = 0;

        std::vector<Entry> m_entries;
        int   m_selected = -1;
        bool  m_dirty    = false;
        float m_zoom     = 1.0f;

        // Uniform grid: covers regular sprite sheets, which are the
        // majority of the UI. It replaces EVERYTHING, and that is why the button says so.
        int  m_gridCols = 4;
        int  m_gridRows = 2;
        int  m_gridOffsetX = 0;
        int  m_gridOffsetY = 0;
        int  m_gridSpacingX = 0;
        int  m_gridSpacingY = 0;
        char m_gridPrefix[64] = "sprite_";

        // Drag in progress. m_dragRect is the rect BEFORE starting: this way the
        // drag is always computed against the original and does not accumulate error.
        Drag         m_drag = Drag::None;
        int          m_dragIndex = -1;
        ImVec2       m_dragStartImg{0.0f, 0.0f};
        UiSpriteRect m_dragRect{};

        char m_nameBuf[128] = {};
    };
}
