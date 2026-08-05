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

namespace DonTopo
{
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

    struct Text : UiElement
    {
        using UiElement::UiElement;
        const char* typeName() const override { return "Text"; }
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
