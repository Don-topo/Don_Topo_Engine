#pragma once

// Unity-style anchor presets. This is sugar over anchorMin/anchorMax/pivot:
// there is no new field and no new branch in the batcher, only a combination
// of values it already knows how to interpret.
//
// A preset deliberately touches neither position nor size: when switching
// corners, the offset the user had already set is kept. In the stretch presets
// the stretched axis ignores size, and the margins rule.

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
        StretchHorizontal,   // stretches in X, anchored to the center in Y
        StretchVertical,     // stretches in Y, anchored to the center in X
        StretchAll           // stretches on both axes: the parent's rect minus the margins
    };

    inline void applyAnchorPreset(UiElement& element, UiAnchorPreset preset)
    {
        // min == max on an axis = anchor point; different = stretched.
        glm::vec2 min{0.0f, 0.0f};
        glm::vec2 max{0.0f, 0.0f};
        // The pivot follows the anchor: an element anchored to the bottom-right
        // corner is measured from ITS bottom-right corner. On a stretched axis
        // the pivot is not read, but it is left at the center in case the preset changes.
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
