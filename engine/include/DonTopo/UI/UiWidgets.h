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

#include <string>

namespace DonTopo
{
    class UiFont;

    struct Panel : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "Panel"; }
    };

    struct Image : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "Image"; }
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

    struct Button : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "Button"; }
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
