#pragma once
#include <cstdint>
#include <string>
#include <memory>
#include <vector>
#include "DonTopo/Editor/AnimatorGraphUndo.h"

namespace ax::NodeEditor { struct EditorContext; }
namespace IGFD { class FileDialog; }

namespace DonTopo {

struct EditorContext;
class GameObject;

// "Animator" window: node canvas of the state graph of the selected GameObject
// (node = state with its clip and its loop, link = transition).
//
// It is its own panel and not a block of Properties because the imgui-node-editor
// canvas needs its own zoom and pan: in the Properties column it would be
// unusable.
class AnimatorPanel {
public:
    AnimatorPanel();
    ~AnimatorPanel();
    AnimatorPanel(const AnimatorPanel&)            = delete;
    AnimatorPanel& operator=(const AnimatorPanel&) = delete;

    void draw(EditorContext& ctx);
    bool* GetOpenPtr() { return &m_open; }
    void open() { m_open = true; }

private:
    // Dumps AnimatorComponent::State::editorPos to the canvas. Only when the object
    // changes: doing it every frame would fight with the user's mouse and the nodes
    // could not be dragged.
    void syncPositionsFromComponent(GameObject* go);
    // Reverse path, every frame: the canvas is the source of truth for the positions
    // while the panel is open, and editorPos is what gets serialized.
    void syncPositionsToComponent(GameObject* go);
    void drawParameterList(EditorContext& ctx, GameObject* go);
    // Layers: the list (select, add, remove, reorder, rename) and, for the ones that
    // are not the base, weight, mode and mask.
    void drawLayerBar(EditorContext& ctx, GameObject* go);
    void drawLayerMaskPopup(GameObject* go);
    // IK constraints of the component (not of a layer): type, bone, target, pole,
    // weight and, for look-at, axis and maximum angle.
    void drawIkList(EditorContext& ctx, GameObject* go);
    // Property clips of the component: the ones that animate an object WITHOUT a
    // skeleton (transform, light, material).
    void drawPropertyClips(EditorContext& ctx, GameObject* go);
    void drawGraph(EditorContext& ctx, GameObject* go);
    void drawConditionsPopup(EditorContext& ctx, GameObject* go);
    // List of clips ("blend") or of float parameters ("by") of a state. It is opened
    // OUTSIDE the node, between ed::Suspend and ed::Resume: a BeginCombo inside the
    // node draws its list in canvas coordinates (with zoom and pan), so it came out
    // displaced and did not receive the clicks.
    void drawBlendPickPopup(GameObject* go);
    // List of FBX files that provide clips, with Add/Remove and inline rename.
    void drawAnimationSources(EditorContext& ctx, GameObject* go);
    // Drains the file dialog every frame, unconditionally, even if the panel is
    // closed or collapsed (that is why draw() calls it outside the if(m_open) and
    // the ImGui::Begin/End, not inside): if it were only drained with the window
    // visible, closing or collapsing the panel with the dialog open would leave
    // m_animSrcDlgOpen (and the internal IGFD state) stuck at true forever. Same
    // pattern as PropertiesPanel::draw + drawMeshDialog.
    void drawAnimationSourceDialog(EditorContext& ctx);
    // Imports path as a source of the selected GameObject, via a command (undo).
    void importAnimationSource(EditorContext& ctx, GameObject* go, const std::string& path);

    ax::NodeEditor::EditorContext* m_ctx = nullptr;
    bool m_open = false;   // starts closed: it is a specialized panel

    // Last GameObject whose positions were dumped to the canvas. When the selection
    // changes they have to be dumped again.
    GameObject* m_boundTo = nullptr;
    // Tooltip of the widget of a node that is under the cursor: it is noted down
    // while the node is drawn and painted after closing the canvas (see drawGraph).
    std::string m_tooltipNodo;
    // Layer whose graph is shown and edited, and the one the canvas had at the last
    // position dump: when the layer changes they are dumped again, as when the object
    // changes.
    // Width of the left column: the user drags it by the edge. It lives in the
    // session, not in project.json (that is where panel visibility goes).
    float m_anchoColumna  = 300.0f;
    int  m_layer          = 0;
    // Sub-machine being looked at from the inside; -1 is the root of the layer. It is
    // reset when the layer or the object changes: its index means nothing in
    // another graph.
    // Sub-machine being looked at from the inside, by **editorId** and not by
    // index: removeState reindexes the vector, so an index stored here
    // ends up pointing to another state as soon as any earlier node is deleted
    // (and the app would get out of the box on its own). It is the same reason why the
    // canvas identifies its nodes by editorId. -1 = root of the layer.
    int  m_nivelId        = -1;
    // The index that corresponds to it in the current layer, resolved every frame from
    // m_nivelId. -1 if the box no longer exists (it was deleted while inside).
    int  nivelActual(const AnimatorComponent& anim) const;
    int  m_boundLayer     = -1;
    int  m_renamingLayer  = -1;
    bool m_focusRename    = false;
    char m_layerNameBuf[64] = {};

    // Index of the transition whose conditions popup is open, -1 if none. Deferred to
    // the end of the frame: opening a popup in the middle of the canvas breaks the
    // node editor layout.
    int m_conditionsFor = -1;

    // Index of the state over which the node context menu was opened, -1 if none. A
    // plain member is used instead of ImGui::GetStateStorage() (the plan B that the
    // spec contemplated): with a single node popup active at a time the indirection
    // of the ImGui state storage is not needed, and a member is easier to reason
    // about and to test by eye.
    int m_nodeCtxTarget = -1;

    // Pending blend popup: the node button requests it and drawBlendPickPopup opens
    // it already in screen coordinates. By editorId, not by index: the state vector
    // can be reindexed between the click and the popup.
    bool m_blendPickRequested = false;
    int  m_blendPickEditorId  = -1;
    int  m_blendPickKind      = 0;       // 0 clip of a blend entry, 1 parameter ("by"), 2 speed multiplier, 3 Y parameter of the 2D blend.
    int  m_blendPickEntry     = -1;      // index into blendEntries for kind 0.
    // Graph undo: turns the live edits of this panel into one command per gesture
    // (see AnimatorGraphUndo.h).
    AnimatorGraphUndoTracker m_graphUndo;
    // History revision in the previous frame. If it changes (undo, redo or a push),
    // the nodes that an undo has reinserted are repositioned from the component: the
    // canvas did not know them.
    uint64_t m_lastUndoRevision = 0;

    char m_newParamName[64] = {};
    int  m_newParamType     = 0;   // index into ParamType: 0 bool, 1 trigger, 2 int, 3 float

    // Own instance and not shared with the PropertiesPanel dialogs: IGFD keeps state
    // per instance, and sharing it would make resizing one popup affect the other.
    std::unique_ptr<IGFD::FileDialog> m_animSrcDialog;
    bool m_animSrcDlgOpen = false;
    // Id of the target GameObject, captured when pressing "Add Animation FBX..."
    // (OpenDialog), NOT read from ctx.selected when draining: the dialog is not
    // modal, so the user can change the selection while choosing the file, and the
    // FBX has to go to whoever was selected when the dialog was opened. It is resolved
    // via Scene::findById instead of storing a raw GameObject* for the same reason
    // as the Undo commands: the object may have been deleted (or rebuilt) while the
    // dialog was open.
    uint64_t m_animSrcDlgTarget = 0;
    std::string m_animSrcError;      // last error, in red under the list
    // Clip whose name is being edited, "" if none.
    std::string m_renamingClip;
    // 256 and not 64: a Mixamo name (e.g. "mixamorig_Explosive_Superhero_Idle")
    // plus the " (N)" suffix that addAnimationSource adds on a collision already
    // comes close to 64 bytes, and a truncated snprintf here becomes a real rename
    // (not a rejection) as soon as the user presses Enter: it would silently lose
    // the last characters of the name. 256 makes truncation implausible without
    // adding a new rejection path in the UI.
    char m_renameBuf[256] = {};
};

} // namespace DonTopo
