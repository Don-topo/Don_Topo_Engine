#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
    // Axis and direction of the bar's fill. The component's OWN enum and not
    // UiFillDirection from UiWidgets.h: that one only distinguishes Horizontal/Vertical
    // (Image's Filled mode), and here the DIRECTION is also needed. A
    // health bar that drains toward the left is not the same as one that drains
    // toward the right.
    enum class UiProgressFillDirection
    {
        LeftToRight,
        RightToLeft,
        BottomToTop,
        TopToBottom
    };

    // A 2D UI progress bar as a GameObject component, with the
    // SAME contract as CanvasComponent, ButtonComponent and TextComponent: DATA
    // ONLY. The live tree is held by the Renderer (Renderer::uiCanvas()) and
    // syncUiWidgets() builds/updates it every frame.
    //
    // It is drawn by COMPOSITION: the core (DonTopo::ProgressBar, UiWidgets.h)
    // is a stub with no value or colors, and UiSpriteBatch does not know how to emit a partial
    // fill. So the sync assembles TWO nodes (the background, the whole rect, and a
    // child with the fill rect), exactly like the Button's label. The
    // component does not touch the UI core.
    class ProgressBarComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};    // px, relative to the anchor
            glm::vec2 size{160.0f, 20.0f};     // px
            glm::vec4 color{0.2f, 0.2f, 0.2f, 1.0f};   // color of the BACKGROUND
            bool      visible = true;

            // --- Value ---------------------------------------------------------
            // No clamp HERE on purpose: the component interprets nothing (same
            // criterion as the rest). The sync normalizes, when computing the
            // fill rect.
            float value    = 0.5f;
            float minValue = 0.0f;
            float maxValue = 1.0f;

            // --- Fill ----------------------------------------------------------
            glm::vec4 fillColor{0.25f, 0.7f, 1.0f, 1.0f};

            UiProgressFillDirection fillDirection = UiProgressFillDirection::LeftToRight;

            // --- Assets --------------------------------------------------------
            // THREE image paths, not sprite names: a core "sprite"
            // is a sub-rect that has to be registered by hand with
            // UiTextureAtlas::addSprite, and the editor registers none (a loose
            // image is used whole, which is what uvRect returns without a
            // name). So each part brings its own file.
            //
            // atlasPath is the SHARED fallback: what is used for any part that
            // has no path of its own. All three empty = flat-color quads.
            std::string atlasPath;
            std::string backgroundPath;
            std::string fillPath;

            // Fraction of the rect that the fill occupies, already bounded to [0,1]. A
            // degenerate range (max <= min) gives 0: there is no way to split an
            // empty interval, and a full bar would be lying about the data.
            float normalizedValue() const
            {
                if (!(maxValue > minValue)) return 0.0f;
                const float t = (value - minValue) / (maxValue - minValue);
                return std::clamp(t, 0.0f, 1.0f);
            }

            // Rect of the fill IN PARENT COORDINATES (the background node), which
            // is what it hangs from. Here and not in the sync so it can be tested without
            // canvas or GPU.
            void fillRect(glm::vec2& outPos, glm::vec2& outSize) const
            {
                const float t = normalizedValue();
                const float w = size.x;
                const float h = size.y;
                switch (fillDirection)
                {
                    case UiProgressFillDirection::RightToLeft:
                        outPos  = glm::vec2(w * (1.0f - t), 0.0f);
                        outSize = glm::vec2(w * t, h);
                        break;
                    case UiProgressFillDirection::TopToBottom:
                        // The canvas Y grows DOWNWARD: filling from the top
                        // means leaving the origin still and growing the height.
                        outPos  = glm::vec2(0.0f, 0.0f);
                        outSize = glm::vec2(w, h * t);
                        break;
                    case UiProgressFillDirection::BottomToTop:
                        outPos  = glm::vec2(0.0f, h * (1.0f - t));
                        outSize = glm::vec2(w, h * t);
                        break;
                    default:   // LeftToRight
                        outPos  = glm::vec2(0.0f, 0.0f);
                        outSize = glm::vec2(w * t, h);
                        break;
                }
            }

            // Dumps the rect and the background into the live node. Does NOT touch `atlas` (it is a
            // GPU pointer: the sync resolves it).
            void applyTo(ProgressBar& p) const
            {
                p.anchorMin = anchorMin;
                p.anchorMax = anchorMax;
                p.pivot     = pivot;
                p.position  = position;
                p.size      = size;
                p.color     = color;
                p.visible   = visible;
            }

            // And the same for the fill child. Anchors and pivot at zero: its rect
            // is counted in pixels from the background's corner, which is exactly
            // what fillRect() returns.
            void applyToFill(UiElement& f) const
            {
                glm::vec2 pos{0.0f};
                glm::vec2 sz{0.0f};
                fillRect(pos, sz);

                f.anchorMin = glm::vec2(0.0f);
                f.anchorMax = glm::vec2(0.0f);
                f.pivot     = glm::vec2(0.0f);
                f.position  = pos;
                f.size      = sz;
                f.color     = fillColor;
                f.visible   = true;
                // At value 0 the rect is degenerate: better not to emit the quad than to
                // emit one of zero area (and with a sprite, one of 0 px width
                // with full UVs).
                f.drawable  = (sz.x > 0.0f && sz.y > 0.0f);
            }

            // The sync uses it to know whether there is anything to dump: without this the
            // node would have to be dirtied EVERY frame, which is exactly what the
            // canvas's vertex cache exists to avoid.
            bool operator==(const ProgressBarComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       value == o.value && minValue == o.minValue && maxValue == o.maxValue &&
                       fillColor == o.fillColor && fillDirection == o.fillDirection &&
                       atlasPath == o.atlasPath && backgroundPath == o.backgroundPath &&
                       fillPath == o.fillPath;
            }
            bool operator!=(const ProgressBarComponent& o) const { return !(*this == o); }
    };

    // Name of a ProgressBar's live node inside the canvas. Same role as
    // uiButtonNodeName/uiTextNodeName and with a DIFFERENT prefix on purpose: a
    // GameObject can carry all three components at once, and two sibling nodes
    // with the same name would make the gizmo and picking grab the
    // wrong one.
    inline std::string uiProgressBarNodeName(uint64_t ownerId)
    {
        return "bar:" + std::to_string(ownerId);
    }

    // Inverse of uiProgressBarNodeName. Returns 0 if the name is not a
    // bar's: 0 is not a valid GameObject id. Cutting at '/' makes the
    // fill node ("bar:7/Fill") also return its owner.
    inline uint64_t uiProgressBarOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("bar:", 0) != 0) return 0;
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
