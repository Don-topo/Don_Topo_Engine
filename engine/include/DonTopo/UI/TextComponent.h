#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "DonTopo/UI/ButtonComponent.h"
#include "DonTopo/UI/ImageComponent.h"
#include "DonTopo/UI/LayoutComponent.h"
#include "DonTopo/UI/PanelComponent.h"
#include "DonTopo/UI/ProgressBarComponent.h"
#include "DonTopo/UI/SliderComponent.h"
#include "DonTopo/UI/CheckboxComponent.h"
#include "DonTopo/UI/ToggleComponent.h"
#include "DonTopo/UI/ScrollbarComponent.h"
#include "DonTopo/UI/InputFieldComponent.h"
#include "DonTopo/UI/DropdownComponent.h"
#include "DonTopo/UI/ScrollViewComponent.h"
#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiFont.h"
#include "DonTopo/UI/UiTextureAtlas.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
    // Una etiqueta de la UI 2D como componente de GameObject, con el MISMO
    // contrato que CanvasComponent y ButtonComponent: SOLO DATOS. No guarda ni
    // un UiElement ni la fuente — el árbol vivo lo sigue teniendo el Renderer
    // (Renderer::uiCanvas()), y quien dibuja lo reconstruye/actualiza cada frame
    // con syncUiWidgets(). Así lo que se ve en Play y en el juego exportado sale
    // de la ESCENA y no de un árbol cableado a mano.
    //
    // Los nombres, los defaults y el significado son EXACTAMENTE los del núcleo:
    //   - el bloque de rect es de UiElement (UiCanvas.h), el mismo que replica
    //     ButtonComponent,
    //   - el resto son TODOS los campos de Text (UiWidgets.h), salvo `font`.
    // Este componente no interpreta ni clampa nada.
    //
    // fontPath es lo ÚNICO que no es un campo del núcleo: el núcleo guarda un
    // puntero a un recurso de GPU, que no se serializa. La resuelve el sync
    // contra el Renderer, no el componente.
    class TextComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};   // px, relativa al ancla
            glm::vec2 size{160.0f, 40.0f};    // px
            glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};   // relleno del glyph
            bool      visible = true;

            // --- Texto (Text) -------------------------------------------------
            std::string text;
            std::string fontPath;   // TTF; vacía = la fuente por defecto
            float       fontSize = 16.0f;

            float     outlineWidth = 0.0f;
            glm::vec4 outlineColor{0.0f, 0.0f, 0.0f, 1.0f};

            glm::vec2 shadowOffset{0.0f, 0.0f};
            glm::vec4 shadowColor{0.0f, 0.0f, 0.0f, 0.5f};

            UiTextAlign    align    = UiTextAlign::Left;
            UiTextVAlign   vAlign   = UiTextVAlign::Top;
            UiTextOverflow overflow = UiTextOverflow::Overflow;
            bool           wordWrap = false;

            float boldStrength = 0.08f;
            float italicSkew   = 0.25f;

            // Vuelca el rect y el texto en el nodo vivo. NO toca `font` (es un
            // puntero a GPU: lo resuelve el sync).
            void applyTo(Text& t) const
            {
                t.anchorMin = anchorMin;
                t.anchorMax = anchorMax;
                t.pivot     = pivot;
                t.position  = position;
                t.size      = size;
                t.color     = color;
                t.visible   = visible;

                t.text         = text;
                t.fontSize     = fontSize;
                t.outlineWidth = outlineWidth;
                t.outlineColor = outlineColor;
                t.shadowOffset = shadowOffset;
                t.shadowColor  = shadowColor;
                t.align        = align;
                t.vAlign       = vAlign;
                t.overflow     = overflow;
                t.wordWrap     = wordWrap;
                t.boldStrength = boldStrength;
                t.italicSkew   = italicSkew;
            }

            // El sync lo usa para saber si hay algo que volcar: sin esto habría
            // que ensuciar el nodo TODOS los frames, que es justo lo que la
            // caché de vértices del canvas existe para evitar.
            bool operator==(const TextComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       text == o.text && fontPath == o.fontPath && fontSize == o.fontSize &&
                       outlineWidth == o.outlineWidth && outlineColor == o.outlineColor &&
                       shadowOffset == o.shadowOffset && shadowColor == o.shadowColor &&
                       align == o.align && vAlign == o.vAlign &&
                       overflow == o.overflow && wordWrap == o.wordWrap &&
                       boldStrength == o.boldStrength && italicSkew == o.italicSkew;
            }
            bool operator!=(const TextComponent& o) const { return !(*this == o); }
    };

    // Nombre del nodo vivo de un Text dentro del canvas. Mismo papel que
    // uiButtonNodeName (única forma de volver del árbol de UI al GameObject) y
    // con prefijo DISTINTO a propósito: un GameObject puede llevar Button y Text
    // a la vez, y dos nodos hermanos con el mismo nombre harían que el gizmo y
    // el picking cogieran el que no toca.
    inline std::string uiTextNodeName(uint64_t ownerId)
    {
        return "txt:" + std::to_string(ownerId);
    }

    // Inversa de uiTextNodeName. Devuelve 0 si el nombre no es de un Text: 0 no
    // es un id válido de GameObject.
    inline uint64_t uiTextOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("txt:", 0) != 0) return 0;
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

// Compatibilidad: la maquinaria de sync se mudó a UiWidgetSync.h y hay ~80
// puntos de llamada que incluyen este fichero esperando encontrarla. Se incluye
// al FINAL a propósito: UiWidgetSync.h necesita TextComponent completo.
#include "DonTopo/UI/UiWidgetSync.h"
