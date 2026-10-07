#include "DonTopo/Editor/PrefabOverrideTracker.h"

#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/PrefabLibrary.h"
#include "DonTopo/Core/PrefabOverrides.h"
#include "DonTopo/Core/Scene.h"

#include <algorithm>
#include <cctype>
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

        // Whether the live value of a folded edit equals the template node's value
        // at the SAME path, resolved inside the template node (diffs report at
        // different depths: a whole "/light" when one side lacks the component,
        // a leaf "/light/intensity" when both have it). A path the template lacks,
        // or a removal (null) of something it has, differs. "/transform/*" is
        // virtual (a decomposition of localTransform): for those, the diff of the
        // live node against the template already says it (vsTemplate).
        // Whether the template node has anything at path ("/transform/*" always exists).
        // Virtual paths: not a value stored in the node but derived from it
        // ("/transform/*" from localTransform, "/scripts/#order" from the order
        // of the scripts). For those the diff of the live node against the
        // template (vsTemplate) is what says whether the live state is the template's.
        bool isVirtualPath(const std::string& path)
        {
            static const std::string kOrderSuffix = "/#order";
            return path.rfind("/transform/", 0) == 0 ||
                   (path.size() >= kOrderSuffix.size() &&
                    path.compare(path.size() - kOrderSuffix.size(), kOrderSuffix.size(), kOrderSuffix) == 0);
        }

        bool templateHas(const nlohmann::json& templateNode, const std::string& path)
        {
            if (isVirtualPath(path)) return true;
            // Keyed segments (scripts by name, material slots by index) resolved.
            return prefabValueAt(templateNode, path).has_value();
        }

        bool equalsTemplateAt(const nlohmann::json& templateNode, const PrefabOverride& edit,
                              const std::vector<PrefabOverride>& vsTemplate)
        {
            if (isVirtualPath(edit.path))
                return std::none_of(vsTemplate.begin(), vsTemplate.end(),
                                    [&](const PrefabOverride& v) { return v.path == edit.path; });
            const std::optional<nlohmann::json> theirs = prefabValueAt(templateNode, edit.path);
            if (!theirs || edit.value.is_null()) return false;
            return prefabValuesEqual(*theirs, edit.value);
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

    bool prefabEditIsRevert(const nlohmann::json& templateNode, const nlohmann::json& liveNode,
                            const PrefabOverride& edit, bool isRoot)
    {
        // Removing something the template does not have either (a component
        // the instance itself had added): back to the template there.
        if (edit.value.is_null() && !templateHas(templateNode, edit.path)) return true;
        const std::vector<PrefabOverride> vsTemplate = diffPrefabNode(edit.node, templateNode, liveNode, isRoot);
        return equalsTemplateAt(templateNode, edit, vsTemplate);
    }

    nlohmann::json seedConfiguredMeshes(const nlohmann::json& tmpl, const nlohmann::json& configuredFor,
                                        const std::unordered_map<std::string, std::shared_ptr<const Mesh>>& configured,
                                        const std::vector<PrefabOverride>& instanceOverrides)
    {
        std::unordered_set<uint64_t> meshOverridden;
        for (const PrefabOverride& ov : instanceOverrides)
            if (touchesMeshIdentity(ov.path)) meshOverridden.insert(ov.node);
        // The "mesh" of each node of the template the meshes were configured for.
        const bool sameTemplate = &tmpl == &configuredFor;
        std::unordered_map<uint64_t, const nlohmann::json*> configuredMesh;
        if (!sameTemplate)
        {
            std::function<void(const nlohmann::json&)> index = [&](const nlohmann::json& n) {
                if (const uint64_t id = localIdOf(n, "id"); id != 0)
                    if (const auto m = n.find("mesh"); m != n.end()) configuredMesh.emplace(id, &*m);
                if (const auto kids = n.find("children"); kids != n.end() && kids->is_array())
                    for (const auto& c : *kids) if (c.is_object()) index(c);
            };
            index(configuredFor);
        }
        nlohmann::json tree = tmpl;
        std::function<void(nlohmann::json&)> seed = [&](nlohmann::json& n) {
            const uint64_t local = localIdOf(n, "id");
            if (const auto mesh = n.find("mesh"); local != 0 && mesh != n.end() && mesh->is_object() &&
                                                  !meshOverridden.count(local))
            {
                const auto src = mesh->find("sourcePath");
                const std::string ref = prefabNodeMeshRef(local);
                bool sameMesh = sameTemplate;
                if (!sameMesh)
                    if (const auto c = configuredMesh.find(local); c != configuredMesh.end())
                        sameMesh = *c->second == *mesh;
                if (sameMesh && src != mesh->end() && src->is_string() && !src->get<std::string>().empty() &&
                    configured.count(ref))
                    (*mesh)["memRef"] = ref;
            }
            if (auto kids = n.find("children"); kids != n.end() && kids->is_array())
                for (auto& c : *kids) if (c.is_object()) seed(c);
        };
        seed(tree);
        return tree;
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
                const auto t = tmpl.find(local);
                for (PrefabOverride& e : diffPrefabNode(local, *it->second, n, isRoot))
                {
                    // The mesh landing from the async loader is not an edit.
                    if (meshPath(e.path) && inFlight(local)) continue;
                    // Back to the template's value, or removing something the
                    // template does not have either (a component the instance
                    // itself had added): the stored override goes; folding a
                    // null over it would leave an orphan removal that the
                    // rebuild reports as "no longer in the prefab".
                    if (t != tmpl.end() && prefabEditIsRevert(*t->second, n, e, isRoot))
                        reverts.emplace_back(local, e.path);
                    else
                        edits.push_back(std::move(e));
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

    int PrefabOverrideTracker::meshInFlightCount() const
    {
        int n = 0;
        for (const auto& [id, b] : m_baseline) n += static_cast<int>(b.meshInFlight.size());
        return n;
    }

    int PrefabOverrideTracker::settleLandedMeshes(Scene& scene)
    {
        int settled = 0;
        // Nothing waited for (the steady state): no scene walk at all.
        bool anyInFlight = false;
        for (const auto& [rootId, b] : m_baseline)
            if (!b.meshInFlight.empty()) { anyInFlight = true; break; }
        if (!anyInFlight) return 0;

        // ONE walk for every waiting instance, not a findById (a full walk) each.
        ++m_settleScans;
        std::unordered_map<uint64_t, GameObject*> rootsById;
        for (const auto& [rootId, b] : m_baseline)
            if (!b.meshInFlight.empty()) rootsById.emplace(rootId, nullptr);
        scene.traverse([&](GameObject* n) {
            if (auto it = rootsById.find(n->id); it != rootsById.end()) it->second = n;
        });

        for (auto& [rootId, b] : m_baseline)
        {
            if (b.meshInFlight.empty()) continue;
            GameObject* root = rootsById[rootId];
            if (!root)
            {
                // Deleted while its mesh was in flight. The undo of Delete brings
                // it back without a mesh and without a request: nothing to wait
                // for, and waiting would cost a scene walk every frame.
                settled += static_cast<int>(b.meshInFlight.size());
                b.meshInFlight.clear();
                continue;
            }
            std::unordered_map<uint64_t, GameObject*> byLocal;
            root->traverse([&](GameObject* n) { if (n->prefabNode != 0) byLocal.emplace(n->prefabNode, n); });

            // The baseline node of each prefab-local id.
            std::unordered_map<uint64_t, nlohmann::json*> baseNodes;
            std::function<void(nlohmann::json&)> index = [&](nlohmann::json& n) {
                if (const uint64_t local = localIdOf(n, "prefabNode"); local != 0) baseNodes.emplace(local, &n);
                if (auto kids = n.find("children"); kids != n.end() && kids->is_array())
                    for (auto& c : *kids) if (c.is_object()) index(c);
            };
            index(b.live);

            std::erase_if(b.meshInFlight, [&](uint64_t local) {
                const auto live = byLocal.find(local);
                if (live == byLocal.end()) return true;   // node gone: nothing to wait for
                GameObject* n = live->second;
                if (n->pendingMeshJob != 0 && (!m_meshRequestLive || m_meshRequestLive(n->pendingMeshJob)))
                    return false;                          // still on its way
                if (const auto base = baseNodes.find(local); base != baseNodes.end())
                {
                    // Only this node's own "mesh" (its children travel separately).
                    const nlohmann::json now = scene.subtreeToMemoryJson(n);
                    if (const auto mesh = now.find("mesh"); mesh != now.end())
                        (*base->second)["mesh"] = *mesh;
                    else
                        base->second->erase("mesh");
                }
                ++settled;
                return true;
            });
        }
        return settled;
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
        const nlohmann::json tree = seedConfiguredMeshes(t.memoryRoot, t.memoryRoot, t.configuredMeshes,
                                                         instanceRoot->prefabLink->overrides);
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

    // ── Editor UI decisions ──────────────────────────────────────────────────

    const GameObject* prefabInstanceRoot(const GameObject* node)
    {
        // The scene root (no parent) never carries a link; an instance root
        // always has a parent.
        for (const GameObject* n = node; n && n->parent; n = n->parent)
            if (n->prefabLink) return n;
        return nullptr;
    }

    GameObject* prefabInstanceRoot(GameObject* node)
    {
        return const_cast<GameObject*>(prefabInstanceRoot(static_cast<const GameObject*>(node)));
    }

    PrefabNodeState prefabNodeState(const GameObject& node, const PrefabLibrary* lib)
    {
        // Only nodes that come from the prefab are colored: an added child has
        // no prefabNode, and neither has anything under it.
        if (node.prefabNode == 0 && !node.prefabLink) return PrefabNodeState::None;
        const GameObject* root = prefabInstanceRoot(&node);
        if (!root) return PrefabNodeState::None;   // a stale prefabNode outside any instance
        if (!lib) return PrefabNodeState::Instance;
        const PrefabTemplate* t = lib->find(root->prefabLink->path);
        if (!t || t->state == PrefabTemplate::State::Failed) return PrefabNodeState::Missing;
        return PrefabNodeState::Instance;
    }

    namespace
    {
        // a == b, or b lies inside a, by JSON-pointer segments.
        bool pathCovers(std::string_view a, std::string_view b)
        {
            if (a.size() > b.size() || b.compare(0, a.size(), a) != 0) return false;
            return a.size() == b.size() || b[a.size()] == '/';
        }

        // The value at `pointer` inside j, nullptr when absent (or not a valid pointer).
        const nlohmann::json* valueAt(const nlohmann::json& j, std::string_view pointer)
        {
            if (pointer.empty()) return &j;
            try
            {
                const nlohmann::json::json_pointer p{std::string(pointer)};
                if (!j.contains(p)) return nullptr;
                return &j.at(p);
            }
            catch (const std::exception&)
            {
                return nullptr;
            }
        }

        enum class Coverage { None, AtOrInside, Ancestor };
        Coverage coverage(const PrefabOverride& o, uint64_t prefabNode, std::string_view prefix)
        {
            if (o.node != prefabNode) return Coverage::None;
            if (pathCovers(prefix, o.path)) return Coverage::AtOrInside;
            if (pathCovers(o.path, prefix)) return Coverage::Ancestor;
            return Coverage::None;
        }
    }

    bool isOverridden(const GameObject& instRoot, uint64_t prefabNode, std::string_view pathPrefix,
                      const nlohmann::json* templateNode)
    {
        if (!instRoot.prefabLink || prefabNode == 0) return false;
        for (const PrefabOverride& o : instRoot.prefabLink->overrides)
        {
            const Coverage c = coverage(o, prefabNode, pathPrefix);
            if (c == Coverage::AtOrInside) return true;
            if (c != Coverage::Ancestor) continue;
            if (!templateNode) return true;
            // The override holds the whole value at o.path (an added component
            // or script, a whole-value array): compare what lies at pathPrefix
            // inside it with the template's at the same path (keyed segments
            // resolved: scripts by name + occurrence, material slots by index).
            const nlohmann::json* mine = valueAt(o.value, pathPrefix.substr(o.path.size()));
            const std::optional<nlohmann::json> theirs = prefabValueAt(*templateNode, pathPrefix);
            if (!mine && !theirs) continue;
            if (!mine || !theirs || !prefabValuesEqual(*mine, *theirs)) return true;
        }
        return false;
    }

    std::vector<PrefabOverride> overridesUnder(const GameObject& instRoot, uint64_t prefabNode,
                                               std::string_view pathPrefix)
    {
        std::vector<PrefabOverride> out;
        if (!instRoot.prefabLink || prefabNode == 0) return out;
        for (const PrefabOverride& o : instRoot.prefabLink->overrides)
            if (coverage(o, prefabNode, pathPrefix) != Coverage::None) out.push_back(o);
        return out;
    }

    namespace
    {
        const nlohmann::json* findTemplateNodeById(const nlohmann::json& n, uint64_t local)
        {
            if (!n.is_object()) return nullptr;
            if (const auto it = n.find("id"); it != n.end() && it->is_number_integer() && it->get<uint64_t>() == local)
                return &n;
            if (const auto kids = n.find("children"); kids != n.end() && kids->is_array())
                for (const auto& c : *kids)
                    if (const nlohmann::json* f = findTemplateNodeById(c, local)) return f;
            return nullptr;
        }

        // "/scripts/<key>": one whole script element (keyed array).
        bool isScriptElementPath(const std::string& path)
        {
            static const std::string kPrefix = "/scripts/";
            return path.size() > kPrefix.size() && path.compare(0, kPrefix.size(), kPrefix) == 0 &&
                   path.find('/', kPrefix.size()) == std::string::npos && path != "/scripts/#order";
        }
    }

    std::string prefabScriptPath(const std::vector<std::string>& scriptNames, size_t index)
    {
        if (index >= scriptNames.size()) return {};
        size_t occurrence = 0;
        for (size_t k = 0; k < index; ++k)
            if (scriptNames[k] == scriptNames[index]) ++occurrence;
        return (nlohmann::json::json_pointer("/scripts") / prefabScriptKey(scriptNames[index], occurrence)).to_string();
    }

    PrefabFieldTarget prefabFieldTarget(const GameObject& instRoot, uint64_t node, std::string_view path,
                                        const nlohmann::json* templateNode)
    {
        PrefabFieldTarget out;
        if (!instRoot.prefabLink || node == 0) return out;
        const std::vector<PrefabOverride>& stored = instRoot.prefabLink->overrides;
        bool any = false;
        for (const PrefabOverride& o : stored)
        {
            const Coverage c = coverage(o, node, path);
            if (c == Coverage::None) continue;
            any = true;
            if (!templateNode || o.value.is_null() || prefabValueAt(*templateNode, o.path)) continue;
            // At or inside something the template does not have: an added
            // script (one keyed element of "/scripts")...
            if (isScriptElementPath(o.path))
            {
                out.kind = PrefabFieldTarget::Kind::AddedScript;
                out.wide = o;
                out.name = o.value.is_object() ? o.value.value("name", std::string()) : std::string();
                return out;
            }
            // ...or an added component (a single-segment path).
            if (o.path.find('/', 1) == std::string::npos)
            {
                out.kind = PrefabFieldTarget::Kind::AddedComponent;
                out.wide = o;
                out.name = o.path.substr(1);
                return out;
            }
        }
        if (any && isOverridden(instRoot, node, path, templateNode)) out.kind = PrefabFieldTarget::Kind::Field;
        return out;
    }

    std::vector<PrefabSelection> resolvePrefabSelection(const std::vector<PrefabOverride>& stored,
                                                        const std::vector<PrefabOverride>& which,
                                                        const nlohmann::json* templateRoot)
    {
        std::vector<PrefabSelection> out;
        auto has = [&](uint64_t node, const std::string& path) {
            return std::any_of(out.begin(), out.end(), [&](const PrefabSelection& s) {
                return s.override.node == node && s.override.path == path;
            });
        };
        for (const PrefabOverride& w : which)
        {
            bool found = false;
            for (const PrefabOverride& o : stored)
                if (coverage(o, w.node, w.path) == Coverage::AtOrInside)
                {
                    found = true;
                    if (!has(o.node, o.path)) out.push_back({ o, false, {} });
                }
            if (found || !templateRoot) continue;
            // A covering entry (a whole-value array like a color or the mesh's
            // animation sources): narrowed to w.path -- only where the template
            // has what w.path lives in (inside an added component or script
            // there is nothing to narrow into).
            for (const PrefabOverride& o : stored)
            {
                if (coverage(o, w.node, w.path) != Coverage::Ancestor) continue;
                const nlohmann::json* tmplNode = findTemplateNodeById(*templateRoot, w.node);
                if (!tmplNode || has(w.node, w.path)) break;
                const size_t slash = w.path.rfind('/');
                if (slash != std::string::npos && slash > 0 && !prefabValueAt(*tmplNode, w.path.substr(0, slash))) break;
                const nlohmann::json* v = valueAt(o.value, std::string_view(w.path).substr(o.path.size()));
                out.push_back({ PrefabOverride{ w.node, w.path, v ? *v : nlohmann::json(nullptr) }, true, w.path });
                break;
            }
        }
        return out;
    }

    PrefabRevertPlan revertPrefabSelection(std::vector<PrefabOverride>& stored, const std::vector<PrefabOverride>& which,
                                           const nlohmann::json& templateRoot)
    {
        PrefabRevertPlan plan;
        for (const PrefabOverride& w : which)
        {
            bool found = false;
            for (auto it = stored.begin(); it != stored.end();)
            {
                if (coverage(*it, w.node, w.path) == Coverage::AtOrInside)
                {
                    found = true;
                    plan.removed.push_back(*it);
                    it = stored.erase(it);
                }
                else ++it;
            }
            if (found) continue;
            const nlohmann::json* tmplNode = findTemplateNodeById(templateRoot, w.node);
            if (!tmplNode) continue;
            for (auto it = stored.begin(); it != stored.end(); ++it)
            {
                if (coverage(*it, w.node, w.path) != Coverage::Ancestor) continue;
                // Inside something the template does not have (an added script
                // or component): nothing to revert the field to.
                if (!prefabValueAt(*tmplNode, it->path)) break;
                const PrefabOverride before = *it;
                nlohmann::json value = it->value;
                const nlohmann::json::json_pointer sub{ w.path.substr(it->path.size()) };
                const std::optional<nlohmann::json> theirs = prefabValueAt(*tmplNode, w.path);
                try
                {
                    if (theirs) value[sub] = *theirs;
                    else if (value.contains(sub))
                    {
                        nlohmann::json& parent = value[sub.parent_pointer()];
                        if (parent.is_object()) parent.erase(sub.back());
                        else if (parent.is_array()) parent.erase(std::stoul(sub.back()));
                    }
                }
                catch (const std::exception&)
                {
                    break;
                }
                if (value == before.value) break;
                // Equal to the template at its own path now: no override left.
                const std::optional<nlohmann::json> own = prefabValueAt(*tmplNode, before.path);
                if (own && prefabValuesEqual(*own, value))
                {
                    plan.removed.push_back(before);
                    stored.erase(it);
                }
                else
                {
                    it->value = std::move(value);
                    plan.rewritten.emplace_back(before, *it);
                }
                break;
            }
        }
        return plan;
    }

    void restoreRewrittenOverrides(std::vector<PrefabOverride>& list,
                                   const std::vector<std::pair<PrefabOverride, PrefabOverride>>& rewritten)
    {
        for (const auto& [before, after] : rewritten)
        {
            const auto it = std::find_if(list.begin(), list.end(), [&](const PrefabOverride& o) {
                return o.node == before.node && o.path == before.path;
            });
            if (it != list.end() && prefabValuesEqual(it->value, after.value)) it->value = before.value;
        }
    }

    std::filesystem::path uniquePrefabPath(const std::filesystem::path& dir, const std::string& name)
    {
        std::string base;
        base.reserve(name.size());
        for (const char ch : name)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            const bool bad = c < 32 || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' ||
                             ch == '\\' || ch == '|' || ch == '?' || ch == '*';
            base.push_back(bad ? '_' : ch);
        }
        // Windows drops trailing spaces and dots from a file name: trimmed here
        // so the name on disk is the one the editor shows.
        const size_t first = base.find_first_not_of(' ');
        const size_t last  = base.find_last_not_of(" .");
        base = (first == std::string::npos || last == std::string::npos || last < first)
                   ? std::string() : base.substr(first, last - first + 1);
        if (base.empty()) base = "Prefab";

        std::error_code ec;
        std::filesystem::path candidate = dir / (base + ".prefab");
        for (int i = 1; std::filesystem::exists(candidate, ec); ++i)
            candidate = dir / (base + " " + std::to_string(i) + ".prefab");
        return candidate;
    }

    bool canDeleteNode(const GameObject& node)
    {
        if (!node.parent) return false;
        if (node.prefabLink || node.prefabNode == 0) return true;
        return prefabInstanceRoot(&node) == nullptr;
    }

    std::string prefabMoveRefusal(const GameObject& dragged, const GameObject& newParent)
    {
        if (!canDeleteNode(dragged) && dragged.parent)
            return "'" + dragged.name + "' comes from a prefab: Unpack the prefab to move this object";
        bool carriesInstance = false;
        const_cast<GameObject&>(dragged).traverse([&](GameObject* n) { if (n->prefabLink) carriesInstance = true; });
        if (carriesInstance && prefabInstanceRoot(&newParent) != nullptr)
            return "'" + dragged.name + "' contains a prefab instance and cannot go inside another one "
                   "(nested prefabs are not supported)";
        return {};
    }

    std::optional<std::string> prefabKeyForAsset(const std::string& assetPath, const PrefabLibrary& lib)
    {
        if (assetPath.empty()) return std::nullopt;
        std::string ext = std::filesystem::path(assetPath).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".prefab") return std::nullopt;
        const std::string key = lib.normalizeKey(assetPath);
        // normalizeKey leaves a path outside the asset root absolute.
        const std::filesystem::path k(key);
        if (key.empty() || k.is_absolute() || k.has_root_name() || key.rfind("..", 0) == 0) return std::nullopt;
        return key;
    }
}
