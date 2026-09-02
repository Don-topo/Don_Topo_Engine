#include "DonTopo/Editor/RenderSettingControls.h"

#include <imgui.h>

namespace DonTopo
{
    void RenderSettingControls::sliderFloat(const char* label, float lo, float hi, const char* fmt,
                                            const std::function<float()>& get,
                                            const std::function<void(float)>& set)
    {
        const float prev = get();
        float v = prev;
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::SliderFloat(label, &v, lo, hi, fmt))
            set(v);

        const unsigned int id = ImGui::GetItemID();
        if (ImGui::IsItemActive() && m_activeId != id)
        {
            m_activeId    = id;
            m_beginScalar = prev;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            pushUndo<float>(label, m_beginScalar, get(), set);
        if (ImGui::IsItemDeactivated())
            m_activeId = 0;
    }

    void RenderSettingControls::sliderInt(const char* label, int lo, int hi,
                                          const std::function<int()>& get,
                                          const std::function<void(int)>& set)
    {
        const int prev = get();
        int v = prev;
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::SliderInt(label, &v, lo, hi))
            set(v);

        const unsigned int id = ImGui::GetItemID();
        if (ImGui::IsItemActive() && m_activeId != id)
        {
            m_activeId = id;
            m_beginInt = prev;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            pushUndo<int>(label, m_beginInt, get(), set);
        if (ImGui::IsItemDeactivated())
            m_activeId = 0;
    }

    // Sin arrastre que esperar: el click ya es el cambio entero, así que el
    // comando se empuja en ese mismo frame. Devuelve el valor vigente porque los
    // llamantes lo usan acto seguido para el BeginDisabled de los sliders de su
    // efecto.
    bool RenderSettingControls::checkbox(const char* label, const std::function<bool()>& get,
                                         const std::function<void(bool)>& set)
    {
        bool v = get();
        if (ImGui::Checkbox(label, &v))
        {
            set(v);
            pushUndo<bool>(label, !v, v, set);
        }
        return v;
    }

    void RenderSettingControls::colorEdit3(const char* label,
                                           const std::function<glm::vec3()>& get,
                                           const std::function<void(const glm::vec3&)>& set)
    {
        const glm::vec3 prev = get();
        glm::vec3 v = prev;
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::ColorEdit3(label, &v.x))
            set(v);

        const unsigned int id = ImGui::GetItemID();
        if (ImGui::IsItemActive() && m_activeId != id)
        {
            m_activeId   = id;
            m_beginColor = prev;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            pushUndo<glm::vec3>(label, m_beginColor, get(), set);
        if (ImGui::IsItemDeactivated())
            m_activeId = 0;
    }
}
