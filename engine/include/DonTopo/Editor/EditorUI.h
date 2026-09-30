#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <vector>
#include <string>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include "DonTopo/Editor/UndoManager.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Editor/LogPanel.h"
#include "DonTopo/Editor/ScenePanel.h"
#include "DonTopo/Editor/ViewportPanel.h"
#include "DonTopo/Editor/PropertiesPanel.h"
#include "DonTopo/Editor/ContentBrowserPanel.h"
#include "DonTopo/Editor/AnimatorPanel.h"
#include "DonTopo/Editor/SpriteEditorPanel.h"
#include "DonTopo/Editor/InputActionsPanel.h"
#include "DonTopo/Editor/PerformancePanel.h"
#include "DonTopo/Editor/RenderingPanel.h"
#include "DonTopo/Editor/LoadingModal.h"
#include "DonTopo/Editor/ProjectContext.h"
#include "DonTopo/Renderer/AsyncAssetLoader.h"
#include "DonTopo/Renderer/RenderBackend.h"
#include "DonTopo/Renderer/UiLayer.h"

namespace IGFD { class FileDialog; }

namespace DonTopo {

class GameObject;
class PhysicsManager;
class AudioManager;
class JobSystem;
class Renderer;
class EditorRenderer;
class Camera;
class Scene;
class ScriptManager;
class ScriptEditorPanel;

// The editor owns the Renderer, and not the other way around: this way the engine does not
// have to know the editor. The relationship is also inverted in drawing: the
// Renderer calls back through the UiLayer interface, which this class implements.
class EditorUI : public UiLayer {
public:
    EditorUI();
    ~EditorUI() override;
    EditorUI(const EditorUI&)            = delete;
    EditorUI& operator=(const EditorUI&) = delete;

    // The backend this editor owns. Whoever builds it knows which one it is
    // (Vulkan or DirectX 12) and hands over ownership here; the editor only uses it
    // through the interface, so from this point on it does not care.
    //
    // It has to be set BEFORE draw(): without a backend there are no panels to draw.
    void setRenderer(std::unique_ptr<EditorRenderer> renderer);

    // The one that was set. A reference and not a pointer: calling here without having
    // set it is an assembly error, not a situation to handle.
    EditorRenderer& renderer();

    // viewportTexture is opaque: the backend that created it knows what it is (a
    // VkDescriptorSet or a D3D12 GPU descriptor) and here it only travels to
    // ImGui::Image, which treats it the same in both cases.
    void draw(uint64_t viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView);

    bool isViewportHovered() const { return m_viewportPanel.isHovered(); }

    // To feed the game UI's input from the loop: corner of the
    // viewport image and whether the mouse is right over it.
    glm::vec2 viewportImagePos()     const { return m_viewportPanel.imagePos(); }
    bool      isViewportImageHovered() const { return m_viewportPanel.imageHovered(); }

    // ── UiLayer ──────────────────────────────────────────────────────────────
    // The ImGui backend (context, descriptor pool and the two _Impl_)
    // lives here because it is an editor detail, not an engine one.
    void     initUi(const InitInfo& info) override;
    void     shutdownUi() override;
    uint64_t registerUiTexture(uint64_t sampler, uint64_t view) override;
    void     unregisterUiTexture(uint64_t handle) override;
    void     buildUiFrame(uint64_t viewportTexture, GameObject* sceneRoot,
                          const glm::mat4& cameraView) override;
    void     recordUi(void* commandList) override;

    // true while Play Mode is active (physics + audio running).
    bool isPlaying() const override { return m_isPlaying; }

    // Notified right before detaching node from its parent (node is still
    // valid and its whole subtree too), so that the owner
    // can release associated external resources (meshes/textures on the GPU).
    // Called with the world axis (1,0,0 / 0,1,0 / 0,0,1) when clicking the axis gizmo ball.
    void setOnAxisSelected(std::function<void(const glm::vec3&)> cb) { m_onAxisSelected = std::move(cb); }
    // Non-owning pointer: PhysicsManager lives in main.cpp, outside the
    // EditorUI lifecycle. Needed to create the PhysX actor when
    // pressing "Add > Box Collider" from the Properties panel.
    void setPhysicsManager(PhysicsManager* physics) { m_physics = physics; }
    // Non-owning pointer: AudioManager lives outside the EditorUI (see the
    // wiring in sandbox/main.cpp), same pattern as m_physics. Needed
    // to load/play clips from the Audio Clip section of the Properties
    // panel.
    void setAudioManager(AudioManager* audio) { m_audio = audio; }
    // Non-owning pointer: Scene lives in main.cpp, outside the EditorUI
    // lifecycle (same pattern as m_physics/m_audio). Needed
    // to delegate the deferred deletion (ScenePanel::m_pendingDelete) to
    // Scene::removeGameObject instead of mutating children by hand.
    //
    // Out of line, same reason as setProject: it also sets the asset root
    // if the project is ALREADY set. See the comment next to the
    // definition in EditorUI.cpp for why the same assignment lives in
    // BOTH setters (setScene and setProject) and is not duplication to clean up.
    void setScene(Scene* scene);
    // Centers the camera on m_selected (no-op if there is no selection). Used by
    // the "F" keyboard shortcut in main.cpp.
    void focusSelected(Camera& camera);
    // Non-owning pointer, same pattern as m_physics. Needed to
    // trigger the lifecycle when pressing Play/Stop and for the Scripts
    // section of the Properties panel (Task 10).
    void setScriptManager(ScriptManager* sm) { m_scriptManager = sm; }
    // External entry point to the Log Console (used by ScriptManager via
    // main.cpp's wiring: compilation messages/script errors).
    void pushExternalLog(const std::string& message) { m_logPanel.push(message); }

    // Drops m_selected if it falls inside node's subtree (node included).
    // Called by Play's destroy (ScriptManager, via Renderer) RIGHT before
    // releasing the GameObject: otherwise the editor would keep a dangling
    // pointer and crash when drawing Properties/gizmo the next frame.
    // It is the same sanitizing that ScenePanel does when deleting from the hierarchy.
    void onGameObjectDestroyed(GameObject* node);

    // Applies to the scene the results returned by AsyncAssetLoader::pumpCompleted
    // and closes the batch with a SINGLE flushPendingUploads. Called by main.cpp's
    // per-frame pump.
    void onAssetsLoaded(std::vector<LoadedMesh> results, Scene& scene, EditorRenderer& renderer);

    // Filled in by main() before the loop; without it, drops enqueue nothing and
    // Load Scene stays on the synchronous path.
    void setAssetLoader(AsyncAssetLoader* loader) { m_assetLoader = loader; }
    // Filled in by main() before the loop: drains the queue of files dropped
    // onto the window from outside the process (glfwSetDropCallback). Empty
    // by default: without a provider, ctx.takeDroppedFiles reaches the
    // Content Browser empty and nothing is imported, same pattern as setAssetLoader.
    void setDroppedFilesProvider(std::function<std::vector<DroppedFile>()> fn)
    { m_droppedFilesProvider = std::move(fn); }
    // Filled in by main() before the loop, like setAssetLoader. Without it there are no
    // thumbnails in the Content Browser.
    void setJobSystem(JobSystem* jobs) { m_jobSystem = jobs; }

    // Project selector: first state of the ImGui loop. While a
    // selector is set, draw() gives it the WHOLE frame and draws no menu, toolbar,
    // dockspace or panel at all: the editor does not appear until
    // the callback returns true (project chosen), and then it is released and the next
    // frame is already the usual editor. The selector's UI lives in
    // main(): here the turn is only yielded.
    void setProjectSelector(std::function<bool()> fn) { m_projectSelector = std::move(fn); }
    bool isProjectSelectorActive() const { return static_cast<bool>(m_projectSelector); }

    // Chosen project, non-owning (lives in main()). It travels to the panels through
    // EditorContext::project: it is what decides which paths can be read/written.
    //
    // Out of line (not inline here): it also sets the asset root of the
    // live scene (Scene::setAssetRoot), and Scene is only forward-declared in
    // this header. See the comment next to the definition in EditorUI.cpp for
    // why it lives HERE and not in applyProjectSettings.
    void setProject(const ProjectContext* project);

    // Backend this process STARTED with, which main() already resolved before
    // creating the Renderer. The View menu combo compares it with the chosen one to
    // know whether a restart is needed; the editor never changes it live.
    void setActiveRenderBackend(RenderBackend backend) { m_activeBackend = backend; }

    // Opens the project's startup scene (ProjectContext::kStartupScene), the
    // one create() leaves made: empty, so on entering only the skybox is seen. It goes
    // through the SAME path as the File menu's Load Scene, so it replaces whatever
    // was loaded (the demo scene from startup) and clears the undo. If the
    // project does not have that scene (one created before it existed) it touches
    // nothing and leaves a line in the Log.
    bool openProjectScene();

private:
    static constexpr float kToolbarHeight = 30.0f;
    // Ctrl+Z/Ctrl+Y/Ctrl+D: no-op if !m_scene or if any text widget has
    // focus (WantTextInput, avoids clashing with the native undo of an
    // ImGuiInputTextMultiline like the Script Editor's). In Play it does work:
    // see the implementation's comment (A11).
    void handleUndoRedoShortcut();
    // W/E/R: viewport gizmo mode (move / rotate / scale), the same
    // keys as Unity. They can be reused because the fly camera now only
    // listens to the keyboard with the RIGHT BUTTON held (see Camera::update): without
    // that split, W would move the camera forward and E would raise it.
    //
    // It does not share gating with handleUndoRedoShortcut: that one requires Ctrl and this one does not.
    void handleGizmoModeShortcut();
    // Ctrl+D: duplicates m_selected as its SIBLING (same parent, not child).
    // Same gating as undo/redo: called by handleUndoRedoShortcut.
    void duplicateSelection();
    void drawMenuBar();
    void drawToolbar();
    void drawDockSpace();
    void drawSceneDialog();
    // Path sandbox guard: true if `path` can be read/written with the
    // project open. Without a project (headless tests, startup before the
    // selector) it lets everything through, which is how the editor behaved before. If
    // it rejects, it leaves a line in the Log and the caller returns without touching disk.
    bool projectAllows(const std::filesystem::path& path, const char* what);
    // --- View menu settings persisted per project -------------------------
    // Snapshot of the current state: the effects are read from the Renderer and the panels'
    // visibility from their GetOpenPtr(). The Renderer is the only source of
    // truth, so there is no copy to keep synchronized control by control.
    ProjectContext::ViewSettings currentSettings();

    // The `open` flag of each persisted panel, indexed by
    // ViewSettings::Panel. ONE place where a panel is tied to its index:
    // saving and applying both read from here, so they cannot get out of sync.
    //
    // Before there were two hand-written lists, one in currentSettings() and another in
    // applyProjectSettings(), and the Rendering panel was left out of both: its
    // docked position was still in imgui.ini, but it started closed and the
    // tab disappeared. A panel missing here is missing in both halves at
    // once, which is a visible failure, not one that gets half lost.
    //
    // A pointer can be nullptr (the Script Editor does not exist without a project):
    // that counts as closed when saving and nothing is applied when reading.
    std::array<bool*, ProjectContext::ViewSettings::PanelCount> panelOpenPtrs();
    // Dumps to the Renderer and to the panels the settings of the newly
    // opened project. Called every frame from draw() and only does something when
    // m_project changes. Without a project (headless tests) it touches nothing.
    void applyProjectSettings();
    // Writes currentSettings() to project.json. Called by the View menu
    // controls when the change ENDS (checkbox/combo click, or a slider's
    // IsItemDeactivatedAfterEdit), never per drag frame.
    void saveProjectSettings();

    // The toolbar's Wireframe button is the only render setting
    // left here: the other 41 live in RenderingPanel since H58,
    // with their wrappers in RenderSettingControls. This one draws and applies on its
    // own (it is a button with its own style, not one of the widgets there), so
    // it only needs to register the step.
    //
    // It does nothing if before == after.
    template <typename T>
    void pushRenderUndo(const char* label, const T& before, const T& after,
                        std::function<void(const T&)> set)
    {
        if (before == after) return;
        m_undoHistory.push(makeRenderSettingCommand<T>(label, before, after, std::move(set),
                                                        [this] { saveProjectSettings(); }),
                            /*dirtiesScene=*/false);
        saveProjectSettings();
    }
    // Clears the current scene's GPU, replaces its contents with j (via
    // Scene::fromJson) and re-registers the GPU (static + skinned) of whatever
    // remains, both if fromJson succeeded (new tree) and if it failed
    // (old tree intact, with indices reset before releasing the GPU).
    // Clears m_selected if fromJson succeeded. Returns whatever
    // fromJson returns. Used by drawSceneDialog (Load Scene) and by the
    // Stop handler in drawToolbar. false with no effect if any pointer is missing
    // (m_scene/m_renderer/m_physics/m_audio).
    // async == true (Load Scene): the meshes are enqueued in m_assetLoader and the
    // progress modal opens. async == false (Play->Stop restore and
    // any path without a loader): synchronous, deterministic load, no modal.
    bool reloadSceneFromJson(const nlohmann::json& j, bool async);
    // Loads the scene from file path: validates the JSON structure, reloads
    // the scene (async) and reports the result through m_sceneIOError/Log. Single
    // load path per file: used by the File menu's Load Scene and by the
    // click on a .json in the Content Browser (ctx.requestLoadScene).
    bool loadSceneFile(const std::string& path);
    // Export Game: drains the destination folder dialog and draws the two
    // modal popups (name and overwrite confirmation). Called every
    // frame from draw(), like drawSceneDialog.
    void drawExportDialog();
    // Runs the full export with m_exportDestDir and m_exportNameBuffer already
    // set. Dumps both the errors and the summary to the Log Console.
    void runExport();

    // Log Console: extracted to LogPanel (Task 2).
    LogPanel m_logPanel;
    // Scene: hierarchical tree of GameObjects, extracted to ScenePanel (Task 3).
    ScenePanel m_scenePanel;
    // Viewport: embedded 3D render + selection gizmo, extracted to
    // ViewportPanel (Task 4).
    ViewportPanel m_viewportPanel;
    // Properties: transform/colliders/mesh/audio/scripts/add-component,
    // extracted to PropertiesPanel (Task 5).
    PropertiesPanel m_propertiesPanel;
    // Content Browser: asset explorer with rename/delete, extracted to
    // ContentBrowserPanel (Task 6).
    ContentBrowserPanel m_contentBrowserPanel;

    // Scene save/load: own dialog instance, same reason as the
    // PropertiesPanel dialogs (the Instance() singleton does not support
    // concurrent dialogs). The same instance is reused for Save and Load
    // because they are never open at the same time (both triggered from
    // sequential toolbar buttons).
    std::unique_ptr<IGFD::FileDialog> m_sceneFileDialog;
    bool        m_sceneDlgOpen = false;
    bool        m_sceneDlgIsSave = false;
    // Last scene save/load error (empty if none pending).
    std::string m_sceneIOError;
    // File of the in-memory scene (last one saved or successfully loaded).
    // Empty = a scene that was never saved: ctx.requestSaveScene has to go
    // through the Save Scene dialog instead of writing directly.
    std::string m_currentScenePath;
    // Scene to load as soon as the in-flight Save Scene dialog
    // confirms and saves (the "Save" of the Content Browser modal on a
    // scene without a file). Empty = the dialog does not chain any load.
    std::string m_pendingSceneLoadAfterSave;

    // Export Game: own dialog instance for the same reason as
    // m_sceneFileDialog: IGFD::FileDialog::Instance() does not support
    // concurrent dialogs.
    std::unique_ptr<IGFD::FileDialog> m_exportDialog;
    bool        m_exportDlgOpen          = false;
    std::string m_exportDestDir;
    char        m_exportNameBuffer[64]   = "Game";
    bool        m_openExportNamePopup    = false;
    bool        m_openExportConfirmPopup = false;

    // Play Mode: in-RAM snapshot of the tree right before pressing Play,
    // fully restored when pressing Stop (Unity Play-In-Editor style). Editing is not
    // blocked while it is active: any change is
    // discarded anyway on restore.
    // SSAA factor slider. Changing it recreates ALL the internal targets,
    // so it is applied on RELEASE and the in-flight value has to be kept. It is
    // a member and not a function static: that one survived the project
    // change, and its refresh depended on IsAnyItemActive(), which is global.
    // Project sky folder. It is edited in the View menu and applied on
    // pressing, not on every key: reloading it releases the cubemap and reconvolves
    // the global IBL.
    char           m_skyboxFolder[260] = "assets/skybox";
    bool           m_environmentWindowOpen = false;
    bool           m_skyboxDlgOpen         = false;
    std::unique_ptr<IGFD::FileDialog> m_skyboxDialog;
    // Sky window: navigate to the folder, drag it from the Content
    // Browser or type it. In a window and not in the View menu because on a
    // menu popup a drag cannot be released.
    void drawEnvironmentWindow();
    void applySkyboxFolder(const std::string& folder);

    // The drag state of the render controls went with them to
    // RenderSettingControls (H58): it belongs to those widgets, not to the editor.

    bool           m_isPlaying = false;
    nlohmann::json m_playSnapshot;

    // Undo/Redo: history of the last 50 edit actions (Transform,
    // collider properties, Create/Delete/Reparent GameObject). It is reset
    // on Load Scene and when entering/leaving Play Mode (see reloadSceneFromJson and
    // the Play handler in drawToolbar).
    UndoManager m_undoHistory;

    // Scene selection
    GameObject* m_selected = nullptr;
    std::function<void(const glm::vec3&)> m_onAxisSelected;

    // Which API the interface was started with. Decides which ImGui backend is used
    // at each point: init, begin frame, record and close.
    GraphicsApi m_api = GraphicsApi::Vulkan;

#ifdef DT_D3D12_ENABLED
    // Split of the descriptor range that the backend hands to the interface.
    // ImGui requests and releases on its own, so a count has to be kept.
    struct D3D12SrvPool {
        uint64_t              cpuStart = 0;
        uint64_t              gpuStart = 0;
        uint32_t              stride   = 0;
        uint32_t              capacity = 0;
        uint32_t              next     = 0;
        std::vector<unsigned> released;
    };
    D3D12SrvPool m_d3dSrvPool;

    void initUiD3D12(const InitInfo& info);
#endif

    PhysicsManager* m_physics = nullptr;
    // Ownership: the editor keeps alive the backend it was given. unique_ptr to an
    // incomplete type, destroyed in the .cpp.
    std::unique_ptr<EditorRenderer> m_renderer;
    AudioManager*   m_audio = nullptr;
    // Buffer of the audio load failure pump (EditorUI::draw). A member and
    // not a local so as not to reallocate a vector on every frame; it is cleared before
    // each use.
    std::vector<std::string> m_audioFailures;
    // The FMOD no-output warning is shown ONCE, not one per frame.
    bool                     m_audioOutputWarned = false;
    Scene*          m_scene = nullptr;
    ScriptManager*  m_scriptManager = nullptr;

    // .lua code editing panel (Task: Script Editor Panel). unique_ptr
    // + forward declaration so as not to drag <TextEditor.h>/<imgui.h> into everything
    // that includes EditorUI.h, same pattern as m_sceneFileDialog.
    std::unique_ptr<ScriptEditorPanel> m_scriptEditor;
    AnimatorPanel m_animatorPanel;
    // Atlas sub-rect editor. It is opened from a Button's Atlas field, and the
    // request arrives DEFERRED: whoever asks for it (PropertiesPanel) does so
    // while the EditorContext is being built, which is exactly what
    // the panel needs to open the image.
    SpriteEditorPanel m_spriteEditor;
    std::string       m_pendingSpriteAtlas;
    PerformancePanel m_performancePanel;
    RenderingPanel   m_renderingPanel;
    // Input action map. Its constructor loads the persistence JSON,
    // so the panel already comes with the previous session's contents when opened.
    InputActionsPanel m_inputActionsPanel;

    // Load Scene progress overlay. Only reloadSceneFromJson activates it on
    // the asynchronous path; main.cpp's pump keeps updating it every frame.
    LoadingModal      m_loadingModal;
    // Non-owning asynchronous loader (lives in main.cpp). Filled in by
    // setAssetLoader before the loop. nullptr => Load Scene falls back to synchronous.
    AsyncAssetLoader* m_assetLoader = nullptr;
    // See setDroppedFilesProvider. Empty in headless tests.
    std::function<std::vector<DroppedFile>()> m_droppedFilesProvider;
    JobSystem* m_jobSystem = nullptr;

    // See setProjectSelector/setProject. Empty/nullptr in the headless tests: without a
    // selector the editor draws as usual, and without a project the path guards
    // behave the same as before the concept existed.
    std::function<bool()>  m_projectSelector;
    const ProjectContext*  m_project = nullptr;
    // Last project whose View menu settings were already applied. Different from
    // m_project => they have to be applied (once per project opening).
    const ProjectContext*  m_appliedProject = nullptr;

    // Render backend. Two values on purpose, because they need not
    // coincide: m_activeBackend is the one this PROCESS's device was created with, and
    // m_selectedBackend the one the project requests for the NEXT
    // startup. While they differ, the View menu shows the restart warning.
    RenderBackend m_activeBackend   = RenderBackend::Vulkan;
    RenderBackend m_selectedBackend = RenderBackend::Vulkan;

    // Backend of the Export Game dialog, as a combo index. It is INDEPENDENT
    // of the two above: the game is exported for the player's machine, which
    // need not use the same backend as this editor. Session setting,
    // not persisted in project.json: the destination is the package's game.cfg.
    int m_exportBackend = 0;

    // ImGui backend. The device is stored in initUi because shutdownUi
    // needs it to release the pool and by then there is no InitInfo.
    VkDescriptorPool m_uiDescPool = VK_NULL_HANDLE;
    VkDevice         m_uiDevice   = VK_NULL_HANDLE;
};

} // namespace DonTopo
