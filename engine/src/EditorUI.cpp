#include "DonTopo/EditorUI.h"
#include "DonTopo/Scene.h"
#include "DonTopo/GameObject.h"
#include "DonTopo/PhysicsManager.h"
#include "DonTopo/AudioManager.h"
#include "DonTopo/AudioClipComponent.h"
#include "DonTopo/BoxCollider.h"
#include "DonTopo/SphereCollider.h"
#include "DonTopo/CapsuleCollider.h"
#include "DonTopo/PlaneCollider.h"
#include "DonTopo/Gizmos.h"
#include "DonTopo/Renderer.h"
#include "DonTopo/Camera.h"
#include "DonTopo/Cube.h"
#include "DonTopo/Sphere.h"
#include "DonTopo/Plane.h"
#include "DonTopo/Capsule.h"
#include "DonTopo/ModelLoader.h"
#include "DonTopo/FileManager.h"
#include "DonTopo/ScriptManager.h"
#include "DonTopo/ScriptComponent.h"
#include "DonTopo/ScriptEditorPanel.h"
#include <imgui.h>
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <type_traits>
#include <variant>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/euler_angles.hpp>

namespace {

// Nombre válido: no vacío tras trim, solo alfanuméricos/espacio/_/-/. (sin
// caracteres de control ni símbolos que puedan romper rutas de asset o UI).
bool isValidGameObjectName(const std::string& name)
{
    size_t begin = name.find_first_not_of(" \t");
    size_t end   = name.find_last_not_of(" \t");
    if (begin == std::string::npos)
        return false;

    for (size_t i = begin; i <= end; ++i)
    {
        unsigned char c = static_cast<unsigned char>(name[i]);
        if (!std::isalnum(c) && c != ' ' && c != '_' && c != '-' && c != '.')
            return false;
    }
    return true;
}

std::string trim(const std::string& name)
{
    size_t begin = name.find_first_not_of(" \t");
    size_t end   = name.find_last_not_of(" \t");
    return name.substr(begin, end - begin + 1);
}

// 2 decimales — suficiente para leer el valor de un vistazo en el Log sin
// líneas kilométricas; el panel Properties ya muestra 3 decimales para
// edición fina, el Log es solo un resumen legible.
std::string formatVec3(const glm::vec3& v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f)", v.x, v.y, v.z);
    return buf;
}

std::string formatFloat(float f)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", f);
    return buf;
}

// Mueve dragged pa la posición de target dentro de la lista de hijos de
// target->parent (o al final de target->children si target es el root: así
// nunca puede quedar como hermano del root ni fuera de su subárbol).
void moveGameObject(DonTopo::GameObject* dragged, DonTopo::GameObject* target)
{
    using DonTopo::GameObject;

    if (!dragged || !target || dragged == target || !dragged->parent)
        return; // root (sin parent) no se puede arrastrar

    bool cycle = false;
    dragged->traverse([&](GameObject* go) { if (go == target) cycle = true; });
    if (cycle)
        return; // no soltar un nodo dentro de su propio subárbol

    GameObject* destParent;
    ptrdiff_t destIndex;
    if (!target->parent)
    {
        destParent = target;
        destIndex  = static_cast<ptrdiff_t>(destParent->children.size());
    }
    else
    {
        destParent = target->parent;
        auto it = std::find_if(destParent->children.begin(), destParent->children.end(),
            [target](const std::unique_ptr<GameObject>& c) { return c.get() == target; });
        destIndex = it - destParent->children.begin();
    }

    GameObject* srcParent = dragged->parent;
    auto srcIt = std::find_if(srcParent->children.begin(), srcParent->children.end(),
        [dragged](const std::unique_ptr<GameObject>& c) { return c.get() == dragged; });
    ptrdiff_t srcIndex = srcIt - srcParent->children.begin();

    std::unique_ptr<GameObject> moved = std::move(*srcIt);
    srcParent->children.erase(srcIt);

    if (srcParent == destParent && srcIndex < destIndex)
        --destIndex; // el hueco dejado por el erase desplaza los índices siguientes

    moved->parent = destParent;
    destParent->children.insert(destParent->children.begin() + destIndex, std::move(moved));
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

// "attackRange" -> "Attack Range" (labels de props de scripts)
std::string prettyPropLabel(const std::string& raw)
{
    std::string out;
    for (size_t i = 0; i < raw.size(); ++i)
    {
        char c = raw[i];
        if (i == 0) { out += static_cast<char>(std::toupper(static_cast<unsigned char>(c))); continue; }
        if (std::isupper(static_cast<unsigned char>(c))) out += ' ';
        out += c;
    }
    return out;
}

} // namespace

namespace DonTopo {

EditorUI::EditorUI()
    : m_meshFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_audioFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_sceneFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_scriptEditor(std::make_unique<ScriptEditorPanel>())
{
    m_scriptEditor->setLogCallback([this](const std::string& msg) { pushLog(msg); });
}

EditorUI::~EditorUI() = default;

void EditorUI::draw(VkDescriptorSet viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView)
{
    handleUndoRedoShortcut();
    drawMenuBar();
    drawToolbar();
    drawDockSpace();
    drawScene(sceneRoot);
    drawSelectionGizmo();
    drawViewport(viewportTexture, cameraView);
    drawProperties();
    drawLogPanel();
    drawMeshDialog();
    drawAudioClipDialog();
    drawSceneDialog();
    drawContentBrowser(sceneRoot);
    m_scriptEditor->draw();
}

void EditorUI::handleUndoRedoShortcut()
{
    if (!m_scene || m_isPlaying || !ImGui::GetIO().KeyCtrl || ImGui::GetIO().WantTextInput)
        return;

    if (ImGui::IsKeyPressed(ImGuiKey_Z) && m_undoHistory.canUndo())
    {
        uint64_t prevSelId = m_selected ? m_selected->id : 0;
        m_undoHistory.undo();
        m_selected = prevSelId ? m_scene->findById(prevSelId) : nullptr;
        pushLog("Undo: " + m_undoHistory.lastLabel());
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Y) && m_undoHistory.canRedo())
    {
        uint64_t prevSelId = m_selected ? m_selected->id : 0;
        m_undoHistory.redo();
        m_selected = prevSelId ? m_scene->findById(prevSelId) : nullptr;
        pushLog("Redo: " + m_undoHistory.lastLabel());
    }
}

void EditorUI::drawMenuBar()
{
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem("Scene", nullptr, &m_sceneOpen);
            ImGui::MenuItem("Viewport", nullptr, &m_viewportOpen);
            ImGui::MenuItem("Properties", nullptr, &m_propertiesOpen);
            ImGui::MenuItem("Log", nullptr, &m_logOpen);
            ImGui::MenuItem("Content Browser", nullptr, &m_contentBrowserOpen);
            ImGui::MenuItem("Script Editor", nullptr, m_scriptEditor->GetOpenPtr());
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void EditorUI::drawToolbar()
{
    // vp->WorkPos/WorkSize (no vp->Pos/vp->Size) porque BeginMainMenuBar
    // reserva su franja restando de WorkPos/WorkSize del viewport principal
    // — así la Toolbar queda justo debajo del MenuBar en vez de solaparlo.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, kToolbarHeight));
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse;
    ImGui::Begin("##Toolbar", nullptr, flags);

    bool canPlay = m_scene && m_physics && m_audio && m_renderer;
    ImGui::BeginDisabled(!canPlay);
    if (m_isPlaying)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button("Stop"))
        {
            if (m_scriptManager) m_scriptManager->onPlayStop();
            m_sceneIOError = reloadSceneFromJson(m_playSnapshot) ? "" : "No se pudo restaurar la escena";
            m_isPlaying = false;
            pushLog("Play Mode detenido");
        }
        ImGui::PopStyleColor();
    }
    else
    {
        if (ImGui::Button("Play"))
        {
            m_playSnapshot = m_scene->toJson();
            m_undoHistory.clear();
            m_isPlaying = true;
            if (m_scriptManager) m_scriptManager->onPlayStart();
            pushLog("Play Mode iniciado");
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    bool wireframe = m_renderer && m_renderer->isWireframeMode();
    if (wireframe)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button("Wireframe") && m_renderer)
        m_renderer->setWireframeMode(!wireframe);
    if (wireframe)
        ImGui::PopStyleColor();

    ImGui::SameLine();
    if (ImGui::Button("Save Scene") && m_scene)
    {
        m_sceneDlgOpen   = true;
        m_sceneDlgIsSave = true;
        IGFD::FileDialogConfig cfg;
        cfg.path  = "assets";
        cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                    ImGuiFileDialogFlags_HideColumnDate |
                    ImGuiFileDialogFlags_DisableThumbnailMode |
                    ImGuiFileDialogFlags_DisablePlaceMode |
                    ImGuiFileDialogFlags_ConfirmOverwrite;
        m_sceneFileDialog->OpenDialog("SceneDlg", "Save Scene", ".json", cfg);
    }

    ImGui::SameLine();
    if (ImGui::Button("Load Scene") && m_scene)
    {
        m_sceneDlgOpen   = true;
        m_sceneDlgIsSave = false;
        IGFD::FileDialogConfig cfg;
        cfg.path  = "assets";
        cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                    ImGuiFileDialogFlags_HideColumnDate |
                    ImGuiFileDialogFlags_DisableThumbnailMode |
                    ImGuiFileDialogFlags_DisablePlaceMode;
        m_sceneFileDialog->OpenDialog("SceneDlg", "Load Scene", ".json", cfg);
    }

    if (!m_sceneIOError.empty())
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_sceneIOError.c_str());
    }

    ImGui::End();
}

void EditorUI::drawDockSpace()
{
    ImGuiWindowFlags dockFlags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + kToolbarHeight));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - kToolbarHeight));
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##DockSpace", nullptr, dockFlags);
    ImGui::PopStyleVar(3);
    ImGui::DockSpace(ImGui::GetID("MainDockSpace"), ImVec2(0, 0), ImGuiDockNodeFlags_None);
    ImGui::End();
}

void EditorUI::pushLog(const std::string& message)
{
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm     tmBuf{};
    localtime_s(&tmBuf, &t);
    char timeStr[16];
    std::strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &tmBuf);

    m_logEntries.push_back(std::string("[") + timeStr + "] " + message);
    if (m_logEntries.size() > kLogMaxEntries)
        m_logEntries.pop_front();
}

void EditorUI::drawLogPanel()
{
    if (!m_logOpen) return;
    ImGui::Begin("Log", &m_logOpen);
    for (const auto& line : m_logEntries)
        ImGui::TextUnformatted(line.c_str());

    // Autoscroll: solo si ya estaba al fondo antes de este frame (no pelea
    // con el usuario si sube a revisar historial mientras entran líneas
    // nuevas).
    if (m_logAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    m_logAutoScroll = ImGui::GetScrollY() >= ImGui::GetScrollMaxY();

    ImGui::End();
}

void EditorUI::drawScene(GameObject* sceneRoot)
{
    if (!m_sceneOpen) return;
    ImGui::Begin("Scene", &m_sceneOpen);
    // El root no se dibuja como nodo: la lista muestra directamente sus
    // hijos, root sigue siendo el padre real por debajo (mismo comportamiento
    // de create/delete/rename/reorder que ya tenían).
    if (sceneRoot)
        for (const auto& child : sceneRoot->children)
            drawSceneNode(child.get());

    // Espacio vacío tras la lista: soltar aquí reengancha el nodo arrastrado
    // como hijo directo del root (equivalente a soltar sobre la fila root
    // de antes, ahora que esa fila ya no existe).
    ImGui::Dummy(ImGui::GetContentRegionAvail());
    if (ImGui::IsItemClicked())
        m_selected = nullptr; // clic en zona vacía deselecciona
    if (sceneRoot && ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DT_GAMEOBJECT"))
        {
            m_pendingMoveSource = *(GameObject**)payload->Data;
            m_pendingMoveTarget = sceneRoot;
        }
        ImGui::EndDragDropTarget();
    }

    bool canDelete = m_selected && m_selected->parent != nullptr;
    bool canRename = m_selected && m_selected->parent != nullptr;

    if (ImGui::IsWindowFocused() && canDelete && ImGui::IsKeyPressed(ImGuiKey_Delete))
        m_pendingDelete = m_selected;
    if (ImGui::IsWindowFocused() && canRename && ImGui::IsKeyPressed(ImGuiKey_F2))
        beginRename(m_selected);

    if (ImGui::BeginPopupContextWindow("##SceneContext",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        if (ImGui::MenuItem("Create GameObject") && sceneRoot)
        {
            GameObject* created = sceneRoot->addChild("GameObject");
            pushLog("GameObject '" + created->name + "' creado");
        }
        if (ImGui::BeginMenu("Basic Shapes"))
        {
            if (ImGui::MenuItem("Cube"))
                createBasicShape(sceneRoot, "Cube", std::make_shared<Mesh>(Cube::create(50.0f)));
            if (ImGui::MenuItem("Sphere"))
                createBasicShape(sceneRoot, "Sphere", std::make_shared<Mesh>(Sphere::create(50.0f)));
            if (ImGui::MenuItem("Plane"))
                createBasicShape(sceneRoot, "Plane", std::make_shared<Mesh>(Plane::create(50.0f, 0.0f)));
            if (ImGui::MenuItem("Capsule"))
                createBasicShape(sceneRoot, "Capsule", std::make_shared<Mesh>(Capsule::create(25.0f, 50.0f)));
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Rename", nullptr, false, canRename))
            beginRename(m_selected);
        if (ImGui::MenuItem("Delete GameObject", nullptr, false, canDelete))
            m_pendingDelete = m_selected;
        ImGui::EndPopup();
    }

    // Ejecutar el borrado tras recorrer todo el árbol: hacerlo antes
    // invalidaría los for-range de children en curso en la pila de llamadas.
    if (m_pendingDelete)
    {
        GameObject* target = m_pendingDelete;
        m_pendingDelete = nullptr;

        // La selección puede ser el propio target o un descendiente suyo;
        // hay que comprobarlo antes de borrar el subárbol (después ya no existe).
        bool selectionInSubtree = false;
        target->traverse([&](GameObject* go) {
            if (go == m_selected) selectionInSubtree = true;
        });

        pushLog("GameObject '" + target->name + "' eliminado");

        if (m_onDelete)
            m_onDelete(target);

        // Sin esto, borrar desde el editor en Play salta OnDestroy y deja
        // punteros muertos en el alive-set hasta el siguiente update
        // (ventana de use-after-free vía hot reload).
        if (m_isPlaying && m_scriptManager)
        {
            // Snapshot antes de llamar a Lua — OnDestroy puede añadir
            // componentes e invalidar la iteración.
            std::vector<ScriptComponent*> subtreeScripts;
            target->traverse([&](GameObject* n) {
                for (auto& s : n->getScripts())
                    subtreeScripts.push_back(s.get());
            });
            for (ScriptComponent* s : subtreeScripts)
                m_scriptManager->callOnDestroy(*s);
        }

        assert(m_scene && "EditorUI::m_scene debe estar asignado (ver Renderer::setScene) antes de borrar GameObjects");
        m_scene->removeGameObject(target);
        if (m_scriptManager)
            m_scriptManager->rebuildAliveSet();
        if (selectionInSubtree)
        {
            m_selected = nullptr;
            m_propsCachedFor = nullptr;
            m_colliderCachedFor = nullptr;
        }
    }

    if (m_pendingMoveSource && m_pendingMoveTarget)
    {
        moveGameObject(m_pendingMoveSource, m_pendingMoveTarget);
        m_pendingMoveSource = nullptr;
        m_pendingMoveTarget = nullptr;
    }

    if (m_openRenamePopup)
    {
        ImGui::OpenPopup("Rename GameObject");
        m_openRenamePopup = false;
    }
    if (ImGui::BeginPopupModal("Rename GameObject", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();

        bool enterPressed = ImGui::InputText("##renameInput", m_renameBuffer, sizeof(m_renameBuffer),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Separator();
        bool accept = ImGui::Button("Accept") || enterPressed;
        ImGui::SameLine();
        bool cancel = ImGui::Button("Cancel");

        if (accept)
        {
            std::string newName = trim(m_renameBuffer);
            if (m_renameTarget && isValidGameObjectName(newName))
            {
                std::string oldName  = m_renameTarget->name;
                m_renameTarget->name = newName;
                pushLog("GameObject renombrado: '" + oldName + "' -> '" + newName + "'");
            }
            m_renameTarget = nullptr;
            ImGui::CloseCurrentPopup();
        }
        else if (cancel)
        {
            m_renameTarget = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

void EditorUI::beginRename(GameObject* node)
{
    if (!node || !node->parent)
        return; // root no se puede renombrar

    m_renameTarget = node;
    std::string current = node->name.empty() ? "GameObject" : node->name;
    std::strncpy(m_renameBuffer, current.c_str(), sizeof(m_renameBuffer) - 1);
    m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
    m_openRenamePopup = true;
}

void EditorUI::beginAssetRename(const std::filesystem::path& path, bool isDir)
{
    m_assetRenameTarget = path;
    m_assetRenameIsDir  = isDir;
    m_assetRenameError.clear();
    std::string prefill = isDir ? path.filename().string() : path.stem().string();
    std::strncpy(m_assetRenameBuffer, prefill.c_str(), sizeof(m_assetRenameBuffer) - 1);
    m_assetRenameBuffer[sizeof(m_assetRenameBuffer) - 1] = '\0';
    m_openAssetRenamePopup = true;
}

void EditorUI::updateSceneReferencesForRename(GameObject* sceneRoot,
                                               const std::filesystem::path& oldPath,
                                               const std::filesystem::path& newPath,
                                               bool isDir)
{
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

int EditorUI::countSceneReferences(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir)
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

void EditorUI::detachSceneReferencesForDelete(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir)
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
                if (m_renderer)
                    m_renderer->removeMeshComponent(go);
            }
            else if (m_renderer && go->staticRenderIndex >= 0)
            {
                if (matches(mesh->material.texturePath))
                {
                    mesh->material.texturePath.clear();
                    m_renderer->replaceStaticTextureWithMissing(go->staticRenderIndex, Renderer::TextureSlot::Diffuse);
                }
                if (matches(mesh->material.normalMapPath))
                {
                    mesh->material.normalMapPath.clear();
                    m_renderer->replaceStaticTextureWithMissing(go->staticRenderIndex, Renderer::TextureSlot::Normal);
                }
                if (matches(mesh->material.metallicRoughnessPath))
                {
                    mesh->material.metallicRoughnessPath.clear();
                    m_renderer->replaceStaticTextureWithMissing(go->staticRenderIndex, Renderer::TextureSlot::MetallicRoughness);
                }
            }
        }
        if (go->hasAudioClip() && matches(go->getAudioClip()->getPath()))
        {
            go->setAudioClip(nullptr);
        }
    });
}

void EditorUI::beginAssetDelete(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir)
{
    m_assetDeleteTarget         = path;
    m_assetDeleteIsDir          = isDir;
    m_assetDeleteAffectedCount  = countSceneReferences(sceneRoot, path, isDir);
    m_assetDeleteError.clear();
    m_openAssetDeletePopup      = true;
}

void EditorUI::createBasicShape(GameObject* parent, const std::string& name, std::shared_ptr<Mesh> mesh)
{
    if (!parent || !m_renderer || !mesh)
        return;

    GameObject* go = parent->addChild(name);
    go->staticRenderIndex = m_renderer->addStaticMesh(*mesh);
    go->setMesh(std::move(mesh));
    pushLog("GameObject '" + go->name + "' creado");
}

void EditorUI::loadMeshForSelected(const std::string& path)
{
    if (!m_selected || !m_renderer || m_selected->hasMesh())
        return;

    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".fbx")
    {
        m_meshLoadError = "Formato no soportado: " + ext;
        return;
    }

    try
    {
        auto mesh = std::make_shared<Mesh>(ModelLoader::load(path));
        m_selected->staticRenderIndex = m_renderer->addStaticMesh(*mesh);
        m_selected->setMesh(std::move(mesh));
        m_meshLoadError.clear();
        pushLog("Componente Mesh añadido a '" + m_selected->name + "'");
    }
    catch (const std::exception& e)
    {
        m_meshLoadError = e.what();
    }
}

void EditorUI::loadAudioClipForSelected(const std::string& path)
{
    if (!m_selected || !m_audio || m_selected->hasAudioClip())
        return;

    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    static const std::set<std::string> kValidExt = {".wav", ".mp3", ".ogg", ".flac"};
    if (!kValidExt.count(ext))
    {
        m_audioLoadError = "Formato no soportado: " + ext;
        return;
    }

    auto clip = m_audio->createAudioClipComponent(path, /*is3D=*/false, /*loop=*/false);
    if (!clip)
    {
        m_audioLoadError = "No se pudo cargar el audio";
        return;
    }
    m_selected->setAudioClip(std::move(clip));
    m_audioLoadError.clear();
    pushLog("Componente Audio Clip añadido a '" + m_selected->name + "'");
}

void EditorUI::drawSceneNode(GameObject* node)
{
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen;
    if (node->children.empty())
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet;
    if (node == m_selected)
        flags |= ImGuiTreeNodeFlags_Selected;

    const std::string label = node->name.empty() ? "GameObject" : node->name;
    bool open = ImGui::TreeNodeEx((const void*)node, flags, "%s", label.c_str());
    if (ImGui::IsItemClicked())
        m_selected = node;

    // Drag: el root (parent == nullptr) no se puede arrastrar.
    if (node->parent && ImGui::BeginDragDropSource())
    {
        ImGui::SetDragDropPayload("DT_GAMEOBJECT", &node, sizeof(GameObject*));
        ImGui::Text("%s", label.c_str());
        ImGui::EndDragDropSource();
    }
    // Drop: soltar sobre cualquier nodo (incluido el root) reposiciona el
    // arrastrado; moveGameObject ya bloquea ciclos y "salir" del root.
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DT_GAMEOBJECT"))
        {
            m_pendingMoveSource = *(GameObject**)payload->Data;
            m_pendingMoveTarget = node;
        }
        ImGui::EndDragDropTarget();
    }

    if (ImGui::BeginPopupContextItem())
    {
        if (ImGui::MenuItem("Create GameObject"))
        {
            GameObject* created = node->addChild("GameObject");
            pushLog("GameObject '" + created->name + "' creado");
        }
        if (ImGui::BeginMenu("Basic Shapes"))
        {
            if (ImGui::MenuItem("Cube"))
                createBasicShape(node, "Cube", std::make_shared<Mesh>(Cube::create(50.0f)));
            if (ImGui::MenuItem("Sphere"))
                createBasicShape(node, "Sphere", std::make_shared<Mesh>(Sphere::create(50.0f)));
            if (ImGui::MenuItem("Plane"))
                createBasicShape(node, "Plane", std::make_shared<Mesh>(Plane::create(50.0f, 0.0f)));
            if (ImGui::MenuItem("Capsule"))
                createBasicShape(node, "Capsule", std::make_shared<Mesh>(Capsule::create(25.0f, 50.0f)));
            ImGui::EndMenu();
        }
        bool canModify = node->parent != nullptr;
        if (ImGui::MenuItem("Rename", nullptr, false, canModify))
            beginRename(node);
        if (ImGui::MenuItem("Delete GameObject", nullptr, false, canModify))
            m_pendingDelete = node;
        ImGui::EndPopup();
    }

    if (open)
    {
        for (const auto& child : node->children)
            drawSceneNode(child.get());
        ImGui::TreePop();
    }
}

float EditorUI::selectionAxisScale(GameObject* node) const
{
    constexpr float kFallback = 50.0f;
    // 2.0 en vez de 1.3: con 1.3 solo sobresalía un poco del mesh y costaba
    // verlo; así el tramo visible fuera del objeto es tan largo como su
    // propio medio-tamaño.
    constexpr float kFactor   = 2.0f;

    if (!node->hasMesh())
        return kFallback;

    const auto& vertices = node->getMesh()->vertices;
    if (vertices.empty())
        return kFallback;

    glm::vec3 bMin = vertices[0].pos;
    glm::vec3 bMax = vertices[0].pos;
    for (const auto& v : vertices)
    {
        bMin = glm::min(bMin, v.pos);
        bMax = glm::max(bMax, v.pos);
    }

    glm::vec3 extent  = bMax - bMin;
    float     maxHalf = glm::max(extent.x, glm::max(extent.y, extent.z)) * 0.5f;
    return glm::max(maxHalf, 1.0f) * kFactor;
}

void EditorUI::focusSelected(Camera& camera)
{
    if (!m_selected)
        return;

    constexpr float kFallbackRadius = 50.0f;

    glm::vec3 center = glm::vec3(m_selected->worldTransform[3]);
    float     radius = kFallbackRadius;

    if (m_selected->hasMesh())
    {
        const auto& vertices = m_selected->getMesh()->vertices;
        if (!vertices.empty())
        {
            glm::vec3 bMin = vertices[0].pos;
            glm::vec3 bMax = vertices[0].pos;
            for (const auto& v : vertices)
            {
                bMin = glm::min(bMin, v.pos);
                bMax = glm::max(bMax, v.pos);
            }
            glm::vec3 extent   = bMax - bMin;
            float     maxHalf  = glm::max(extent.x, glm::max(extent.y, extent.z)) * 0.5f;

            glm::vec3 worldScale(
                glm::length(glm::vec3(m_selected->worldTransform[0])),
                glm::length(glm::vec3(m_selected->worldTransform[1])),
                glm::length(glm::vec3(m_selected->worldTransform[2])));
            float maxWorldScale = glm::max(worldScale.x, glm::max(worldScale.y, worldScale.z));

            radius = glm::max(maxHalf, 1.0f) * maxWorldScale;
        }
    }

    camera.focusOn(center, radius);
}

void EditorUI::drawSelectionGizmo()
{
    if (!m_selected)
        return;
    Gizmos::drawAxes(m_selected->worldTransform, selectionAxisScale(m_selected));

    const glm::vec3 kColliderColor(1.0f, 1.0f, 0.0f);
    if (m_selected->hasBoxCollider())
    {
        BoxCollider* bc = m_selected->getBoxCollider().get();
        Gizmos::drawWireBox(m_selected->worldTransform, bc->getCenter(),
                             bc->getHalfExtents(), kColliderColor);
    }
    else if (m_selected->hasSphereCollider())
    {
        SphereCollider* sc = m_selected->getSphereCollider().get();
        Gizmos::drawWireSphere(m_selected->worldTransform, sc->getCenter(),
                                sc->getRadius(), kColliderColor);
    }
    else if (m_selected->hasCapsuleCollider())
    {
        CapsuleCollider* cc = m_selected->getCapsuleCollider().get();
        Gizmos::drawWireCapsule(m_selected->worldTransform, cc->getCenter(),
                                 cc->getRadius(), cc->getHalfHeight(), kColliderColor);
    }
    else if (m_selected->hasPlaneCollider())
    {
        PlaneCollider* pc = m_selected->getPlaneCollider().get();
        Gizmos::drawWirePlane(m_selected->worldTransform, pc->getCenter(), kColliderColor);
    }
}

void EditorUI::drawViewport(VkDescriptorSet viewportTexture, const glm::mat4& cameraView)
{
    if (!m_viewportOpen)
    {
        // Sin esto, cerrar Viewport dejaría m_viewportHovered en su último
        // valor (posiblemente true) y el mouse-look de la cámara en
        // sandbox/src/main.cpp seguiría respondiendo con el panel oculto.
        m_viewportHovered = false;
        return;
    }
    ImGui::Begin("Viewport", &m_viewportOpen);
    m_viewportHovered = ImGui::IsWindowHovered();
    ImVec2 vpPos  = ImGui::GetCursorScreenPos();
    ImVec2 vpSize = ImGui::GetContentRegionAvail();
    ImGui::Image((ImTextureID)(intptr_t)viewportTexture, vpSize);

    // Axis gizmo estilo Unity/Godot (esquina superior derecha): ejes mundo
    // proyectados por la rotación real de la cámara (parte 3x3 de la view
    // matrix), así que gira con ella. Clicar una bola reorienta la cámara
    // pa mirar a lo largo de ese eje (via m_onAxisSelected).
    const glm::mat3 camRot(cameraView);

    struct Axis { glm::vec3 world; glm::vec3 screenDir; ImU32 color; const char* label; };
    Axis axes[3] = {
        { glm::vec3(1, 0, 0), camRot * glm::vec3(1, 0, 0), IM_COL32(220,  60,  60, 255), "X" },
        { glm::vec3(0, 1, 0), camRot * glm::vec3(0, 1, 0), IM_COL32( 70, 200,  70, 255), "Y" },
        { glm::vec3(0, 0, 1), camRot * glm::vec3(0, 0, 1), IM_COL32( 70, 130, 230, 255), "Z" },
    };

    const float radius = 34.0f;
    const float margin  = 16.0f;
    const float ballRadius = 7.0f;
    ImVec2 center(vpPos.x + vpSize.x - radius - margin, vpPos.y + radius + margin);

    // Pinta primero el eje más lejano de cámara pa que el más cercano quede encima.
    int order[3] = { 0, 1, 2 };
    std::sort(order, order + 3, [&](int a, int b) { return axes[a].screenDir.z < axes[b].screenDir.z; });

    ImVec2 mouse = ImGui::GetIO().MousePos;
    bool clicked = m_viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (int i : order)
    {
        const glm::vec3& d = axes[i].screenDir;
        ImVec2 tip(center.x + d.x * radius, center.y - d.y * radius);
        drawList->AddLine(center, tip, axes[i].color, 2.0f);
        drawList->AddCircleFilled(tip, ballRadius, axes[i].color);

        ImVec2 textSize = ImGui::CalcTextSize(axes[i].label);
        drawList->AddText(ImVec2(tip.x - textSize.x * 0.5f, tip.y - textSize.y * 0.5f),
                           IM_COL32(0, 0, 0, 255), axes[i].label);

        if (clicked && m_onAxisSelected)
        {
            float dx = mouse.x - tip.x, dy = mouse.y - tip.y;
            if (dx * dx + dy * dy <= ballRadius * ballRadius)
                m_onAxisSelected(axes[i].world);
        }
    }
    drawList->AddCircleFilled(center, 3.0f, IM_COL32(200, 200, 200, 255));

    ImGui::End();
}

void EditorUI::drawProperties()
{
    if (!m_propertiesOpen) return;
    ImGui::Begin("Properties", &m_propertiesOpen);
    if (!m_selected)
    {
        m_propsCachedFor = nullptr;
        ImGui::End();
        return;
    }

    // Solo re-sincroniza el cache de edición al cambiar de selección: si se
    // recompusiera desde localTransform en cada frame, un valor intermedio
    // inválido (p.ej. escala 0 mientras se teclea "0.5") se re-descompondría
    // y rompería posición/rotación de forma permanente.
    if (m_propsCachedFor != m_selected)
    {
        glm::vec3 skew;
        glm::vec4 perspective;
        glm::quat orientation;
        glm::decompose(m_selected->localTransform, m_editScale, orientation, m_editPosition, skew, perspective);
        m_editRotationDeg = glm::degrees(glm::eulerAngles(orientation));
        m_propsCachedFor = m_selected;
        m_meshLoadError.clear();
        m_audioLoadError.clear();
    }
    // BoxCollider dinámico (useGravity=true): PhysX mueve worldTransform (y
    // localTransform, ver traverse en el loop principal) cada frame, pero eso
    // nunca toca este cache de edición — sin este refresco, Position/Rotation
    // mostrados quedan congelados en el valor de cuando se seleccionó, aunque
    // el objeto siga cayendo/rotando por física. Solo posición+rotación (la
    // escala es puramente del editor, physx no la conoce); se salta mientras
    // se está arrastrando un slider pa no pelear con el drag del usuario.
    else if (m_selected->hasBoxCollider() && m_selected->getBoxCollider()->isDynamic() && !m_transformDragActive)
    {
        glm::vec3 skew, unusedScale;
        glm::vec4 perspective;
        glm::quat orientation;
        glm::decompose(m_selected->worldTransform, unusedScale, orientation, m_editPosition, skew, perspective);
        m_editRotationDeg = glm::degrees(glm::eulerAngles(orientation));
    }

    ImGui::Text("%s", m_selected->name.empty() ? "GameObject" : m_selected->name.c_str());
    ImGui::Separator();

    bool changed = false;
    bool posRotActive = false;
    bool scaleActive = false;
    bool activated = false;
    bool posCommitted = false;
    bool rotCommitted = false;
    bool scaleCommitted = false;

    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNodeEx("Transform", ImGuiTreeNodeFlags_OpenOnArrow))
    {
        ImGui::Text("Position");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("X##1", &m_editPosition.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        posCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Y##1", &m_editPosition.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        posCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Z##1", &m_editPosition.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        posCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::Text("Rotation");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("X##2", &m_editRotationDeg.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        rotCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Y##2", &m_editRotationDeg.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        rotCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Z##2", &m_editRotationDeg.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        rotCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::Text("Scale   ");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("X##3", &m_editScale.x, 0.005f, 0.001f, +FLT_MAX, "% .3f");
        scaleActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        scaleCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Y##3", &m_editScale.y, 0.005f, 0.001f, +FLT_MAX, "% .3f");
        scaleActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        scaleCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Z##3", &m_editScale.z, 0.005f, 0.001f, +FLT_MAX, "% .3f");
        scaleActive |= ImGui::IsItemActive();
        activated |= ImGui::IsItemActivated();
        scaleCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::TreePop();
    }

    m_transformDragActive = posRotActive || scaleActive;

    if (activated)
        m_transformBeforeEdit = m_selected->localTransform;

    if (posCommitted)
        pushLog("Position de '" + m_selected->name + "' cambiado a " + formatVec3(m_editPosition));
    if (rotCommitted)
        pushLog("Rotation de '" + m_selected->name + "' cambiado a " + formatVec3(m_editRotationDeg));
    if (scaleCommitted)
        pushLog("Scale de '" + m_selected->name + "' cambiado a " + formatVec3(m_editScale));

    if (changed)
    {
        glm::mat4 t = glm::translate(glm::mat4(1.0f), m_editPosition);
        glm::mat4 r = glm::mat4_cast(glm::quat(glm::radians(m_editRotationDeg)));
        glm::mat4 s = glm::scale(glm::mat4(1.0f), m_editScale);
        m_selected->localTransform = t * r * s;

        if (m_selected->hasAnyCollider())
        {
            m_selected->updateWorldTransforms(m_selected->parent ? m_selected->parent->worldTransform
                                                                   : glm::mat4(1.0f));
            // teleport() (no syncTransform): funciona tanto si el actor es
            // dinámico (isDynamic()==true) como si es kinematic
            // (isDynamic()==false) — syncTransform usa setKinematicTarget,
            // que solo es válido en modo kinematic. hasAnyCollider() cubre
            // los 4 tipos porque son mutuamente excluyentes.
            if (m_selected->hasBoxCollider())
                m_selected->getBoxCollider()->teleport(m_selected->worldTransform);
            else if (m_selected->hasSphereCollider())
                m_selected->getSphereCollider()->teleport(m_selected->worldTransform);
            else if (m_selected->hasCapsuleCollider())
                m_selected->getCapsuleCollider()->teleport(m_selected->worldTransform);
            else if (m_selected->hasPlaneCollider())
                m_selected->getPlaneCollider()->teleport(m_selected->worldTransform);
        }
    }

    if ((posCommitted || rotCommitted || scaleCommitted) && m_scene)
    {
        Scene* scene = m_scene;
        uint64_t id = m_selected->id;
        glm::mat4 before = m_transformBeforeEdit;
        glm::mat4 after = m_selected->localTransform;
        m_undoHistory.push(std::make_unique<PropertyCommand<glm::mat4>>(
            "Transform de '" + m_selected->name + "'", before, after,
            [scene, id](const glm::mat4& t) {
                GameObject* go = scene->findById(id);
                if (!go) return;
                go->localTransform = t;
                if (go->hasAnyCollider())
                {
                    go->updateWorldTransforms(go->parent ? go->parent->worldTransform : glm::mat4(1.0f));
                    if (go->hasBoxCollider())
                        go->getBoxCollider()->teleport(go->worldTransform);
                    else if (go->hasSphereCollider())
                        go->getSphereCollider()->teleport(go->worldTransform);
                    else if (go->hasCapsuleCollider())
                        go->getCapsuleCollider()->teleport(go->worldTransform);
                    else if (go->hasPlaneCollider())
                        go->getPlaneCollider()->teleport(go->worldTransform);
                }
            }));
    }

    drawBoxColliderSection();
    drawSphereColliderSection();
    drawCapsuleColliderSection();
    drawPlaneColliderSection();
    drawMeshSection();
    drawAudioClipSection();
    drawScriptsSection();
    drawAddComponentButton();

    ImGui::End();
}

void EditorUI::drawBoxColliderSection()
{
    if (!m_selected->hasBoxCollider())
    {
        m_colliderCachedFor = nullptr;
        return;
    }

    BoxCollider* bc = m_selected->getBoxCollider().get();

    if (m_colliderCachedFor != bc)
    {
        m_editColliderCenter = bc->getCenter();
        m_editColliderSize   = bc->getHalfExtents() * 2.0f;
        m_editUseGravity     = bc->getUseGravity();
        m_colliderCachedFor  = bc;
    }
    else if (bc->isDynamic() && !m_colliderDragActive)
    {
        // Solo Center/Size se refrescan (son estables bajo simulación); el
        // toggle de gravedad lo controla el usuario y no cambia solo.
        m_editColliderCenter = bc->getCenter();
        m_editColliderSize   = bc->getHalfExtents() * 2.0f;
    }

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Box Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;
    bool centerCommitted = false;
    bool sizeCommitted = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##c1", &m_editColliderCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##c1", &m_editColliderCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##c1", &m_editColliderCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::Text("Size  ");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##c2", &m_editColliderSize.x, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        sizeCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##c2", &m_editColliderSize.y, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        sizeCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##c2", &m_editColliderSize.z, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        sizeCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        if (ImGui::Checkbox("Use Gravity", &m_editUseGravity))
        {
            colliderChanged = true;
            pushLog(std::string("Use Gravity de '") + m_selected->name +
                     "' (Box Collider) " + (m_editUseGravity ? "activado" : "desactivado"));
        }

        ImGui::TreePop();
    }

    m_colliderDragActive = dragActive;

    if (centerCommitted)
        pushLog("Center de '" + m_selected->name + "' (Box Collider) cambiado a " + formatVec3(m_editColliderCenter));
    if (sizeCommitted)
        pushLog("Size de '" + m_selected->name + "' (Box Collider) cambiado a " + formatVec3(m_editColliderSize));

    if (colliderChanged)
    {
        bc->setCenter(m_editColliderCenter);
        bc->setHalfExtents(m_editColliderSize * 0.5f);
        bc->setUseGravity(m_editUseGravity);
    }

    if (removeClicked)
    {
        m_selected->setBoxCollider(nullptr);
        m_colliderCachedFor = nullptr;
        pushLog("Componente Box Collider quitado de '" + m_selected->name + "'");
    }
}

void EditorUI::drawSphereColliderSection()
{
    if (!m_selected->hasSphereCollider())
    {
        m_sphereColliderCachedFor = nullptr;
        return;
    }

    SphereCollider* sc = m_selected->getSphereCollider().get();

    if (m_sphereColliderCachedFor != sc)
    {
        m_editSphereCenter        = sc->getCenter();
        m_editSphereRadius        = sc->getRadius();
        m_editSphereUseGravity    = sc->getUseGravity();
        m_sphereColliderCachedFor = sc;
    }
    else if (sc->isDynamic() && !m_sphereColliderDragActive)
    {
        m_editSphereCenter = sc->getCenter();
        m_editSphereRadius = sc->getRadius();
    }

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Sphere Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;
    bool centerCommitted = false;
    bool radiusCommitted = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##s1", &m_editSphereCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##s1", &m_editSphereCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##s1", &m_editSphereCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::Text("Radius");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("##s2", &m_editSphereRadius, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        radiusCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        if (ImGui::Checkbox("Use Gravity", &m_editSphereUseGravity))
        {
            colliderChanged = true;
            pushLog(std::string("Use Gravity de '") + m_selected->name +
                     "' (Sphere Collider) " + (m_editSphereUseGravity ? "activado" : "desactivado"));
        }

        ImGui::TreePop();
    }

    m_sphereColliderDragActive = dragActive;

    if (centerCommitted)
        pushLog("Center de '" + m_selected->name + "' (Sphere Collider) cambiado a " + formatVec3(m_editSphereCenter));
    if (radiusCommitted)
        pushLog("Radius de '" + m_selected->name + "' (Sphere Collider) cambiado a " + formatFloat(m_editSphereRadius));

    if (colliderChanged)
    {
        sc->setCenter(m_editSphereCenter);
        sc->setRadius(m_editSphereRadius);
        sc->setUseGravity(m_editSphereUseGravity);
    }

    if (removeClicked)
    {
        m_selected->setSphereCollider(nullptr);
        m_sphereColliderCachedFor = nullptr;
        pushLog("Componente Sphere Collider quitado de '" + m_selected->name + "'");
    }
}

void EditorUI::drawCapsuleColliderSection()
{
    if (!m_selected->hasCapsuleCollider())
    {
        m_capsuleColliderCachedFor = nullptr;
        return;
    }

    CapsuleCollider* cc = m_selected->getCapsuleCollider().get();

    if (m_capsuleColliderCachedFor != cc)
    {
        m_editCapsuleCenter        = cc->getCenter();
        m_editCapsuleRadius        = cc->getRadius();
        m_editCapsuleHeight        = cc->getHalfHeight() * 2.0f;
        m_editCapsuleUseGravity    = cc->getUseGravity();
        m_capsuleColliderCachedFor = cc;
    }
    else if (cc->isDynamic() && !m_capsuleColliderDragActive)
    {
        m_editCapsuleCenter = cc->getCenter();
        m_editCapsuleRadius = cc->getRadius();
        m_editCapsuleHeight = cc->getHalfHeight() * 2.0f;
    }

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Capsule Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;
    bool centerCommitted = false;
    bool radiusCommitted = false;
    bool heightCommitted = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##k1", &m_editCapsuleCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##k1", &m_editCapsuleCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##k1", &m_editCapsuleCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::Text("Radius");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("##k2", &m_editCapsuleRadius, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        radiusCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::Text("Height");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("##k3", &m_editCapsuleHeight, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        heightCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        if (ImGui::Checkbox("Use Gravity", &m_editCapsuleUseGravity))
        {
            colliderChanged = true;
            pushLog(std::string("Use Gravity de '") + m_selected->name +
                     "' (Capsule Collider) " + (m_editCapsuleUseGravity ? "activado" : "desactivado"));
        }

        ImGui::TreePop();
    }

    m_capsuleColliderDragActive = dragActive;

    if (centerCommitted)
        pushLog("Center de '" + m_selected->name + "' (Capsule Collider) cambiado a " + formatVec3(m_editCapsuleCenter));
    if (radiusCommitted)
        pushLog("Radius de '" + m_selected->name + "' (Capsule Collider) cambiado a " + formatFloat(m_editCapsuleRadius));
    if (heightCommitted)
        pushLog("Height de '" + m_selected->name + "' (Capsule Collider) cambiado a " + formatFloat(m_editCapsuleHeight));

    if (colliderChanged)
    {
        cc->setCenter(m_editCapsuleCenter);
        cc->setRadius(m_editCapsuleRadius);
        cc->setHalfHeight(m_editCapsuleHeight * 0.5f);
        cc->setUseGravity(m_editCapsuleUseGravity);
    }

    if (removeClicked)
    {
        m_selected->setCapsuleCollider(nullptr);
        m_capsuleColliderCachedFor = nullptr;
        pushLog("Componente Capsule Collider quitado de '" + m_selected->name + "'");
    }
}

void EditorUI::drawPlaneColliderSection()
{
    if (!m_selected->hasPlaneCollider())
    {
        m_planeColliderCachedFor = nullptr;
        return;
    }

    PlaneCollider* pc = m_selected->getPlaneCollider().get();

    if (m_planeColliderCachedFor != pc)
    {
        m_editPlaneCenter        = pc->getCenter();
        m_planeColliderCachedFor = pc;
    }

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Plane Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;
    bool centerCommitted = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##p1", &m_editPlaneCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##p1", &m_editPlaneCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##p1", &m_editPlaneCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::TreePop();
    }

    m_planeColliderDragActive = dragActive;

    if (centerCommitted)
        pushLog("Center de '" + m_selected->name + "' (Plane Collider) cambiado a " + formatVec3(m_editPlaneCenter));

    if (colliderChanged)
        pc->setCenter(m_editPlaneCenter);

    if (removeClicked)
    {
        m_selected->setPlaneCollider(nullptr);
        m_planeColliderCachedFor = nullptr;
        pushLog("Componente Plane Collider quitado de '" + m_selected->name + "'");
    }
}

void EditorUI::drawMeshSection()
{
    // Oculto por defecto: solo se dibuja si ya tiene mesh, o si se pulsó
    // "Add > Mesh" para este GameObject concreto (m_meshAddRequestedFor).
    if (!m_selected->hasMesh() && m_meshAddRequestedFor != m_selected)
        return;

    ImGui::Separator();

    if (m_selected->hasMesh())
    {
        bool sectionOpen = ImGui::TreeNodeEx("Mesh", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
        bool removeClicked = ImGui::SmallButton("x");

        if (sectionOpen)
        {
            ImGui::Text("%s", m_selected->getMesh()->name.c_str());
            ImGui::TreePop();
        }

        if (removeClicked && m_renderer)
        {
            m_renderer->removeMeshComponent(m_selected);
            // Vuelve a ocultar la sección tras quitar el mesh — hay que
            // pulsar "Add > Mesh" de nuevo para reabrirla.
            m_meshAddRequestedFor = nullptr;
            pushLog("Componente Mesh quitado de '" + m_selected->name + "'");
        }

        return;
    }

    ImGui::Text("Mesh");
    if (ImGui::Button("Browse..."))
    {
        m_meshDlgOpen = true;
        IGFD::FileDialogConfig cfg;
        cfg.path  = "assets";
        cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                    ImGuiFileDialogFlags_HideColumnDate |
                    ImGuiFileDialogFlags_DisableThumbnailMode |
                    ImGuiFileDialogFlags_DisablePlaceMode;
        // Key sin prefijo "##": Display() construye el nombre interno de la
        // ventana como título+"##"+key; con key="##AddMeshDlg" el resultado
        // llevaba 4 almohadillas seguidas ("Choose FBX####AddMeshDlg"), y
        // ImGui trata "###" como separador especial de ID (todo lo posterior
        // determina el ID, ignorando el resto) — se calculaba distinto en
        // window->ID que en el ID guardado en settings al persistir el
        // layout, y el mismatch disparaba
        // "Assertion failed: settings->ID == window->ID" al redimensionar
        // (momento en que se fuerza el guardado). El ejemplo oficial de IGFD
        // usa keys planas (sin "##"), como aquí.
        m_meshFileDialog->OpenDialog("AddMeshDlg", "Choose FBX", ".fbx", cfg);
    }

    ImGui::BeginChild("##MeshDropZone", ImVec2(0, 40), true);
    ImGui::TextDisabled("Drop .fbx here");
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DT_ASSET_PATH"))
            loadMeshForSelected(std::string(static_cast<const char*>(payload->Data)));
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();

    if (!m_meshLoadError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_meshLoadError.c_str());
}

void EditorUI::drawMeshDialog()
{
    // Se ejecuta cada frame independientemente de m_selected/hasMesh(): si no
    // se drena aquí, cambiar de selección (o deseleccionar) mientras el
    // diálogo está abierto deja m_meshDlgOpen atascado en true para siempre.
    // m_meshFileDialog es una instancia propia (no el singleton Instance()
    // de Content Browser), así que redimensionar este popup no toca el
    // estado interno de ContentDlg ni viceversa.
    if (m_meshDlgOpen && m_meshFileDialog->Display("AddMeshDlg"))
    {
        if (m_meshFileDialog->IsOk())
            loadMeshForSelected(m_meshFileDialog->GetFilePathName());
        m_meshFileDialog->Close();
        m_meshDlgOpen = false;
    }
}

void EditorUI::drawAudioClipSection()
{
    // Oculto por defecto: solo se dibuja si ya tiene AudioClip, o si se
    // pulsó "Add > Audio Clip" para este GameObject concreto
    // (m_audioClipAddRequestedFor).
    if (!m_selected->hasAudioClip() && m_audioClipAddRequestedFor != m_selected)
        return;

    ImGui::Separator();

    if (m_selected->hasAudioClip())
    {
        auto& clip = m_selected->getAudioClip();
        bool sectionOpen = ImGui::TreeNodeEx("Audio Clip", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
        bool removeClicked = ImGui::SmallButton("x");

        if (sectionOpen)
        {
            std::string fname = std::filesystem::path(clip->getPath()).filename().string();
            ImGui::Text("%s", fname.c_str());

            ImGui::BeginDisabled(m_audio == nullptr);
            if (ImGui::Button("Play"))
            {
                glm::vec3 worldPos(m_selected->worldTransform[3]);
                clip->play(worldPos);
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop"))
                clip->stop();
            ImGui::EndDisabled();

            bool loop = clip->getLoop();
            if (ImGui::Checkbox("Loop", &loop))
                clip->setLoop(loop);

            bool is3D = clip->getIs3D();
            if (ImGui::Checkbox("Is 3D?", &is3D))
                clip->setIs3D(is3D);

            ImGui::TreePop();
        }

        if (removeClicked)
        {
            m_selected->setAudioClip(nullptr);
            // Vuelve a ocultar la sección tras quitar el clip — hay que
            // pulsar "Add > Audio Clip" de nuevo para reabrirla.
            m_audioClipAddRequestedFor = nullptr;
            pushLog("Componente Audio Clip quitado de '" + m_selected->name + "'");
        }

        return;
    }

    ImGui::Text("Audio Clip");
    ImGui::BeginDisabled(m_audio == nullptr);
    if (ImGui::Button("Browse..."))
    {
        m_audioDlgOpen = true;
        IGFD::FileDialogConfig cfg;
        cfg.path  = "assets";
        cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                    ImGuiFileDialogFlags_HideColumnDate |
                    ImGuiFileDialogFlags_DisableThumbnailMode |
                    ImGuiFileDialogFlags_DisablePlaceMode;
        // Key plana sin "##" (mismo motivo documentado en drawMeshSection
        // para AddMeshDlg: con prefijo "##" el título concatenado generaba
        // 4 almohadillas seguidas y rompía el ID persistido de ImGui).
        m_audioFileDialog->OpenDialog("AddAudioDlg", "Choose Audio", ".wav,.mp3,.ogg,.flac", cfg);
    }
    ImGui::EndDisabled();

    ImGui::BeginChild("##AudioDropZone", ImVec2(0, 40), true);
    ImGui::TextDisabled("Drop audio here");
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DT_ASSET_PATH"))
            loadAudioClipForSelected(std::string(static_cast<const char*>(payload->Data)));
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();

    if (!m_audioLoadError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_audioLoadError.c_str());
}

void EditorUI::drawAudioClipDialog()
{
    // Se ejecuta cada frame independientemente de m_selected/hasAudioClip():
    // si no se drena aquí, cambiar de selección mientras el diálogo está
    // abierto deja m_audioDlgOpen atascado en true (mismo motivo que
    // drawMeshDialog).
    if (m_audioDlgOpen && m_audioFileDialog->Display("AddAudioDlg"))
    {
        if (m_audioFileDialog->IsOk())
            loadAudioClipForSelected(m_audioFileDialog->GetFilePathName());
        m_audioFileDialog->Close();
        m_audioDlgOpen = false;
    }
}

bool EditorUI::reloadSceneFromJson(const nlohmann::json& j)
{
    if (!m_scene || !m_renderer || !m_physics || !m_audio)
        return false;

    // Libera recursos GPU de la escena actual y resetea sus índices a -1:
    // si fromJson falla más abajo por malformación anidada, m_root sigue
    // siendo este mismo árbol (Scene::fromJson es atómico), y resetear los
    // índices aquí permite que el traverse de re-registro de abajo lo
    // vuelva a registrar igual que si fuera el árbol nuevo — sin esto, el
    // árbol viejo quedaría con índices obsoletos (Renderer::removeGameObject
    // no los resetea) y sin re-registrar tras un fallo, dejando el viewport
    // vacío pese a que los datos de Scene no cambiaron.
    for (auto& child : m_scene->getRoot().children)
    {
        m_renderer->removeGameObject(child.get());
        child->traverse([](GameObject* go) {
            go->staticRenderIndex = -1;
            go->skinnedRenderIndex = -1;
        });
    }

    bool loaded = m_scene->fromJson(j, *m_physics, *m_audio);
    // Se ejecuta tanto si loaded es true (árbol nuevo, índices ya en -1 por
    // construcción) como si es false (árbol viejo intacto, índices
    // reseteados justo arriba) — en ambos casos hay que volver a subir los
    // meshes a GPU.
    m_renderer->registerGameObject(&m_scene->getRoot());

    if (loaded)
    {
        m_selected = nullptr; // la selección anterior ya no existe
        m_undoHistory.clear();
    }

    return loaded;
}

void EditorUI::drawSceneDialog()
{
    // Mismo motivo que drawMeshDialog/drawAudioClipDialog: se ejecuta cada
    // frame independientemente de m_sceneDlgOpen para drenar el diálogo aunque
    // el usuario lo cierre sin confirmar.
    if (!m_sceneDlgOpen || !m_sceneFileDialog->Display("SceneDlg"))
        return;

    if (m_sceneFileDialog->IsOk())
    {
        std::string path = m_sceneFileDialog->GetFilePathName();

        if (m_sceneDlgIsSave)
        {
            bool saved   = m_scene && m_scene->save(path);
            m_sceneIOError = saved ? "" : "No se pudo guardar la escena";
            pushLog(saved ? ("Escena guardada: " + path) : ("Error al guardar escena: " + path));
        }
        else
        {
            // Valida la estructura básica del JSON ANTES de tocar GPU/Scene:
            // rechaza un fichero top-level corrupto sin tocar nada (fast
            // path, evita el churn de GPU de reloadSceneFromJson). No cubre
            // malformación anidada — para eso, Scene::fromJson es atómico y
            // reloadSceneFromJson cubre ambos desenlaces.
            auto parsed = FileManager::readJson(path);
            bool structureOk = parsed.has_value() &&
                                parsed->contains("version") && (*parsed)["version"].is_number_integer() &&
                                (*parsed)["version"].get<int>() == 1 &&
                                parsed->contains("root") && (*parsed)["root"].is_object();

            bool loaded  = structureOk && reloadSceneFromJson(*parsed);
            m_sceneIOError = loaded ? "" : "No se pudo cargar la escena";
            pushLog(loaded ? ("Escena cargada: " + path) : ("Error al cargar escena: " + path));
        }
    }

    m_sceneFileDialog->Close();
    m_sceneDlgOpen = false;
}

void EditorUI::drawScriptsSection()
{
    if (!m_selected || !m_scriptManager || !m_selected->hasScripts()) return;

    ScriptComponent* toRemove = nullptr;

    for (auto& compPtr : m_selected->getScripts())
    {
        ScriptComponent* comp = compPtr.get();
        ImGui::PushID(comp);

        // TreeNodeEx (label estrecho) y no CollapsingHeader (frame de ancho
        // completo): el header solaparía el botón "x" y se comería su click.
        // Mismo patrón que las secciones de collider.
        ImGui::Separator();
        bool open = ImGui::TreeNodeEx((comp->scriptName + " (Script)").c_str(),
            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::SameLine(ImGui::GetWindowWidth() - 65.0f);
        if (ImGui::SmallButton("Edit"))
            m_scriptEditor->openFile(m_scriptManager->scriptsDirPath() / (comp->scriptName + ".lua"));
        ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
        if (ImGui::SmallButton("x"))
            toRemove = comp;

        if (open)
        {
            if (!m_scriptManager->hasClass(comp->scriptName))
            {
                const std::string* err = m_scriptManager->getCompileError(comp->scriptName);
                if (err)
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                        "Error de compilación:\n%s", err->c_str());
                else
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                        "Script no encontrado: %s.lua", comp->scriptName.c_str());
                // Overrides intactos (spec: no se pierden datos)
            }
            else
            {
                const ScriptClass& cls = m_scriptManager->getRegistry().at(comp->scriptName);
                const bool live = m_isPlaying && comp->instance.valid();

                for (const ScriptProp& prop : cls.props)
                {
                    // Valor mostrado: instancia viva > override > default
                    ScriptValue value = prop.defaultValue;
                    if (auto it = comp->overrides.find(prop.name); it != comp->overrides.end())
                        value = it->second;
                    if (live)
                    {
                        sol::object lv = comp->instance[prop.name];
                        if (lv.get_type() == sol::type::number)       value = lv.as<double>();
                        else if (lv.get_type() == sol::type::boolean) value = lv.as<bool>();
                        else if (lv.get_type() == sol::type::string)  value = lv.as<std::string>();
                    }

                    const std::string label = prettyPropLabel(prop.name);
                    bool edited = false;

                    if (std::holds_alternative<double>(value))
                    {
                        double d = std::get<double>(value);
                        if (prop.isInteger)
                        {
                            int i = static_cast<int>(d);
                            if (ImGui::DragInt(label.c_str(), &i)) { value = double(i); edited = true; }
                        }
                        else
                        {
                            float f = static_cast<float>(d);
                            if (ImGui::DragFloat(label.c_str(), &f, 0.1f)) { value = double(f); edited = true; }
                        }
                    }
                    else if (std::holds_alternative<bool>(value))
                    {
                        bool b = std::get<bool>(value);
                        if (ImGui::Checkbox(label.c_str(), &b)) { value = b; edited = true; }
                    }
                    else
                    {
                        char buf[256] = {};
                        const std::string& s = std::get<std::string>(value);
                        strncpy_s(buf, s.c_str(), sizeof(buf) - 1);
                        if (ImGui::InputText(label.c_str(), buf, sizeof(buf)))
                        { value = std::string(buf); edited = true; }
                    }

                    if (edited)
                    {
                        comp->overrides[prop.name] = value;
                        if (live)
                        {
                            std::visit([&](auto&& v) {
                                using T = std::decay_t<decltype(v)>;
                                if constexpr (std::is_same_v<T, double>)
                                {
                                    if (prop.isInteger) comp->instance[prop.name] = static_cast<int64_t>(v);
                                    else                comp->instance[prop.name] = v;
                                }
                                else comp->instance[prop.name] = v;
                            }, value);
                        }
                        pushLog("Script '" + comp->scriptName + "." + prop.name +
                                "' cambiado en '" + m_selected->name + "'");
                    }
                }

                if (ImGui::Button("Reset"))
                {
                    comp->overrides.clear();
                    if (live)
                    {
                        // Reaplica defaults del .lua a la instancia viva
                        for (const ScriptProp& prop : cls.props)
                            std::visit([&](auto&& v) {
                                using T = std::decay_t<decltype(v)>;
                                if constexpr (std::is_same_v<T, double>)
                                {
                                    if (prop.isInteger) comp->instance[prop.name] = static_cast<int64_t>(v);
                                    else                comp->instance[prop.name] = v;
                                }
                                else comp->instance[prop.name] = v;
                            }, prop.defaultValue);
                    }
                    pushLog("Script '" + comp->scriptName + "' reseteado a defaults en '" +
                            m_selected->name + "'");
                }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    if (toRemove)
    {
        if (m_isPlaying) m_scriptManager->callOnDestroy(*toRemove);
        const std::string name = toRemove->scriptName;
        m_selected->removeScript(toRemove);
        pushLog("Componente Script '" + name + "' quitado de '" + m_selected->name + "'");
    }
}

void EditorUI::drawAddComponentButton()
{
    ImGui::Separator();
    if (ImGui::Button("Add"))
        ImGui::OpenPopup("AddComponentPopup");

    if (ImGui::BeginPopup("AddComponentPopup"))
    {
        bool alreadyHasAny = m_selected->hasAnyCollider();
        ImGui::BeginDisabled(alreadyHasAny);

        if (ImGui::Selectable("Box Collider") && !alreadyHasAny && m_physics)
        {
            m_selected->setBoxCollider(m_physics->createBoxColliderComponent(
                glm::vec3(25.0f, 25.0f, 25.0f), glm::vec3(0.0f),
                m_selected->worldTransform, /*useGravity=*/false));
            m_colliderCachedFor = nullptr;
            pushLog("Componente Box Collider añadido a '" + m_selected->name + "'");
        }

        if (ImGui::Selectable("Sphere Collider") && !alreadyHasAny && m_physics)
        {
            m_selected->setSphereCollider(m_physics->createSphereColliderComponent(
                25.0f, glm::vec3(0.0f), m_selected->worldTransform, /*useGravity=*/false));
            m_sphereColliderCachedFor = nullptr;
            pushLog("Componente Sphere Collider añadido a '" + m_selected->name + "'");
        }

        if (ImGui::Selectable("Capsule Collider") && !alreadyHasAny && m_physics)
        {
            m_selected->setCapsuleCollider(m_physics->createCapsuleColliderComponent(
                15.0f, 25.0f, glm::vec3(0.0f), m_selected->worldTransform, /*useGravity=*/false));
            m_capsuleColliderCachedFor = nullptr;
            pushLog("Componente Capsule Collider añadido a '" + m_selected->name + "'");
        }

        if (ImGui::Selectable("Plane Collider") && !alreadyHasAny && m_physics)
        {
            m_selected->setPlaneCollider(m_physics->createPlaneColliderComponent(
                glm::vec3(0.0f), m_selected->worldTransform));
            m_planeColliderCachedFor = nullptr;
            pushLog("Componente Plane Collider añadido a '" + m_selected->name + "'");
        }

        ImGui::EndDisabled();

        bool alreadyHasMesh = m_selected->hasMesh();
        ImGui::BeginDisabled(alreadyHasMesh);
        if (ImGui::Selectable("Mesh") && !alreadyHasMesh)
            m_meshAddRequestedFor = m_selected;
        ImGui::EndDisabled();

        bool alreadyHasAudio = m_selected->hasAudioClip();
        ImGui::BeginDisabled(alreadyHasAudio);
        if (ImGui::Selectable("Audio Clip") && !alreadyHasAudio)
            m_audioClipAddRequestedFor = m_selected;
        ImGui::EndDisabled();

        if (m_scriptManager)
        {
            if (ImGui::BeginMenu("Script"))
            {
                for (const auto& entry : m_scriptManager->getRegistry())
                {
                    const std::string& name = entry.first;
                    if (ImGui::MenuItem(name.c_str()))
                    {
                        auto comp = std::make_unique<ScriptComponent>(name, m_selected);
                        m_selected->addScript(std::move(comp));
                        // En Play el lifecycle instancia y dispara Awake/Start
                        // en el siguiente update (started == false).
                        pushLog("Componente Script '" + name + "' añadido a '" + m_selected->name + "'");
                    }
                }
                if (!m_scriptManager->getRegistry().empty())
                    ImGui::Separator();
                if (ImGui::MenuItem("Nuevo Script..."))
                {
                    m_newScriptTarget = m_selected;
                    m_newScriptNameBuffer[0] = '\0';
                    m_newScriptError.clear();
                    m_openNewScriptPopup = true;
                }
                ImGui::EndMenu();
            }
        }

        ImGui::EndPopup();
    }

    drawNewScriptPopup();
}

void EditorUI::drawNewScriptPopup()
{
    if (m_openNewScriptPopup)
    {
        ImGui::OpenPopup("Nuevo Script");
        m_openNewScriptPopup = false;
    }

    if (!ImGui::BeginPopupModal("Nuevo Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::Text("Nombre del script (sin .lua):");
    ImGui::InputText("##NewScriptName", m_newScriptNameBuffer, sizeof(m_newScriptNameBuffer));
    if (!m_newScriptError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", m_newScriptError.c_str());

    if (ImGui::Button("Crear"))
    {
        const std::string name = m_newScriptNameBuffer;

        // Identificador Lua válido: letra o '_' + alfanuméricos/'_' — el
        // nombre del archivo es también el de la tabla global de la clase.
        bool validName = !name.empty() &&
            (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_');
        for (size_t i = 1; validName && i < name.size(); ++i)
            validName = std::isalnum(static_cast<unsigned char>(name[i])) || name[i] == '_';

        const std::filesystem::path path = m_scriptManager->scriptsDirPath() / (name + ".lua");

        if (!validName)
            m_newScriptError = "Nombre inválido: letra o '_' inicial, luego alfanuméricos o '_'";
        else if (m_scriptManager->hasClass(name) || std::filesystem::exists(path))
            m_newScriptError = "Ya existe un script con ese nombre";
        else
        {
            std::ofstream file(path);
            if (!file)
                m_newScriptError = "No se pudo crear el archivo en " + path.string();
            else
            {
                file << name << " = {\n"
                     << "    -- Propiedades serializables (aparecen en el editor)\n"
                     << "    speed = 1\n"
                     << "}\n\n"
                     << "function " << name << ":Start()\n"
                     << "end\n\n"
                     << "function " << name << ":Update(dt)\n"
                     << "end\n";
                file.close();

                if (m_scriptManager->loadScript(path))
                {
                    m_scriptEditor->openFile(path);

                    // El GameObject pudo borrarse mientras el popup estaba
                    // abierto — comprobar que sigue vivo antes de añadir.
                    bool targetAlive = false;
                    if (m_scene && m_newScriptTarget)
                        m_scene->traverse([&](GameObject* go) {
                            if (go == m_newScriptTarget) targetAlive = true;
                        });
                    if (targetAlive)
                    {
                        m_newScriptTarget->addScript(
                            std::make_unique<ScriptComponent>(name, m_newScriptTarget));
                        pushLog("Script '" + name + "' creado y añadido a '" +
                                m_newScriptTarget->name + "'");
                    }
                    else
                        pushLog("Script '" + name + "' creado (el GameObject ya no existe)");
                    ImGui::CloseCurrentPopup();
                }
                else
                    m_newScriptError = "El script no compiló (ver Log)";
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancelar"))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

void EditorUI::drawContentBrowser(GameObject* sceneRoot)
{
    if (!m_contentBrowserOpen) return;
    ImGui::Begin("Content Browser", &m_contentBrowserOpen);
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
                m_scriptEditor->openFile(path);
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
                            pushLog("Asset renombrado: '" + m_assetRenameTarget.filename().string() +
                                    "' -> '" + newPath.filename().string() + "'");
                            updateSceneReferencesForRename(sceneRoot, m_assetRenameTarget, newPath, m_assetRenameIsDir);
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
                    pushLog("Asset eliminado: " + m_assetDeleteTarget.string());
                    detachSceneReferencesForDelete(sceneRoot, m_assetDeleteTarget, m_assetDeleteIsDir);
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
