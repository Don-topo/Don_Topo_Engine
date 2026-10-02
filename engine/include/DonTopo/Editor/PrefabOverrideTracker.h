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
            void clear() { m_syncedGeneration.clear(); m_failedGeneration.clear(); }

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
    };

    // True when the live subtree of instanceRoot is exactly what a propagation
    // would rebuild from prefabMemoryRoot: the same linked nodes in the same
    // places, and a diff equal to the stored overrides (numbers within 1e-5).
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
    // synced. Untouched (same object, same GPU slots) when it already matches;
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

    // propagateInstance for every instance of t in the scene (link path equal to
    // t.key). Ignores templates that are not Ready.
    PropagationResult propagateReadyTemplate(Scene& scene, const PrefabTemplate& t,
                                             PrefabOverrideTracker& tracker, PhysicsManager& physics,
                                             AudioManager& audio, const RegisterSubtreeFn& registerGpu);
}
