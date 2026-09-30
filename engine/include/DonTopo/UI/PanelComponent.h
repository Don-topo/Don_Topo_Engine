#pragma once
#include <cstdint>
#include <string>

#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
    // A 2D UI Panel as a GameObject component, with the SAME contract
    // as CanvasComponent, ButtonComponent, TextComponent, ProgressBarComponent
    // and LayoutComponent: DATA ONLY. The live tree is held by the Renderer
    // (Renderer::uiCanvas()) and syncUiWidgets() builds/updates it every frame.
    //
    // The core Panel (UiWidgets.h) has NOT A SINGLE field of its own: it is a
    // UiElement with another typeName(), that is the background rectangle that
    // frames, groups and windows are built from. So there is nothing here beyond the
    // rect block the others already share, the atlas/sprite pair and raycastTarget.
    //
    // raycastTarget is here and not in the others because in a panel it is THE field that
    // decides the behavior: a full-screen background with raycastTarget set to
    // true swallows the clicks of everything behind it, and since nothing
    // betrays it visually, without this field the editor would be hiding something
    // the core does support.
    class PanelComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};      // px, relative to the anchor
            glm::vec2 size{200.0f, 120.0f};      // px
            glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
            bool      visible = true;

            // When false the panel is still drawn but lets the mouse through: this is
            // what a decorative background behind live widgets wants.
            bool raycastTarget = true;

            // --- Sprite --------------------------------------------------------
            // Empty = flat-color quad, which is what UiElement draws without an
            // atlas. `sprite` is a NAME inside the atlas (registered by the
            // sidecar <atlas>.sprites.json); empty = the whole image.
            std::string atlasPath;
            std::string sprite;

            // Dumps the rect and the sprite into the live node. Does NOT touch `atlas` (it is a
            // GPU pointer: the sync resolves it).
            void applyTo(Panel& p) const
            {
                p.anchorMin     = anchorMin;
                p.anchorMax     = anchorMax;
                p.pivot         = pivot;
                p.position      = position;
                p.size          = size;
                p.color         = color;
                p.visible       = visible;
                p.raycastTarget = raycastTarget;
                p.sprite        = sprite;
            }

            // The sync uses it to know whether there is anything to dump: without this the
            // node would have to be dirtied EVERY frame, which is exactly what the
            // canvas's vertex cache exists to avoid.
            bool operator==(const PanelComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       raycastTarget == o.raycastTarget &&
                       atlasPath == o.atlasPath && sprite == o.sprite;
            }
            bool operator!=(const PanelComponent& o) const { return !(*this == o); }
    };

    // Name of a Panel's live node inside the canvas. DIFFERENT prefix from the
    // button's ("go:"), the text's ("txt:"), the bar's ("bar:") and the container's
    // ("lay:") for the same reason those differ among themselves: a GameObject can carry
    // several UI components at once, and two sibling nodes with the same
    // name would make the gizmo and picking grab the wrong one.
    inline std::string uiPanelNodeName(uint64_t ownerId)
    {
        return "pnl:" + std::to_string(ownerId);
    }

    // Inverse of uiPanelNodeName. Returns 0 if the name is not a panel's: 0
    // is not a valid GameObject id.
    inline uint64_t uiPanelOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("pnl:", 0) != 0) return 0;
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
