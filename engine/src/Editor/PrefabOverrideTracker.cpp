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

        bool nearlyEqual(const nlohmann::json& a, const nlohmann::json& b) { return prefabValuesEqual(a, b); }

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

    bool prefabValuesEqual(const nlohmann::json& a, const nlohmann::json& b)
    {
        // A tolerance only where a float is involved: a value that went through a
        // float component and back is not a new override. Two integers (ids,
        // masks, counts) compare exactly: 1e-5 relative would make 0xFFFFFFFF
        // equal to 0xFFFFFFFE and two 64-bit ids indistinguishable.
        if (a.is_number() && b.is_number())
        {
            if (!a.is_number_float() && !b.is_number_float())
            {
                if (a.is_number_unsigned() && b.is_number_unsigned()) return a.get<uint64_t>() == b.get<uint64_t>();
                if (a.is_number_unsigned() || b.is_number_unsigned())
                {
                    const nlohmann::json& u = a.is_number_unsigned() ? a : b;
                    const nlohmann::json& s = a.is_number_unsigned() ? b : a;
                    return s.get<int64_t>() >= 0 && static_cast<uint64_t>(s.get<int64_t>()) == u.get<uint64_t>();
                }
                return a.get<int64_t>() == b.get<int64_t>();
            }
            const double x = a.get<double>(), y = b.get<double>();
            return std::fabs(x - y) <= 1e-5 * (std::max)(1.0, (std::max)(std::fabs(x), std::fabs(y)));
        }
        if (a.is_array() && b.is_array())
        {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) if (!prefabValuesEqual(a[i], b[i])) return false;
            return true;
        }
        if (a.is_object() && b.is_object())
        {
            if (a.size() != b.size()) return false;
            for (auto it = a.begin(); it != a.end(); ++it)
            {
                const auto other = b.find(it.key());
                if (other == b.end() || !prefabValuesEqual(it.value(), *other)) return false;
            }
            return true;
        }
        return a == b;
    }

    // ---------------------------------------------------------------- tracker

    void PrefabOverrideTracker::captureBaseline(Scene& scene, GameObject* instanceRoot,
                                                const nlohmann::json* templateRoot)
    {
        if (!instanceRoot || !instanceRoot->prefabLink) return;
        Baseline b;
        b.live = scene.subtreeToMemoryJson(instanceRoot);
        // Nodes whose file mesh is still on its way (Load Scene): nodeToJson
        // writes no "mesh" for them, so the mesh landing later would read as an
        // edit of the whole mesh. Only linked nodes matter (the fold walks those).
        instanceRoot->traverse([&](GameObject* n) {
            if (n->prefabNode != 0 && n->pendingMeshJob != 0 &&
                (!m_meshRequestLive || m_meshRequestLive(n->pendingMeshJob)))
                b.meshInFlight.push_back(n->prefabNode);
        });
        if (templateRoot && templateRoot->is_object()) b.templateRoot = *templateRoot;
        m_baseline[instanceRoot->id] = std::move(b);
    }

    int PrefabOverrideTracker::foldUnrecordedEdits(Scene& scene, GameObject* instanceRoot)
    {
        if (!instanceRoot || !instanceRoot->prefabLink) return 0;
        const auto base = m_baseline.find(instanceRoot->id);
        if (base == m_baseline.end()) return 0;

        // Linked nodes of the baseline, by prefab-local id.
        std::unordered_map<uint64_t, const nlohmann::json*> before;
        std::function<void(const nlohmann::json&)> index = [&](const nlohmann::json& n) {
            const uint64_t local = localIdOf(n, "prefabNode");
            if (local == 0) return;   // added children travel whole; not overrides
            before.emplace(local, &n);
            if (const auto kids = n.find("children"); kids != n.end() && kids->is_array())
                for (const auto& c : *kids) if (c.is_object()) index(c);
        };
        const Baseline& baseline = base->second;
        index(baseline.live);

        // The template the baseline stood on, by prefab-local id ("id" there).
        std::unordered_map<uint64_t, const nlohmann::json*> tmpl;
        std::function<void(const nlohmann::json&)> indexTmpl = [&](const nlohmann::json& n) {
            if (const uint64_t id = localIdOf(n, "id"); id != 0) tmpl.emplace(id, &n);
            if (const auto kids = n.find("children"); kids != n.end() && kids->is_array())
                for (const auto& c : *kids) if (c.is_object()) indexTmpl(c);
        };
        if (baseline.templateRoot.is_object()) indexTmpl(baseline.templateRoot);

        auto meshPath = [](const std::string& p) { return p == "/mesh" || p.rfind("/mesh/", 0) == 0; };
        auto inFlight = [&](uint64_t local) {
            return std::find(baseline.meshInFlight.begin(), baseline.meshInFlight.end(), local) !=
                   baseline.meshInFlight.end();
        };

        // The same diff the tracker records, only against the baseline: what
        // changed since, as overrides (value = the live value; null = removed).
        // Reverts -- a live value equal to the baseline TEMPLATE's at that path --
        // are collected apart: they remove the stored override instead.
        std::vector<PrefabOverride> edits;
        std::vector<std::pair<uint64_t, std::string>> reverts;
        auto& overridesRef = instanceRoot->prefabLink->overrides;
        const nlohmann::json live = scene.subtreeToMemoryJson(instanceRoot);
        std::function<void(const nlohmann::json&, bool)> walk = [&](const nlohmann::json& n, bool isRoot) {
            const uint64_t local = localIdOf(n, "prefabNode");
            if (local == 0) return;
            if (const auto it = before.find(local); it != before.end())
            {
                std::vector<PrefabOverride> vsTemplate;
                const auto t = tmpl.find(local);
                if (t != tmpl.end()) vsTemplate = diffPrefabNode(local, *t->second, n, isRoot);
                for (PrefabOverride& e : diffPrefabNode(local, *it->second, n, isRoot))
                {
                    // The mesh landing from the async loader is not an edit.
                    if (meshPath(e.path) && inFlight(local)) continue;
                    const bool differsFromTemplate =
                        t == tmpl.end() ||
                        std::any_of(vsTemplate.begin(), vsTemplate.end(),
                                    [&](const PrefabOverride& v) { return v.path == e.path; });
                    if (differsFromTemplate) edits.push_back(std::move(e));
                    else                     reverts.emplace_back(local, e.path);
                }
            }
            if (const auto kids = n.find("children"); kids != n.end() && kids->is_array())
                for (const auto& c : *kids) if (c.is_object()) walk(c, false);
        };
        walk(live, true);

        for (const auto& [node, path] : reverts)
            std::erase_if(overridesRef, [&](const PrefabOverride& o) { return o.node == node && o.path == path; });
        for (PrefabOverride& e : edits)
        {
            auto same = std::find_if(overridesRef.begin(), overridesRef.end(), [&](const PrefabOverride& o) {
                return o.node == e.node && o.path == e.path;
            });
            if (same != overridesRef.end()) same->value = std::move(e.value);
            else                            overridesRef.push_back(std::move(e));
        }
        return static_cast<int>(edits.size() + reverts.size());
    }

    bool PrefabOverrideTracker::meshPending(const GameObject* instanceRoot) const
    {
        if (!instanceRoot) return false;
        bool pending = false;
        const_cast<GameObject*>(instanceRoot)->traverse([&](GameObject* n) {
            if (n->pendingMeshJob != 0 && (!m_meshRequestLive || m_meshRequestLive(n->pendingMeshJob)))
                pending = true;
        });
        return pending;
    }

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
            const nlohmann::json& tmpl = readyTemplateOf(lib, root)->memoryRoot;
            if (scene.recomputePrefabOverrides(root, tmpl)) ++changed;
            // Recorded: from here on only NEWER edits are unrecorded.
            captureBaseline(scene, root, &tmpl);
        }
        return changed;
    }

    int PrefabOverrideTracker::onItemDeactivated(Scene& scene, const PrefabLibrary& lib, GameObject* selected)
    {
        // The nearest instance root above (or at) the selection.
        GameObject* root = selected;
        while (root && root->parent && !root->prefabLink) root = root->parent;
        if (!root || !root->parent || !root->prefabLink || !isSynced(root, lib)) return 0;
        const nlohmann::json& tmpl = readyTemplateOf(lib, root)->memoryRoot;
        const bool changed = scene.recomputePrefabOverrides(root, tmpl);
        captureBaseline(scene, root, &tmpl);
        return changed ? 1 : 0;
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
        m_baseline.erase(instanceRootId);
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
        if (tracker.meshPending(instanceRoot))
        {
            if (result) ++result->deferred;
            return instanceRoot;
        }
        // Edits made while the instance could not be diffed (template loading,
        // not synced yet) become overrides BEFORE anything is compared or
        // rebuilt: otherwise the rebuild would re-derive the subtree without them.
        tracker.foldUnrecordedEdits(scene, instanceRoot);

        // Equal already: nothing is rebuilt (same objects, same GPU slots, and an
        // unchanged scene stays byte for byte the same).
        if (prefabInstanceMatchesTemplate(scene, instanceRoot, t.memoryRoot))
        {
            tracker.markSynced(id, t.generation);
            tracker.captureBaseline(scene, instanceRoot, &t.memoryRoot);
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
            tracker.captureBaseline(scene, live, &t.memoryRoot);
            if (result) ++result->rebuilt;
        }
        else if (live)
        {
            // Restored as it was: it does NOT match the template, so it must not
            // be diffed against it either (and is not retried for this generation).
            // Its overrides already hold the folded edits, so its baseline moves on.
            tracker.markFailed(id, t.generation);
            tracker.captureBaseline(scene, live);
        }
        else
        {
            tracker.forget(id);
        }
        return live;
    }

    PropagationResult propagateReadyTemplate(Scene& scene, const PrefabLibrary& lib, const PrefabTemplate& t,
                                             PrefabOverrideTracker& tracker, PhysicsManager& physics,
                                             AudioManager& audio, const RegisterSubtreeFn& registerGpu)
    {
        PropagationResult result;
        if (t.state != PrefabTemplate::State::Ready || !t.memoryRoot.is_object()) return result;
        // Ids first, then one rebuild at a time: a rebuild frees the old subtree,
        // so no pointer collected before it can be trusted afterwards.
        std::vector<uint64_t> ids;
        for (GameObject* root : instanceRoots(scene))
            if (lib.normalizeKey(root->prefabLink->path) == t.key) ids.push_back(root->id);
        for (uint64_t id : ids)
            if (GameObject* root = scene.findById(id))
                if (root->prefabLink) propagateInstance(scene, root, t, tracker, physics, audio, registerGpu, &result);
        return result;
    }
}
