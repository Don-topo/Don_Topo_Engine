#pragma once
#include "DonTopo/Core/PrefabOverrides.h"   // PrefabOverride (overridesUnder returns them)

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace DonTopo
{
    class AudioManager;
    class GameObject;
    class PhysicsManager;
    class PrefabLibrary;
    class Scene;
    struct Mesh;
    struct PrefabOverride;
    struct PrefabTemplate;

    // The single point where the editor detects prefab overrides (spec §3.2).
    // Edit Mode only: the host does not call it while playing.
    //
    // The stored override list is the source of truth (§3.1). A recompute diffs
    // the live instance against its template and REPLACES that list, so it is
    // only safe against the exact template content the instance was last built
    // from. Two guards, both required:
    //  - the template must be Ready. While a template (re)loads, memoryRoot may
    //    already hold the NEW content (PrefabLibrary::updateFromMemory sets it
    //    before the meshes arrive), with the old generation still in place;
    //  - the instance must have been propagated (markSynced) with the template's
    //    CURRENT generation. Otherwise "the prefab changed" would be recorded as
    //    "the user overrode it" (Review Focus 1).
    class PrefabOverrideTracker
    {
        public:
            // UndoManager revision changed (push / undo / redo / clear): recomputes
            // every synced instance of the scene. Returns how many links changed.
            int onRevisionChanged(Scene& scene, const PrefabLibrary& lib);
            // Falling edge of ImGui::IsAnyItemActive(): recomputes the instance
            // that contains `selected` (edits that bypass the undo stack, like
            // script properties). 1 if its link changed, else 0.
            int onItemDeactivated(Scene& scene, const PrefabLibrary& lib, GameObject* selected);

            // After each propagation (or a check that found the instance already
            // equal to its template): remembers the template generation per
            // instance root id.
            void markSynced(uint64_t instanceRootId, uint64_t generation);
            // A propagation to this generation failed and the instance was left as
            // it was: never diffed against it, and not reported by
            // unsyncedInstances again until the template changes.
            void markFailed(uint64_t instanceRootId, uint64_t generation);
            void forget(uint64_t instanceRootId);
            // Scene reload: ids of the new scene may repeat the old ones, but its
            // instances were never checked against their templates.
            void clear() { m_syncedGeneration.clear(); m_failedGeneration.clear(); m_baseline.clear(); }

            // ── Edits made while an instance cannot be diffed ──────────────────
            // While its template loads (or reloads after an Apply) or the instance
            // is not synced yet, an edit cannot be recorded as an override. Its
            // BASELINE -- the live subtree when it was loaded, last synced or last
            // recomputed -- is what tells those edits apart from the stored state:
            // before an instance is propagated, every difference between its live
            // subtree and its baseline is folded into its overrides, so the edit
            // survives the rebuild instead of being silently reverted.
            //
            // templateRoot: the template the instance currently stands on (memory
            // form), when one is Ready. With it, a folded value equal to THAT
            // template's is not an edit but a revert (an override undone while
            // the template reloaded): it is dropped, and so is the stored override
            // on that path, so the instance follows the NEW prefab value.
            // A node whose mesh is still in flight from the async loader has no
            // "mesh" in its baseline; its mesh paths are never folded (the mesh
            // landing is not a user edit).
            void captureBaseline(Scene& scene, GameObject* instanceRoot,
                                 const nlohmann::json* templateRoot = nullptr);
            bool hasBaseline(uint64_t instanceRootId) const { return m_baseline.count(instanceRootId) != 0; }
            // A node recorded as "mesh in flight" whose request is over (the mesh
            // landed, failed or was cancelled): its baseline "mesh" is patched
            // from the live node and it leaves the in-flight list, so an edit of
            // that mesh from now on is folded like any other. Call it at the START
            // of an editor frame (after the loader results are applied, before
            // any panel can edit). Cheap when nothing is in flight. Returns how
            // many nodes it settled.
            int settleLandedMeshes(Scene& scene);
            // Tests: linked nodes still waited for, and scene walks settleLandedMeshes made.
            int meshInFlightCount() const;
            int settleScans() const { return m_settleScans; }
            // Folds live-vs-baseline differences into instanceRoot's overrides (a
            // difference on a path already overridden replaces that value). No-op
            // without a link or a baseline. Returns how many entries it wrote.
            int foldUnrecordedEdits(Scene& scene, GameObject* instanceRoot);

            // Whether the async-loader request a GameObject::pendingMeshJob names
            // can still deliver (AsyncAssetLoader::isRequestLive). Without it, any
            // non-zero pendingMeshJob counts as pending -- so a request cancelled
            // with the loading modal would defer its instance forever.
            void setMeshRequestLive(std::function<bool(uint64_t)> fn) { m_meshRequestLive = std::move(fn); }
            bool meshPending(const GameObject* instanceRoot) const;

            // True when instanceRoot has a link, its template is Ready, and the
            // instance was synced with the template's current generation.
            bool isSynced(const GameObject* instanceRoot, const PrefabLibrary& lib) const;
            // Instance roots whose template is Ready but which were never synced
            // with its current generation (a clone, the undo of a Delete, an
            // editor drop...). The host checks them with propagateInstance.
            std::vector<GameObject*> unsyncedInstances(Scene& scene, const PrefabLibrary& lib) const;

        private:
            std::unordered_map<uint64_t, uint64_t> m_syncedGeneration;
            std::unordered_map<uint64_t, uint64_t> m_failedGeneration;
            struct Baseline
            {
                nlohmann::json live;                    // subtreeToMemoryJson at capture
                std::vector<uint64_t> meshInFlight;     // prefab-local ids waiting for a live mesh request
                nlohmann::json templateRoot;            // null when no template was Ready
            };
            std::unordered_map<uint64_t, Baseline> m_baseline;
            std::function<bool(uint64_t)> m_meshRequestLive;
            int m_settleScans = 0;
    };

    // Folding an edit (a diff of liveNode against an EARLIER state of it, value =
    // the live value): true when it takes the node back to templateNode's value
    // at edit.path -- a revert of an override, not a new one -- or removes
    // something templateNode does not have either. Both nodes: one node json
    // each, memory form (children ignored). Shared by the tracker's fold and the
    // undo of Unpack, so the two cannot tell reverts apart differently.
    bool prefabEditIsRevert(const nlohmann::json& templateNode, const nlohmann::json& liveNode,
                            const PrefabOverride& edit, bool isRoot);

    // The tree an instance is rebuilt from: tmpl (memory form) with each
    // file-mesh node pointing (memRef = prefabNodeMeshRef(local)) at the mesh the
    // library configured for that node in `configured` -- only where the node's
    // "mesh" equals its "mesh" in configuredFor (the template those meshes were
    // configured for; pass tmpl itself when they belong to it) and the instance
    // does not override the mesh's identity. The rebuild's cache must hold
    // `configured` too. Without it, a node whose animation sources are not the
    // first list of its file reconfigures them from disk (Assimp) on the main
    // thread. Shared by propagation and Apply so the two cannot drift.
    nlohmann::json seedConfiguredMeshes(const nlohmann::json& tmpl, const nlohmann::json& configuredFor,
                                        const std::unordered_map<std::string, std::shared_ptr<const Mesh>>& configured,
                                        const std::vector<PrefabOverride>& instanceOverrides);

    // Value comparison of the override model: a number that went through a float
    // is compared within 1e-5 (relative above 1); integers on both sides, and
    // everything else, exactly (spec §3.2) -- 0xFFFFFFFF is not 0xFFFFFFFE.
    bool prefabValuesEqual(const nlohmann::json& a, const nlohmann::json& b);

    // True when the live subtree of instanceRoot is exactly what a propagation
    // would rebuild from prefabMemoryRoot: the same linked nodes in the same
    // places, and a diff equal to the stored overrides (prefabValuesEqual).
    // Added children are not compared (propagation carries them as they are).
    // False when instanceRoot has no link or prefabMemoryRoot is not an object.
    bool prefabInstanceMatchesTemplate(const Scene& scene, const GameObject* instanceRoot,
                                       const nlohmann::json& prefabMemoryRoot);

    // Called with every subtree a propagation rebuilt: the host registers it on
    // the GPU (render indices are -1).
    using RegisterSubtreeFn = std::function<void(GameObject*)>;

    struct PropagationResult
    {
        int rebuilt  = 0;   // instances rebuilt from the template
        int upToDate = 0;   // instances already equal to it (left untouched)
        // Instances skipped because a node of theirs still waits for its mesh
        // from the async loader (Load Scene): serialized now, that node would
        // have no mesh, and an added child would lose it for good. Not synced;
        // the host retries once the loader is done.
        int deferred = 0;
        std::vector<std::string> warnings;   // Scene::lastWarnings of each rebuild
    };

    // Brings one instance up to a Ready template (spec §3.3), then marks it
    // synced (and captures its baseline). First it folds the edits made since its
    // baseline into its overrides (PrefabOverrideTracker::foldUnrecordedEdits).
    // Deferred, untouched, while a node still waits for a LIVE async mesh
    // request. Untouched (same object, same GPU slots) when it already matches;
    // otherwise rebuilt through Scene::rebuildPrefabInstance -- seeded with the
    // template's own meshes (PrefabTemplate::configuredMeshes and meshes) and
    // .mat cache, so a propagation reads no FBX and no .mat -- and the new
    // subtree is handed to registerGpu. Returns the LIVE root afterwards
    // (nullptr if the instance was lost). Not an undo command and does not dirty
    // the scene: the stored subtree is re-derived, not edited.
    GameObject* propagateInstance(Scene& scene, GameObject* instanceRoot, const PrefabTemplate& t,
                                  PrefabOverrideTracker& tracker, PhysicsManager& physics,
                                  AudioManager& audio, const RegisterSubtreeFn& registerGpu,
                                  PropagationResult* result = nullptr);

    // propagateInstance for every instance of t in the scene: link paths are
    // matched through lib.normalizeKey, the same normalization t.key went
    // through (a backslash or an absolute path inside the project still match).
    // Ignores templates that are not Ready.
    PropagationResult propagateReadyTemplate(Scene& scene, const PrefabLibrary& lib, const PrefabTemplate& t,
                                             PrefabOverrideTracker& tracker, PhysicsManager& physics,
                                             AudioManager& audio, const RegisterSubtreeFn& registerGpu);

    // ── Decisions behind the editor's prefab UI (spec §5), kept out of ImGui
    // so they can be tested ──────────────────────────────────────────────────

    // Hierarchy color of a node. None: not a prefab node (a plain node, an
    // added child of an instance or anything under one, the scene root).
    // Instance: a linked node whose template is Ready or still loading (or
    // there is no library to ask). Missing: a linked node whose template Failed
    // or is unknown to the library (never requested, evicted).
    enum class PrefabNodeState { None, Instance, Missing };
    PrefabNodeState prefabNodeState(const GameObject& node, const PrefabLibrary* lib);

    // The instance root node is inside of (itself for a root; any node under an
    // added child counts as inside), nullptr when it is in no instance.
    GameObject* prefabInstanceRoot(GameObject* node);
    const GameObject* prefabInstanceRoot(const GameObject* node);

    // Whether instRoot's link stores an override of node prefabNode at
    // pathPrefix or inside it, matched by JSON-pointer segments ("/light"
    // covers "/light/range", "/nam" does not cover "/name"). An override on an
    // ANCESTOR path covers it too (an added component or script, a whole-value
    // array like a color): with templateNode (that node's json in the
    // template, memory form) it counts only when the value at pathPrefix
    // differs from the template's (prefabValueAt: keyed segments resolved);
    // without it, always.
    bool isOverridden(const GameObject& instRoot, uint64_t prefabNode, std::string_view pathPrefix,
                      const nlohmann::json* templateNode = nullptr);
    // The overrides isOverridden matches (at, inside or covering pathPrefix).
    std::vector<PrefabOverride> overridesUnder(const GameObject& instRoot, uint64_t prefabNode,
                                               std::string_view pathPrefix);

    // The override path of the index-th script of a node whose scripts are
    // named scriptNames, in order: "/scripts/<name>#<k>" (keyed by name and
    // occurrence, see PrefabOverrides.h). What the Properties panel bolds and
    // applies for that script.
    std::string prefabScriptPath(const std::vector<std::string>& scriptNames, size_t index);

    // What a right click on a field or header of node `node` can act on.
    //  Field: the label's own path (the commands narrow a covering
    //         whole-value override, like a color array, to it).
    //  AddedComponent / AddedScript: the label lies inside something the
    //         prefab does not have, so only the whole `wide` override (the
    //         added component, or the added script element "/scripts/<key>")
    //         can be applied or removed; `name` is what the menu calls it.
    struct PrefabFieldTarget
    {
        enum class Kind { None, Field, AddedComponent, AddedScript };
        Kind           kind = Kind::None;
        PrefabOverride wide;
        std::string    name;
    };
    // templateNode null (template not Ready): Field when anything is overridden.
    PrefabFieldTarget prefabFieldTarget(const GameObject& instRoot, uint64_t node, std::string_view path,
                                        const nlohmann::json* templateNode);

    // The overrides of `stored` a `which` selection (Apply/Revert) acts on,
    // all entries for one node at a time in the result:
    //  - every stored entry at or inside a wanted path, as stored;
    //  - when none is, but a stored entry COVERS the wanted path (a whole-value
    //    array): a narrow override {node, path, the value at that sub-path
    //    inside the covering entry (null when absent)} -- `narrowed` true.
    //    Skipped when the template lacks what the path lives in (an added
    //    component or script: only the whole entry can go).
    // templateRoot: the template (memory form, prefab-local "id"s); null = no
    // narrowing (only stored entries are selected).
    struct PrefabSelection
    {
        PrefabOverride override;
        bool           narrowed = false;
        std::string    instancePath;   // narrowed: the wanted path it was narrowed to
    };
    std::vector<PrefabSelection> resolvePrefabSelection(const std::vector<PrefabOverride>& stored,
                                                        const std::vector<PrefabOverride>& which,
                                                        const nlohmann::json* templateRoot);

    // Revert of a selection on the stored list, in place: stored entries at or
    // inside a wanted path are removed; a covering entry is REWRITTEN at the
    // wanted sub-path to the template's value (erased where the template has
    // none), and dropped only if it then equals the template at its own path.
    struct PrefabRevertPlan
    {
        std::vector<PrefabOverride> removed;                                // as they were
        std::vector<std::pair<PrefabOverride, PrefabOverride>> rewritten;   // (before, after)
        bool empty() const { return removed.empty() && rewritten.empty(); }
    };
    PrefabRevertPlan revertPrefabSelection(std::vector<PrefabOverride>& stored,
                                           const std::vector<PrefabOverride>& which,
                                           const nlohmann::json& templateRoot);
    // The undo of such a Revert, for its rewritten entries: an entry goes back
    // to its value before the Revert only while it still holds what the Revert
    // wrote (prefabValuesEqual: float noise of a recompute is not an edit).
    // Changed since, or gone (the user set the live value back to the
    // template's, so the recompute dropped it): the newer state wins.
    void restoreRewrittenOverrides(std::vector<PrefabOverride>& list,
                                   const std::vector<std::pair<PrefabOverride, PrefabOverride>>& rewritten);

    // dir / "<name>.prefab", or "<name> 1.prefab", "<name> 2.prefab"... the
    // first that does not exist. Characters a file name cannot hold become
    // '_'; an empty name (or "." / "..") becomes "Prefab".
    std::filesystem::path uniquePrefabPath(const std::filesystem::path& dir, const std::string& name);

    // False for a node that comes from the prefab and is not its instance root
    // ("Unpack the prefab to delete this object"), and for the scene root.
    bool canDeleteNode(const GameObject& node);

    // Why the hierarchy may not move `dragged` under `newParent` (empty =
    // allowed): a linked node of an instance never leaves its place (the
    // structure is the prefab's), and nothing carrying an instance goes inside
    // another instance (nested prefabs are not supported: the outer template's
    // readers would take the inner nodes for their own).
    std::string prefabMoveRefusal(const GameObject& dragged, const GameObject& newParent);

    // A dropped asset path (absolute, from the Content Browser) as a prefab key:
    // project-relative, normalized like the library's keys. nullopt when it is
    // not a .prefab or lies outside the project.
    std::optional<std::string> prefabKeyForAsset(const std::string& assetPath, const PrefabLibrary& lib);
}
