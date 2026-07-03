#include "DonTopo/EditorUI.h"
#include <imgui.h>
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <filesystem>

namespace DonTopo {

void EditorUI::draw(VkDescriptorSet viewportTexture,
                    const std::vector<std::string>& staticNames,
                    const std::vector<std::string>& skinnedNames)
{
    drawDockSpace();
    drawScene(staticNames, skinnedNames);
    drawViewport(viewportTexture);
    drawProperties();
    drawContentBrowser();
}

void EditorUI::drawDockSpace()
{
    ImGuiWindowFlags dockFlags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##DockSpace", nullptr, dockFlags);
    ImGui::PopStyleVar(3);
    ImGui::DockSpace(ImGui::GetID("MainDockSpace"), ImVec2(0, 0), ImGuiDockNodeFlags_None);
    ImGui::End();
}

void EditorUI::drawScene(const std::vector<std::string>& staticNames,
                         const std::vector<std::string>& skinnedNames)
{
    ImGui::Begin("Scene");
    if (ImGui::CollapsingHeader("Static Meshes", ImGuiTreeNodeFlags_DefaultOpen))
        for (const auto& name : staticNames)
            ImGui::Selectable(name.c_str());
    if (ImGui::CollapsingHeader("Skinned Meshes", ImGuiTreeNodeFlags_DefaultOpen))
        for (const auto& name : skinnedNames)
            ImGui::Selectable(name.c_str());
    ImGui::End();
}

void EditorUI::drawViewport(VkDescriptorSet viewportTexture)
{
    ImGui::Begin("Viewport");
    ImVec2 vpSize = ImGui::GetContentRegionAvail();
    ImGui::Image((ImTextureID)(intptr_t)viewportTexture, vpSize);
    ImGui::End();
}

void EditorUI::drawProperties()
{
    ImGui::Begin("Properties");
    ImGui::Text("Transform");
    ImGui::Separator();

    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNodeEx("Transform", ImGuiTreeNodeFlags_OpenOnArrow))
    {
        ImGui::Text("Position");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        ImGui::DragFloat("X##1", &m_pos[0], 0.005f, -FLT_MAX, +FLT_MAX, "% .3f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        ImGui::DragFloat("Y##1", &m_pos[1], 0.005f, -FLT_MAX, +FLT_MAX, "% .3f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        ImGui::DragFloat("Z##1", &m_pos[2], 0.005f, -FLT_MAX, +FLT_MAX, "% .3f");

        ImGui::Text("Rotation");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        ImGui::DragFloat("X##2", &m_rot[0], 0.005f, -FLT_MAX, +FLT_MAX, "% .3f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        ImGui::DragFloat("Y##2", &m_rot[1], 0.005f, -FLT_MAX, +FLT_MAX, "% .3f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        ImGui::DragFloat("Z##2", &m_rot[2], 0.005f, -FLT_MAX, +FLT_MAX, "% .3f");

        ImGui::Text("Scale   ");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        ImGui::DragFloat("X##3", &m_scl[0], 0.005f, -FLT_MAX, +FLT_MAX, "% .3f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        ImGui::DragFloat("Y##3", &m_scl[1], 0.005f, -FLT_MAX, +FLT_MAX, "% .3f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        ImGui::DragFloat("Z##3", &m_scl[2], 0.005f, -FLT_MAX, +FLT_MAX, "% .3f");

        ImGui::TreePop();
    }
    ImGui::End();
}

void EditorUI::drawContentBrowser()
{
    ImGui::Begin("Content Browser");
    float totalWidth  = ImGui::GetContentRegionAvail().x;
    float totalHeight = ImGui::GetContentRegionAvail().y;
    float leftWidth   = totalWidth * 0.38f;

    // Left: ImGuiFileDialog embedded
    ImGui::BeginChild("##FileDlgPane", ImVec2(leftWidth, totalHeight), false);
    {
        if (!m_dlgOpen) {
            IGFD::FileDialogConfig cfg;
            cfg.path  = "assets";
            cfg.flags = ImGuiFileDialogFlags_NoDialog |
                        ImGuiFileDialogFlags_DontShowHiddenFiles;
            IGFD::FileDialog::Instance()->OpenDialog(
                "##ContentDlg", "Files", nullptr, cfg);
            m_dlgOpen = true;
        }
        ImVec2 dlgSize = ImGui::GetContentRegionAvail();
        if (IGFD::FileDialog::Instance()->Display(
                "##ContentDlg", ImGuiWindowFlags_None, dlgSize, dlgSize))
        {
            IGFD::FileDialog::Instance()->Close();
            m_dlgOpen = false;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right: Asset browser with type icons
    ImGui::BeginChild("##AssetPane", ImVec2(0, totalHeight), false);
    {
        if (!m_scanned) {
            m_assets.clear();
            if (std::filesystem::exists("assets"))
                for (auto& e : std::filesystem::directory_iterator("assets"))
                    if (e.is_regular_file())
                        m_assets.push_back(e.path());
            std::sort(m_assets.begin(), m_assets.end());
            m_scanned = true;
        }

        if (ImGui::SmallButton("Refresh")) m_scanned = false;
        ImGui::Separator();

        constexpr float ICON_SIZE = 56.0f;
        constexpr float CELL_PAD  = 12.0f;
        float cellW = ICON_SIZE + CELL_PAD;
        float paneW = ImGui::GetContentRegionAvail().x;
        int   cols  = std::max(1, (int)(paneW / cellW));
        ImGui::Columns(cols, "##AssetGrid", false);

        for (auto& path : m_assets) {
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            ImVec4      btnColor;
            const char* label;
            if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb") {
                btnColor = ImVec4(0.15f, 0.55f, 0.85f, 1.0f); label = "3D";
            } else if (ext == ".mp3" || ext == ".wav" || ext == ".ogg" || ext == ".flac") {
                btnColor = ImVec4(0.20f, 0.72f, 0.35f, 1.0f); label = "SFX";
            } else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") {
                btnColor = ImVec4(0.85f, 0.72f, 0.10f, 1.0f); label = "IMG";
            } else if (ext == ".spv") {
                btnColor = ImVec4(0.80f, 0.35f, 0.10f, 1.0f); label = "SPV";
            } else {
                btnColor = ImVec4(0.40f, 0.40f, 0.40f, 1.0f); label = "...";
            }

            ImGui::PushID(path.string().c_str());
            ImGui::PushStyleColor(ImGuiCol_Button, btnColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(btnColor.x + 0.15f, btnColor.y + 0.15f, btnColor.z + 0.15f, 1.0f));
            ImGui::Button(label, ImVec2(ICON_SIZE, ICON_SIZE));
            ImGui::PopStyleColor(2);

            std::string fname = path.filename().string();
            if (fname.size() > 11) fname = fname.substr(0, 10) + "..";
            ImGui::TextUnformatted(fname.c_str());

            ImGui::NextColumn();
            ImGui::PopID();
        }
        ImGui::Columns(1);
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace DonTopo
