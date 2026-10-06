#pragma once
#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace DonTopo
{
    class AudioManager;
    class GameObject;
    class PhysicsManager;
    class PrefabLibrary;
    class Scene;
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
}
