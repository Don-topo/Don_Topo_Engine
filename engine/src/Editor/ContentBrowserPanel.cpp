#include "DonTopo/Editor/ContentBrowserPanel.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Renderer/Renderer.h"
#include <imgui.h>
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <set>

namespace {

std::string trim(const std::string& name)
{
    size_t begin = name.find_first_not_of(" \t");
    size_t end   = name.find_last_not_of(" \t");
    return name.substr(begin, end - begin + 1);
}

// Compara dos paths de forma robusta a mayúsc/minúsc (Windows es
// case-insensitive pero std::filesystem::path::operator== no lo es) y a
// formato relativo/absoluto (weakly_canonical antes de comparar).
bool samePath(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::error_code ecA, ecB;
    std::filesystem::path ca = std::filesystem::weakly_canonical(a, ecA);
    std::filesystem::path cb = std::filesystem::weakly_canonical(b, ecB);
    std::string sa = (ecA ? a : ca).string();
    std::string sb = (ecB ? b : cb).string();
    std::transform(sa.begin(), sa.end(), sa.begin(), ::tolower);
    std::transform(sb.begin(), sb.end(), sb.begin(), ::tolower);
    return sa == sb;
}

// true si p está estrictamente dentro de dir (p == dir cuenta como false).
bool pathUnderDir(const std::filesystem::path& p, const std::filesystem::path& dir)
{
    std::error_code ecP, ecD;
    std::filesystem::path cp = std::filesystem::weakly_canonical(p, ecP);
    std::filesystem::path cd = std::filesystem::weakly_canonical(dir, ecD);
    std::string sp = (ecP ? p : cp).string();
    std::string sd = (ecD ? dir : cd).string();
    std::transform(sp.begin(), sp.end(), sp.begin(), ::tolower);
    std::transform(sd.begin(), sd.end(), sd.begin(), ::tolower);
    if (sp.size() <= sd.size() || sp.compare(0, sd.size(), sd) != 0)
        return false;
    char sep = sp[sd.size()];
    return sep == '\\' || sep == '/';
}

// Sustituye el prefijo oldDir por newDir en original. Asume que
// pathUnderDir(original, oldDir) ya dio true.
std::string replacePathPrefix(const std::string& original,
                               const std::filesystem::path& oldDir,
                               const std::filesystem::path& newDir)
{
    std::error_code ecO, ecD;
    std::filesystem::path canonOriginal = std::filesystem::weakly_canonical(std::filesystem::path(original), ecO);
    std::filesystem::path canonOldDir   = std::filesystem::weakly_canonical(oldDir, ecD);
    std::string canonOriginalStr = ecO ? original      : canonOriginal.string();
    std::string canonOldDirStr   = ecD ? oldDir.string() : canonOldDir.string();
    if (canonOriginalStr.size() <= canonOldDirStr.size())
        return newDir.string(); // defensive: pathUnderDir should already guarantee original is strictly under oldDir
    return newDir.string() + canonOriginalStr.substr(canonOldDirStr.size());
}

// Nombre de fichero/carpeta válido: no vacío tras trim, sin separadores de
// path ni caracteres reservados de Windows.
bool isValidFileName(const std::string& name)
{
    if (name.empty())
        return false;
    static const std::string kReserved = "\\/:*?\"<>|";
    for (char c : name)
        if (kReserved.find(c) != std::string::npos)
            return false;
    return true;
}

} // namespace

namespace DonTopo {

std::vector<std::filesystem::path> listVisibleSubdirs(const std::filesystem::path& dir)
{
    static const std::set<std::string> kHiddenDirs = { "build-ninja" };

    std::vector<std::filesystem::path> out;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) return out; // no existe, es un fichero o no hay permisos

    for (const auto& entry : it)
    {
        if (!entry.is_directory(ec) || ec) { ec.clear(); continue; }
        std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.' || kHiddenDirs.count(name)) continue;
        out.push_back(entry.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

void ContentBrowserPanel::beginAssetRename(const std::filesystem::path& path, bool isDir)
{
    m_assetRenameTarget = path;
    m_assetRenameIsDir  = isDir;
    m_assetRenameError.clear();
    std::string prefill = isDir ? path.filename().string() : path.stem().string();
    std::strncpy(m_assetRenameBuffer, prefill.c_str(), sizeof(m_assetRenameBuffer) - 1);
    m_assetRenameBuffer[sizeof(m_assetRenameBuffer) - 1] = '\0';
    m_openAssetRenamePopup = true;
}

void ContentBrowserPanel::updateSceneReferencesForRename(EditorContext& ctx, GameObject* sceneRoot,
                                                           const std::filesystem::path& oldPath,
                                                           const std::filesystem::path& newPath,
                                                           bool isDir)
{
    (void)ctx;
    if (!sceneRoot) return;

    sceneRoot->traverse([&](GameObject* go)
    {
        auto updateField = [&](std::string& field)
        {
            if (field.empty()) return;
            bool matches = isDir ? pathUnderDir(field, oldPath) : samePath(field, oldPath);
            if (matches)
                field = isDir ? replacePathPrefix(field, oldPath, newPath) : newPath.string();
        };

        if (go->hasMesh())
        {
            Mesh* mesh = go->getMesh().get();
            updateField(mesh->sourcePath);
            updateField(mesh->material.texturePath);
            updateField(mesh->material.normalMapPath);
            updateField(mesh->material.metallicRoughnessPath);
        }
        if (go->hasAudioClip())
        {
            std::string audioPath = go->getAudioClip()->getPath();
            bool matches = isDir ? pathUnderDir(audioPath, oldPath) : samePath(audioPath, oldPath);
            if (matches)
            {
                std::string newAudioPath = isDir ? replacePathPrefix(audioPath, oldPath, newPath) : newPath.string();
                go->getAudioClip()->setPath(newAudioPath);
            }
        }
    });
}

int ContentBrowserPanel::countSceneReferences(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir)
{
    if (!sceneRoot) return 0;

    int count = 0;
    sceneRoot->traverse([&](GameObject* go)
    {
        auto matches = [&](const std::string& field)
        {
            if (field.empty()) return false;
            return isDir ? pathUnderDir(field, path) : samePath(field, path);
        };

        bool meshMatches = go->hasMesh() &&
            (matches(go->getMesh()->sourcePath) ||
             matches(go->getMesh()->material.texturePath) ||
             matches(go->getMesh()->material.normalMapPath) ||
             matches(go->getMesh()->material.metallicRoughnessPath));
        bool audioMatches = go->hasAudioClip() && matches(go->getAudioClip()->getPath());
        if (meshMatches || audioMatches)
            ++count;
    });
    return count;
}

void ContentBrowserPanel::detachSceneReferencesForDelete(EditorContext& ctx, GameObject* sceneRoot,
                                                           const std::filesystem::path& path, bool isDir)
{
    if (!sceneRoot) return;

    sceneRoot->traverse([&](GameObject* go)
    {
        auto matches = [&](const std::string& field)
        {
            if (field.empty()) return false;
            return isDir ? pathUnderDir(field, path) : samePath(field, path);
        };

        if (go->hasMesh())
        {
            Mesh* mesh = go->getMesh().get();
            if (matches(mesh->sourcePath))
            {
                if (ctx.renderer)
                    ctx.renderer->removeMeshComponent(go);
            }
            else if (ctx.renderer && go->staticRenderIndex >= 0)
            {
                if (matches(mesh->material.texturePath))
                {
                    mesh->material.texturePath.clear();
                    ctx.renderer->replaceStaticTextureWithMissing(go->staticRenderIndex, Renderer::TextureSlot::Diffuse);
                }
                if (matches(mesh->material.normalMapPath))
                {
                    mesh->material.normalMapPath.clear();
                    ctx.renderer->replaceStaticTextureWithMissing(go->staticRenderIndex, Renderer::TextureSlot::Normal);
                }
                if (matches(mesh->material.metallicRoughnessPath))
                {
                    mesh->material.metallicRoughnessPath.clear();
                    ctx.renderer->replaceStaticTextureWithMissing(go->staticRenderIndex, Renderer::TextureSlot::MetallicRoughness);
                }
            }
        }
        if (go->hasAudioClip() && matches(go->getAudioClip()->getPath()))
        {
            go->setAudioClip(nullptr);
        }
    });
}

void ContentBrowserPanel::beginAssetDelete(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir)
{
    m_assetDeleteTarget         = path;
    m_assetDeleteIsDir          = isDir;
    m_assetDeleteAffectedCount  = countSceneReferences(sceneRoot, path, isDir);
    m_assetDeleteError.clear();
    m_openAssetDeletePopup      = true;
}

void ContentBrowserPanel::draw(EditorContext& ctx, GameObject* sceneRoot)
{
    if (!m_open) return;
    ImGui::Begin("Content Browser", &m_open);
    float totalWidth  = ImGui::GetContentRegionAvail().x;
    float totalHeight = ImGui::GetContentRegionAvail().y;
    float leftWidth   = totalWidth * 0.38f;

    if (m_projectRoot.empty())
        m_projectRoot = std::filesystem::canonical(std::filesystem::current_path());

    // Left: ImGuiFileDialog embedded
    ImGui::BeginChild("##FileDlgPane", ImVec2(leftWidth, totalHeight), false);
    {
        if (!m_dlgOpen) {
            IGFD::FileDialogConfig cfg;
            cfg.path  = m_dlgReopenPath.empty() ? m_projectRoot.string() : m_dlgReopenPath;
            cfg.flags = ImGuiFileDialogFlags_NoDialog |
                        ImGuiFileDialogFlags_DontShowHiddenFiles |
                        ImGuiFileDialogFlags_HideColumnType |
                        ImGuiFileDialogFlags_HideColumnDate |
                        ImGuiFileDialogFlags_DisableThumbnailMode |
                        ImGuiFileDialogFlags_DisablePlaceMode;
            IGFD::FileDialog::Instance()->OpenDialog(
                "##ContentDlg", "Files", ".*", cfg);
            m_dlgOpen = true;
            m_dlgReopenPath.clear();
        }
        ImVec2 dlgSize = ImGui::GetContentRegionAvail();
        if (IGFD::FileDialog::Instance()->Display(
                "##ContentDlg", ImGuiWindowFlags_None, dlgSize, dlgSize))
        {
            IGFD::FileDialog::Instance()->Close();
            m_dlgOpen = false;
        }

        // Clamp: si el usuario navegó por encima de la raíz (".." o
        // breadcrumb), reabrir el diálogo anclado en m_projectRoot.
        if (m_dlgOpen) {
            std::error_code ec;
            std::string     rawPath = IGFD::FileDialog::Instance()->GetCurrentPath();
            std::filesystem::path canon =
                std::filesystem::weakly_canonical(std::filesystem::path(rawPath), ec);
            bool insideRoot = !ec && std::mismatch(m_projectRoot.begin(), m_projectRoot.end(),
                                                    canon.begin(), canon.end())
                                          .first == m_projectRoot.end();
            if (!insideRoot) {
                IGFD::FileDialog::Instance()->Close();
                m_dlgOpen = false;
                m_dlgReopenPath.clear();
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right: Asset browser with type icons
    ImGui::BeginChild("##AssetPane", ImVec2(0, totalHeight), false);
    {
        std::string browsedDir = IGFD::FileDialog::Instance()->GetCurrentPath();
        if (browsedDir.empty()) browsedDir = "assets";
        if (browsedDir != m_currentDir) {
            m_currentDir = browsedDir;
            m_scanned = false;
        }

        if (!m_scanned) {
            m_assets.clear();
            if (std::filesystem::exists(m_currentDir))
                for (auto& e : std::filesystem::directory_iterator(m_currentDir))
                    if (e.is_regular_file() || e.is_directory())
                        m_assets.push_back(e.path());
            std::sort(m_assets.begin(), m_assets.end());
            m_scanned = true;
        }

        constexpr float ICON_SIZE = 56.0f;
        constexpr float CELL_PAD  = 12.0f;
        float cellW = ICON_SIZE + CELL_PAD;
        float paneW = ImGui::GetContentRegionAvail().x;
        int   cols  = std::max(1, (int)(paneW / cellW));
        ImGui::Columns(cols, "##AssetGrid", false);

        static const std::set<std::string> kDraggableExt = {".fbx", ".wav", ".mp3", ".ogg", ".flac"};

        for (auto& path : m_assets) {
            bool isDir = std::filesystem::is_directory(path);
            std::string ext = isDir ? "" : path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            ImVec4      btnColor;
            const char* label;
            if (isDir) {
                btnColor = ImVec4(0.55f, 0.55f, 0.60f, 1.0f); label = "DIR";
            } else if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb") {
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

            if (isDir && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                m_dlgReopenPath = path.string();
                IGFD::FileDialog::Instance()->Close();
                m_dlgOpen    = false;
                m_currentDir = path.string();
                m_scanned    = false;
            }

            if (!isDir && ext == ".lua" && ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                ctx.openScript(path);
            }

            if (!isDir && kDraggableExt.count(ext) && ImGui::BeginDragDropSource())
            {
                std::string fullPath = path.string();
                ImGui::SetDragDropPayload("DT_ASSET_PATH", fullPath.c_str(), fullPath.size() + 1);
                ImGui::Text("%s", fullPath.c_str());
                ImGui::EndDragDropSource();
            }

            if (ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Rename"))
                    beginAssetRename(path, isDir);
                if (ImGui::MenuItem("Delete"))
                    beginAssetDelete(sceneRoot, path, isDir);
                ImGui::EndPopup();
            }

            std::string fname = path.filename().string();
            if (fname.size() > 11) fname = fname.substr(0, 10) + "..";
            ImGui::TextUnformatted(fname.c_str());

            ImGui::NextColumn();
            ImGui::PopID();
        }
        ImGui::Columns(1);

        if (m_openAssetRenamePopup)
        {
            ImGui::OpenPopup("Rename Asset");
            m_openAssetRenamePopup = false;
        }
        if (ImGui::BeginPopupModal("Rename Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();

            bool enterPressed = ImGui::InputText("##assetRenameInput", m_assetRenameBuffer,
                                                  sizeof(m_assetRenameBuffer),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
            if (!m_assetRenameIsDir)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", m_assetRenameTarget.extension().string().c_str());
            }
            if (!m_assetRenameError.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_assetRenameError.c_str());
            ImGui::Separator();
            bool accept = ImGui::Button("Accept") || enterPressed;
            ImGui::SameLine();
            bool cancel = ImGui::Button("Cancel");

            if (accept)
            {
                std::string newStem = trim(m_assetRenameBuffer);
                if (!isValidFileName(newStem))
                {
                    m_assetRenameError = "Nombre invalido";
                }
                else
                {
                    std::string newName = m_assetRenameIsDir
                        ? newStem
                        : (newStem + m_assetRenameTarget.extension().string());
                    std::filesystem::path newPath = m_assetRenameTarget.parent_path() / newName;
                    std::error_code existsEc;
                    if (!samePath(newPath, m_assetRenameTarget) && std::filesystem::exists(newPath, existsEc))
                    {
                        m_assetRenameError = "Ya existe un fichero/carpeta con ese nombre";
                    }
                    else
                    {
                        std::error_code renameEc;
                        std::filesystem::rename(m_assetRenameTarget, newPath, renameEc);
                        if (renameEc)
                        {
                            m_assetRenameError = renameEc.message();
                        }
                        else
                        {
                            ctx.pushLog("Asset renombrado: '" + m_assetRenameTarget.filename().string() +
                                    "' -> '" + newPath.filename().string() + "'");
                            updateSceneReferencesForRename(ctx, sceneRoot, m_assetRenameTarget, newPath, m_assetRenameIsDir);
                            m_scanned       = false;
                            m_dlgReopenPath = m_currentDir;
                            m_dlgOpen       = false;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
            }
            else if (cancel)
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (m_openAssetDeletePopup)
        {
            ImGui::OpenPopup("Delete Asset");
            m_openAssetDeletePopup = false;
        }
        if (ImGui::BeginPopupModal("Delete Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Borrar '%s'?", m_assetDeleteTarget.filename().string().c_str());
            if (m_assetDeleteAffectedCount > 0)
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                    "%d objeto(s) lo usan y perderan la referencia.", m_assetDeleteAffectedCount);
            if (!m_assetDeleteError.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_assetDeleteError.c_str());
            ImGui::Separator();
            bool confirm = ImGui::Button("Borrar");
            ImGui::SameLine();
            bool cancel = ImGui::Button("Cancelar");

            if (confirm)
            {
                std::error_code removeEc;
                if (m_assetDeleteIsDir)
                    std::filesystem::remove_all(m_assetDeleteTarget, removeEc);
                else
                    std::filesystem::remove(m_assetDeleteTarget, removeEc);

                if (removeEc)
                {
                    m_assetDeleteError = removeEc.message();
                }
                else
                {
                    ctx.pushLog("Asset eliminado: " + m_assetDeleteTarget.string());
                    detachSceneReferencesForDelete(ctx, sceneRoot, m_assetDeleteTarget, m_assetDeleteIsDir);
                    m_scanned       = false;
                    m_dlgReopenPath = m_currentDir;
                    m_dlgOpen       = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            else if (cancel)
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace DonTopo
