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
    class ScrollViewComponent;

    // Lo que una vista tiene EN VIVO y no se serializa. Mismo papel y mismos
    // motivos que UiButtonRuntime. El callback lleva DOS floats (la posición
    // normalizada de los dos ejes) y no un vec2 porque en Lua no hay vec2: los
    // vectores viajan como varios valores de vuelta, igual que los Get* del
    // resto de componentes.
    struct UiScrollViewRuntime
    {
        std::function<void(float, float)> onValueChanged;
        ScrollViewComponent*              owner = nullptr;
    };

    struct UiScrollViewCallbackSlot
    {
        std::shared_ptr<UiScrollViewRuntime> ptr = std::make_shared<UiScrollViewRuntime>();

        UiScrollViewCallbackSlot() = default;
        UiScrollViewCallbackSlot(const UiScrollViewCallbackSlot&) {}
        UiScrollViewCallbackSlot& operator=(const UiScrollViewCallbackSlot&) { return *this; }
        UiScrollViewCallbackSlot(UiScrollViewCallbackSlot&&) = default;
        UiScrollViewCallbackSlot& operator=(UiScrollViewCallbackSlot&&) = default;

        bool operator==(const UiScrollViewCallbackSlot&) const { return true; }
    };

    // Una vista desplazable de la UI 2D como componente de GameObject, con el
    // MISMO contrato que el resto: SOLO DATOS. El ScrollView del núcleo
    // (UiWidgets.h) es un stub SIN campos, así que el widget se monta por
    // COMPOSICIÓN: el viewport es el nodo raíz (de tipo ScrollView, con
    // clipChildren) y de él cuelga el contenido, que es el que se mueve.
    //
    // Es el único cuyo nodo PRINCIPAL no es el que recibe el ratón: los hijos de
    // la escena cuelgan del CONTENIDO, no del viewport. Si colgaran del viewport,
    // desplazarse no los arrastraría y el scroll no serviría de nada.
    //
    // NO tiene una referencia a un Scrollbar. Enlazarlos es una línea de script
    // (`barra:OnValueChanged(...)`), y una referencia entre componentes de la
    // escena obligaría a serializar el id del otro GameObject y a mantenerlo vivo
    // en el clone, el undo y el borrado — mucho aparato para lo que resuelve.
    class ScrollViewComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};      // px, relativa al ancla
            glm::vec2 size{200.0f, 200.0f};      // px: el VIEWPORT
            glm::vec4 color{0.1f, 0.1f, 0.1f, 1.0f};
            bool      visible = true;

            // --- Ejes -----------------------------------------------------------
            // Un eje apagado no se mueve aunque el contenido sea más grande.
            bool horizontal = false;
            bool vertical   = true;

            // --- Contenido ------------------------------------------------------
            // Tamaño del área desplazable. Es un CAMPO y no algo medido de los
            // hijos a propósito: medir el subárbol cada frame para decidir cuánto
            // se puede desplazar acopla el scroll al layout y hace que el
            // recorrido cambie solo cuando alguien mueve un hijo.
            glm::vec2 contentSize{200.0f, 400.0f};

            // 0 = principio, 1 = final, por eje. Sin clamp AQUÍ (el componente no
            // interpreta nada); quien acota es la rueda y contentOffset().
            glm::vec2 normalizedPosition{0.0f, 0.0f};

            // Píxeles que mueve la rueda por muesca. En píxeles y no en fracción
            // porque una lista de 50 filas y otra de 5 quieren el mismo recorrido
            // por muesca, no la misma fracción.
            float scrollSensitivity = 40.0f;

            // --- Sprites --------------------------------------------------------
            std::string atlasPath;
            std::string backgroundSprite;

            // --- Runtime (no se serializa) --------------------------------------
            UiScrollViewCallbackSlot callbacks;

            // Cuánto se puede desplazar por eje, en píxeles. Un contenido más
            // pequeño que el viewport da 0: no hay nada que desplazar.
            glm::vec2 scrollRange() const
            {
                return glm::vec2(horizontal ? std::max(contentSize.x - size.x, 0.0f) : 0.0f,
                                 vertical   ? std::max(contentSize.y - size.y, 0.0f) : 0.0f);
            }

            // Desplazamiento del contenido dentro del viewport, en coordenadas
            // del viewport. Siempre <= 0: empujar el contenido hacia dentro
            // dejaría un hueco arriba que nadie ha pedido.
            glm::vec2 contentOffset() const
            {
                const glm::vec2 r = scrollRange();
                return glm::vec2(-r.x * std::clamp(normalizedPosition.x, 0.0f, 1.0f),
                                 -r.y * std::clamp(normalizedPosition.y, 0.0f, 1.0f));
            }

            // Vuelca el rect y el viewport en el nodo vivo. NO toca `atlas` (es un
            // puntero a GPU: lo resuelve el sync).
            void applyTo(ScrollView& v) const
            {
                v.anchorMin = anchorMin;
                v.anchorMax = anchorMax;
                v.pivot     = pivot;
                v.position  = position;
                v.size      = size;
                v.color     = color;
                v.visible   = visible;
                v.sprite    = backgroundSprite;
                // Recorta a TODO lo que cuelgue: es lo que hace que el contenido
                // no se salga por los bordes al desplazarse.
                v.clipChildren  = true;
                // Y sí recibe el ratón: la rueda es suya.
                v.raycastTarget = true;
            }

            void applyToContent(UiElement& c) const
            {
                const glm::vec2 off = contentOffset();

                c.anchorMin = glm::vec2(0.0f);
                c.anchorMax = glm::vec2(0.0f);
                c.pivot     = glm::vec2(0.0f);
                c.position  = off;
                c.size      = contentSize;
                c.visible   = true;
                // Agrupa y mueve, pero no pinta: sin esto saldría un quad de
                // color liso TAPANDO a todo lo que lleve dentro.
                c.drawable  = false;
                // Y no recibe el ratón: la rueda la quiere el viewport, y un
                // contenedor que no pinta comiéndose los clics no habría forma de
                // verlo.
                c.raycastTarget = false;
            }

            // El sync lo usa para saber si hay algo que volcar.
            bool operator==(const ScrollViewComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       horizontal == o.horizontal && vertical == o.vertical &&
                       contentSize == o.contentSize &&
                       normalizedPosition == o.normalizedPosition &&
                       scrollSensitivity == o.scrollSensitivity &&
                       atlasPath == o.atlasPath && backgroundSprite == o.backgroundSprite;
            }
            bool operator!=(const ScrollViewComponent& o) const { return !(*this == o); }
    };

    // Nombre del nodo vivo de un ScrollView dentro del canvas. "scv:" y no
    // "scr:", que es del Scrollbar: comparten las tres primeras letras y son dos
    // widgets distintos, así que confundirlos haría que el picking devolviera el
    // GameObject que no toca.
    inline std::string uiScrollViewNodeName(uint64_t ownerId)
    {
        return "scv:" + std::to_string(ownerId);
    }

    // Inversa de uiScrollViewNodeName. Devuelve 0 si el nombre no es de una
    // vista. El corte por '/' hace que el contenido devuelva también a su dueño.
    inline uint64_t uiScrollViewOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("scv:", 0) != 0) return 0;
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
