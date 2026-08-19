#pragma once
#include <cstdint>
#include <string>

#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
    // El auto-layout de la UI 2D como componente de GameObject, con el MISMO
    // contrato que CanvasComponent, ButtonComponent, TextComponent y
    // ProgressBarComponent: SOLO DATOS. El árbol vivo lo tiene el Renderer
    // (Renderer::uiCanvas()) y lo monta/actualiza syncUiWidgets() cada frame.
    //
    // El solver NO está aquí: vive en UiElement (layoutMode, padding, spacing,
    // cellSize, columns, crossAlign, fitWidth/fitHeight, ignoreLayout) y lo
    // resuelve UiSpriteBatch::build. Este componente es la capa de escena que
    // faltaba para poder usarlo: ni una rama nueva en el batcher.
    //
    // Hace los dos papeles que en Unity van en componentes distintos:
    //   - CONTENEDOR (LayoutGroup): mode/padding/spacing/cellSize/columns/
    //     crossAlign/fitWidth/fitHeight COLOCAN a los hijos del GameObject.
    //   - HIJO (LayoutElement): ignoreLayout saca a ESTE GameObject del layout
    //     de su padre. Van juntos porque son dos campos, no dos sistemas: un
    //     componente aparte solo para un bool sería una entrada más en el menú
    //     de Add sin nada que la justifique.
    //
    // El rect (anchorMin/Max, pivot, position, size) SOLO se usa cuando el
    // GameObject no tiene ningún otro componente de UI: ahí el sync monta un
    // contenedor no dibujable y este componente es su dueño. Con un Button, una
    // ProgressBar o un Text en el mismo GameObject el rect lo manda AQUEL —
    // dos dueños del mismo rect es un conflicto sin ganador — y de aquí solo
    // viajan los campos de layout.
    class LayoutComponent
    {
        public:
            // --- Rect (solo si el contenedor es de este componente) -----------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};      // px, relativa al ancla
            glm::vec2 size{200.0f, 200.0f};      // px
            bool      visible = true;

            // --- Contenedor ----------------------------------------------------
            // Vertical y no None por defecto: un componente recién añadido que no
            // hace NADA parece un bug del editor. Con None el contenedor sigue
            // siendo un rect que agrupa y recorta, que también es un uso válido.
            UiLayoutMode mode = UiLayoutMode::Vertical;

            float paddingLeft   = 0.0f;
            float paddingRight  = 0.0f;
            float paddingTop    = 0.0f;
            float paddingBottom = 0.0f;

            glm::vec2 spacing{0.0f, 0.0f};     // .x entre columnas, .y entre filas
            glm::vec2 cellSize{100.0f, 100.0f};  // solo Grid
            uint32_t  columns = 0;             // solo Grid; 0 = las que quepan

            UiCrossAlign crossAlign = UiCrossAlign::Start;

            // Content size fitter: ese eje del size pasa a ser la extensión de
            // los hijos colocados más el padding.
            bool fitWidth  = false;
            bool fitHeight = false;

            // --- Como hijo del layout de OTRO ----------------------------------
            bool ignoreLayout = false;

            // --- Recorte -------------------------------------------------------
            // Recorta a los descendientes contra el rect del contenedor. La
            // intersección con el scissor heredado la hace el batcher.
            bool clipChildren = false;

            // Vuelca los campos de layout en el nodo vivo. `ownsRect` a false es
            // el caso de compartir nodo con otro componente de UI: ese manda el
            // rect y aquí no se toca ni una de sus cuatro esquinas.
            void applyTo(UiElement& e, bool ownsRect) const
            {
                if (ownsRect)
                {
                    e.anchorMin = anchorMin;
                    e.anchorMax = anchorMax;
                    e.pivot     = pivot;
                    e.position  = position;
                    e.size      = size;
                    e.visible   = visible;
                    // Un contenedor agrupa y recorta, pero no pinta: sin esto
                    // saldría un quad de color liso TAPANDO a sus hijos.
                    e.drawable  = false;
                    // Y tampoco recibe el ratón. El hit test NO mira drawable,
                    // solo raycastTarget: sin esto, un grupo que únicamente
                    // coloca se comería los clics de todo lo que quedara detrás
                    // —y como no pinta nada, no habría forma de ver por qué—.
                    // A cambio, en el editor el contenedor se selecciona desde
                    // el Hierarchy y no clicando en el viewport.
                    e.raycastTarget = false;
                }

                e.layoutMode    = mode;
                e.paddingLeft   = paddingLeft;
                e.paddingRight  = paddingRight;
                e.paddingTop    = paddingTop;
                e.paddingBottom = paddingBottom;
                e.spacing       = spacing;
                e.cellSize      = cellSize;
                e.columns       = columns;
                e.crossAlign    = crossAlign;
                e.fitWidth      = fitWidth;
                e.fitHeight     = fitHeight;
                e.ignoreLayout  = ignoreLayout;
                e.clipChildren  = clipChildren;
            }

            // El sync lo usa para saber si hay algo que volcar: sin esto habría
            // que ensuciar el nodo TODOS los frames, que es justo lo que la
            // caché de vértices del canvas existe para evitar.
            bool operator==(const LayoutComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       visible == o.visible &&
                       mode == o.mode &&
                       paddingLeft == o.paddingLeft && paddingRight == o.paddingRight &&
                       paddingTop == o.paddingTop && paddingBottom == o.paddingBottom &&
                       spacing == o.spacing && cellSize == o.cellSize && columns == o.columns &&
                       crossAlign == o.crossAlign &&
                       fitWidth == o.fitWidth && fitHeight == o.fitHeight &&
                       ignoreLayout == o.ignoreLayout && clipChildren == o.clipChildren;
            }
            bool operator!=(const LayoutComponent& o) const { return !(*this == o); }
    };

    // Nombre del nodo vivo del contenedor dentro del canvas. Prefijo DISTINTO al
    // del botón ("go:"), el texto ("txt:") y la barra ("bar:") por lo mismo que
    // aquellos entre sí: un GameObject puede llevar varios componentes de UI a la
    // vez, y dos nodos con el mismo nombre harían que el gizmo y el picking
    // cogieran el que no toca.
    inline std::string uiLayoutNodeName(uint64_t ownerId)
    {
        return "lay:" + std::to_string(ownerId);
    }

    // Inversa de uiLayoutNodeName. Devuelve 0 si el nombre no es de un
    // contenedor: 0 no es un id válido de GameObject.
    inline uint64_t uiLayoutOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("lay:", 0) != 0) return 0;
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
