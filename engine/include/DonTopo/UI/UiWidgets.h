#pragma once

// Los tipos concretos de widget. TODOS están vacíos a propósito: esta fase solo
// fija la jerarquía (que existan, que hereden y que compartan el estado de
// UiElement), no el dibujado ni el comportamiento de ninguno.
//
// Un widget se crea con padre.add<Button>("Aceptar") y de momento se dibuja
// exactamente igual que su base: quad de color, con atlas y sprite si los
// tiene. Lo único que los distingue hoy es typeName().
//
// Aquí no hay .cpp ni habrá campos nuevos hasta que cada widget tenga su fase:
// añadir estado antes de tener dibujado sería estado que nadie lee.

#include "DonTopo/UI/UiCanvas.h"

#include <cstdint>
#include <string>

namespace DonTopo
{
    class UiFont;

    struct Panel : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "Panel"; }
    };

    // Cómo se reparte el sprite dentro del rect del Image. Los cuatro modos se
    // resuelven en CPU dentro del batcher (N quads del mismo atlas y el mismo
    // scissor): ni un shader, ni un pipeline, ni un campo más en el vértice.
    enum class UiImageMode
    {
        Normal,   // un quad, el sprite estirado al rect: exactamente lo de siempre
        Tiled,    // el sprite repetido a su tamaño NATIVO, con la última fila/columna recortada por UV
        Sliced,   // 9-slice: las esquinas no se estiran, los bordes solo en su eje
        Filled    // solo una fracción del rect, recortando posición Y UV a la vez
    };

    // Eje del relleno del modo Filled. El radial queda fuera a propósito: pide
    // geometría en abanico, y esto se resuelve con quads.
    enum class UiFillDirection
    {
        Horizontal,
        Vertical
    };

    // Desde qué extremo del eje crece el relleno. Start es izquierda en
    // Horizontal y arriba en Vertical (la misma convención de +Y hacia abajo del
    // canvas).
    enum class UiFillOrigin
    {
        Start,
        End
    };

    struct Image : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "Image"; }
        const Image* asImage() const override { return this; }

        UiImageMode mode = UiImageMode::Normal;

        // --- Sliced ------------------------------------------------------------
        // Bordes en píxeles DEL SPRITE, no del rect: son los que definen dónde
        // corta el 9-slice la textura, así que escalar el elemento no los mueve.
        float borderLeft   = 0.0f;
        float borderRight  = 0.0f;
        float borderTop    = 0.0f;
        float borderBottom = 0.0f;

        // Sin centro salen 8 quads: es lo que quiere un marco que deja ver lo de
        // detrás.
        bool fillCenter = true;

        // --- Tiled -------------------------------------------------------------
        // Tope duro de quads del Image. Un rect grande con un sprite de 2 px
        // pediría decenas de miles de quads y reventaría el buffer, así que
        // pasado el tope el elemento se dibuja como Normal.
        uint32_t maxTiles = 1024;

        // --- Filled ------------------------------------------------------------
        UiFillDirection fillDirection = UiFillDirection::Horizontal;
        UiFillOrigin    fillOrigin    = UiFillOrigin::Start;
        float           fillAmount    = 1.0f;   // 0..1; a 0 no se emite ni un quad
    };

    // El único con estado propio de momento: sin campos no habría nada que
    // dibujar. Una sola línea, sin wrap, sin alineación y sin rich text: eso
    // es otra fase.
    // Alineación horizontal del bloque de texto DENTRO del ancho del rect del
    // elemento. Justify reparte el sobrante entre los espacios de la línea y
    // nunca toca la última ni una acabada en '\n': una línea suelta estirada a
    // todo lo ancho se ve como un error, no como texto justificado.
    enum class UiTextAlign
    {
        Left,
        Center,
        Right,
        Justify
    };

    // Dónde cae el BLOQUE de líneas dentro del rect, que es la otra mitad de
    // UiTextAlign: aquella reparte cada línea a lo ancho y esta el bloque
    // entero a lo alto. Por defecto `Top`, que es lo que hacía el emisor antes
    // de que esto existiera (línea base a un ascent del borde de arriba), así
    // que ningún texto ya colocado se mueve.
    enum class UiTextVAlign
    {
        Top,
        Middle,
        Bottom
    };

    // Qué pasa con lo que no cabe en el rect. El recorte no es gratis (parte el
    // lote por scissor), así que el modo por defecto es no recortar nada.
    enum class UiTextOverflow
    {
        Overflow,   // se dibuja fuera del rect
        Clip,       // scissor contra el propio rect
        Ellipsis    // la última línea que cabe acaba en '…'
    };

    struct Text : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "Text"; }
        const Text* asText() const override { return this; }

        // Sin fuente el elemento vuelve a dibujarse como su base (quad de
        // color): así un Text a medio configurar no desaparece en silencio.
        const UiFont* font = nullptr;

        std::string text;   // UTF-8; se decodifica a codepoints al emitir

        // En píxeles de PANTALLA. No tiene por qué coincidir con el tamaño al
        // que se horneó el atlas: de eso va el MSDF.
        float fontSize = 16.0f;

        // El color del relleno es UiElement::color.

        // Grosor en píxeles de pantalla. A 0 no hay outline y el shader ni
        // entra en esa rama.
        float     outlineWidth = 0.0f;
        glm::vec4 outlineColor{0.0f, 0.0f, 0.0f, 1.0f};

        // Desplazamiento en píxeles. A {0,0} (o con alfa 0) no se emite ni un
        // quad de sombra.
        glm::vec2 shadowOffset{0.0f, 0.0f};
        glm::vec4 shadowColor{0.0f, 0.0f, 0.0f, 0.5f};

        // ── Rich text, alineación, wrap y overflow ──────────────────────────
        // El texto SIEMPRE se parsea buscando tags: un tag malformado,
        // desconocido o sin cerrar se dibuja como texto literal, así que un
        // texto plano da exactamente lo mismo que antes de esta fase.
        //   <color=#RRGGBB> <color=#RRGGBBAA> <size=N> <b> <i> y sus cierres.
        // Anidan sobre una pila: el cierre restaura el estilo de fuera.
        UiTextAlign    align    = UiTextAlign::Left;
        UiTextVAlign   vAlign   = UiTextVAlign::Top;
        UiTextOverflow overflow = UiTextOverflow::Overflow;

        // Corta por palabras contra el ancho del rect; una palabra que no cabe
        // ni sola se parte por glyph. Los '\n' del texto siempre cortan, con
        // wrap o sin él.
        bool wordWrap = false;

        // <b> NO carga una segunda fuente: engorda el glyph por el MISMO canal
        // que ya usa el outline, en fracción del tamaño del tramo (así una
        // negrita a 12 px y otra a 48 px engordan lo mismo en proporción).
        float boldStrength = 0.08f;

        // <i> tampoco: es una cizalla del quad sobre la línea base. Es la
        // tangente del ángulo, así que 0.25 son unos 14 grados.
        float italicSkew = 0.25f;
    };

    // Los cinco estados de un botón. NO hay máquina de estados: el estado se
    // DERIVA cada updateInput del que ya lleva el elemento (hovered, botón
    // izquierdo abajo encima, focused) más interactable y selected, con una
    // prioridad FIJA: Disabled > Pressed > Selected > Hover > Normal.
    enum class UiButtonState
    {
        Normal,
        Hover,
        Pressed,
        Disabled,
        Selected
    };

    // Cómo se ve el cambio de estado. El botón NO toca el batcher: escribe en
    // los campos que UiSpriteBatch ya lee (color y sprite), así que ninguna de
    // las tres transiciones añade un quad, un lote ni un pipeline.
    enum class UiButtonTransition
    {
        ColorTint,    // color = el del estado, en el acto
        SpriteSwap,   // sprite = el del estado; mismo atlas, así que mismo lote
        Animation     // color interpolado LINEALMENTE durante fadeDuration
    };

    struct Button : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "Button"; }
        Button*       asButton()       override { return this; }
        const Button* asButton() const override { return this; }

        // A false el botón sigue recibiendo hit test (para que Disabled se pinte
        // al pasar por encima) pero NO emite Click ni DoubleClick.
        bool interactable = true;

        // Estado propio, del juego: un botón de una barra de pestañas sigue
        // marcado con el ratón lejos. Se suma al foco: un focusable enfocado
        // también cuenta como Selected.
        bool selected = false;

        UiButtonTransition transition = UiButtonTransition::ColorTint;

        // Tinte BASE del botón, sobre el que se multiplican los colores de
        // estado. UiElement::color no sirve para esto: lo reescribe el canvas en
        // cada updateInput con el color del estado, así que lo que pusiera ahí
        // el usuario duraba hasta el primer frame de input y parecía que el
        // campo no hacía nada. Blanco = neutro, o sea el comportamiento de
        // siempre: el color que se ve es exactamente el del estado.
        glm::vec4 baseColor{1.0f, 1.0f, 1.0f, 1.0f};

        // Colores por estado. Campos, no constantes escondidas: cada botón los
        // suyos. El de Normal es el que se restaura al volver a Normal, así que
        // por defecto vale el mismo blanco que UiElement::color.
        glm::vec4 normalColor{1.0f, 1.0f, 1.0f, 1.0f};
        glm::vec4 hoverColor{1.0f, 1.0f, 1.0f, 1.0f};
        glm::vec4 pressedColor{1.0f, 1.0f, 1.0f, 1.0f};
        glm::vec4 disabledColor{1.0f, 1.0f, 1.0f, 1.0f};
        glm::vec4 selectedColor{1.0f, 1.0f, 1.0f, 1.0f};

        // Sprites por estado, nombres DEL MISMO atlas del elemento. Uno vacío
        // deja el sprite como esté: un estado sin arte no borra el que había.
        std::string normalSprite;
        std::string hoverSprite;
        std::string pressedSprite;
        std::string disabledSprite;
        std::string selectedSprite;

        // Segundos del fundido de Animation. El tiempo ENTRA por
        // UiInputState::timeSeconds: aquí no hay reloj, y por eso el fundido es
        // reproducible en un test sin GUI. A <= 0 el color salta de golpe.
        float fadeDuration = 0.1f;

        // Estado resuelto por el último updateInput. Lectura: lo escribe el
        // canvas, no el usuario.
        UiButtonState state = UiButtonState::Normal;

        // Interior del fundido: de qué color arrancó y cuándo. m_stateReady a
        // false = el botón no ha visto todavía ni un updateInput, y el primero
        // COLOCA el color del estado sin fundir (fundir desde el color de
        // fábrica sería una animación que nadie ha pedido).
        glm::vec4 fadeFrom{1.0f, 1.0f, 1.0f, 1.0f};
        float     fadeStartTime = 0.0f;
        bool      stateReady    = false;
    };

    struct Slider : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "Slider"; }
    };

    struct Checkbox : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "Checkbox"; }
    };

    struct Toggle : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "Toggle"; }
    };

    struct Scrollbar : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "Scrollbar"; }
    };

    struct InputField : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "InputField"; }
    };

    struct ProgressBar : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "ProgressBar"; }
    };

    struct Dropdown : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "Dropdown"; }
    };

    struct ScrollView : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "ScrollView"; }
    };
}
