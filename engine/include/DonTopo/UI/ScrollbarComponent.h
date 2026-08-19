#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
    class ScrollbarComponent;

    // Eje y sentido del recorrido. Enum PROPIO del componente, como el de la
    // ProgressBar y el del Slider: cada widget declara el suyo en vez de
    // compartir uno, para que un cambio en la barra no arrastre al slider.
    enum class UiScrollbarDirection
    {
        LeftToRight,
        RightToLeft,
        TopToBottom,
        BottomToTop
    };

    // Lo que una barra tiene EN VIVO y no se serializa. Mismo papel y mismos
    // motivos que UiButtonRuntime y UiSliderRuntime.
    struct UiScrollbarRuntime
    {
        std::function<void(float)> onValueChanged;
        ScrollbarComponent*        owner = nullptr;
    };

    struct UiScrollbarCallbackSlot
    {
        std::shared_ptr<UiScrollbarRuntime> ptr = std::make_shared<UiScrollbarRuntime>();

        UiScrollbarCallbackSlot() = default;
        UiScrollbarCallbackSlot(const UiScrollbarCallbackSlot&) {}
        UiScrollbarCallbackSlot& operator=(const UiScrollbarCallbackSlot&) { return *this; }
        UiScrollbarCallbackSlot(UiScrollbarCallbackSlot&&) = default;
        UiScrollbarCallbackSlot& operator=(UiScrollbarCallbackSlot&&) = default;

        bool operator==(const UiScrollbarCallbackSlot&) const { return true; }
    };

    // Una barra de scroll de la UI 2D como componente de GameObject, con el
    // MISMO contrato que el resto: SOLO DATOS. El Scrollbar del núcleo
    // (UiWidgets.h) es un stub SIN campos, así que el widget se monta por
    // COMPOSICIÓN: el canal es el nodo raíz (de tipo Scrollbar) y el asa cuelga
    // de él.
    //
    // Se parece al Slider pero NO es el mismo widget: aquí el asa tiene tamaño
    // VARIABLE (la fracción del contenido que se ve) y el valor va siempre en
    // 0..1 — no hay rango propio porque quien lo interpreta es lo que se
    // desplaza, no la barra.
    class ScrollbarComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};    // px, relativa al ancla
            glm::vec2 size{20.0f, 200.0f};     // px
            glm::vec4 color{0.15f, 0.15f, 0.15f, 1.0f};   // color del CANAL
            bool      visible = true;

            bool interactable = true;

            // --- Valor ---------------------------------------------------------
            // Siempre 0..1. El clamp lo hace quien escribe (snapValue y el
            // handler del sync), no el campo: el componente no interpreta nada,
            // igual que el resto.
            float value = 0.0f;

            // Fracción del canal que ocupa el asa: 1 = el contenido cabe entero
            // (no hay nada que desplazar), 0 = un asa de grosor nulo.
            float handleFraction = 0.25f;

            UiScrollbarDirection direction = UiScrollbarDirection::TopToBottom;

            // Paradas discretas. 0 y 1 = continuo: enganchar a una sola parada
            // dejaría la barra muerta en un sitio. Con N >= 2 hay N paradas
            // repartidas por todo el recorrido (0, 1/(N-1), ..., 1), como Unity.
            uint32_t numberOfSteps = 0;

            // --- Asa ------------------------------------------------------------
            glm::vec4 handleColor{0.6f, 0.6f, 0.6f, 1.0f};

            // Cuánto mueve la rueda del ratón por muesca, en fracción del
            // recorrido. Campo y no constante escondida: una lista larga y un
            // selector de tres opciones no quieren el mismo paso.
            float scrollStep = 0.1f;

            // --- Sprites --------------------------------------------------------
            std::string atlasPath;
            std::string backgroundSprite;
            std::string handleSprite;

            // --- Runtime (no se serializa) --------------------------------------
            UiScrollbarCallbackSlot callbacks;

            bool isVertical() const
            {
                return direction == UiScrollbarDirection::TopToBottom ||
                       direction == UiScrollbarDirection::BottomToTop;
            }

            // Sentido invertido respecto al crecimiento natural del eje. La Y del
            // canvas crece hacia ABAJO, así que TopToBottom (0 arriba) es el
            // natural en vertical y BottomToTop el invertido.
            bool isReversed() const
            {
                return direction == UiScrollbarDirection::RightToLeft ||
                       direction == UiScrollbarDirection::BottomToTop;
            }

            float clampedFraction() const { return std::clamp(handleFraction, 0.0f, 1.0f); }

            // Engancha un valor a las paradas discretas y lo acota a [0,1].
            float snapValue(float v) const
            {
                const float c = std::clamp(v, 0.0f, 1.0f);
                if (numberOfSteps < 2u) return c;
                const float pasos = (float)(numberOfSteps - 1u);
                return std::round(c * pasos) / pasos;
            }

            // De un punto EN COORDENADAS DEL RECT (px desde su esquina superior
            // izquierda) al valor. rectSize entra aparte y no se lee de `size`
            // porque el input trabaja en píxeles de PANTALLA, que llevan aplicada
            // la escala del canvas; la fracción del asa sí es unitless.
            float valueFromLocal(glm::vec2 local, glm::vec2 rectSize) const
            {
                const float largo = isVertical() ? rectSize.y : rectSize.x;
                if (!(largo > 0.0f)) return 0.0f;

                const float frac = clampedFraction();
                const float util = largo * (1.0f - frac);
                const float p    = (isVertical() ? local.y : local.x) - largo * frac * 0.5f;

                // Asa tan grande como el canal: no hay recorrido y dividir daría
                // un infinito.
                float t = (util > 0.0f) ? (p / util) : 0.0f;
                t = std::clamp(t, 0.0f, 1.0f);
                return snapValue(isReversed() ? (1.0f - t) : t);
            }

            // Rect del ASA en coordenadas del canal, ya acotado para que no se
            // salga por ninguna punta.
            void handleRect(glm::vec2& outPos, glm::vec2& outSize) const
            {
                const float frac = clampedFraction();
                const float t    = std::clamp(value, 0.0f, 1.0f);
                const float tt   = isReversed() ? (1.0f - t) : t;

                if (isVertical())
                {
                    const float h = size.y * frac;
                    outSize = glm::vec2(size.x, h);
                    outPos  = glm::vec2(0.0f, (size.y - h) * tt);
                }
                else
                {
                    const float wpx = size.x * frac;
                    outSize = glm::vec2(wpx, size.y);
                    outPos  = glm::vec2((size.x - wpx) * tt, 0.0f);
                }
            }

            // Vuelca el rect y el canal en el nodo vivo. NO toca `atlas` (es un
            // puntero a GPU: lo resuelve el sync).
            void applyTo(Scrollbar& s) const
            {
                s.anchorMin = anchorMin;
                s.anchorMax = anchorMax;
                s.pivot     = pivot;
                s.position  = position;
                s.size      = size;
                s.color     = color;
                s.visible   = visible;
                s.sprite    = backgroundSprite;
                s.raycastTarget = true;
            }

            void applyToHandle(UiElement& h) const
            {
                glm::vec2 pos{0.0f}, sz{0.0f};
                handleRect(pos, sz);

                h.anchorMin = glm::vec2(0.0f);
                h.anchorMax = glm::vec2(0.0f);
                h.pivot     = glm::vec2(0.0f);
                h.position  = pos;
                h.size      = sz;
                h.color     = handleColor;
                h.sprite    = handleSprite;
                h.visible   = true;
                h.drawable  = (sz.x > 0.0f && sz.y > 0.0f);
                h.raycastTarget = false;
            }

            // El sync lo usa para saber si hay algo que volcar.
            bool operator==(const ScrollbarComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       interactable == o.interactable &&
                       value == o.value && handleFraction == o.handleFraction &&
                       direction == o.direction && numberOfSteps == o.numberOfSteps &&
                       handleColor == o.handleColor && scrollStep == o.scrollStep &&
                       atlasPath == o.atlasPath && backgroundSprite == o.backgroundSprite &&
                       handleSprite == o.handleSprite;
            }
            bool operator!=(const ScrollbarComponent& o) const { return !(*this == o); }
    };

    // Nombre del nodo vivo de un Scrollbar dentro del canvas. Prefijo DISTINTO
    // al de los demás, por lo de siempre.
    inline std::string uiScrollbarNodeName(uint64_t ownerId)
    {
        return "scr:" + std::to_string(ownerId);
    }

    // Inversa de uiScrollbarNodeName. Devuelve 0 si el nombre no es de una barra.
    // El corte por '/' hace que el nodo del asa devuelva también a su dueño.
    inline uint64_t uiScrollbarOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("scr:", 0) != 0) return 0;
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
