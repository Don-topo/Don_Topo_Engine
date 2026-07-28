#include "DonTopo/Editor/LoadingModal.h"

#include <imgui.h>

#include <algorithm>

namespace DonTopo
{
    void LoadingModal::begin(int total)
    {
        if (total <= 0) return;   // nada que cargar: no se abre el modal
        m_active = true;
        m_total  = total;
        m_done   = 0;
    }

    void LoadingModal::update(int pending)
    {
        if (!m_active) return;
        m_done = std::max(0, m_total - pending);
        if (pending <= 0) m_active = false;
    }

    bool LoadingModal::draw()
    {
        if (!m_active) return false;

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::Begin("##loading", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration
                     | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

        ImGui::Text("Cargando escena...  %d / %d", m_done, m_total);
        const float frac = (m_total > 0) ? (float)m_done / (float)m_total : 0.0f;
        ImGui::ProgressBar(frac, ImVec2(320.0f, 0.0f));

        const bool cancelled = ImGui::Button("Cancelar");
        ImGui::End();

        // Cancelar deja la escena con lo cargado hasta aquí. Es un estado
        // válido y guardable, no una escena a medias que haya que tirar.
        if (cancelled) m_active = false;
        return cancelled;
    }
}
