#include "DonTopo/ScriptEditorPanel.h"
#include "DonTopo/FileManager.h"
#include <imgui.h>

namespace DonTopo {

void ScriptEditorPanel::openFile(const std::filesystem::path& path)
{
    for (size_t i = 0; i < m_tabs.size(); ++i)
    {
        if (m_tabs[i].path == path)
        {
            m_focusIndex = static_cast<int>(i);
            return;
        }
    }

    std::optional<std::string> content = FileManager::readText(path.string());
    if (!content)
    {
        log("Script Editor: no se pudo abrir '" + path.string() + "'");
        return;
    }

    Tab tab;
    tab.path = path;
    tab.editor.SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
    tab.editor.SetText(*content);
    m_tabs.push_back(std::move(tab));
    m_focusIndex = static_cast<int>(m_tabs.size()) - 1;
}

void ScriptEditorPanel::saveTab(Tab& tab)
{
    if (FileManager::writeText(tab.path.string(), tab.editor.GetText()))
        tab.dirty = false;
    else
        log("Script Editor: no se pudo guardar '" + tab.path.string() + "'");
}

void ScriptEditorPanel::draw()
{
    ImGui::Begin("Script Editor");

    int closeRequested = -1;

    if (ImGui::BeginTabBar("##ScriptEditorTabs", ImGuiTabBarFlags_Reorderable))
    {
        for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
        {
            Tab& tab = m_tabs[i];
            std::string title = tab.path.filename().string() + (tab.dirty ? " *" : "");
            ImGuiTabItemFlags flags = (m_focusIndex == i) ? ImGuiTabItemFlags_SetSelected
                                                           : ImGuiTabItemFlags_None;
            bool open = true;

            ImGui::PushID(i);
            if (ImGui::BeginTabItem(title.c_str(), &open, flags))
            {
                if (ImGui::Button("Save"))
                    saveTab(tab);

                if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                    ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))
                    saveTab(tab);

                tab.editor.Render("##TextEditor", ImGui::GetContentRegionAvail());
                if (tab.editor.IsTextChanged())
                    tab.dirty = true;

                ImGui::EndTabItem();
            }
            ImGui::PopID();

            if (!open)
                closeRequested = i;
        }
        ImGui::EndTabBar();
    }
    m_focusIndex = -1;

    if (closeRequested >= 0)
    {
        if (m_tabs[closeRequested].dirty)
        {
            m_closeConfirmIndex = closeRequested;
            m_openCloseConfirmPopup = true;
        }
        else
            m_tabs.erase(m_tabs.begin() + closeRequested);
    }

    if (m_openCloseConfirmPopup)
    {
        ImGui::OpenPopup("Cambios sin guardar##ScriptEditor");
        m_openCloseConfirmPopup = false;
    }

    if (ImGui::BeginPopupModal("Cambios sin guardar##ScriptEditor", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
    {
        Tab& tab = m_tabs[m_closeConfirmIndex];
        ImGui::Text("'%s' tiene cambios sin guardar.", tab.path.filename().string().c_str());

        if (ImGui::Button("Guardar"))
        {
            saveTab(tab);
            m_tabs.erase(m_tabs.begin() + m_closeConfirmIndex);
            m_closeConfirmIndex = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Descartar"))
        {
            m_tabs.erase(m_tabs.begin() + m_closeConfirmIndex);
            m_closeConfirmIndex = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar"))
        {
            m_closeConfirmIndex = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

} // namespace DonTopo
