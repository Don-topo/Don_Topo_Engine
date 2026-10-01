#pragma once
#include "DonTopo/Core/JobSystem.h"
#include "DonTopo/Core/MaterialAsset.h"
#include "DonTopo/Core/Scene.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace DonTopo
{
    class AsyncAssetLoader;
    class AudioManager;
    class AudioClipComponent;

    // A prefab already in RAM: what Scene::instantiatePrefab builds from with
    // no disk IO. Everything here is owned by the library and only touched on
    // the main thread; workers never see a PrefabTemplate.
    struct PrefabTemplate
    {
        enum class State { Loading, Ready, Failed };
        State          state = State::Loading;
        std::string    key;          // normalized project-relative path, generic separators
        std::string    error;        // not empty only when Failed
        nlohmann::json memoryRoot;   // root node, memory form, prefab-local ids
        PreloadedMeshCache meshes;   // by meshCacheKey; values may be SkinnedMesh
        MaterialAssetCache mats;     // by absolute .mat path
        std::vector<std::shared_ptr<AudioClipComponent>> warmAudio;  // keeps FMOD sounds cached
        // Unique across the whole library (never reused, not even after clear()):
        // assigned every time the template becomes Ready, by a load or by
        // updateFromMemory. A host compares it to know the content changed.
        uint64_t       generation = 0;
    };

    // Preloads prefab templates asynchronously (spec 4.2). One per host.
    //
    // Pipeline per key: a JobSystem job reads + validates the .prefab, turns it
    // into memory form and reads the .mat files it references; pump() then asks a
    // DEDICATED AsyncAssetLoader for its meshes (never the editor's one), applies
    // the animation-source config of skinned meshes on a worker, and when nothing
    // is outstanding warms the audio clips, marks the template Ready and requests
    // every prefab it references (recursion; idempotent request() ends cycles).
    //
    // Stale results never resurrect anything: every load carries a serial, and a
    // result whose serial is not the entry's current one (the key was cleared,
    // retained out, reloaded or already Failed) is dropped. Workers only hold a
    // shared mailbox, never the library, so destroying the library with jobs in
    // flight is safe -- PROVIDED the JobSystem outlives the library (declare it
    // first, or shut it down before): the library's AsyncAssetLoader waits in its
    // destructor for its running jobs, which needs live workers to finish them.
    class PrefabLibrary
    {
        public:
            PrefabLibrary(JobSystem& jobs, std::string assetRoot);
            ~PrefabLibrary();
            PrefabLibrary(const PrefabLibrary&)            = delete;
            PrefabLibrary& operator=(const PrefabLibrary&) = delete;

            // Clears everything (project change).
            void setAssetRoot(std::string root);
            const std::string& assetRoot() const { return m_assetRoot; }

            // Absolute path inside the asset root -> project-relative; '\\' -> '/';
            // lexically normalized. No filesystem access.
            std::string normalizeKey(const std::string& path) const;

            // Idempotent: a key already Loading, Ready or Failed is not read again.
            void request(const std::string& path);

            // Main thread, once per frame. budgetMs bounds the mesh results taken
            // from the loader this frame (what is left stays for the next one).
            void pump(AudioManager& audio, float budgetMs);

            // nullptr if the key was never requested (or was cleared).
            const PrefabTemplate* find(const std::string& path) const;
            int pending() const;   // templates in Loading

            void setOnReady(std::function<void(const PrefabTemplate&)> cb) { m_onReady = std::move(cb); }

            // Apply/Create: replace a template from a .prefab file JSON already in
            // memory (disk form). The file is not read; meshes and .mat files the
            // previous version already had are reused. If everything is cached the
            // template is Ready on return; otherwise the missing pieces load
            // asynchronously (on workers) and it is Loading until pump() ends them.
            void updateFromMemory(const std::string& path, const nlohmann::json& prefabFile,
                                  AudioManager& audio);

            // Drops every template whose key is not in keys (keys are normalized).
            void retainOnly(const std::set<std::string>& keys);
            void clear();

            // Every {"prefab": "<path>"} value (a serialized PrefabRef) inside a
            // node tree or a scene/prefab file: script overrides, and also those
            // values inside prefab-link overrides. Unique, in first-seen order.
            static std::vector<std::string> collectReferences(const nlohmann::json& anyTree);

            int readCount() const;          // tests: .prefab files read from disk
            int meshLoadsPending() const;   // tests: mesh requests the loader has not delivered

            // Worker -> main thread messages. Public only so the .cpp's free
            // functions can name them; not part of the API.
            struct MeshRequest
            {
                std::string    sourcePath;
                int            piece = 0;
                nlohmann::json animationSources;   // null when the node has none
            };
            struct LoadResult
            {
                std::string              key;
                uint64_t                 serial = 0;
                std::string              error;
                nlohmann::json           root;     // memory form
                MaterialAssetCache       mats;
                std::vector<MeshRequest> meshes;   // unique by meshCacheKey
            };
            struct AnimResult
            {
                std::string                 key;
                uint64_t                    serial = 0;
                std::string                 meshKey;
                std::shared_ptr<const Mesh> mesh;
                std::string                 error;
            };
            struct Mailbox;

        private:
            struct Entry
            {
                PrefabTemplate t;
                // The meshes exactly as the loader returned them, before any
                // animation-source config. updateFromMemory configures a copy of
                // THESE: applyAnimationSourceConfig only appends, so configuring
                // the already configured t.meshes again would keep stale clips.
                PreloadedMeshCache rawMeshes;
                uint64_t       serial      = 0;
                int            outstanding = 0;   // mesh loads + animation jobs not back yet
            };
            struct MeshTicket
            {
                std::string      key;
                uint64_t         serial = 0;
                std::string      meshKey;
                nlohmann::json   animationSources;
                JobSystem::JobId loaderJob = 0;   // to cancel it when the ticket is dropped
            };

            Entry* liveEntry(const std::string& key, uint64_t serial);
            void   startMeshes(Entry& e, std::vector<MeshRequest> meshes, MaterialAssetCache mats,
                               nlohmann::json root, AudioManager& audio);
            // Sets e.t.meshes[meshKey] from the RAW loader mesh: as is when it already
            // matches the node's animation sources, else from a worker job that
            // configures a copy.
            void   configureMesh(Entry& e, const std::string& meshKey,
                                 const std::shared_ptr<const Mesh>& raw,
                                 const nlohmann::json& animationSources);
            void   fail(Entry& e, const std::string& error);
            void   finishIfDone(Entry& e, AudioManager& audio);
            void   submitLoad(const std::string& key, uint64_t serial, const nlohmann::json* inMemoryRoot,
                              MaterialAssetCache knownMats);
            // Erases (and cancels in the loader) every ticket for which drop(ticket) is true.
            template <typename Pred> void dropTickets(const Pred& drop);

            JobSystem&                          m_jobs;
            std::string                         m_assetRoot;
            std::unique_ptr<AsyncAssetLoader>   m_loader;
            std::shared_ptr<Mailbox>            m_mailbox;
            std::unordered_map<std::string, Entry>    m_entries;   // node-based: find() pointers stay valid
            std::unordered_map<uint64_t, MeshTicket>  m_tickets;   // by loader targetId
            std::function<void(const PrefabTemplate&)> m_onReady;
            uint64_t                            m_nextSerial     = 1;
            uint64_t                            m_nextTicket     = 1;
            uint64_t                            m_nextGeneration = 1;
            // Bumped by clear(): finishIfDone compares it around the onReady
            // callback, which may clear the library (or change its root).
            uint64_t                            m_clearCount     = 0;
    };
}
