#pragma once
#include <functional>
#include <string>
#include <filesystem>
#include <vector>
#include <glm/glm.hpp>

namespace DonTopo {

class GameObject;
class PhysicsManager;
class AudioManager;
class Renderer;
class EditorRenderer;
class Scene;
class ScriptManager;
class UndoManager;
class AsyncAssetLoader;
class ProjectContext;
class JobSystem;

// File dropped onto the editor window from outside the process (drag
// from Windows Explorer), with the screen position where it
// landed. screenX/screenY are ImGui screen coordinates (the same as
// ImGui::GetMousePos()/GetWindowPos()), not GLFW window coordinates.
struct DroppedFile {
    std::filesystem::path path;
    float screenX = 0.0f;
    float screenY = 0.0f;
};

// State shared between the editor panels, rebuilt every
// frame inside EditorUI::draw() and passed by reference to each
// Panel::draw(). `selected` is a real reference to EditorUI::m_selected:
// a panel that reassigns it (e.g. ScenePanel when clicking a node)
// propagates the change to the panels drawn later in the same frame
// (Viewport, Properties), just like EditorUI's single m_selected used to.
struct EditorContext {
    GameObject*& selected;
    bool&        isPlaying;

    PhysicsManager* physics       = nullptr;
    // The interface, not the Vulkan Renderer: the panels only use what
    // any backend can provide.
    EditorRenderer* renderer      = nullptr;
    AudioManager*   audio         = nullptr;
    Scene*          scene         = nullptr;
    ScriptManager*  scriptManager = nullptr;
    UndoManager*    undo          = nullptr;

    std::function<void(const std::string&)>   pushLog;
    // Same as pushLog but tagging the line with a module ("Renderer",
    // "Physics", ...), which the Log Console draws as a colored chip. It travels through
    // the same one-argument callback (the panel decodes the "[Module] "
    // prefix), so pushLog callers do not change.
    void logModule(const std::string& module, const std::string& message) const
    {
        if (pushLog)
            pushLog("[" + module + "] " + message);
    }
    // onDelete used to live here, and it served to release the subtree's GPU resources before
    // deleting it. It is gone: now Scene::setOnNodeRemoved notifies, which covers the
    // three callers of removeGameObject instead of just this panel.
    std::function<void(const glm::vec3&)>     onAxisSelected;
    // Opens path in the Script Editor (EditorUI::m_scriptEditor, outside the
    // original scope of PropertiesPanel; Task 5 added this callback
    // because drawScriptsSection/drawNewScriptPopup need to open the
    // .lua file after editing/creating a script, and ScriptEditorPanel is still
    // owned by EditorUI, not by any panel). Empty/unassigned
    // by default; only EditorUI::draw() fills it in.
    std::function<void(const std::filesystem::path&)> openScript;
    // Opens the Animator panel (EditorUI::m_animatorPanel, outside the scope of
    // PropertiesPanel; same case and same pattern as openScript). Empty by
    // default: only EditorUI::draw() fills it in.
    std::function<void()> openAnimator;
    // Opens the sprite editor on that image (EditorUI::m_spriteEditor,
    // outside the scope of PropertiesPanel; same pattern as openAnimator).
    std::function<void(const std::string&)> openSpriteEditor;
    // Someone has changed the sub-rects of an atlas. PropertiesPanel uses it to
    // drop its name cache: without this the combos keep showing the previous
    // list until the path changes. Empty by default.
    std::function<void()> onSpritesChanged;
    // Saves the project settings (project.json). Used by RenderingPanel:
    // its 41 controls are applied AND persisted, and each one's undo command
    // calls it again so that undoing leaves the file as it was. The
    // owner of project.json is still EditorUI. Empty by default.
    std::function<void()> saveSettings;
    // Opens the ambient/skybox window (EditorUI::m_environmentWindowOpen).
    // Same pattern as openAnimator: the window lives in EditorUI (it is tied to
    // its folder dialog and to applySkyboxFolder) and RenderingPanel only has
    // the button that opens it. Empty by default.
    std::function<void()> openEnvironment;

    // Asynchronous asset loader (lives in main.cpp, non-owning). Without it,
    // the FBX drop enqueues nothing (loadMeshForSelected is a no-op). Filled in by
    // EditorUI::draw() from EditorUI::m_assetLoader.
    AsyncAssetLoader* assetLoader = nullptr;

    // Open project (lives in main(), non-owning). Decides which paths a panel may
    // read or write: everything belonging to the user (scenes, scripts, assets)
    // goes through project->contains() before touching disk. nullptr in the headless
    // tests and in any path before the selector: without a project there is no
    // sandbox and the behavior is the usual one.
    const ProjectContext* project = nullptr;

    // true while the loading modal is active (Load Scene in flight). It vetoes
    // editing (gizmo, hierarchy reparent, asset drops) but NOT rendering:
    // the window keeps drawing frames. Editing sites check it with
    // `if (!ctx.editingLocked)`. Default false: outside Load Scene everything is edited
    // as usual (the headless tests leave it at its default).
    bool editingLocked = false;

    // Loads the scene from disk at path through the same route as the File menu's
    // Load Scene (JSON validation + reloadSceneFromJson + undo clear).
    // Same pattern as openScript/openAnimator: only
    // EditorUI::draw() fills it in, empty by default in the headless tests.
    std::function<void(const std::filesystem::path&)> requestLoadScene;
    // Saves the current scene and, if saving succeeds, loads thenLoad (empty
    // = load nothing afterwards). If the scene was never saved, it opens the same
    // Save Scene dialog as the File menu and chains the load to its confirmation;
    // if the user cancels it, nothing is loaded.
    std::function<void(const std::filesystem::path& thenLoad)> requestSaveScene;

    // Empties the queue of files dropped onto the window this frame (OS-level
    // drop via glfwSetDropCallback, not ImGui's internal drag&drop of
    // DT_ASSET_PATH payloads). It is consumed once: calling it twice in the
    // same frame returns empty the second time. Empty/unassigned in the headless
    // tests and at runtime; only EditorUI::draw() fills it in from
    // EditorUI::m_droppedFilesProvider (wiring in sandbox/main.cpp).
    std::function<std::vector<DroppedFile>()> takeDroppedFiles;

    // The engine's worker pool (lives in main.cpp, non-owning). Used by the
    // Content Browser thumbnails to decode images off the main thread.
    // Without it, there are no thumbnails and the grid behaves as usual.
    JobSystem* jobs = nullptr;
};

} // namespace DonTopo
