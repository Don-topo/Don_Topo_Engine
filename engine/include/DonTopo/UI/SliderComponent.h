#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
    class SliderComponent;

    // Axis and direction of travel. The component's OWN enum and not
    // UiFillDirection from UiWidgets.h, for the same reason ProgressBar has its
    // own: that one only distinguishes Horizontal/Vertical, and here the DIRECTION is also
    // needed (a bass slider that grows toward the left is not the
    // same as one that grows toward the right).
    enum class UiSliderDirection
    {
        LeftToRight,
        RightToLeft,
        BottomToTop,
        TopToBottom
    };

    // What a slider has LIVE and is not serialized. Same role and same
    // reasons as UiButtonRuntime (ButtonComponent.h): the canvas node is
    // destroyed by clearChildren() on every rebuild, so the owner of the
    // callback is the COMPONENT and the node only keeps a weak_ptr to this.
    //
    // `owner` is what the Button does not need: the drag has to write the
    // value IN THE COMPONENT (it is what gets serialized and what the editor reads), not
    // in the node. The sync sets it on every dump; if the component dies, the runtime dies
    // with it and the node's weak_ptr no longer resolves, so the
    // pointer never dangles.
    struct UiSliderRuntime
    {
        std::function<void(float)> onValueChanged;
        SliderComponent*           owner = nullptr;
    };

    // The runtime slot inside the component, with the SAME two rules as the
    // Button's UiCallbackSlot: copying a component gets fresh callbacks (a clone
    // does not fire the original's) and comparing them ignores this field (the sync uses
    // operator== to know whether to dump, and a callback is not data to
    // dump).
    struct UiSliderCallbackSlot
    {
        std::shared_ptr<UiSliderRuntime> ptr = std::make_shared<UiSliderRuntime>();

        UiSliderCallbackSlot() = default;
        UiSliderCallbackSlot(const UiSliderCallbackSlot&) {}
        UiSliderCallbackSlot& operator=(const UiSliderCallbackSlot&) { return *this; }
        UiSliderCallbackSlot(UiSliderCallbackSlot&&) = default;
        UiSliderCallbackSlot& operator=(UiSliderCallbackSlot&&) = default;

        bool operator==(const UiSliderCallbackSlot&) const { return true; }
    };

    // A 2D UI slider as a GameObject component, with the SAME contract
    // as the rest: DATA ONLY. The live tree is held by the Renderer and
    // syncUiWidgets() assembles it every frame.
    //
    // The core Slider (UiWidgets.h) is a stub with NO fields, so the
    // widget is assembled by COMPOSITION just like the ProgressBar: the track is the
    // root node (of type Slider) and the fill and the handle hang from it. Not
    // a single line of the UI core is touched here.
    //
    // The difference from the bar is the INPUT: the sync hooks handlers onto the
    // track that write `value` here. That is why the sync receives this component
    // by NON-const pointer, unlike those that are only drawn.
    class SliderComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};     // px, relative to the anchor
            glm::vec2 size{200.0f, 20.0f};      // px
            glm::vec4 color{0.2f, 0.2f, 0.2f, 1.0f};   // color of the TRACK
            bool      visible = true;

            // When false it is drawn the same but cannot be moved: it is the "read
            // only" mode of a HUD that shows a value without letting it be touched.
            bool interactable = true;

            // --- Value ---------------------------------------------------------
            // No clamp HERE on purpose (same criterion as ProgressBar): the
            // component interprets nothing. normalizedValue normalizes,
            // and valueFromNormalized rounds on write.
            float value    = 0.0f;
            float minValue = 0.0f;
            float maxValue = 1.0f;

            // The value that is WRITTEN is rounded, not only the one that is drawn: if
            // the rounding were on the drawing side, the component would store 3.7 and a
            // script would read 3.7 with the handle showing 4.
            bool wholeNumbers = false;

            UiSliderDirection direction = UiSliderDirection::LeftToRight;

            // --- Fill and handle -----------------------------------------------
            glm::vec4 fillColor{0.25f, 0.7f, 1.0f, 1.0f};
            glm::vec4 handleColor{1.0f, 1.0f, 1.0f, 1.0f};

            // Length of the handle ALONG the travel axis, in px of the rect. It is subtracted
            // from the travel so the handle does not stick out at the ends. At 0 the
            // travel is the whole rect.
            float handleSize = 20.0f;

            // --- Sprites -------------------------------------------------------
            // ONE atlas for the three parts and three sub-rect NAMES inside
            // it (registered by the sidecar <atlas>.sprites.json). A single atlas load
            // instead of ProgressBar's three loose paths.
            std::string atlasPath;
            std::string backgroundSprite;
            std::string fillSprite;
            std::string handleSprite;

            // --- Runtime (not serialized) ---------------------------------------
            UiSliderCallbackSlot callbacks;

            bool isVertical() const
            {
                return direction == UiSliderDirection::BottomToTop ||
                       direction == UiSliderDirection::TopToBottom;
            }

            // Direction inverted relative to the axis's natural growth. The canvas Y
            // grows DOWNWARD, so "bottom to top" is the inverted one
            // vertically.
            bool isReversed() const
            {
                return direction == UiSliderDirection::RightToLeft ||
                       direction == UiSliderDirection::BottomToTop;
            }

            // Fraction of the rect that the handle occupies on the travel axis, bounded
            // to [0,1]. A degenerate rect gives 0.
            float handleFraction() const
            {
                const float largo = isVertical() ? size.y : size.x;
                if (!(largo > 0.0f)) return 0.0f;
                return std::clamp(handleSize / largo, 0.0f, 1.0f);
            }

            // The value in [0,1]. A degenerate range (max <= min) gives 0: there is no
            // way to split an empty interval.
            float normalizedValue() const
            {
                if (!(maxValue > minValue)) return 0.0f;
                return std::clamp((value - minValue) / (maxValue - minValue), 0.0f, 1.0f);
            }

            // The way back: from [0,1] to the value, with the range and the
            // wholeNumbers rounding already applied.
            float valueFromNormalized(float t) const
            {
                if (!(maxValue > minValue)) return minValue;
                const float clamped = std::clamp(t, 0.0f, 1.0f);
                float v = minValue + clamped * (maxValue - minValue);
                if (wholeNumbers) v = std::round(v);
                return std::clamp(v, minValue, maxValue);
            }

            // From a point IN RECT COORDINATES (px from its top-left
            // corner) to the normalized value. rectSize comes in separately and is not read
            // from `size` because the input works in SCREEN pixels, which
            // have the canvas scale applied; the handle fraction is
            // unitless and works the same in both spaces.
            float normalizedFromLocal(glm::vec2 local, glm::vec2 rectSize) const
            {
                const float largo = isVertical() ? rectSize.y : rectSize.x;
                if (!(largo > 0.0f)) return 0.0f;

                const float frac = handleFraction();
                // USEFUL travel: the handle moves between its left edge at 0 and
                // its right edge at the end, so the center travels
                // (length - handle) and starts half a handle from the beginning.
                const float util = largo * (1.0f - frac);
                const float p    = (isVertical() ? local.y : local.x) - largo * frac * 0.5f;

                // Handle as large as the track: there is no travel, and dividing would give
                // an infinity. Any point is worth the same.
                float t = (util > 0.0f) ? (p / util) : 0.0f;
                t = std::clamp(t, 0.0f, 1.0f);
                return isReversed() ? (1.0f - t) : t;
            }

            // Rect of the FILL in track coordinates. It reaches up to the center
            // of the handle, which is where it marks the value.
            void fillRect(glm::vec2& outPos, glm::vec2& outSize) const
            {
                const float t    = normalizedValue();
                const float frac = handleFraction();
                // Same travel as the handle plus half a handle: the fill's edge
                // ends exactly at the handle's center at any position.
                const float f = t * (1.0f - frac) + frac * 0.5f;

                if (isVertical())
                {
                    const float h = size.y * f;
                    outSize = glm::vec2(size.x, h);
                    // BottomToTop grows upward: the fill sticks to the bottom.
                    outPos  = glm::vec2(0.0f, isReversed() ? (size.y - h) : 0.0f);
                }
                else
                {
                    const float wpx = size.x * f;
                    outSize = glm::vec2(wpx, size.y);
                    outPos  = glm::vec2(isReversed() ? (size.x - wpx) : 0.0f, 0.0f);
                }
            }

            // Rect of the HANDLE in track coordinates, already bounded so it does not
            // stick out at either end.
            void handleRect(glm::vec2& outPos, glm::vec2& outSize) const
            {
                const float t    = normalizedValue();
                const float frac = handleFraction();
                const float tt   = isReversed() ? (1.0f - t) : t;

                if (isVertical())
                {
                    const float h = size.y * frac;
                    outSize = glm::vec2(size.x, h);
                    outPos  = glm::vec2(0.0f, (size.y - h) * tt);
                }
                else
                {
                    const float wpx = size.x * frac;
                    outSize = glm::vec2(wpx, size.y);
                    outPos  = glm::vec2((size.x - wpx) * tt, 0.0f);
                }
            }

            // Dumps the rect and the track into the live node. Does NOT touch `atlas` (it is a
            // GPU pointer: the sync resolves it).
            void applyTo(Slider& s) const
            {
                s.anchorMin = anchorMin;
                s.anchorMax = anchorMax;
                s.pivot     = pivot;
                s.position  = position;
                s.size      = size;
                s.color     = color;
                s.visible   = visible;
                s.sprite    = backgroundSprite;
                // The track ALWAYS receives the mouse, interactable or not: the interactable
                // gate is in the handler, and removing its raycast here would
                // make whatever was behind it swallow the click.
                s.raycastTarget = true;
            }

            void applyToFill(UiElement& f) const
            {
                glm::vec2 pos{0.0f}, sz{0.0f};
                fillRect(pos, sz);

                f.anchorMin = glm::vec2(0.0f);
                f.anchorMax = glm::vec2(0.0f);
                f.pivot     = glm::vec2(0.0f);
                f.position  = pos;
                f.size      = sz;
                f.color     = fillColor;
                f.sprite    = fillSprite;
                f.visible   = true;
                // For a degenerate rect the quad is not emitted: one of zero area with
                // full UVs is garbage in the buffer.
                f.drawable  = (sz.x > 0.0f && sz.y > 0.0f);
                // Neither the fill nor the handle receive the mouse: the hit test returns
                // the DEEPEST node, so a raycastable child would swallow the
                // drag that starts on top of it and the track would not find out.
                f.raycastTarget = false;
            }

            void applyToHandle(UiElement& h) const
            {
                glm::vec2 pos{0.0f}, sz{0.0f};
                handleRect(pos, sz);

                h.anchorMin = glm::vec2(0.0f);
                h.anchorMax = glm::vec2(0.0f);
                h.pivot     = glm::vec2(0.0f);
                h.position  = pos;
                h.size      = sz;
                h.color     = handleColor;
                h.sprite    = handleSprite;
                h.visible   = true;
                h.drawable  = (sz.x > 0.0f && sz.y > 0.0f);
                h.raycastTarget = false;
            }

            // The sync uses it to know whether there is anything to dump: without this the
            // node would have to be dirtied EVERY frame, which is exactly what the
            // canvas's vertex cache exists to avoid.
            bool operator==(const SliderComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       interactable == o.interactable &&
                       value == o.value && minValue == o.minValue && maxValue == o.maxValue &&
                       wholeNumbers == o.wholeNumbers && direction == o.direction &&
                       fillColor == o.fillColor && handleColor == o.handleColor &&
                       handleSize == o.handleSize &&
                       atlasPath == o.atlasPath && backgroundSprite == o.backgroundSprite &&
                       fillSprite == o.fillSprite && handleSprite == o.handleSprite;
            }
            bool operator!=(const SliderComponent& o) const { return !(*this == o); }
    };

    // Name of a Slider's live node inside the canvas. DIFFERENT prefix from
    // the others for the same reason those differ among themselves: a GameObject can
    // carry several UI components at once, and two sibling nodes with the
    // same name would make the gizmo and picking grab the wrong one.
    inline std::string uiSliderNodeName(uint64_t ownerId)
    {
        return "sld:" + std::to_string(ownerId);
    }

    // Inverse of uiSliderNodeName. Returns 0 if the name is not a slider's: 0
    // is not a valid GameObject id. Cutting at '/' makes the fill and handle
    // nodes also return their owner.
    inline uint64_t uiSliderOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("sld:", 0) != 0) return 0;
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
