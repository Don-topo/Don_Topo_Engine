#pragma once
#include <cstdint>
#include <string>

#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
    // Un Image de la UI 2D como componente de GameObject, con el MISMO contrato
    // que el resto: SOLO DATOS. El árbol vivo lo tiene el Renderer
    // (Renderer::uiCanvas()) y lo monta/actualiza syncUiWidgets() cada frame.
    //
    // A diferencia del Panel, el Image del núcleo (UiWidgets.h) SÍ tiene estado
    // propio: los cuatro modos de reparto del sprite dentro del rect, los bordes
    // del 9-slice, el tope de tiles y el bloque de Filled. Todos se resuelven en
    // CPU dentro del batcher (N quads del mismo atlas y el mismo scissor), así
    // que aquí no hay más trabajo que hacerlos llegar al nodo: el componente
    // expone los NUEVE campos, ni uno menos.
    class ImageComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};      // px, relativa al ancla
            glm::vec2 size{100.0f, 100.0f};      // px
            glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};   // tinte del sprite
            bool      visible = true;

            // A false la imagen se sigue dibujando pero deja pasar el ratón.
            bool raycastTarget = true;

            // --- Sprite --------------------------------------------------------
            // Vacías = quad de color plano. `sprite` es un NOMBRE dentro del
            // atlas (los registra el sidecar <atlas>.sprites.json); vacío = la
            // imagen entera.
            std::string atlasPath;
            std::string sprite;

            // --- Modo ----------------------------------------------------------
            UiImageMode mode = UiImageMode::Normal;

            // --- Sliced --------------------------------------------------------
            // Bordes en píxeles DEL SPRITE, no del rect: escalar el elemento no
            // los mueve. Sin centro salen 8 quads, que es lo que quiere un marco
            // que deja ver lo de detrás.
            float borderLeft   = 0.0f;
            float borderRight  = 0.0f;
            float borderTop    = 0.0f;
            float borderBottom = 0.0f;
            bool  fillCenter   = true;

            // --- Tiled ---------------------------------------------------------
            // Tope duro de quads: pasado el tope el elemento se dibuja como
            // Normal en vez de reventar el buffer de vértices.
            uint32_t maxTiles = 1024;

            // --- Filled --------------------------------------------------------
            UiFillDirection fillDirection = UiFillDirection::Horizontal;
            UiFillOrigin    fillOrigin    = UiFillOrigin::Start;
            float           fillAmount    = 1.0f;   // 0..1; a 0 no se emite ni un quad

            // Vuelca el rect y los campos propios en el nodo vivo. NO toca
            // `atlas` (es un puntero a GPU: lo resuelve el sync).
            void applyTo(Image& im) const
            {
                im.anchorMin     = anchorMin;
                im.anchorMax     = anchorMax;
                im.pivot         = pivot;
                im.position      = position;
                im.size          = size;
                im.color         = color;
                im.visible       = visible;
                im.raycastTarget = raycastTarget;
                im.sprite        = sprite;

                im.mode         = mode;
                im.borderLeft   = borderLeft;
                im.borderRight  = borderRight;
                im.borderTop    = borderTop;
                im.borderBottom = borderBottom;
                im.fillCenter   = fillCenter;
                im.maxTiles     = maxTiles;

                im.fillDirection = fillDirection;
                im.fillOrigin    = fillOrigin;
                im.fillAmount    = fillAmount;
            }

            // El sync lo usa para saber si hay algo que volcar: sin esto habría
            // que ensuciar el nodo TODOS los frames, que es justo lo que la
            // caché de vértices del canvas existe para evitar.
            bool operator==(const ImageComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       raycastTarget == o.raycastTarget &&
                       atlasPath == o.atlasPath && sprite == o.sprite &&
                       mode == o.mode &&
                       borderLeft == o.borderLeft && borderRight == o.borderRight &&
                       borderTop == o.borderTop && borderBottom == o.borderBottom &&
                       fillCenter == o.fillCenter && maxTiles == o.maxTiles &&
                       fillDirection == o.fillDirection && fillOrigin == o.fillOrigin &&
                       fillAmount == o.fillAmount;
            }
            bool operator!=(const ImageComponent& o) const { return !(*this == o); }
    };

    // Nombre del nodo vivo de un Image dentro del canvas. Prefijo DISTINTO al de
    // los demás por lo mismo que aquellos entre sí: un GameObject puede llevar
    // varios componentes de UI a la vez, y dos nodos hermanos con el mismo
    // nombre harían que el gizmo y el picking cogieran el que no toca.
    inline std::string uiImageNodeName(uint64_t ownerId)
    {
        return "img:" + std::to_string(ownerId);
    }

    // Inversa de uiImageNodeName. Devuelve 0 si el nombre no es de una imagen: 0
    // no es un id válido de GameObject.
    inline uint64_t uiImageOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("img:", 0) != 0) return 0;
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
