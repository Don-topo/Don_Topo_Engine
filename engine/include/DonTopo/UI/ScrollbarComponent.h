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
    class ScrollbarComponent;

    // Axis and direction of travel. The component's OWN enum, like ProgressBar's
    // and Slider's: each widget declares its own instead of
    // sharing one, so a change in the bar does not drag the slider along.
    enum class UiScrollbarDirection
    {
        LeftToRight,
        RightToLeft,
        TopToBottom,
        BottomToTop
    };

    // What a bar has LIVE and is not serialized. Same role and same
    // reasons as UiButtonRuntime and UiSliderRuntime.
    struct UiScrollbarRuntime
    {
        std::function<void(float)> onValueChanged;
        ScrollbarComponent*        owner = nullptr;
    };

    struct UiScrollbarCallbackSlot
    {
        std::shared_ptr<UiScrollbarRuntime> ptr = std::make_shared<UiScrollbarRuntime>();

        UiScrollbarCallbackSlot() = default;
        UiScrollbarCallbackSlot(const UiScrollbarCallbackSlot&) {}
        UiScrollbarCallbackSlot& operator=(const UiScrollbarCallbackSlot&) { return *this; }
        UiScrollbarCallbackSlot(UiScrollbarCallbackSlot&&) = default;
        UiScrollbarCallbackSlot& operator=(UiScrollbarCallbackSlot&&) = default;

        bool operator==(const UiScrollbarCallbackSlot&) const { return true; }
    };

    // A 2D UI scroll bar as a GameObject component, with the
    // SAME contract as the rest: DATA ONLY. The core Scrollbar
    // (UiWidgets.h) is a stub with NO fields, so the widget is assembled by
    // COMPOSITION: the channel is the root node (of type Scrollbar) and the handle hangs
    // from it.
    //
    // It looks like the Slider but is NOT the same widget: here the handle has a
    // VARIABLE size (the fraction of the content that is visible) and the value is always
    // 0..1. There is no range of its own because whatever is scrolled interprets it,
    // not the bar.
    class ScrollbarComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};    // px, relative to the anchor
            glm::vec2 size{20.0f, 200.0f};     // px
            glm::vec4 color{0.15f, 0.15f, 0.15f, 1.0f};   // color of the CHANNEL
            bool      visible = true;

            bool interactable = true;

            // --- Value ---------------------------------------------------------
            // Always 0..1. Whoever writes does the clamp (snapValue and the
            // sync's handler), not the field: the component interprets nothing,
            // like the rest.
            float value = 0.0f;

            // Fraction of the channel that the handle occupies: 1 = the whole content fits
            // (there is nothing to scroll), 0 = a handle of zero thickness.
            float handleFraction = 0.25f;

            UiScrollbarDirection direction = UiScrollbarDirection::TopToBottom;

            // Discrete stops. 0 and 1 = continuous: snapping to a single stop
            // would leave the bar dead in one place. With N >= 2 there are N stops
            // spread over the whole travel (0, 1/(N-1), ..., 1), like Unity.
            uint32_t numberOfSteps = 0;

            // --- Handle ---------------------------------------------------------
            glm::vec4 handleColor{0.6f, 0.6f, 0.6f, 1.0f};

            // How much the mouse wheel moves per notch, as a fraction of the
            // travel. A field and not a hidden constant: a long list and a
            // three-option selector do not want the same step.
            float scrollStep = 0.1f;

            // --- Sprites --------------------------------------------------------
            std::string atlasPath;
            std::string backgroundSprite;
            std::string handleSprite;

            // --- Runtime (not serialized) ---------------------------------------
            UiScrollbarCallbackSlot callbacks;

            bool isVertical() const
            {
                return direction == UiScrollbarDirection::TopToBottom ||
                       direction == UiScrollbarDirection::BottomToTop;
            }

            // Direction inverted relative to the axis's natural growth. The canvas Y
            // grows DOWNWARD, so TopToBottom (0 at the top) is the
            // natural one vertically and BottomToTop the inverted one.
            bool isReversed() const
            {
                return direction == UiScrollbarDirection::RightToLeft ||
                       direction == UiScrollbarDirection::BottomToTop;
            }

            float clampedFraction() const { return std::clamp(handleFraction, 0.0f, 1.0f); }

            // Snaps a value to the discrete stops and bounds it to [0,1].
            float snapValue(float v) const
            {
                const float c = std::clamp(v, 0.0f, 1.0f);
                if (numberOfSteps < 2u) return c;
                const float pasos = (float)(numberOfSteps - 1u);
                return std::round(c * pasos) / pasos;
            }

            // From a point IN RECT COORDINATES (px from its top-left
            // corner) to the value. rectSize comes in separately and is not read from `size`
            // because the input works in SCREEN pixels, which have the canvas
            // scale applied; the handle fraction is unitless.
            float valueFromLocal(glm::vec2 local, glm::vec2 rectSize) const
            {
                const float largo = isVertical() ? rectSize.y : rectSize.x;
                if (!(largo > 0.0f)) return 0.0f;

                const float frac = clampedFraction();
                const float util = largo * (1.0f - frac);
                const float p    = (isVertical() ? local.y : local.x) - largo * frac * 0.5f;

                // Handle as large as the channel: there is no travel and dividing would give
                // an infinity.
                float t = (util > 0.0f) ? (p / util) : 0.0f;
                t = std::clamp(t, 0.0f, 1.0f);
                return snapValue(isReversed() ? (1.0f - t) : t);
            }

            // Rect of the HANDLE in channel coordinates, already bounded so it does not
            // stick out at either end.
            void handleRect(glm::vec2& outPos, glm::vec2& outSize) const
            {
                const float frac = clampedFraction();
                const float t    = std::clamp(value, 0.0f, 1.0f);
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

            // Dumps the rect and the channel into the live node. Does NOT touch `atlas` (it is a
            // GPU pointer: the sync resolves it).
            void applyTo(Scrollbar& s) const
            {
                s.anchorMin = anchorMin;
                s.anchorMax = anchorMax;
                s.pivot     = pivot;
                s.position  = position;
                s.size      = size;
                s.color     = color;
                s.visible   = visible;
                s.sprite    = backgroundSprite;
                s.raycastTarget = true;
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

            // The sync uses it to know whether there is anything to dump.
            bool operator==(const ScrollbarComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       interactable == o.interactable &&
                       value == o.value && handleFraction == o.handleFraction &&
                       direction == o.direction && numberOfSteps == o.numberOfSteps &&
                       handleColor == o.handleColor && scrollStep == o.scrollStep &&
                       atlasPath == o.atlasPath && backgroundSprite == o.backgroundSprite &&
                       handleSprite == o.handleSprite;
            }
            bool operator!=(const ScrollbarComponent& o) const { return !(*this == o); }
    };

    // Name of a Scrollbar's live node inside the canvas. DIFFERENT prefix
    // from the others, for the usual reason.
    inline std::string uiScrollbarNodeName(uint64_t ownerId)
    {
        return "scr:" + std::to_string(ownerId);
    }

    // Inverse of uiScrollbarNodeName. Returns 0 if the name is not a bar's.
    // Cutting at '/' makes the handle node also return its owner.
    inline uint64_t uiScrollbarOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("scr:", 0) != 0) return 0;
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
