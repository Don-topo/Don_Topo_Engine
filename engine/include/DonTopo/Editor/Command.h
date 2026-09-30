#pragma once
#include "DonTopo/Audio/AudioBus.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include "DonTopo/Core/CameraComponent.h"
#include "DonTopo/Core/GameObject.h" // MaterialOverride and Mesh, for MeshComponentCommand
#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Renderer/ModelLoader.h" // ModelPiece, for insertModelPieces
#include "DonTopo/UI/CanvasComponent.h"
#include "DonTopo/UI/ButtonComponent.h"
#include "DonTopo/UI/TextComponent.h"

namespace DonTopo {

class Scene;
class Renderer;
class EditorRenderer;
class PhysicsManager;
class AudioManager;

class GameObject;

// Duplicates `src` as its SIBLING (same parent, not a child of the original) and
// returns the clone, or nullptr if it cannot be duplicated (scene root, or
// failure of Scene::cloneGameObject).
//
// The copying itself is Scene::cloneGameObject and is not reimplemented here: the
// only thing this function adds is the parent DECISION. It lives outside EditorUI
// so it can be tested without a GUI, just as `makeRenderSettingCommand` is the seam
// of the render settings: EditorUI only looks at the shortcut's gate, calls here and
// pushes a CreateGameObjectCommand with the result's snapshot.
//
// It does NOT register the clone on the GPU nor push it onto the undo stack: that is the caller's job,
// which is the one holding the EditorRenderer and the UndoManager.
GameObject* duplicateAsSibling(Scene& scene, GameObject* src,
                                PhysicsManager& physics, AudioManager& audio);

class ICommand {
public:
    virtual ~ICommand() = default;
    virtual void execute() = 0;   // applies "after" (redo)
    virtual void undo() = 0;      // applies "before"
    virtual std::string label() const = 0;   // pa Log Console
};

// Several commands as ONE undo step: execute in order, undo in reverse
// order. It was born for "Add Mesh" of a multi-piece model (one
// CreateGameObjectCommand per child created), but does not depend on that: it groups
// any list of ICommand.
class CompositeCommand : public ICommand {
public:
    explicit CompositeCommand(std::string label) : m_label(std::move(label)) {}
    void add(std::unique_ptr<ICommand> cmd) { m_cmds.push_back(std::move(cmd)); }
    bool empty() const { return m_cmds.empty(); }
    void execute() override { for (auto& c : m_cmds) c->execute(); }
    void undo() override { for (auto it = m_cmds.rbegin(); it != m_cmds.rend(); ++it) (*it)->undo(); }
    std::string label() const override { return m_label; }
private:
    std::string m_label;
    std::vector<std::unique_ptr<ICommand>> m_cmds;
};

// Generic command for any value-type property of a GameObject or of
// one of its components. apply() resolves the live object every time it is
// invoked (it never captures a raw GameObject*), so it survives the
// GameObject having been rebuilt in the meantime by an Undo of Delete.
template <typename T>
class PropertyCommand : public ICommand {
public:
    PropertyCommand(std::string label, T before, T after,
                     std::function<void(const T&)> apply)
        : m_label(std::move(label)), m_before(std::move(before)),
          m_after(std::move(after)), m_apply(std::move(apply)) {}

    void execute() override { m_apply(m_after); }
    void undo()    override { m_apply(m_before); }
    std::string label() const override { return m_label; }

private:
    std::string m_label;
    T m_before;
    T m_after;
    std::function<void(const T&)> m_apply;
};

// Command for a RENDER SETTING (those of the View menu: bloom, SSAO, fog, AA...).
//
// It is a PropertyCommand<T> with two differences that come from where the
// data lives, not from what type it has:
//
//  1. `persist` is ALWAYS called together with the setter. A render setting is not
//     in the scene, it is in project.json: an undo that applies the value but
//     does not rewrite the file fixes the image and leaves the undone thing waiting for
//     the project to be reopened. Having them paired in the helper is what prevents
//     any of the 39 callers from forgetting one.
//  2. It is pushed with `UndoManager::push(cmd, /*dirtiesScene=*/false)`: moving a
//     bloom slider is not a scene edit and cannot trigger the
//     "there are unsaved changes" modal.
//
// The helper does NOT apply anything when constructed: the ImGui widget already wrote the
// new value when it returned true, just like in the rest of the editor.
template <typename T, typename Setter, typename Persist>
std::unique_ptr<ICommand> makeRenderSettingCommand(std::string label, T before, T after,
                                                    Setter set, Persist persist)
{
    return std::make_unique<PropertyCommand<T>>(
        std::move(label), std::move(before), std::move(after),
        [set = std::move(set), persist = std::move(persist)](const T& v) {
            set(v);
            persist();
        });
}

// Value-type snapshots for each collider type: T of PropertyCommand<T>
// in the Box/Sphere/Capsule/Plane Collider sections of the Properties panel.
// Gravity no longer lives in the collider (it moved to the Rigidbody): see RigidbodyState.
// staticFriction/dynamicFriction/bounciness: physics material per collider.
// They go in the snapshot so that the section's undo covers them just like
// center/size; the defaults match those of Collider (0.5 / 0.5 / 0.1).
struct BoxColliderState     { glm::vec3 center; glm::vec3 size; bool isTrigger;
                              float staticFriction; float dynamicFriction; float bounciness; };
struct SphereColliderState  { glm::vec3 center; float radius; bool isTrigger;
                              float staticFriction; float dynamicFriction; float bounciness; };
struct CapsuleColliderState { glm::vec3 center; float radius; float height; bool isTrigger;
                              float staticFriction; float dynamicFriction; float bounciness; };
struct PlaneColliderState   { glm::vec3 center; bool isTrigger;
                              float staticFriction; float dynamicFriction; float bounciness; };

// Value-type snapshot of the Rigidbody: T of PropertyCommand<T> in the
// Rigidbody section of the Properties panel.
struct RigidbodyState {
    float    mass;
    bool     useGravity;
    bool     isKinematic;
    float    drag;
    float    angularDrag;
    uint32_t constraints;
    bool     ccd;
    bool     interpolate;
};

// Value-type snapshot of the AudioClipComponent: T of PropertyCommand<T> in the
// Audio Clip section of the Properties panel. The four sliders (volume, pitch and
// the two 3D distances) PLUS the three checkboxes: before, loop/is3D/playOnAwake were
// written directly and had no undo, so unchecking "Is 3D?" with a clip
// playing cut it off abruptly and Ctrl+Z did not bring it back.
//
// The three go in the same struct as the sliders, not in a separate one: a single
// command type for the whole section makes an undo restore the complete state
// even if sliders and checkboxes were touched in a different order.
struct AudioClipState {
    float volume;
    float pitch;
    float minDistance;
    float maxDistance;
    bool  loop;
    bool  is3D;
    bool  playOnAwake;
    // Output bus. It goes in the same snapshot as the rest for the same reason as the
    // checkboxes: a single command for the whole section.
    AudioBus bus;
    // Like loop and is3D: changing it reloads the sound.
    AudioLoadMode loadMode;
    AudioRolloff  rolloff;
    float spread;
    float stereoPan;
    float dopplerLevel;
    bool  mute;
};

// Value-type snapshot of the CameraComponent: T of PropertyCommand<T> in the
// Camera section of the Properties panel.
struct CameraState {
    CameraComponent::ProjectionMode mode;
    float fov;
    float orthographicSize;
    float nearPlane;
    float farPlane;
};

class ReparentCommand : public ICommand {
public:
    ReparentCommand(Scene& scene, std::string label, uint64_t id,
                     uint64_t oldParentId, size_t oldIndex,
                     uint64_t newParentId, size_t newIndex);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void moveTo(uint64_t parentId, size_t index);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    uint64_t m_oldParentId;
    size_t m_oldIndex;
    uint64_t m_newParentId;
    size_t m_newIndex;
};

// Deletes an already existing GameObject (execute) / rebuilds it from a
// JSON snapshot taken BEFORE deleting it (undo). The snapshot keeps the original
// id (Scene::subtreeToJson/nodeToJson serialize "id"), so later
// commands in the stack that reference that id still resolve
// it after an undo() of this command.
class DeleteGameObjectCommand : public ICommand {
public:
    DeleteGameObjectCommand(Scene& scene, PhysicsManager& physics, AudioManager& audio, EditorRenderer& renderer,
                             std::string label, uint64_t parentId, size_t index, nlohmann::json snapshot);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    Scene& m_scene;
    PhysicsManager& m_physics;
    AudioManager& m_audio;
    EditorRenderer& m_renderer;
    std::string m_label;
    uint64_t m_parentId;
    size_t m_index;
    nlohmann::json m_snapshot;
    // Live meshes of the subtree, taken in execute() before deleting: undo
    // reuses them instead of re-reading the FBX files (~230 ms per character).
    // Same type as Scene::PreloadedMeshCache, written by hand so as not to drag
    // Scene.h into everything that includes Command.h.
    std::unordered_map<std::string, std::shared_ptr<const Mesh>> m_meshes;
};

// Same type as Scene.h's PreloadedMeshCache (sourcePath/piece -> live
// mesh). It is repeated here, instead of including Scene.h, for the same reason as
// DeleteGameObjectCommand's hand-written m_meshes: Scene.h would drag half the engine
// into everything that includes Command.h. Redeclaring the same alias in the same
// namespace with the same underlying type is legal.
using PreloadedMeshCache = std::unordered_map<std::string, std::shared_ptr<const Mesh>>;

// Inverse of DeleteGameObjectCommand: rebuilds from snapshot (execute) /
// deletes (undo). snapshot already includes the complete subtree exactly as it was
// right after creating it (same format as DeleteGameObjectCommand).
class CreateGameObjectCommand : public ICommand {
public:
    // preloaded: meshes already in RAM for insertFromJson (see Scene::insertFromJson).
    // With them, a redo does not re-read the source file. Empty (the default) behaves
    // as before: insertFromJson receives nullptr and reads from disk.
    CreateGameObjectCommand(Scene& scene, PhysicsManager& physics, AudioManager& audio, EditorRenderer& renderer,
                             std::string label, uint64_t parentId, size_t index, nlohmann::json snapshot,
                             PreloadedMeshCache preloaded = {});
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    Scene& m_scene;
    PhysicsManager& m_physics;
    AudioManager& m_audio;
    EditorRenderer& m_renderer;
    std::string m_label;
    uint64_t m_parentId;
    size_t m_index;
    nlohmann::json m_snapshot;
    PreloadedMeshCache m_preloaded;
};

// Creates a child of `parent` per occurrence of `pieces`, at the end of its children,
// with the piece's name and transform and the mesh meshes[piece] (without
// reading disk). A transform with any non-finite value becomes identity
// with a warning in `warnings`. Returns the created children, in order; render
// indices at -1 (the caller registers). It is the seam that is tested without a GPU.
std::vector<GameObject*> insertModelPieces(Scene& scene, GameObject* parent, const std::string& sourcePath,
                                           const std::vector<ModelPiece>& pieces,
                                           const std::vector<std::shared_ptr<const Mesh>>& meshes,
                                           PhysicsManager& physics, AudioManager& audio,
                                           std::vector<std::string>* warnings);

// Adds (add=true) or removes (add=false) the CameraComponent of GameObject id;
// undo() does the opposite.
//
// Unlike the Add of collider/Rigidbody (which do not go through the stack),
// the camera one DOES: without this you can end up with two cameras in the scene: Add to X,
// Delete X (the snapshot takes the camera with it), Add to Z (allowed, findCamera()
// is nullptr), Ctrl+Z resurrects X WITH its camera. With the Add in the stack, to
// undo X's Delete you first have to undo Z's Add, and the order enforces
// the invariant without discarding anything.
//
// It resolves the GameObject by id on every execute()/undo() (never a raw pointer),
// same contract as PropertyCommand. m_state keeps the values so that an
// Add-undo-redo does not return them to the defaults.
class CameraComponentCommand : public ICommand {
public:
    CameraComponentCommand(Scene& scene, std::string label, uint64_t id,
                            bool add, CameraState state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    CameraState m_state;
};

// Add/Remove of the CanvasComponent, same contract as CameraComponentCommand:
// it resolves the GameObject by id on every execute()/undo() (never a raw pointer),
// and m_state is a COPY of the whole component (10 fields, all POD) so that an
// Add-undo-redo does not return the resolution to the defaults.
class CanvasComponentCommand : public ICommand {
public:
    CanvasComponentCommand(Scene& scene, std::string label, uint64_t id,
                            bool add, CanvasComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    CanvasComponent m_state;
};

// Add/Remove of the AudioClipComponent. Same pattern as CanvasComponentCommand
// (it resolves the GameObject by id on every execute()/undo(), never a raw pointer)
// with one forced difference: AudioClipComponent is NOT copyable (it wraps an FMOD
// soundId and its destructor unloads the sound), so the snapshot is
// plain data (path + AudioClipState) and redoing the Add recreates the component
// with createAudioClipComponent, the same factory used by Scene::fromJson.
//
// Without this, removing an Audio Clip permanently lost the volume, pitch and the two
// hand-tuned distances, and Ctrl+Z brought back nothing.
class AudioClipComponentCommand : public ICommand {
public:
    AudioClipComponentCommand(Scene& scene, AudioManager& audio, std::string label,
                               uint64_t id, bool add, std::string path, AudioClipState state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene&         m_scene;
    AudioManager&  m_audio;
    std::string    m_label;
    uint64_t       m_id;
    bool           m_add;
    std::string    m_path;
    AudioClipState m_state;
};

// Add/Remove of the AudioListenerComponent. Its whole state is a bool, but the
// command exists for the same reason as the others: adding and removing it goes
// through the undo stack like everything else in the panel.
class AudioListenerComponentCommand : public ICommand {
public:
    AudioListenerComponentCommand(Scene& scene, std::string label, uint64_t id,
                                   bool add, bool enabled);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene&      m_scene;
    std::string m_label;
    uint64_t    m_id;
    bool        m_add;
    bool        m_enabled;
};

// Add/Remove of the ButtonComponent, copied from CanvasComponentCommand: it resolves the
// GameObject by id on every execute()/undo() (never a raw pointer), and m_state is
// a COPY of the whole component so that an Add-undo-redo does not return the
// colors, the text or the paths to the defaults.
class ButtonComponentCommand : public ICommand {
public:
    ButtonComponentCommand(Scene& scene, std::string label, uint64_t id,
                            bool add, ButtonComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    ButtonComponent m_state;
};

// Add/Remove of the TextComponent, copied from ButtonComponentCommand: it resolves the
// GameObject by id on every execute()/undo() (never a raw pointer), and m_state is
// a COPY of the whole component so that an Add-undo-redo does not return the text,
// the colors or the font path to the defaults.
class TextComponentCommand : public ICommand {
public:
    TextComponentCommand(Scene& scene, std::string label, uint64_t id,
                          bool add, TextComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    TextComponent m_state;
};

// Add/Remove of the ProgressBarComponent, copied from TextComponentCommand: it resolves
// the GameObject by id on every execute()/undo() (never a raw pointer), and m_state
// is a COPY of the whole component so that an Add-undo-redo does not return the
// value, the colors or the sprite paths to the defaults.
class ProgressBarComponentCommand : public ICommand {
public:
    ProgressBarComponentCommand(Scene& scene, std::string label, uint64_t id,
                                 bool add, ProgressBarComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    ProgressBarComponent m_state;
};

// Add/Remove of the LayoutComponent, copied from ProgressBarComponentCommand:
// it resolves the GameObject by id on every execute()/undo() (never a raw pointer),
// and m_state is a COPY of the whole component so that an Add-undo-redo does not
// return the mode, the padding or the cell to the defaults.
class LayoutComponentCommand : public ICommand {
public:
    LayoutComponentCommand(Scene& scene, std::string label, uint64_t id,
                            bool add, LayoutComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    LayoutComponent m_state;
};

// Add/Remove of the PanelComponent, copied from LayoutComponentCommand: it resolves the
// GameObject by id on every execute()/undo() (never a raw pointer), and m_state is
// a COPY of the whole component so that an Add-undo-redo does not return the rect,
// the color or the sprite to the defaults.
class PanelComponentCommand : public ICommand {
public:
    PanelComponentCommand(Scene& scene, std::string label, uint64_t id,
                          bool add, PanelComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    PanelComponent m_state;
};

// Add/Remove of the ImageComponent, same pattern: m_state is a COPY of the
// whole component so that an Add-undo-redo does not return the mode, the 9-slice
// borders or the Filled block to the defaults.
class ImageComponentCommand : public ICommand {
public:
    ImageComponentCommand(Scene& scene, std::string label, uint64_t id,
                          bool add, ImageComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    ImageComponent m_state;
};

// Add/Remove of the SliderComponent, same pattern: m_state is a COPY of the
// whole component so that an Add-undo-redo does not return the value, the range or
// the colors to the defaults. The copy starts with FRESH callbacks (UiSliderCallbackSlot
// does not copy them), so an undo does not revive the handler of a dead script.
class SliderComponentCommand : public ICommand {
public:
    SliderComponentCommand(Scene& scene, std::string label, uint64_t id,
                           bool add, SliderComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    SliderComponent m_state;
};

// Add/Remove of the CheckboxComponent, same pattern as the others: m_state is a COPY
// of the whole component so that an Add-undo-redo does not return its fields to the
// defaults, and the copy starts with FRESH callbacks (the slot does not copy them) so an undo
// does not revive the handler of a dead script.
class CheckboxComponentCommand : public ICommand {
public:
    CheckboxComponentCommand(Scene& scene, std::string label, uint64_t id,
                        bool add, CheckboxComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    CheckboxComponent m_state;
};
// Add/Remove of the ToggleComponent, same pattern as the others: m_state is a COPY
// of the whole component so that an Add-undo-redo does not return its fields to the
// defaults, and the copy starts with FRESH callbacks (the slot does not copy them) so an undo
// does not revive the handler of a dead script.
class ToggleComponentCommand : public ICommand {
public:
    ToggleComponentCommand(Scene& scene, std::string label, uint64_t id,
                        bool add, ToggleComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    ToggleComponent m_state;
};
// Add/Remove of the ScrollbarComponent, same pattern as the others: m_state is a COPY
// of the whole component so that an Add-undo-redo does not return its fields to the
// defaults, and the copy starts with FRESH callbacks (the slot does not copy them) so an undo
// does not revive the handler of a dead script.
class ScrollbarComponentCommand : public ICommand {
public:
    ScrollbarComponentCommand(Scene& scene, std::string label, uint64_t id,
                        bool add, ScrollbarComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    ScrollbarComponent m_state;
};

// Add/Remove of the InputFieldComponent, same pattern as the others: m_state is a COPY
// of the whole component so that an Add-undo-redo does not return its fields to the
// defaults, and the copy starts with FRESH callbacks (the slot does not copy them) so an undo
// does not revive the handler of a dead script.
class InputFieldComponentCommand : public ICommand {
public:
    InputFieldComponentCommand(Scene& scene, std::string label, uint64_t id,
                        bool add, InputFieldComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    InputFieldComponent m_state;
};
// Add/Remove of the DropdownComponent, same pattern as the others: m_state is a COPY
// of the whole component so that an Add-undo-redo does not return its fields to the
// defaults, and the copy starts with FRESH callbacks (the slot does not copy them) so an undo
// does not revive the handler of a dead script.
class DropdownComponentCommand : public ICommand {
public:
    DropdownComponentCommand(Scene& scene, std::string label, uint64_t id,
                        bool add, DropdownComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    DropdownComponent m_state;
};
// Add/Remove of the ScrollViewComponent, same pattern as the others: m_state is a COPY
// of the whole component so that an Add-undo-redo does not return its fields to the
// defaults, and the copy starts with FRESH callbacks (the slot does not copy them) so an undo
// does not revive the handler of a dead script.
class ScrollViewComponentCommand : public ICommand {
public:
    ScrollViewComponentCommand(Scene& scene, std::string label, uint64_t id,
                        bool add, ScrollViewComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    ScrollViewComponent m_state;
};

// Add/Remove of the AnimatorComponent, same contract as CameraComponentCommand:
// it resolves the GameObject by id on every execute()/undo() (never a raw pointer),
// and m_state keeps the graph so that an Add-undo-redo does not return it empty.
//
// The state is a COPY of the whole component, not a POD of fields like
// CameraState: an Animator's "state" is the complete graph, and
// AnimatorComponent is copyable (only vectors, maps and PODs). Serializing it to
// JSON for this would buy nothing: the JSON functions live in the anonymous
// namespace of Scene.cpp and are not accessible from here.
class AnimatorComponentCommand : public ICommand {
public:
    AnimatorComponentCommand(Scene& scene, std::string label, uint64_t id,
                              bool add, AnimatorComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    AnimatorComponent m_state;
};

// Edit of the GRAPH of an Animator that already exists (states, transitions,
// conditions, parameters, entry), through the undo stack. It is created by
// AnimatorGraphUndoTracker at the end of a gesture in the AnimatorPanel, with the
// change ALREADY applied: it is pushed without execute().
//
// Unlike AnimatorComponentCommand, which adds or removes the whole
// component, this applies only what is authored via applyGraph: the parameter
// values and the playhead survive, so an undo in Play does not take away
// what the script was writing (H4).
//
// It resolves the GameObject by id on each application, never by pointer. If the
// object or its Animator no longer exist, it does nothing.
class AnimatorGraphCommand : public ICommand {
public:
    AnimatorGraphCommand(Scene& scene, std::string label, uint64_t id,
                          AnimatorComponent::Graph before, AnimatorComponent::Graph after);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(const AnimatorComponent::Graph& g);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    AnimatorComponent::Graph m_before;
    AnimatorComponent::Graph m_after;
};

// Adds (add=true) or removes (add=false) an animation source of the SkinnedMesh
// of GameObject id; undo() does the opposite. Same contract as the rest:
// it resolves the GameObject by id on every execute()/undo().
//
// m_clipNames stores the names the source contributed, and does two jobs.
// One: without it, undoing a Remove would reimport the file with the FBX's names
// and any rename would be lost, leaving orphaned the graph states
// that used them. Two: it is the IDENTITY with which applyRemove locates its
// source. Clip names are unique within the mesh (uniqueClipName guarantees it)
// and travel WITH the source, whereas a position describes where it
// was: applyAdd re-adds at the end, so any ordinal stored by
// another command in the stack changes meaning as soon as a Remove is undone.
//
// renderer can be nullptr (headless tests). When it is not, the skinned
// object's SSBOs are rebuilt: the clip list has changed and the GPU has the
// old one.
class AnimationSourceCommand : public ICommand {
public:
    // pathOccurrence: positional FALLBACK for applyRemove: which non-builtin
    // source with that path to remove, COUNTED FROM THE END of the vector
    // (0 = the most recent/last one). It is only used when the search by
    // identity (m_clipNames) finds nothing, e.g. with an empty m_clipNames.
    // Importing the same FBX twice is legal, and AnimatorPanel distinguishes the
    // rows with this same ordinal. 0 by default is valid both for a real Add
    // (nothing to disambiguate, the new source always goes at the end) and for the
    // undo of an Add.
    AnimationSourceCommand(Scene& scene, EditorRenderer* renderer, std::string label,
                            uint64_t id, bool add, std::string path,
                            std::vector<std::string> clipNames,
                            size_t pathOccurrence = 0);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void applyAdd();
    void applyRemove();

    Scene& m_scene;
    EditorRenderer* m_renderer;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    std::string m_path;
    std::vector<std::string> m_clipNames;
    size_t m_pathOccurrence;
};

// Renames a clip of the mesh and carries along the Animator states that used it.
// It does not touch the GPU: the buffers go by clip index, and renaming does not reorder
// anything.
class ClipRenameCommand : public ICommand {
public:
    ClipRenameCommand(Scene& scene, std::string label, uint64_t id,
                       std::string oldName, std::string newName);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(const std::string& from, const std::string& to);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    std::string m_oldName;
    std::string m_newName;
};

enum class MaterialTextureSlot { Albedo, Normal, Orm };

// Writes ONE texture path into the override of material `materialIndex` and
// applies it to the Material. It creates the override entry if it does not exist.
//
// It exists so that "writing the override" and "applying it" cannot go
// separately: they are two steps, the second would be forgotten, and the symptom would be that the
// panel shows the new path and the viewport the old one. The command and the panel
// (later on) both call through here, never directly to
// go.materialOverrides.
void setMaterialTextureOverride(GameObject& go, int materialIndex,
                                 MaterialTextureSlot slot, const std::string& path);

// Change of ONE texture of ONE material, through the undo stack.
//
// The renderer is a POINTER and can be nullptr (headless tests): same pattern
// as AnimationSourceCommand. It resolves the GameObject by id on every
// application, never by pointer, to survive an undo of Delete that
// rebuilds it.
class MaterialTextureCommand : public ICommand {
public:
    MaterialTextureCommand(Scene& scene, EditorRenderer* renderer, std::string label,
                            uint64_t id, int materialIndex, MaterialTextureSlot slot,
                            std::string before, std::string after);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(const std::string& path);

    Scene&              m_scene;
    EditorRenderer*     m_renderer;
    std::string         m_label;
    uint64_t            m_id;
    int                 m_materialIndex;
    MaterialTextureSlot m_slot;
    std::string         m_before;
    std::string         m_after;
};

// Writes the path of a .mat into the override of material `materialIndex` and
// applies it to the Material. Symmetric to setMaterialTextureOverride.
void setMaterialAssetOverride(GameObject& go, int materialIndex, const std::string& matAssetPath);

// Change of the .mat linked to ONE material, through the undo stack. Same pattern
// as MaterialTextureCommand: it resolves by id, optional renderer.
class MaterialAssetCommand : public ICommand {
public:
    MaterialAssetCommand(Scene& scene, EditorRenderer* renderer, std::string label,
                         uint64_t id, int materialIndex, std::string before, std::string after);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(const std::string& matAssetPath);

    Scene&          m_scene;
    EditorRenderer* m_renderer;
    std::string     m_label;
    uint64_t        m_id;
    int             m_materialIndex;
    std::string     m_before;
    std::string     m_after;
};

enum class MaterialFactorSlot { Metallic, Roughness };

// Writes ONE PBR factor (metallic or roughness) into the override of material
// `materialIndex` and applies it to the Material. Symmetric to
// setMaterialTextureOverride: it creates the override entry if it does not exist, and it is
// the only place (together with the texture one) that writes to materialOverrides; the
// command and the panel call here, never directly to go.materialOverrides.
//
// `value` can be a slider value (0..1) OR the sentinel -1.0f for "no
// override" (see MaterialOverride in GameObject.h); the undo of the first
// edit of a factor passes the sentinel here in production (see
// currentFactorOverride() in PropertiesPanel.cpp, which is where the command's
// "before" comes from). This setter does not distinguish the two cases: it writes
// `value` as is into the override and lets applyMaterialOverrides be
// the one to interpret the sign.
void setMaterialFactorOverride(GameObject& go, int materialIndex,
                                MaterialFactorSlot slot, float value);

// Change of ONE PBR factor of ONE material, through the undo stack. Copied from
// MaterialTextureCommand (same three guards: it resolves the GameObject by id
// on every application, it cuts if the index no longer describes a material of the live
// mesh, and rebuild only if there is a renderer) with the value type changed from
// std::string to float. Two independent commands (Metallic and Roughness
// are two distinct MaterialFactorSlot) instead of one that carries both
// factors at once: if they shared a single command with a combined snapshot
// like AudioClipState, dragging ONLY Metallic would have to rewrite
// Roughness's override too with its current value in order to undo
// both together, and that would "touch" it (activate its sentinel) even though the
// user never dragged it, and the factor that was not touched would be serialized the same
// as the one that was.
class MaterialFactorCommand : public ICommand {
public:
    MaterialFactorCommand(Scene& scene, EditorRenderer* renderer, std::string label,
                           uint64_t id, int materialIndex, MaterialFactorSlot slot,
                           float before, float after);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(float value);

    Scene&             m_scene;
    EditorRenderer*    m_renderer;
    std::string        m_label;
    uint64_t           m_id;
    int                m_materialIndex;
    MaterialFactorSlot m_slot;
    float              m_before;
    float              m_after;
};

// Add or remove the Mesh component, through the undo stack. Same contract as
// CameraComponentCommand: `add` says what execute() does, and undo() does the
// opposite. The two directions are the same two operations (set this
// mesh with these overrides, and remove it) with the roles swapped.
//
// REMOVE: the panel builds, calls execute() and pushes, like the rest.
//
// ADD: it is pushed WITHOUT execute(), when the load has already landed. Adding a
// Mesh is asynchronous: the button only enqueues, and until applyLoadedMesh does
// the setMesh there is no mesh to store. That is why EditorUI::onAssetsLoaded pushes it
// and not the panel, and only for the loads the user requested from Properties
// (the scene load uses the same requestMesh and is not an edit).
//
// Before, the "x" of the Mesh section removed the mesh and EMPTIED materialOverrides
// outside the undo: Ctrl+Z did not bring it back, and with it the textures and
// the hand-assigned factors were lost. Emptying is the right thing (without it, a Remove +
// Add with an FBX with fewer materials would rewrite overrides with indices that no
// longer exist, through a path that no clamp detects because the index was
// valid when it was written); what was missing was being able to undo it.
//
// It stores the mesh's shared_ptr, not its path: the undo is synchronous, without
// re-reading the FBX or going through the async load, and it returns EXACTLY the same
// mesh (a procedural one has no path to re-read). And it stores the WHOLE overrides,
// baselines included, to restore them AFTER setMesh: setMesh
// lowers the base*Taken flags, and the stored mesh already carries the overrides
// baked into its Material, so without that order applyMaterialOverrides
// would recapture the user's texture as the "FBX original", and a later
// Clear would return it to that.
//
// Two guards, one per direction, because there are paths that change the mesh WITHOUT
// going through the undo (deleting an FBX in use from the Content Browser, or a
// load that lands later):
//  - setting does nothing if the object already has a mesh or a load in flight:
//    restoring ours would overwrite the other with no way to recover it;
//  - removing only removes OURS (same instance): if the mesh is already another one, it is
//    not this command's job to take it away.
//
// The renderer is a POINTER and can be nullptr (headless tests), as in
// MaterialTextureCommand.
class MeshComponentCommand : public ICommand {
public:
    // Captures the mesh and the overrides of `go` exactly as they are NOW: with add
    // = false, right before execute(); with add = true, right after
    // the load landed.
    MeshComponentCommand(Scene& scene, EditorRenderer* renderer, std::string label,
                          GameObject& go, bool add);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void put();
    void remove();

    Scene&                        m_scene;
    EditorRenderer*               m_renderer;
    std::string                   m_label;
    uint64_t                      m_id;
    bool                          m_add;
    // Owner of the mesh ONLY while it is removed. While it is set on the
    // object, the command does not retain it (m_meshVista): if it retained it, the object
    // would see it as shared, editMesh() would copy it on the first material change,
    // and the "is it ours?" comparison in remove() would stop matching by itself.
    std::shared_ptr<const Mesh>   m_mesh;
    std::weak_ptr<const Mesh>     m_meshVista;
    std::vector<MaterialOverride> m_overrides;
};

} // namespace DonTopo
