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
    class CheckboxComponent;

    // Lo que una casilla tiene EN VIVO y no se serializa. Mismo papel y mismos
    // motivos que UiButtonRuntime (ButtonComponent.h) y que UiSliderRuntime: el
    // nodo del canvas lo destruye clearChildren() en cada reconstrucción, así
    // que el dueño del callback es el COMPONENTE y el nodo solo guarda un
    // weak_ptr a esto.
    //
    // `owner` es por donde el click escribe `isOn` EN EL COMPONENTE, que es lo
    // que se serializa y lo que lee el editor. Lo pone el sync en cada volcado;
    // si el componente muere, muere el runtime con él y el weak_ptr del nodo ya
    // no resuelve, así que el puntero nunca se queda colgando.
    struct UiCheckboxRuntime
    {
        std::function<void(bool)> onValueChanged;
        CheckboxComponent*        owner = nullptr;
    };

    // El hueco del runtime dentro del componente, con las MISMAS dos reglas que
    // UiCallbackSlot del Button: copiar un componente estrena callbacks y
    // compararlos ignora este campo.
    struct UiCheckboxCallbackSlot
    {
        std::shared_ptr<UiCheckboxRuntime> ptr = std::make_shared<UiCheckboxRuntime>();

        UiCheckboxCallbackSlot() = default;
        UiCheckboxCallbackSlot(const UiCheckboxCallbackSlot&) {}
        UiCheckboxCallbackSlot& operator=(const UiCheckboxCallbackSlot&) { return *this; }
        UiCheckboxCallbackSlot(UiCheckboxCallbackSlot&&) = default;
        UiCheckboxCallbackSlot& operator=(UiCheckboxCallbackSlot&&) = default;

        bool operator==(const UiCheckboxCallbackSlot&) const { return true; }
    };

    // Una casilla de verificación de la UI 2D como componente de GameObject, con
    // el MISMO contrato que el resto: SOLO DATOS.
    //
    // El Checkbox del núcleo (UiWidgets.h) es un stub SIN campos, así que el
    // widget se monta por COMPOSICIÓN: la caja es el nodo raíz (de tipo
    // Checkbox) y la marca cuelga de ella. Aquí no se toca ni una línea del core
    // de UI.
    //
    // No lleva etiqueta de texto a propósito: el Text es su propio componente y
    // cabe en el mismo GameObject (son nodos hermanos con prefijos distintos),
    // así que meter aquí una copia de los campos del texto sería mantener dos.
    class CheckboxComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};    // px, relativa al ancla
            glm::vec2 size{24.0f, 24.0f};      // px
            glm::vec4 color{0.2f, 0.2f, 0.2f, 1.0f};   // color de la CAJA
            bool      visible = true;

            // A false se dibuja igual pero el click no la mueve.
            bool interactable = true;

            // --- Valor ---------------------------------------------------------
            bool isOn = false;

            // --- Marca ----------------------------------------------------------
            glm::vec4 checkColor{1.0f, 1.0f, 1.0f, 1.0f};

            // Píxeles que la marca se mete hacia DENTRO de la caja por los cuatro
            // lados. Un padding que no cabe deja la marca a cero, que es lo peor
            // que puede pasar; nunca un rect del revés.
            float checkPadding = 4.0f;

            // --- Sprites --------------------------------------------------------
            // UN atlas y dos NOMBRES de sub-rect dentro de él (los registra el
            // sidecar <atlas>.sprites.json). Vacíos = quads de color plano.
            std::string atlasPath;
            std::string backgroundSprite;
            std::string checkmarkSprite;

            // --- Runtime (no se serializa) --------------------------------------
            UiCheckboxCallbackSlot callbacks;

            // Rect de la MARCA en coordenadas de la caja.
            void checkRect(glm::vec2& outPos, glm::vec2& outSize) const
            {
                const float p = std::max(checkPadding, 0.0f);
                outPos  = glm::vec2(std::min(p, size.x * 0.5f), std::min(p, size.y * 0.5f));
                outSize = glm::vec2(std::max(size.x - 2.0f * p, 0.0f),
                                    std::max(size.y - 2.0f * p, 0.0f));
            }

            // Vuelca el rect y la caja en el nodo vivo. NO toca `atlas` (es un
            // puntero a GPU: lo resuelve el sync).
            void applyTo(Checkbox& c) const
            {
                c.anchorMin = anchorMin;
                c.anchorMax = anchorMax;
                c.pivot     = pivot;
                c.position  = position;
                c.size      = size;
                c.color     = color;
                c.visible   = visible;
                c.sprite    = backgroundSprite;
                // La caja recibe el ratón SIEMPRE, interactable o no: el gate
                // está en el handler, y quitarle el raycast aquí haría que el
                // click se lo comiera lo que hubiera detrás.
                c.raycastTarget = true;
            }

            void applyToCheck(UiElement& m) const
            {
                glm::vec2 pos{0.0f}, sz{0.0f};
                checkRect(pos, sz);

                m.anchorMin = glm::vec2(0.0f);
                m.anchorMax = glm::vec2(0.0f);
                m.pivot     = glm::vec2(0.0f);
                m.position  = pos;
                m.size      = sz;
                m.color     = checkColor;
                m.sprite    = checkmarkSprite;
                m.visible   = true;
                // Apagada, el nodo SIGUE EXISTIENDO pero no pinta: si apareciera
                // y desapareciera cambiaría la forma del subárbol y habría que
                // reconstruir la raíz del canvas en cada click.
                m.drawable  = isOn && sz.x > 0.0f && sz.y > 0.0f;
                // Y no recibe el ratón: el hit test devuelve el nodo MÁS
                // PROFUNDO, así que la marca se comería el click de la caja.
                m.raycastTarget = false;
            }

            // El sync lo usa para saber si hay algo que volcar: sin esto habría
            // que ensuciar el nodo TODOS los frames, que es justo lo que la
            // caché de vértices del canvas existe para evitar.
            bool operator==(const CheckboxComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       interactable == o.interactable && isOn == o.isOn &&
                       checkColor == o.checkColor && checkPadding == o.checkPadding &&
                       atlasPath == o.atlasPath && backgroundSprite == o.backgroundSprite &&
                       checkmarkSprite == o.checkmarkSprite;
            }
            bool operator!=(const CheckboxComponent& o) const { return !(*this == o); }
    };

    // Nombre del nodo vivo de un Checkbox dentro del canvas. Prefijo DISTINTO al
    // de los demás por lo mismo que aquellos entre sí: un GameObject puede
    // llevar varios componentes de UI a la vez, y dos nodos hermanos con el
    // mismo nombre harían que el gizmo y el picking cogieran el que no toca.
    inline std::string uiCheckboxNodeName(uint64_t ownerId)
    {
        return "chk:" + std::to_string(ownerId);
    }

    // Inversa de uiCheckboxNodeName. Devuelve 0 si el nombre no es de una
    // casilla: 0 no es un id válido de GameObject. El corte por '/' hace que el
    // nodo de la marca devuelva también a su dueño.
    inline uint64_t uiCheckboxOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("chk:", 0) != 0) return 0;
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
