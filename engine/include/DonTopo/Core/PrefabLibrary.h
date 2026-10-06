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
        // By meshCacheKey; values may be SkinnedMesh. Configured with the animation
        // sources of the FIRST node using that file+piece, material overrides NOT
        // applied: what propagation (rebuildPrefabInstance) is seeded with.
        PreloadedMeshCache meshes;
        MaterialAssetCache mats;     // by absolute .mat path
        std::vector<std::shared_ptr<AudioClipComponent>> warmAudio;  // keeps FMOD sounds cached

        // What Scene::instantiatePrefab builds from, prepared once when the
        // template becomes Ready:
        //  - instanceRoot: memoryRoot with the prefab-local ids moved to prefabNode
        //    (no "id"), the root link {key, no overrides}, and every node with a
        //    file mesh pointing at its OWN final mesh by memRef (prefabNodeMeshRef)
        //    with the baselines of its material overrides written in.
        //  - instanceMeshes: t.meshes plus those per-node final meshes: the mesh
        //    configured with THAT node's animation sources and with its material
        //    overrides already applied. Every instance shares them as they are, so
        //    no instance reconfigures (Assimp) or copies (editMesh) anything.
        nlohmann::json     instanceRoot;
        PreloadedMeshCache instanceMeshes;
        // What propagation (rebuildPrefabInstance) is seeded with, by
        // prefabNodeMeshRef(local id): each file-mesh node's mesh configured with
        // THAT node's animation sources, material overrides NOT applied (the
        // rebuild applies the prefab's and the instance's own on top, deriving the
        // baselines as a disk load does). t.meshes only holds the FIRST source list
        // of each file+piece, so without these a node with another list re-reads
        // its external sources (Assimp) on the main thread at every propagation.
        PreloadedMeshCache configuredMeshes;

        // Non-fatal problems found while preparing the template (an external
        // animation source that cannot be read, ...): reported ONCE here, never
        // per instance. The host logs them when the template becomes Ready.
        std::vector<std::string> warnings;
        // Unique across the whole library (never reused, not even after clear()):
        // assigned every time the template becomes Ready, by a load or by
        // updateFromMemory. A host compares it to know the content changed.
        uint64_t       generation = 0;
    };

    // Key of a node's final mesh in PrefabTemplate::instanceMeshes (and its
    // memRef in instanceRoot). Cannot clash with a meshCacheKey (a file path) nor
    // with the procedural memRefs of cloneGameObject.
    std::string prefabNodeMeshRef(uint64_t prefabLocalId);

    // The meshes an instance of t registers on the GPU: per node, its prepared
    // mesh (memRef into instanceMeshes) or, without one, the template's mesh for
    // its file+piece. Unique, pre-order. Procedural meshes are built per instance
    // and are in no cache, so they are not listed. What a host pins
    // (EditorRenderer::pinTemplateMeshes) when the template becomes Ready.
    std::vector<std::shared_ptr<const Mesh>> prefabTemplateGpuMeshes(const PrefabTemplate& t);

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

            // Keep, for the template's whole life, the unconfigured loader copy of
            // every skinned mesh whose animation sources had to be applied. It
            // doubles that mesh's RAM, and only pays off for updateFromMemory with
            // CHANGED sources (rebuilt from RAM instead of re-read by the loader).
            // Default off: the exported runtime never calls updateFromMemory. The
            // EDITOR turns it on (Apply edits prefabs). Set it before requesting;
            // it does not drop or recover raw meshes of templates already loaded.
            void setKeepRawMeshes(bool keep) { m_keepRawMeshes = keep; }

            // Apply/Create: replace a template from a .prefab file JSON already in
            // memory (disk form). The file is not read; meshes and .mat files the
            // previous version already had are reused. If everything is cached the
            // template is Ready on return; otherwise the missing pieces load
            // asynchronously (on workers) and it is Loading until pump() ends them.
            void updateFromMemory(const std::string& path, const nlohmann::json& prefabFile,
                                  AudioManager& audio);

            // Drops every template whose key is not in keys (keys are normalized).
            void retainOnly(const std::set<std::string>& keys);
            // Drops one template (its in-flight loads are cancelled and their late
            // results ignored, as with retainOnly) and tells onEvicted, so the
            // host releases what it holds for it (its GPU pin). The undo of a
            // Create whose .prefab was deleted. No-op for an unknown key.
            void evict(const std::string& path);
            void setOnEvicted(std::function<void(const std::string&)> cb) { m_onEvicted = std::move(cb); }
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
                // Other nodes on the same file+piece with a DIFFERENT source list
                // (null = no key): each gets its own configured mesh.
                std::vector<nlohmann::json> extraSources;
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
                // configKey of this configuration; extra = it is not the primary
                // (t.meshes) one but goes to Entry::configMeshes under configKey.
                std::string                 configKey;
                bool                        extra = false;
                std::shared_ptr<const Mesh> mesh;
                std::string                 error;
                std::vector<std::string>    warnings;   // of applyAnimationSourceConfig
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
                // Without setKeepRawMeshes, a raw mesh is dropped as soon as its
                // configured copy arrives (one that needs no config IS the final
                // mesh, same object, so it stays at no cost).
                PreloadedMeshCache rawMeshes;
                // Configured meshes for the source lists that are not the first one
                // of their file+piece, by configKey(meshKey, sources).
                PreloadedMeshCache configMeshes;
                // Warnings of applying each configuration (primary ones included),
                // by configKey: kept with the configuration they belong to, so a
                // configured mesh reused by updateFromMemory keeps its warning.
                std::unordered_map<std::string, std::vector<std::string>> configWarnings;
                uint64_t       serial      = 0;
                int            outstanding = 0;   // mesh loads + animation jobs not back yet
            };
            struct MeshTicket
            {
                std::string      key;
                uint64_t         serial = 0;
                std::string      meshKey;
                nlohmann::json   animationSources;
                std::vector<nlohmann::json> extraSources;
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
            // The same for an extra source list: Entry::configMeshes[configKey].
            void   configureExtra(Entry& e, const std::string& meshKey,
                                  const std::shared_ptr<const Mesh>& raw,
                                  const nlohmann::json& animationSources);
            // Submits the worker job that configures a copy of raw; the result
            // comes back as an AnimResult (configKey empty = the t.meshes one).
            void   submitConfigJob(Entry& e, const std::string& meshKey, const std::string& configKey,
                                   const std::shared_ptr<const Mesh>& raw,
                                   const nlohmann::json& animationSources);
            // Ready-time preparation: instanceRoot + instanceMeshes (see PrefabTemplate).
            void   prepareInstance(Entry& e);
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
            std::function<void(const std::string&)>    m_onEvicted;
            uint64_t                            m_nextSerial     = 1;
            uint64_t                            m_nextTicket     = 1;
            uint64_t                            m_nextGeneration = 1;
            // Bumped by clear(): finishIfDone compares it around the onReady
            // callback, which may clear the library (or change its root).
            uint64_t                            m_clearCount     = 0;
            bool                                m_keepRawMeshes  = false;
    };
}
