#pragma once
#include <cstdint>
#include <string>

#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
    // A 2D UI Image as a GameObject component, with the SAME contract
    // as the rest: DATA ONLY. The live tree is held by the Renderer
    // (Renderer::uiCanvas()) and syncUiWidgets() builds/updates it every frame.
    //
    // Unlike Panel, the core Image (UiWidgets.h) DOES have state of
    // its own: the four modes of laying out the sprite inside the rect, the 9-slice
    // borders, the tile cap and the Filled block. All of them are resolved on the
    // CPU inside the batcher (N quads of the same atlas and the same scissor), so
    // there is no more work here than getting them to the node: the component
    // exposes the NINE fields, not one fewer.
    class ImageComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};      // px, relative to the anchor
            glm::vec2 size{100.0f, 100.0f};      // px
            glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};   // sprite tint
            bool      visible = true;

            // When false the image is still drawn but lets the mouse through.
            bool raycastTarget = true;

            // --- Sprite --------------------------------------------------------
            // Empty = flat-color quad. `sprite` is a NAME inside the
            // atlas (registered by the sidecar <atlas>.sprites.json); empty = the
            // whole image.
            std::string atlasPath;
            std::string sprite;

            // --- Modo ----------------------------------------------------------
            UiImageMode mode = UiImageMode::Normal;

            // --- Sliced --------------------------------------------------------
            // Borders in pixels OF THE SPRITE, not of the rect: scaling the element does not
            // move them. Without a center it yields 8 quads, which is what a frame
            // that shows what is behind it wants.
            float borderLeft   = 0.0f;
            float borderRight  = 0.0f;
            float borderTop    = 0.0f;
            float borderBottom = 0.0f;
            bool  fillCenter   = true;

            // --- Tiled ---------------------------------------------------------
            // Hard quad cap: past the cap the element is drawn as
            // Normal instead of blowing up the vertex buffer.
            uint32_t maxTiles = 1024;

            // --- Filled --------------------------------------------------------
            UiFillDirection fillDirection = UiFillDirection::Horizontal;
            UiFillOrigin    fillOrigin    = UiFillOrigin::Start;
            float           fillAmount    = 1.0f;   // 0..1; at 0 not a single quad is emitted

            // Dumps the rect and its own fields into the live node. Does NOT touch
            // `atlas` (it is a GPU pointer: the sync resolves it).
            void applyTo(Image& im) const
            {
                im.anchorMin     = anchorMin;
                im.anchorMax     = anchorMax;
                im.pivot         = pivot;
                im.position      = position;
                im.size          = size;
                im.color         = color;
                im.visible       = visible;
                im.raycastTarget = raycastTarget;
                im.sprite        = sprite;

                im.mode         = mode;
                im.borderLeft   = borderLeft;
                im.borderRight  = borderRight;
                im.borderTop    = borderTop;
                im.borderBottom = borderBottom;
                im.fillCenter   = fillCenter;
                im.maxTiles     = maxTiles;

                im.fillDirection = fillDirection;
                im.fillOrigin    = fillOrigin;
                im.fillAmount    = fillAmount;
            }

            // The sync uses it to know whether there is anything to dump: without this the
            // node would have to be dirtied EVERY frame, which is exactly what the
            // canvas's vertex cache exists to avoid.
            bool operator==(const ImageComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       raycastTarget == o.raycastTarget &&
                       atlasPath == o.atlasPath && sprite == o.sprite &&
                       mode == o.mode &&
                       borderLeft == o.borderLeft && borderRight == o.borderRight &&
                       borderTop == o.borderTop && borderBottom == o.borderBottom &&
                       fillCenter == o.fillCenter && maxTiles == o.maxTiles &&
                       fillDirection == o.fillDirection && fillOrigin == o.fillOrigin &&
                       fillAmount == o.fillAmount;
            }
            bool operator!=(const ImageComponent& o) const { return !(*this == o); }
    };

    // Name of an Image's live node inside the canvas. DIFFERENT prefix from that of
    // the others for the same reason those differ among themselves: a GameObject can carry
    // several UI components at once, and two sibling nodes with the same
    // name would make the gizmo and picking grab the wrong one.
    inline std::string uiImageNodeName(uint64_t ownerId)
    {
        return "img:" + std::to_string(ownerId);
    }

    // Inverse of uiImageNodeName. Returns 0 if the name is not an image's: 0
    // is not a valid GameObject id.
    inline uint64_t uiImageOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("img:", 0) != 0) return 0;
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
