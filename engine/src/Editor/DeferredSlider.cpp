#include "DonTopo/Editor/DeferredSlider.h"

#include <imgui.h>

namespace DonTopo
{
    DeferredSliderFloat::Result DeferredSliderFloat::draw(const char* label, float current,
                                                          float lo, float hi, const char* fmt)
    {
        const unsigned int id    = ImGui::GetID(label);
        const int          frame = ImGui::GetFrameCount();

        // The pending value is only shown if THIS widget was active the previous frame:
        // the release frame needs it (ImGui does not write the value that frame), but if
        // the widget disappeared mid-drag (the selection changed to an object without a
        // mesh) m_activeId stays set, and without checking the frame the slider would
        // show a value nobody applied.
        const bool nuestro = m_activeId == id && m_lastActiveFrame == frame - 1;
        // And it is FORGOTTEN, not just no longer shown: with m_activeId still pointing
        // here, the rest of the function would keep returning the abandoned pending value
        // as `value`.
        if (m_activeId == id && !nuestro) m_activeId = 0;
        float v = nuestro ? m_pending : current;
        const bool changed = ImGui::SliderFloat(label, &v, lo, hi, fmt);

        Result r;
        if (ImGui::IsItemActivated())
        {
            m_activeId  = id;
            m_begin     = current;
            r.activated = true;
        }
        if (m_activeId != id)
        {
            r.value = v;
            return r;
        }

        // `changed` in addition to IsItemActive: text editing (Ctrl+click) can deliver
        // the value in the same frame in which it stops being active.
        const bool active = ImGui::IsItemActive();
        if (changed || active) m_pending = v;
        if (active) m_lastActiveFrame = frame;

        r.active = active;
        r.begin  = m_begin;
        r.value  = m_pending;
        if (ImGui::IsItemDeactivated())
        {
            r.committed = ImGui::IsItemDeactivatedAfterEdit();
            r.cancelled = !r.committed;
            m_activeId  = 0;
        }
        return r;
    }
}
