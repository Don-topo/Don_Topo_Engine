#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
    // Eje y sentido del relleno de la barra. Enum PROPIO del componente y no el
    // UiFillDirection de UiWidgets.h: aquel solo distingue Horizontal/Vertical
    // (el modo Filled del Image), y aquí hace falta también el SENTIDO — una
    // barra de vida que baja hacia la izquierda no es la misma que una que baja
    // hacia la derecha.
    enum class UiProgressFillDirection
    {
        LeftToRight,
        RightToLeft,
        BottomToTop,
        TopToBottom
    };

    // Una barra de progreso de la UI 2D como componente de GameObject, con el
    // MISMO contrato que CanvasComponent, ButtonComponent y TextComponent: SOLO
    // DATOS. El árbol vivo lo tiene el Renderer (Renderer::uiCanvas()) y lo
    // monta/actualiza syncUiWidgets() cada frame.
    //
    // Se dibuja por COMPOSICIÓN: el núcleo (DonTopo::ProgressBar, UiWidgets.h)
    // es un stub sin valor ni colores, y UiSpriteBatch no sabe emitir un relleno
    // parcial. Así que el sync monta DOS nodos —el fondo (el rect entero) y un
    // hijo con el rect del relleno— exactamente como la etiqueta del Button. El
    // componente no toca el core de UI.
    class ProgressBarComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};    // px, relativa al ancla
            glm::vec2 size{160.0f, 20.0f};     // px
            glm::vec4 color{0.2f, 0.2f, 0.2f, 1.0f};   // color del FONDO
            bool      visible = true;

            // --- Valor ---------------------------------------------------------
            // Sin clamp AQUÍ a propósito: el componente no interpreta nada (mismo
            // criterio que el resto). Quien normaliza es el sync, al calcular el
            // rect del relleno.
            float value    = 0.5f;
            float minValue = 0.0f;
            float maxValue = 1.0f;

            // --- Relleno -------------------------------------------------------
            glm::vec4 fillColor{0.25f, 0.7f, 1.0f, 1.0f};

            UiProgressFillDirection fillDirection = UiProgressFillDirection::LeftToRight;

            // --- Assets --------------------------------------------------------
            // TRES rutas de imagen, no nombres de sprite: un "sprite" del núcleo
            // es un sub-rect que hay que registrar a mano con
            // UiTextureAtlas::addSprite, y el editor no registra ninguno (una
            // imagen suelta se usa entera, que es lo que devuelve uvRect sin
            // nombre). Así que cada parte trae su propio fichero.
            //
            // atlasPath es el fallback COMPARTIDO: lo que se usa en la parte que
            // no tenga ruta propia. Las tres vacías = quads de color plano.
            std::string atlasPath;
            std::string backgroundPath;
            std::string fillPath;

            // Fracción del rect que ocupa el relleno, ya acotada a [0,1]. Un
            // rango degenerado (max <= min) da 0: no hay forma de repartir un
            // intervalo vacío, y una barra llena sería mentir sobre el dato.
            float normalizedValue() const
            {
                if (!(maxValue > minValue)) return 0.0f;
                const float t = (value - minValue) / (maxValue - minValue);
                return std::clamp(t, 0.0f, 1.0f);
            }

            // Rect del relleno EN COORDENADAS DEL PADRE (el nodo de fondo), que
            // es de donde cuelga. Aquí y no en el sync para poder probarlo sin
            // canvas ni GPU.
            void fillRect(glm::vec2& outPos, glm::vec2& outSize) const
            {
                const float t = normalizedValue();
                const float w = size.x;
                const float h = size.y;
                switch (fillDirection)
                {
                    case UiProgressFillDirection::RightToLeft:
                        outPos  = glm::vec2(w * (1.0f - t), 0.0f);
                        outSize = glm::vec2(w * t, h);
                        break;
                    case UiProgressFillDirection::TopToBottom:
                        // La Y del canvas crece hacia ABAJO: llenar desde arriba
                        // es dejar el origen quieto y crecer el alto.
                        outPos  = glm::vec2(0.0f, 0.0f);
                        outSize = glm::vec2(w, h * t);
                        break;
                    case UiProgressFillDirection::BottomToTop:
                        outPos  = glm::vec2(0.0f, h * (1.0f - t));
                        outSize = glm::vec2(w, h * t);
                        break;
                    default:   // LeftToRight
                        outPos  = glm::vec2(0.0f, 0.0f);
                        outSize = glm::vec2(w * t, h);
                        break;
                }
            }

            // Vuelca el rect y el fondo en el nodo vivo. NO toca `atlas` (es un
            // puntero a GPU: lo resuelve el sync).
            void applyTo(ProgressBar& p) const
            {
                p.anchorMin = anchorMin;
                p.anchorMax = anchorMax;
                p.pivot     = pivot;
                p.position  = position;
                p.size      = size;
                p.color     = color;
                p.visible   = visible;
            }

            // Y lo mismo para el hijo del relleno. Anclas y pivot a cero: su rect
            // se cuenta en píxeles desde la esquina del fondo, que es justo lo
            // que devuelve fillRect().
            void applyToFill(UiElement& f) const
            {
                glm::vec2 pos{0.0f};
                glm::vec2 sz{0.0f};
                fillRect(pos, sz);

                f.anchorMin = glm::vec2(0.0f);
                f.anchorMax = glm::vec2(0.0f);
                f.pivot     = glm::vec2(0.0f);
                f.position  = pos;
                f.size      = sz;
                f.color     = fillColor;
                f.visible   = true;
                // A valor 0 el rect es degenerado: mejor no emitir el quad que
                // emitir uno de área nula (y con un sprite, uno de 0 px de ancho
                // con UVs completas).
                f.drawable  = (sz.x > 0.0f && sz.y > 0.0f);
            }

            // El sync lo usa para saber si hay algo que volcar: sin esto habría
            // que ensuciar el nodo TODOS los frames, que es justo lo que la
            // caché de vértices del canvas existe para evitar.
            bool operator==(const ProgressBarComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       value == o.value && minValue == o.minValue && maxValue == o.maxValue &&
                       fillColor == o.fillColor && fillDirection == o.fillDirection &&
                       atlasPath == o.atlasPath && backgroundPath == o.backgroundPath &&
                       fillPath == o.fillPath;
            }
            bool operator!=(const ProgressBarComponent& o) const { return !(*this == o); }
    };

    // Nombre del nodo vivo de una ProgressBar dentro del canvas. Mismo papel que
    // uiButtonNodeName/uiTextNodeName y con prefijo DISTINTO a propósito: un
    // GameObject puede llevar los tres componentes a la vez, y dos nodos
    // hermanos con el mismo nombre harían que el gizmo y el picking cogieran el
    // que no toca.
    inline std::string uiProgressBarNodeName(uint64_t ownerId)
    {
        return "bar:" + std::to_string(ownerId);
    }

    // Inversa de uiProgressBarNodeName. Devuelve 0 si el nombre no es de una
    // barra: 0 no es un id válido de GameObject. El corte por '/' hace que el
    // nodo del relleno ("bar:7/Fill") devuelva también su dueño.
    inline uint64_t uiProgressBarOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("bar:", 0) != 0) return 0;
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
