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
    class ToggleComponent;

    // What a switch has LIVE and is not serialized. Same role and
    // same reasons as UiButtonRuntime and UiCheckboxRuntime.
    struct UiToggleRuntime
    {
        std::function<void(bool)> onValueChanged;
        ToggleComponent*          owner = nullptr;
    };

    struct UiToggleCallbackSlot
    {
        std::shared_ptr<UiToggleRuntime> ptr = std::make_shared<UiToggleRuntime>();

        UiToggleCallbackSlot() = default;
        UiToggleCallbackSlot(const UiToggleCallbackSlot&) {}
        UiToggleCallbackSlot& operator=(const UiToggleCallbackSlot&) { return *this; }
        UiToggleCallbackSlot(UiToggleCallbackSlot&&) = default;
        UiToggleCallbackSlot& operator=(UiToggleCallbackSlot&&) = default;

        bool operator==(const UiToggleCallbackSlot&) const { return true; }
    };

    // A sliding switch of the 2D UI as a GameObject component, with
    // the SAME contract as the rest: DATA ONLY. The core Toggle
    // (UiWidgets.h) is a stub with NO fields, so the widget is assembled by
    // COMPOSITION: the track is the root node (of type Toggle) and the knob hangs
    // from it.
    //
    // It stores the SAME datum as the Checkbox (a bool) and yet they are two
    // components and not one with a style enum: what changes is not the datum
    // but the FIELDS. The box has a checkmark padding and a checkmark color;
    // the switch has two track colors (on and off) and the
    // knob size. With a single component, half of the inspector fields
    // would do nothing depending on the chosen style.
    class ToggleComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};    // px, relative to the anchor
            glm::vec2 size{56.0f, 28.0f};      // px
            bool      visible = true;

            bool interactable = true;

            // --- Value ---------------------------------------------------------
            bool isOn = false;

            // --- Colors ---------------------------------------------------------
            // The track does NOT use UiElement::color as a field of its own: the
            // sync writes it with the state's color, just as the canvas does with the
            // Button. Having both would mean a field that the first dump
            // overwrites and that seems to do nothing.
            glm::vec4 offColor{0.3f, 0.3f, 0.3f, 1.0f};
            glm::vec4 onColor{0.25f, 0.7f, 1.0f, 1.0f};
            glm::vec4 knobColor{1.0f, 1.0f, 1.0f, 1.0f};

            // --- Knob -----------------------------------------------------------
            // Side of the knob in px. It is bounded to what fits between the paddings: one
            // bigger than the track would peek out over the edge with nothing
            // saying so.
            float knobSize    = 20.0f;
            float knobPadding = 4.0f;

            // --- Sprites --------------------------------------------------------
            std::string atlasPath;
            std::string backgroundSprite;
            std::string knobSprite;

            // --- Runtime (not serialized) ---------------------------------------
            UiToggleCallbackSlot callbacks;

            // The track color according to the state. Here and not in the sync so it can
            // be tested without canvas or GPU.
            glm::vec4 trackColor() const { return isOn ? onColor : offColor; }

            // Rect of the KNOB in track coordinates, stuck to one end or the
            // other and always inside the padding.
            void knobRect(glm::vec2& outPos, glm::vec2& outSize) const
            {
                const float p = std::max(knobPadding, 0.0f);
                // What is left of the track between the two paddings. A padding that does not
                // fit gives zero, never negative.
                const float dispW = std::max(size.x - 2.0f * p, 0.0f);
                const float dispH = std::max(size.y - 2.0f * p, 0.0f);

                const float lado = std::max(knobSize, 0.0f);
                const float w = std::min(lado, dispW);
                const float h = std::min(lado, dispH);

                const float x = isOn ? (p + dispW - w) : p;
                outPos  = glm::vec2(x, p);
                outSize = glm::vec2(w, h);
            }

            // Dumps the rect and the track into the live node. Does NOT touch `atlas` (it is a
            // GPU pointer: the sync resolves it).
            void applyTo(Toggle& t) const
            {
                t.anchorMin = anchorMin;
                t.anchorMax = anchorMax;
                t.pivot     = pivot;
                t.position  = position;
                t.size      = size;
                t.color     = trackColor();
                t.visible   = visible;
                t.sprite    = backgroundSprite;
                t.raycastTarget = true;
            }

            void applyToKnob(UiElement& k) const
            {
                glm::vec2 pos{0.0f}, sz{0.0f};
                knobRect(pos, sz);

                k.anchorMin = glm::vec2(0.0f);
                k.anchorMax = glm::vec2(0.0f);
                k.pivot     = glm::vec2(0.0f);
                k.position  = pos;
                k.size      = sz;
                k.color     = knobColor;
                k.sprite    = knobSprite;
                k.visible   = true;
                k.drawable  = (sz.x > 0.0f && sz.y > 0.0f);
                // The knob does not receive the mouse: the hit test returns the DEEPEST
                // node and it would swallow the track's click.
                k.raycastTarget = false;
            }

            // The sync uses it to know whether there is anything to dump.
            bool operator==(const ToggleComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       visible == o.visible && interactable == o.interactable &&
                       isOn == o.isOn &&
                       offColor == o.offColor && onColor == o.onColor &&
                       knobColor == o.knobColor &&
                       knobSize == o.knobSize && knobPadding == o.knobPadding &&
                       atlasPath == o.atlasPath && backgroundSprite == o.backgroundSprite &&
                       knobSprite == o.knobSprite;
            }
            bool operator!=(const ToggleComponent& o) const { return !(*this == o); }
    };

    // Name of a Toggle's live node inside the canvas. DIFFERENT prefix from
    // the others, for the usual reason.
    inline std::string uiToggleNodeName(uint64_t ownerId)
    {
        return "tgl:" + std::to_string(ownerId);
    }

    // Inverse of uiToggleNodeName. Returns 0 if the name is not a
    // switch's. Cutting at '/' makes the knob node also return
    // its owner.
    inline uint64_t uiToggleOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("tgl:", 0) != 0) return 0;
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
