#pragma once
#include <cstdint>
#include <string>

#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
    // Un Panel de la UI 2D como componente de GameObject, con el MISMO contrato
    // que CanvasComponent, ButtonComponent, TextComponent, ProgressBarComponent
    // y LayoutComponent: SOLO DATOS. El árbol vivo lo tiene el Renderer
    // (Renderer::uiCanvas()) y lo monta/actualiza syncUiWidgets() cada frame.
    //
    // El Panel del núcleo (UiWidgets.h) no tiene NI UN campo propio: es un
    // UiElement con otro typeName(), o sea el rectángulo de fondo con el que se
    // montan marcos, grupos y ventanas. Así que aquí no hay más que el bloque de
    // rect que ya comparten los demás, el par atlas/sprite y raycastTarget.
    //
    // raycastTarget sí está y en los otros no porque en un panel es EL campo que
    // decide el comportamiento: un fondo a pantalla completa con raycastTarget a
    // true se come los clics de todo lo que tenga detrás, y como no hay nada que
    // lo delate visualmente, sin este campo el editor estaría escondiendo algo
    // que el núcleo sí soporta.
    class PanelComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};      // px, relativa al ancla
            glm::vec2 size{200.0f, 120.0f};      // px
            glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
            bool      visible = true;

            // A false el panel se sigue dibujando pero deja pasar el ratón: es
            // lo que quiere un fondo decorativo detrás de widgets vivos.
            bool raycastTarget = true;

            // --- Sprite --------------------------------------------------------
            // Vacías = quad de color plano, que es lo que dibuja UiElement sin
            // atlas. `sprite` es un NOMBRE dentro del atlas (los registra el
            // sidecar <atlas>.sprites.json); vacío = la imagen entera.
            std::string atlasPath;
            std::string sprite;

            // Vuelca el rect y el sprite en el nodo vivo. NO toca `atlas` (es un
            // puntero a GPU: lo resuelve el sync).
            void applyTo(Panel& p) const
            {
                p.anchorMin     = anchorMin;
                p.anchorMax     = anchorMax;
                p.pivot         = pivot;
                p.position      = position;
                p.size          = size;
                p.color         = color;
                p.visible       = visible;
                p.raycastTarget = raycastTarget;
                p.sprite        = sprite;
            }

            // El sync lo usa para saber si hay algo que volcar: sin esto habría
            // que ensuciar el nodo TODOS los frames, que es justo lo que la
            // caché de vértices del canvas existe para evitar.
            bool operator==(const PanelComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       raycastTarget == o.raycastTarget &&
                       atlasPath == o.atlasPath && sprite == o.sprite;
            }
            bool operator!=(const PanelComponent& o) const { return !(*this == o); }
    };

    // Nombre del nodo vivo de un Panel dentro del canvas. Prefijo DISTINTO al
    // del botón ("go:"), el texto ("txt:"), la barra ("bar:") y el contenedor
    // ("lay:") por lo mismo que aquellos entre sí: un GameObject puede llevar
    // varios componentes de UI a la vez, y dos nodos hermanos con el mismo
    // nombre harían que el gizmo y el picking cogieran el que no toca.
    inline std::string uiPanelNodeName(uint64_t ownerId)
    {
        return "pnl:" + std::to_string(ownerId);
    }

    // Inversa de uiPanelNodeName. Devuelve 0 si el nombre no es de un panel: 0
    // no es un id válido de GameObject.
    inline uint64_t uiPanelOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("pnl:", 0) != 0) return 0;
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
