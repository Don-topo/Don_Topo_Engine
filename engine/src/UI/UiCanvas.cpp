#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiWidgets.h"

#include <cmath>

namespace DonTopo
{
    namespace
    {
        bool pointInRect(const glm::vec2& p, const glm::vec2& pos, const glm::vec2& size)
        {
            // Half-open on the right and bottom: two adjoining rects do not
            // fight over the pixel column they share. A rect of size 0
            // (or negative, which margin stretching can produce) receives
            // nothing, because no point satisfies both inequalities at once.
            return p.x >= pos.x && p.x < pos.x + size.x &&
                   p.y >= pos.y && p.y < pos.y + size.y;
        }

        bool pointInScissor(const glm::vec2& p, const UiScissor& s)
        {
            if (s.empty()) return false;
            const float x0 = (float)s.x;
            const float y0 = (float)s.y;
            return p.x >= x0 && p.x < x0 + (float)s.width &&
                   p.y >= y0 && p.y < y0 + (float)s.height;
        }

        // REVERSE pre-order: the tree is drawn parent-before-children and in sibling
        // order, so walking it backwards gives FIRST what was drawn last,
        // which is what is visually on top.
        UiElement* hitTestNode(UiElement& node, const glm::vec2& p)
        {
            if (!node.visible)  return nullptr;
            // Without a resolved rect the node is not placed (neither it nor its subtree):
            // the emitter never got to visit it or clipped it to zero.
            if (!node.rectValid) return nullptr;

            const auto& children = node.children();
            for (size_t i = children.size(); i > 0; --i)
            {
                if (UiElement* hit = hitTestNode(*children[i - 1], p)) return hit;
            }

            // The element can be transparent to the mouse without its children being so:
            // that is why this goes AFTER testing them.
            if (!node.raycastTarget) return nullptr;
            if (!pointInRect(p, node.screenPos, node.screenSize)) return nullptr;
            // screenScissor already comes intersected with the parent's clip (and its own
            // if the node has clipChildren): a child that sticks out of the parent's
            // clip receives nothing even if its rect contains the point.
            if (!pointInScissor(p, node.screenScissor)) return nullptr;

            return &node;
        }

        void invalidateSubtree(const UiElement& node)
        {
            node.rectValid = false;
            for (const auto& child : node.children()) invalidateSubtree(*child);
        }

        // Normal pre-order, skipping invisible or disabled subtrees
        // WHOLE: a hidden (or turned off) container hides not only its rect,
        // but also its focusable children.
        void collectFocusables(UiElement& node, std::vector<UiElement*>& out)
        {
            if (!node.visible || !node.enabled) return;
            if (node.focusable) out.push_back(&node);
            for (const auto& child : node.children()) collectFocusables(*child, out);
        }

        // Weight of the TRANSVERSE axis in directional navigation. Greater than 1
        // so that an aligned neighbor beats another one closer diagonally, which
        // is what someone navigating with a gamepad expects.
        constexpr float kNavCrossPenalty = 2.0f;

        glm::vec2 rectCenter(const UiElement& node)
        {
            return node.screenPos + node.screenSize * 0.5f;
        }

        float distance2(const glm::vec2& a, const glm::vec2& b)
        {
            const glm::vec2 d = a - b;
            return d.x * d.x + d.y * d.y;
        }
    }

    UiCanvas::UiCanvas()
    {
        // The root groups, does not paint: its rect is the whole screen and drawing it
        // would cover the scene with a white rectangle.
        m_root.drawable = false;
        // And it does not intercept the mouse either: if it did, EVERY click would land on it
        // and never reach the scene's background.
        m_root.raycastTarget = false;
    }

    void UiCanvas::clear()
    {
        // The state pointers point inside the tree that is going away: dropping them
        // here is what keeps the next updateInput from reading dead memory.
        m_hovered         = nullptr;
        m_focused         = nullptr;
        m_lastClickTarget = nullptr;
        for (int b = 0; b < 3; ++b) m_pressTarget[b] = nullptr;

        m_root.clearChildren();
    }

    void UiCanvas::buildDrawData(uint32_t width, uint32_t height, UiDrawData& out) const
    {
        out.clear();
        if (!m_visible || width == 0 || height == 0)
        {
            // Nothing was placed this frame: leaving the previous one's rects would make
            // the input keep responding on a canvas that is no longer drawn.
            // For the same reason the resolution goes back to neutral: an old uiScale on
            // a canvas that was not drawn would be a lie.
            m_uiScale       = 1.0f;
            m_uiOrigin      = {0.0f, 0.0f};
            m_referenceSize = {0.0f, 0.0f};
            m_lastWidth     = 0;
            m_lastHeight    = 0;
            m_rebuiltNodes  = 0;
            invalidateSubtree(m_root);
            return;
        }
        UiSpriteBatch::build(*this, width, height, out);
    }

    UiElement* UiCanvas::hitTest(const glm::vec2& point) const
    {
        if (!m_visible) return nullptr;
        return hitTestNode(const_cast<UiElement&>(m_root), point);
    }

    void UiCanvas::dispatch(UiElement* target, UiEvent& event, UiEventHandler UiElement::* slot) const
    {
        // Bubbling: from the element it was passed to toward the root, stopping as soon as
        // someone consumes. event.target does NOT change along the way.
        for (UiElement* n = target; n != nullptr && !event.consumed; n = n->parent())
        {
            UiEventHandler& handler = n->*slot;
            if (handler) handler(event);
        }
    }

    void UiCanvas::setFocus(UiElement* element)
    {
        if (element != nullptr && !element->focusable) return;
        if (element == m_focused) return;

        UiElement* previous = m_focused;

        // The focus moves BEFORE notifying: a handler that looks at focused() during
        // the Blur or the Focus sees the new state, not a half-done one.
        if (previous) previous->focused = false;
        m_focused = element;
        if (m_focused) m_focused->focused = true;

        // Blur first, Focus after: always in that order.
        if (previous)
        {
            UiEvent e{};
            e.type   = UiEventType::Blur;
            e.target = previous;
            dispatch(previous, e, &UiElement::onBlur);
        }
        if (m_focused)
        {
            UiEvent e{};
            e.type   = UiEventType::Focus;
            e.target = m_focused;
            dispatch(m_focused, e, &UiElement::onFocus);
        }
    }

    void UiCanvas::moveFocus(int direction)
    {
        std::vector<UiElement*> order;
        collectFocusables(m_root, order);
        if (order.empty()) return;

        size_t index = 0;
        bool   found = false;
        for (size_t i = 0; i < order.size(); ++i)
        {
            if (order[i] == m_focused) { index = i; found = true; break; }
        }

        // Without a previous focus (or with one that is no longer in the traversal) it enters through
        // the first going forward and through the last going backward.
        if (!found)
        {
            setFocus(direction >= 0 ? order.front() : order.back());
            return;
        }

        const size_t n = order.size();
        const size_t next = direction >= 0 ? (index + 1) % n
                                           : (index + n - 1) % n;
        setFocus(order[next]);
    }

    bool UiCanvas::navigate(UiNavDir dir)
    {
        UiElement* const previous = m_focused;

        // Next/Previous do NOT touch geometry: they are the Tab traversal, as is.
        if (dir == UiNavDir::Next || dir == UiNavDir::Previous)
        {
            moveFocus(dir == UiNavDir::Next ? 1 : -1);
            return m_focused != previous;
        }

        // Without a previous focus there is nowhere to measure from: it enters through the first of the
        // pre-order, whatever direction the navigation comes from.
        if (m_focused == nullptr)
        {
            std::vector<UiElement*> order;
            collectFocusables(m_root, order);
            if (order.empty()) return false;
            setFocus(order.front());
            return m_focused != previous;
        }

        // Explicit override: it rules over geometry, even if it points to the
        // opposite side. If the target is not focusable, setFocus ignores it and the focus
        // stays where it is (navigate returns false).
        UiElement* forced = nullptr;
        switch (dir)
        {
            case UiNavDir::Up:    forced = m_focused->navUp;    break;
            case UiNavDir::Down:  forced = m_focused->navDown;  break;
            case UiNavDir::Left:  forced = m_focused->navLeft;  break;
            case UiNavDir::Right: forced = m_focused->navRight; break;
            default: break;
        }
        if (forced != nullptr)
        {
            setFocus(forced);
            return m_focused != previous;
        }

        // From here on everything comes from the rects of the last buildDrawData. Without
        // it, the focus itself is not placed and there is nothing to compare.
        if (!m_focused->rectValid) return false;

        std::vector<UiElement*> order;
        collectFocusables(m_root, order);

        const glm::vec2 origin = rectCenter(*m_focused);

        UiElement* best      = nullptr;
        float      bestScore = 0.0f;
        for (UiElement* candidate : order)
        {
            if (candidate == m_focused || !candidate->rectValid) continue;

            const glm::vec2 d = rectCenter(*candidate) - origin;

            // And it grows DOWNWARD on screen: up is the smaller Y.
            float along = 0.0f;
            float cross = 0.0f;
            switch (dir)
            {
                case UiNavDir::Left:  along = -d.x; cross = std::fabs(d.y); break;
                case UiNavDir::Right: along =  d.x; cross = std::fabs(d.y); break;
                case UiNavDir::Up:    along = -d.y; cross = std::fabs(d.x); break;
                case UiNavDir::Down:  along =  d.y; cross = std::fabs(d.x); break;
                default: break;
            }
            // Its center has to fall TOWARD that direction; what lies on the
            // exact perpendicular (along == 0) does not count.
            if (along <= 0.0f) continue;

            const float score = along + kNavCrossPenalty * cross;
            // Strictly less, walking in pre-order: a perfect tie
            // is won by the first in the tree, not by whichever comes out of a foreign order.
            if (best == nullptr || score < bestScore)
            {
                best      = candidate;
                bestScore = score;
            }
        }

        // The directional one does NOT wrap around: without a candidate the focus stays.
        if (best == nullptr) return false;

        setFocus(best);
        return m_focused != previous;
    }

    namespace
    {
        // A non-interactable button still enters the hit test (otherwise Disabled
        // would never be painted when hovering over it) but it swallows the Click and
        // DoubleClick. The rest (Down, Up, Drag) still comes out.
        bool tragaElClick(const UiElement* element)
        {
            const Button* b = element ? element->asButton() : nullptr;
            return b != nullptr && !b->interactable;
        }

        // FIXED priority: Disabled > Pressed > Selected > Hover > Normal. There is
        // no state machine here; it is derived whole every frame from what the element already
        // carries plus interactable and selected.
        UiButtonState estadoDe(const Button& b, const UiInputState& input)
        {
            if (!b.interactable)                    return UiButtonState::Disabled;
            if (b.hovered && input.mouseDown[0])    return UiButtonState::Pressed;
            if (b.selected || (b.focusable && b.focused)) return UiButtonState::Selected;
            if (b.hovered)                          return UiButtonState::Hover;
            return UiButtonState::Normal;
        }

        // The state's color MULTIPLIED by the button's base tint. With the
        // base at white (the default) the state's color comes out as is, that is
        // exactly the usual; with another base, the same set of five
        // states serves buttons of different colors without duplicating them.
        glm::vec4 colorDe(const Button& b, UiButtonState s)
        {
            const glm::vec4* estado = &b.normalColor;
            switch (s)
            {
                case UiButtonState::Hover:    estado = &b.hoverColor;    break;
                case UiButtonState::Pressed:  estado = &b.pressedColor;  break;
                case UiButtonState::Disabled: estado = &b.disabledColor; break;
                case UiButtonState::Selected: estado = &b.selectedColor; break;
                case UiButtonState::Normal:
                default:                      estado = &b.normalColor;   break;
            }
            return *estado * b.baseColor;
        }

        const std::string& spriteDe(const Button& b, UiButtonState s)
        {
            switch (s)
            {
                case UiButtonState::Hover:    return b.hoverSprite;
                case UiButtonState::Pressed:  return b.pressedSprite;
                case UiButtonState::Disabled: return b.disabledSprite;
                case UiButtonState::Selected: return b.selectedSprite;
                case UiButtonState::Normal:
                default:                      return b.normalSprite;
            }
        }

        // The button is repainted every frame, but almost no frame CHANGES color:
        // marking only when the value is different is what keeps a
        // still canvas with buttons from re-emitting the whole tree on every updateInput.
        void escribeColor(Button& b, const glm::vec4& c)
        {
            if (b.color == c) return;
            b.color = c;
            b.markDirty(UiElement::DirtyMaterial);
        }

        void aplicaEstado(Button& b, const UiInputState& input)
        {
            const UiButtonState nuevo = estadoDe(b, input);

            if (b.transition == UiButtonTransition::SpriteSwap)
            {
                b.state      = nuevo;
                b.stateReady = true;
                // A state without art does NOT erase the sprite that was there: it keeps the one that
                // is there instead of leaving the element without drawing.
                const std::string& s = spriteDe(b, nuevo);
                if (!s.empty() && b.sprite != s)
                {
                    b.sprite = s;
                    b.markDirty(UiElement::DirtyMaterial);
                }
                return;
            }

            const glm::vec4 destino = colorDe(b, nuevo);

            if (b.transition == UiButtonTransition::ColorTint)
            {
                b.state      = nuevo;
                b.stateReady = true;
                escribeColor(b, destino);
                return;
            }

            // Animation: linear, and the caller sets the time.
            if (!b.stateReady)
            {
                // The button's first updateInput: PLACES, does not fade.
                b.state         = nuevo;
                b.stateReady    = true;
                b.fadeFrom      = destino;
                b.fadeStartTime = input.timeSeconds;
                escribeColor(b, destino);
                return;
            }

            if (nuevo != b.state)
            {
                // It starts from the CURRENT color, not from that of the state being
                // left: changing state in the middle of a fade does not give a jump.
                b.fadeFrom      = b.color;
                b.fadeStartTime = input.timeSeconds;
                b.state         = nuevo;
            }

            float t = 1.0f;
            if (b.fadeDuration > 0.0f)
                t = (input.timeSeconds - b.fadeStartTime) / b.fadeDuration;

            // The clamp is what keeps the color from running on past the end of the fade
            // (and a backward time from sending it to the other side).
            if (t <= 0.0f)      escribeColor(b, b.fadeFrom);
            else if (t >= 1.0f) escribeColor(b, destino);   // exact, without the mix's error
            else                escribeColor(b, b.fadeFrom + (destino - b.fadeFrom) * t);
        }

        // A single pass over the tree, at the end of updateInput.
        void tickBotones(UiElement& element, const UiInputState& input)
        {
            if (Button* b = element.asButton()) aplicaEstado(*b, input);
            for (const auto& hijo : element.children()) tickBotones(*hijo, input);
        }

        // ── Animation curves ────────────────────────────────────────────────
        // Pure functions of t: same t, same value, always. The two end
        // snaps are NOT a convenience clamp, they are what guarantees
        // EXACT f(0)=0 and f(1)=1 even if the formula inside comes out at
        // 0.99999994 because of rounding (Bounce and Elastic do).
        float curvaAnim(UiAnimCurve curva, float t)
        {
            if (t <= 0.0f) return 0.0f;
            if (t >= 1.0f) return 1.0f;

            switch (curva)
            {
                case UiAnimCurve::EaseIn:
                    return t * t;

                case UiAnimCurve::EaseOut:
                {
                    const float u = 1.0f - t;
                    return 1.0f - u * u;
                }

                case UiAnimCurve::Bounce:
                {
                    // Four parabolas that get smaller and higher: it does not
                    // leave [0,1], but it is NOT monotonic (that is where the bounces are).
                    const float n = 7.5625f;
                    const float d = 2.75f;
                    if (t < 1.0f / d) return n * t * t;
                    if (t < 2.0f / d) { const float u = t - 1.5f   / d; return n * u * u + 0.75f; }
                    if (t < 2.5f / d) { const float u = t - 2.25f  / d; return n * u * u + 0.9375f; }
                    const float u = t - 2.625f / d;
                    return n * u * u + 0.984375f;
                }

                case UiAnimCurve::Elastic:
                {
                    // Damped spring: it OVERSHOOTS the target and comes back, so
                    // it goes past 1 halfway on purpose.
                    const float c = 2.0f * 3.14159265358979323846f / 3.0f;
                    return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c) + 1.0f;
                }

                case UiAnimCurve::Linear:
                default:
                    return t;
            }
        }

        // Writes the property. On finishing, animTo is copied AS IS: the lerp
        // with t=1 leaves 0.99999994 and the property would stay a hair short of its
        // target forever.
        void aplicaAnim(UiElement& e, float k, bool remata)
        {
            const glm::vec4 v = remata ? e.animTo
                                       : e.animFrom + (e.animTo - e.animFrom) * k;

            switch (e.anim)
            {
                // Each curve marks EXACTLY what it writes. Fade goes into
                // Transform and not Material because opacity is multiplied
                // downward: if it were not marked, the children would keep the alpha
                // of the previous frame.
                case UiAnim::Fade:     e.opacity  = v.x;                  e.markDirty(UiElement::DirtyTransform); break;
                case UiAnim::Scale:    e.scale    = glm::vec2(v.x, v.y);  e.markDirty(UiElement::DirtyTransform); break;
                case UiAnim::Move:     e.position = glm::vec2(v.x, v.y);  e.markDirty(UiElement::DirtyTransform); break;
                // Rotation only turns the vertices that THIS node emits (the
                // children go back to the earlier state), so it does not propagate out of it.
                case UiAnim::Rotation: e.rotation = v.x;                  e.markDirty(UiElement::DirtyVertex);    break;
                case UiAnim::Color:    e.color    = v;                    e.markDirty(UiElement::DirtyMaterial);  break;
                case UiAnim::None:
                default: break;
            }
        }

        // A single pass over the tree with the frame's delta. With animPlaying
        // false it neither advances NOR writes: the property stays wherever it is.
        void tickAnimaciones(UiElement& e, float dt)
        {
            if (e.anim != UiAnim::None && e.animPlaying && e.animDuration > 0.0f)
            {
                e.animTime += dt;

                float t      = 0.0f;
                bool  remata = false;

                switch (e.animLoop)
                {
                    case UiAnimLoop::Loop:
                        // fmod and not subtracting the duration by hand: a time
                        // jump of several laps lands where it should in one go.
                        t = std::fmod(e.animTime, e.animDuration) / e.animDuration;
                        break;

                    case UiAnimLoop::PingPong:
                    {
                        const float ciclo = e.animDuration * 2.0f;
                        const float m     = std::fmod(e.animTime, ciclo);
                        t = (m <= e.animDuration) ? m / e.animDuration
                                                  : (ciclo - m) / e.animDuration;
                        break;
                    }

                    case UiAnimLoop::Once:
                    default:
                        if (e.animTime >= e.animDuration)
                        {
                            e.animTime = e.animDuration;
                            t          = 1.0f;
                            remata     = true;
                        }
                        else t = e.animTime / e.animDuration;
                        break;
                }

                aplicaAnim(e, curvaAnim(e.animCurve, t), remata);
                if (remata) e.animPlaying = false;
            }

            for (const auto& hijo : e.children()) tickAnimaciones(*hijo, dt);
        }
    }

    void UiCanvas::updateInput(const UiInputState& input)
    {
        // The animations, BEFORE anything else: the clock is the one here and the advance
        // is the delta against the previous frame. What they write is seen in the
        // next buildDrawData, not in this frame's rects (which are
        // the ones the hit test just inherited from the previous build).
        const float dtAnim = m_hasLastTime ? (input.timeSeconds - m_lastTime) : 0.0f;
        m_lastTime    = input.timeSeconds;
        m_hasLastTime = true;
        tickAnimaciones(m_root, dtAnim);

        UiElement* hit = hitTest(input.mousePos);

        const glm::vec2 delta = m_hasLastMouse ? (input.mousePos - m_lastMousePos)
                                               : glm::vec2(0.0f, 0.0f);
        const bool moved = !m_hasLastMouse || input.mousePos != m_lastMousePos;

        // Common template: all the events carry the same mouse snapshot, the
        // same time and the same modifiers.
        UiEvent base{};
        base.mousePos = input.mousePos;
        base.delta    = delta;
        base.shift    = input.shift;
        base.ctrl     = input.ctrl;
        base.alt      = input.alt;
        base.time     = input.timeSeconds;

        // ── Hover ───────────────────────────────────────────────────────────
        // Enter and Exit are DERIVED: this frame's hit and the previous one's are
        // compared. There is no Hover event, there is a bool hovered.
        if (hit != m_hovered)
        {
            UiElement* previous = m_hovered;
            if (previous) previous->hovered = false;
            m_hovered = hit;
            if (m_hovered) m_hovered->hovered = true;

            if (previous)
            {
                UiEvent e  = base;
                e.type     = UiEventType::MouseExit;
                e.target   = previous;
                dispatch(previous, e, &UiElement::onMouseExit);
            }
            if (m_hovered)
            {
                UiEvent e  = base;
                e.type     = UiEventType::MouseEnter;
                e.target   = m_hovered;
                dispatch(m_hovered, e, &UiElement::onMouseEnter);
            }
        }

        if (hit && moved)
        {
            UiEvent e = base;
            e.type    = UiEventType::MouseMove;
            e.target  = hit;
            dispatch(hit, e, &UiElement::onMouseMove);
        }

        // ── Buttons ─────────────────────────────────────────────────────────
        for (int b = 0; b < 3; ++b)
        {
            const bool now = input.mouseDown[b];
            const bool was = m_buttonDown[b];
            const UiMouseButton button = (UiMouseButton)b;

            if (now && !was)
            {
                m_pressTarget[b] = hit;
                m_pressPos[b]    = input.mousePos;
                m_dragging[b]    = false;

                if (hit)
                {
                    UiEvent e  = base;
                    e.type     = UiEventType::MouseDown;
                    e.target   = hit;
                    e.button   = button;
                    dispatch(hit, e, &UiElement::onMouseDown);

                    // The focus is taken by the first focusable in the chain: clicking
                    // on the label inside a field focuses the field.
                    for (UiElement* n = hit; n != nullptr; n = n->parent())
                    {
                        if (n->focusable) { setFocus(n); break; }
                    }
                }
            }
            else if (now && was)
            {
                UiElement* source = m_pressTarget[b];
                if (source && !m_dragging[b])
                {
                    const float d2 = distance2(input.mousePos, m_pressPos[b]);
                    if (d2 > dragThreshold * dragThreshold)
                    {
                        m_dragging[b] = true;
                        UiEvent e   = base;
                        e.type      = UiEventType::DragBegin;
                        e.target    = source;
                        e.button    = button;
                        e.dragStart = m_pressPos[b];
                        e.dragSource = source;
                        dispatch(source, e, &UiElement::onDragBegin);
                    }
                }

                // The frame that crosses the threshold emits DragBegin AND its first Drag:
                // otherwise, a one-jump gesture would give not even one Drag.
                if (source && m_dragging[b] && moved)
                {
                    UiEvent e   = base;
                    e.type      = UiEventType::Drag;
                    e.target    = source;
                    e.button    = button;
                    e.dragStart = m_pressPos[b];
                    e.dragSource = source;
                    dispatch(source, e, &UiElement::onDrag);
                }
            }
            else if (!now && was)
            {
                UiElement* source = m_pressTarget[b];

                // MouseUp goes to whoever is UNDER THE CURSOR, which may not be whoever
                // received the Down: that difference is why there is no Click.
                if (hit)
                {
                    UiEvent e = base;
                    e.type    = UiEventType::MouseUp;
                    e.target  = hit;
                    e.button  = button;
                    dispatch(hit, e, &UiElement::onMouseUp);
                }

                if (m_dragging[b])
                {
                    if (source)
                    {
                        UiEvent e   = base;
                        e.type      = UiEventType::DragEnd;
                        e.target    = source;
                        e.button    = button;
                        e.dragStart = m_pressPos[b];
                        e.dragSource = source;
                        dispatch(source, e, &UiElement::onDragEnd);
                    }
                    // The Drop belongs to the TARGET element, and it may not be the DragBegin's
                    // (nor exist, if released outside of everything).
                    if (hit)
                    {
                        UiEvent e   = base;
                        e.type      = UiEventType::Drop;
                        e.target    = hit;
                        e.button    = button;
                        e.dragStart = m_pressPos[b];
                        e.dragSource = source;
                        dispatch(hit, e, &UiElement::onDrop);
                    }
                    // A drag CANCELS that gesture's click, and also cuts
                    // the double-click chain: releasing after dragging cannot
                    // be the first half of a double click.
                    m_lastClickTarget = nullptr;
                }
                // The threshold is checked again here and not only in the frames with the
                // button held: a gesture that goes down and up in two consecutive frames
                // passes through none of those, and 200 px of travel is not a click.
                else if (source && hit == source && !tragaElClick(source) &&
                         distance2(input.mousePos, m_pressPos[b]) <= dragThreshold * dragThreshold)
                {
                    UiEvent e = base;
                    e.type    = UiEventType::Click;
                    e.target  = source;
                    e.button  = button;
                    dispatch(source, e, &UiElement::onClick);

                    const bool doble =
                        m_lastClickTarget == source &&
                        (input.timeSeconds - m_lastClickTime) <= doubleClickTime &&
                        distance2(input.mousePos, m_lastClickPos) <= doubleClickDistance * doubleClickDistance;

                    if (doble)
                    {
                        UiEvent d = base;
                        d.type    = UiEventType::DoubleClick;
                        d.target  = source;
                        d.button  = button;
                        dispatch(source, d, &UiElement::onDoubleClick);

                        // Consumed: a third click starts a new pair instead
                        // of firing another double.
                        m_lastClickTarget = nullptr;
                    }
                    else
                    {
                        m_lastClickTarget = source;
                        m_lastClickTime   = input.timeSeconds;
                        m_lastClickPos    = input.mousePos;
                    }
                }

                m_pressTarget[b] = nullptr;
                m_dragging[b]    = false;
            }

            m_buttonDown[b] = now;
        }

        // ── Wheel ───────────────────────────────────────────────────────────
        if (input.scrollDelta != 0.0f && hit)
        {
            UiEvent e     = base;
            e.type        = UiEventType::Scroll;
            e.target      = hit;
            e.scrollDelta = input.scrollDelta;
            dispatch(hit, e, &UiElement::onScroll);
        }

        // ── Keyboard ────────────────────────────────────────────────────────
        // ONLY to the focused element, and bubbling. Without focus nothing is emitted:
        // not even Tab, which without a starting point would not know which way to go.
        for (UiKey key : input.keys)
        {
            if (!m_focused) break;

            UiElement* target = m_focused;

            UiEvent e = base;
            e.type    = UiEventType::KeyDown;
            e.target  = target;
            e.key     = key;
            dispatch(target, e, &UiElement::onKeyDown);

            // The keys are DELIVERED as they are; the canvas only reserves a
            // few actions of its own, and yields them if someone consumed the key.
            if (e.consumed) continue;
            if (key == UiKey::Tab)         { moveFocus(input.shift ? -1 : 1); continue; }
            if (key == UiKey::Escape)      { setFocus(nullptr); continue; }

            // Arrows and Enter: this is what makes a menu playable with a gamepad. Whoever
            // wants the arrows for something else consumes them in their handler, or
            // turns off keyboardNavigation.
            if (!keyboardNavigation) continue;
            switch (key)
            {
                case UiKey::Left:  navigate(UiNavDir::Left);  break;
                case UiKey::Right: navigate(UiNavDir::Right); break;
                case UiKey::Up:    navigate(UiNavDir::Up);    break;
                case UiKey::Down:  navigate(UiNavDir::Down);  break;
                case UiKey::Enter: submitFocused();           break;
                default: break;
            }
        }

        // ── Text ────────────────────────────────────────────────────────────
        // AFTER the keys and with the same rule: only to the focused element
        // and bubbling. A character without a destination is discarded instead of going to the
        // first one that happens to pass by.
        //
        // It is kept apart from the key loop and not inside it because the two lists are
        // independent: a frame can carry only text (typing), only
        // keys (Tab, Backspace) or both, and there is no way to interleave them
        // without inventing an order that the caller has not given.
        for (uint32_t cp : input.chars)
        {
            if (!m_focused) break;

            UiEvent e   = base;
            e.type      = UiEventType::TextInput;
            e.target    = m_focused;
            e.codepoint = cp;
            dispatch(m_focused, e, &UiElement::onTextInput);
        }

        m_lastMousePos = input.mousePos;
        m_hasLastMouse = true;

        // ── Buttons ─────────────────────────────────────────────────────────
        // LAST: the states are derived from the hover, the focus and the mouse
        // button that have just been fixed above. Whoever does not call
        // updateInput sees not a single change: buildDrawData keeps giving the same
        // vertices and the same batches.
        tickBotones(m_root, input);
    }

    bool UiCanvas::submitFocused()
    {
        UiElement* target = m_focused;
        if (target == nullptr) return false;
        // The same rules applied to the mouse: what cannot be
        // clicked with the cursor is not activated with the gamepad either.
        if (!target->visible || !target->enabled) return false;
        if (tragaElClick(target)) return false;

        UiEvent e{};
        e.type   = UiEventType::Click;
        e.target = target;
        e.button = UiMouseButton::Left;
        e.time   = m_lastTime;
        // The "cursor" is the element's center: a handler that looks at where it
        // was pressed receives a point that falls INSIDE, not a (0,0) that would be
        // anywhere else on the screen.
        if (target->rectValid) e.mousePos = target->screenPos + target->screenSize * 0.5f;

        dispatch(target, e, &UiElement::onClick);
        return true;
    }

    void UiCanvas::releaseInput()
    {
        // The hover, with its MouseExit: for the same reason it is emitted to
        // an input with the mouse outside. Whoever turns something off on exit has to
        // find out, and "stuck on the last hover" is exactly the failure that
        // dispatchUiInput avoids for those who lose the pointer; here it cannot,
        // because the canvas is no longer in its list.
        if (m_hovered)
        {
            UiElement* previo = m_hovered;
            previo->hovered   = false;
            m_hovered         = nullptr;

            UiEvent e{};
            e.type     = UiEventType::MouseExit;
            e.target   = previo;
            e.mousePos = uiPointerAway();
            e.time     = m_lastTime;
            dispatch(previo, e, &UiElement::onMouseExit);
        }

        // The capture and the half-done gesture. m_buttonDown is also lowered: if it
        // were left at true, the frame in which the canvas comes back would see `!now && was`
        // and emit the MouseUp of a press that no longer exists.
        for (int b = 0; b < 3; ++b)
        {
            m_pressTarget[b] = nullptr;
            m_dragging[b]    = false;
            m_buttonDown[b]  = false;
        }
        // And a cut gesture cannot be the first half of a double click.
        m_lastClickTarget = nullptr;

        // The focus, with its Blur. It goes through setFocus and not by hand so that the Blur
        // comes out through the same place as always. Without this, a canvas that comes back
        // with focus can end up being the keyboard owner ahead of the one
        // the user just pressed, because outside the list dispatchUiInput
        // could not release it either.
        setFocus(nullptr);
    }

    void dispatchUiInput(const std::vector<UiCanvas*>& canvases, const UiInputState& input)
    {
        // ── Who gets the MOUSE ──────────────────────────────────────────────
        // 1) The CAPTURE rules over the overlap. A button held down and not released
        //    keeps the pointer even if the cursor has gone over another
        //    canvas: without this, dragging a slider that peeks out from under another
        //    canvas cuts the gesture right when crossing the edge, and the drag is
        //    lost without a single warning.
        UiCanvas* raton = nullptr;
        for (UiCanvas* c : canvases)
            if (c && c->pointerCaptured()) { raton = c; break; }

        // 2) Without capture, the TOPMOST one with something under the cursor wins.
        //    `canvases` already arrives in that order (the last one drawn, first),
        //    so nothing is decided here: it is walked.
        if (!raton)
            for (UiCanvas* c : canvases)
                if (c && c->hitTest(input.mousePos)) { raton = c; break; }

        // ── Who gets the KEYBOARD ───────────────────────────────────────────
        // The FOCUS, not the cursor: typing in a text field keeps arriving
        // even if the mouse wanders over another canvas. If nobody has
        // focus they go to the topmost one. Today that is not noticeable (updateInput ignores
        // keys without focus), but it leaves the rule complete instead of a gap.
        UiCanvas* teclado = nullptr;
        for (UiCanvas* c : canvases)
            if (c && c->focused()) { teclado = c; break; }
        if (!teclado)
            for (UiCanvas* c : canvases)
                if (c) { teclado = c; break; }

        // What the one that does NOT have the pointer receives: the mouse OUTSIDE. It is what
        // clears its hover (with its MouseExit and its colors back to Normal) instead
        // of leaving it stuck. The clock is kept: its animations and its
        // buttons' fade keep running.
        //
        // It lies about the POSITION, and NOT about the BUTTONS. That is the difference between cleaning
        // the state and faking it: the edge count (`now && !was`)
        // has to remain the real one or the canvas loses presses.
        // Lying here gave a PHANTOM CLICK, and even with a SINGLE canvas: when
        // nobody wins the pointer (the cursor over the background, no widget underneath) the
        // scene's only canvas receives this, so it did not see the MouseDown; when the cursor
        // later entered a button with the button STILL down, it saw a NEW
        // edge, registered the press there and on release emitted a Click that
        // nobody asked for, also stealing the focus.
        //
        // With the real buttons no extra guard is needed: the mouse
        // is outside, so `hitTest` gives nullptr and the press is registered on
        // nullptr. No MouseDown is dispatched, the focus does not move, and on release there
        // is no Click because the origin is null (there is a MouseUp if the cursor is over
        // something, which is the usual semantics). The colors do not change either:
        // `estadoDe` requires `hovered` to paint Pressed. And `pointerCaptured()`
        // stays false for the loser, which is what keeps it from stealing the
        // pointer from the one on top in step 1.
        UiInputState fuera = input;
        fuera.mousePos     = uiPointerAway();
        fuera.scrollDelta  = 0.0f;
        fuera.keys.clear();
        fuera.chars.clear();

        for (UiCanvas* c : canvases)
        {
            if (!c) continue;
            UiInputState propio = (c == raton) ? input : fuera;
            if (c == teclado)
            {
                propio.keys  = input.keys;
                propio.chars = input.chars;
            }
            else
            {
                propio.keys.clear();
                propio.chars.clear();
            }
            c->updateInput(propio);
        }

        // The focus moves on CLICK, and can only be in one canvas: as soon as
        // the one that has the pointer takes focus, the others release it. Without this
        // two focus rings would remain at once and the keyboard owner would be
        // whichever the list order decided, not the one that just pressed.
        //
        // It goes AFTER the loop on purpose: the focus to respect is the
        // one this frame leaves, not the previous one's. And `focused()` is looked at instead
        // of "did a button go down?" because it is the same without storing state between
        // frames: clicking on something NOT focusable does not steal the focus, exactly
        // as already happens inside a single canvas.
        if (raton && raton->focused())
            for (UiCanvas* c : canvases)
                if (c && c != raton) c->setFocus(nullptr);
    }
}
