#include "DonTopo/Core/PrefabLibrary.h"

#include "DonTopo/Audio/AudioBus.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Core/Platform.h"
#include "DonTopo/Files/FileManager.h"
#include "DonTopo/Renderer/AsyncAssetLoader.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Renderer/SkinnedMeshAnimations.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_set>

namespace DonTopo
{
    // What workers share with the library. Jobs capture a shared_ptr to this,
    // never the library: after ~PrefabLibrary sets `closed`, a late job finds
    // the flag and posts nothing.
    struct PrefabLibrary::Mailbox
    {
        std::mutex              mutex;
        bool                    closed    = false;
        int                     readCount = 0;
        std::vector<LoadResult> loads;
        std::vector<AnimResult> anims;
    };

    namespace
    {
        namespace fs = std::filesystem;

        bool isPrefabFile(const nlohmann::json& f)
        {
            if (!f.is_object()) return false;
            const auto v = f.find("version");
            const auto t = f.find("type");
            const auto r = f.find("root");
            return v != f.end() && v->is_number_integer() && v->get<int>() == 1 &&
                   t != f.end() && t->is_string() && t->get<std::string>() == "prefab" &&
                   r != f.end() && r->is_object();
        }

        template <typename F>
        void forEachNode(const nlohmann::json& node, const F& f)
        {
            if (!node.is_object()) return;
            f(node);
            const auto c = node.find("children");
            if (c != node.end() && c->is_array())
                for (const auto& child : *c)
                    forEachNode(child, f);
        }

        std::string stringField(const nlohmann::json& j, const char* key)
        {
            const auto it = j.find(key);
            return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
        }

        bool boolField(const nlohmann::json& j, const char* key, bool fallback)
        {
            const auto it = j.find(key);
            return (it != j.end() && it->is_boolean()) ? it->get<bool>() : fallback;
        }

        // Every non-empty matAsset of the tree (memory form: absolute paths).
        std::vector<std::string> matAssetPaths(const nlohmann::json& root)
        {
            std::vector<std::string> out;
            forEachNode(root, [&](const nlohmann::json& node) {
                const auto mesh = node.find("mesh");
                if (mesh == node.end() || !mesh->is_object()) return;
                const auto mats = mesh->find("materials");
                if (mats == mesh->end() || !mats->is_array()) return;
                for (const auto& entry : *mats)
                    if (entry.is_object())
                        if (std::string p = stringField(entry, "matAsset"); !p.empty())
                            out.push_back(std::move(p));
            });
            return out;
        }

        // One request per meshCacheKey: two nodes sharing a file and a piece share
        // the cached mesh. The first node's animationSources wins for a key (two
        // nodes of the same rig with different source lists: the second one gets a
        // deep copy at Instantiate, as nodeFromJson does on a mismatch).
        std::vector<PrefabLibrary::MeshRequest> meshRequests(const nlohmann::json& root)
        {
            std::vector<PrefabLibrary::MeshRequest> out;
            std::unordered_set<std::string> seen;
            forEachNode(root, [&](const nlohmann::json& node) {
                const auto mesh = node.find("mesh");
                if (mesh == node.end() || !mesh->is_object()) return;
                PrefabLibrary::MeshRequest r;
                r.sourcePath = stringField(*mesh, "sourcePath");
                if (r.sourcePath.empty()) return;   // procedural / inline mesh: nothing to load
                if (const auto p = mesh->find("piece"); p != mesh->end() && p->is_number_integer())
                    r.piece = std::max(0, p->get<int>());
                if (!seen.insert(meshCacheKey(r.sourcePath, r.piece)).second) return;
                if (const auto a = mesh->find("animationSources"); a != mesh->end())
                    r.animationSources = *a;
                out.push_back(std::move(r));
            });
            return out;
        }

        // Same parsing as nodeFromJson's, so the configured mesh is what an
        // instance would have built: a corrupt clip list keeps no names.
        std::vector<AnimationSourceConfig> animationConfigs(const nlohmann::json& sources)
        {
            std::vector<AnimationSourceConfig> out;
            for (const auto& sj : sources)
            {
                if (!sj.is_object()) continue;
                AnimationSourceConfig cfg;
                cfg.path    = stringField(sj, "path");
                cfg.builtin = boolField(sj, "builtin", false);
                if (const auto cj = sj.find("clips"); cj != sj.end() && cj->is_array())
                {
                    bool ok = true;
                    for (const auto& c : *cj) ok = ok && c.is_string();
                    if (ok) cfg.clipNames = cj->get<std::vector<std::string>>();
                }
                out.push_back(std::move(cfg));
            }
            return out;
        }

        void collectRefs(const nlohmann::json& j, std::vector<std::string>& out,
                         std::unordered_set<std::string>& seen)
        {
            if (j.is_object())
            {
                // A serialized PrefabRef is exactly {"prefab": "<path>"}. A prefab
                // LINK is {"prefab": {"path", "overrides"}} and is not matched.
                if (j.size() == 1)
                    if (const auto p = j.find("prefab"); p != j.end() && p->is_string())
                    {
                        const std::string path = p->get<std::string>();
                        if (!path.empty() && seen.insert(path).second) out.push_back(path);
                        return;
                    }
                for (const auto& [k, v] : j.items())
                    collectRefs(v, out, seen);
            }
            else if (j.is_array())
            {
                for (const auto& v : j)
                    collectRefs(v, out, seen);
            }
        }
    }

    PrefabLibrary::PrefabLibrary(JobSystem& jobs, std::string assetRoot)
        : m_jobs(jobs),
          m_assetRoot(std::move(assetRoot)),
          m_loader(std::make_unique<AsyncAssetLoader>(jobs)),
          m_mailbox(std::make_shared<Mailbox>())
    {
    }

    PrefabLibrary::~PrefabLibrary()
    {
        {
            std::lock_guard<std::mutex> lock(m_mailbox->mutex);
            m_mailbox->closed = true;
            m_mailbox->loads.clear();
            m_mailbox->anims.clear();
        }
        // The loader's own destructor cancels its queue and waits for the mesh
        // jobs already running: they post into the loader, which is ours.
        m_loader.reset();
    }

    void PrefabLibrary::setAssetRoot(std::string root)
    {
        clear();
        m_assetRoot = std::move(root);
    }

    std::string PrefabLibrary::normalizeKey(const std::string& path) const
    {
        if (path.empty()) return {};
        std::string s = path;
        std::replace(s.begin(), s.end(), '\\', '/');
        const fs::path p = fs::path(s).lexically_normal();
        const std::string g = p.generic_string();
        if (p.is_absolute() && !m_assetRoot.empty())
        {
            std::string r = m_assetRoot;
            std::replace(r.begin(), r.end(), '\\', '/');
            r = fs::path(r).lexically_normal().generic_string();
            while (!r.empty() && r.back() == '/') r.pop_back();
            // Plain prefix test on the normalized strings. lexically_relative
            // compares elements case-sensitively, but on Windows "c:/users" and
            // "C:/Users" are the same folder, and both spellings reach us (a
            // dialog, a drop, a stored path).
            if (!r.empty() && g.size() > r.size() + 1 && g[r.size()] == '/')
            {
                const bool caseInsensitive = platform::currentOs() == platform::Os::Windows;
                bool same = true;
                for (size_t i = 0; same && i < r.size(); ++i)
                {
                    same = caseInsensitive
                         ? std::tolower(static_cast<unsigned char>(g[i])) ==
                           std::tolower(static_cast<unsigned char>(r[i]))
                         : g[i] == r[i];
                }
                if (same) return g.substr(r.size() + 1);
            }
        }
        return g;
    }

    PrefabLibrary::Entry* PrefabLibrary::liveEntry(const std::string& key, uint64_t serial)
    {
        const auto it = m_entries.find(key);
        if (it == m_entries.end()) return nullptr;
        Entry& e = it->second;
        // A result of an older load of this key (re-requested after clear or
        // retainOnly, or replaced by updateFromMemory), or for a template that
        // already failed, is dropped.
        if (e.serial != serial || e.t.state != PrefabTemplate::State::Loading) return nullptr;
        return &e;
    }

    void PrefabLibrary::submitLoad(const std::string& key, uint64_t serial,
                                   const nlohmann::json* inMemoryRoot, MaterialAssetCache knownMats)
    {
        std::optional<nlohmann::json> memoryRoot;
        if (inMemoryRoot) memoryRoot = *inMemoryRoot;
        const std::shared_ptr<Mailbox> mailbox = m_mailbox;
        const std::string root = m_assetRoot;

        const JobSystem::JobId id = m_jobs.submit(
            [mailbox, key, serial, root, memoryRoot = std::move(memoryRoot), knownMats = std::move(knownMats)]() mutable
            {
                {
                    std::lock_guard<std::mutex> lock(mailbox->mutex);
                    if (mailbox->closed) return;
                }
                LoadResult r;
                r.key    = key;
                r.serial = serial;
                try
                {
                    nlohmann::json tree;
                    if (memoryRoot)
                    {
                        tree = std::move(*memoryRoot);
                    }
                    else
                    {
                        const fs::path file = fs::path(key).is_absolute() || root.empty()
                                            ? fs::path(key) : fs::path(root) / key;
                        std::optional<nlohmann::json> f = FileManager::readJson(file.string());
                        {
                            std::lock_guard<std::mutex> lock(mailbox->mutex);
                            ++mailbox->readCount;
                        }
                        if (!f)
                            r.error = "cannot read '" + file.generic_string() + "' (missing or not valid JSON)";
                        else if (!isPrefabFile(*f))
                            r.error = "not a prefab file";
                        else
                        {
                            tree = std::move((*f)["root"]);
                            resolveStoredPaths(tree, root);
                        }
                    }
                    if (r.error.empty())
                    {
                        r.mats = std::move(knownMats);
                        for (const std::string& p : matAssetPaths(tree))
                            if (!r.mats.count(p))
                                r.mats.emplace(p, loadMaterialAsset(p));
                        r.meshes = meshRequests(tree);
                        r.root   = std::move(tree);
                    }
                }
                catch (const std::exception& e)
                {
                    r.error = e.what();
                }
                catch (...)
                {
                    r.error = "unknown error loading the prefab";
                }

                std::lock_guard<std::mutex> lock(mailbox->mutex);
                if (mailbox->closed) return;
                mailbox->loads.push_back(std::move(r));
            });

        if (id == 0)
            if (Entry* e = liveEntry(key, serial))
                fail(*e, "the job system is not running");
    }

    void PrefabLibrary::request(const std::string& path)
    {
        const std::string key = normalizeKey(path);
        if (key.empty() || m_entries.count(key)) return;
        Entry& e = m_entries[key];
        e.t.key   = key;
        e.t.state = PrefabTemplate::State::Loading;
        e.serial  = m_nextSerial++;
        submitLoad(key, e.serial, nullptr, {});
    }

    template <typename Pred>
    void PrefabLibrary::dropTickets(const Pred& drop)
    {
        for (auto it = m_tickets.begin(); it != m_tickets.end();)
        {
            if (!drop(it->second)) { ++it; continue; }
            // Nobody will consume this result: free the loader's work too. A job
            // already running finishes, and its result finds no ticket.
            if (it->second.loaderJob != 0) m_loader->cancel(it->second.loaderJob);
            it = m_tickets.erase(it);
        }
    }

    void PrefabLibrary::fail(Entry& e, const std::string& error)
    {
        const std::string key = e.t.key;
        dropTickets([&](const MeshTicket& t) { return t.key == key; });
        e.t.state = PrefabTemplate::State::Failed;
        e.t.error = error.empty() ? std::string("unknown error") : error;
        e.t.memoryRoot = nlohmann::json();
        e.t.meshes.clear();
        e.t.mats.clear();
        e.t.warmAudio.clear();
        e.rawMeshes.clear();
        e.outstanding = 0;
    }

    void PrefabLibrary::startMeshes(Entry& e, std::vector<MeshRequest> meshes, MaterialAssetCache mats,
                                    nlohmann::json root, AudioManager& audio)
    {
        e.t.memoryRoot = std::move(root);
        e.t.mats       = std::move(mats);

        // Meshes kept from a previous version (updateFromMemory) that the new tree
        // no longer uses are dropped; the ones it still uses are not reloaded.
        std::unordered_set<std::string> wanted;
        for (const MeshRequest& m : meshes) wanted.insert(meshCacheKey(m.sourcePath, m.piece));
        std::erase_if(e.t.meshes,   [&](const auto& kv) { return !wanted.count(kv.first); });
        std::erase_if(e.rawMeshes,  [&](const auto& kv) { return !wanted.count(kv.first); });

        for (MeshRequest& m : meshes)
        {
            if (e.t.state != PrefabTemplate::State::Loading) return;   // failed meanwhile
            const std::string meshKey = meshCacheKey(m.sourcePath, m.piece);
            if (const auto it = e.rawMeshes.find(meshKey); it != e.rawMeshes.end())
            {
                const std::shared_ptr<const Mesh> raw = it->second;
                configureMesh(e, meshKey, raw, m.animationSources);
                continue;
            }
            // No raw mesh (dropped after configuring, setKeepRawMeshes off): the
            // configured copy is reused only if it already has exactly the sources
            // asked for; anything else needs a fresh raw mesh from the loader,
            // since configuring the configured copy would only append.
            if (const auto it = e.t.meshes.find(meshKey); it != e.t.meshes.end())
            {
                const auto* sk = dynamic_cast<const SkinnedMesh*>(it->second.get());
                if (sk && m.animationSources.is_array() && meshMatchesAnimationConfig(*sk, m.animationSources))
                    continue;
                e.t.meshes.erase(it);
            }
            const uint64_t ticket = m_nextTicket++;
            const JobSystem::JobId job = m_loader->requestMesh(m.sourcePath, ticket, m.piece);
            if (job == 0)
            {
                fail(e, "the job system is not running");
                return;
            }
            m_tickets[ticket] = MeshTicket{ e.t.key, e.serial, meshKey, std::move(m.animationSources), job };
            ++e.outstanding;
        }
        finishIfDone(e, audio);
    }

    void PrefabLibrary::configureMesh(Entry& e, const std::string& meshKey,
                                      const std::shared_ptr<const Mesh>& raw,
                                      const nlohmann::json& animationSources)
    {
        // No key: nodeFromJson shares a mesh whose only source is its own builtin
        // one, which is exactly the raw loader mesh. Same for a static mesh and for
        // a raw skinned mesh that already matches the requested sources.
        const auto* sk = dynamic_cast<const SkinnedMesh*>(raw.get());
        if (!animationSources.is_array() || !sk || meshMatchesAnimationConfig(*sk, animationSources))
        {
            e.t.meshes[meshKey] = raw;
            return;
        }
        // The configured mesh of the previous version already matches (an Apply
        // that did not touch the sources): keep it, it may be shared by instances.
        if (const auto it = e.t.meshes.find(meshKey); it != e.t.meshes.end())
            if (const auto* cur = dynamic_cast<const SkinnedMesh*>(it->second.get()))
                if (meshMatchesAnimationConfig(*cur, animationSources))
                    return;
        // Otherwise nothing stale may stay visible while the new config is built.
        e.t.meshes.erase(meshKey);
        const std::shared_ptr<const Mesh>& mesh = raw;

        // applyAnimationSourceConfig reads the external FBX files and only APPENDS
        // sources: a worker job, on a copy of the RAW mesh.
        std::vector<AnimationSourceConfig> configs = animationConfigs(animationSources);
        const std::shared_ptr<Mailbox> mailbox = m_mailbox;
        const std::string key    = e.t.key;
        const uint64_t    serial = e.serial;
        ++e.outstanding;
        const JobSystem::JobId id = m_jobs.submit(
            [mailbox, key, serial, meshKey, mesh, configs = std::move(configs)]
            {
                {
                    std::lock_guard<std::mutex> lock(mailbox->mutex);
                    if (mailbox->closed) return;
                }
                AnimResult r;
                r.key     = key;
                r.serial  = serial;
                r.meshKey = meshKey;
                try
                {
                    auto copy = std::make_shared<SkinnedMesh>(static_cast<const SkinnedMesh&>(*mesh));
                    std::vector<std::string> warnings;   // same "warn and continue" as nodeFromJson
                    applyAnimationSourceConfig(*copy, configs, warnings);
                    r.mesh = std::move(copy);
                }
                catch (const std::exception& ex)
                {
                    r.error = ex.what();
                }
                catch (...)
                {
                    r.error = "unknown error applying the animation sources of '" + meshKey + "'";
                }
                std::lock_guard<std::mutex> lock(mailbox->mutex);
                if (mailbox->closed) return;
                mailbox->anims.push_back(std::move(r));
            });
        if (id == 0) fail(e, "the job system is not running");
    }

    void PrefabLibrary::finishIfDone(Entry& e, AudioManager& audio)
    {
        if (e.t.state != PrefabTemplate::State::Loading || e.outstanding > 0) return;

        // Same calls nodeFromJson makes for an audioClip, so Instantiate hits the
        // AudioManager's sound cache (and the sidecar is read here, once).
        std::vector<std::shared_ptr<AudioClipComponent>> warm;
        forEachNode(e.t.memoryRoot, [&](const nlohmann::json& node) {
            const auto c = node.find("audioClip");
            if (c == node.end() || !c->is_object()) return;
            const std::string path = stringField(*c, "path");
            if (path.empty()) return;
            std::string ext = fs::path(path).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (!isSupportedAudioExtension(ext)) return;
            const bool is3D = boolField(*c, "is3D", false);
            const bool loop = boolField(*c, "loop", false);
            AudioLoadMode mode = AudioLoadMode::Sample;
            if (!audioLoadModeFromStr(stringField(*c, "loadMode"), mode)) mode = AudioLoadMode::Sample;
            AudioRolloff rolloff = AudioRolloff::Inverse;
            if (!audioRolloffFromStr(stringField(*c, "rolloff"), rolloff)) rolloff = AudioRolloff::Inverse;
            // nodeFromJson creates the clip with the defaults and then RELOADS it for
            // its loadMode and then for its rolloff (setLoadMode / setRolloff). Every
            // sound key on that path must stay cached, not only the last one: a
            // step that misses creates the FMOD sound again and reads the sidecar.
            // One warm clip per step keeps each key alive.
            auto warmStep = [&](bool withMode, bool withRolloff) {
                auto clip = audio.createAudioClipComponent(path, is3D, loop);
                if (!clip) return false;
                if (withMode) clip->setLoadMode(mode);
                if (withRolloff) clip->setRolloff(rolloff);
                warm.push_back(std::move(clip));
                return true;
            };
            if (!warmStep(false, false)) return;
            if (mode != AudioLoadMode::Sample)    warmStep(true, false);
            if (rolloff != AudioRolloff::Inverse) warmStep(true, true);
        });
        e.t.warmAudio  = std::move(warm);   // new ones first: the shared sounds never hit zero refs
        e.t.state      = PrefabTemplate::State::Ready;
        e.t.error.clear();
        e.t.generation = m_nextGeneration++;

        // Copied before the callback: it may clear() or retainOnly() and leave `e`
        // dangling. If it cleared the library (or changed the root), these
        // references belong to a library state that no longer exists.
        const std::vector<std::string> refs = collectReferences(e.t.memoryRoot);
        const uint64_t clearsBefore = m_clearCount;
        if (m_onReady) m_onReady(e.t);
        if (m_clearCount != clearsBefore) return;
        for (const std::string& r : refs)
            request(r);
    }

    void PrefabLibrary::pump(AudioManager& audio, float budgetMs)
    {
        std::vector<LoadResult> loads;
        std::vector<AnimResult> anims;
        {
            std::lock_guard<std::mutex> lock(m_mailbox->mutex);
            loads.swap(m_mailbox->loads);
            anims.swap(m_mailbox->anims);
        }

        for (LoadResult& r : loads)
        {
            Entry* e = liveEntry(r.key, r.serial);
            if (!e) continue;
            if (!r.error.empty()) { fail(*e, r.error); continue; }
            startMeshes(*e, std::move(r.meshes), std::move(r.mats), std::move(r.root), audio);
        }

        for (AnimResult& r : anims)
        {
            Entry* e = liveEntry(r.key, r.serial);
            if (!e) continue;
            --e->outstanding;
            if (!r.error.empty() || !r.mesh) { fail(*e, r.error); continue; }
            e->t.meshes[r.meshKey] = std::move(r.mesh);
            // The configured copy exists: unless asked to keep it, the raw one
            // would only double this mesh's RAM for the template's whole life.
            if (!m_keepRawMeshes) e->rawMeshes.erase(r.meshKey);
            finishIfDone(*e, audio);
        }

        for (LoadedMesh& m : m_loader->pumpCompleted(budgetMs))
        {
            const auto tk = m_tickets.find(m.targetId);
            if (tk == m_tickets.end()) continue;   // cleared meanwhile
            MeshTicket ticket = std::move(tk->second);
            m_tickets.erase(tk);

            Entry* e = liveEntry(ticket.key, ticket.serial);
            if (!e) continue;
            --e->outstanding;
            if (!m.error.empty() || !m.mesh)
            {
                fail(*e, m.error.empty() ? "cannot load mesh '" + m.path + "'" : m.error);
                continue;
            }
            // Multi-piece static model: every piece of the file is already in RAM;
            // keep them all so sibling pieces need no request of their own.
            // (Static pieces: no animation config, raw == final.)
            for (size_t i = 0; i < m.pieceMeshes.size(); ++i)
            {
                const std::string pieceKey = meshCacheKey(m.path, static_cast<int>(i));
                e->rawMeshes.try_emplace(pieceKey, m.pieceMeshes[i]);
                e->t.meshes.try_emplace(pieceKey, m.pieceMeshes[i]);
            }
            const std::shared_ptr<const Mesh> raw = std::move(m.mesh);
            e->rawMeshes[ticket.meshKey] = raw;
            configureMesh(*e, ticket.meshKey, raw, ticket.animationSources);
            finishIfDone(*e, audio);
        }
    }

    const PrefabTemplate* PrefabLibrary::find(const std::string& path) const
    {
        const auto it = m_entries.find(normalizeKey(path));
        return it == m_entries.end() ? nullptr : &it->second.t;
    }

    int PrefabLibrary::pending() const
    {
        int n = 0;
        for (const auto& [key, e] : m_entries)
            if (e.t.state == PrefabTemplate::State::Loading) ++n;
        return n;
    }

    void PrefabLibrary::updateFromMemory(const std::string& path, const nlohmann::json& prefabFile,
                                         AudioManager& audio)
    {
        const std::string key = normalizeKey(path);
        if (key.empty()) return;
        // Mesh loads of the old version are no longer wanted under the new serial
        // (a mesh the new version still uses is requested again below).
        dropTickets([&](const MeshTicket& t) { return t.key == key; });
        Entry& e = m_entries[key];
        e.t.key       = key;
        e.t.state     = PrefabTemplate::State::Loading;
        e.t.error.clear();
        e.serial      = m_nextSerial++;   // any in-flight load of the old version is now stale
        e.outstanding = 0;
        if (!isPrefabFile(prefabFile)) { fail(e, "not a prefab file"); return; }

        nlohmann::json root = prefabFile["root"];
        resolveStoredPaths(root, m_assetRoot);

        MaterialAssetCache mats;
        bool missingMats = false;
        for (const std::string& p : matAssetPaths(root))
        {
            if (mats.count(p)) continue;
            if (const auto it = e.t.mats.find(p); it != e.t.mats.end())
                mats.emplace(p, it->second);
            else
                missingMats = true;
        }
        // A .mat the old version did not have is read on a worker, never here.
        if (missingMats)
        {
            submitLoad(key, e.serial, &root, std::move(mats));
            return;
        }
        // Scanned BEFORE the call: argument evaluation order is unspecified, and
        // MSVC builds the moved-into `root` parameter first, leaving
        // meshRequests(root) an empty tree.
        std::vector<MeshRequest> meshes = meshRequests(root);
        startMeshes(e, std::move(meshes), std::move(mats), std::move(root), audio);
    }

    void PrefabLibrary::retainOnly(const std::set<std::string>& keys)
    {
        std::unordered_set<std::string> keep;
        for (const std::string& k : keys) keep.insert(normalizeKey(k));
        std::erase_if(m_entries, [&](const auto& kv) { return !keep.count(kv.first); });
        dropTickets([&](const MeshTicket& t) { return !keep.count(t.key); });
    }

    void PrefabLibrary::clear()
    {
        ++m_clearCount;
        m_entries.clear();
        m_tickets.clear();
        m_loader->cancelAllPending();
        // The mailbox is NOT emptied: a job can post after any point we pick, so
        // the per-load serial is the one mechanism that drops stale results
        // (liveEntry), and the tests exercise exactly that.
    }

    std::vector<std::string> PrefabLibrary::collectReferences(const nlohmann::json& anyTree)
    {
        std::vector<std::string> out;
        std::unordered_set<std::string> seen;
        collectRefs(anyTree, out, seen);
        return out;
    }

    int PrefabLibrary::meshLoadsPending() const
    {
        return m_loader->pending();
    }

    int PrefabLibrary::readCount() const
    {
        std::lock_guard<std::mutex> lock(m_mailbox->mutex);
        return m_mailbox->readCount;
    }
}
