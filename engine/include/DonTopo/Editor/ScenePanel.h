#pragma once
#include <memory>
#include <string>

namespace DonTopo {

class GameObject;
class Mesh;
struct EditorContext;

// Ventana "Scene" — árbol jerárquico de GameObjects (hover/click de
// selección, drag&drop de reorder, popup de rename, menú contextual de
// Create/Delete/Basic Shapes).
class ScenePanel {
public:
    void draw(EditorContext& ctx, GameObject* sceneRoot);
    bool* GetOpenPtr() { return &m_open; }

private:
    void drawNode(EditorContext& ctx, GameObject* node);
    void beginRename(GameObject* node);
    void createBasicShape(EditorContext& ctx, GameObject* parent, const std::string& name,
                           std::shared_ptr<Mesh> mesh);

    bool m_open = true;

    // Borrado/reorder diferidos al final del frame: el árbol se recorre con
    // recursión sobre std::vector<unique_ptr<GameObject>>, mutarlo en medio
    // de esa recursión invalidaría los iteradores de los for-range activos.
    GameObject* m_pendingDelete = nullptr;
    GameObject* m_pendingMoveSource = nullptr;
    GameObject* m_pendingMoveTarget = nullptr;

    // Rename — popup modal disparado por "Rename" (click derecho) o F2.
    GameObject* m_renameTarget = nullptr;
    char        m_renameBuffer[128] = {};
    bool        m_openRenamePopup = false;
};

} // namespace DonTopo
