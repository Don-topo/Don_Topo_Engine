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
    class CheckboxComponent;

    // What a checkbox has LIVE and is not serialized. Same role and same
    // reasons as UiButtonRuntime (ButtonComponent.h) and UiSliderRuntime: the
    // canvas node is destroyed by clearChildren() on every rebuild,
    // so the owner of the callback is the COMPONENT and the node only keeps a
    // weak_ptr to this.
    //
    // `owner` is where the click writes `isOn` IN THE COMPONENT, which is what
    // gets serialized and what the editor reads. The sync sets it on every dump;
    // if the component dies, the runtime dies with it and the node's weak_ptr no
    // longer resolves, so the pointer never dangles.
    struct UiCheckboxRuntime
    {
        std::function<void(bool)> onValueChanged;
        CheckboxComponent*        owner = nullptr;
    };

    // The runtime slot inside the component, with the SAME two rules as the
    // Button's UiCallbackSlot: copying a component gets fresh callbacks and
    // comparing them ignores this field.
    struct UiCheckboxCallbackSlot
    {
        std::shared_ptr<UiCheckboxRuntime> ptr = std::make_shared<UiCheckboxRuntime>();

        UiCheckboxCallbackSlot() = default;
        UiCheckboxCallbackSlot(const UiCheckboxCallbackSlot&) {}
        UiCheckboxCallbackSlot& operator=(const UiCheckboxCallbackSlot&) { return *this; }
        UiCheckboxCallbackSlot(UiCheckboxCallbackSlot&&) = default;
        UiCheckboxCallbackSlot& operator=(UiCheckboxCallbackSlot&&) = default;

        bool operator==(const UiCheckboxCallbackSlot&) const { return true; }
    };

    // A 2D UI checkbox as a GameObject component, with
    // the SAME contract as the rest: DATA ONLY.
    //
    // The core Checkbox (UiWidgets.h) is a stub with NO fields, so the
    // widget is assembled by COMPOSITION: the box is the root node (of type
    // Checkbox) and the mark hangs from it. Not a single line of the UI core is
    // touched here.
    //
    // It carries no text label on purpose: Text is its own component and
    // fits in the same GameObject (they are sibling nodes with different prefixes),
    // so putting a copy of the text fields here would mean maintaining two.
    class CheckboxComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};    // px, relative to the anchor
            glm::vec2 size{24.0f, 24.0f};      // px
            glm::vec4 color{0.2f, 0.2f, 0.2f, 1.0f};   // color of the BOX
            bool      visible = true;

            // When false it is drawn the same but the click does not toggle it.
            bool interactable = true;

            // --- Value ---------------------------------------------------------
            bool isOn = false;

            // --- Mark -----------------------------------------------------------
            glm::vec4 checkColor{1.0f, 1.0f, 1.0f, 1.0f};

            // Pixels the mark is inset INTO the box on all four
            // sides. A padding that does not fit leaves the mark at zero, which is the worst
            // that can happen; never an inverted rect.
            float checkPadding = 4.0f;

            // --- Sprites --------------------------------------------------------
            // ONE atlas and two sub-rect NAMES inside it (registered by the
            // sidecar <atlas>.sprites.json). Empty = flat-color quads.
            std::string atlasPath;
            std::string backgroundSprite;
            std::string checkmarkSprite;

            // --- Runtime (not serialized) ---------------------------------------
            UiCheckboxCallbackSlot callbacks;

            // Rect of the MARK in box coordinates.
            void checkRect(glm::vec2& outPos, glm::vec2& outSize) const
            {
                const float p = std::max(checkPadding, 0.0f);
                outPos  = glm::vec2(std::min(p, size.x * 0.5f), std::min(p, size.y * 0.5f));
                outSize = glm::vec2(std::max(size.x - 2.0f * p, 0.0f),
                                    std::max(size.y - 2.0f * p, 0.0f));
            }

            // Dumps the rect and the box into the live node. Does NOT touch `atlas` (it is a
            // GPU pointer: the sync resolves it).
            void applyTo(Checkbox& c) const
            {
                c.anchorMin = anchorMin;
                c.anchorMax = anchorMax;
                c.pivot     = pivot;
                c.position  = position;
                c.size      = size;
                c.color     = color;
                c.visible   = visible;
                c.sprite    = backgroundSprite;
                // The box ALWAYS receives the mouse, interactable or not: the gate
                // is in the handler, and removing its raycast here would make whatever was
                // behind it swallow the click.
                c.raycastTarget = true;
            }

            void applyToCheck(UiElement& m) const
            {
                glm::vec2 pos{0.0f}, sz{0.0f};
                checkRect(pos, sz);

                m.anchorMin = glm::vec2(0.0f);
                m.anchorMax = glm::vec2(0.0f);
                m.pivot     = glm::vec2(0.0f);
                m.position  = pos;
                m.size      = sz;
                m.color     = checkColor;
                m.sprite    = checkmarkSprite;
                m.visible   = true;
                // When off, the node KEEPS EXISTING but does not paint: if it appeared
                // and disappeared it would change the shape of the subtree and the canvas root
                // would have to be rebuilt on every click.
                m.drawable  = isOn && sz.x > 0.0f && sz.y > 0.0f;
                // And it does not receive the mouse: the hit test returns the DEEPEST
                // node, so the mark would swallow the box's click.
                m.raycastTarget = false;
            }

            // The sync uses it to know whether there is anything to dump: without this the
            // node would have to be dirtied EVERY frame, which is exactly what the
            // canvas's vertex cache exists to avoid.
            bool operator==(const CheckboxComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       interactable == o.interactable && isOn == o.isOn &&
                       checkColor == o.checkColor && checkPadding == o.checkPadding &&
                       atlasPath == o.atlasPath && backgroundSprite == o.backgroundSprite &&
                       checkmarkSprite == o.checkmarkSprite;
            }
            bool operator!=(const CheckboxComponent& o) const { return !(*this == o); }
    };

    // Name of a Checkbox's live node inside the canvas. DIFFERENT prefix from
    // the others for the same reason those differ among themselves: a GameObject can
    // carry several UI components at once, and two sibling nodes with the
    // same name would make the gizmo and picking grab the wrong one.
    inline std::string uiCheckboxNodeName(uint64_t ownerId)
    {
        return "chk:" + std::to_string(ownerId);
    }

    // Inverse of uiCheckboxNodeName. Returns 0 if the name is not a
    // checkbox's: 0 is not a valid GameObject id. Cutting at '/' makes the
    // mark node also return its owner.
    inline uint64_t uiCheckboxOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("chk:", 0) != 0) return 0;
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
