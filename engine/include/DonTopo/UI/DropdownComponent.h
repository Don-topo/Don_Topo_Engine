#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
    class DropdownComponent;

    // What a dropdown has LIVE and is not serialized. Same role and
    // same reasons as UiButtonRuntime.
    struct UiDropdownRuntime
    {
        std::function<void(int)> onValueChanged;
        DropdownComponent*       owner = nullptr;
    };

    struct UiDropdownCallbackSlot
    {
        std::shared_ptr<UiDropdownRuntime> ptr = std::make_shared<UiDropdownRuntime>();

        UiDropdownCallbackSlot() = default;
        UiDropdownCallbackSlot(const UiDropdownCallbackSlot&) {}
        UiDropdownCallbackSlot& operator=(const UiDropdownCallbackSlot&) { return *this; }
        UiDropdownCallbackSlot(UiDropdownCallbackSlot&&) = default;
        UiDropdownCallbackSlot& operator=(UiDropdownCallbackSlot&&) = default;

        bool operator==(const UiDropdownCallbackSlot&) const { return true; }
    };

    // A 2D UI dropdown as a GameObject component, with the SAME
    // contract as the rest: DATA ONLY. The core Dropdown (UiWidgets.h) is
    // a stub with NO fields, so the widget is assembled by COMPOSITION: the box is
    // the root node (of type Dropdown) and the label, the arrow and
    // the list hang from it, with one row per option.
    //
    // It is the only one whose subtree CHANGES SHAPE with the data: one more option is
    // one more node, so the sync has to rebuild when the NUMBER
    // of options changes (not when their text changes). Opening and closing does NOT change the shape
    // (the list always exists and is only turned off), which is what avoids rebuilding
    // the whole canvas on every click.
    class DropdownComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};    // px, relative to the anchor
            glm::vec2 size{200.0f, 32.0f};     // px
            glm::vec4 color{0.2f, 0.2f, 0.2f, 1.0f};   // color of the BOX
            bool      visible = true;

            bool interactable = true;

            // --- Options --------------------------------------------------------
            std::vector<std::string> options;

            // Index of the selected one. No clamp HERE on purpose (the
            // component interprets nothing, same criterion as the rest): whoever
            // READS is the one that copes with an out-of-range index, and that is what
            // selectedLabel() is for.
            int value = 0;

            // LIVE state: if it were stored, a scene could open with the list
            // dropped down covering the menu. It goes into operator== (which is what
            // makes the sync dump again on opening) but NOT into the JSON.
            bool isOpen = false;

            // --- List -----------------------------------------------------------
            float    itemHeight      = 24.0f;
            // 0 = all. The height of the list is bounded to this so a combo of
            // fifty languages does not take up three screens.
            uint32_t maxVisibleItems = 6;

            glm::vec4 listColor{0.12f, 0.12f, 0.12f, 1.0f};
            glm::vec4 itemColor{0.18f, 0.18f, 0.18f, 1.0f};
            glm::vec4 itemSelectedColor{0.25f, 0.45f, 0.7f, 1.0f};
            glm::vec4 arrowColor{1.0f, 1.0f, 1.0f, 1.0f};

            // --- Text -----------------------------------------------------------
            std::string fontPath;   // TTF; empty = the default font
            float       fontSize = 16.0f;
            glm::vec4   textColor{1.0f, 1.0f, 1.0f, 1.0f};
            float       padding  = 6.0f;

            // --- Sprites --------------------------------------------------------
            std::string atlasPath;
            std::string backgroundSprite;
            std::string arrowSprite;
            std::string itemSprite;

            // --- Runtime (not serialized) ---------------------------------------
            UiDropdownCallbackSlot callbacks;

            // Text of the chosen option, or empty if the index does not point to
            // any. An out-of-range index is not a failure worth bailing out
            // of: a hand-edited scene can carry value 99 with two
            // options, and that must not blow up.
            std::string selectedLabel() const
            {
                if (value < 0 || value >= (int)options.size()) return std::string();
                return options[(size_t)value];
            }

            // Rows that are shown at once.
            int visibleItemCount() const
            {
                const int total = (int)options.size();
                if (maxVisibleItems == 0) return total;
                return std::min(total, (int)maxVisibleItems);
            }

            float listHeight() const { return itemHeight * (float)visibleItemCount(); }

            // Dumps the rect and the box into the live node. Does NOT touch `atlas` (it is a
            // GPU pointer: the sync resolves it).
            void applyTo(Dropdown& d) const
            {
                d.anchorMin = anchorMin;
                d.anchorMax = anchorMax;
                d.pivot     = pivot;
                d.position  = position;
                d.size      = size;
                d.color     = color;
                d.visible   = visible;
                d.sprite    = backgroundSprite;
                d.raycastTarget = true;
            }

            void applyToLabel(Text& t) const
            {
                t.anchorMin = glm::vec2(0.0f);
                t.anchorMax = glm::vec2(0.0f);
                t.pivot     = glm::vec2(0.0f);
                t.position  = glm::vec2(padding, 0.0f);
                // Leaves room for the arrow on the right.
                t.size      = glm::vec2(std::max(size.x - 2.0f * padding - size.y, 0.0f), size.y);
                t.text      = selectedLabel();
                t.fontSize  = fontSize;
                t.color     = textColor;
                t.vAlign    = UiTextVAlign::Middle;
                t.overflow  = UiTextOverflow::Ellipsis;
                t.visible   = true;
                t.raycastTarget = false;
            }

            // The arrow is a square stuck to the right edge. Without a sprite it is a
            // color quad: no triangle is drawn because the batcher emits
            // quads and a real arrow is art, not geometry.
            void applyToArrow(UiElement& a) const
            {
                const float lado = std::max(size.y * 0.4f, 0.0f);
                a.anchorMin = glm::vec2(0.0f);
                a.anchorMax = glm::vec2(0.0f);
                a.pivot     = glm::vec2(0.0f);
                a.position  = glm::vec2(std::max(size.x - padding - lado, 0.0f),
                                        (size.y - lado) * 0.5f);
                a.size      = glm::vec2(lado, lado);
                a.color     = arrowColor;
                a.sprite    = arrowSprite;
                a.visible   = true;
                a.drawable  = lado > 0.0f;
                a.raycastTarget = false;
            }

            // The list hangs RIGHT BELOW the box.
            void applyToList(UiElement& l) const
            {
                l.anchorMin = glm::vec2(0.0f);
                l.anchorMax = glm::vec2(0.0f);
                l.pivot     = glm::vec2(0.0f);
                l.position  = glm::vec2(0.0f, size.y);
                l.size      = glm::vec2(size.x, listHeight());
                l.color     = listColor;
                l.visible   = isOpen;
                l.drawable  = listHeight() > 0.0f;
                // Clips to the rows that do not fit in maxVisibleItems.
                l.clipChildren  = true;
                // The list itself does not receive the mouse: the rows do. This way a
                // click in the leftover gap selects nothing instead of selecting the
                // row that would be underneath.
                l.raycastTarget = false;
            }

            void applyToItem(UiElement& fila, Text& etiqueta, int index) const
            {
                fila.anchorMin = glm::vec2(0.0f);
                fila.anchorMax = glm::vec2(0.0f);
                fila.pivot     = glm::vec2(0.0f);
                fila.position  = glm::vec2(0.0f, itemHeight * (float)index);
                fila.size      = glm::vec2(size.x, itemHeight);
                fila.color     = (index == value) ? itemSelectedColor : itemColor;
                fila.sprite    = itemSprite;
                fila.visible   = true;
                fila.raycastTarget = true;

                etiqueta.anchorMin = glm::vec2(0.0f);
                etiqueta.anchorMax = glm::vec2(0.0f);
                etiqueta.pivot     = glm::vec2(0.0f);
                etiqueta.position  = glm::vec2(padding, 0.0f);
                etiqueta.size      = glm::vec2(std::max(size.x - 2.0f * padding, 0.0f), itemHeight);
                etiqueta.text      = (index >= 0 && index < (int)options.size())
                                         ? options[(size_t)index] : std::string();
                etiqueta.fontSize  = fontSize;
                etiqueta.color     = textColor;
                etiqueta.vAlign    = UiTextVAlign::Middle;
                etiqueta.overflow  = UiTextOverflow::Ellipsis;
                etiqueta.visible   = true;
                // The label does NOT receive the mouse or it would swallow the click of its
                // own row: the hit test returns the deepest node.
                etiqueta.raycastTarget = false;
            }

            // The sync uses it to know whether there is anything to dump.
            bool operator==(const DropdownComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       interactable == o.interactable &&
                       options == o.options && value == o.value && isOpen == o.isOpen &&
                       itemHeight == o.itemHeight && maxVisibleItems == o.maxVisibleItems &&
                       listColor == o.listColor && itemColor == o.itemColor &&
                       itemSelectedColor == o.itemSelectedColor && arrowColor == o.arrowColor &&
                       fontPath == o.fontPath && fontSize == o.fontSize &&
                       textColor == o.textColor && padding == o.padding &&
                       atlasPath == o.atlasPath && backgroundSprite == o.backgroundSprite &&
                       arrowSprite == o.arrowSprite && itemSprite == o.itemSprite;
            }
            bool operator!=(const DropdownComponent& o) const { return !(*this == o); }
    };

    // Name of a Dropdown's live node inside the canvas.
    inline std::string uiDropdownNodeName(uint64_t ownerId)
    {
        return "drp:" + std::to_string(ownerId);
    }

    // Inverse of uiDropdownNodeName. Returns 0 if the name is not a
    // dropdown's. Cutting at '/' makes the label, the arrow, the list and
    // its rows also return their owner.
    inline uint64_t uiDropdownOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("drp:", 0) != 0) return 0;
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
