#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
    class ToggleComponent;

    // Lo que un interruptor tiene EN VIVO y no se serializa. Mismo papel y
    // mismos motivos que UiButtonRuntime y UiCheckboxRuntime.
    struct UiToggleRuntime
    {
        std::function<void(bool)> onValueChanged;
        ToggleComponent*          owner = nullptr;
    };

    struct UiToggleCallbackSlot
    {
        std::shared_ptr<UiToggleRuntime> ptr = std::make_shared<UiToggleRuntime>();

        UiToggleCallbackSlot() = default;
        UiToggleCallbackSlot(const UiToggleCallbackSlot&) {}
        UiToggleCallbackSlot& operator=(const UiToggleCallbackSlot&) { return *this; }
        UiToggleCallbackSlot(UiToggleCallbackSlot&&) = default;
        UiToggleCallbackSlot& operator=(UiToggleCallbackSlot&&) = default;

        bool operator==(const UiToggleCallbackSlot&) const { return true; }
    };

    // Un interruptor deslizante de la UI 2D como componente de GameObject, con
    // el MISMO contrato que el resto: SOLO DATOS. El Toggle del núcleo
    // (UiWidgets.h) es un stub SIN campos, así que el widget se monta por
    // COMPOSICIÓN: la pista es el nodo raíz (de tipo Toggle) y el mando cuelga
    // de ella.
    //
    // Guarda el MISMO dato que el Checkbox (un bool) y aun así son dos
    // componentes y no uno con un enum de estilo: lo que cambia no es el dato
    // sino los CAMPOS. La casilla tiene un padding de marca y un color de marca;
    // el interruptor tiene dos colores de pista (encendido y apagado) y el
    // tamaño del mando. Con un solo componente, la mitad de los campos del
    // inspector no harían nada según el estilo elegido.
    class ToggleComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};    // px, relativa al ancla
            glm::vec2 size{56.0f, 28.0f};      // px
            bool      visible = true;

            bool interactable = true;

            // --- Valor ---------------------------------------------------------
            bool isOn = false;

            // --- Colores --------------------------------------------------------
            // La pista NO usa UiElement::color como campo propio: lo escribe el
            // sync con el color del estado, igual que hace el canvas con el
            // Button. Tener los dos sería tener un campo que el primer volcado
            // pisa y que parece no hacer nada.
            glm::vec4 offColor{0.3f, 0.3f, 0.3f, 1.0f};
            glm::vec4 onColor{0.25f, 0.7f, 1.0f, 1.0f};
            glm::vec4 knobColor{1.0f, 1.0f, 1.0f, 1.0f};

            // --- Mando ----------------------------------------------------------
            // Lado del mando en px. Se acota a lo que quede entre paddings: uno
            // más grande que la pista asomaría por el borde sin que nada lo
            // dijera.
            float knobSize    = 20.0f;
            float knobPadding = 4.0f;

            // --- Sprites --------------------------------------------------------
            std::string atlasPath;
            std::string backgroundSprite;
            std::string knobSprite;

            // --- Runtime (no se serializa) --------------------------------------
            UiToggleCallbackSlot callbacks;

            // El color de la pista según el estado. Aquí y no en el sync para
            // poder probarlo sin canvas ni GPU.
            glm::vec4 trackColor() const { return isOn ? onColor : offColor; }

            // Rect del MANDO en coordenadas de la pista, pegado a un extremo o al
            // otro y siempre dentro del padding.
            void knobRect(glm::vec2& outPos, glm::vec2& outSize) const
            {
                const float p = std::max(knobPadding, 0.0f);
                // Lo que queda de pista entre los dos paddings. Un padding que no
                // cabe da cero, nunca negativo.
                const float dispW = std::max(size.x - 2.0f * p, 0.0f);
                const float dispH = std::max(size.y - 2.0f * p, 0.0f);

                const float lado = std::max(knobSize, 0.0f);
                const float w = std::min(lado, dispW);
                const float h = std::min(lado, dispH);

                const float x = isOn ? (p + dispW - w) : p;
                outPos  = glm::vec2(x, p);
                outSize = glm::vec2(w, h);
            }

            // Vuelca el rect y la pista en el nodo vivo. NO toca `atlas` (es un
            // puntero a GPU: lo resuelve el sync).
            void applyTo(Toggle& t) const
            {
                t.anchorMin = anchorMin;
                t.anchorMax = anchorMax;
                t.pivot     = pivot;
                t.position  = position;
                t.size      = size;
                t.color     = trackColor();
                t.visible   = visible;
                t.sprite    = backgroundSprite;
                t.raycastTarget = true;
            }

            void applyToKnob(UiElement& k) const
            {
                glm::vec2 pos{0.0f}, sz{0.0f};
                knobRect(pos, sz);

                k.anchorMin = glm::vec2(0.0f);
                k.anchorMax = glm::vec2(0.0f);
                k.pivot     = glm::vec2(0.0f);
                k.position  = pos;
                k.size      = sz;
                k.color     = knobColor;
                k.sprite    = knobSprite;
                k.visible   = true;
                k.drawable  = (sz.x > 0.0f && sz.y > 0.0f);
                // El mando no recibe el ratón: el hit test devuelve el nodo MÁS
                // PROFUNDO y se comería el click de la pista.
                k.raycastTarget = false;
            }

            // El sync lo usa para saber si hay algo que volcar.
            bool operator==(const ToggleComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       visible == o.visible && interactable == o.interactable &&
                       isOn == o.isOn &&
                       offColor == o.offColor && onColor == o.onColor &&
                       knobColor == o.knobColor &&
                       knobSize == o.knobSize && knobPadding == o.knobPadding &&
                       atlasPath == o.atlasPath && backgroundSprite == o.backgroundSprite &&
                       knobSprite == o.knobSprite;
            }
            bool operator!=(const ToggleComponent& o) const { return !(*this == o); }
    };

    // Nombre del nodo vivo de un Toggle dentro del canvas. Prefijo DISTINTO al
    // de los demás, por lo de siempre.
    inline std::string uiToggleNodeName(uint64_t ownerId)
    {
        return "tgl:" + std::to_string(ownerId);
    }

    // Inversa de uiToggleNodeName. Devuelve 0 si el nombre no es de un
    // interruptor. El corte por '/' hace que el nodo del mando devuelva también
    // a su dueño.
    inline uint64_t uiToggleOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("tgl:", 0) != 0) return 0;
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
