#pragma once

namespace DonTopo
{
    // A SliderFloat whose value is NOT written into the data while it is dragged:
    // it is shown, previewed and delivered whole on release. This is the case of the
    // Material's Metallic/Roughness, which during the drag only push to the
    // GPU and write the Material (with its undo command) once at the end.
    //
    // It exists because the usual pattern (a local initialized from the data,
    // the widget on top, and the commit in IsItemDeactivatedAfterEdit reading that
    // local) does NOT work when the data is not written live. On the frame the
    // mouse is released, SliderBehaviorT only calls ClearActiveID() and does not touch the
    // value, so the local holds what the data said BEFORE the drag: the
    // commit saw "nothing changed" and nothing happened, while the GPU
    // kept the last thing pushed to it. The object looked right and the
    // slider, the undo and the .scene stayed at the old value.
    //
    // Sliders that DO write live (Audio Clip, the render settings)
    // do not need it: for them the data already carries the value on release.
    //
    // The pending value lives here, between frames, like m_ssaaPendingFactor
    // in RenderingPanel. A single instance works for several sliders: only one
    // ImGui widget can be active at a time, and the id says which.
    //
    // No ImGui in the header, like RenderSettingControls: the body goes in the .cpp.
    class DeferredSliderFloat
    {
        public:
            struct Result
            {
                bool  activated = false;   // dragging started this frame
                bool  active    = false;   // is being dragged (value = under the cursor)
                bool  committed = false;   // released after editing: value is the final one
                bool  cancelled = false;   // released without editing
                float begin     = 0.0f;    // the data BEFORE the click (not the jumped one)
                float value     = 0.0f;    // what the widget shows this frame
            };

            // `current` is the data's value, read before drawing: SliderFloat
            // jumps to the value under the cursor in the same frame as the click, and `begin`
            // has to be the one from before that jump.
            Result draw(const char* label, float current, float lo, float hi, const char* fmt);

        private:
            unsigned int m_activeId        = 0;
            int          m_lastActiveFrame = -1;
            float        m_begin           = 0.0f;
            float        m_pending         = 0.0f;
    };
}
