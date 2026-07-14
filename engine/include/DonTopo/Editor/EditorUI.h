#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include "DonTopo/Editor/UndoManager.h"
#include "DonTopo/Editor/LogPanel.h"
#include "DonTopo/Editor/ScenePanel.h"
#include "DonTopo/Editor/ViewportPanel.h"
#include "DonTopo/Editor/PropertiesPanel.h"

namespace IGFD { class FileDialog; }

namespace DonTopo {

class GameObject;
class Mesh;
class PhysicsManager;
class AudioManager;
class Renderer;
class Camera;
class Scene;
class ScriptManager;
class ScriptEditorPanel;

class EditorUI {
public:
    EditorUI();
    ~EditorUI();
    EditorUI(const EditorUI&)            = delete;
    EditorUI& operator=(const EditorUI&) = delete;

    void draw(VkDescriptorSet viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView);

    bool isViewportHovered() const { return m_viewportPanel.isHovered(); }

    // true mientras Play Mode está activo (física + audio corriendo).
    bool isPlaying() const { return m_isPlaying; }

    // Notificado justo antes de desenganchar node de su padre (node sigue
    // siendo válido y su subárbol completo también), para que el dueño
    // pueda liberar recursos externos (meshes/texturas en GPU) asociados.
    void setOnDelete(std::function<void(GameObject*)> cb) { m_onDelete = std::move(cb); }
    // Llamado con el eje mundo (1,0,0 / 0,1,0 / 0,0,1) al clicar la bola del axis gizmo.
    void setOnAxisSelected(std::function<void(const glm::vec3&)> cb) { m_onAxisSelected = std::move(cb); }
    // Puntero no-propietario: PhysicsManager vive en main.cpp, fuera del
    // ciclo de vida del EditorUI. Necesario para crear el actor PhysX al
    // pulsar "Add > Box Collider" desde el panel Properties.
    void setPhysicsManager(PhysicsManager* physics) { m_physics = physics; }
    // Puntero no-propietario: AudioManager vive fuera del EditorUI (ver
    // wiring en sandbox/main.cpp), mismo patrón que m_physics. Necesario
    // para cargar/reproducir clips desde la sección Audio Clip del panel
    // Properties.
    void setAudioManager(AudioManager* audio) { m_audio = audio; }
    // Puntero no-propietario: Scene vive en main.cpp, fuera del ciclo de
    // vida del EditorUI (mismo patrón que m_physics/m_audio). Necesario
    // para delegar el borrado diferido (ScenePanel::m_pendingDelete) en
    // Scene::removeGameObject en vez de mutar children a mano.
    void setScene(Scene* scene) { m_scene = scene; }
    // Puntero no-propietario: Renderer es dueño de este EditorUI y se pasa a sí
    // mismo desde setSceneRoot. Necesario para registrar el mesh GPU (addStaticMesh)
    // al crear un shape desde el menú "Basic Shapes".
    void setRenderer(Renderer* renderer) { m_renderer = renderer; }
    // Centra la cámara en m_selected (no-op si no hay selección). Usado por
    // el atajo de teclado "F" en main.cpp.
    void focusSelected(Camera& camera);
    // Puntero no-propietario, mismo patrón que m_physics. Necesario para
    // disparar el ciclo de vida al pulsar Play/Stop y para la sección
    // Scripts del panel Properties (Task 10).
    void setScriptManager(ScriptManager* sm) { m_scriptManager = sm; }
    // Punto de entrada externo al Log Console (usado por ScriptManager vía
    // el wiring de main.cpp: mensajes de compilación/errores de scripts).
    void pushExternalLog(const std::string& message) { m_logPanel.push(message); }

private:
    static constexpr float kToolbarHeight = 30.0f;
    // Ctrl+Z/Ctrl+Y — no-op si !m_scene, si m_isPlaying, o si algún widget de
    // texto tiene el foco (WantTextInput, evita chocar con el undo nativo de
    // un ImGuiInputTextMultiline como el del Script Editor).
    void handleUndoRedoShortcut();
    void drawMenuBar();
    void drawToolbar();
    void drawDockSpace();
    void drawSceneDialog();
    // Limpia GPU de la escena actual, reemplaza su contenido con j (vía
    // Scene::fromJson) y re-registra GPU (estático + skinned) de lo que
    // quede — tanto si fromJson tuvo éxito (árbol nuevo) como si falló
    // (árbol viejo intacto, con índices reseteados antes de liberar GPU).
    // Limpia m_selected si fromJson tuvo éxito. Devuelve lo que devuelva
    // fromJson. Usado por drawSceneDialog (Load Scene) y por el handler de
    // Stop en drawToolbar. false sin efecto si falta algún puntero
    // (m_scene/m_renderer/m_physics/m_audio).
    bool reloadSceneFromJson(const nlohmann::json& j);
    void drawContentBrowser(GameObject* sceneRoot);
    // Arma el popup modal "Rename Asset" precargado con el nombre actual de
    // path (stem si es fichero, nombre completo si es carpeta).
    void beginAssetRename(const std::filesystem::path& path, bool isDir);
    // Recorre sceneRoot actualizando Mesh::sourcePath, los 3 paths de
    // Material y AudioClipComponent::getPath() que matcheen oldPath (exacto
    // si !isDir, por prefijo si isDir) al nuevo valor tras un rename en
    // disco ya realizado.
    void updateSceneReferencesForRename(GameObject* sceneRoot,
                                         const std::filesystem::path& oldPath,
                                         const std::filesystem::path& newPath,
                                         bool isDir);
    // Arma el popup modal "Delete Asset", precalculando cuántos GameObjects
    // referencian path (mesh o audio) para mostrarlo en el texto de aviso.
    void beginAssetDelete(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir);
    // Cuenta cuántos GameObjects de sceneRoot referencian path (mesh o
    // audio; exacto si !isDir, por prefijo si isDir).
    int countSceneReferences(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir);
    // Desengancha de la escena cualquier referencia a path antes de
    // borrarlo de disco: mesh en uso -> Renderer::removeMeshComponent;
    // audio en uso -> setAudioClip(nullptr); textura de Material en uso ->
    // limpia el campo de path (Task 5 añade el hot-swap de GPU aquí).
    void detachSceneReferencesForDelete(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir);

    // Visibilidad de paneles — togglable desde el menú View (drawMenuBar).
    // Todos empiezan visibles; cerrar solo oculta la ventana ImGui, el
    // estado interno de cada panel (selección, scroll, tabs de Script
    // Editor...) no se pierde mientras está oculto.
    bool m_contentBrowserOpen = true;

    // Log Console — extraído a LogPanel (Task 2).
    LogPanel m_logPanel;
    // Scene — árbol jerárquico de GameObjects, extraído a ScenePanel (Task 3).
    ScenePanel m_scenePanel;
    // Viewport — render 3D embebido + gizmo de selección, extraído a
    // ViewportPanel (Task 4).
    ViewportPanel m_viewportPanel;
    // Properties — transform/colliders/mesh/audio/scripts/add-component,
    // extraído a PropertiesPanel (Task 5).
    PropertiesPanel m_propertiesPanel;

    // Content Browser
    bool m_dlgOpen = false;
    bool m_scanned = false;
    std::string m_currentDir;
    // Raíz del proyecto (canonicalizada una vez); el Content Browser no deja
    // navegar por encima de este path (ni vía ".." ni vía breadcrumb).
    std::filesystem::path m_projectRoot;
    // Path al que reabrir el diálogo IGFD la próxima vez que !m_dlgOpen;
    // vacío = reabrir en m_projectRoot. Se consume (se vacía) en cada
    // reapertura — quien quiera reabrir en una carpeta concreta debe
    // asignarlo de nuevo antes de poner m_dlgOpen = false.
    std::string m_dlgReopenPath;
    std::vector<std::filesystem::path> m_assets;

    // Asset rename — popup modal disparado por right-click > Rename en el
    // grid derecho del Content Browser.
    std::filesystem::path m_assetRenameTarget;
    bool                   m_assetRenameIsDir = false;
    char                   m_assetRenameBuffer[128] = {};
    std::string            m_assetRenameError;
    bool                   m_openAssetRenamePopup = false;

    // Asset delete — popup modal disparado por right-click > Delete.
    std::filesystem::path m_assetDeleteTarget;
    bool                   m_assetDeleteIsDir = false;
    int                    m_assetDeleteAffectedCount = 0;
    bool                   m_openAssetDeletePopup = false;
    std::string            m_assetDeleteError;

    // Scene save/load — instancia propia de diálogo, mismo motivo que los
    // diálogos de PropertiesPanel (Instance() singleton no soporta
    // diálogos concurrentes). Se reusa la misma instancia para Save y Load
    // porque nunca están abiertos a la vez (ambos disparados desde botones
    // secuenciales del toolbar).
    std::unique_ptr<IGFD::FileDialog> m_sceneFileDialog;
    bool        m_sceneDlgOpen = false;
    bool        m_sceneDlgIsSave = false;
    // Último error de guardado/carga de escena (vacío si ninguno pendiente).
    std::string m_sceneIOError;

    // Play Mode — snapshot en RAM del árbol justo antes de pulsar Play,
    // restaurado íntegro al pulsar Stop (tipo Unity Play-In-Editor). No se
    // bloquea la edición mientras está activo — cualquier cambio se
    // descarta igual al restaurar.
    bool           m_isPlaying = false;
    nlohmann::json m_playSnapshot;

    // Undo/Redo — historial de las últimas 50 acciones de edición (Transform,
    // propiedades de collider, Create/Delete/Reparent GameObject). Se resetea
    // en Load Scene y al entrar/salir de Play Mode (ver reloadSceneFromJson y
    // el handler de Play en drawToolbar).
    UndoManager m_undoHistory;

    // Scene selection
    GameObject* m_selected = nullptr;
    std::function<void(GameObject*)> m_onDelete;
    std::function<void(const glm::vec3&)> m_onAxisSelected;

    PhysicsManager* m_physics = nullptr;
    Renderer*       m_renderer = nullptr;
    AudioManager*   m_audio = nullptr;
    Scene*          m_scene = nullptr;
    ScriptManager*  m_scriptManager = nullptr;

    // Panel de edición de código .lua (Task: Script Editor Panel). unique_ptr
    // + forward declaration para no arrastrar <TextEditor.h>/<imgui.h> a todo
    // el que incluya EditorUI.h — mismo patrón que m_sceneFileDialog.
    std::unique_ptr<ScriptEditorPanel> m_scriptEditor;
};

} // namespace DonTopo
