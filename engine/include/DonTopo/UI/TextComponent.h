#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "DonTopo/UI/ButtonComponent.h"
#include "DonTopo/UI/ImageComponent.h"
#include "DonTopo/UI/LayoutComponent.h"
#include "DonTopo/UI/PanelComponent.h"
#include "DonTopo/UI/ProgressBarComponent.h"
#include "DonTopo/UI/SliderComponent.h"
#include "DonTopo/UI/CheckboxComponent.h"
#include "DonTopo/UI/ToggleComponent.h"
#include "DonTopo/UI/ScrollbarComponent.h"
#include "DonTopo/UI/InputFieldComponent.h"
#include "DonTopo/UI/DropdownComponent.h"
#include "DonTopo/UI/ScrollViewComponent.h"
#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiFont.h"
#include "DonTopo/UI/UiTextureAtlas.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
    // A 2D UI label as a GameObject component, with the SAME
    // contract as CanvasComponent and ButtonComponent: DATA ONLY. It holds neither
    // a UiElement nor the font. The live tree is still held by the Renderer
    // (Renderer::uiCanvas()), and whoever draws rebuilds/updates it every frame
    // with syncUiWidgets(). This way what is seen in Play and in the exported game comes from
    // the SCENE and not from a hand-wired tree.
    //
    // The names, defaults and meaning are EXACTLY those of the core:
    //   - the rect block belongs to UiElement (UiCanvas.h), the same one
    //     ButtonComponent replicates,
    //   - the rest are ALL the fields of Text (UiWidgets.h), except `font`.
    // This component neither interprets nor clamps anything.
    //
    // fontPath is the ONLY thing that is not a core field: the core holds a
    // pointer to a GPU resource, which is not serialized. The sync resolves it
    // against the Renderer, not the component.
    class TextComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};   // px, relative to the anchor
            glm::vec2 size{160.0f, 40.0f};    // px
            glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};   // glyph fill
            bool      visible = true;

            // --- Text (Text) --------------------------------------------------
            std::string text;
            std::string fontPath;   // TTF; empty = the default font
            float       fontSize = 16.0f;

            float     outlineWidth = 0.0f;
            glm::vec4 outlineColor{0.0f, 0.0f, 0.0f, 1.0f};

            glm::vec2 shadowOffset{0.0f, 0.0f};
            glm::vec4 shadowColor{0.0f, 0.0f, 0.0f, 0.5f};

            UiTextAlign    align    = UiTextAlign::Left;
            UiTextVAlign   vAlign   = UiTextVAlign::Top;
            UiTextOverflow overflow = UiTextOverflow::Overflow;
            bool           wordWrap = false;

            float boldStrength = 0.08f;
            float italicSkew   = 0.25f;

            // Dumps the rect and the text into the live node. Does NOT touch `font` (it is a
            // GPU pointer: the sync resolves it).
            void applyTo(Text& t) const
            {
                t.anchorMin = anchorMin;
                t.anchorMax = anchorMax;
                t.pivot     = pivot;
                t.position  = position;
                t.size      = size;
                t.color     = color;
                t.visible   = visible;

                t.text         = text;
                t.fontSize     = fontSize;
                t.outlineWidth = outlineWidth;
                t.outlineColor = outlineColor;
                t.shadowOffset = shadowOffset;
                t.shadowColor  = shadowColor;
                t.align        = align;
                t.vAlign       = vAlign;
                t.overflow     = overflow;
                t.wordWrap     = wordWrap;
                t.boldStrength = boldStrength;
                t.italicSkew   = italicSkew;
            }

            // The sync uses it to know whether there is anything to dump: without this the
            // node would have to be dirtied EVERY frame, which is exactly what the
            // canvas's vertex cache exists to avoid.
            bool operator==(const TextComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       text == o.text && fontPath == o.fontPath && fontSize == o.fontSize &&
                       outlineWidth == o.outlineWidth && outlineColor == o.outlineColor &&
                       shadowOffset == o.shadowOffset && shadowColor == o.shadowColor &&
                       align == o.align && vAlign == o.vAlign &&
                       overflow == o.overflow && wordWrap == o.wordWrap &&
                       boldStrength == o.boldStrength && italicSkew == o.italicSkew;
            }
            bool operator!=(const TextComponent& o) const { return !(*this == o); }
    };

    // Name of a Text's live node inside the canvas. Same role as
    // uiButtonNodeName (the only way back from the UI tree to the GameObject) and
    // with a DIFFERENT prefix on purpose: a GameObject can carry Button and Text
    // at once, and two sibling nodes with the same name would make the gizmo and
    // picking grab the wrong one.
    inline std::string uiTextNodeName(uint64_t ownerId)
    {
        return "txt:" + std::to_string(ownerId);
    }

    // Inverse of uiTextNodeName. Returns 0 if the name is not a Text's: 0 is not
    // a valid GameObject id.
    inline uint64_t uiTextOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("txt:", 0) != 0) return 0;
        uint64_t id = 0;
        for (size_t i = 4; i < nodeName.size(); i++)
        {
            const char c = nodeName[i];
            if (c == '/') break;
            if (c < '0' || c > '9') return 0;
            id = id * 10 + (uint64_t)(c - '0');
        }
        return id;
    }

}

// Compatibility: the sync machinery moved to UiWidgetSync.h and there are ~80
// call sites that include this file expecting to find it. It is included
// at the END on purpose: UiWidgetSync.h needs the complete TextComponent.
#include "DonTopo/UI/UiWidgetSync.h"
