#include "DonTopo/Editor/EditorUI.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Renderer/Renderer.h"
#include "DonTopo/Files/FileManager.h"
#include "DonTopo/Scripting/ScriptManager.h"
#include "DonTopo/Editor/ScriptEditorPanel.h"
#include <imgui.h>
#include <ImGuiFileDialog.h>
#include <cassert>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace DonTopo {

EditorUI::EditorUI()
    : m_sceneFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_scriptEditor(std::make_unique<ScriptEditorPanel>())
{
    m_scriptEditor->setLogCallback([this](const std::string& msg) { m_logPanel.push(msg); });
}

EditorUI::~EditorUI() = default;

void EditorUI::draw(VkDescriptorSet viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView)
{
    handleUndoRedoShortcut();
    drawMenuBar();
    drawToolbar();
    drawDockSpace();

    // Ctx único, construido una vez por frame y compartido por referencia
    // con todos los paneles (patrón fijado aquí para las tareas siguientes).
    EditorContext ctx{
        m_selected,
        m_isPlaying,
        m_physics,
        m_renderer,
        m_audio,
        m_scene,
        m_scriptManager,
        &m_undoHistory,
        [this](const std::string& msg) { m_logPanel.push(msg); },
        m_onDelete,
        m_onAxisSelected,
        [this](const std::filesystem::path& p) { m_scriptEditor->openFile(p); },
    };

    m_scenePanel.draw(ctx, sceneRoot);
    // ScenePanel ha borrado el GameObject seleccionado — invalida los caches
    // de edición de Properties pa que no arrastren punteros colgantes
    // (GameObject / BoxCollider ya liberados) hasta la próxima selección real.
    if (m_scenePanel.selectionWasDeletedThisFrame())
        m_propertiesPanel.invalidateCaches();
    m_viewportPanel.draw(ctx, viewportTexture, cameraView);
    m_propertiesPanel.draw(ctx);
    m_logPanel.draw();
    drawSceneDialog();
    m_contentBrowserPanel.draw(ctx, sceneRoot);
    m_scriptEditor->draw();
}

void EditorUI::onGameObjectDestroyed(GameObject* node)
{
    if (!node || !m_selected) return;
    bool selectionInSubtree = false;
    node->traverse([&](GameObject* n) { if (n == m_selected) selectionInSubtree = true; });
    if (selectionInSubtree)
    {
        m_selected = nullptr;               // el objeto va a liberarse: no dejar puntero colgante
        m_propertiesPanel.invalidateCaches(); // los caches de edición apuntaban a componentes ya liberados
    }
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
        m_propertiesPanel.invalidateCaches();
        m_logPanel.push("Undo: " + m_undoHistory.lastLabel());
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Y) && m_undoHistory.canRedo())
    {
        uint64_t prevSelId = m_selected ? m_selected->id : 0;
        m_undoHistory.redo();
        m_selected = prevSelId ? m_scene->findById(prevSelId) : nullptr;
        m_propertiesPanel.invalidateCaches();
        m_logPanel.push("Redo: " + m_undoHistory.lastLabel());
    }
}

void EditorUI::drawMenuBar()
{
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem("Scene", nullptr, m_scenePanel.GetOpenPtr());
            ImGui::MenuItem("Viewport", nullptr, m_viewportPanel.GetOpenPtr());
            ImGui::MenuItem("Properties", nullptr, m_propertiesPanel.GetOpenPtr());
            ImGui::MenuItem("Log", nullptr, m_logPanel.GetOpenPtr());
            ImGui::MenuItem("Content Browser", nullptr, m_contentBrowserPanel.GetOpenPtr());
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
            m_logPanel.push("Play Mode detenido");
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
            m_scene->traverse([](GameObject* go) {
                if (go->hasAudioClip() && go->getAudioClip()->getPlayOnAwake())
                    go->getAudioClip()->play(glm::vec3(go->worldTransform[3]));
            });
            m_logPanel.push("Play Mode iniciado");
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

void EditorUI::focusSelected(Camera& camera)
{
    // ctx local, no miembro persistente: evita vida útil ambigua de las
    // referencias (mismo patrón que EditorContext en draw()).
    EditorContext ctx{
        m_selected,
        m_isPlaying,
        m_physics,
        m_renderer,
        m_audio,
        m_scene,
        m_scriptManager,
        &m_undoHistory,
        [this](const std::string& msg) { m_logPanel.push(msg); },
        m_onDelete,
        m_onAxisSelected,
    };
    m_viewportPanel.focusSelected(ctx, camera);
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
        m_selected = nullptr; // la selección anterior ya no existe
    m_undoHistory.clear();

    return loaded;
}

void EditorUI::drawSceneDialog()
{
    // Mismo motivo que PropertiesPanel::drawMeshDialog/drawAudioClipDialog:
    // se ejecuta cada frame independientemente de m_sceneDlgOpen para drenar
    // el diálogo aunque el usuario lo cierre sin confirmar.
    if (!m_sceneDlgOpen || !m_sceneFileDialog->Display("SceneDlg"))
        return;

    if (m_sceneFileDialog->IsOk())
    {
        std::string path = m_sceneFileDialog->GetFilePathName();

        if (m_sceneDlgIsSave)
        {
            bool saved   = m_scene && m_scene->save(path);
            m_sceneIOError = saved ? "" : "No se pudo guardar la escena";
            m_logPanel.push(saved ? ("Escena guardada: " + path) : ("Error al guardar escena: " + path));
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
            m_logPanel.push(loaded ? ("Escena cargada: " + path) : ("Error al cargar escena: " + path));
        }
    }

    m_sceneFileDialog->Close();
    m_sceneDlgOpen = false;
}

} // namespace DonTopo
