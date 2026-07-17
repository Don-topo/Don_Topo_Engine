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
#include "DonTopo/Editor/ContentBrowserPanel.h"
#include "DonTopo/Editor/AnimatorPanel.h"

namespace IGFD { class FileDialog; }

namespace DonTopo {

class GameObject;
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

    // Suelta m_selected si cae dentro del subárbol de node (incluido node).
    // Lo llama el destroy de Play (ScriptManager, vía Renderer) JUSTO antes de
    // liberar el GameObject: de lo contrario el editor conservaría un puntero
    // colgante y crashearía al dibujar Properties/gizmo el frame siguiente.
    // Es el mismo saneo que hace ScenePanel al borrar desde la jerarquía.
    void onGameObjectDestroyed(GameObject* node);

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
    // Content Browser — explorador de assets con rename/delete, extraído a
    // ContentBrowserPanel (Task 6).
    ContentBrowserPanel m_contentBrowserPanel;

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
    AnimatorPanel m_animatorPanel;
};

} // namespace DonTopo
