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
    class SliderComponent;

    // Eje y sentido del recorrido. Enum PROPIO del componente y no el
    // UiFillDirection de UiWidgets.h, por lo mismo que la ProgressBar tiene el
    // suyo: aquel solo distingue Horizontal/Vertical, y aquí hace falta también
    // el SENTIDO (un slider de graves que crece hacia la izquierda no es el
    // mismo que uno que crece hacia la derecha).
    enum class UiSliderDirection
    {
        LeftToRight,
        RightToLeft,
        BottomToTop,
        TopToBottom
    };

    // Lo que un slider tiene EN VIVO y no se serializa. Mismo papel y mismos
    // motivos que UiButtonRuntime (ButtonComponent.h): el nodo del canvas lo
    // destruye clearChildren() en cada reconstrucción, así que el dueño del
    // callback es el COMPONENTE y el nodo solo guarda un weak_ptr a esto.
    //
    // `owner` es lo que el Button no necesita: el arrastre tiene que escribir el
    // valor EN EL COMPONENTE (es lo que se serializa y lo que lee el editor), no
    // en el nodo. Lo pone el sync en cada volcado; si el componente muere, muere
    // el runtime con él y el weak_ptr del nodo ya no resuelve, así que el
    // puntero nunca se queda colgando.
    struct UiSliderRuntime
    {
        std::function<void(float)> onValueChanged;
        SliderComponent*           owner = nullptr;
    };

    // El hueco del runtime dentro del componente, con las MISMAS dos reglas que
    // UiCallbackSlot del Button: copiar un componente estrena callbacks (un clon
    // no dispara el del original) y compararlos ignora este campo (el sync usa
    // operator== para saber si hay que volcar, y un callback no es un dato que
    // volcar).
    struct UiSliderCallbackSlot
    {
        std::shared_ptr<UiSliderRuntime> ptr = std::make_shared<UiSliderRuntime>();

        UiSliderCallbackSlot() = default;
        UiSliderCallbackSlot(const UiSliderCallbackSlot&) {}
        UiSliderCallbackSlot& operator=(const UiSliderCallbackSlot&) { return *this; }
        UiSliderCallbackSlot(UiSliderCallbackSlot&&) = default;
        UiSliderCallbackSlot& operator=(UiSliderCallbackSlot&&) = default;

        bool operator==(const UiSliderCallbackSlot&) const { return true; }
    };

    // Un slider de la UI 2D como componente de GameObject, con el MISMO contrato
    // que el resto: SOLO DATOS. El árbol vivo lo tiene el Renderer y lo monta
    // syncUiWidgets() cada frame.
    //
    // El Slider del núcleo (UiWidgets.h) es un stub SIN campos, así que el
    // widget se monta por COMPOSICIÓN igual que la ProgressBar: la pista es el
    // nodo raíz (de tipo Slider) y de ella cuelgan el relleno y el asa. Aquí no
    // se toca ni una línea del core de UI.
    //
    // La diferencia con la barra es el INPUT: el sync engancha handlers a la
    // pista que escriben `value` aquí. Por eso el sync recibe este componente
    // por puntero NO const, al revés que los que solo se dibujan.
    class SliderComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};     // px, relativa al ancla
            glm::vec2 size{200.0f, 20.0f};      // px
            glm::vec4 color{0.2f, 0.2f, 0.2f, 1.0f};   // color de la PISTA
            bool      visible = true;

            // A false se dibuja igual pero no se deja mover: es el modo "solo
            // lectura" de un HUD que enseña un valor sin dejarlo tocar.
            bool interactable = true;

            // --- Valor ---------------------------------------------------------
            // Sin clamp AQUÍ a propósito (mismo criterio que la ProgressBar): el
            // componente no interpreta nada. Quien normaliza es normalizedValue,
            // y quien redondea al escribir es valueFromNormalized.
            float value    = 0.0f;
            float minValue = 0.0f;
            float maxValue = 1.0f;

            // El valor que se ESCRIBE se redondea, no solo el que se dibuja: si
            // el redondeo fuera del dibujado, el componente guardaría 3,7 y un
            // script leería 3,7 con el asa enseñando 4.
            bool wholeNumbers = false;

            UiSliderDirection direction = UiSliderDirection::LeftToRight;

            // --- Relleno y asa -------------------------------------------------
            glm::vec4 fillColor{0.25f, 0.7f, 1.0f, 1.0f};
            glm::vec4 handleColor{1.0f, 1.0f, 1.0f, 1.0f};

            // Largo del asa EN EL EJE del recorrido, en px del rect. Se descuenta
            // del recorrido para que el asa no se salga por las puntas. A 0 el
            // recorrido es el rect entero.
            float handleSize = 20.0f;

            // --- Sprites -------------------------------------------------------
            // UN atlas para las tres partes y tres NOMBRES de sub-rect dentro de
            // él (los registra el sidecar <atlas>.sprites.json). Una sola carga
            // de atlas en vez de las tres rutas sueltas de la ProgressBar.
            std::string atlasPath;
            std::string backgroundSprite;
            std::string fillSprite;
            std::string handleSprite;

            // --- Runtime (no se serializa) --------------------------------------
            UiSliderCallbackSlot callbacks;

            bool isVertical() const
            {
                return direction == UiSliderDirection::BottomToTop ||
                       direction == UiSliderDirection::TopToBottom;
            }

            // Sentido invertido respecto al crecimiento natural del eje. La Y del
            // canvas crece hacia ABAJO, así que "de abajo a arriba" es el
            // invertido en vertical.
            bool isReversed() const
            {
                return direction == UiSliderDirection::RightToLeft ||
                       direction == UiSliderDirection::BottomToTop;
            }

            // Fracción del rect que ocupa el asa en el eje del recorrido, acotada
            // a [0,1]. Un rect degenerado da 0.
            float handleFraction() const
            {
                const float largo = isVertical() ? size.y : size.x;
                if (!(largo > 0.0f)) return 0.0f;
                return std::clamp(handleSize / largo, 0.0f, 1.0f);
            }

            // El valor en [0,1]. Un rango degenerado (max <= min) da 0: no hay
            // forma de repartir un intervalo vacío.
            float normalizedValue() const
            {
                if (!(maxValue > minValue)) return 0.0f;
                return std::clamp((value - minValue) / (maxValue - minValue), 0.0f, 1.0f);
            }

            // El camino de vuelta: de [0,1] al valor, ya con el rango y con el
            // redondeo de wholeNumbers aplicados.
            float valueFromNormalized(float t) const
            {
                if (!(maxValue > minValue)) return minValue;
                const float clamped = std::clamp(t, 0.0f, 1.0f);
                float v = minValue + clamped * (maxValue - minValue);
                if (wholeNumbers) v = std::round(v);
                return std::clamp(v, minValue, maxValue);
            }

            // De un punto EN COORDENADAS DEL RECT (px desde su esquina superior
            // izquierda) al valor normalizado. rectSize entra aparte y no se lee
            // de `size` porque el input trabaja en píxeles de PANTALLA, que
            // llevan aplicada la escala del canvas; la fracción del asa sí es
            // unitless y sirve igual en los dos espacios.
            float normalizedFromLocal(glm::vec2 local, glm::vec2 rectSize) const
            {
                const float largo = isVertical() ? rectSize.y : rectSize.x;
                if (!(largo > 0.0f)) return 0.0f;

                const float frac = handleFraction();
                // Recorrido ÚTIL: el asa se mueve entre su borde izquierdo a 0 y
                // su borde derecho al final, así que el centro recorre
                // (largo - asa) y arranca a media asa del principio.
                const float util = largo * (1.0f - frac);
                const float p    = (isVertical() ? local.y : local.x) - largo * frac * 0.5f;

                // Asa tan grande como la pista: no hay recorrido, y dividir daría
                // un infinito. Cualquier punto vale lo mismo.
                float t = (util > 0.0f) ? (p / util) : 0.0f;
                t = std::clamp(t, 0.0f, 1.0f);
                return isReversed() ? (1.0f - t) : t;
            }

            // Rect del RELLENO en coordenadas de la pista. Llega hasta el centro
            // del asa, que es donde marca el valor.
            void fillRect(glm::vec2& outPos, glm::vec2& outSize) const
            {
                const float t    = normalizedValue();
                const float frac = handleFraction();
                // Mismo recorrido que el asa más media asa: el borde del relleno
                // acaba justo en el centro del asa en cualquier posición.
                const float f = t * (1.0f - frac) + frac * 0.5f;

                if (isVertical())
                {
                    const float h = size.y * f;
                    outSize = glm::vec2(size.x, h);
                    // BottomToTop crece hacia arriba: el relleno se pega abajo.
                    outPos  = glm::vec2(0.0f, isReversed() ? (size.y - h) : 0.0f);
                }
                else
                {
                    const float wpx = size.x * f;
                    outSize = glm::vec2(wpx, size.y);
                    outPos  = glm::vec2(isReversed() ? (size.x - wpx) : 0.0f, 0.0f);
                }
            }

            // Rect del ASA en coordenadas de la pista, ya acotado para que no se
            // salga por ninguna punta.
            void handleRect(glm::vec2& outPos, glm::vec2& outSize) const
            {
                const float t    = normalizedValue();
                const float frac = handleFraction();
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

            // Vuelca el rect y la pista en el nodo vivo. NO toca `atlas` (es un
            // puntero a GPU: lo resuelve el sync).
            void applyTo(Slider& s) const
            {
                s.anchorMin = anchorMin;
                s.anchorMax = anchorMax;
                s.pivot     = pivot;
                s.position  = position;
                s.size      = size;
                s.color     = color;
                s.visible   = visible;
                s.sprite    = backgroundSprite;
                // La pista recibe el ratón SIEMPRE, interactable o no: el gate de
                // interactable está en el handler, y quitarle el raycast aquí
                // haría que el clic se lo comiera lo que hubiera detrás.
                s.raycastTarget = true;
            }

            void applyToFill(UiElement& f) const
            {
                glm::vec2 pos{0.0f}, sz{0.0f};
                fillRect(pos, sz);

                f.anchorMin = glm::vec2(0.0f);
                f.anchorMax = glm::vec2(0.0f);
                f.pivot     = glm::vec2(0.0f);
                f.position  = pos;
                f.size      = sz;
                f.color     = fillColor;
                f.sprite    = fillSprite;
                f.visible   = true;
                // A rect degenerado no se emite el quad: uno de área nula con
                // UVs completas es basura en el buffer.
                f.drawable  = (sz.x > 0.0f && sz.y > 0.0f);
                // Ni el relleno ni el asa reciben el ratón: el hit test devuelve
                // el nodo MÁS PROFUNDO, así que un hijo raycastable se comería el
                // arrastre que empieza encima de él y la pista no se enteraría.
                f.raycastTarget = false;
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

            // El sync lo usa para saber si hay algo que volcar: sin esto habría
            // que ensuciar el nodo TODOS los frames, que es justo lo que la
            // caché de vértices del canvas existe para evitar.
            bool operator==(const SliderComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       interactable == o.interactable &&
                       value == o.value && minValue == o.minValue && maxValue == o.maxValue &&
                       wholeNumbers == o.wholeNumbers && direction == o.direction &&
                       fillColor == o.fillColor && handleColor == o.handleColor &&
                       handleSize == o.handleSize &&
                       atlasPath == o.atlasPath && backgroundSprite == o.backgroundSprite &&
                       fillSprite == o.fillSprite && handleSprite == o.handleSprite;
            }
            bool operator!=(const SliderComponent& o) const { return !(*this == o); }
    };

    // Nombre del nodo vivo de un Slider dentro del canvas. Prefijo DISTINTO al
    // de los demás por lo mismo que aquellos entre sí: un GameObject puede
    // llevar varios componentes de UI a la vez, y dos nodos hermanos con el
    // mismo nombre harían que el gizmo y el picking cogieran el que no toca.
    inline std::string uiSliderNodeName(uint64_t ownerId)
    {
        return "sld:" + std::to_string(ownerId);
    }

    // Inversa de uiSliderNodeName. Devuelve 0 si el nombre no es de un slider: 0
    // no es un id válido de GameObject. El corte por '/' hace que los nodos del
    // relleno y del asa devuelvan también a su dueño.
    inline uint64_t uiSliderOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("sld:", 0) != 0) return 0;
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
