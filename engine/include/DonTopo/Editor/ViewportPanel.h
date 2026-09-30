#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <cstdint>
#include <string>

namespace DonTopo {

class GameObject;
class Camera;
class Scene;
struct EditorContext;

// What the viewport gizmo manipulates on the selected object. One of the three at
// a time, as in Unity: all three sets of handles at once would be impossible to
// click.
//
// Own enum and not `ImGuizmo::OPERATION` so as not to pull ImGuizmo.h (and with it
// imgui.h) into a header that the toolbar and the tests include. The translation
// to the library enums lives in the .cpp, in a single place.
enum class GizmoMode { Translate, Rotate, Scale };

// Label of the channel that each mode edits, exactly as the Log Console and the
// Properties panel write it: "Position", "Rotation", "Scale". It is what makes the
// gizmo log line indistinguishable from the one Properties emits when editing the
// same value by hand.
const char* gizmoChannelLabel(GizmoMode mode);

// The three numbers the log shows after a drag, taken from the resulting LOCAL
// matrix: the position in world units, the rotation in DEGREES (not radians: the
// inspector shows degrees) or the scale as a factor.
//
// It sits next to gizmoChannelLabel and not inside it because it is what can
// really go wrong: a copy-paste that leaves Rotate mode reporting the position
// compiles just as well and is only noticed by reading the log.
glm::vec3 gizmoLoggedValue(GizmoMode mode, const glm::mat4& localTransform);

// What is asked of ImGuizmo for each mode: `outOperation` is an
// `ImGuizmo::OPERATION` and `outSpace` an `ImGuizmo::MODE`, both as int so as not
// to drag ImGuizmo.h (and with it imgui.h) into this header. Whoever uses them
// casts them back; whoever tests them compares them against the real enums.
//
// The space is NOT the same in the three, and that is a decision, not an oversight:
//   - Translate -> WORLD: dragging "X" moves along the world X.
//   - Rotate    -> LOCAL: the rings stick to the object's axes; in
//     WORLD they are drawn aligned to the world and, with a tilted object, they do
//     not match anything that can be seen.
//   - Scale     -> LOCAL is mandatory. ImGuizmo does
//     `ComputeContext(..., (operation & SCALE) ? LOCAL : mode)` and discards what
//     it is given; LOCAL is passed so the call does not lie.
//
// It is out here because it is the ONLY part of the per-mode dispatch that can be
// tested without a GUI: sending Rotate to TRANSLATE compiles, runs and is only
// visible on screen.
void gizmoImGuizmoEnums(GizmoMode mode, int& outOperation, int& outSpace);

// LOCAL matrix that leaves the object exactly at newWorld, given the
// worldTransform of its parent (identity if it has no parent).
//
// ImGuizmo manipulates a WORLD matrix, but what the scene serializes, what the
// Properties panel edits and what the undo stacks is `localTransform`. Writing the
// world into the local would work ONLY on roots: a child would jump by having the
// parent's transform applied to it a second time.
//
// It lives outside the class (and in the header) because it is the only piece of
// the manipulation that can be tested without a GUI: mouse interaction cannot be
// simulated headless, matrix arithmetic can.
glm::mat4 localFromWorld(const glm::mat4& parentWorld, const glm::mat4& newWorld);

// Writes t as the localTransform of the object with that id, propagates the world
// to its children and teleports its collider if it has one. No-op if the id no
// longer exists.
//
// It is the body of the manipulator's undo command, out here for the same reason
// as localFromWorld: it is the part that can be asserted without a GUI. The
// object is looked up by ID and not by pointer on purpose: between stacking the
// command and undoing it a deletion and a scene load can happen, and the rebuilt
// GameObject keeps the id but not the address.
void applyLocalTransform(Scene& scene, uint64_t id, const glm::mat4& t);

// "Viewport" window: embedded 3D render (Renderer texture) + axes/collider
// wireframe gizmo over the active selection.
class ViewportPanel {
public:
    // viewportTexture is an opaque handle of the active backend (VkDescriptorSet
    // with Vulkan, GPU descriptor with DirectX 12): it is only forwarded to
    // ImGui::Image, which treats it the same in both cases.
    void draw(EditorContext& ctx, uint64_t viewportTexture, const glm::mat4& cameraView);
    // Centers the camera on ctx.selected (no-op if there is no selection). Used
    // by the "F" keyboard shortcut in main.cpp via EditorUI::focusSelected.
    void focusSelected(EditorContext& ctx, Camera& camera);
    bool isHovered() const { return m_hovered; }
    bool* GetOpenPtr() { return &m_open; }
    // Image area of the panel in pixels, the one from the last draw(). The Renderer
    // renders EXACTLY at this size: if it rendered at the window size, ImGui would
    // rescale the image when drawing it and that bilinear filtering would eat the
    // stair-stepping (and with it the difference between anti-aliasing modes), and
    // would also distort the scene when the panel aspect does not match the window's.
    // (0,0) while the panel is closed.
    uint32_t contentWidth()  const { return m_contentWidth; }
    uint32_t contentHeight() const { return m_contentHeight; }

    // Top-left corner of the IMAGE in screen coordinates, and whether the mouse is
    // over it (not over the window: a popup on top does not count). Since the image
    // is drawn 1:1 with the render, subtracting this corner from the mouse directly
    // gives the UI canvas pixel.
    glm::vec2 imagePos()     const { return m_imagePos; }
    bool      imageHovered() const { return m_imageHovered; }

    // Gizmo mode. It is written by the three toolbar buttons and the W/E/R shortcuts,
    // both in EditorUI; the state lives here because it belongs to this panel (whoever
    // reads it is the manipulator) and so no extra field is needed in EditorContext
    // nor do the panel and the toolbar have to pass the value to each other every
    // frame.
    GizmoMode gizmoMode() const     { return m_gizmoMode; }
    void setGizmoMode(GizmoMode m)  { m_gizmoMode = m; }

private:
    void drawSelectionGizmo(EditorContext& ctx);
    // Transform manipulator (ImGuizmo) on the selected object, in the mode that
    // m_gizmoMode says. It is the only thing in this panel that EDITS the scene:
    // drawSelectionGizmo and the other thirteen only paint.
    //
    // imagePos/imageSize are the rect of the IMAGE, not of the window: the
    // manipulator has to land on the same pixel as the object, and the window carries
    // the title bar and the dock borders on top.
    void drawTransformGizmo(EditorContext& ctx, const glm::mat4& cameraView,
                             const glm::vec2& imagePos, const glm::vec2& imageSize);
    // Wireframe of the scene camera frustum, always visible in edit mode (not only
    // when selecting it). Only the frustum: the transform axes are already drawn by
    // drawSelectionGizmo when selecting any object, and repeating them here would
    // give two overlapping sets of axes of different length.
    void drawCameraGizmo(EditorContext& ctx);
    // Gizmo of ALL the lights in the scene (not only the selected one), in edit mode
    // and in Play. It lives in the editor on purpose: that guarantees it does not show
    // up in the exported game, which does not compile this panel.
    void drawLightGizmos(EditorContext& ctx);
    // Rectangle of the USABLE AREA of the selected Canvas, in 2D over the viewport
    // image (the UI is screen space, not world: it does not go through Gizmos). The
    // rect COMES from the live canvas (uiOrigin/uiScale/referenceSize), it is not
    // recomputed here: safe area and aspect ratio are already applied.
    void drawCanvasGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                          const glm::vec2& imageSize);
    // Rect + X/Y axes of the selected Button, in 2D over the image just like the
    // Canvas gizmo. The rect comes from the LIVE canvas node (anchors and scale
    // already applied), not from the component fields.
    void drawButtonGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                          const glm::vec2& imageSize);
    // The same for the selected Text. It is another canvas node (name with a
    // different prefix), so a GameObject with Button and Text paints both.
    void drawTextGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                        const glm::vec2& imageSize);
    // And the same for the selected ProgressBar. The rect is the BACKGROUND one (the
    // root node of the bar), not the fill one: the fill shrinks with the value and the
    // gizmo measures the widget, not the data.
    void drawProgressBarGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                               const glm::vec2& imageSize);
    // And the one for the Layout container, which is the ONLY one that cannot be
    // clicked in the viewport (it is not a raycastTarget: a group that paints nothing
    // must not eat the clicks). It is selected from the Hierarchy, and this gizmo is
    // the only thing that shows where its rect is.
    void drawLayoutGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                          const glm::vec2& imageSize);
    // And those of the rest of the UI widgets. Same criterion as the ones above: the
    // rect comes from the LIVE node, not from the component fields, so it already has
    // the anchors, the canvas scale and the layout applied.
    //
    // The ScrollView one measures the VIEWPORT (the node that clips) and not the
    // content: the content moves and is bigger than the view, so its rect does not
    // say where the widget is.
    void drawInputFieldGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                       const glm::vec2& imageSize);
    void drawDropdownGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                       const glm::vec2& imageSize);
    void drawScrollViewGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                       const glm::vec2& imageSize);
    void drawSliderGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                       const glm::vec2& imageSize);
    void drawCheckboxGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                       const glm::vec2& imageSize);
    void drawToggleGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                       const glm::vec2& imageSize);
    void drawScrollbarGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                       const glm::vec2& imageSize);
    void drawPanelGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                         const glm::vec2& imageSize);
    void drawImageGizmo(EditorContext& ctx, const glm::vec2& imagePos,
                         const glm::vec2& imageSize);
    // Click selection of a UI widget: 2D hit test of the canvas, which takes
    // precedence over the 3D raycast because the UI is drawn ON TOP of the scene.
    // mousePx is in pixels relative to the image corner, same as in
    // pickObject. nullptr if the click does not land on any widget.
    GameObject* pickUiObject(EditorContext& ctx, const glm::vec2& mousePx,
                              const glm::vec2& imageSize) const;
    // Axis length proportional to the local bbox of the mesh of node (half of
    // the longest axis); if node has no mesh (or the mesh has no vertices), a fixed
    // fallback value.
    float selectionAxisScale(GameObject* node) const;
    // CPU ray picking: unprojects mousePx (pixels RELATIVE to the top-left corner of
    // the viewport image, not of the ImGui window) with the camera of the frame (the
    // editor fly camera or the scene one in Play) and returns the object with a mesh
    // whose bounding sphere the ray cuts closest to the camera. nullptr if it cuts
    // none.
    GameObject* pickObject(EditorContext& ctx, const glm::mat4& cameraView,
                           const glm::vec2& mousePx, const glm::vec2& imageSize) const;

    bool m_open = true;
    // The camera `view` with which the last frame was drawn. draw() fills it in right
    // on entry, just as it already fills m_imagePos and m_contentWidth.
    //
    // It exists because the widget gizmos of a WORLD canvas have to PROJECT their
    // rect, and projecting needs the view twice over: for the camera matrix and for
    // the canvas billboard. The thirteen drawXGizmo read it from here and pass it to
    // drawUiNodeGizmo. A parameter in those thirteen signatures would be the same
    // with more noise; what does NOT work is a file-level static, which would leave
    // the data outside the panel's reach.
    glm::mat4 m_cameraView{1.0f};

    // State of the translation manipulator drag. It exists so that a whole drag
    // leaves a SINGLE command in the undo, not one per frame: `before` is captured on
    // the entry edge and the command is stacked on the exit edge, just as
    // PropertiesPanel does with IsItemActivated / IsItemDeactivatedAfterEdit in its
    // DragFloat.
    GizmoMode m_gizmoMode  = GizmoMode::Translate;
    // The mode with which the drag in progress STARTED, for the log line. The
    // shortcuts keep responding while dragging.
    GizmoMode m_gizmoModeAtGrab = GizmoMode::Translate;
    bool      m_gizmoUsing = false;
    glm::mat4 m_gizmoBefore{1.0f};
    // And the object is remembered by ID, not by pointer: between the entry edge and
    // the exit edge a scene load and a deletion can happen, and a stored GameObject*
    // would be left dangling. Same criterion as the PropertyCommand lambda, which
    // also resolves by findById.
    uint64_t    m_gizmoId = 0;
    std::string m_gizmoName;

    bool m_hovered = false;
    bool m_imageHovered = false;
    glm::vec2 m_imagePos{0.0f};
    uint32_t m_contentWidth  = 0;
    uint32_t m_contentHeight = 0;
};

} // namespace DonTopo
