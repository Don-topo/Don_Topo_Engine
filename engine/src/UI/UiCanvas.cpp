#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiWidgets.h"

#include <cmath>

namespace DonTopo
{
    namespace
    {
        bool pointInRect(const glm::vec2& p, const glm::vec2& pos, const glm::vec2& size)
        {
            // Medio abierto por la derecha y por abajo: dos rects pegados no se
            // disputan la columna de píxeles que comparten. Un rect de tamaño 0
            // (o negativo, que el estirado por márgenes puede producir) no recibe
            // nada, porque ningún punto cumple las dos desigualdades a la vez.
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

        // Pre-orden INVERSO: el árbol se dibuja padre-antes-que-hijos y en orden
        // de hermanos, así que recorrerlo al revés da PRIMERO lo último dibujado,
        // que es lo que está visualmente encima.
        UiElement* hitTestNode(UiElement& node, const glm::vec2& p)
        {
            if (!node.visible)  return nullptr;
            // Sin rect resuelto el nodo no está colocado (ni él ni su subárbol):
            // el emisor no llegó a visitarlo o lo recortó a cero.
            if (!node.rectValid) return nullptr;

            const auto& children = node.children();
            for (size_t i = children.size(); i > 0; --i)
            {
                if (UiElement* hit = hitTestNode(*children[i - 1], p)) return hit;
            }

            // El elemento puede ser transparente al ratón sin que lo sean sus
            // hijos: por eso esto va DESPUÉS de probarlos.
            if (!node.raycastTarget) return nullptr;
            if (!pointInRect(p, node.screenPos, node.screenSize)) return nullptr;
            // screenScissor ya trae intersecado el recorte del padre (y el propio
            // si el nodo tiene clipChildren): un hijo que se sale del recorte del
            // padre no recibe nada aunque su rect contenga el punto.
            if (!pointInScissor(p, node.screenScissor)) return nullptr;

            return &node;
        }

        void invalidateSubtree(const UiElement& node)
        {
            node.rectValid = false;
            for (const auto& child : node.children()) invalidateSubtree(*child);
        }

        // Pre-orden normal, saltando subárboles invisibles o deshabilitados
        // ENTEROS: un contenedor oculto (o apagado) no esconde solo su rect,
        // también a sus hijos focusables.
        void collectFocusables(UiElement& node, std::vector<UiElement*>& out)
        {
            if (!node.visible || !node.enabled) return;
            if (node.focusable) out.push_back(&node);
            for (const auto& child : node.children()) collectFocusables(*child, out);
        }

        // Peso del eje TRANSVERSAL en la navegación direccional. Mayor que 1
        // para que un vecino alineado gane a otro más cercano en diagonal, que
        // es lo que espera quien navega con un mando.
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
        // La raíz agrupa, no pinta: su rect es la pantalla entera y dibujarla
        // taparía la escena con un rectángulo blanco.
        m_root.drawable = false;
        // Y tampoco intercepta el ratón: si lo hiciera, TODO click caería en ella
        // y nunca llegaría al fondo de la escena.
        m_root.raycastTarget = false;
    }

    void UiCanvas::clear()
    {
        // Los punteros de estado apuntan dentro del árbol que se va: soltarlos
        // aquí es lo que evita que el siguiente updateInput lea memoria muerta.
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
            // Nada se colocó este frame: dejar los rects del anterior haría que
            // el input siguiera respondiendo sobre un canvas que ya no se dibuja.
            // Por lo mismo la resolución vuelve a neutra: un uiScale viejo sobre
            // un canvas que no se dibujó sería una mentira.
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
        // Burbujeo: del elemento al que le pasó hacia la raíz, parando en cuanto
        // alguien consuma. event.target NO cambia por el camino.
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

        // El foco se mueve ANTES de avisar: un handler que mire focused() durante
        // el Blur o el Focus ve el estado nuevo, no uno a medias.
        if (previous) previous->focused = false;
        m_focused = element;
        if (m_focused) m_focused->focused = true;

        // Blur primero, Focus después: siempre en ese orden.
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

        // Sin foco previo (o con uno que ya no está en el recorrido) se entra por
        // el primero yendo hacia delante y por el último yendo hacia atrás.
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

        // Next/Previous NO tocan geometría: son el recorrido del Tab, tal cual.
        if (dir == UiNavDir::Next || dir == UiNavDir::Previous)
        {
            moveFocus(dir == UiNavDir::Next ? 1 : -1);
            return m_focused != previous;
        }

        // Sin foco previo no hay desde dónde medir: se entra por el primero del
        // pre-orden, venga la navegación de la dirección que venga.
        if (m_focused == nullptr)
        {
            std::vector<UiElement*> order;
            collectFocusables(m_root, order);
            if (order.empty()) return false;
            setFocus(order.front());
            return m_focused != previous;
        }

        // Override explícito: manda sobre la geometría, aunque apunte al lado
        // contrario. Si el destino no es focusable, setFocus lo ignora y el foco
        // se queda donde está (navigate devuelve false).
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

        // A partir de aquí todo sale de los rects del último buildDrawData. Sin
        // él, el propio foco no está colocado y no hay nada que comparar.
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

            // Y crece hacia ABAJO en pantalla: arriba es la Y menor.
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
            // Su centro tiene que caer HACIA esa dirección; lo que queda en la
            // perpendicular exacta (along == 0) no cuenta.
            if (along <= 0.0f) continue;

            const float score = along + kNavCrossPenalty * cross;
            // Estrictamente menor, recorriendo en pre-orden: un empate perfecto
            // lo gana el primero del árbol, no el que salga de un orden ajeno.
            if (best == nullptr || score < bestScore)
            {
                best      = candidate;
                bestScore = score;
            }
        }

        // La direccional NO da la vuelta: sin candidato el foco se queda.
        if (best == nullptr) return false;

        setFocus(best);
        return m_focused != previous;
    }

    namespace
    {
        // Un botón no interactable sigue entrando en el hit test (si no, Disabled
        // no se pintaría nunca al pasar por encima) pero se come el Click y el
        // DoubleClick. Lo demás (Down, Up, Drag) sigue saliendo.
        bool tragaElClick(const UiElement* element)
        {
            const Button* b = element ? element->asButton() : nullptr;
            return b != nullptr && !b->interactable;
        }

        // Prioridad FIJA: Disabled > Pressed > Selected > Hover > Normal. Aquí
        // no hay máquina de estados; se deriva entera cada frame de lo que ya
        // lleva el elemento más interactable y selected.
        UiButtonState estadoDe(const Button& b, const UiInputState& input)
        {
            if (!b.interactable)                    return UiButtonState::Disabled;
            if (b.hovered && input.mouseDown[0])    return UiButtonState::Pressed;
            if (b.selected || (b.focusable && b.focused)) return UiButtonState::Selected;
            if (b.hovered)                          return UiButtonState::Hover;
            return UiButtonState::Normal;
        }

        // El color del estado MULTIPLICADO por el tinte base del botón. Con la
        // base en blanco (el default) sale el color del estado tal cual, o sea
        // exactamente lo de siempre; con otra base, el mismo juego de cinco
        // estados sirve para botones de colores distintos sin duplicarlos.
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

        // El botón se repinta cada frame, pero casi ningún frame CAMBIA de color:
        // marcar solo cuando el valor es distinto es lo que impide que un canvas
        // quieto con botones reemita el árbol entero por cada updateInput.
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
                // Un estado sin arte NO borra el sprite que hubiera: deja el que
                // está en vez de dejar el elemento sin dibujo.
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

            // Animation: lineal, y el tiempo lo pone quien llama.
            if (!b.stateReady)
            {
                // Primer updateInput del botón: COLOCA, no funde.
                b.state         = nuevo;
                b.stateReady    = true;
                b.fadeFrom      = destino;
                b.fadeStartTime = input.timeSeconds;
                escribeColor(b, destino);
                return;
            }

            if (nuevo != b.state)
            {
                // Se arranca desde el color ACTUAL, no desde el del estado que
                // se deja: cambiar de estado a mitad de fundido no da un salto.
                b.fadeFrom      = b.color;
                b.fadeStartTime = input.timeSeconds;
                b.state         = nuevo;
            }

            float t = 1.0f;
            if (b.fadeDuration > 0.0f)
                t = (input.timeSeconds - b.fadeStartTime) / b.fadeDuration;

            // El clamp es lo que impide que pasado el fundido el color siga de
            // largo (y que un tiempo hacia atrás lo mande al otro lado).
            if (t <= 0.0f)      escribeColor(b, b.fadeFrom);
            else if (t >= 1.0f) escribeColor(b, destino);   // exacto, sin el error del mix
            else                escribeColor(b, b.fadeFrom + (destino - b.fadeFrom) * t);
        }

        // Una sola pasada por el árbol, al final del updateInput.
        void tickBotones(UiElement& element, const UiInputState& input)
        {
            if (Button* b = element.asButton()) aplicaEstado(*b, input);
            for (const auto& hijo : element.children()) tickBotones(*hijo, input);
        }

        // ── Curvas de animación ─────────────────────────────────────────────
        // Funciones puras de t: mismo t, mismo valor, siempre. Los dos remates
        // de los extremos NO son un clamp de conveniencia, son lo que garantiza
        // f(0)=0 y f(1)=1 EXACTOS aunque la fórmula de dentro salga a
        // 0.99999994 por el redondeo (Bounce y Elastic lo hacen).
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
                    // Cuatro parábolas cada vez más pequeñas y más altas: no se
                    // sale de [0,1], pero NO es monótona (ahí están los botes).
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
                    // Muelle amortiguado: SE PASA del destino y vuelve, así que
                    // pasa de 1 a mitad de camino a propósito.
                    const float c = 2.0f * 3.14159265358979323846f / 3.0f;
                    return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c) + 1.0f;
                }

                case UiAnimCurve::Linear:
                default:
                    return t;
            }
        }

        // Escribe la propiedad. Al rematar se copia animTo TAL CUAL: el lerp
        // con t=1 deja 0.99999994 y la propiedad se quedaría a un pelo de su
        // destino para siempre.
        void aplicaAnim(UiElement& e, float k, bool remata)
        {
            const glm::vec4 v = remata ? e.animTo
                                       : e.animFrom + (e.animTo - e.animFrom) * k;

            switch (e.anim)
            {
                // Cada curva marca EXACTAMENTE lo que escribe. Fade entra en
                // Transform y no en Material porque la opacidad se multiplica
                // hacia abajo: si no bajase, los hijos se quedarían con el alfa
                // del frame anterior.
                case UiAnim::Fade:     e.opacity  = v.x;                  e.markDirty(UiElement::DirtyTransform); break;
                case UiAnim::Scale:    e.scale    = glm::vec2(v.x, v.y);  e.markDirty(UiElement::DirtyTransform); break;
                case UiAnim::Move:     e.position = glm::vec2(v.x, v.y);  e.markDirty(UiElement::DirtyTransform); break;
                // La rotación solo gira los vértices que emite ESTE nodo (los
                // hijos vuelven al estado de antes), así que no sale de él.
                case UiAnim::Rotation: e.rotation = v.x;                  e.markDirty(UiElement::DirtyVertex);    break;
                case UiAnim::Color:    e.color    = v;                    e.markDirty(UiElement::DirtyMaterial);  break;
                case UiAnim::None:
                default: break;
            }
        }

        // Una sola pasada por el árbol con el delta del frame. Con animPlaying
        // a false no se avanza NI se escribe: la propiedad se queda donde esté.
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
                        // fmod y no restar la duración a mano: un salto de
                        // tiempo de varias vueltas cae donde toca de una vez.
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
        // Las animaciones, ANTES que nada: el reloj es el de aquí y el avance
        // es el delta contra el frame anterior. Lo que escriban se ve en el
        // siguiente buildDrawData, no en los rects de este frame (que son los
        // que el hit test acaba de heredar del build anterior).
        const float dtAnim = m_hasLastTime ? (input.timeSeconds - m_lastTime) : 0.0f;
        m_lastTime    = input.timeSeconds;
        m_hasLastTime = true;
        tickAnimaciones(m_root, dtAnim);

        UiElement* hit = hitTest(input.mousePos);

        const glm::vec2 delta = m_hasLastMouse ? (input.mousePos - m_lastMousePos)
                                               : glm::vec2(0.0f, 0.0f);
        const bool moved = !m_hasLastMouse || input.mousePos != m_lastMousePos;

        // Plantilla común: todos los eventos llevan la misma foto del ratón, el
        // mismo tiempo y los mismos modificadores.
        UiEvent base{};
        base.mousePos = input.mousePos;
        base.delta    = delta;
        base.shift    = input.shift;
        base.ctrl     = input.ctrl;
        base.alt      = input.alt;
        base.time     = input.timeSeconds;

        // ── Hover ───────────────────────────────────────────────────────────
        // Enter y Exit son DERIVADOS: se comparan el hit de este frame y el del
        // anterior. No hay evento Hover, hay un bool hovered.
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

        // ── Botones ─────────────────────────────────────────────────────────
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

                    // El foco lo toma el primer focusable de la cadena: pinchar
                    // en la etiqueta de dentro de un campo enfoca el campo.
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

                // El frame que cruza el umbral emite DragBegin Y su primer Drag:
                // si no, un gesto de un solo salto no daría ni un Drag.
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

                // MouseUp va a quien esté BAJO EL CURSOR, que puede no ser quien
                // recibió el Down: de esa diferencia sale que no haya Click.
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
                    // El Drop es del elemento de DESTINO, y puede no ser el del
                    // DragBegin (ni existir, si se suelta fuera de todo).
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
                    // Un arrastre CANCELA el click de ese gesto, y también corta
                    // la cadena de doble click: soltar tras arrastrar no puede
                    // ser la primera mitad de un doble click.
                    m_lastClickTarget = nullptr;
                }
                // El umbral se vuelve a mirar aquí y no solo en los frames con el
                // botón mantenido: un gesto que baja y sube en dos frames seguidos
                // no pasa por ninguno de esos, y 200 px de recorrido no son un click.
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

                        // Consumido: un tercer click empieza pareja nueva en vez
                        // de disparar otro doble.
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

        // ── Rueda ───────────────────────────────────────────────────────────
        if (input.scrollDelta != 0.0f && hit)
        {
            UiEvent e     = base;
            e.type        = UiEventType::Scroll;
            e.target      = hit;
            e.scrollDelta = input.scrollDelta;
            dispatch(hit, e, &UiElement::onScroll);
        }

        // ── Teclado ─────────────────────────────────────────────────────────
        // SOLO al elemento con foco, y burbujeando. Sin foco no se emite nada:
        // ni siquiera el Tab, que sin un punto de partida no sabría hacia dónde.
        for (UiKey key : input.keys)
        {
            if (!m_focused) break;

            UiElement* target = m_focused;

            UiEvent e = base;
            e.type    = UiEventType::KeyDown;
            e.target  = target;
            e.key     = key;
            dispatch(target, e, &UiElement::onKeyDown);

            // Las teclas se ENTREGAN tal cual; el canvas solo se reserva unas
            // pocas acciones propias, y las cede si alguien consumió la tecla.
            if (e.consumed) continue;
            if (key == UiKey::Tab)         { moveFocus(input.shift ? -1 : 1); continue; }
            if (key == UiKey::Escape)      { setFocus(nullptr); continue; }

            // Flechas y Enter: es lo que hace jugable un menú con mando. Quien
            // quiera las flechas para otra cosa las consume en su handler, o
            // apaga keyboardNavigation.
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

        // ── Texto ───────────────────────────────────────────────────────────
        // DESPUES de las teclas y con la misma regla: solo al elemento con foco
        // y burbujeando. Un caracter sin destino se descarta en vez de ir al
        // primero que pase por ahi.
        //
        // Va aparte del bucle de teclas y no dentro porque las dos listas son
        // independientes: un frame puede traer solo texto (escribir), solo
        // teclas (Tab, Backspace) o las dos, y no hay forma de intercalarlas
        // sin inventarse un orden que el caller no ha dado.
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

        // ── Botones ─────────────────────────────────────────────────────────
        // Lo ÚLTIMO: los estados se derivan del hover, el foco y el botón del
        // ratón que acaban de quedar fijados arriba. Quien no llame a
        // updateInput no ve ni un cambio: buildDrawData sigue dando los mismos
        // vértices y los mismos lotes.
        tickBotones(m_root, input);
    }

    bool UiCanvas::submitFocused()
    {
        UiElement* target = m_focused;
        if (target == nullptr) return false;
        // Las mismas reglas que se le aplican al ratón: lo que no se puede
        // clicar con el cursor tampoco se activa con el mando.
        if (!target->visible || !target->enabled) return false;
        if (tragaElClick(target)) return false;

        UiEvent e{};
        e.type   = UiEventType::Click;
        e.target = target;
        e.button = UiMouseButton::Left;
        e.time   = m_lastTime;
        // El "cursor" es el centro del elemento: un handler que mire dónde le
        // han pulsado recibe un punto que cae DENTRO, no un (0,0) que estaría
        // en cualquier otro sitio de la pantalla.
        if (target->rectValid) e.mousePos = target->screenPos + target->screenSize * 0.5f;

        dispatch(target, e, &UiElement::onClick);
        return true;
    }

    void dispatchUiInput(const std::vector<UiCanvas*>& canvases, const UiInputState& input)
    {
        // ── Quién se lleva el RATÓN ─────────────────────────────────────────
        // 1) La CAPTURA manda sobre el solape. Un botón bajado y sin soltar se
        //    queda el puntero aunque el cursor se haya ido encima de otro
        //    canvas: sin esto, arrastrar un slider que asome por debajo de otro
        //    canvas corta el gesto justo al cruzar el borde, y el arrastre se
        //    pierde sin un solo aviso.
        UiCanvas* raton = nullptr;
        for (UiCanvas* c : canvases)
            if (c && c->pointerCaptured()) { raton = c; break; }

        // 2) Sin captura, gana el de MÁS ARRIBA que tenga algo bajo el cursor.
        //    `canvases` ya llega en ese orden (el último que se dibuja, primero),
        //    así que aquí no se decide nada: se recorre.
        if (!raton)
            for (UiCanvas* c : canvases)
                if (c && c->hitTest(input.mousePos)) { raton = c; break; }

        // ── Quién se lleva el TECLADO ───────────────────────────────────────
        // El FOCO, no el cursor: escribir en un campo de texto sigue llegando
        // aunque el ratón se pasee por encima de otro canvas. Si nadie tiene
        // foco van al de más arriba — hoy eso no se nota (updateInput ignora las
        // teclas sin foco), pero deja la regla completa en vez de un hueco.
        UiCanvas* teclado = nullptr;
        for (UiCanvas* c : canvases)
            if (c && c->focused()) { teclado = c; break; }
        if (!teclado)
            for (UiCanvas* c : canvases)
                if (c) { teclado = c; break; }

        // Lo que recibe el que NO tiene el puntero: el ratón FUERA y los botones
        // sueltos. Es lo que le limpia el hover (con su MouseExit y sus colores
        // de vuelta a Normal) en vez de dejárselo pegado. El reloj se conserva:
        // sus animaciones y el fundido de sus botones siguen corriendo.
        UiInputState fuera = input;
        fuera.mousePos     = uiPointerAway();
        fuera.mouseDown[0] = false;
        fuera.mouseDown[1] = false;
        fuera.mouseDown[2] = false;
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

        // El foco se mueve al CLICAR, y solo puede estar en un canvas: en cuanto
        // el que tiene el puntero coge foco, los demás lo sueltan. Sin esto se
        // quedarían dos anillos de foco a la vez y el dueño del teclado sería el
        // que decidiera el orden de la lista, no el que acaba de pulsar.
        //
        // Va DESPUÉS del bucle a propósito: el foco que hay que respetar es el
        // que deja este frame, no el del anterior. Y se mira `focused()` en vez
        // de "¿ha bajado un botón?" porque es lo mismo sin guardar estado entre
        // frames: pinchar en algo NO focusable no roba el foco, exactamente
        // igual que ya pasa dentro de un solo canvas.
        if (raton && raton->focused())
            for (UiCanvas* c : canvases)
                if (c && c != raton) c->setFocus(nullptr);
    }
}
