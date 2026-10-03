#pragma once
#include "DonTopo/Editor/PrefabOverrideTracker.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace DonTopo
{
    class AudioManager;
    class GameObject;
    class PhysicsManager;
    class PrefabLibrary;
    class Scene;
    struct Mesh;
    struct PrefabTemplate;

    // Everything the editor does around its PrefabLibrary, without ImGui or a GPU
    // (EditorUI forwards to it; the tests drive it directly):
    //  - Ready template -> log its warnings once, pin its meshes, propagate it to
    //    the scene's instances in Edit Mode;
    //  - scene (re)load -> request what it references, evict the rest (Edit Mode
    //    only), re-read Failed keys, capture every instance's baseline, propagate
    //    the templates already Ready;
    //  - per frame -> log Failed templates once, sync instances the tracker never
    //    saw on a revision change, retry deferred propagations (bounded), and
    //    recompute overrides (revision change / falling edge of an active item).
    // It installs itself as the library's onReady callback; the library must
    // outlive it.
    class PrefabSceneSync
    {
        public:
            struct Host
            {
                Scene*          scene   = nullptr;
                PhysicsManager* physics = nullptr;
                AudioManager*   audio   = nullptr;
                std::function<bool()>                 isPlaying;     // null = Edit Mode
                RegisterSubtreeFn                     registerGpu;   // rebuilt subtrees
                std::function<void(const std::string&, const std::vector<std::shared_ptr<const Mesh>>&)> pin;
                std::function<void(const std::string&)> unpin;
                std::function<void(const std::string&)> log;
                // AsyncAssetLoader::isRequestLive / pending(); null = no loader.
                std::function<bool(uint64_t)>         meshRequestLive;
                std::function<int()>                  loaderPending;
            };

            // Frames between two retries of a deferred propagation. A retry walks
            // the whole scene; it must not run every frame while an instance waits.
            static constexpr int kRetryIntervalFrames = 30;

            explicit PrefabSceneSync(PrefabLibrary& lib);
            ~PrefabSceneSync();
            PrefabSceneSync(const PrefabSceneSync&)            = delete;
            PrefabSceneSync& operator=(const PrefabSceneSync&) = delete;

            void setHost(Host host);
            const Host& host() const { return m_host; }

            // Pumps the library (onReady fires from here) and logs Failed keys.
            void pump(float budgetMs);
            void onTemplateReady(const PrefabTemplate& t);
            void sceneLoaded(const nlohmann::json& sceneJson);
            // End of an editor frame. deactivatedSelection: the selection on the
            // frame an active item was released, else nullptr.
            void frame(bool revisionChanged, GameObject* deactivatedSelection);

            // Project change (the library is re-rooted) / release: every pin goes.
            void unpinAll();
            void resetForNewRoot();

            PrefabOverrideTracker&       tracker()       { return m_tracker; }
            const std::set<std::string>& pinned() const  { return m_pinned; }
            // Instances rebuilt since the last call (the host invalidates its
            // per-object caches and re-resolves its selection).
            int takeRebuiltCount() { const int n = m_rebuilt; m_rebuilt = 0; return n; }
            int syncPasses() const        { return m_syncPasses; }   // tests
            bool retryPending() const     { return m_syncPending; }  // tests

        private:
            bool playing() const { return m_host.isPlaying && m_host.isPlaying(); }
            bool ready() const   { return m_host.scene && m_host.physics && m_host.audio; }
            void propagate(const PrefabTemplate& t);
            void syncUnsynced();
            void logFailed();
            void absorb(PropagationResult& r);

            PrefabLibrary&        m_lib;
            Host                  m_host;
            PrefabOverrideTracker m_tracker;
            std::set<std::string> m_pinned;
            std::set<std::string> m_known;
            std::set<std::string> m_reportedFailures;
            bool                  m_syncPending   = false;
            int                   m_retryCooldown = 0;
            int                   m_syncPasses    = 0;
            int                   m_rebuilt       = 0;
    };
}
