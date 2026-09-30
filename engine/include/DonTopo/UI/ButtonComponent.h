#pragma once
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiFont.h"
#include "DonTopo/UI/UiTextureAtlas.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
    // A 2D UI button as a GameObject component, with the SAME contract
    // as CanvasComponent: DATA ONLY. It stores neither a UiElement nor the atlas nor
    // the font. The live tree is still held by the Renderer
    // (Renderer::uiCanvas()), and whoever draws rebuilds/updates it every frame
    // with syncUiWidgets(). This way what is seen in Play and in the exported game comes
    // from the SCENE and not from a hand-wired tree.
    //
    // The names, defaults and meaning are EXACTLY those of the core:
    //   - the rect block and the sprite block belong to UiElement (UiCanvas.h),
    //   - the states block belongs to Button (UiWidgets.h),
    //   - the text block belongs to Text (UiWidgets.h), because Button has NO
    //     text: the label is a Text CHILD that the sync assembles.
    // This component neither interprets nor clamps anything.
    //
    // The two paths (atlasPath, fontPath) are the ONLY thing that is not a core
    // field: the core stores pointers to GPU resources, which are not serialized.
    // The sync resolves them against the Renderer, not the component.
    // What a button has LIVE and is not serialized: the callbacks a script has
    // hooked onto it and the state resolved by the last updateInput.
    //
    // It lives here and not in the canvas node on purpose: the node is destroyed by
    // clearChildren() every time syncUiWidgets rebuilds the root (that is, when
    // adding or removing ANY widget in the scene), so a handler
    // hooked directly to the node would disappear without warning. The owner is the
    // component (which lives as long as the GameObject does) and the sync just
    // reinstalls on the node a handler that points here with a weak_ptr.
    //
    // `state` is the way back: the node writes it, the component
    // publishes it. Without this a script could not read the button's state, which only
    // exists in the live tree.
    struct UiButtonRuntime
    {
        std::function<void()> onClick;
        std::function<void()> onDoubleClick;
        UiButtonState         state = UiButtonState::Normal;
    };

    // The runtime slot inside the component. It is its own type and not a loose
    // shared_ptr because of the TWO rules it has to break relative to the default
    // copy:
    //   - copying a component (duplicating a GameObject, or the copy the sync
    //     keeps as a snapshot) does NOT share the callbacks: each copy gets
    //     its own, or the clone would fire the original's callback;
    //   - comparing two components IGNORES this field: the sync uses operator==
    //     to know whether to dump the node, and neither a callback nor the live state
    //     is data to dump.
    struct UiCallbackSlot
    {
        std::shared_ptr<UiButtonRuntime> ptr = std::make_shared<UiButtonRuntime>();

        UiCallbackSlot() = default;
        UiCallbackSlot(const UiCallbackSlot&) {}
        UiCallbackSlot& operator=(const UiCallbackSlot&) { return *this; }
        UiCallbackSlot(UiCallbackSlot&&) = default;
        UiCallbackSlot& operator=(UiCallbackSlot&&) = default;

        bool operator==(const UiCallbackSlot&) const { return true; }
    };

    class ButtonComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};   // px, relative to the anchor
            glm::vec2 size{160.0f, 40.0f};    // px
            glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
            bool      visible = true;

            // Atlas path (PNG) and name of the base sprite inside it. Empty
            // = flat-color button, which is what UiElement draws without an atlas.
            std::string atlasPath;
            std::string sprite;

            // --- States (Button) ----------------------------------------------
            bool               interactable = true;
            bool               selected     = false;
            UiButtonTransition transition   = UiButtonTransition::ColorTint;

            glm::vec4 normalColor{1.0f, 1.0f, 1.0f, 1.0f};
            glm::vec4 hoverColor{1.0f, 1.0f, 1.0f, 1.0f};
            glm::vec4 pressedColor{1.0f, 1.0f, 1.0f, 1.0f};
            glm::vec4 disabledColor{1.0f, 1.0f, 1.0f, 1.0f};
            glm::vec4 selectedColor{1.0f, 1.0f, 1.0f, 1.0f};

            // Names from the button's SAME atlas. An empty one leaves the sprite as
            // it is (that is Button's contract, not a decision made here).
            std::string normalSprite;
            std::string hoverSprite;
            std::string pressedSprite;
            std::string disabledSprite;
            std::string selectedSprite;

            float fadeDuration = 0.1f;   // seconds of the Animation fade

            // --- Label (Text child) -------------------------------------------
            std::string text;
            std::string fontPath;                        // TTF; empty = no text
            float       fontSize = 16.0f;
            glm::vec4   textColor{1.0f, 1.0f, 1.0f, 1.0f};
            UiTextAlign textAlign = UiTextAlign::Center;
            // Vertically it is CENTERED by default, unlike a standalone Text:
            // a label stuck to the top edge of the button is what made
            // "textAlign: Center" look like it centered nothing. It is still
            // selectable in case someone wants the label at the top or bottom.
            UiTextVAlign textVAlign = UiTextVAlign::Middle;

            // --- Live (NOT serialized) ----------------------------------------
            // Script callbacks and resolved state. Outside toJson/fromJson,
            // outside operator== and outside applyTo: it is not scene data.
            UiCallbackSlot callbacks;

            // Dumps the rect and the states into the live button. Does NOT touch either the atlas
            // (it is a GPU pointer: the sync resolves it) or the fields
            // the canvas itself writes (state, fadeFrom, fadeStartTime,
            // stateReady): overwriting them every frame would kill the fade.
            void applyTo(Button& b) const
            {
                b.anchorMin    = anchorMin;
                b.anchorMax    = anchorMax;
                b.pivot        = pivot;
                b.position     = position;
                b.size         = size;
                // The BASE tint, not the color that is painted: the canvas resolves that
                // by multiplying the state's color by this one. Writing it into
                // b.color was useless: the first updateInput overwrote it, and
                // that is why the editor's "Color" field did absolutely
                // nothing on a button.
                b.baseColor    = color;
                // Starting value for the frame before the first updateInput:
                // without it, a newly created button is painted white for one frame. It comes
                // from the COMPONENT's normalColor, which is the one that will be dumped
                // a few lines below; the node's is still the old one.
                b.color        = color * normalColor;
                b.visible      = visible;
                b.sprite       = sprite;

                b.interactable = interactable;
                b.selected     = selected;
                b.transition   = transition;

                b.normalColor   = normalColor;
                b.hoverColor    = hoverColor;
                b.pressedColor  = pressedColor;
                b.disabledColor = disabledColor;
                b.selectedColor = selectedColor;

                b.normalSprite   = normalSprite;
                b.hoverSprite    = hoverSprite;
                b.pressedSprite  = pressedSprite;
                b.disabledSprite = disabledSprite;
                b.selectedSprite = selectedSprite;

                b.fadeDuration = fadeDuration;
            }

            // The label takes the button's WHOLE rect (anchored to all four
            // corners, no margins): this way the text follows the button when its
            // size changes without a second set of fields to maintain.
            void applyToLabel(Text& t) const
            {
                t.anchorMin = glm::vec2(0.0f, 0.0f);
                t.anchorMax = glm::vec2(1.0f, 1.0f);
                t.pivot     = glm::vec2(0.0f, 0.0f);
                t.position  = glm::vec2(0.0f, 0.0f);
                t.text      = text;
                t.fontSize  = fontSize;
                t.color     = textColor;
                t.align     = textAlign;
                t.vAlign    = textVAlign;
                t.visible   = visible;

                // And it does NOT intercept the mouse. The hit test tries the children
                // before the parent, so a label covering the whole rect
                // would keep the hover and the button would never leave
                // Normal: the five state colors would do nothing. The
                // click would work (events bubble from child to
                // parent), which is what makes the bug so hard to see.
                t.raycastTarget = false;
            }

            // The sync uses it to know whether there is anything to dump: without this the
            // node would have to be dirtied EVERY frame, which is exactly what the
            // canvas's vertex cache exists to avoid.
            bool operator==(const ButtonComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       atlasPath == o.atlasPath && sprite == o.sprite &&
                       interactable == o.interactable && selected == o.selected &&
                       transition == o.transition &&
                       normalColor == o.normalColor && hoverColor == o.hoverColor &&
                       pressedColor == o.pressedColor && disabledColor == o.disabledColor &&
                       selectedColor == o.selectedColor &&
                       normalSprite == o.normalSprite && hoverSprite == o.hoverSprite &&
                       pressedSprite == o.pressedSprite && disabledSprite == o.disabledSprite &&
                       selectedSprite == o.selectedSprite &&
                       fadeDuration == o.fadeDuration &&
                       text == o.text && fontPath == o.fontPath && fontSize == o.fontSize &&
                       textColor == o.textColor && textAlign == o.textAlign &&
                       textVAlign == o.textVAlign;
            }
            bool operator!=(const ButtonComponent& o) const { return !(*this == o); }
    };

    // Font used when the button has text and NOBODY has set a path.
    // A text without a font is not drawn (the emitter falls back to the base quad), so
    // without this fallback typing in the Text field would not show until a TTF
    // was looked up by hand.
    //
    // It goes INSIDE the project and not to a system font on purpose: the exported
    // game takes the project's assets, not those of the machine that
    // exported, and a path like C:/Windows/Fonts/... leaves the text invisible on
    // any other PC. Relative to the working directory, like the rest
    // of the assets. The exporter packs it when any button has text without its
    // own font (GameExporter::collectSceneAssets).
    inline constexpr const char* kDefaultUiFontPath =
        "assets/DancingScript-VariableFont_wght.ttf";

    // Name of a button's live node inside the canvas. It is the ONLY way to
    // get back from the UI tree to the GameObject (the tree holds no pointers to the
    // scene), so the convention lives here and is not repeated in every caller:
    // the sync uses it to create the nodes and the editor for the gizmo and
    // picking. The label hangs as "<name>/Label".
    inline std::string uiButtonNodeName(uint64_t ownerId)
    {
        return "go:" + std::to_string(ownerId);
    }

    // Inverse of uiButtonNodeName, tolerant of the "/Label" suffix (the hit
    // test returns the deepest node, which can be the label). Returns
    // 0 if the name is not a button's: 0 is not a valid GameObject id.
    inline uint64_t uiButtonOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("go:", 0) != 0) return 0;
        uint64_t id = 0;
        for (size_t i = 3; i < nodeName.size(); i++)
        {
            const char c = nodeName[i];
            if (c == '/') break;
            if (c < '0' || c > '9') return 0;
            id = id * 10 + (uint64_t)(c - '0');
        }
        return id;
    }

    // The sync and its cache do NOT live here: the canvas root is rebuilt with
    // clearChildren(), so there is ONE single sync that owns all the widgets
    // (syncUiWidgets in TextComponent.h). Two syncs over the same root would
    // delete each other's nodes every time one of them rebuilt.
}
