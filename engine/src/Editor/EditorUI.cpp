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
#include "DonTopo/Editor/GameExporter.h"
#include <imgui.h>
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace {

// Valida que 'name' sea un componente de ruta seguro para construir
// destDir / name. writeExportPackage() hace remove_all() sobre ese path
// confiando ciegamente en que la UI ya lo validó (ver comentario de
// GameExporter.h:68-70): sin este chequeo, ".." sube un nivel, un nombre
// absoluto como "C:\Windows" hace que operator/ IGNORE destDir por
// completo (así es como std::filesystem::path::operator/ trata una ruta
// absoluta), y Win32 descarta espacios/puntos finales del último
// componente al crear la carpeta, colapsando el destino real sobre la
// carpeta padre aunque el string en pantalla parezca inofensivo.
// isspace de <cctype> con un char con signo (p.ej. una tilde en Latin-1) es
// UB; se pasa siempre por unsigned char primero.
bool isBlankChar(char c)
{
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

bool isValidExportGameName(const std::string& name, std::string& reason)
{
    // find_first_not_of(' ') solo descartaba el espacio U+0020: un nombre de
    // puros tabuladores ("\t\t\t") lo pasaba y reventaba después al crear la
    // carpeta. std::all_of + isBlankChar cubre cualquier espacio en blanco
    // real (tab, CR, LF, form feed...).
    if (name.empty() || std::all_of(name.begin(), name.end(), isBlankChar))
    {
        reason = "El nombre no puede estar vacio";
        return false;
    }
    // Cubre "." y ".." a la vez que cualquier nombre con puntos/espacios
    // finales (p.ej. "...", "Juego. "): Win32 los descarta al crear la
    // carpeta, así que el destino real deja de ser el que se le mostró al
    // usuario en el popup.
    if (name.back() == '.' || isBlankChar(name.back()))
    {
        reason = "El nombre no puede terminar en '.' ni en espacio";
        return false;
    }
    // Mismo conjunto de caracteres reservados de Windows que
    // ContentBrowserPanel.cpp::isValidFileName (kReserved ahí): el comentario
    // que decía "mismo patrón" solo cubría ':' y los separadores, así que
    // "Mi*Juego", "a?b", "x|y" o "<z>" pasaban aquí y fallaban después con un
    // "no se pudo crear" genérico en vez de este motivo concreto.
    static const std::string kReserved = "\\/:*?\"<>|";
    for (char c : name)
    {
        if (kReserved.find(c) != std::string::npos)
        {
            reason = "El nombre no puede contener ninguno de estos caracteres: \\ / : * ? \" < > |";
            return false;
        }
    }
    // filename() distinto del nombre completo == contiene separadores de
    // ruta ('/' o '\') o es una ruta absoluta; en ambos casos destDir / name
    // deja de apuntar dentro de la carpeta que el usuario eligió en el
    // diálogo. Redundante con kReserved de arriba (ambos separadores ya están
    // en el set) pero se deja como red de seguridad extra sobre operator/.
    if (std::filesystem::path(name).filename().string() != name)
    {
        reason = "El nombre no puede contener separadores de ruta";
        return false;
    }
    // Nombres de dispositivo reservados por Windows (CON, NUL, COM1..9,
    // LPT1..9): "<destino>\NUL" no crea una carpeta, resuelve al dispositivo
    // NUL. exists() sobre eso da true, así que el popup de confirmación
    // afirmaría "la carpeta ya existe y se borrará su contenido" sobre algo
    // que no es una carpeta y no tiene contenido. La regla de Windows mira el
    // nombre SIN extensión (todo lo anterior al primer '.'), sin distinguir
    // mayúsculas/minúsculas, así que "NUL.txt" también está reservado.
    static const std::array<std::string, 22> kReservedDeviceNames = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };
    std::string baseUpper = name.substr(0, name.find('.'));
    std::transform(baseUpper.begin(), baseUpper.end(), baseUpper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    for (const std::string& reserved : kReservedDeviceNames)
    {
        if (baseUpper == reserved)
        {
            reason = "'" + name + "' es un nombre de dispositivo reservado por Windows";
            return false;
        }
    }
    return true;
}

// True si 'inner' es exactamente 'root' o cae dentro de él. Compara vía
// DonTopo::exportPathKey (weakly_canonical + minúsculas + '/'), la misma
// normalización que usa el resto del módulo de export (GameExporter.h:36-42),
// para que mayúsculas o separadores mezclados no dejen colar una carpeta que
// en realidad sí está dentro de root.
bool isPathWithinOrEqual(const std::filesystem::path& inner, const std::filesystem::path& root)
{
    const std::string innerKey = DonTopo::exportPathKey(inner.string());
    const std::string rootKey  = DonTopo::exportPathKey(root.string());
    if (innerKey == rootKey)
        return true;
    // exportPathKey siempre normaliza a '/' como separador, así que
    // comprobar el prefijo "rootKey/" basta para detectar contención.
    return innerKey.size() > rootKey.size() &&
           innerKey.compare(0, rootKey.size(), rootKey) == 0 &&
           innerKey[rootKey.size()] == '/';
}

} // namespace

namespace DonTopo {

EditorUI::EditorUI()
    : m_sceneFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_exportDialog(std::make_unique<IGFD::FileDialog>())
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
        [this]() { m_animatorPanel.open(); },
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
    drawExportDialog();
    m_contentBrowserPanel.draw(ctx, sceneRoot);
    m_scriptEditor->draw();
    m_animatorPanel.draw(ctx);
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
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Export Game...", nullptr, false, m_scene != nullptr))
            {
                IGFD::FileDialogConfig cfg;
                cfg.path  = ".";
                cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_HideColumnDate |
                            ImGuiFileDialogFlags_DisableThumbnailMode |
                            ImGuiFileDialogFlags_DisablePlaceMode;
                // filters = nullptr -> IGFD selecciona carpeta, no fichero.
                m_exportDialog->OpenDialog("ExportDlg", "Carpeta destino del export", nullptr, cfg);
                m_exportDlgOpen = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem("Scene", nullptr, m_scenePanel.GetOpenPtr());
            ImGui::MenuItem("Viewport", nullptr, m_viewportPanel.GetOpenPtr());
            ImGui::MenuItem("Properties", nullptr, m_propertiesPanel.GetOpenPtr());
            ImGui::MenuItem("Log", nullptr, m_logPanel.GetOpenPtr());
            ImGui::MenuItem("Content Browser", nullptr, m_contentBrowserPanel.GetOpenPtr());
            ImGui::MenuItem("Script Editor", nullptr, m_scriptEditor->GetOpenPtr());
            ImGui::MenuItem("Animator", nullptr, m_animatorPanel.GetOpenPtr());
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
            // Aviso una sola vez al arrancar Play (no cada frame: el Renderer
            // consulta findCamera() en todos, y loguear ahí inundaría la
            // consola). Sin cámara, Play arranca igual con la del editor — que
            // se pueda iterar sin cámara importa más que forzar disciplina.
            if (!m_scene->findCamera())
                m_logPanel.push("No hay cámara en la escena; usando la del editor");
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
    {
        m_selected = nullptr; // la selección anterior ya no existe
        // Avisos de la carga (p.ej. escena con dos cámaras, donde fromJson se
        // queda con la primera): Core no conoce el Log Console, así que los
        // vuelca aquí quien sí lo conoce. Solo si loaded — una carga fallida
        // no modifica la escena y sus avisos no aplican.
        for (const auto& w : m_scene->lastWarnings())
            m_logPanel.push(w);
    }
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

void EditorUI::drawExportDialog()
{
    // Corre cada frame porque los dos BeginPopupModal de abajo (nombre y
    // confirmación) necesitan submitirse en todo frame para que ImGui los
    // mantenga abiertos tras el OpenPopup que los dispara — si esta función
    // no se llamara, el popup se cerraría solo aunque el usuario no pulsara
    // Cancel. (El Display("ExportDlg") sí es condicional a m_exportDlgOpen:
    // el && de abajo cortocircuita y no lo evalúa cuando el diálogo de
    // carpeta está cerrado.)
    if (m_exportDlgOpen && m_exportDialog->Display("ExportDlg"))
    {
        if (m_exportDialog->IsOk())
        {
            m_exportDestDir = m_exportDialog->GetCurrentPath();
            m_openExportNamePopup = true;
        }
        m_exportDialog->Close();
        m_exportDlgOpen = false;
    }

    if (m_openExportNamePopup)
    {
        ImGui::OpenPopup("Export Game");
        m_openExportNamePopup = false;
    }

    if (ImGui::BeginPopupModal("Export Game", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Destino: %s", m_exportDestDir.c_str());
        ImGui::InputText("Nombre", m_exportNameBuffer, sizeof(m_exportNameBuffer));

        // pkg es lo que realmente se va a crear/borrar: se calcula y se
        // enseña aquí (no el nombre crudo) para que el usuario evalúe la
        // ruta real, no un fragmento de texto que podría no coincidir con
        // ella (ver isValidExportGameName más arriba).
        const std::filesystem::path pkg =
            std::filesystem::path(m_exportDestDir) / m_exportNameBuffer;
        std::string nameError;
        const bool nameOk = isValidExportGameName(m_exportNameBuffer, nameError);
        if (nameOk)
            ImGui::Text("Paquete: %s", pkg.string().c_str());
        else
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", nameError.c_str());

        ImGui::BeginDisabled(!nameOk);
        if (ImGui::Button("Export"))
        {
            std::error_code ec;
            const bool exists = std::filesystem::exists(pkg, ec);
            // Si exists() falla, ec queda set y el valor de retorno es
            // false — es decir, "no puedo saberlo" se leería como "no
            // existe" y nos saltaríamos la confirmación justo antes de un
            // remove_all(). Tratamos cualquier fallo de la consulta como si
            // la carpeta existiera: falla cerrado, no abierto. Se guarda cuál
            // de los dos casos fue para que el popup no mienta diciendo
            // "ya existe" cuando en realidad no se pudo comprobar.
            m_exportExistsCheckFailed = static_cast<bool>(ec);
            if (exists || ec)
                m_openExportConfirmPopup = true;
            else
                runExport();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (m_openExportConfirmPopup)
    {
        ImGui::OpenPopup("Sobrescribir export");
        m_openExportConfirmPopup = false;
    }

    if (ImGui::BeginPopupModal("Sobrescribir export", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        // Misma ruta resuelta que el popup anterior, no el nombre crudo: es
        // literalmente lo que remove_all() va a borrar si el usuario
        // confirma, y el nombre por sí solo no lo representa (ver hallazgo
        // de review: "La carpeta '..' ya existe" no dice "voy a borrar
        // C:\Users\ruben").
        const std::filesystem::path pkg =
            std::filesystem::path(m_exportDestDir) / m_exportNameBuffer;
        // Falla cerrado (ver comentario del botón Export): se llega aquí
        // tanto si pkg existe de verdad como si exists() no pudo
        // determinarlo. Son dos afirmaciones distintas y solo una es cierta
        // en cada caso — mentir sobre cuál fue socava la confirmación.
        if (m_exportExistsCheckFailed)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                               "No se pudo comprobar si '%s' ya existe.", pkg.string().c_str());
        else
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                               "La carpeta '%s' ya existe.", pkg.string().c_str());
        ImGui::Text("Se borrara todo su contenido antes de exportar.");
        if (ImGui::Button("Borrar y exportar"))
        {
            runExport();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void EditorUI::runExport()
{
    namespace fs = std::filesystem;

    // runExport es la última función antes del remove_all() destructivo, y
    // hoy confía en que BeginDisabled(!nameOk) del popup haya bloqueado todo
    // camino inválido. Ese invariante vive en drawExportDialog, no aquí: el
    // guardián de un borrado irreversible no debería depender de un flag de
    // UI ajeno. Se revalida el nombre aquí mismo, por si acaso.
    std::string nameError;
    if (!isValidExportGameName(m_exportNameBuffer, nameError))
    {
        m_logPanel.push("Export cancelado: nombre invalido (" + nameError + ")");
        return;
    }

    if (!m_scene)
    {
        m_logPanel.push("Export cancelado: no hay escena abierta");
        return;
    }
    // Sin camara el juego no podria renderizar: se falla aqui, donde el
    // usuario puede arreglarlo, y no en un .exe que abre una ventana negra.
    if (!m_scene->findCamera())
    {
        m_logPanel.push("Export cancelado: la escena no tiene camara (Add > Camera en Properties)");
        return;
    }

    std::error_code ec;
    fs::path projectRoot = fs::current_path(ec);
    if (ec) projectRoot = ".";
    fs::path canon = fs::canonical(projectRoot, ec);
    if (!ec) projectRoot = canon;

    const fs::path runtimeExe = projectRoot / "DonTopoRuntime.exe";
    if (!fs::exists(runtimeExe, ec))
    {
        m_logPanel.push("Export cancelado: falta " + runtimeExe.string() +
                        ". Compila el target DonTopoRuntime.");
        return;
    }

    const fs::path scriptsDir = m_scriptManager ? m_scriptManager->scriptsDirPath()
                                                : projectRoot / "Scripts";

    // writeExportPackage() hace remove_all(destDir/gameName) sin validar
    // nada (GameExporter.h:68-70): si ese path cae dentro del proyecto o
    // coincide con la carpeta de scripts, el borrado se lleva por delante
    // los assets/scripts originales ANTES de que collectSceneAssets/copyOne
    // lleguen a leerlos, y el export encima falla después porque ya no
    // encuentra lo que acaba de borrar. Se aborta aquí, antes de tocar nada.
    const fs::path pkg = fs::path(m_exportDestDir) / m_exportNameBuffer;

    // isPathWithinOrEqual(inner, root) es direccional: solo responde "¿inner
    // cae dentro de root?". Faltaba el sentido contrario -- que pkg CONTENGA
    // al proyecto o a scripts. Ejemplo real con este repo: projectRoot es
    // <repo>\build-ninja\sandbox y scriptsDir es <repo>\Scripts; con destino
    // "C:\Users\ruben\Documents" y nombre "Don_Topo_Engine", pkg =
    // C:\Users\ruben\Documents\Don_Topo_Engine contiene a los dos. Sin este
    // chequeo ninguna de las dos guardas de abajo saltaba y writeExportPackage
    // hacía remove_all() sobre el repositorio entero (.git, Scripts/,
    // assets/, el propio árbol de build) -- el espejo estrictamente peor del
    // caso que estas guardas existen para prevenir. Se reusa el mismo
    // predicado invirtiendo los argumentos.
    if (isPathWithinOrEqual(pkg, projectRoot))
    {
        m_logPanel.push("Export cancelado: el destino '" + pkg.string() +
                        "' cae dentro de la carpeta del proyecto (" + projectRoot.string() +
                        "); el export borraria los assets originales antes de copiarlos.");
        return;
    }
    if (isPathWithinOrEqual(projectRoot, pkg))
    {
        m_logPanel.push("Export cancelado: el destino '" + pkg.string() +
                        "' contiene a la carpeta del proyecto (" + projectRoot.string() +
                        "); el export borraria el proyecto entero (incluido el propio editor) antes de exportar.");
        return;
    }
    if (isPathWithinOrEqual(pkg, scriptsDir))
    {
        m_logPanel.push("Export cancelado: el destino '" + pkg.string() +
                        "' coincide con la carpeta de scripts (" + scriptsDir.string() +
                        ") o cae dentro de ella.");
        return;
    }
    if (isPathWithinOrEqual(scriptsDir, pkg))
    {
        m_logPanel.push("Export cancelado: el destino '" + pkg.string() +
                        "' contiene a la carpeta de scripts (" + scriptsDir.string() +
                        "); el export borraria los scripts originales antes de copiarlos.");
        return;
    }

    std::map<std::string, fs::path> scriptPaths;
    if (m_scriptManager)
        for (const auto& [name, cls] : m_scriptManager->getRegistry())
            scriptPaths[name] = cls.path;

    std::vector<ExportAsset> assets = collectSceneAssets(*m_scene, projectRoot, scriptPaths);

    std::vector<std::string> missing;
    for (const ExportAsset& a : assets)
        if (!a.existsOnDisk) missing.push_back(a.sourcePath);
    if (!missing.empty())
    {
        m_logPanel.push("Export cancelado: faltan en disco " +
                        std::to_string(missing.size()) + " assets referenciados:");
        for (const std::string& m : missing)
            m_logPanel.push("  " + m);
        return;
    }

    std::map<std::string, std::string> sourceToPackage;
    for (const ExportAsset& a : assets)
        sourceToPackage[exportPathKey(a.sourcePath)] = a.packagePath;

    nlohmann::json sceneJson = m_scene->toJson();
    rewriteScenePaths(sceneJson, sourceToPackage);

    ExportResult result = writeExportPackage(assets, sceneJson, m_exportDestDir,
                                             m_exportNameBuffer, projectRoot,
                                             scriptsDir, runtimeExe);
    for (const std::string& msg : result.messages)
        m_logPanel.push(msg);
    if (!result.ok)
        m_logPanel.push("Export FALLIDO");
}

} // namespace DonTopo
