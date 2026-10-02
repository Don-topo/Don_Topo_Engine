#include "DonTopo/Editor/PrefabOverrideTracker.h"

#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/PrefabLibrary.h"
#include "DonTopo/Core/PrefabOverrides.h"
#include "DonTopo/Core/Scene.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iterator>
#include <unordered_set>

namespace DonTopo
{
    namespace
    {
        uint64_t localIdOf(const nlohmann::json& n, const char* key)
        {
            const auto it = n.find(key);
            if (it == n.end()) return 0;
            if (it->is_number_unsigned()) return it->get<uint64_t>();
            if (it->is_number_integer())  { const int64_t v = it->get<int64_t>(); return v > 0 ? static_cast<uint64_t>(v) : 0; }
            return 0;
        }

        // The template a link may be diffed against: Ready and with content. A
        // Loading template can already hold its NEXT content (see the header).
        const PrefabTemplate* readyTemplateOf(const PrefabLibrary& lib, const GameObject* root)
        {
            if (!root || !root->prefabLink) return nullptr;
            const PrefabTemplate* t = lib.find(root->prefabLink->path);
            if (!t || t->state != PrefabTemplate::State::Ready || !t->memoryRoot.is_object()) return nullptr;
            return t;
        }

        std::vector<GameObject*> instanceRoots(Scene& scene)
        {
            std::vector<GameObject*> out;
            scene.traverse([&](GameObject* n) { if (n->prefabLink && n->parent) out.push_back(n); });
            return out;
        }

        // Numbers within 1e-5 (relative above 1): a value that went through a
        // float component and back is not a new override. Everything else exact.
        bool nearlyEqual(const nlohmann::json& a, const nlohmann::json& b)
        {
            if (a.is_number() && b.is_number())
            {
                const double x = a.get<double>(), y = b.get<double>();
                return std::fabs(x - y) <= 1e-5 * (std::max)(1.0, (std::max)(std::fabs(x), std::fabs(y)));
            }
            if (a.is_array() && b.is_array())
            {
                if (a.size() != b.size()) return false;
                for (size_t i = 0; i < a.size(); ++i) if (!nearlyEqual(a[i], b[i])) return false;
                return true;
            }
            if (a.is_object() && b.is_object())
            {
                if (a.size() != b.size()) return false;
                for (auto it = a.begin(); it != a.end(); ++it)
                {
                    const auto other = b.find(it.key());
                    if (other == b.end() || !nearlyEqual(it.value(), *other)) return false;
                }
                return true;
            }
            return a == b;
        }

        // Same multiset of (node, path, value), in any order.
        bool sameOverrides(const std::vector<PrefabOverride>& stored, const std::vector<PrefabOverride>& fresh)
        {
            if (stored.size() != fresh.size()) return false;
            std::vector<bool> used(stored.size(), false);
            for (const PrefabOverride& f : fresh)
            {
                bool found = false;
                for (size_t i = 0; i < stored.size() && !found; ++i)
                {
                    if (used[i] || stored[i].node != f.node || stored[i].path != f.path) continue;
                    if (!nearlyEqual(stored[i].value, f.value)) continue;
                    used[i] = found = true;
                }
                if (!found) return false;
            }
            return true;
        }

        std::string normalizedLinkPath(const std::string& path)
        {
            return std::filesystem::path(path).lexically_normal().generic_string();
        }

        // An override that changes WHICH mesh a node has (its file, piece or
        // animation sources): the template's configured mesh for that node no
        // longer describes it. Materials, visibility and the mesh name do not.
        bool touchesMeshIdentity(const std::string& path)
        {
            if (path == "/mesh") return true;
            if (path.rfind("/mesh/", 0) != 0) return false;
            return path.rfind("/mesh/materials", 0) != 0 && path != "/mesh/visible" && path != "/mesh/name";
        }
    }

    // ---------------------------------------------------------------- tracker

    bool PrefabOverrideTracker::isSynced(const GameObject* instanceRoot, const PrefabLibrary& lib) const
    {
        const PrefabTemplate* t = readyTemplateOf(lib, instanceRoot);
        if (!t) return false;
        const auto it = m_syncedGeneration.find(instanceRoot->id);
        return it != m_syncedGeneration.end() && it->second == t->generation;
    }

    int PrefabOverrideTracker::onRevisionChanged(Scene& scene, const PrefabLibrary& lib)
    {
        int changed = 0;
        for (GameObject* root : instanceRoots(scene))
        {
            if (!isSynced(root, lib)) continue;
            if (scene.recomputePrefabOverrides(root, readyTemplateOf(lib, root)->memoryRoot)) ++changed;
        }
        return changed;
    }

    int PrefabOverrideTracker::onItemDeactivated(Scene& scene, const PrefabLibrary& lib, GameObject* selected)
    {
        // The nearest instance root above (or at) the selection.
        GameObject* root = selected;
        while (root && root->parent && !root->prefabLink) root = root->parent;
        if (!root || !root->parent || !root->prefabLink || !isSynced(root, lib)) return 0;
        return scene.recomputePrefabOverrides(root, readyTemplateOf(lib, root)->memoryRoot) ? 1 : 0;
    }

    void PrefabOverrideTracker::markSynced(uint64_t instanceRootId, uint64_t generation)
    {
        m_syncedGeneration[instanceRootId] = generation;
        m_failedGeneration.erase(instanceRootId);
    }

    void PrefabOverrideTracker::markFailed(uint64_t instanceRootId, uint64_t generation)
    {
        m_syncedGeneration.erase(instanceRootId);
        m_failedGeneration[instanceRootId] = generation;
    }

    void PrefabOverrideTracker::forget(uint64_t instanceRootId)
    {
        m_syncedGeneration.erase(instanceRootId);
        m_failedGeneration.erase(instanceRootId);
    }

    std::vector<GameObject*> PrefabOverrideTracker::unsyncedInstances(Scene& scene, const PrefabLibrary& lib) const
    {
        std::vector<GameObject*> out;
        for (GameObject* root : instanceRoots(scene))
        {
            const PrefabTemplate* t = readyTemplateOf(lib, root);
            if (!t || isSynced(root, lib)) continue;
            // Already tried against this very generation and the rebuild failed:
            // retrying on every edit would only repeat the same warnings.
            if (const auto f = m_failedGeneration.find(root->id); f != m_failedGeneration.end() && f->second == t->generation)
                continue;
            out.push_back(root);
        }
        return out;
    }

    // ---------------------------------------------------------------- match

    bool prefabInstanceMatchesTemplate(const Scene& scene, const GameObject* instanceRoot,
                                       const nlohmann::json& prefabMemoryRoot)
    {
        if (!instanceRoot || !instanceRoot->prefabLink || !prefabMemoryRoot.is_object()) return false;

        struct TemplateNode
        {
            const nlohmann::json* json = nullptr;
            uint64_t parent = 0;
            std::vector<uint64_t> linkedChildren;
        };
        std::unordered_map<uint64_t, TemplateNode> tmpl;
        bool ok = true;
        std::function<void(const nlohmann::json&, uint64_t)> index = [&](const nlohmann::json& n, uint64_t parent) {
            const uint64_t id = localIdOf(n, "id");
            if (id == 0 || tmpl.count(id)) { ok = false; return; }
            TemplateNode& node = tmpl[id];
            node.json   = &n;
            node.parent = parent;
            if (const auto kids = n.find("children"); kids != n.end() && kids->is_array())
                for (const auto& c : *kids)
                {
                    if (!c.is_object()) continue;
                    tmpl[id].linkedChildren.push_back(localIdOf(c, "id"));
                    index(c, id);
                }
        };
        index(prefabMemoryRoot, 0);
        if (!ok) return false;

        // Memory form, no baseline: the form the template is in.
        const nlohmann::json inst = scene.subtreeToMemoryJson(instanceRoot);
        std::vector<PrefabOverride> fresh;
        std::unordered_set<uint64_t> seen;
        std::function<void(const nlohmann::json&, uint64_t, bool)> walk =
            [&](const nlohmann::json& n, uint64_t parentLocal, bool isRoot) {
                if (!ok) return;
                const uint64_t local = localIdOf(n, "prefabNode");
                const auto it = tmpl.find(local);
                if (local == 0 || !seen.insert(local).second || it == tmpl.end() || it->second.parent != parentLocal)
                {
                    ok = false;
                    return;
                }
                // Linked children in the template's order. Added children (no
                // prefabNode) are carried as they are by a propagation: skipped.
                std::vector<uint64_t> linked;
                if (const auto kids = n.find("children"); kids != n.end() && kids->is_array())
                    for (const auto& c : *kids)
                        if (const uint64_t cl = localIdOf(c, "prefabNode"); cl != 0) linked.push_back(cl);
                if (linked != it->second.linkedChildren) { ok = false; return; }

                auto d = diffPrefabNode(local, *it->second.json, n, isRoot);
                fresh.insert(fresh.end(), std::make_move_iterator(d.begin()), std::make_move_iterator(d.end()));
                if (const auto kids = n.find("children"); kids != n.end() && kids->is_array())
                    for (const auto& c : *kids)
                        if (localIdOf(c, "prefabNode") != 0) walk(c, local, false);
            };
        walk(inst, 0, true);
        return ok && seen.size() == tmpl.size() && sameOverrides(instanceRoot->prefabLink->overrides, fresh);
    }

    // ---------------------------------------------------------------- propagation

    GameObject* propagateInstance(Scene& scene, GameObject* instanceRoot, const PrefabTemplate& t,
                                  PrefabOverrideTracker& tracker, PhysicsManager& physics,
                                  AudioManager& audio, const RegisterSubtreeFn& registerGpu,
                                  PropagationResult* result)
    {
        if (!instanceRoot || !instanceRoot->parent || !instanceRoot->prefabLink ||
            t.state != PrefabTemplate::State::Ready || !t.memoryRoot.is_object())
            return instanceRoot;

        const uint64_t id = instanceRoot->id;
        bool meshPending = false;
        instanceRoot->traverse([&](GameObject* n) { if (n->pendingMeshJob != 0) meshPending = true; });
        if (meshPending)
        {
            if (result) ++result->deferred;
            return instanceRoot;
        }
        // Equal already: nothing is rebuilt (same objects, same GPU slots, and an
        // unchanged scene stays byte for byte the same).
        if (prefabInstanceMatchesTemplate(scene, instanceRoot, t.memoryRoot))
        {
            tracker.markSynced(id, t.generation);
            if (result) ++result->upToDate;
            return instanceRoot;
        }

        // The template, with each file-mesh node pointing at the mesh the library
        // already configured for it (memRef): nodeFromJson takes it as is, so no
        // node reconfigures its animation sources from disk. The material
        // overrides are applied on top by the rebuild, as on a disk load. A node
        // whose mesh identity the instance overrides goes the ordinary way.
        std::unordered_set<uint64_t> meshOverridden;
        for (const PrefabOverride& ov : instanceRoot->prefabLink->overrides)
            if (touchesMeshIdentity(ov.path)) meshOverridden.insert(ov.node);
        nlohmann::json tree = t.memoryRoot;
        std::function<void(nlohmann::json&)> seed = [&](nlohmann::json& n) {
            const uint64_t local = localIdOf(n, "id");
            if (const auto mesh = n.find("mesh"); local != 0 && mesh != n.end() && mesh->is_object() &&
                                                  !meshOverridden.count(local))
            {
                const auto src = mesh->find("sourcePath");
                const std::string ref = prefabNodeMeshRef(local);
                if (src != mesh->end() && src->is_string() && !src->get<std::string>().empty() &&
                    t.configuredMeshes.count(ref))
                    (*mesh)["memRef"] = ref;
            }
            if (auto kids = n.find("children"); kids != n.end() && kids->is_array())
                for (auto& c : *kids) if (c.is_object()) seed(c);
        };
        seed(tree);
        PreloadedMeshCache cache = t.meshes;
        for (const auto& [ref, mesh] : t.configuredMeshes) cache[ref] = mesh;

        bool rebuilt = false;
        GameObject* live = scene.rebuildPrefabInstance(instanceRoot, tree, physics, audio, &cache, &t.mats, &rebuilt);
        if (result)
            result->warnings.insert(result->warnings.end(), scene.lastWarnings().begin(), scene.lastWarnings().end());
        if (live && registerGpu) registerGpu(live);   // rebuilt OR restored: both lost their GPU slots
        if (live && rebuilt)
        {
            tracker.markSynced(id, t.generation);
            if (result) ++result->rebuilt;
        }
        else if (live)
        {
            // Restored as it was: it does NOT match the template, so it must not
            // be diffed against it either (and is not retried for this generation).
            tracker.markFailed(id, t.generation);
        }
        else
        {
            tracker.forget(id);
        }
        return live;
    }

    PropagationResult propagateReadyTemplate(Scene& scene, const PrefabTemplate& t,
                                             PrefabOverrideTracker& tracker, PhysicsManager& physics,
                                             AudioManager& audio, const RegisterSubtreeFn& registerGpu)
    {
        PropagationResult result;
        if (t.state != PrefabTemplate::State::Ready || !t.memoryRoot.is_object()) return result;
        // Ids first, then one rebuild at a time: a rebuild frees the old subtree,
        // so no pointer collected before it can be trusted afterwards.
        std::vector<uint64_t> ids;
        for (GameObject* root : instanceRoots(scene))
            if (normalizedLinkPath(root->prefabLink->path) == t.key) ids.push_back(root->id);
        for (uint64_t id : ids)
            if (GameObject* root = scene.findById(id))
                if (root->prefabLink) propagateInstance(scene, root, t, tracker, physics, audio, registerGpu, &result);
        return result;
    }
}
