#pragma once

// Game UI hierarchy, Unity style: Canvas -> Panel -> children.
//
// Everything in PIXELS and with the origin (0,0) at the top left, +X to the right
// and +Y downward. A node's position is relative to its parent's top-left
// corner, and the parent's scale accumulates in the child.
//
// UiElement is the base of ALL the widgets: the concrete types (Panel, Image,
// Button, ...) live in UiWidgets.h and for now add neither a field nor a
// behavior of their own, only their typeName(). A panel is an element without an atlas
// (flat color) and an image is the same element with atlas and sprite.
//
// This lives in DonTopoCore, not in the editor: the exported game draws the same
// canvas with the same code.

#include "DonTopo/UI/UiSpriteBatch.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace DonTopo
{
    struct Text;
    struct Image;
    struct Button;
    class  UiElement;

    // ── Events ──────────────────────────────────────────────────────────────
    // The whole input system is pure, DETERMINISTIC CPU: it does not query a clock or a
    // window. The time and the mouse state COME IN as parameters, so the
    // same sequence of UiInputState always gives the same sequence of events.

    enum class UiMouseButton : uint32_t
    {
        Left   = 0,
        Right  = 1,
        Middle = 2,
        Count  = 3
    };

    // Only the keys the UI needs to tell apart. Typed text does not
    // go through here: that is another phase.
    enum class UiKey : uint32_t
    {
        None = 0,
        Tab,
        Enter,
        Escape,
        Left,
        Right,
        Up,
        Down,

        // Text editing. They go at the end on purpose: the values of the ones
        // above do not move, so nothing already stored or mapped changes
        // meaning.
        Backspace,
        Delete,
        Home,
        End
    };

    // Filled in by the CALLER (GLFW, the editor, a test). The Core knows neither
    // GLFW nor ImGui: only numbers come in here.
    struct UiInputState
    {
        glm::vec2 mousePos{0.0f, 0.0f};       // px, same space as the canvas
        bool      mouseDown[3] = {false, false, false};   // HELD state per button
        float     scrollDelta  = 0.0f;        // + is up; 0 = there was no wheel

        // Keys pressed THIS frame (edge, not held): repeating a key
        // frame by frame is up to the caller, not the canvas.
        std::vector<UiKey> keys;

        // CHARACTERS typed this frame, in Unicode codepoints and in order.
        // It is a SEPARATE channel from `keys` and not one more key because they are not
        // the same thing: UiKey names physical keys with a meaning of their own (Tab,
        // Enter, arrows), and an 'a' is not a named key. It comes from the keyboard
        // layout, dead keys and the input method, which the core neither knows nor
        // has any reason to know. The caller fills it in (GLFW via
        // UiInputBridge, the editor, a test), just like the mouse position.
        std::vector<uint32_t> chars;

        bool shift = false;
        bool ctrl  = false;
        bool alt   = false;

        // Seconds. It is only compared with itself (double click), so the
        // origin does not matter as long as it is monotonic.
        float timeSeconds = 0.0f;
    };

    enum class UiEventType : uint32_t
    {
        MouseMove,
        MouseEnter,
        MouseExit,
        MouseDown,
        MouseUp,
        Click,
        DoubleClick,
        DragBegin,
        Drag,
        DragEnd,
        Drop,
        Scroll,
        Focus,
        Blur,
        KeyDown,
        // A typed character. Different from KeyDown for the same reason `chars` is
        // different from `keys`: what arrives here is text, not a key.
        TextInput
    };

    struct UiEvent
    {
        UiEventType type = UiEventType::MouseMove;

        // Where the event ORIGINATED. It does not change when bubbling: a parent that
        // receives a child's click still sees the child here.
        UiElement* target = nullptr;

        glm::vec2 mousePos{0.0f, 0.0f};
        glm::vec2 delta{0.0f, 0.0f};       // movement relative to the previous frame
        glm::vec2 dragStart{0.0f, 0.0f};   // where the dragging button went down

        UiMouseButton button = UiMouseButton::Left;
        float scrollDelta = 0.0f;

        UiKey key = UiKey::None;
        // Only TextInput fills it in: the character's Unicode codepoint.
        uint32_t codepoint = 0;
        bool  shift = false;
        bool  ctrl  = false;
        bool  alt   = false;

        float time = 0.0f;

        // Who started the drag. Only DragBegin/Drag/DragEnd/Drop fill it in,
        // and in a Drop it may NOT be the same as target.
        UiElement* dragSource = nullptr;

        // While it stays false the event keeps going up to the parent. Setting it to true
        // cuts the bubble right there.
        bool consumed = false;
    };

    using UiEventHandler = std::function<void(UiEvent&)>;

    // Auto-layout: with a mode other than None the container PLACES its
    // children and they stop anchoring on their own.
    enum class UiLayoutMode
    {
        None,
        Horizontal,
        Vertical,
        Grid
    };

    // Alignment on the axis TRANSVERSE to the layout's (the Y of a Horizontal and the
    // X of a Vertical). The Grid does not use it: the cell already fixes both.
    enum class UiCrossAlign
    {
        Start,
        Center,
        End
    };

    // ── Property animation ──────────────────────────────────────────────────
    // Property the element animates. ONE at a time: it is not a track
    // system, it is a field. Fade writes opacity, and there is NO separate "Opacity"
    // mode: opacity IS ANIMATED WITH Fade.
    enum class UiAnim
    {
        None,
        Fade,       // opacity      <- animFrom.x .. animTo.x
        Scale,      // scale        <- .xy
        Move,       // position     <- .xy
        Rotation,   // rotation     <- .x (radianes)
        Color       // color        <- all 4 channels
    };

    // All of them are PURE functions of t in [0,1] with EXACT f(0)=0 and f(1)=1.
    // Bounce and Elastic are not monotonic and Elastic overshoots 1 halfway:
    // that is what they do, not a failure that has to be clipped.
    enum class UiAnimCurve
    {
        Linear,
        EaseIn,
        EaseOut,
        Bounce,
        Elastic
    };

    enum class UiAnimLoop
    {
        Once,       // on reaching the end it stays at animTo and stops
        Loop,       // starts over at animFrom
        PingPong    // goes back the way it came
    };

    class UiElement
    {
    public:
        explicit UiElement(std::string nodeName = {}) : name(std::move(nodeName)) {}
        virtual ~UiElement() = default;

        // Type identity without RTTI or a parallel enum: each derived class returns
        // its literal and there is nothing else to keep in sync.
        virtual const char* typeName() const { return "UiElement"; }

        // Widgets that emit something that is NOT a single quad of their own. The batcher
        // asks them here, not with dynamic_cast: type identity in this
        // tree goes without RTTI, and a parallel enum would be another thing to maintain.
        virtual const Text* asText() const { return nullptr; }

        // An Image can emit N quads (Tiled, Sliced, Filled), all with the
        // SAME atlas and the SAME scissor, so none of them splits the batch.
        virtual const Image* asImage() const { return nullptr; }

        // The Button is NOT looked at by the batcher: it is looked at by updateInput, which needs to
        // WRITE into it (color, sprite, state), hence the non-const version.
        virtual Button*       asButton()       { return nullptr; }
        virtual const Button* asButton() const { return nullptr; }

        std::string name;

        glm::vec2 position{0.0f, 0.0f};   // px, relative to the anchor inside the parent
        glm::vec2 size{0.0f, 0.0f};       // px, before the inherited scale
        glm::vec2 scale{1.0f, 1.0f};
        glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};

        // Normalized 0..1 over the PARENT's rect. EQUAL on an axis = anchor
        // point: position counts from there and pivot is the point OF THIS
        // element that lands on top ({0,0} and {0,0} = top-left corner
        // against top-left corner). DIFFERENT on an axis = STRETCHED on
        // that axis: the margins rule and size and pivot of that axis are ignored.
        glm::vec2 anchorMin{0.0f, 0.0f};
        glm::vec2 anchorMax{0.0f, 0.0f};
        glm::vec2 pivot{0.0f, 0.0f};

        // Pixels INWARD from the parent's corresponding edge. Only
        // the stretched axis reads them; on an axis anchored to a point they do nothing.
        float marginLeft   = 0.0f;
        float marginRight  = 0.0f;
        float marginTop    = 0.0f;
        float marginBottom = 0.0f;

        // ── Auto-layout ─────────────────────────────────────────────────────
        // With mode != None this element places its children and for them
        // anchorMin/Max, margins and position are ignored. Horizontal and Vertical
        // respect the child's size (by its scale); Grid forces it to cellSize.
        UiLayoutMode layoutMode = UiLayoutMode::None;

        float paddingLeft   = 0.0f;
        float paddingRight  = 0.0f;
        float paddingTop    = 0.0f;
        float paddingBottom = 0.0f;

        glm::vec2 spacing{0.0f, 0.0f};    // gap between cells: .x between columns, .y between rows
        glm::vec2 cellSize{0.0f, 0.0f};   // solo Grid
        uint32_t  columns = 0;            // Grid only; 0 = as many as fit in the width

        UiCrossAlign crossAlign = UiCrossAlign::Start;

        // This child does NOT take a slot in the parent's layout: it anchors on its
        // own, as if the parent had no layout.
        bool ignoreLayout = false;

        // Content size fitter: that axis of size becomes the extent of the
        // PLACED children plus the padding. Without layoutMode there is no placement, so
        // without it they do nothing.
        bool fitWidth  = false;
        bool fitHeight = false;

        // Radians, clockwise on screen (+Y goes down). It rotates
        // the 4 corners of what THIS element emits around its pivot;
        // it is not inherited by descendants.
        // The clipChildren scissor is still the UNROTATED AABB: the
        // mask of a rotated element clips by its upright rectangle, which
        // is the only thing a VkRect2D can express.
        // At 0.0f not a single coordinate is touched: the vertices come out bit for bit
        // as they did before the rotation was applied.
        float rotation = 0.0f;

        // It is multiplied by the parent's all through the tree and ends up in the alpha
        // of the vertex color.
        float opacity = 1.0f;

        bool visible  = true;
        // Input only: it does NOT affect drawing. A disabled element
        // (and its subtree) is left out of the focus and navigation traversal.
        bool enabled  = true;
        // An element can be just a container (groups and clips) without painting.
        bool drawable = true;

        const UiTextureAtlas* atlas = nullptr;
        std::string sprite;

        // Clips THIS ELEMENT and its descendants against its own rect.
        // The resulting scissor is the INTERSECTION with the one that already came from the
        // parent, never a replacement.
        bool clipChildren = false;

        // ── Rectangular mask ────────────────────────────────────────────────
        // All of this MODULATES clipChildren and nothing else: without clipChildren none
        // of these fields does anything at all, so a tree that does not
        // touch them comes out with the same vertices and the same batches as always.

        // Screen pixels that the clip rect is inset INWARD from the
        // element's rect, each on its own side. Insets that cross give an EMPTY
        // mask (width/height at 0), never a negative one.
        float maskInsetLeft   = 0.0f;
        float maskInsetRight  = 0.0f;
        float maskInsetTop    = 0.0f;
        float maskInsetBottom = 0.0f;

        // When false the element is drawn WHOLE with the scissor it inherited and the
        // mask only clips its descendants: it is what leaves a window's frame
        // outside its own mask. When true (default) the behavior is the
        // usual: it is clipped too.
        bool maskSelf = true;

        // Turns the mask off without taking the element out of the tree: with false the node
        // behaves as if it had no clipChildren.
        bool maskEnabled = true;

        // ── Animation ───────────────────────────────────────────────────────
        // All CPU and deterministic. What moves this is UiCanvas::updateInput
        // and NOBODY else: a canvas that does not call updateInput animates not one
        // pixel, and with anim at None these fields write nothing, so a
        // tree that does not touch them comes out with the same vertices as always.
        UiAnim      anim      = UiAnim::None;
        UiAnimCurve animCurve = UiAnimCurve::Linear;

        // One vec4 for the five properties: it covers the 4 channels of Color and has
        // components to spare for the vec2 (Scale, Move) and for the float
        // (Fade, Rotation). What the mode does not read is not looked at.
        glm::vec4 animFrom{0.0f, 0.0f, 0.0f, 0.0f};
        glm::vec4 animTo  {0.0f, 0.0f, 0.0f, 0.0f};

        float      animDuration = 1.0f;             // seconds; <= 0 does not advance
        UiAnimLoop animLoop     = UiAnimLoop::Once;
        bool       animPlaying  = false;            // when false it FREEZES: neither advanced nor written

        // Seconds already elapsed. updateInput carries it by adding the DELTA between
        // frames of UiInputState::timeSeconds; no outside dt is accumulated,
        // and that is why asking for the same instant twice gives the same result.
        float animTime = 0.0f;

        // ── Input ───────────────────────────────────────────────────────────
        // None of this affects drawing: whoever does not call UiCanvas::updateInput
        // sees not a single change in the vertices or in the batches.

        // When false the element is INVISIBLE TO THE MOUSE, but its children are not: the
        // hit test keeps testing them (it is a container that lets things through).
        bool raycastTarget = true;

        // It can take focus with a Down on top of it and enters the Tab traversal.
        bool focusable = false;

        // STATE, not an event: updateInput maintains it and MouseEnter and MouseExit are
        // DERIVED from it by comparing this frame's hit with the previous one's.
        bool hovered = false;
        bool focused = false;

        // Directional navigation overrides (gamepad). Null by default:
        // geometry rules. When set they RULE over it, even if they point to the
        // opposite side. The target has to be focusable or the focus does not move.
        UiElement* navUp    = nullptr;
        UiElement* navDown  = nullptr;
        UiElement* navLeft  = nullptr;
        UiElement* navRight = nullptr;

        // Handlers. ALL null by default: a canvas without handlers does nothing
        // on receiving input, it only moves its internal state.
        UiEventHandler onMouseMove;
        UiEventHandler onMouseEnter;
        UiEventHandler onMouseExit;
        UiEventHandler onMouseDown;
        UiEventHandler onMouseUp;
        UiEventHandler onClick;
        UiEventHandler onDoubleClick;
        UiEventHandler onDragBegin;
        UiEventHandler onDrag;
        UiEventHandler onDragEnd;
        UiEventHandler onDrop;
        UiEventHandler onScroll;
        UiEventHandler onFocus;
        UiEventHandler onBlur;
        UiEventHandler onKeyDown;
        UiEventHandler onTextInput;

        // ── Rect resolved by the last buildDrawData ─────────────────────────
        // The layout already computes these three values per node; the input REUSES them
        // instead of measuring the tree again (measuring it twice with two different
        // pieces of code is the way to make them diverge). rectValid stays false on
        // the nodes the emitter never got to visit: invisible and clipped to zero.
        mutable glm::vec2 screenPos{0.0f, 0.0f};
        mutable glm::vec2 screenSize{0.0f, 0.0f};
        mutable UiScissor screenScissor{};
        mutable bool      rectValid = false;

        // ── Dirty flags ─────────────────────────────────────────────────────
        // What has changed in this node since the last buildDrawData. A node
        // without a single bit on is NOT emitted again: the vertices left by the
        // previous time are copied as they are. EVERYTHING is born dirty, so a freshly
        // assembled tree is emitted whole just like always.
        //
        //   Transform  position, scale, inherited rotation and OPACITY: everything
        //              that also moves or dims the descendants.
        //   Layout     anchors, margins, padding, spacing, layout mode,
        //              size, fitters: what repositions the subtree.
        //   Material   color, sprite, atlas: only what this node paints changes.
        //   Vertex     the geometry of the node's own quad (own rotation, sliced
        //              insets, text): it does not leave the node either.
        //
        // Transform and Layout GO UP the parent chain (a child that grows
        // can change the parent's size with a fitter) and GO DOWN to all
        // the descendants (they move their rects). Material and Vertex stay where
        // they are. That asymmetry is exactly what makes moving a leaf NOT
        // re-emit its siblings.
        enum : uint32_t
        {
            DirtyTransform = 1u << 0,
            DirtyLayout    = 1u << 1,
            DirtyMaterial  = 1u << 2,
            DirtyVertex    = 1u << 3,
            DirtyAll       = DirtyTransform | DirtyLayout | DirtyMaterial | DirtyVertex
        };

        mutable uint32_t dirty = DirtyAll;

        // The ONLY way to dirty. The fields are still public and are touched
        // bare: whoever touches them calls this right after, with what they touched
        // and nothing more. It is const because the emitter and the input work on
        // const references and the dirty state is not observable state of the tree.
        void markDirty(uint32_t flags) const
        {
            dirty |= flags;

            const uint32_t prop = flags & (DirtyTransform | DirtyLayout);
            if (prop == 0) return;

            // Upward: only the parent chain, without going back down through them (if it
            // went down, moving a leaf would dirty all its siblings).
            for (const UiElement* p = m_parent; p != nullptr; p = p->m_parent)
                p->dirty |= prop;

            markSubtreeDirty(prop);
        }

        // ── Emission cache ──────────────────────────────────────────────────
        // What this node emitted in the last build, split into the same
        // spans (atlas + scissor) with which the batches were cut. The
        // indices are RELATIVE to the node's first vertex: when relocating them the
        // destination base is added and the same uint16 as always come out.
        struct CacheSegment
        {
            const UiTextureAtlas* atlas = nullptr;
            UiScissor scissor{};
            uint32_t  vertexCount = 0;
            uint32_t  indexCount  = 0;
        };

        mutable std::vector<UiVertex>     cacheVertices;
        mutable std::vector<uint16_t>     cacheIndices;
        mutable std::vector<CacheSegment> cacheSegments;
        mutable bool                      cacheValid = false;

        // Resolved placement of the node. It is reused when neither Transform nor
        // Layout is dirty: changing ONLY the color does not recompute even one
        // rect. It is in world units (what the children see) and in pixels
        // (what the emitter sees).
        mutable glm::vec2 cacheWorldPos{0.0f, 0.0f};
        mutable glm::vec2 cacheWorldSize{0.0f, 0.0f};
        mutable glm::vec2 cacheWorldScale{1.0f, 1.0f};
        mutable glm::vec2 cacheChildArea{0.0f, 0.0f};
        mutable glm::vec2 cacheScreenPos{0.0f, 0.0f};
        mutable glm::vec2 cacheScreenSize{0.0f, 0.0f};
        mutable UiScissor cacheSelfScissor{};
        mutable UiScissor cacheChildScissor{};
        mutable float     cacheOpacity   = 1.0f;
        mutable bool      cacheSelfCulled = false;   // own mask empty
        mutable bool      cacheGeomValid  = false;

        // How many times this node has RE-EMITTED. Only for measuring: nobody reads it
        // inside the engine. It goes up in the build that rebuilds its vertices and not
        // in the one that copies them from the cache.
        mutable uint32_t rebuildCount = 0;

        UiElement*       parent()       { return m_parent; }
        const UiElement* parent() const { return m_parent; }

        // The only way to create children: the tree owns them and returns the concrete
        // type, not the base.
        template <class T = UiElement>
        T& add(std::string childName = {})
        {
            static_assert(std::is_base_of<UiElement, T>::value,
                          "A canvas child must derive from UiElement");
            m_children.push_back(std::make_unique<T>(std::move(childName)));
            // The parent is wired HERE and nowhere else: it is what allows an
            // event to bubble without the canvas keeping a separate map.
            m_children.back()->m_parent = this;
            // An extra child changes the parent's size and distribution: without this
            // the layout would keep the previous tree's.
            markDirty(DirtyLayout);
            return static_cast<T&>(*m_children.back());
        }

        void clearChildren()
        {
            m_children.clear();
            markDirty(DirtyLayout);
        }

        const std::vector<std::unique_ptr<UiElement>>& children() const { return m_children; }

    private:
        void markSubtreeDirty(uint32_t flags) const
        {
            for (const auto& child : m_children)
            {
                child->dirty |= flags;
                child->markSubtreeDirty(flags);
            }
        }

        std::vector<std::unique_ptr<UiElement>> m_children;
        UiElement* m_parent = nullptr;   // nullptr only at the canvas root
    };

    // Focus navigation directions. Next/Previous walk the tree;
    // the other four are resolved by geometry.
    enum class UiNavDir : uint32_t
    {
        Next,
        Previous,
        Left,
        Right,
        Up,
        Down
    };

    // ── Canvas resolution ───────────────────────────────────────────────────
    // How the tree (which is ALWAYS resolved in reference units) is converted
    // to the render's pixels. All CPU and deterministic: it is equally valid
    // in the editor in Play and in the exported game.
    enum class UiScaleMode : uint32_t
    {
        ConstantPixelSize,     // 1 unit = 1 pixel (times scaleFactor)
        ScaleWithScreenSize,   // the scale comes from the usable area against the reference
        ConstantPhysicalSize   // the scale comes from the DPI
    };

    // How the X ratio and the Y ratio are combined in ScaleWithScreenSize.
    enum class UiScreenMatch : uint32_t
    {
        MatchWidthOrHeight,   // logarithmic lerp between the two, by matchWidthOrHeight
        Expand,               // the SMALLER: nothing sticks out, margins may be left over
        Shrink                // the LARGER: nothing is left over, it may stick out
    };

    // Safe area insets, in REAL render PIXELS (not in reference
    // units): whoever fills them in receives them that way from the OS.
    struct UiSafeArea
    {
        float left   = 0.0f;
        float top    = 0.0f;
        float right  = 0.0f;
        float bottom = 0.0f;
    };

    class UiCanvas
    {
    public:
        UiCanvas();

        UiElement&       root()       { return m_root; }
        const UiElement& root() const { return m_root; }

        bool visible() const { return m_visible; }
        void setVisible(bool v) { m_visible = v; }

        // Empties the hierarchy. The canvas costs zero again.
        void clear();

        // width/height are the render's size in pixels: they fix the root scissor
        // and the orthographic.
        void buildDrawData(uint32_t width, uint32_t height, UiDrawData& out) const;

        // ── Resolution ──────────────────────────────────────────────────────
        // The usable area ALWAYS comes out in this order: (a) the whole render, (b) the
        // safe area insets are subtracted, (c) if aspectRatio > 0 it is clipped
        // CENTERED to that ratio. From that comes a SINGLE, UNIFORM scale, and
        // that scale enters only once, when each node's rect goes from
        // reference units to pixels. The root scissor is the usable area:
        // what falls in the bars or in the inset is NOT drawn.
        UiScaleMode   scaleMode           = UiScaleMode::ConstantPixelSize;
        float         scaleFactor         = 1.0f;               // multiplies all three modes
        glm::vec2     referenceResolution{1920.0f, 1080.0f};    // ScaleWithScreenSize
        UiScreenMatch screenMatch         = UiScreenMatch::MatchWidthOrHeight;
        float         matchWidthOrHeight  = 0.5f;               // 0 = width, 1 = height (clamped)
        float         screenDpi           = 0.0f;               // 0 = unknown
        float         fallbackDpi         = 96.0f;              // the one used if it is not known
        float         referenceDpi        = 96.0f;              // ConstantPhysicalSize
        UiSafeArea    safeArea{};                               // in real pixels
        float         aspectRatio         = 0.0f;               // 0 = off (16/9 = 1.777…)

        // What the last buildDrawData left. Read-only: so it can be tested
        // and so the editor can show it some day.
        float     uiScale()       const { return m_uiScale; }
        glm::vec2 uiOrigin()      const { return m_uiOrigin; }
        glm::vec2 referenceSize() const { return m_referenceSize; }

        // How many nodes RE-EMITTED their vertices in the last buildDrawData. The
        // rest were copied from their cache. It is the only honest measure of what is
        // being saved: it does not depend on the clock or the machine.
        uint32_t rebuiltNodes() const { return m_rebuiltNodes; }

        // The clock of the last updateInput. It is needed by whoever animates OUTSIDE the
        // canvas and has to keep in step with what is animated inside: the blinking
        // of a text field's caret is the first case. Having a clock of its
        // own there would be one that drifts from this one as soon as the two advance
        // by different paths.
        float lastTimeSeconds() const { return m_lastTime; }

        // ── Input ───────────────────────────────────────────────────────────
        // The ONLY entry point. It goes AFTER the layout, that is after a
        // buildDrawData: it reuses the rects that one left in each element and does not
        // measure anything again. Without a previous buildDrawData there are no rects and the hit
        // test finds nobody (it is not a failure: it is an unplaced canvas).
        void updateInput(const UiInputState& input);

        // Thresholds, here and not hidden in constants of the .cpp: a game with the
        // mouse and another with a gamepad do not want the same numbers.
        float doubleClickTime     = 0.35f;   // s between the two clicks
        float doubleClickDistance = 8.0f;    // px between the two clicks
        float dragThreshold       = 5.0f;    // px from the Down for it to be a drag

        // Element under the cursor in the last updateInput.
        UiElement* hovered() const { return m_hovered; }

        // Is there a mouse button DOWN on an element of this canvas and not
        // released yet? It is the pointer capture: while it lasts, the cursor
        // can leave the widget (even over ANOTHER canvas) without the
        // drag being cut, which is how a slider has always behaved.
        // dispatchUiInput reads it to distribute the mouse among several canvases.
        bool pointerCaptured() const
        {
            return m_pressTarget[0] != nullptr || m_pressTarget[1] != nullptr ||
                   m_pressTarget[2] != nullptr;
        }

        // Releases this canvas's hover, pointer capture and focus, with
        // their MouseExit and Blur, just as if the mouse had gone outside and
        // the focus elsewhere. It does NOT touch the tree or the animations.
        //
        // It is called by whoever takes the canvas out of the input distribution in the middle of a press:
        // today, changing its `renderMode` to World (it is writable from Lua). A
        // canvas that leaves with a button down never sees the MouseUp and keeps
        // `m_pressTarget`: when coming back it would enter with pointerCaptured() true WITHOUT
        // any button down, would take the mouse in step 1 of
        // dispatchUiInput and would emit a MouseUp/Click that nobody asked for. And without
        // releasing the hover it would also stay STUCK on the last one it saw, which is
        // the same failure dispatchUiInput avoids for those that lose the
        // pointer; here it cannot avoid it because the canvas is no longer in its
        // list.
        void releaseInput();

        // Only one per canvas. setFocus emits Blur on the old one and Focus on the
        // new one, IN THAT ORDER. It ignores those that are not focusable (nullptr is fine:
        // it means releasing the focus).
        UiElement* focused() const { return m_focused; }
        void       setFocus(UiElement* element);

        // Moves the focus. Returns whether it CHANGED. Neither the keyboard nor the
        // gamepad come in here: whoever reads them calls this. Pure, deterministic CPU, so it
        // is equally valid in the editor in Play and in the exported game.
        //
        // Next/Previous walk the SAME order as Tab (tree pre-order,
        // skipping what is not focusable, invisible or disabled) and WRAP
        // AROUND. Left/Right/Up/Down come from the rects left by the last
        // buildDrawData: without a previous buildDrawData there is no geometry and the
        // directional one finds nobody (just like the hit test), while
        // Next/Previous keep working. The directional one does NOT wrap around.
        //
        // Without a previous focus, any direction enters through the first focusable in
        // pre-order. Without a candidate the focus does not move and it returns false.
        bool navigate(UiNavDir dir);

        // ── Keyboard and gamepad navigation ─────────────────────────────────
        // With this on, a key that NOBODY has consumed moves the focus
        // (arrows) or activates the focused element (Enter), just as Tab and
        // Escape already did. It is turned off for a game that wants to read the arrows
        // on its own without the canvas getting ahead of it.
        bool keyboardNavigation = true;

        // Fires the focused element's Click, as if it had been pressed
        // with the mouse on top. It is what allows playing with a GAMEPAD: without this the
        // focus could be moved but nothing could be pressed.
        //
        // It returns whether it got to be emitted. It does not without focus, nor on an
        // invisible or disabled element, nor on a button that is not
        // interactable: the same rules applied to the mouse.
        bool submitFocused();

        // Which element falls under a point, with the same rules the input uses:
        // REVERSE pre-order (the last drawn wins), respecting visible, the
        // inherited scissor and raycastTarget.
        UiElement* hitTest(const glm::vec2& point) const;

    private:
        // The only one that resolves the scale is the emitter, and buildDrawData is
        // const: that is why the three results are mutable and it is a friend.
        friend class UiSpriteBatch;

        void dispatch(UiElement* target, UiEvent& event, UiEventHandler UiElement::* slot) const;
        void moveFocus(int direction);

        UiElement m_root{"Canvas"};
        bool   m_visible = true;

        // Result of the last buildDrawData. Neutral while there has been
        // none: scale 1, no offset and no area.
        mutable float     m_uiScale = 1.0f;
        mutable glm::vec2 m_uiOrigin{0.0f, 0.0f};
        mutable glm::vec2 m_referenceSize{0.0f, 0.0f};

        // Resolution of the previous build. If any of these changes, the
        // placement of ALL the nodes changes and the whole cache is useless.
        mutable uint32_t m_lastWidth  = 0;
        mutable uint32_t m_lastHeight = 0;

        mutable uint32_t m_rebuiltNodes = 0;

        // Input state between frames. Every pointer here points INSIDE
        // m_root, so clear() has to release them.
        UiElement* m_hovered = nullptr;
        UiElement* m_focused = nullptr;

        bool       m_buttonDown[3]  = {false, false, false};
        UiElement* m_pressTarget[3] = {nullptr, nullptr, nullptr};
        glm::vec2  m_pressPos[3]    = {};
        bool       m_dragging[3]    = {false, false, false};

        glm::vec2  m_lastMousePos{0.0f, 0.0f};
        bool       m_hasLastMouse = false;

        // First click of a possible double.
        UiElement* m_lastClickTarget = nullptr;
        glm::vec2  m_lastClickPos{0.0f, 0.0f};
        float      m_lastClickTime = 0.0f;

        // Animation clock. The advance is the DELTA against the previous frame;
        // without a previous frame (the first updateInput) there is no advance,
        // which is what keeps a large starting timeSeconds from eating
        // a whole animation in the first frame.
        float m_lastTime    = 0.0f;
        bool  m_hasLastTime = false;
    };

    // Where the mouse of a canvas that does NOT have the pointer this frame is left.
    // A point that falls inside no imaginable rect: that canvas's hit test
    // returns nullptr and its hover clears itself, with its MouseExit and with
    // its state colors back to Normal. "Not calling it" is not enough: a
    // canvas that stops being given input stays STUCK on the last hover
    // it saw, forever.
    inline glm::vec2 uiPointerAway() { return glm::vec2(-1.0e6f, -1.0e6f); }

    // Distributes ONE input state among ALL the scene's screen canvases.
    // `canvases` goes in PRIORITY order: the topmost first (that is, the
    // LAST one drawn, which is the one the user sees on top).
    //
    // Calling updateInput with the same state on the N canvases is NOT valid: two
    // overlapping canvases would leave BOTH with a widget in hover and a click would activate
    // two buttons at once. The distribution is:
    //
    //   - MOUSE to ONE only. It goes to the one that has the pointer capture
    //     (a button held down and not released, that is a drag in progress); if there is
    //     none, the first in the list with something under the cursor. The
    //     others receive the mouse in uiPointerAway() and with the buttons released,
    //     which is what clears their hover instead of leaving it stuck.
    //   - KEYBOARD and GAMEPAD to ONE only, and it does NOT have to be the same: the focus
    //     is not moved by the cursor. The keys go to the first canvas in the list
    //     that HAS focus, so typing in a text field keeps arriving
    //     even if the mouse wanders over another canvas. If none has
    //     focus they go to the topmost (today that is not noticeable: updateInput ignores
    //     keys without focus).
    //   - The FOCUS does not jump between canvases on its own: it moves on click. Tab and the
    //     arrows wrap around INSIDE the canvas that has it, as always.
    //     When the canvas that has the pointer takes focus, the others release it,
    //     which is what prevents two focus rings at once.
    //
    // All the canvases receive updateInput, including those that see nothing: it is
    // there that their animations and their buttons' color fade run.
    void dispatchUiInput(const std::vector<UiCanvas*>& canvases, const UiInputState& input);

    // Live node by NAME in the WHOLE subtree of `node`, or nullptr if there is
    // none. FULL traversal and not just the direct children: since the
    // sync respects the scene hierarchy, a nested widget's node
    // hangs from its parent's, and looking it up at a single level would leave
    // anything not at first level unfound.
    //
    // It lived as a local function (findUiNodeNamed) in ViewportPanel.cpp, which
    // only looked at the screen canvas. It is moved here, free, so that the
    // Renderer also uses it (findUiNode, which walks ALL the scene's canvases,
    // not just the screen one) without the editor and the engine each having
    // their own copy.
    inline const UiElement* findUiNodeIn(const UiElement& node, const std::string& wanted)
    {
        for (const auto& child : node.children())
        {
            if (child->name == wanted) return child.get();
            if (const UiElement* hit = findUiNodeIn(*child, wanted)) return hit;
        }
        return nullptr;
    }
}
