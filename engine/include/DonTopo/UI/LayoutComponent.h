#pragma once
#include <cstdint>
#include <string>

#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
    // The 2D UI auto-layout as a GameObject component, with the SAME
    // contract as CanvasComponent, ButtonComponent, TextComponent and
    // ProgressBarComponent: DATA ONLY. The live tree is held by the Renderer
    // (Renderer::uiCanvas()) and syncUiWidgets() builds/updates it every frame.
    //
    // The solver is NOT here: it lives in UiElement (layoutMode, padding, spacing,
    // cellSize, columns, crossAlign, fitWidth/fitHeight, ignoreLayout) and is
    // solved by UiSpriteBatch::build. This component is the scene layer that
    // was missing to be able to use it: not a single new branch in the batcher.
    //
    // It plays the two roles that in Unity go in separate components:
    //   - CONTAINER (LayoutGroup): mode/padding/spacing/cellSize/columns/
    //     crossAlign/fitWidth/fitHeight PLACE the GameObject's children.
    //   - CHILD (LayoutElement): ignoreLayout takes THIS GameObject out of its
    //     parent's layout. They go together because they are two fields, not two systems: a
    //     separate component just for a bool would be one more entry in the Add
    //     menu with nothing to justify it.
    //
    // The rect (anchorMin/Max, pivot, position, size) is ONLY used when the
    // GameObject has no other UI component: there the sync assembles a
    // non-drawable container and this component is its owner. With a Button, a
    // ProgressBar or a Text on the same GameObject, THAT one rules the rect
    // (two owners of the same rect is a conflict with no winner) and only the layout
    // fields travel from here.
    class LayoutComponent
    {
        public:
            // --- Rect (only if the container belongs to this component) -------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};      // px, relative to the anchor
            glm::vec2 size{200.0f, 200.0f};      // px
            bool      visible = true;

            // --- Container -----------------------------------------------------
            // Vertical and not None by default: a newly added component that does
            // NOTHING looks like an editor bug. With None the container is still
            // a rect that groups and clips, which is also a valid use.
            UiLayoutMode mode = UiLayoutMode::Vertical;

            float paddingLeft   = 0.0f;
            float paddingRight  = 0.0f;
            float paddingTop    = 0.0f;
            float paddingBottom = 0.0f;

            glm::vec2 spacing{0.0f, 0.0f};     // .x between columns, .y between rows
            glm::vec2 cellSize{100.0f, 100.0f};  // solo Grid
            uint32_t  columns = 0;             // Grid only; 0 = as many as fit

            UiCrossAlign crossAlign = UiCrossAlign::Start;

            // Content size fitter: that axis of size becomes the extent of
            // the placed children plus the padding.
            bool fitWidth  = false;
            bool fitHeight = false;

            // --- As a child of ANOTHER's layout --------------------------------
            bool ignoreLayout = false;

            // --- Clipping ------------------------------------------------------
            // Clips the descendants against the container's rect. The
            // intersection with the inherited scissor is done by the batcher.
            bool clipChildren = false;

            // Dumps the layout fields into the live node. `ownsRect` false is
            // the case of sharing a node with another UI component: that one rules the
            // rect and not one of its four corners is touched here.
            void applyTo(UiElement& e, bool ownsRect) const
            {
                if (ownsRect)
                {
                    e.anchorMin = anchorMin;
                    e.anchorMax = anchorMax;
                    e.pivot     = pivot;
                    e.position  = position;
                    e.size      = size;
                    e.visible   = visible;
                    // A container groups and clips, but does not paint: without this a
                    // flat color quad would come out COVERING its children.
                    e.drawable  = false;
                    // And it does not receive the mouse either. The hit test does NOT look at drawable,
                    // only raycastTarget: without this, a group that only
                    // places would swallow the clicks of everything behind it
                    // (and since it paints nothing, there would be no way to see why).
                    // In exchange, in the editor the container is selected from
                    // the Hierarchy and not by clicking in the viewport.
                    e.raycastTarget = false;
                }

                e.layoutMode    = mode;
                e.paddingLeft   = paddingLeft;
                e.paddingRight  = paddingRight;
                e.paddingTop    = paddingTop;
                e.paddingBottom = paddingBottom;
                e.spacing       = spacing;
                e.cellSize      = cellSize;
                e.columns       = columns;
                e.crossAlign    = crossAlign;
                e.fitWidth      = fitWidth;
                e.fitHeight     = fitHeight;
                e.ignoreLayout  = ignoreLayout;
                e.clipChildren  = clipChildren;
            }

            // The sync uses it to know whether there is anything to dump: without this the
            // node would have to be dirtied EVERY frame, which is exactly what the
            // canvas's vertex cache exists to avoid.
            bool operator==(const LayoutComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       visible == o.visible &&
                       mode == o.mode &&
                       paddingLeft == o.paddingLeft && paddingRight == o.paddingRight &&
                       paddingTop == o.paddingTop && paddingBottom == o.paddingBottom &&
                       spacing == o.spacing && cellSize == o.cellSize && columns == o.columns &&
                       crossAlign == o.crossAlign &&
                       fitWidth == o.fitWidth && fitHeight == o.fitHeight &&
                       ignoreLayout == o.ignoreLayout && clipChildren == o.clipChildren;
            }
            bool operator!=(const LayoutComponent& o) const { return !(*this == o); }
    };

    // Name of the container's live node inside the canvas. DIFFERENT prefix from
    // the button's ("go:"), the text's ("txt:") and the bar's ("bar:") for the same reason as
    // those differ among themselves: a GameObject can carry several UI components at
    // once, and two nodes with the same name would make the gizmo and picking
    // grab the wrong one.
    inline std::string uiLayoutNodeName(uint64_t ownerId)
    {
        return "lay:" + std::to_string(ownerId);
    }

    // Inverse of uiLayoutNodeName. Returns 0 if the name is not a
    // container's: 0 is not a valid GameObject id.
    inline uint64_t uiLayoutOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("lay:", 0) != 0) return 0;
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
