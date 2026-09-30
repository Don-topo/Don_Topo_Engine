#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json_fwd.hpp>
#include "DonTopo/Core/GameObject.h"

namespace DonTopo
{
    // Declared, not included: both only appear as `std::vector<T>&`
    // in parameters, and that works with an incomplete type. Including their headers
    // would undo the work of GameObject.h: `UiWidgetSync.h` drags in the 14
    // UI components, so Scene.h would go back to rebuilding half the repo
    // every time someone touched a slider.
    struct Light;
    struct UiCanvasBinding;

    class PhysicsManager;
    class AudioManager;
    class AsyncAssetLoader;
    struct Mesh;

    // Optional cache of meshes already loaded in RAM, indexed by sourcePath. It is
    // queried by scene loading (nodeFromJson) to skip the disk ReadFile of
    // a sourcePath that was already preloaded — the runtime fills it in
    // parallel with the JobSystem and shows progress on the splash meanwhile.
    // The values may be SkinnedMesh (an FBX with a rig): loading does a
    // dynamic_cast to rebuild the correct type. If the animation configuration
    // in the JSON matches that of the mesh, loading SHARES it
    // (clone, undo of Delete); if not, it makes a deep copy and configures it.
    using PreloadedMeshCache = std::unordered_map<std::string, std::shared_ptr<const Mesh>>;

    // Key of PreloadedMeshCache: the sourcePath for piece 0 (older caches
    // remain valid) and "<sourcePath>#piece=<n>" for the others. Without the piece
    // in the key, the undo of Delete of a multi-piece model would give the same
    // mesh to all its children.
    std::string meshCacheKey(const std::string& sourcePath, int piece);

    struct SkinnedMesh;
    // true if the mesh's animation sources (path, builtin and clip
    // names, in order) are exactly those of the "animationSources" block of the
    // .scene. Decides whether a preloaded mesh can be SHARED as is.
    bool meshMatchesAnimationConfig(const SkinnedMesh& mesh, const nlohmann::json& animationSources);

    class Scene
    {
        public:
            explicit Scene(std::string name = "Scene");

            GameObject& getRoot() { return m_root; }
            const GameObject& getRoot() const { return m_root; }

            GameObject* addGameObject(const std::string& name, GameObject* parent = nullptr);

            // Takes node out of the tree and destroys it with its whole subtree. Does
            // nothing if node is null or is the root (the root hangs from nobody).
            //
            // It notifies setOnNodeRemoved BEFORE releasing it: that is where the host
            // frees what Core does not know about (the GPU slots of the subtree and,
            // in the editor, the selection). See the comment of that setter for
            // why the notification lives here and not in every caller.
            void removeGameObject(GameObject* node);

            // Called right BEFORE destroying a node in removeGameObject, with
            // the subtree still whole and traversable: whoever listens needs to
            // read the staticRenderIndex/skinnedRenderIndex of ALL its
            // descendants, not just those of the root.
            //
            // It exists because that obligation —"remember to release the GPU before
            // deleting"— was implemented THREE times (EditorUI::onDelete,
            // ScriptManager::onDestroying and raw in DeleteGameObjectCommand) and
            // a fourth caller would have needed a fourth. The three fulfilled it;
            // the problem was not that it failed, it is that it was forgettable.
            //
            // Core does not know the Renderer, so the wiring belongs to the host (see
            // the main.cpp files of the sandbox and the runtime). Without a listener, deleting
            // works the same: the tests do not set one.
            void setOnNodeRemoved(std::function<void(GameObject*)> cb)
            {
                m_onNodeRemoved = std::move(cb);
            }

            // Moves node so that it hangs from newParent (nullptr = the scene
            // root), inserting it at position index among the destination's
            // children. index is interpreted over the list ALREADY WITHOUT node, so
            // a value >= the resulting size leaves it at the end (which is what
            // the default does).
            //
            // Returns false —touching nothing— if node is null or is the root (the
            // root hangs from nobody), or if newParent is INSIDE the subtree
            // of node: that would detach the subtree from the tree and lose the
            // unique_ptr that keeps it alive, so it is a safe hang, not
            // a weird scene.
            //
            // It does NOT touch transforms on purpose: keeping the local pose (the
            // object jumps with the parent) or the world pose (it stays where it is)
            // are both things that are wanted, and the decision belongs to the caller.
            // Whoever wants the world pose reads it BEFORE and rewrites it AFTER.
            // The two callers today are the editor hierarchy reparent
            // (via ReparentCommand, which also needs the exact index to
            // undo) and Lua's Entity:SetParent.
            bool reparent(GameObject* node, GameObject* newParent,
                          size_t index = static_cast<size_t>(-1));

            // Searches by GameObject::id in the whole tree (including the root).
            // nullptr if no node has that id. O(n) over the tree — used
            // by the Undo/Redo commands (Command.cpp) to resolve their
            // live target on every execute()/undo(), never a raw pointer.
            // Deterministic: if (through a broken invariant) more than one node
            // had that id, the FIRST in pre-order wins, never the last. The id
            // is in theory unique (Scene::insertFromJson reassigns any
            // that clashes with one already alive when reinserting a subtree), but
            // findById does not check it again here.
            GameObject* findById(uint64_t id);

            // Single source of truth of the invariant "at most one camera per
            // scene": it is sought by the Properties "Add" gate, the Scene panel context
            // menu, the Renderer camera switch and the
            // warning on pressing Play — none keeps its own state. Pre-order
            // from the root (the first wins), nullptr if there is none. O(n)
            // over the tree, like findById.
            GameObject* findCamera();
            const GameObject* findCamera() const;

            // Same idea for the invariant "at most one Audio Listener per
            // scene": it is queried by the Properties "Add" gate, the playback
            // gate on entering Play and the resolution of the listener that is
            // passed to AudioManager::update every frame. Pre-order from the root
            // (the first wins), nullptr if there is none.
            GameObject* findAudioListener();
            const GameObject* findAudioListener() const;

            // UI canvas that is applied to the Renderer's live canvas. There is no
            // invariant to enforce here (several fit in the scene), but the Renderer's
            // UiCanvas is just one: the first in pre-order wins.
            // It is queried by the editor loop and by the exported runtime loop, every
            // frame, and by the usable-area gizmo. nullptr if there is none.
            GameObject* findCanvas();
            const GameObject* findCanvas() const;

            // Each canvas of the scene with ITS widgets, ready for
            // syncUiWidgets: one per CanvasComponent, with its own
            // UiWidgetLists (list per type and HIERARCHY flattened to (id, parent
            // id) in pre-order, with 0 for those that hang from the root of THAT
            // canvas). Before there was a single bag (collectUiWidgets) because only
            // one Canvas fit in the scene; with several, putting them all together
            // would draw the pause menu on top of the HUD without anything saying so.
            //
            // The "parent" of a widget is the nearest ancestor INSIDE THE SAME
            // CANVAS that HAS some UI component, not the immediate parent:
            // an intermediate GameObject without UI contributes no rect to
            // anchor against, so it cannot hold anybody and its children
            // go up to the first one that does. A nested Canvas opens its own binding
            // and cuts that chain: whatever hangs from it is anchored to ITS root.
            //
            // A widget without any Canvas above it does not appear in any
            // binding: it is not drawn. The editor already prevents it (uiComponentsAvailable
            // requires an ancestor Canvas), so this only happens in hand-made
            // scenes.
            //
            // It lives here and not in each loop because all three need it (editor
            // with both backends and exported runtime), and three copies of this
            // traversal is how they get out of sync.
            void collectCanvases(std::vector<UiCanvasBinding>& out) const;

            // Warnings from the last operation that had to correct the loaded
            // scene (corrupt fields, several cameras, clips that no longer match).
            // Core does not know the Log Console: EditorUI dumps them after loading. They are
            // cleared at the start of each operation that can fill them, so
            // they never grow out of control.
            //
            // Repeated ones come collapsed into a single entry with " (xN)" at the
            // end: a corrupt mesh generates an IDENTICAL warning per vertex (the
            // context is the object name, not the index), and without collapsing a
            // single broken mesh writes thousands of lines to the Log and buries the
            // other warnings of that same load.
            //
            // NOTE: today only the editor drains them after loading a scene. The clone
            // warning (Lua Instantiate, in Play) has no Log consumer —
            // it is recorded for the tests and for a future consumer.
            const std::vector<std::string>& lastWarnings() const { return m_warnings; }

            // Serializes only the subtree of node (same node format that
            // toJson() uses internally, including its id) — used by
            // CreateGameObjectCommand/DeleteGameObjectCommand (Command.cpp)
            // to capture the snapshot of a GameObject without serializing the
            // whole scene.
            nlohmann::json subtreeToJson(const GameObject* node) const;

            // Rebuilds a subtree from j as a child of parent (or of the
            // root if parent is nullptr, same criterion as cloneGameObject),
            // inserted at position index of parent->children (if index
            // is out of range, at the end). The render indices of the
            // subtree are left at -1: the caller must register the meshes on the
            // GPU (see Renderer::registerGameObject). nullptr if the
            // reconstruction fails (malformed subtree).
            // preloaded: live meshes by sourcePath (the undo of Delete stores
            // them before deleting). With them the FBX is not reread and, if their
            // configuration matches, they are shared.
            GameObject* insertFromJson(const nlohmann::json& j, GameObject* parent, size_t index,
                                        PhysicsManager& physics, AudioManager& audio,
                                        const PreloadedMeshCache* preloaded = nullptr);

            // The meshes of the subtree that have a file, by sourcePath. It is
            // what seeds the preload cache of the clone and of the undo of Delete.
            static PreloadedMeshCache collectMeshes(GameObject* root);

            // Deep clone of src (transform, mesh, colliders, audio, scripts
            // with overrides) as a new child of parent (or of src's parent if
            // parent is nullptr). The render indices of the subtree are left at -1:
            // the caller must register the meshes on the GPU. nullptr if src is
            // the root or the reconstruction fails.
            GameObject* cloneGameObject(GameObject* src, GameObject* parent,
                                        PhysicsManager& physics, AudioManager& audio);

            // Collects the scene lights in the format the Renderer
            // consumes (setLights/setLightRadii). Pre-order from the root, and it
            // keeps the first MAX_LIGHTS: the rest is discarded
            // SILENTLY — it is a cap of the UBO block, not an error of the scene.
            //
            // The position and direction come from the worldTransform of each
            // GameObject (column 3 and local -Z), so it has to be called
            // AFTER propagating the frame's transforms. It returns how many
            // GameObjects with a light there were in total, which lets the caller
            // tell "scene without lights" from "scene with more than fit".
            //
            // Core does not know the Renderer: the two setters are called by the caller
            // (a scene without lights does not have to leave the viewport in
            // the dark, and that decision belongs to whoever builds the frame).
            size_t collectLights(std::vector<Light>& outLights,
                                 std::vector<float>& outRadii) const;

            template <typename Fn>
            void traverse(Fn fn) { m_root.traverse(fn); }

            void update(float dt);
            // Pushes the position of each GameObject to the voice it has playing,
            // so that 3D AudioClips follow their object. Called by Scene::update
            // (Play) and by the host paths in Edit Mode, which do not go through update
            // but do move objects with the gizmo. Cheap: 2D clips exit at
            // the first if of AudioClipComponent::updateSpatial.
            // dt feeds the doppler (velocity of each source). With 0 there is no
            // doppler effect, which is correct in Edit Mode.
            void updateAudioSpatial(float dt = 0.0f);

            // Pushes the scene reverb zones to the AudioManager: creates the
            // new ones, moves the existing ones and destroys those of GameObjects that are
            // no longer there. Called per frame; syncReverbZone is idempotent.
            //
            // It lives here and not in the AudioManager because the manager does not know the
            // scene, and not in each host loop because there are three.
            void syncReverbZones(AudioManager& audio);

            // Leaves the scene EMPTY: destroys the whole tree, so the destructors
            // of all the components of all the nodes run. That is what it
            // exists for —not to 'clean up a bit'—: both hosts call it right
            // before destroying PhysicsManager and AudioManager, and a ~Collider
            // running afterwards, against an already released PxScene, is the failure
            // this avoids. The root survives (keeps id and name) but is left
            // without children and without components.
            //
            // After this the scene is not used again: its three callers either
            // replace it (fromJson) or are closing the process.
            //
            // CALLER OBLIGATION, and now the signature does not hint at it: call it
            // BEFORE destroying PhysicsManager and AudioManager. The two managers
            // were parameters that this function did NOT use, and asking for them guaranteed
            // nothing -an already shut down one could be passed- while forcing every
            // caller to have one alive: the tests set up a PhysicsManager just
            // to satisfy the signature, and since PhysX admits a single PxFoundation
            // per process, it had to be shared among all the tests of the binary.
            // A dead parameter imposing a real restriction (H20).
            void shutdown();

            // Serializes the whole tree (transforms, mesh, colliders, audio
            // clip) to an in-memory nlohmann::json.
            nlohmann::json toJson() const;
            // Replaces the current tree with the contents of j. Clears the existing
            // scene (shutdown + move-assignment) ONLY if j is
            // valid — a failed load does not modify the in-memory scene.
            // Recreates colliders/audio via physics/audio (same factories that
            // EditorUI uses). It does not touch the Renderer — the caller must register/
            // release the meshes on the GPU (see EditorUI::reloadSceneFromJson).
            //
            // loader == nullptr → synchronous load, identical behavior to
            // always. It is what the Play→Stop restore and the tests use.
            //
            // loader != nullptr → the GameObjects are created complete but without
            // mesh, and each sourcePath enqueues a request. The caller is
            // responsible for pumping and showing the progress.
            //
            // preloaded == nullptr → no cache, each sourcePath is read from disk
            // as always. preloaded != nullptr → before reading the disk the
            // cache is queried by sourcePath and, if it is there, a deep
            // copy of the preloaded mesh is used (skinned included). A miss falls to the
            // normal disk path, so the result is identical except for
            // not repeating the ReadFile. It goes AFTER loader on purpose: the existing
            // callers (editor, tests) do not pass it and stay byte for
            // byte the same.
            bool fromJson(const nlohmann::json& j, PhysicsManager& physics, AudioManager& audio,
                          AsyncAssetLoader* loader = nullptr,
                          const PreloadedMeshCache* preloaded = nullptr);

            // Serializes the whole tree to path in JSON format (via
            // toJson()). false if the write fails.
            bool save(const std::string& path) const;
            // Reads and parses path, delegates to fromJson(...). false if the
            // file does not exist or the JSON is invalid. See fromJson for the
            // loader contract.
            bool load(const std::string& path, PhysicsManager& physics, AudioManager& audio,
                      AsyncAssetLoader* loader = nullptr,
                      const PreloadedMeshCache* preloaded = nullptr);

            // Project root against which texture paths are made relative when saving
            // and resolved when loading. Empty = paths go and come back
            // as they are, which is what the tests and any caller that does not
            // set it do (its working directory is already the root).
            //
            // It lives here and is not taken from ProjectContext because Scene is in
            // Core, and Core cannot depend on the Editor.
            void setAssetRoot(std::string root) { m_assetRoot = std::move(root); }
            const std::string& assetRoot() const { return m_assetRoot; }

        private:
            std::string m_name;
            GameObject  m_root;
            std::string m_assetRoot;

            // Enforces the one-camera-per-scene invariant after rebuilding the
            // tree: it keeps the first in pre-order and removes the
            // CameraComponent from the rest (the GameObject is kept — only the
            // component is dropped). This way a hand-edited .scene with two cameras
            // opens all the same, with a warning, instead of failing to load or ending up in a
            // state where findCamera() decides on an incoherent scene.
            // Repairs a file with repeated ids: the second and following
            // get a new id and a warning is left. Without this, findById -and with it the
            // gizmo, the undo commands and the panel- resolve to the wrong
            // object, which takes the whole matrix of the other one.
            void pruneDuplicateIds();
            void pruneExtraCameras();

            // The same for the Audio Listener: it keeps the first in pre-order
            // and removes the component from the rest, leaving a warning per discarded
            // object. The GameObject is kept.
            void pruneExtraAudioListeners();

            // Collapses the repeated warnings of m_warnings in place, keeping the
            // order of first appearance and appending " (xN)" to those that appeared
            // more than once. Called at the end of each operation that fills
            // m_warnings, never during: the producers push without looking.
            void collapseWarnings();

            std::vector<std::string> m_warnings;
            std::function<void(GameObject*)> m_onNodeRemoved;
    };
}
