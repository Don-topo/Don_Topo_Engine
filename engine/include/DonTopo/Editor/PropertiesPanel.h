#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>
#include "DonTopo/Editor/UndoManager.h" // BoxColliderState, SphereColliderState, CapsuleColliderState, PlaneColliderState
#include "DonTopo/Editor/DeferredSlider.h"
#include "DonTopo/Core/CameraComponent.h"
#include "DonTopo/Core/GameObject.h" // uiComponentsAvailable needs the complete type

namespace IGFD { class FileDialog; }

namespace DonTopo {

class GameObject;
class BoxCollider;
class SphereCollider;
class CapsuleCollider;
class PlaneCollider;
class Rigidbody;
struct EditorContext;

// "Properties" window: transform, colliders (Box/Sphere/Capsule/Plane),
// Mesh, Audio Clip, Scripts and the "Add" button with its "New Script" popup.
class PropertiesPanel {
public:
    PropertiesPanel();
    ~PropertiesPanel();
    PropertiesPanel(const PropertiesPanel&) = delete;
    PropertiesPanel& operator=(const PropertiesPanel&) = delete;

    void draw(EditorContext& ctx);

    // Drops the sprite name cache. Whoever touches a sidecar from outside (the sprite
    // editor) calls it: without this the combos keep showing the list the section
    // was opened with.
    void invalidateSpriteNames() { m_spriteNamesValid = false; }
    bool* GetOpenPtr() { return &m_open; }
    // Forgets EVERYTHING the sections have cached. Two callers:
    //   - ScenePanel deleted the selected node: the pointers point to components
    //     that are already freed and cannot survive the frame.
    //   - Undo/Redo: they mutate the components in place, so the section has to
    //     read them again or it keeps showing the undone value.
    void invalidateCaches();

    // Was the load `job` that has just landed on `targetId` requested by the user
    // from the Mesh section? If so, it forgets it and returns true. EditorUI::onAssetsLoaded
    // calls it to decide whether to stack the "add Mesh" undo: scene loading uses
    // the same requestMesh and is not an edit. By object AND job, not only by object:
    // a reload of the same scene keeps the ids, so a cancelled request that stayed
    // here would sneak that object's scene load in as if it were the user's.
    bool consumeUserMeshJob(uint64_t targetId, uint64_t job);

    // A GameObject only offers the UI components if it ALREADY has a Canvas: the
    // Canvas is the root they hang from. It is the only source of truth for the
    // gate (the "Add" popup uses it), and it is here and not inside the ImGui code so
    // that it can be tested without a GUI.
    // The GameObject's own Canvas or the one of ANY ancestor counts: a button
    // normally hangs from the Canvas, it is not the Canvas.
    static bool uiComponentsAvailable(const GameObject* go)
    {
        for (const GameObject* n = go; n; n = n->parent)
            if (n->hasCanvas()) return true;
        return false;
    }

    // What each asset box of the Button accepts: the font ones are those FreeType
    // opens (UiFont::loadFromFile) and the atlas ones those stb_image reads
    // (UiTextureAtlas::loadFromFile). Here and not inside the ImGui code so they can
    // be tested without a GUI, like uiComponentsAvailable. The comparison is in
    // lowercase: a ".PNG" can arrive from the content browser.
    static bool isUiFontPath(const std::string& path)
    {
        static const char* const kExts[] = { ".ttf", ".otf", ".ttc" };
        return hasExtension(path, kExts, sizeof(kExts) / sizeof(kExts[0]));
    }

    static bool isUiAtlasPath(const std::string& path)
    {
        static const char* const kExts[] = { ".png", ".jpg", ".jpeg", ".bmp", ".tga" };
        return hasExtension(path, kExts, sizeof(kExts) / sizeof(kExts[0]));
    }

private:
    static bool hasExtension(const std::string& path, const char* const* exts, size_t count)
    {
        const size_t dot = path.find_last_of('.');
        if (dot == std::string::npos) return false;
        // A dot that comes BEFORE the last separator belongs to a directory
        // ("C:/my.stuff/font"), it is not an extension.
        const size_t sep = path.find_last_of("/\\");
        if (sep != std::string::npos && dot < sep) return false;
        std::string ext = path.substr(dot);
        for (char& c : ext)
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        for (size_t i = 0; i < count; i++)
            if (ext == exts[i]) return true;
        return false;
    }

    // Asset box of the UI components, modeled on the Mesh one: "Browse..." button
    // and below it the 40 px drop zone with its message. The hand-editable path is
    // drawn by the caller right BEFORE calling here: each section has its own
    // inputText (with its typed accessor, its undo and its tooltip) and putting it
    // in the helper would force duplicating the label (the one of the Text above
    // and the one ImGui draws to the right of the field).
    //
    // idSuffix identifies the box: it feeds the id of the button and of the child,
    // which have to be unique within the panel. onBrowse opens the file dialog of the
    // place (each with its own instance, owner and flag) and onDrop applies the
    // dropped path; the extension veto still lives in the set*Path functions, which
    // is where both origins go through, not here.
    void drawAssetDropBox(EditorContext& ctx, const char* idSuffix, const char* hint,
                          const std::function<void()>& onBrowse,
                          const std::function<void(const std::string&)>& onDrop);

    void drawBoxColliderSection(EditorContext& ctx);
    void drawSphereColliderSection(EditorContext& ctx);
    void drawCapsuleColliderSection(EditorContext& ctx);
    void drawPlaneColliderSection(EditorContext& ctx);
    void drawRigidbodySection(EditorContext& ctx);
    void drawCameraSection(EditorContext& ctx);
    void drawAnimatorSection(EditorContext& ctx);
    void drawMeshSection(EditorContext& ctx);
    // The three textures of each material of the mesh. It goes INSIDE the Mesh
    // section, without its own Add-gate: it is not a new component, it is part of
    // the one that is already set.
    void drawTexturesSection(EditorContext& ctx);
    // Live push of the factors while a slider is dragged: it goes to the GPU and NOT
    // to the Material nor to the undo stack (see the comment in the .cpp).
    void previewMaterialFactors(EditorContext& ctx, uint64_t ownerId,
                                float metallic, float roughness);
    // empty path = Clear. A single place from which the six calls come out
    // (three slots x drop and browse) and the only one that stacks the command.
    // ownerId instead of reading ctx.selected: the result of the Browse dialog
    // arrives several frames after opening it, and no dialog of this panel is modal
    // (the selection may have changed in the meantime), so it resolves the
    // GameObject by id, same pattern as setButtonAssetPath.
    void assignMaterialTexture(EditorContext& ctx, uint64_t ownerId, int materialIndex,
                                MaterialTextureSlot slot, const std::string& path);
    // Links (or unlinks, with an empty path) a .mat to a material slot.
    // Same pattern as assignMaterialTexture: it resolves by id, validates index and
    // extension, and stacks a MaterialAssetCommand.
    void assignMaterialAsset(EditorContext& ctx, uint64_t ownerId, int materialIndex,
                             const std::string& path);
    // Screen Space Reflections of the object. It is not a component and does not go
    // through "Add": they are two fields of the GameObject (like the transform), so
    // the section appears on any object with a mesh.
    void drawSsrSection(EditorContext& ctx);
    // Reflection Probe. It IS a component and it DOES go through "Add": the section
    // is hidden until the user adds it, just like the colliders.
    void drawReflectionProbeSection(EditorContext& ctx);
    // Light. Also a component and also behind "Add": the section does not exist
    // until the user adds it, just like the colliders.
    void drawLightSection(EditorContext& ctx);
    void drawMeshDialog(EditorContext& ctx);
    void drawAudioClipSection(EditorContext& ctx);
    // Audio Listener: minimal section (only Enabled and remove). At most one per
    // scene; the uniqueness gate is in the "Add" popup, against
    // Scene::findAudioListener.
    void drawAudioListenerSection(EditorContext& ctx);
    void drawReverbZoneSection(EditorContext& ctx);
    // Canvas: root of the 2D UI. Section behind "Add" like the colliders, with the
    // 10 resolution fields that UiCanvas resolves.
    void drawCanvasSection(EditorContext& ctx);
    void drawButtonSection(EditorContext& ctx);
    // Drains the file dialogs of the Button paths. Outside the section and not
    // conditioned on the selection, just like drawMeshDialog.
    void drawButtonPathDialogs(EditorContext& ctx);
    // Writes an asset path of the Button (font or atlas), resolving the
    // GameObject by id, validating the extension and leaving the change on the undo
    // stack. It is the single point that the drop, the file dialog and any other
    // future origin go through.
    void setButtonAssetPath(EditorContext& ctx, uint64_t ownerId, bool isFont,
                             const std::string& path);
    // Sprite names of the atlas of a path, so they can be PICKED instead of typed
    // blindly. They are queried from the renderer (which caches the atlas by path)
    // and stored here: without this cache, a path that does not exist would be
    // tried again on every frame the section is visible.
    //
    // The list refreshes by itself when the path changes. Whoever touches the sidecar
    // from outside (the sprite editor) has to call invalidateSpriteNames().
    const std::vector<std::string>& spriteNamesFor(EditorContext& ctx, const std::string& atlasPath);

    std::string              m_spriteNamesPath;      // path the list came from
    std::vector<std::string> m_spriteNames;
    bool                     m_spriteNamesValid = false;
    // Text: 2D UI label. Section behind "Add" like the Button, with the same rect and
    // ALL the Text fields (outline, shadow, wrap and overflow).
    void drawTextSection(EditorContext& ctx);
    // Drains the file dialog of the Text font. Outside the section and not
    // conditioned on the selection, for the same reason as drawButtonPathDialogs.
    void drawTextPathDialog(EditorContext& ctx);
    // Writes the Text font path, resolving the GameObject by id, validating the
    // extension and leaving the change on the undo stack. Single point that the
    // drop and the file dialog go through.
    void setTextFontPath(EditorContext& ctx, uint64_t ownerId, const std::string& path);
    // ProgressBar: progress bar of the 2D UI. Section behind "Add" like the Button
    // and the Text, with the same rect, the value and its range, the two colors and
    // the two sprites (from the SAME atlas).
    void drawProgressBarSection(EditorContext& ctx);
    // Drains the file dialog of the ProgressBar atlas. Outside the section and not
    // conditioned on the selection, for the same reason as the Button ones.
    void drawProgressBarPathDialog(EditorContext& ctx);
    // Writes ONE of the three image paths of the ProgressBar (field: 0 atlas,
    // 1 background, 2 fill), resolving the GameObject by id, validating the
    // extension and leaving the change on the undo stack. Single point that the
    // drop and the file dialog of the three boxes go through.
    void setProgressBarImagePath(EditorContext& ctx, uint64_t ownerId, int field,
                                  const std::string& path);
    // Layout: the auto-layout of the 2D UI. Section behind "Add" like the other three
    // UI components, with the container rect, the mode and its parameters
    // (padding, spacing, cell, columns), the fitters and ignoreLayout.
    void drawLayoutSection(EditorContext& ctx);
    // Panel: the background rectangle of the 2D UI. Section behind "Add" like the
    // others, with the rect, the color, raycastTarget and the atlas/sprite pair.
    void drawPanelSection(EditorContext& ctx);
    // Drains the file dialog of the Panel atlas. Outside the section and for the
    // same reason as the ProgressBar one: if it is not always drained, changing the
    // selection with the dialog open leaves the flag stuck.
    void drawPanelPathDialog(EditorContext& ctx);
    // Writes the Panel atlas path (from the dialog or from a drop), with the
    // extension veto in a single place.
    void setPanelAtlasPath(EditorContext& ctx, uint64_t ownerId, const std::string& path);

    // Image: the 2D UI sprite with its four modes. Same pattern as the Panel
    // plus the widget's own block (borders, tiles and Filled).
    void drawImageSection(EditorContext& ctx);
    void drawImagePathDialog(EditorContext& ctx);
    void setImageAtlasPath(EditorContext& ctx, uint64_t ownerId, const std::string& path);

    // Slider: INTERACTIVE widget of the 2D UI. Section behind "Add" like the rest.
    // InputField: widget of the 2D UI. Section behind "Add" like the rest.
    void drawInputFieldSection(EditorContext& ctx);
    // Drains the file dialogs of the InputField. Outside the section for the same
    // reason as the others: if they are not always drained, changing the selection
    // with one open leaves the flag stuck.
    void drawInputFieldPathDialog(EditorContext& ctx);
    void setInputFieldAtlasPath(EditorContext& ctx, uint64_t ownerId, const std::string& path);
    void setInputFieldFontPath(EditorContext& ctx, uint64_t ownerId, const std::string& path);
    // Dropdown: widget of the 2D UI. Section behind "Add" like the rest.
    void drawDropdownSection(EditorContext& ctx);
    // Drains the file dialogs of the Dropdown. Outside the section for the same
    // reason as the others: if they are not always drained, changing the selection
    // with one open leaves the flag stuck.
    void drawDropdownPathDialog(EditorContext& ctx);
    void setDropdownAtlasPath(EditorContext& ctx, uint64_t ownerId, const std::string& path);
    void setDropdownFontPath(EditorContext& ctx, uint64_t ownerId, const std::string& path);
    // ScrollView: widget of the 2D UI. Section behind "Add" like the rest.
    void drawScrollViewSection(EditorContext& ctx);
    // Drains the file dialogs of the ScrollView. Outside the section for the same
    // reason as the others: if they are not always drained, changing the selection
    // with one open leaves the flag stuck.
    void drawScrollViewPathDialog(EditorContext& ctx);
    void setScrollViewAtlasPath(EditorContext& ctx, uint64_t ownerId, const std::string& path);

    void drawSliderSection(EditorContext& ctx);
    // Drains the file dialog of the Slider atlas, outside the section for the same
    // reason as the others: if it is not always drained, changing the selection with
    // the dialog open leaves the flag stuck.
    void drawSliderPathDialog(EditorContext& ctx);
    void setSliderAtlasPath(EditorContext& ctx, uint64_t ownerId, const std::string& path);
    // Checkbox: INTERACTIVE widget of the 2D UI. Section behind "Add" like the rest.
    void drawCheckboxSection(EditorContext& ctx);
    // Drains the file dialog of the Checkbox atlas, outside the section for the same
    // reason as the others: if it is not always drained, changing the selection with
    // the dialog open leaves the flag stuck.
    void drawCheckboxPathDialog(EditorContext& ctx);
    void setCheckboxAtlasPath(EditorContext& ctx, uint64_t ownerId, const std::string& path);
    // Toggle: INTERACTIVE widget of the 2D UI. Section behind "Add" like the rest.
    void drawToggleSection(EditorContext& ctx);
    // Drains the file dialog of the Toggle atlas, outside the section for the same
    // reason as the others: if it is not always drained, changing the selection with
    // the dialog open leaves the flag stuck.
    void drawTogglePathDialog(EditorContext& ctx);
    void setToggleAtlasPath(EditorContext& ctx, uint64_t ownerId, const std::string& path);
    // Scrollbar: INTERACTIVE widget of the 2D UI. Section behind "Add" like the rest.
    void drawScrollbarSection(EditorContext& ctx);
    // Drains the file dialog of the Scrollbar atlas, outside the section for the same
    // reason as the others: if it is not always drained, changing the selection with
    // the dialog open leaves the flag stuck.
    void drawScrollbarPathDialog(EditorContext& ctx);
    void setScrollbarAtlasPath(EditorContext& ctx, uint64_t ownerId, const std::string& path);

    void drawAudioClipDialog(EditorContext& ctx);
    void drawScriptsSection(EditorContext& ctx);
    void drawAddComponentButton(EditorContext& ctx);
    void drawNewScriptPopup(EditorContext& ctx);
    // ownerId, not ctx.selected: the Browse dialog is not modal and is drained
    // several frames after being opened, with the selection already changed. Same
    // pattern as assignMaterialTexture and setScrollbarAtlasPath.
    void loadMeshForSelected(EditorContext& ctx, uint64_t ownerId, const std::string& path);
    void loadAudioClipForSelected(EditorContext& ctx, const std::string& path);

    bool m_open = true;

    // WHO owns what each section has cached. Each section re-synchronizes its edit
    // fields when this stops matching the component it is drawing.
    //
    // ALL of them go together in a struct on purpose: invalidateCaches() clears them
    // in one statement (`m_caches = {}`), so a new section is covered by the mere fact
    // of declaring its pointer here. Enumerating them by hand, this function fell
    // short FOUR times (sphere, capsule, plane and rigidbody), and the symptom is one
    // of those that do not announce themselves: Undo changes the component, the
    // section keeps showing the old value, and the next drag of another field
    // reapplies it and resurrects what had just been undone.
    struct EditCaches
    {
        GameObject*      props     = nullptr;
        BoxCollider*     box       = nullptr;
        SphereCollider*  sphere    = nullptr;
        CapsuleCollider* capsule   = nullptr;
        PlaneCollider*   plane     = nullptr;
        const void*      rigidbody = nullptr;
        const void*      camera    = nullptr;
    };
    EditCaches m_caches;

    // Properties: edit cache of the selected node (persists between
    // frames so that DragFloat can accumulate the drag delta; it is only
    // re-synchronized with localTransform when the selection changes).
    glm::vec3   m_editPosition{0.0f};
    glm::vec3   m_editRotationDeg{0.0f};
    glm::vec3   m_editScale{1.0f};
    // true if in the previous frame the user had the mouse pressed on some
    // Position/Rotation/Scale DragFloat (prevents the live refresh of a dynamic
    // BoxCollider from fighting with the drag, and delimits the edit session
    // for the Undo snapshot below).
    bool        m_transformDragActive = false;
    // Snapshot of localTransform taken when a Position/Rotation/Scale drag starts
    // (first IsItemActivated of the session): the "before" of the
    // PropertyCommand<glm::mat4> that is pushed on confirm (commit).
    glm::mat4   m_transformBeforeEdit{1.0f};
    // The localTransform the three m_edit* above came from. It exists to detect
    // that someone OUTSIDE moved the object without changing the selection (the
    // viewport gizmo, a Lua script, a Ctrl+Z) and decompose again.
    //
    // Without this the fields stayed frozen at the value from when it was selected,
    // and it was worse than cosmetic: the next touch on any DragFloat recomposed the
    // matrix from that stale cache and ERASED the movement. It goes here, in the
    // owner of the cache, and not as a notice that every place that moves an object
    // has to remember to send.
    glm::mat4   m_transformCached{1.0f};

    // Reflection Probe: Radius/Intensity drag. Same pattern as SSR: the
    // "before" is taken in IsItemActivated and the owner id prevents applying a
    // foreign "before" if the drag was interrupted without a commit.
    bool     m_probeDragActive   = false;
    uint64_t m_probeDragOwnerId  = 0;
    float    m_probeDragBefore   = 0.0f;
    // Which of the two sliders is being dragged (the "before" is a single float).
    bool     m_probeDragIsRadius = false;

    // Light: drag of the sliders (intensity/range/angles/area size).
    // Same pattern as the probe, but with the dragged field identified by its
    // label: there are six sliders and a bool is not enough.
    bool        m_lightDragActive  = false;
    uint64_t    m_lightDragOwnerId = 0;
    float       m_lightDragBefore  = 0.0f;
    const char* m_lightDragField   = nullptr;
    // The color "before" does not fit in the float above: ColorEdit3 opens a
    // popup and the commit arrives frames after touching it.
    glm::vec3   m_lightColorBefore {1.0f};

    // Drag session of the Canvas fields, same dance as the light: the BEFORE
    // value is frozen in IsItemActivated and committed whole in
    // IsItemDeactivatedAfterEdit, so a drag is ONE undo step and not
    // hundreds. The field is identified by its label (a bool is not enough for 9).
    uint64_t    m_canvasDragOwnerId  = 0;
    const char* m_canvasDragField    = nullptr;
    float       m_canvasDragBefore   = 0.0f;
    glm::vec2   m_canvasDragBefore2 {0.0f};

    // The same for the Button fields. Four "before" values because the component
    // has the four field families (float, vec2, color and text) and each
    // one commits its own PropertyCommand<T>.
    uint64_t    m_buttonDragOwnerId = 0;
    const char* m_buttonDragField   = nullptr;
    float       m_buttonDragBefore  = 0.0f;
    glm::vec2   m_buttonDragBefore2 {0.0f};
    glm::vec4   m_buttonDragBefore4 {1.0f};
    std::string m_buttonDragBeforeStr;

    // And the same for the Text fields: their own and not shared with the
    // Button because both components can be on the SAME GameObject, and
    // a shared "before" would mix the two drags.
    uint64_t    m_textDragOwnerId = 0;
    const char* m_textDragField   = nullptr;
    float       m_textDragBefore  = 0.0f;
    glm::vec2   m_textDragBefore2 {0.0f};
    glm::vec4   m_textDragBefore4 {1.0f};
    std::string m_textDragBeforeStr;

    // And the same for the ProgressBar fields: their own and not shared with
    // the Button nor the Text, because the THREE components can be on the
    // SAME GameObject and a shared "before" would mix the drags.
    uint64_t    m_barDragOwnerId = 0;
    const char* m_barDragField   = nullptr;
    float       m_barDragBefore  = 0.0f;
    glm::vec2   m_barDragBefore2 {0.0f};
    glm::vec4   m_barDragBefore4 {1.0f};
    std::string m_barDragBeforeStr;

    // And the same for the Layout fields, for the same reason: the container
    // can coexist with the other three components on the same GameObject.
    uint64_t    m_layoutDragOwnerId = 0;
    const char* m_layoutDragField   = nullptr;
    float       m_layoutDragBefore  = 0.0f;
    glm::vec2   m_layoutDragBefore2 {0.0f};

    // And the same for the Panel and for the Image, each with its own: both
    // fit on the same GameObject (and with the Button, the Text and the bar), so
    // a shared "before" would mix drags of different components.
    uint64_t    m_panelDragOwnerId = 0;
    const char* m_panelDragField   = nullptr;
    glm::vec2   m_panelDragBefore2 {0.0f};
    glm::vec4   m_panelDragBefore4 {1.0f};
    std::string m_panelDragBeforeStr;

    // And one of its own for each interactive widget, for the same reason: they all
    // fit on the same GameObject and a shared "before" would mix drags.

    // And one of its own for each widget of the third batch, for the same reason as
    // the others: they all fit on the same GameObject.

    uint64_t    m_inputFieldDragOwnerId = 0;
    const char* m_inputFieldDragField   = nullptr;
    float       m_inputFieldDragBefore  = 0.0f;
    glm::vec2   m_inputFieldDragBefore2 {0.0f};
    glm::vec4   m_inputFieldDragBefore4 {1.0f};
    std::string m_inputFieldDragBeforeStr;

    uint64_t    m_dropdownDragOwnerId = 0;
    const char* m_dropdownDragField   = nullptr;
    float       m_dropdownDragBefore  = 0.0f;
    glm::vec2   m_dropdownDragBefore2 {0.0f};
    glm::vec4   m_dropdownDragBefore4 {1.0f};
    std::string m_dropdownDragBeforeStr;

    uint64_t    m_scrollViewDragOwnerId = 0;
    const char* m_scrollViewDragField   = nullptr;
    float       m_scrollViewDragBefore  = 0.0f;
    glm::vec2   m_scrollViewDragBefore2 {0.0f};
    glm::vec4   m_scrollViewDragBefore4 {1.0f};
    std::string m_scrollViewDragBeforeStr;

    uint64_t    m_sliderDragOwnerId = 0;
    const char* m_sliderDragField   = nullptr;
    float       m_sliderDragBefore  = 0.0f;
    glm::vec2   m_sliderDragBefore2 {0.0f};
    glm::vec4   m_sliderDragBefore4 {1.0f};
    std::string m_sliderDragBeforeStr;

    uint64_t    m_checkboxDragOwnerId = 0;
    const char* m_checkboxDragField   = nullptr;
    float       m_checkboxDragBefore  = 0.0f;
    glm::vec2   m_checkboxDragBefore2 {0.0f};
    glm::vec4   m_checkboxDragBefore4 {1.0f};
    std::string m_checkboxDragBeforeStr;

    uint64_t    m_toggleDragOwnerId = 0;
    const char* m_toggleDragField   = nullptr;
    float       m_toggleDragBefore  = 0.0f;
    glm::vec2   m_toggleDragBefore2 {0.0f};
    glm::vec4   m_toggleDragBefore4 {1.0f};
    std::string m_toggleDragBeforeStr;

    uint64_t    m_scrollbarDragOwnerId = 0;
    const char* m_scrollbarDragField   = nullptr;
    float       m_scrollbarDragBefore  = 0.0f;
    glm::vec2   m_scrollbarDragBefore2 {0.0f};
    glm::vec4   m_scrollbarDragBefore4 {1.0f};
    std::string m_scrollbarDragBeforeStr;

    uint64_t    m_imageDragOwnerId = 0;
    const char* m_imageDragField   = nullptr;
    float       m_imageDragBefore  = 0.0f;
    glm::vec2   m_imageDragBefore2 {0.0f};
    glm::vec4   m_imageDragBefore4 {1.0f};
    std::string m_imageDragBeforeStr;

    // Box Collider: same cache pattern as Transform: it persists between
    // frames so the DragFloat can accumulate the drag delta, and it is
    // resynchronized with the real BoxCollider when the selection changes or (if it
    // is dynamic and is not being dragged) every frame to reflect external
    // changes of size/gravity.
    glm::vec3    m_editColliderCenter{0.0f};
    glm::vec3    m_editColliderSize{50.0f};
    bool         m_editIsTrigger = false;
    // Physics material of the collider (defaults equal to those of Collider).
    float        m_editColliderStaticFriction  = 0.5f;
    float        m_editColliderDynamicFriction = 0.5f;
    float        m_editColliderBounciness      = 0.1f;
    bool         m_colliderDragActive = false;
    // Snapshot taken when a Center/Size drag starts: the "before" of the
    // PropertyCommand<BoxColliderState> that is pushed on confirm.
    BoxColliderState m_boxColliderBeforeEdit{};

    // Sphere Collider: same cache pattern as Box Collider.
    glm::vec3       m_editSphereCenter{0.0f};
    float           m_editSphereRadius{25.0f};
    bool            m_editSphereIsTrigger = false;
    float           m_editSphereStaticFriction  = 0.5f;
    float           m_editSphereDynamicFriction = 0.5f;
    float           m_editSphereBounciness      = 0.1f;
    bool            m_sphereColliderDragActive = false;
    SphereColliderState m_sphereColliderBeforeEdit{};

    // Capsule Collider: same cache pattern as Box Collider.
    glm::vec3        m_editCapsuleCenter{0.0f};
    float            m_editCapsuleRadius{15.0f};
    float            m_editCapsuleHeight{50.0f};
    bool             m_editCapsuleIsTrigger = false;
    float            m_editCapsuleStaticFriction  = 0.5f;
    float            m_editCapsuleDynamicFriction = 0.5f;
    float            m_editCapsuleBounciness      = 0.1f;
    bool             m_capsuleColliderDragActive = false;
    CapsuleColliderState m_capsuleColliderBeforeEdit{};

    // Plane Collider: only Center (without Size/Use Gravity, always static).
    glm::vec3      m_editPlaneCenter{0.0f};
    bool           m_editPlaneIsTrigger = false;
    float          m_editPlaneStaticFriction  = 0.5f;
    float          m_editPlaneDynamicFriction = 0.5f;
    float          m_editPlaneBounciness      = 0.1f;
    bool           m_planeColliderDragActive = false;
    PlaneColliderState m_planeColliderBeforeEdit{};

    // Rigidbody: same cache pattern as the colliders. The DragFloat
    // (mass/drag/angularDrag) use begin/commit with m_rigidbodyBeforeEdit to
    // push a single PropertyCommand<RigidbodyState> on release; the checkboxes
    // (gravity/kinematic/constraints) push an immediate command.
    float          m_editRbMass = 1.0f;
    bool           m_editRbUseGravity = true;
    bool           m_editRbKinematic = false;
    float          m_editRbDrag = 0.0f;
    float          m_editRbAngularDrag = 0.05f;
    uint32_t       m_editRbConstraints = 0;
    bool           m_editRbCcd = false;
    bool           m_editRbInterpolate = false;
    bool           m_rigidbodyDragActive = false;
    uint64_t       m_rigidbodyDragOwnerId = 0;
    RigidbodyState m_rigidbodyBeforeEdit{};

    // Camera: same cache pattern as Rigidbody. The DragFloat (fov/size/
    // near/far) use begin/commit with m_cameraBeforeEdit to push a single
    // PropertyCommand<CameraState> on release; the mode combo pushes an
    // immediate command.
    CameraComponent::ProjectionMode m_editCamMode = CameraComponent::ProjectionMode::Perspective;
    float       m_editCamFov = 45.0f;
    float       m_editCamOrthoSize = 100.0f;
    float       m_editCamNear = 1.0f;
    float       m_editCamFar = 2000.0f;
    bool        m_cameraDragActive = false;
    uint64_t    m_cameraDragOwnerId = 0;
    CameraState m_cameraBeforeEdit{};

    // Own ImGuiFileDialog instance for "Add > Mesh", separate from
    // m_audioFileDialog: the library documents that a single shared instance
    // (e.g. the IGFD::FileDialog::Instance() singleton) does not support 2
    // concurrent dialogs (same internal state of file list/thumbnails/columns), and
    // the Mesh and Audio dialogs can be open at the same time; sharing an instance
    // caused corruption when resizing the popup of one while the other was still open
    // the same frame. unique_ptr because IGFD::FileDialog is an incomplete type here.
    bool m_meshDlgOpen = false;
    // Which GameObject the chosen FBX goes back to when the dialog closes, several
    // frames after being opened: none of the dialogs of this panel is modal
    // (zero ImGuiFileDialogFlags_Modal), so the Hierarchy is still clickable and the
    // selection may have changed by then. Without this the mesh was loaded onto the
    // object selected AT THAT MOMENT, not the one that opened the dialog.
    // Same pattern, and same reason, as m_textureDlgOwner and m_fontDlgOwner.
    uint64_t m_meshDlgOwner = 0;
    std::unique_ptr<IGFD::FileDialog> m_meshFileDialog;
    // Message of the last failed Mesh load attempt (empty if there is no pending
    // error); it is cleared when the selection changes or on a successful load.
    std::string m_meshLoadError;
    // Material texture rejected for an unsupported extension. It is cleared when the
    // selection changes, same reason as m_meshLoadError: otherwise the error of the
    // previous object stays painted under the textures of the new one.
    std::string m_textureLoadError;
    // Own ImGuiFileDialog instance for the Material section, never shared with
    // m_meshFileDialog nor with m_audioFileDialog (same reason documented above:
    // resizing the popup of one touches the internal state of the one drawing it).
    bool m_textureDlgOpen = false;
    std::unique_ptr<IGFD::FileDialog> m_textureFileDialog;
    // Which object, material and slot the Browse result goes back to when the
    // modal closes, several frames later: none of the dialogs of this
    // panel is modal (zero ImGuiFileDialogFlags_Modal), so the Hierarchy
    // is still clickable and the selection may have changed by then. Without
    // m_textureDlgOwner the result would be applied to the object selected AT THAT
    // MOMENT, not the one that opened the dialog.
    uint64_t            m_textureDlgOwner    = 0;
    int                 m_textureDlgMaterial = 0;
    MaterialTextureSlot m_textureDlgSlot     = MaterialTextureSlot::Albedo;

    // Own ImGuiFileDialog instance for linking a .mat from the "Material asset" row
    // of the Material section. Same pattern as m_textureFileDialog.
    bool m_matAssetDlgOpen = false;
    std::unique_ptr<IGFD::FileDialog> m_matAssetFileDialog;
    uint64_t m_matAssetDlgOwner    = 0;
    int      m_matAssetDlgMaterial = 0;

    // Snapshot when the drag of the Metallic/Roughness sliders of the Material
    // section starts: same pattern as m_audioDragActive/m_ssrDragActive,
    // with one difference: the Material is NOT written while dragging. The
    // viewport does follow the slider (previewMaterialFactors pushes the two floats
    // to the GPU), but the override, the Material and the command arrive only once
    // on release. Writing the Material live would capture a half-dragged value as
    // the FBX baseline (see previewMaterialFactors).
    //
    // And because the Material is not written live, the pending value lives in
    // m_materialFactorSlider and not in a local: ImGui does not deliver the value in
    // the release frame, and with a local the commit saw "nothing has changed"
    // (see DeferredSlider.h). A single instance for both sliders, for the same
    // reason as the rest of these members.
    //
    // A single set of members for the two sliders (not two): only one ImGui
    // widget can have the ActiveId at a time, so Metallic and
    // Roughness are never dragged simultaneously, and m_materialFactorDragSlot
    // says which of the two it is.
    bool                m_materialFactorDragActive        = false;
    uint64_t            m_materialFactorDragOwnerId       = 0;
    int                 m_materialFactorDragMaterialIndex = 0;
    MaterialFactorSlot  m_materialFactorDragSlot          = MaterialFactorSlot::Metallic;
    float               m_materialFactorDragBefore        = 0.0f;
    DeferredSliderFloat m_materialFactorSlider;

    // GameObject for which "Add > Mesh" was pressed (it reveals the Browse/drop
    // section until a mesh is assigned or "x" is pressed to remove it).
    // 0 = section hidden. It is not cleared when the selection changes: if the
    // user returns to the same GameObject without having completed the load, the
    // section stays visible (like leaving a collider dialog half done).
    //
    // The id and not the pointer: nobody clears this when the GameObject is deleted,
    // and since it survives selection changes on purpose, the pointer could end up
    // pointing to freed memory. It is never dereferenced (only compared),
    // so the damage was not a crash but something worse to see: a new GameObject
    // recycling that address opened the Mesh section without anyone having
    // requested it. Ids start at 1 (GameObject.cpp: s_nextId{1}), so 0
    // never collides with a real one.
    uint64_t m_meshAddRequestedFor = 0;

    // Mesh loads requested from the Mesh section, object -> job, until they
    // land (see consumeUserMeshJob). At most one per object: the
    // pendingMeshJob of loadMeshForSelected does not allow requesting another in flight.
    std::unordered_map<uint64_t, uint64_t> m_userMeshJobs;

    // Same reason as m_meshFileDialog: own instance, never shared
    // with m_meshFileDialog.
    bool m_audioDlgOpen = false;
    std::unique_ptr<IGFD::FileDialog> m_audioFileDialog;

    // Button paths (font and atlas). Same reason as m_meshFileDialog for
    // having an instance of its own per dialog, never shared. The owner id is
    // stored on open: the dialog is drained outside the section and by then
    // the selection may have changed, so resolving by id (and not by
    // ctx.selected) is what prevents writing the path onto another GameObject.
    bool     m_fontDlgOpen  = false;
    uint64_t m_fontDlgOwner = 0;
    std::unique_ptr<IGFD::FileDialog> m_fontFileDialog;

    bool     m_uiAtlasDlgOpen  = false;
    uint64_t m_uiAtlasDlgOwner = 0;
    std::unique_ptr<IGFD::FileDialog> m_uiAtlasFileDialog;

    // Last rejection by extension, so it can say WHY the file was not accepted
    // instead of swallowing it silently. It is cleared on success.
    std::string m_buttonPathError;

    // Text font. Own dialog instance (never shared with the Button one) and its
    // own owner id, for the same reason as the Button ones.
    bool     m_textFontDlgOpen  = false;
    uint64_t m_textFontDlgOwner = 0;
    std::unique_ptr<IGFD::FileDialog> m_textFontFileDialog;
    std::string m_textPathError;

    // ProgressBar images. Own dialog instance and its own owner id, for the same
    // reason as the Button ones and the Text one. A SINGLE instance for the three
    // boxes (only one dialog can be open at a time) plus the field that opened it:
    // without it, choosing a file would always write to the atlas.
    bool     m_barAtlasDlgOpen  = false;
    uint64_t m_barAtlasDlgOwner = 0;
    int      m_barAtlasDlgField = 0;   // 0 atlas, 1 background, 2 fill
    std::unique_ptr<IGFD::FileDialog> m_barAtlasFileDialog;
    std::string m_barPathError;

    // Atlas of the Panel and of the Image. Each with its own dialog instance (never
    // shared) and its own owner id, for the same reason as the others.
    bool     m_panelAtlasDlgOpen  = false;
    uint64_t m_panelAtlasDlgOwner = 0;
    std::unique_ptr<IGFD::FileDialog> m_panelAtlasFileDialog;
    std::string m_panelPathError;

    // Atlas of the four interactive ones: each with its own dialog instance.

    // Atlas (and font where there is one) of the widgets of the third batch.

    bool     m_inputFieldAtlasDlgOpen  = false;
    uint64_t m_inputFieldAtlasDlgOwner = 0;
    std::unique_ptr<IGFD::FileDialog> m_inputFieldAtlasFileDialog;
    std::string m_inputFieldPathError;
    bool     m_inputFieldFontDlgOpen  = false;
    uint64_t m_inputFieldFontDlgOwner = 0;
    std::unique_ptr<IGFD::FileDialog> m_inputFieldFontFileDialog;

    bool     m_dropdownAtlasDlgOpen  = false;
    uint64_t m_dropdownAtlasDlgOwner = 0;
    std::unique_ptr<IGFD::FileDialog> m_dropdownAtlasFileDialog;
    std::string m_dropdownPathError;
    bool     m_dropdownFontDlgOpen  = false;
    uint64_t m_dropdownFontDlgOwner = 0;
    std::unique_ptr<IGFD::FileDialog> m_dropdownFontFileDialog;

    bool     m_scrollViewAtlasDlgOpen  = false;
    uint64_t m_scrollViewAtlasDlgOwner = 0;
    std::unique_ptr<IGFD::FileDialog> m_scrollViewAtlasFileDialog;
    std::string m_scrollViewPathError;

    bool     m_sliderAtlasDlgOpen  = false;
    uint64_t m_sliderAtlasDlgOwner = 0;
    std::unique_ptr<IGFD::FileDialog> m_sliderAtlasFileDialog;
    std::string m_sliderPathError;

    bool     m_checkboxAtlasDlgOpen  = false;
    uint64_t m_checkboxAtlasDlgOwner = 0;
    std::unique_ptr<IGFD::FileDialog> m_checkboxAtlasFileDialog;
    std::string m_checkboxPathError;

    bool     m_toggleAtlasDlgOpen  = false;
    uint64_t m_toggleAtlasDlgOwner = 0;
    std::unique_ptr<IGFD::FileDialog> m_toggleAtlasFileDialog;
    std::string m_togglePathError;

    bool     m_scrollbarAtlasDlgOpen  = false;
    uint64_t m_scrollbarAtlasDlgOwner = 0;
    std::unique_ptr<IGFD::FileDialog> m_scrollbarAtlasFileDialog;
    std::string m_scrollbarPathError;

    bool     m_imageAtlasDlgOpen  = false;
    uint64_t m_imageAtlasDlgOwner = 0;
    std::unique_ptr<IGFD::FileDialog> m_imageAtlasFileDialog;
    std::string m_imagePathError;
    // Same pattern as m_meshLoadError/m_meshAddRequestedFor but for the
    // AudioClip component, id included, and for the same reason: nobody
    // clears it when the GameObject is deleted and it survives selection changes on
    // purpose, so the pointer could be left dangling and a new object
    // recycling that address opened the Audio Clip section without being asked.
    // 0 = section hidden.
    std::string m_audioLoadError;
    uint64_t    m_audioClipAddRequestedFor = 0;

    // Snapshot when the drag of the audio sliders starts: a continuous drag
    // cannot push one command per frame, so it is captured on activation and a
    // single one is pushed on release. Unlike Transform/Rigidbody (which use
    // DragFloat), SliderFloat is used here: it jumps to the value under the cursor in the
    // SAME frame in which it is activated by click, so the "before" cannot be
    // re-read from the component after drawing the widget (it would already hold the
    // new value); that is why the .cpp hoists the reads before the slider.
    bool     m_audioDragActive = false;
    // The WHOLE snapshot, not one float per slider: the section already has seven
    // continuous values (volume, pitch, the two distances, spread, pan and
    // doppler) and keeping one member for each gets out of hand. With the complete
    // struct, adding one more slider does not touch this header.
    AudioClipState m_audioDragBefore{};
    // Owner of the snapshot in progress: if the drag is interrupted without a commit
    // (e.g. Ctrl+Z in the middle of a drag rebuilds/deletes the selected GameObject)
    // and the next commit arrives for another AudioClip, this id prevents applying
    // a "before" that does not belong to it.
    uint64_t m_audioDragOwnerId = 0;

    // SSR: same snapshot pattern as the audio sliders (SliderFloat, not
    // DragFloat: it jumps to the value under the cursor in the same frame of the click, so
    // the "before" is read before drawing the widget).
    bool     m_ssrDragActive = false;
    float    m_ssrDragBeforeIntensity = 0.5f;
    uint64_t m_ssrDragOwnerId = 0;

    // "New Script" popup, triggered from Add > Script > New Script...
    // The owner is captured on open (ctx.selected can change with the popup
    // open) and resolved against the scene before adding.
    //
    // By id and not by pointer: the earlier revalidation walked the scene
    // comparing ADDRESSES, and that check does not distinguish "still alive" from
    // "another GameObject has recycled its address": the allocator reuses
    // blocks of the same size, so deleting the target and creating another one with the
    // popup open gave the green light and the script was added to the wrong
    // object. And here the pointer IS dereferenced (addScript, and the
    // ScriptComponent keeps it as owner), so it was not just a panel opening
    // when it should not. See m_meshAddRequestedFor, same defect without
    // dereference. 0 = nobody.
    bool        m_openNewScriptPopup = false;
    char        m_newScriptNameBuffer[64] = {};
    std::string m_newScriptError;
    uint64_t    m_newScriptTargetId = 0;
};

} // namespace DonTopo
