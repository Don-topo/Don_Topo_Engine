#pragma once
#include <memory>
#include <string>

namespace DonTopo {

class GameObject;
struct Mesh;
struct EditorContext;

// "Scene" window: hierarchical tree of GameObjects (selection hover/click,
// reorder drag&drop, rename popup, Create/Delete/Basic Shapes context
// menu).
class ScenePanel {
public:
    void draw(EditorContext& ctx, GameObject* sceneRoot);
    bool* GetOpenPtr() { return &m_open; }
    // true if the most recent draw() deleted the GameObject that was
    // selected (so EditorUI invalidates the Properties edit caches
    // ONLY in that case, not on any deselection).
    bool selectionWasDeletedThisFrame() const { return m_selectionDeletedThisFrame; }

private:
    void drawNode(EditorContext& ctx, GameObject* node);
    void beginRename(GameObject* node);
    void createBasicShape(EditorContext& ctx, GameObject* parent, const std::string& name,
                           std::shared_ptr<Mesh> mesh);
    // Creates a GameObject with a CameraComponent in one go, going through the Undo
    // stack just like createBasicShape. The caller checks that there is no
    // camera yet (Scene::findCamera) before offering the action.
    void createCamera(EditorContext& ctx, GameObject* parent);

    bool m_open = true;

    // Delete/reorder deferred to the end of the frame: the tree is walked with
    // recursion over std::vector<unique_ptr<GameObject>>, and mutating it in the middle
    // of that recursion would invalidate the iterators of the active range-fors.
    GameObject* m_pendingDelete = nullptr;
    GameObject* m_pendingMoveSource = nullptr;
    GameObject* m_pendingMoveTarget = nullptr;

    // true if the most recent draw() deleted the selected GameObject.
    bool m_selectionDeletedThisFrame = false;

    // Rename: modal popup triggered by "Rename" (right click) or F2.
    GameObject* m_renameTarget = nullptr;
    char        m_renameBuffer[128] = {};
    bool        m_openRenamePopup = false;
};

} // namespace DonTopo
