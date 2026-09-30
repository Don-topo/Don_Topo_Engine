#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
    class ScrollViewComponent;

    // What a view has LIVE and is not serialized. Same role and same
    // reasons as UiButtonRuntime. The callback carries TWO floats (the normalized
    // position of both axes) and not a vec2 because Lua has no vec2: vectors
    // travel as several return values, just like the Get* of the
    // rest of the components.
    struct UiScrollViewRuntime
    {
        std::function<void(float, float)> onValueChanged;
        ScrollViewComponent*              owner = nullptr;
    };

    struct UiScrollViewCallbackSlot
    {
        std::shared_ptr<UiScrollViewRuntime> ptr = std::make_shared<UiScrollViewRuntime>();

        UiScrollViewCallbackSlot() = default;
        UiScrollViewCallbackSlot(const UiScrollViewCallbackSlot&) {}
        UiScrollViewCallbackSlot& operator=(const UiScrollViewCallbackSlot&) { return *this; }
        UiScrollViewCallbackSlot(UiScrollViewCallbackSlot&&) = default;
        UiScrollViewCallbackSlot& operator=(UiScrollViewCallbackSlot&&) = default;

        bool operator==(const UiScrollViewCallbackSlot&) const { return true; }
    };

    // A scrollable view of the 2D UI as a GameObject component, with the
    // SAME contract as the rest: DATA ONLY. The core ScrollView
    // (UiWidgets.h) is a stub with NO fields, so the widget is assembled by
    // COMPOSITION: the viewport is the root node (of type ScrollView, with
    // clipChildren) and the content hangs from it, which is what moves.
    //
    // It is the only one whose MAIN node is not the one that receives the mouse: the scene's
    // children hang from the CONTENT, not from the viewport. If they hung from the viewport,
    // scrolling would not drag them along and the scroll would be useless.
    //
    // It has NO reference to a Scrollbar. Linking them is one line of script
    // (`barra:OnValueChanged(...)`), and a reference between scene components
    // would force serializing the other GameObject's id and keeping it alive
    // in clone, undo and delete. Too much machinery for what it solves.
    class ScrollViewComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};      // px, relative to the anchor
            glm::vec2 size{200.0f, 200.0f};      // px: el VIEWPORT
            glm::vec4 color{0.1f, 0.1f, 0.1f, 1.0f};
            bool      visible = true;

            // --- Axes -----------------------------------------------------------
            // A turned-off axis does not move even if the content is bigger.
            bool horizontal = false;
            bool vertical   = true;

            // --- Content --------------------------------------------------------
            // Size of the scrollable area. It is a FIELD and deliberately not something measured from the
            // children: measuring the subtree every frame to decide how much
            // can be scrolled couples the scroll to the layout and makes the
            // travel change only when someone moves a child.
            glm::vec2 contentSize{200.0f, 400.0f};

            // 0 = start, 1 = end, per axis. No clamp HERE (the component interprets
            // nothing); whoever bounds it is the wheel and contentOffset().
            glm::vec2 normalizedPosition{0.0f, 0.0f};

            // Pixels the wheel moves per notch. In pixels and not as a fraction
            // because a list of 50 rows and one of 5 want the same travel
            // per notch, not the same fraction.
            float scrollSensitivity = 40.0f;

            // --- Sprites --------------------------------------------------------
            std::string atlasPath;
            std::string backgroundSprite;

            // --- Runtime (not serialized) ---------------------------------------
            UiScrollViewCallbackSlot callbacks;

            // How much can be scrolled per axis, in pixels. Content smaller
            // than the viewport gives 0: there is nothing to scroll.
            glm::vec2 scrollRange() const
            {
                return glm::vec2(horizontal ? std::max(contentSize.x - size.x, 0.0f) : 0.0f,
                                 vertical   ? std::max(contentSize.y - size.y, 0.0f) : 0.0f);
            }

            // Offset of the content inside the viewport, in viewport
            // coordinates. Always <= 0: pushing the content inward
            // would leave a gap at the top that nobody asked for.
            glm::vec2 contentOffset() const
            {
                const glm::vec2 r = scrollRange();
                return glm::vec2(-r.x * std::clamp(normalizedPosition.x, 0.0f, 1.0f),
                                 -r.y * std::clamp(normalizedPosition.y, 0.0f, 1.0f));
            }

            // Dumps the rect and the viewport into the live node. Does NOT touch `atlas` (it is a
            // GPU pointer: the sync resolves it).
            void applyTo(ScrollView& v) const
            {
                v.anchorMin = anchorMin;
                v.anchorMax = anchorMax;
                v.pivot     = pivot;
                v.position  = position;
                v.size      = size;
                v.color     = color;
                v.visible   = visible;
                v.sprite    = backgroundSprite;
                // Clips EVERYTHING that hangs from it: this is what makes the content
                // not stick out over the edges when scrolling.
                v.clipChildren  = true;
                // And it does receive the mouse: the wheel is its own.
                v.raycastTarget = true;
            }

            void applyToContent(UiElement& c) const
            {
                const glm::vec2 off = contentOffset();

                c.anchorMin = glm::vec2(0.0f);
                c.anchorMax = glm::vec2(0.0f);
                c.pivot     = glm::vec2(0.0f);
                c.position  = off;
                c.size      = contentSize;
                c.visible   = true;
                // It groups and moves, but does not paint: without this a flat
                // color quad would come out COVERING everything it contains.
                c.drawable  = false;
                // And it does not receive the mouse: the viewport wants the wheel, and a
                // container that does not paint while swallowing clicks would have no way
                // of being seen.
                c.raycastTarget = false;
            }

            // The sync uses it to know whether there is anything to dump.
            bool operator==(const ScrollViewComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       horizontal == o.horizontal && vertical == o.vertical &&
                       contentSize == o.contentSize &&
                       normalizedPosition == o.normalizedPosition &&
                       scrollSensitivity == o.scrollSensitivity &&
                       atlasPath == o.atlasPath && backgroundSprite == o.backgroundSprite;
            }
            bool operator!=(const ScrollViewComponent& o) const { return !(*this == o); }
    };

    // Name of a ScrollView's live node inside the canvas. "scv:" and not
    // "scr:", which is the Scrollbar's: they share the first three letters and are two
    // different widgets, so mixing them up would make picking return the
    // wrong GameObject.
    inline std::string uiScrollViewNodeName(uint64_t ownerId)
    {
        return "scv:" + std::to_string(ownerId);
    }

    // Inverse of uiScrollViewNodeName. Returns 0 if the name is not a
    // view's. Cutting at '/' makes the content also return its owner.
    inline uint64_t uiScrollViewOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("scv:", 0) != 0) return 0;
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
