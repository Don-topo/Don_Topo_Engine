#pragma once
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <memory>

// The 28 components are DECLARED, not included. Before, their concrete headers were
// included here, and since the whole editor includes GameObject.h —directly or
// via Scene.h—, touching any of them rebuilt half the repo: `touch
// UI/SliderComponent.h`, a hundred lines, dragged in 23 TUs and 266 s, MORE than a
// whole clean build (223 s). Measured on 2026-09-04; see H19 of
// docs/core-audit.md.
//
// It is possible because the members are `std::shared_ptr<T>`, which works with an
// incomplete type, and because the GameObject destructor is OUT OF LINE: it is
// in the .cpp where the types have to be complete, and the 28 are included there.
// The three inline functions that also required it —isSkinned and getSkinnedMesh because of
// dynamic_cast, anyCollider because of the derived-to-base shared_ptr
// conversion— were moved to the .cpp for the same reason.
//
// Whoever uses a concrete component includes ITS header, which is what they should
// have done instead of living off the transitive include.

namespace DonTopo
{
    struct Mesh;
    struct SkinnedMesh;
    struct Material;
    class Collider;
    class BoxCollider;
    class SphereCollider;
    class CapsuleCollider;
    class PlaneCollider;
    class Rigidbody;
    class AudioClipComponent;
    class AudioListenerComponent;
    class ReverbZoneComponent;
    class CameraComponent;
    class AnimatorComponent;
    class ReflectionProbeComponent;
    class LightComponent;
    class CanvasComponent;
    class ButtonComponent;
    class ImageComponent;
    class LayoutComponent;
    class PanelComponent;
    class TextComponent;
    class ProgressBarComponent;
    class SliderComponent;
    class CheckboxComponent;
    class ToggleComponent;
    class ScrollbarComponent;
    class InputFieldComponent;
    class DropdownComponent;
    class ScrollViewComponent;
    class ScriptComponent;

    // Texture paths and PBR factors that the user has set by hand from
    // Properties, on top of whatever the FBX brought.
    //
    // They live here and not in Material on purpose: Material is what the
    // uploaders of both backends read and what enters the dedup key of
    // SharedGpuMesh, and it cannot tell "the model set this" from "the
    // user set this". Without that distinction there is no possible Clear.
    //
    // The base* are what was in the material the FIRST time that slot was overridden: it is what
    // Clear goes back to live. They are not serialized — when loading
    // the scene the material is re-derived from the FBX and the baseline is captured
    // again by itself.
    //
    // metallic/roughness carry the SAME mechanism as the three textures, but
    // with a sentinel instead of an empty string: 0.0 and 1.0 are valid
    // slider values (non-metallic / fully rough), so they do not work
    // as "no override" the way "" does for a path. -1.0 is outside the
    // slider's 0..1 range and cannot be reached by dragging, so it is
    // a safe sentinel. baseMetallicTaken/baseRoughnessTaken do
    // EXACTLY the same job as baseAlbedoTaken and company —"the baseline of this slot
    // was already captured"—, nothing more: whoever decides whether there is an ACTIVE
    // override is the value itself (override_ < 0.0f in
    // applyMaterialOverrides), the sentinel, just as for a texture it is
    // decided by the path being empty.
    struct MaterialOverride
    {
        int         index = 0;   // index in SkinnedMesh::materials; 0 = Mesh::material
        std::string albedo, normal, orm;
        std::string baseAlbedo, baseNormal, baseOrm;
        // "The baseline of this slot was already taken". It cannot be deduced from
        // base* being empty: a legitimately empty baseline (procedural mesh
        // without texture) would be indistinguishable from "not yet taken", and Clear
        // would leave the user's texture in place instead of removing it.
        bool        baseAlbedoTaken = false, baseNormalTaken = false, baseOrmTaken = false;

        // -1.0 = no active override (sentinel; see the note above).
        float       metallic  = -1.0f;
        float       roughness = -1.0f;
        float       baseMetallic  = 0.0f;
        float       baseRoughness = 0.0f;
        bool        baseMetallicTaken = false, baseRoughnessTaken = false;

        // Path of a .mat (Core/MaterialAsset) from which this slot takes values
        // when the object does not bring its own override for that field. "" = no
        // material asset. It lives here and not in Material for the same reason as
        // the three textures: Material does not tell "the model set this" from
        // "the user set this", and without that distinction there is no possible Clear.
        std::string matAsset;
    };

    class GameObject
    {
        public:
            // Unique among all GameObjects of the session (atomic counter
            // in the constructor) — used by the Undo/Redo commands to
            // resolve the live object via Scene::findById after an
            // undo/redo cycle that rebuilds the GameObject (Undo of Delete), where
            // a raw GameObject* would be left dangling.
            uint64_t id;

            // Advances the global counter so that it never hands out an id again
            // (nor any below it). Needed by whoever ASSIGNS an id by hand
            // instead of letting the constructor set it: today, scene loading
            // (Scene.cpp, nodeFromJson), which reuses the id that the file brings.
            //
            // Without this, a .scene saved in another session —with higher ids
            // than the ones this process has handed out— leaves the counter BEHIND
            // ids that already live in the tree, and the next GameObject
            // created gets a repeated one. From then on
            // findById keeps the last one in the traversal and the Undo commands
            // write into the wrong object, silently.
            static void reserveIdAtLeast(uint64_t id);

            // Hands out a new id, unique against EVERYTHING handed out or
            // reserved so far (same counter as the constructor and as
            // reserveIdAtLeast). Used by Scene::insertFromJson to re-id
            // a snapshot id that clashes with one already alive in the rest
            // of the tree — reusing reserveIdAtLeast there does not work because
            // that one only ADVANCES the counter, it does not hand out a value.
            static uint64_t allocateId();

            // JobId of the in-flight mesh load, 0 = none. It is an opaque uint64_t
            // on purpose: Core does not know AsyncAssetLoader, and the
            // destructor does NOT cancel anything — the pump already discards the results
            // whose targetId does not exist.
            uint64_t pendingMeshJob = 0;

            explicit GameObject(std::string name = "");
            ~GameObject();
            GameObject(GameObject&&) noexcept;
            GameObject& operator=(GameObject&&) noexcept;

            GameObject* addChild(std::string childName);

            // The baseline of each override (base*/base*Taken) belongs to THE
            // MESH it came from, not to the GameObject: if it survived a
            // mesh change, a later Clear would return the texture of a
            // model that is no longer the one loaded. Two callers broke it
            // before this reset: AsyncAssetLoader::applyLoadedMesh does
            // setMesh(nullptr) in its catch AFTER applyMaterialOverrides
            // had already captured the baseline of the mesh that never
            // came together, and removeMeshComponent removes the mesh without touching
            // materialOverrides — a Remove + Add with another FBX inherited the
            // baseline of the previous one. Resetting here, at the ONLY point
            // through which the mesh changes, covers both without depending on every place
            // that drops a mesh remembering to also clean the overrides.
            // It covers the TWO backends: D3D12Renderer::removeMeshComponent
            // only called releaseObjectSlot/releaseSkinnedSlot and skipped
            // this whole reset; since it calls setMesh(nullptr) just like
            // the Vulkan one, a Remove + Add can no longer inherit the baseline of the
            // previous FBX in either of the two.
            //
            // Safe for the scene loading path (Scene::nodeFromJson):
            // this setMesh ALWAYS runs before the JSON overrides are read/loaded
            // into `materialOverrides` (which at that point
            // is empty for a newly created node), so the reset does not
            // overwrite anything — see the ordering note in nodeFromJson.
            void setMesh(std::shared_ptr<const Mesh> mesh)
            {
                m_mesh = std::move(mesh);
                for (MaterialOverride& ov : materialOverrides)
                {
                    ov.baseAlbedo.clear(); ov.baseAlbedoTaken = false;
                    ov.baseNormal.clear(); ov.baseNormalTaken = false;
                    ov.baseOrm.clear();    ov.baseOrmTaken    = false;
                    // Same reason as the three above: the baseline of a
                    // factor belongs to THE MESH it came from, not to the
                    // GameObject. baseMetallic/baseRoughness have no "empty"
                    // value to clear (they are floats, not std::string): it is enough
                    // to lower the flag, which is the only thing
                    // applyMaterialOverrides looks at to decide whether to recapture.
                    ov.baseMetallicTaken  = false;
                    ov.baseRoughnessTaken = false;
                }
            }
            // Read-only: the mesh may be SHARED with other objects
            // (clone, undo of Delete). To modify it, editMesh().
            const std::shared_ptr<const Mesh>& getMesh() const { return m_mesh; }
            bool hasMesh()   const { return m_mesh != nullptr; }
            // Out of line: the dynamic_cast needs the complete SkinnedMesh.
            bool isSkinned() const;
            const SkinnedMesh* getSkinnedMesh() const;
            // Only write path. If the mesh is shared
            // (use_count > 1) it copies it first —keeping its type— and the
            // object now points to its copy. References that are not from
            // another GameObject (preload cache, an undo command) also
            // count: at most one copy too many, never a wrong result.
            Mesh*        editMesh();
            SkinnedMesh* editSkinnedMesh();

            void setBoxCollider(std::shared_ptr<BoxCollider> bc) { m_boxCollider = std::move(bc); }
            const std::shared_ptr<BoxCollider>& getBoxCollider() const { return m_boxCollider; }
            bool hasBoxCollider() const { return m_boxCollider != nullptr; }

            void setSphereCollider(std::shared_ptr<SphereCollider> sc) { m_sphereCollider = std::move(sc); }
            const std::shared_ptr<SphereCollider>& getSphereCollider() const { return m_sphereCollider; }
            bool hasSphereCollider() const { return m_sphereCollider != nullptr; }

            void setCapsuleCollider(std::shared_ptr<CapsuleCollider> cc) { m_capsuleCollider = std::move(cc); }
            const std::shared_ptr<CapsuleCollider>& getCapsuleCollider() const { return m_capsuleCollider; }
            bool hasCapsuleCollider() const { return m_capsuleCollider != nullptr; }

            void setPlaneCollider(std::shared_ptr<PlaneCollider> pc) { m_planeCollider = std::move(pc); }
            const std::shared_ptr<PlaneCollider>& getPlaneCollider() const { return m_planeCollider; }
            bool hasPlaneCollider() const { return m_planeCollider != nullptr; }

            // true if it has any of the 4 collider types — the 4 are
            // mutually exclusive (enforced by EditorUI, not by this class),
            // used as the single guard in the "Add" popup.
            bool hasAnyCollider() const
            {
                return m_boxCollider || m_sphereCollider || m_capsuleCollider || m_planeCollider;
            }

            // Returns the GameObject's collider as the Collider base (there is at
            // most one because of mutual exclusivity), or nullptr if it has none.
            // Used by scripting to register the trigger listener without
            // branching by concrete type.
            // Out of line: converting shared_ptr<BoxCollider> to
            // shared_ptr<Collider> requires seeing the inheritance.
            std::shared_ptr<Collider> anyCollider() const;

            // Rigidbody: body dynamics (mass/gravity/forces/constraints).
            // Requires a collider that provides the shape; one per object.
            void setRigidbody(std::shared_ptr<Rigidbody> rb) { m_rigidbody = std::move(rb); }
            const std::shared_ptr<Rigidbody>& getRigidbody() const { return m_rigidbody; }
            bool hasRigidbody() const { return m_rigidbody != nullptr; }

            void setAudioClip(std::shared_ptr<AudioClipComponent> clip) { m_audioClip = std::move(clip); }
            const std::shared_ptr<AudioClipComponent>& getAudioClip() const { return m_audioClip; }
            bool hasAudioClip() const { return m_audioClip != nullptr; }

            // Audio Listener: from where 3D audio is heard. At most one per
            // scene, just like the camera — the invariant is enforced by
            // Scene::findAudioListener, not by this class. The position and axes
            // come from the worldTransform, not from the component.
            void setAudioListener(std::shared_ptr<AudioListenerComponent> l) { m_audioListener = std::move(l); }
            const std::shared_ptr<AudioListenerComponent>& getAudioListener() const { return m_audioListener; }
            bool hasAudioListener() const { return m_audioListener != nullptr; }

            // Reverb zone: sphere of sound ambience. Several per
            // scene, unlike the listener. The FMOD resource does not live
            // here: AudioManager holds it, paired by this id.
            void setReverbZone(std::shared_ptr<ReverbZoneComponent> z) { m_reverbZone = std::move(z); }
            const std::shared_ptr<ReverbZoneComponent>& getReverbZone() const { return m_reverbZone; }
            bool hasReverbZone() const { return m_reverbZone != nullptr; }

            // Game camera: on Play the Renderer renders from this
            // GameObject (its worldTransform gives position and orientation). At most
            // one per scene — the invariant is enforced by Scene::findCamera,
            // not by this class, just like the exclusivity of colliders is enforced
            // by the editor.
            void setCameraComponent(std::shared_ptr<CameraComponent> camera) { m_cameraComponent = std::move(camera); }
            const std::shared_ptr<CameraComponent>& getCameraComponent() const { return m_cameraComponent; }
            bool hasCameraComponent() const { return m_cameraComponent != nullptr; }

            // Animator: state machine that decides which clip of the SkinnedMesh
            // plays. Unlike the camera, there is no per-scene uniqueness
            // invariant: each skinned GameObject carries its own.
            void setAnimator(std::shared_ptr<AnimatorComponent> a) { m_animator = std::move(a); }
            const std::shared_ptr<AnimatorComponent>& getAnimator() const { return m_animator; }
            bool hasAnimator() const { return m_animator != nullptr; }

            // Reflection Probe: environment probe. No per-scene uniqueness
            // invariant (unlike the camera): as many as fit in
            // memory, and the Renderer resolves which probe lights each object by
            // radius of influence.
            void setReflectionProbe(std::shared_ptr<ReflectionProbeComponent> p) { m_reflectionProbe = std::move(p); }
            const std::shared_ptr<ReflectionProbeComponent>& getReflectionProbe() const { return m_reflectionProbe; }
            bool hasReflectionProbe() const { return m_reflectionProbe != nullptr; }

            // Light. It has no uniqueness invariant either: several of the same
            // type fit per scene, and it is Scene that keeps the first
            // MAX_LIGHTS when collecting them for the Renderer. The position and
            // direction come from the worldTransform, not from the component.
            void setLight(std::shared_ptr<LightComponent> l) { m_light = std::move(l); }
            const std::shared_ptr<LightComponent>& getLight() const { return m_light; }
            bool hasLight() const { return m_light != nullptr; }

            // Canvas: the root of the 2D UI. No per-scene uniqueness invariant
            // (like the light, unlike the camera), but the LIVE canvas is
            // just one —the Renderer's—, so whoever draws applies the
            // first in pre-order (Scene::findCanvas). The position does not come from the
            // worldTransform: the UI is screen space.
            void setCanvas(std::shared_ptr<CanvasComponent> c) { m_canvas = std::move(c); }
            const std::shared_ptr<CanvasComponent>& getCanvas() const { return m_canvas; }
            bool hasCanvas() const { return m_canvas != nullptr; }

            // Button: a 2D UI widget. It only makes sense hanging from a
            // Canvas (the editor gate is PropertiesPanel::uiComponentsAvailable),
            // and like the Canvas, it is DATA ONLY: the live node is built by whoever draws
            // with syncUiWidgets(). One per GameObject, just like the light.
            void setButton(std::shared_ptr<ButtonComponent> b) { m_button = std::move(b); }
            const std::shared_ptr<ButtonComponent>& getButton() const { return m_button; }
            bool hasButton() const { return m_button != nullptr; }

            // Text: a 2D UI label. Same contract as the Button —
            // it only makes sense hanging from a Canvas and is DATA ONLY: the live
            // node is built by whoever draws with syncUiWidgets(). One per
            // GameObject, and compatible with the Button on the same object (they are
            // two sibling nodes with different names).
            void setText(std::shared_ptr<TextComponent> t) { m_text = std::move(t); }
            const std::shared_ptr<TextComponent>& getText() const { return m_text; }
            bool hasText() const { return m_text != nullptr; }

            // Progress bar of the 2D UI. Same contract as the Text: DATA
            // ONLY, and the live node (background + fill) is built by syncUiWidgets().
            // Compatible with Button and Text on the same GameObject: they are sibling
            // nodes with different name prefixes.
            void setProgressBar(std::shared_ptr<ProgressBarComponent> p) { m_progressBar = std::move(p); }
            const std::shared_ptr<ProgressBarComponent>& getProgressBar() const { return m_progressBar; }
            bool hasProgressBar() const { return m_progressBar != nullptr; }

            // Panel: the background rectangle of the 2D UI. Same contract as the
            // rest — DATA ONLY, and the live node is built by syncUiWidgets(). One
            // per GameObject, and compatible with the other UI components on the
            // same object: they are sibling nodes with different name prefixes.
            void setPanel(std::shared_ptr<PanelComponent> p) { m_panel = std::move(p); }
            const std::shared_ptr<PanelComponent>& getPanel() const { return m_panel; }
            bool hasPanel() const { return m_panel != nullptr; }

            // Image: a 2D UI sprite with its four layout modes
            // (Normal, Tiled, Sliced, Filled). Same contract as the Panel.
            void setImage(std::shared_ptr<ImageComponent> i) { m_image = std::move(i); }
            const std::shared_ptr<ImageComponent>& getImage() const { return m_image; }
            bool hasImage() const { return m_image != nullptr; }

            // Slider: the draggable value widget of the 2D UI. Same
            // contract as the rest — DATA ONLY —, with one difference: the live
            // node has mouse handlers that WRITE `value` here, so the
            // sync receives it by non-const pointer.
            void setSlider(std::shared_ptr<SliderComponent> s) { m_slider = std::move(s); }
            const std::shared_ptr<SliderComponent>& getSlider() const { return m_slider; }
            bool hasSlider() const { return m_slider != nullptr; }

            // Checkbox: the check box. Like the Slider, its live
            // node has a click handler that writes `isOn` HERE, so the
            // sync receives it by non-const pointer.
            void setCheckbox(std::shared_ptr<CheckboxComponent> c) { m_checkbox = std::move(c); }
            const std::shared_ptr<CheckboxComponent>& getCheckbox() const { return m_checkbox; }
            bool hasCheckbox() const { return m_checkbox != nullptr; }

            // Toggle: the sliding switch. Same data as the Checkbox (a
            // bool) but other fields, so it is another component.
            void setToggle(std::shared_ptr<ToggleComponent> t) { m_toggle = std::move(t); }
            const std::shared_ptr<ToggleComponent>& getToggle() const { return m_toggle; }
            bool hasToggle() const { return m_toggle != nullptr; }

            // Scrollbar: the channel with a variable-size handle. Interactive by
            // drag AND by wheel, so also by non-const pointer.
            void setScrollbar(std::shared_ptr<ScrollbarComponent> s) { m_scrollbar = std::move(s); }
            const std::shared_ptr<ScrollbarComponent>& getScrollbar() const { return m_scrollbar; }
            bool hasScrollbar() const { return m_scrollbar != nullptr; }

            // InputField: the text field. Interactive and also the only one that
            // receives KEYBOARD, so the sync takes it by non-const pointer.
            void setInputField(std::shared_ptr<InputFieldComponent> f) { m_inputField = std::move(f); }
            const std::shared_ptr<InputFieldComponent>& getInputField() const { return m_inputField; }
            bool hasInputField() const { return m_inputField != nullptr; }

            // Dropdown: the drop-down. Its subtree changes shape with the number
            // of options, which the sync takes into account when deciding whether to
            // rebuild.
            void setDropdown(std::shared_ptr<DropdownComponent> d) { m_dropdown = std::move(d); }
            const std::shared_ptr<DropdownComponent>& getDropdown() const { return m_dropdown; }
            bool hasDropdown() const { return m_dropdown != nullptr; }

            // ScrollView: the scrollable view. NOTE: the children of this GameObject
            // hang from its CONTENT node, not from the viewport — if they hung from the
            // viewport, scrolling would not drag them.
            void setScrollView(std::shared_ptr<ScrollViewComponent> s) { m_scrollView = std::move(s); }
            const std::shared_ptr<ScrollViewComponent>& getScrollView() const { return m_scrollView; }
            bool hasScrollView() const { return m_scrollView != nullptr; }

            // Auto-layout: places the CHILDREN of this GameObject, and with
            // ignoreLayout takes this one out of its parent's placement. Same
            // contract as the rest: DATA ONLY, and the node is resolved by
            // syncUiWidgets(). With no other UI component on the object, the
            // sync builds it its own container (not drawable); with one, it writes
            // the layout fields into that one's node.
            void setLayout(std::shared_ptr<LayoutComponent> l) { m_layout = std::move(l); }
            const std::shared_ptr<LayoutComponent>& getLayout() const { return m_layout; }
            bool hasLayout() const { return m_layout != nullptr; }

            // Lua scripts — unlike the rest of the slots, a vector: several
            // scripts per GameObject are allowed (even repeated).
            void addScript(std::unique_ptr<ScriptComponent> script);
            void removeScript(ScriptComponent* script);
            std::vector<std::unique_ptr<ScriptComponent>>&       getScripts()       { return m_scripts; }
            const std::vector<std::unique_ptr<ScriptComponent>>& getScripts() const { return m_scripts; }
            bool hasScripts() const { return !m_scripts.empty(); }

            void updateWorldTransforms(const glm::mat4& parentWorld = glm::mat4(1.0f));

            // First in preorder that satisfies `pred`, or nullptr. It STOPS as soon as
            // it finds it: `traverse` cannot: it visits the whole tree, so the four
            // Scene finders emulated it with an `if (!found && ...)`
            // that kept descending through everything else for nothing.
            //
            // What that costs, measured in /O2 with 5000 nodes and 20,000 searches:
            // full traversal 175 ms no matter what; stopping early, 0.007 ms if
            // the node is at the root, 50 ms if it is a third of the way in and 160 ms if it is NOT
            // there (nothing to save there, it is the only case that still
            // costs the same). That is 8.8 us per search today — and
            // PropertiesPanel, which is drawn every frame, does several.
            //
            // Returns GameObject* and not bool so that the caller does not have to
            // capture the result by hand, which is exactly the pattern being
            // removed.
            template <typename Pred>
            GameObject* findFirst(Pred&& pred)
            {
                if (pred(this)) return this;
                for (auto& c : children)
                    if (GameObject* hit = c->findFirst(pred)) return hit;
                return nullptr;
            }

            // Fn&& and not Fn: by value the functor was copied per child AND per
            // level. With today's callers that costs NOTHING measurable —they are
            // [&] lambdas of 8 bytes; measured in /O2 with 5000 nodes x 2000
            // traversals: 25.3 ms by value against 24.3 ms by reference, that is
            // noise—. With a 264-byte functor the same measurement gives 62-69 ms
            // against 26-29: 2.4x. So the cost exists and today nobody pays it.
            //
            // It is changed for the OTHER reason, which is not performance: by value, a
            // MUTABLE functor that accumulates in its own state silently loses what
            // the children add up, because each subtree receives its copy. Today there is
            // not a single mutable lambda in the repo's traverse calls (grep), so this
            // does not fix anything broken: it closes the door before someone
            // finds it while debugging why their counter comes out as zero.
            //
            // Fn&& and NOT Fn&: almost all callers pass the lambda in the call itself,
            // and a temporary cannot bind to an lvalue reference. Inside it recurses
            // with `fn`, which is already a named lvalue, so the child deduces Fn& and
            // nothing is copied.
            template <typename Fn>
            void traverse(Fn&& fn)
            {
                fn(this);
                for (auto& c : children) c->traverse(fn);
            }

            std::string name;
            glm::mat4   localTransform {1.0f};
            glm::mat4   worldTransform {1.0f};
            GameObject* parent = nullptr;
            std::vector<std::unique_ptr<GameObject>> children;

            // The Renderer keeps two separate collections/pipelines (static vs skinned),
            // which is why two indices are needed instead of a single flat meshIndex.
            int staticRenderIndex  = -1;
            int skinnedRenderIndex = -1;

            // Visibility of the Mesh component. false = the mesh is not sent to the
            // GPU: no scene pass, no shadows, no AO. Physics, collisions and
            // selection in the viewport stay the same. It reaches the Renderer per frame
            // via setObjectMeshVisible/setSkinnedMeshVisible, like SSR.
            bool meshVisible = true;

            // Empty = the material is exactly as the FBX brought it. See
            // MaterialOverride.
            std::vector<MaterialOverride> materialOverrides;

            // Screen Space Reflections per object. It is not a component: they are two
            // fields of the GameObject itself, like the transform, because what they
            // configure is how ITS mesh is drawn. ssrIntensity is the
            // reflectivity at normal incidence (F0 in ssr.comp): 1 = mirror,
            // low values reflect mostly at grazing angles. The Renderer receives them
            // per frame via setObjectSsr/setSkinnedSsr, and with ssrEnabled false
            // the object contributes no mask at all.
            bool  ssrEnabled   = false;
            float ssrIntensity = 0.5f;

        private:
            std::shared_ptr<const Mesh> m_mesh;
            std::shared_ptr<BoxCollider> m_boxCollider;
            std::shared_ptr<SphereCollider> m_sphereCollider;
            std::shared_ptr<CapsuleCollider> m_capsuleCollider;
            std::shared_ptr<PlaneCollider> m_planeCollider;
            std::shared_ptr<Rigidbody> m_rigidbody;
            std::shared_ptr<AudioClipComponent> m_audioClip;
            std::shared_ptr<AudioListenerComponent> m_audioListener;
            std::shared_ptr<ReverbZoneComponent> m_reverbZone;
            std::shared_ptr<CameraComponent> m_cameraComponent;
            std::shared_ptr<AnimatorComponent> m_animator;
            std::shared_ptr<ReflectionProbeComponent> m_reflectionProbe;
            std::shared_ptr<LightComponent> m_light;
            std::shared_ptr<CanvasComponent> m_canvas;
            std::shared_ptr<ButtonComponent> m_button;
            std::shared_ptr<TextComponent> m_text;
            std::shared_ptr<ProgressBarComponent> m_progressBar;
            std::shared_ptr<LayoutComponent> m_layout;
            std::shared_ptr<PanelComponent> m_panel;
            std::shared_ptr<ImageComponent> m_image;
            std::shared_ptr<SliderComponent> m_slider;
            std::shared_ptr<CheckboxComponent> m_checkbox;
            std::shared_ptr<ToggleComponent> m_toggle;
            std::shared_ptr<ScrollbarComponent> m_scrollbar;
            std::shared_ptr<InputFieldComponent> m_inputField;
            std::shared_ptr<DropdownComponent> m_dropdown;
            std::shared_ptr<ScrollViewComponent> m_scrollView;
            std::vector<std::unique_ptr<ScriptComponent>> m_scripts;
    };

    // The EDITABLE materials of an object, in the order the
    // overrides index: the submesh ones if it is a skinned that brings them, and otherwise the
    // one inherited from Mesh. Empty if there is no mesh.
    //
    // Out of line: the dynamic_cast needs the complete SkinnedMesh, same
    // reason as isSkinned().
    std::vector<const Material*> materialsOfMesh(const GameObject& go);
    // Same, but writable: it goes through editMesh(), so it copies the mesh if
    // it is shared.
    std::vector<Material*> editMaterialsOfMesh(GameObject& go);

    // Writes the overrides onto the materials, capturing the baseline the
    // first time it overwrites each slot. Idempotent: calling it twice in a row
    // leaves the same thing.
    void applyMaterialOverrides(GameObject& go);

    // The indices that applyMaterialOverrides is going to silently ignore (the FBX
    // was re-exported with fewer submeshes), as text and for whatever channel the
    // caller has. It is separate because applyMaterialOverrides has nowhere to write
    // a warning and none is going to be given to it: the two paths that load meshes
    // —Scene::nodeFromJson with its vector of warnings, and the asynchronous pump with
    // the editor Log— have different channels, and this helper is what makes
    // both say exactly the same thing. It appends to `out`, it does not clear it.
    // Without a mesh it writes nothing: there are no materials to compare against.
    void collectMaterialOverrideWarnings(GameObject& go, std::vector<std::string>& out);
}
