#pragma once
#include "DonTopo/Editor/PrefabOverrideTracker.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
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
            // Pumps between two looks at the .prefab files of Failed templates.
            // One that changed on disk since it failed (restored, recreated,
            // moved back) is loaded again; its instances stop being Missing.
            static constexpr int kFailedPollFrames = 30;

            explicit PrefabSceneSync(PrefabLibrary& lib);
            ~PrefabSceneSync();
            PrefabSceneSync(const PrefabSceneSync&)            = delete;
            PrefabSceneSync& operator=(const PrefabSceneSync&) = delete;

            void setHost(Host host);
            const Host& host() const { return m_host; }

            // Pumps the library (onReady fires from here), logs Failed keys and,
            // every kFailedPollFrames calls, reloads the Failed ones whose file
            // changed on disk.
            void pump(float budgetMs);
            // ── Drops of a .prefab whose template is not Ready yet (spec §5.2) ──
            // Never waited for on the main thread: the drop is queued and the
            // host instantiates it once the template is Ready (takeReady...).
            struct QueuedInstantiate
            {
                std::string key;            // normalized
                uint64_t    parentId = 0;   // 0 = scene root; resolved when it runs
                std::chrono::steady_clock::time_point deadline;
            };
            // Requests key, logs "Loading prefab '<key>'...".
            void queueInstantiate(const std::string& key, uint64_t parentId, std::chrono::milliseconds timeout);
            // The queued drops whose template is Ready, in order, removed from
            // the queue. Failed / evicted / past their deadline: dropped with a
            // Log line. The caller runs an InstantiatePrefabCommand per item
            // (which refuses a parent that is gone or now inside an instance).
            std::vector<QueuedInstantiate> takeReadyInstantiates(std::chrono::steady_clock::time_point now);
            // Scene reload, project switch, Play/Stop: queued ids belong to a
            // scene that is gone. sceneLoaded and resetForNewRoot call it.
            void clearQueuedInstantiates() { m_queuedInstantiates.clear(); }
            size_t queuedInstantiateCount() const { return m_queuedInstantiates.size(); }
            void onTemplateReady(const PrefabTemplate& t);
            // PrefabLibrary::evict (installed as its onEvicted): unpins the key.
            void onTemplateEvicted(const std::string& key);
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
            void retryChangedFailures();
            void absorb(PropagationResult& r);

            // What the .prefab of a Failed key looked like when it failed.
            struct FileStamp
            {
                bool                            exists = false;
                std::uintmax_t                  size   = 0;
                std::filesystem::file_time_type time{};
                bool operator==(const FileStamp&) const = default;
            };
            FileStamp stampOf(const std::string& key) const;
            std::unordered_map<std::string, FileStamp> m_failedStamps;
            std::vector<QueuedInstantiate> m_queuedInstantiates;
            int m_failedPollCountdown = kFailedPollFrames;

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
