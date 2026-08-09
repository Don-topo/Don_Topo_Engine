#pragma once
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiFont.h"
#include "DonTopo/UI/UiTextureAtlas.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
    // Un botón de la UI 2D como componente de GameObject, con el MISMO contrato
    // que CanvasComponent: SOLO DATOS. No guarda ni un UiElement ni el atlas ni
    // la fuente — el árbol vivo lo sigue teniendo el Renderer
    // (Renderer::uiCanvas()), y quien dibuja lo reconstruye/actualiza cada frame
    // con syncUiWidgets(). Así lo que se ve en Play y en el juego exportado sale
    // de la ESCENA y no de un árbol cableado a mano.
    //
    // Los nombres, los defaults y el significado son EXACTAMENTE los del núcleo:
    //   - el bloque de rect y el de sprite son de UiElement (UiCanvas.h),
    //   - el bloque de estados es de Button (UiWidgets.h),
    //   - el bloque de texto es de Text (UiWidgets.h), porque Button NO tiene
    //     texto: la etiqueta es un HIJO Text que monta el sync.
    // Este componente no interpreta ni clampa nada.
    //
    // Las dos rutas (atlasPath, fontPath) son lo ÚNICO que no es un campo del
    // núcleo: el núcleo guarda punteros a recursos de GPU, que no se serializan.
    // Las resuelve el sync contra el Renderer, no el componente.
    class ButtonComponent
    {
        public:
            // --- Rect (UiElement) ---------------------------------------------
            glm::vec2 anchorMin{0.0f, 0.0f};
            glm::vec2 anchorMax{0.0f, 0.0f};
            glm::vec2 pivot{0.0f, 0.0f};
            glm::vec2 position{0.0f, 0.0f};   // px, relativa al ancla
            glm::vec2 size{160.0f, 40.0f};    // px
            glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
            bool      visible = true;

            // Ruta del atlas (PNG) y nombre del sprite base dentro de él. Vacías
            // = botón de color plano, que es lo que dibuja UiElement sin atlas.
            std::string atlasPath;
            std::string sprite;

            // --- Estados (Button) ---------------------------------------------
            bool               interactable = true;
            bool               selected     = false;
            UiButtonTransition transition   = UiButtonTransition::ColorTint;

            glm::vec4 normalColor{1.0f, 1.0f, 1.0f, 1.0f};
            glm::vec4 hoverColor{1.0f, 1.0f, 1.0f, 1.0f};
            glm::vec4 pressedColor{1.0f, 1.0f, 1.0f, 1.0f};
            glm::vec4 disabledColor{1.0f, 1.0f, 1.0f, 1.0f};
            glm::vec4 selectedColor{1.0f, 1.0f, 1.0f, 1.0f};

            // Nombres del MISMO atlas del botón. Uno vacío deja el sprite como
            // esté (es el contrato de Button, no una decisión de aquí).
            std::string normalSprite;
            std::string hoverSprite;
            std::string pressedSprite;
            std::string disabledSprite;
            std::string selectedSprite;

            float fadeDuration = 0.1f;   // segundos del fundido de Animation

            // --- Etiqueta (hijo Text) -----------------------------------------
            std::string text;
            std::string fontPath;                        // TTF; vacía = sin texto
            float       fontSize = 16.0f;
            glm::vec4   textColor{1.0f, 1.0f, 1.0f, 1.0f};
            UiTextAlign textAlign = UiTextAlign::Center;

            // Vuelca el rect y los estados en el botón vivo. NO toca ni el atlas
            // (es un puntero a GPU: lo resuelve el sync) ni los campos que
            // escribe el propio canvas (state, fadeFrom, fadeStartTime,
            // stateReady): pisarlos cada frame mataría el fundido.
            void applyTo(Button& b) const
            {
                b.anchorMin    = anchorMin;
                b.anchorMax    = anchorMax;
                b.pivot        = pivot;
                b.position     = position;
                b.size         = size;
                b.color        = color;
                b.visible      = visible;
                b.sprite       = sprite;

                b.interactable = interactable;
                b.selected     = selected;
                b.transition   = transition;

                b.normalColor   = normalColor;
                b.hoverColor    = hoverColor;
                b.pressedColor  = pressedColor;
                b.disabledColor = disabledColor;
                b.selectedColor = selectedColor;

                b.normalSprite   = normalSprite;
                b.hoverSprite    = hoverSprite;
                b.pressedSprite  = pressedSprite;
                b.disabledSprite = disabledSprite;
                b.selectedSprite = selectedSprite;

                b.fadeDuration = fadeDuration;
            }

            // La etiqueta ocupa el rect ENTERO del botón (anclada a las cuatro
            // esquinas, sin márgenes): así el texto sigue al botón al cambiarle
            // el tamaño sin un segundo juego de campos que mantener.
            void applyToLabel(Text& t) const
            {
                t.anchorMin = glm::vec2(0.0f, 0.0f);
                t.anchorMax = glm::vec2(1.0f, 1.0f);
                t.pivot     = glm::vec2(0.0f, 0.0f);
                t.position  = glm::vec2(0.0f, 0.0f);
                t.text      = text;
                t.fontSize  = fontSize;
                t.color     = textColor;
                t.align     = textAlign;
                t.visible   = visible;
            }

            // El sync lo usa para saber si hay algo que volcar: sin esto habría
            // que ensuciar el nodo TODOS los frames, que es justo lo que la
            // caché de vértices del canvas existe para evitar.
            bool operator==(const ButtonComponent& o) const
            {
                return anchorMin == o.anchorMin && anchorMax == o.anchorMax &&
                       pivot == o.pivot && position == o.position && size == o.size &&
                       color == o.color && visible == o.visible &&
                       atlasPath == o.atlasPath && sprite == o.sprite &&
                       interactable == o.interactable && selected == o.selected &&
                       transition == o.transition &&
                       normalColor == o.normalColor && hoverColor == o.hoverColor &&
                       pressedColor == o.pressedColor && disabledColor == o.disabledColor &&
                       selectedColor == o.selectedColor &&
                       normalSprite == o.normalSprite && hoverSprite == o.hoverSprite &&
                       pressedSprite == o.pressedSprite && disabledSprite == o.disabledSprite &&
                       selectedSprite == o.selectedSprite &&
                       fadeDuration == o.fadeDuration &&
                       text == o.text && fontPath == o.fontPath && fontSize == o.fontSize &&
                       textColor == o.textColor && textAlign == o.textAlign;
            }
            bool operator!=(const ButtonComponent& o) const { return !(*this == o); }
    };

    // Fuente que se usa cuando el botón tiene texto y NADIE ha puesto una ruta.
    // Un texto sin fuente no se dibuja (el emisor cae al quad de la base), así
    // que sin este fallback escribir en el campo Text no se vería hasta buscar
    // un TTF a mano.
    //
    // Va DENTRO del proyecto y no a una fuente del sistema a propósito: el juego
    // exportado se lleva los assets del proyecto, no los de la máquina que
    // exportó, y una ruta tipo C:/Windows/Fonts/... deja el texto invisible en
    // cualquier otro PC. Relativa al directorio de trabajo, igual que el resto
    // de assets. El exportador la empaqueta cuando algún botón tiene texto sin
    // fuente propia (GameExporter::collectSceneAssets).
    inline constexpr const char* kDefaultUiFontPath =
        "assets/DancingScript-VariableFont_wght.ttf";

    // Nombre del nodo vivo de un botón dentro del canvas. Es la ÚNICA forma de
    // volver del árbol de UI al GameObject (el árbol no guarda punteros a la
    // escena), así que la convención vive aquí y no repetida en cada caller:
    // la usa el sync para crear los nodos y el editor para el gizmo y el
    // picking. La etiqueta cuelga como "<nombre>/Label".
    inline std::string uiButtonNodeName(uint64_t ownerId)
    {
        return "go:" + std::to_string(ownerId);
    }

    // Inversa de uiButtonNodeName, tolerante con el sufijo "/Label" (el hit
    // test devuelve el nodo más profundo, que puede ser la etiqueta). Devuelve
    // 0 si el nombre no es de un botón: 0 no es un id válido de GameObject.
    inline uint64_t uiButtonOwnerId(const std::string& nodeName)
    {
        if (nodeName.rfind("go:", 0) != 0) return 0;
        uint64_t id = 0;
        for (size_t i = 3; i < nodeName.size(); i++)
        {
            const char c = nodeName[i];
            if (c == '/') break;
            if (c < '0' || c > '9') return 0;
            id = id * 10 + (uint64_t)(c - '0');
        }
        return id;
    }

    // El sync y su caché NO viven aquí: la raíz del canvas se reconstruye con
    // clearChildren(), así que hay UN solo sync dueño de todos los widgets
    // (syncUiWidgets en TextComponent.h). Dos syncs sobre la misma raíz se
    // borrarían los nodos el uno al otro cada vez que uno reconstruyera.
}
