#pragma once

// The concrete widget types. ALL of them are empty on purpose: this phase only
// fixes the hierarchy (that they exist, inherit and share UiElement's
// state), not the drawing or the behavior of any of them.
//
// A widget is created with parent.add<Button>("Aceptar") and for now it is drawn
// exactly like its base: a color quad, with atlas and sprite if it has
// them. The only thing that tells them apart today is typeName().
//
// There is no .cpp here and there will be no new fields until each widget has its phase:
// adding state before having drawing would be state that nobody reads.

#include "DonTopo/UI/UiCanvas.h"

#include <cstdint>
#include <string>

namespace DonTopo
{
    class UiFont;

    struct Panel : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "Panel"; }
    };

    // How the sprite is laid out inside the Image's rect. The four modes are
    // resolved on the CPU inside the batcher (N quads of the same atlas and the same
    // scissor): no shader, no pipeline, and not one more field in the vertex.
    enum class UiImageMode
    {
        Normal,   // one quad, the sprite stretched to the rect: exactly the usual
        Tiled,    // the sprite repeated at its NATIVE size, with the last row/column clipped by UV
        Sliced,   // 9-slice: the corners are not stretched, the edges only along their axis
        Filled    // only a fraction of the rect, clipping position AND UV at once
    };

    // Axis of the Filled mode's fill. Radial is left out on purpose: it asks for
    // fan geometry, and this is solved with quads.
    enum class UiFillDirection
    {
        Horizontal,
        Vertical
    };

    // From which end of the axis the fill grows. Start is left in
    // Horizontal and top in Vertical (the same +Y-downward convention of the
    // canvas).
    enum class UiFillOrigin
    {
        Start,
        End
    };

    struct Image : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "Image"; }
        const Image* asImage() const override { return this; }

        UiImageMode mode = UiImageMode::Normal;

        // --- Sliced ------------------------------------------------------------
        // Borders in pixels OF THE SPRITE, not of the rect: they are what defines where
        // the 9-slice cuts the texture, so scaling the element does not move them.
        float borderLeft   = 0.0f;
        float borderRight  = 0.0f;
        float borderTop    = 0.0f;
        float borderBottom = 0.0f;

        // Without a center it yields 8 quads: it is what a frame that shows what is
        // behind it wants.
        bool fillCenter = true;

        // --- Tiled -------------------------------------------------------------
        // Hard quad cap of the Image. A large rect with a 2 px sprite
        // would ask for tens of thousands of quads and blow up the buffer, so
        // past the cap the element is drawn as Normal.
        uint32_t maxTiles = 1024;

        // --- Filled ------------------------------------------------------------
        UiFillDirection fillDirection = UiFillDirection::Horizontal;
        UiFillOrigin    fillOrigin    = UiFillOrigin::Start;
        float           fillAmount    = 1.0f;   // 0..1; at 0 not a single quad is emitted
    };

    // The only one with state of its own for the moment: without fields there would be nothing to
    // draw. A single line, no wrap, no alignment and no rich text: that
    // is another phase.
    // Horizontal alignment of the text block INSIDE the width of the element's
    // rect. Justify distributes the leftover among the spaces of the line and
    // never touches the last one nor one ending in '\n': a lone line stretched to
    // full width looks like an error, not like justified text.
    enum class UiTextAlign
    {
        Left,
        Center,
        Right,
        Justify
    };

    // Where the BLOCK of lines falls inside the rect, which is the other half of
    // UiTextAlign: that one spreads each line across the width and this one the whole
    // block along the height. Default `Top`, which is what the emitter did before
    // this existed (baseline one ascent from the top edge), so
    // no text that is already placed moves.
    enum class UiTextVAlign
    {
        Top,
        Middle,
        Bottom
    };

    // What happens to what does not fit in the rect. Clipping is not free (it splits the
    // batch by scissor), so the default mode is to clip nothing.
    enum class UiTextOverflow
    {
        Overflow,   // drawn outside the rect
        Clip,       // scissor against its own rect
        Ellipsis    // the last line that fits ends in '…'
    };

    struct Text : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "Text"; }
        const Text* asText() const override { return this; }

        // Without a font the element goes back to being drawn as its base (color
        // quad): this way a half-configured Text does not disappear silently.
        const UiFont* font = nullptr;

        std::string text;   // UTF-8; decoded to codepoints when emitting

        // In SCREEN pixels. It does not have to match the size the atlas was
        // baked at: that is what MSDF is for.
        float fontSize = 16.0f;

        // The fill color is UiElement::color.

        // Thickness in screen pixels. At 0 there is no outline and the shader does not
        // even enter that branch.
        float     outlineWidth = 0.0f;
        glm::vec4 outlineColor{0.0f, 0.0f, 0.0f, 1.0f};

        // Offset in pixels. At {0,0} (or with alpha 0) not a single
        // shadow quad is emitted.
        glm::vec2 shadowOffset{0.0f, 0.0f};
        glm::vec4 shadowColor{0.0f, 0.0f, 0.0f, 0.5f};

        // ── Rich text, alignment, wrap and overflow ─────────────────────────
        // The text is ALWAYS parsed looking for tags: a malformed,
        // unknown or unclosed tag is drawn as literal text, so plain
        // text gives exactly the same as before this phase.
        //   <color=#RRGGBB> <color=#RRGGBBAA> <size=N> <b> <i> and their closings.
        // They nest on a stack: the closing restores the outer style.
        UiTextAlign    align    = UiTextAlign::Left;
        UiTextVAlign   vAlign   = UiTextVAlign::Top;
        UiTextOverflow overflow = UiTextOverflow::Overflow;

        // Breaks by words against the rect's width; a word that does not fit
        // even alone is split by glyph. The '\n' in the text always break, with
        // or without wrap.
        bool wordWrap = false;

        // <b> does NOT load a second font: it thickens the glyph through the SAME channel
        // the outline already uses, as a fraction of the span's size (so a
        // bold at 12 px and another at 48 px thicken the same in proportion).
        float boldStrength = 0.08f;

        // <i> neither: it is a shear of the quad along the baseline. It is the
        // tangent of the angle, so 0.25 is about 14 degrees.
        float italicSkew = 0.25f;
    };

    // The five states of a button. There is NO state machine: the state is
    // DERIVED on every updateInput from what the element already carries (hovered, left
    // button down on top, focused) plus interactable and selected, with a
    // FIXED priority: Disabled > Pressed > Selected > Hover > Normal.
    enum class UiButtonState
    {
        Normal,
        Hover,
        Pressed,
        Disabled,
        Selected
    };

    // How the state change looks. The button does NOT touch the batcher: it writes into
    // the fields that UiSpriteBatch already reads (color and sprite), so none of
    // the three transitions adds a quad, a batch or a pipeline.
    enum class UiButtonTransition
    {
        ColorTint,    // color = the state's, immediately
        SpriteSwap,   // sprite = the state's; same atlas, so same batch
        Animation     // color interpolated LINEARLY during fadeDuration
    };

    struct Button : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "Button"; }
        Button*       asButton()       override { return this; }
        const Button* asButton() const override { return this; }

        // When false the button still gets hit tested (so that Disabled is painted
        // when hovering over it) but does NOT emit Click or DoubleClick.
        bool interactable = true;

        // The game's own state: a button in a tab bar stays
        // marked with the mouse far away. It is added to focus: a focused focusable
        // also counts as Selected.
        bool selected = false;

        UiButtonTransition transition = UiButtonTransition::ColorTint;

        // BASE tint of the button, on which the state colors are multiplied.
        // UiElement::color is no use for this: the canvas rewrites it on every
        // updateInput with the state's color, so whatever the user put there
        // lasted until the first input frame and it looked like the field did
        // nothing. White = neutral, that is the usual behavior: the color that is seen is
        // exactly the state's.
        glm::vec4 baseColor{1.0f, 1.0f, 1.0f, 1.0f};

        // Colors per state. Fields, not hidden constants: each button has its
        // own. The Normal one is the one restored when going back to Normal, so
        // by default it is the same white as UiElement::color.
        glm::vec4 normalColor{1.0f, 1.0f, 1.0f, 1.0f};
        glm::vec4 hoverColor{1.0f, 1.0f, 1.0f, 1.0f};
        glm::vec4 pressedColor{1.0f, 1.0f, 1.0f, 1.0f};
        glm::vec4 disabledColor{1.0f, 1.0f, 1.0f, 1.0f};
        glm::vec4 selectedColor{1.0f, 1.0f, 1.0f, 1.0f};

        // Sprites per state, names FROM THE ELEMENT'S SAME atlas. An empty one
        // leaves the sprite as it is: a state without art does not erase the one that was there.
        std::string normalSprite;
        std::string hoverSprite;
        std::string pressedSprite;
        std::string disabledSprite;
        std::string selectedSprite;

        // Seconds of the Animation fade. The time COMES IN through
        // UiInputState::timeSeconds: there is no clock here, and that is why the fade is
        // reproducible in a test without GUI. At <= 0 the color jumps at once.
        float fadeDuration = 0.1f;

        // State resolved by the last updateInput. Read-only: the canvas
        // writes it, not the user.
        UiButtonState state = UiButtonState::Normal;

        // Fade internals: what color it started from and when. m_stateReady at
        // false = the button has not yet seen even one updateInput, and the first one
        // PLACES the state's color without fading (fading from the factory
        // color would be an animation nobody asked for).
        glm::vec4 fadeFrom{1.0f, 1.0f, 1.0f, 1.0f};
        float     fadeStartTime = 0.0f;
        bool      stateReady    = false;
    };

    struct Slider : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "Slider"; }
    };

    struct Checkbox : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "Checkbox"; }
    };

    struct Toggle : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "Toggle"; }
    };

    struct Scrollbar : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "Scrollbar"; }
    };

    struct InputField : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "InputField"; }
    };

    struct ProgressBar : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "ProgressBar"; }
    };

    struct Dropdown : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "Dropdown"; }
    };

    struct ScrollView : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "ScrollView"; }
    };
}
