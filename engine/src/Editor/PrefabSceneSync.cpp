#include "DonTopo/Editor/PrefabSceneSync.h"

#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/PrefabLibrary.h"
#include "DonTopo/Core/PrefabOverrides.h"
#include "DonTopo/Core/Scene.h"

#include <exception>

namespace DonTopo
{
    PrefabSceneSync::PrefabSceneSync(PrefabLibrary& lib) : m_lib(lib)
    {
        m_lib.setOnReady([this](const PrefabTemplate& t) { onTemplateReady(t); });
    }

    PrefabSceneSync::~PrefabSceneSync()
    {
        // The library may outlive this object by a few lines (EditorUI releases
        // them in order): it must not call back into a dead one.
        m_lib.setOnReady(nullptr);
    }

    void PrefabSceneSync::setHost(Host host)
    {
        m_host = std::move(host);
        m_tracker.setMeshRequestLive(m_host.meshRequestLive);
    }

    void PrefabSceneSync::pump(float budgetMs)
    {
        if (!m_host.audio) return;
        // Top of the editor frame: async meshes applied since the last one become
        // part of their instances' baselines BEFORE any panel can edit them (an
        // edit of a landed mesh is then folded instead of reverted), and before
        // onReady below may propagate.
        if (m_host.scene) m_tracker.settleLandedMeshes(*m_host.scene);
        m_lib.pump(*m_host.audio, budgetMs);
        logFailed();
    }

    void PrefabSceneSync::onTemplateReady(const PrefabTemplate& t)
    {
        // Non-fatal problems of the template: reported once, never per instance.
        for (const std::string& w : t.warnings)
            if (m_host.log) m_host.log("[Prefab] " + t.key + ": " + w);
        m_reportedFailures.erase(t.key);
        m_known.insert(t.key);
        for (const std::string& ref : PrefabLibrary::collectReferences(t.memoryRoot))
            m_known.insert(m_lib.normalizeKey(ref));

        // GPU: every Instantiate of this template hits the shared caches. A pin
        // that throws (VRAM exhausted) must not escape: it would unwind through
        // PrefabLibrary::pump mid-batch and the editor frame mid-draw. The
        // template stays usable; its instances simply upload on registration.
        if (m_host.pin)
        {
            try
            {
                m_host.pin(t.key, prefabTemplateGpuMeshes(t));
                m_pinned.insert(t.key);
            }
            catch (const std::exception& e)
            {
                if (m_host.log) m_host.log("[Prefab] could not pin '" + t.key + "' in the renderer: " + e.what());
            }
            catch (...)
            {
                if (m_host.log) m_host.log("[Prefab] could not pin '" + t.key + "' in the renderer");
            }
        }

        // Play Mode: no propagation (spec §3.3); the Stop restore re-checks.
        if (!playing())
            propagate(t);
    }

    void PrefabSceneSync::absorb(PropagationResult& r)
    {
        if (m_host.log)
            for (const std::string& w : r.warnings) m_host.log("[Prefab] " + w);
        if (r.deferred > 0) m_syncPending = true;
        m_rebuilt += r.rebuilt;
    }

    void PrefabSceneSync::propagate(const PrefabTemplate& t)
    {
        if (!ready()) return;
        PropagationResult r = propagateReadyTemplate(*m_host.scene, m_lib, t, m_tracker, *m_host.physics,
                                                     *m_host.audio, m_host.registerGpu);
        absorb(r);
        if (r.rebuilt > 0 && m_host.log)
            m_host.log("[Prefab] " + t.key + ": " + std::to_string(r.rebuilt) +
                       " instance(s) updated from the prefab");
    }

    void PrefabSceneSync::sceneLoaded(const nlohmann::json& sceneJson)
    {
        if (!m_host.scene) return;
        Scene& scene = *m_host.scene;
        // The new scene's ids may repeat the old ones; none of its instances has
        // been checked against a template yet.
        m_tracker.clear();

        // What the scene needs: its scripts' references, its instances' links,
        // and (transitively) what Ready templates reference -- a template's own
        // references are requested by the library when it gets Ready, so
        // evicting them here would orphan Enemy -> Bullet.
        std::set<std::string> keys;
        for (const std::string& ref : PrefabLibrary::collectReferences(sceneJson))
            keys.insert(m_lib.normalizeKey(ref));
        scene.traverse([&](GameObject* n) {
            if (n->prefabLink && !n->prefabLink->path.empty())
                keys.insert(m_lib.normalizeKey(n->prefabLink->path));
        });
        std::vector<std::string> frontier(keys.begin(), keys.end());
        while (!frontier.empty())
        {
            const std::string k = frontier.back();
            frontier.pop_back();
            const PrefabTemplate* t = m_lib.find(k);
            if (!t || t->state != PrefabTemplate::State::Ready) continue;
            for (const std::string& ref : PrefabLibrary::collectReferences(t->memoryRoot))
            {
                const std::string nk = m_lib.normalizeKey(ref);
                if (keys.insert(nk).second) frontier.push_back(nk);
            }
        }

        if (!playing())
        {
            // Failed keys are dropped too, so the request() below reads them
            // again: a .prefab fixed on disk loads on the next scene load
            // (request() alone never retries a Failed key).
            std::set<std::string> retained;
            for (const std::string& k : keys)
            {
                const PrefabTemplate* t = m_lib.find(k);
                if (!t || t->state != PrefabTemplate::State::Failed)
                    retained.insert(k);
            }
            m_lib.retainOnly(retained);
            for (const std::string& k : std::set<std::string>(m_pinned))
                if (!retained.count(k))
                {
                    m_pinned.erase(k);
                    if (m_host.unpin) m_host.unpin(k);
                }
            m_known = keys;
            m_reportedFailures.clear();   // a retried key that fails again is logged again
        }
        else
        {
            // A scene loaded by a script in Play: nothing is evicted. The
            // exported runtime never evicts either, and the scripts of the scene
            // being left may still Instantiate what they hold.
            m_known.insert(keys.begin(), keys.end());
        }
        for (const std::string& k : keys)
            m_lib.request(k);

        // Every instance's state as loaded: an edit made before its template
        // is Ready is told apart from it at the first propagation.
        scene.traverse([&](GameObject* n) {
            if (!n->prefabLink || !n->parent) return;
            const PrefabTemplate* t = m_lib.find(n->prefabLink->path);
            m_tracker.captureBaseline(scene, n,
                (t && t->state == PrefabTemplate::State::Ready) ? &t->memoryRoot : nullptr);
        });

        // Templates already Ready (kept across the load) will not call onReady
        // again: the new scene's instances are checked against them now.
        if (!playing())
            for (const std::string& k : keys)
                if (const PrefabTemplate* t = m_lib.find(k); t && t->state == PrefabTemplate::State::Ready)
                    propagate(*t);
    }

    void PrefabSceneSync::syncUnsynced()
    {
        ++m_syncPasses;
        Scene& scene = *m_host.scene;
        std::vector<uint64_t> ids;
        for (GameObject* root : m_tracker.unsyncedInstances(scene, m_lib))
            ids.push_back(root->id);
        for (uint64_t id : ids)
        {
            GameObject* root = scene.findById(id);
            if (!root || !root->prefabLink) continue;
            const PrefabTemplate* t = m_lib.find(root->prefabLink->path);
            if (!t) continue;
            PropagationResult r;
            propagateInstance(scene, root, *t, m_tracker, *m_host.physics, *m_host.audio, m_host.registerGpu, &r);
            absorb(r);
        }
    }

    void PrefabSceneSync::frame(bool revisionChanged, GameObject* deactivatedSelection)
    {
        // Edit Mode only (spec §3.2): in Play nothing is detected or propagated.
        if (!ready() || playing())
            return;

        // A deferred propagation is retried once the loader has nothing left,
        // and at most every kRetryIntervalFrames: the retry walks the scene.
        bool retry = false;
        if (m_syncPending)
        {
            if (m_retryCooldown > 0)
                --m_retryCooldown;
            else if (!m_host.loaderPending || m_host.loaderPending() == 0)
                retry = true;
        }

        if (revisionChanged)
        {
            // An instance the tracker has never seen (a clone, an editor or Lua
            // drop, the undo of a Delete) gets its baseline at the revision change
            // of the command that created it -- before any edit of the user's --
            // so an edit made while its template loads is folded, not reverted.
            Scene& scene = *m_host.scene;
            std::vector<GameObject*> fresh;
            scene.traverse([&](GameObject* n) {
                if (n->prefabLink && n->parent && !m_tracker.hasBaseline(n->id)) fresh.push_back(n);
            });
            for (GameObject* n : fresh)
            {
                const PrefabTemplate* t = m_lib.find(n->prefabLink->path);
                m_tracker.captureBaseline(scene, n,
                    (t && t->state == PrefabTemplate::State::Ready) ? &t->memoryRoot : nullptr);
            }
        }

        if (revisionChanged || retry)
        {
            // Instances the tracker never checked against their Ready template
            // -- a clone, the undo of a Delete, one whose propagation waited for
            // its meshes -- are brought to it first: only then is diffing them
            // meaningful.
            m_syncPending = false;
            syncUnsynced();
            if (m_syncPending) m_retryCooldown = kRetryIntervalFrames;
            if (revisionChanged)
                m_tracker.onRevisionChanged(*m_host.scene, m_lib);
        }
        if (deactivatedSelection)
            m_tracker.onItemDeactivated(*m_host.scene, m_lib, deactivatedSelection);
    }

    void PrefabSceneSync::logFailed()
    {
        for (const std::string& k : m_known)
        {
            const PrefabTemplate* t = m_lib.find(k);
            if (!t || t->state != PrefabTemplate::State::Failed || m_reportedFailures.count(k))
                continue;
            m_reportedFailures.insert(k);
            if (m_host.log)
                m_host.log("[Prefab] Could not load '" + k + "': " + t->error +
                           " (its instances keep their stored state; reload the scene to retry)");
        }
    }

    void PrefabSceneSync::unpinAll()
    {
        if (m_host.unpin)
            for (const std::string& key : m_pinned) m_host.unpin(key);
        m_pinned.clear();
    }

    void PrefabSceneSync::resetForNewRoot()
    {
        unpinAll();
        m_tracker.clear();
        m_known.clear();
        m_reportedFailures.clear();
        m_syncPending = false;
        m_retryCooldown = 0;
    }
}
