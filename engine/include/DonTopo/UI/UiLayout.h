#pragma once

// Presets de ancla al estilo Unity. Es azúcar sobre anchorMin/anchorMax/pivot:
// no hay ni un campo nuevo ni una rama nueva en el batcher, solo la combinación
// de valores que ya sabe interpretar.
//
// Un preset NO toca ni position ni size a propósito: al cambiar de esquina se
// conserva el offset que el usuario ya había puesto. En los presets de estirado
// el eje estirado ignora size, y quien manda son los márgenes.

#include "DonTopo/UI/UiCanvas.h"

#include <glm/glm.hpp>

namespace DonTopo
{
    enum class UiAnchorPreset
    {
        TopLeft,
        TopCenter,
        TopRight,
        MiddleLeft,
        MiddleCenter,
        MiddleRight,
        BottomLeft,
        BottomCenter,
        BottomRight,
        StretchHorizontal,   // estira en X, anclado al centro en Y
        StretchVertical,     // estira en Y, anclado al centro en X
        StretchAll           // estira en los dos ejes: el rect del padre menos los márgenes
    };

    inline void applyAnchorPreset(UiElement& element, UiAnchorPreset preset)
    {
        // min == max en un eje = punto de ancla; distintos = estirado.
        glm::vec2 min{0.0f, 0.0f};
        glm::vec2 max{0.0f, 0.0f};
        // El pivote acompaña al ancla: un elemento anclado a la esquina inferior
        // derecha se mide desde SU esquina inferior derecha. En un eje estirado
        // el pivote no se lee, pero se deja al centro por si el preset cambia.
        glm::vec2 pivot{0.0f, 0.0f};

        switch (preset)
        {
            case UiAnchorPreset::TopLeft:      min = max = pivot = {0.0f, 0.0f}; break;
            case UiAnchorPreset::TopCenter:    min = max = pivot = {0.5f, 0.0f}; break;
            case UiAnchorPreset::TopRight:     min = max = pivot = {1.0f, 0.0f}; break;
            case UiAnchorPreset::MiddleLeft:   min = max = pivot = {0.0f, 0.5f}; break;
            case UiAnchorPreset::MiddleCenter: min = max = pivot = {0.5f, 0.5f}; break;
            case UiAnchorPreset::MiddleRight:  min = max = pivot = {1.0f, 0.5f}; break;
            case UiAnchorPreset::BottomLeft:   min = max = pivot = {0.0f, 1.0f}; break;
            case UiAnchorPreset::BottomCenter: min = max = pivot = {0.5f, 1.0f}; break;
            case UiAnchorPreset::BottomRight:  min = max = pivot = {1.0f, 1.0f}; break;

            case UiAnchorPreset::StretchHorizontal:
                min   = {0.0f, 0.5f};
                max   = {1.0f, 0.5f};
                pivot = {0.5f, 0.5f};
                break;
            case UiAnchorPreset::StretchVertical:
                min   = {0.5f, 0.0f};
                max   = {0.5f, 1.0f};
                pivot = {0.5f, 0.5f};
                break;
            case UiAnchorPreset::StretchAll:
                min   = {0.0f, 0.0f};
                max   = {1.0f, 1.0f};
                pivot = {0.5f, 0.5f};
                break;
        }

        element.anchorMin = min;
        element.anchorMax = max;
        element.pivot     = pivot;
    }
}
