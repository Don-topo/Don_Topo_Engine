#include "DonTopo/Editor/DeferredSlider.h"

#include <imgui.h>

namespace DonTopo
{
    DeferredSliderFloat::Result DeferredSliderFloat::draw(const char* label, float current,
                                                          float lo, float hi, const char* fmt)
    {
        const unsigned int id    = ImGui::GetID(label);
        const int          frame = ImGui::GetFrameCount();

        // El pendiente solo se enseña si ESTE widget estaba activo el frame
        // anterior: el de soltar lo necesita (ImGui no escribe el valor ese
        // frame), pero si el widget desaparecio a mitad de arrastre -la
        // seleccion cambio a un objeto sin malla- m_activeId se queda puesto, y
        // sin comprobar el frame el slider enseñaria un valor que nadie aplico.
        const bool nuestro = m_activeId == id && m_lastActiveFrame == frame - 1;
        // Y se OLVIDA, no solo se deja de enseñar: con m_activeId todavia
        // apuntando aqui, el resto de la funcion seguiria devolviendo el
        // pendiente abandonado como `value`.
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

        // `changed` ademas de IsItemActive: la edicion por texto (Ctrl+clic)
        // puede entregar el valor en el mismo frame en que deja de estar activa.
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
