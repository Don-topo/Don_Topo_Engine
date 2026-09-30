#include "DonTopo/Renderer/AsyncAssetLoader.h"
#include "DonTopo/Renderer/MaterialTextureSource.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/MaterialAsset.h"


#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <unordered_map>
#include <utility>
#include "DonTopo/Renderer/EditorRenderer.h"

namespace DonTopo
{
    namespace
    {
        // Decodes a slot to RGBA8. Returns false if there is nothing to
        // decode or if stb fails; the fallback (checkerboard, flat normal,
        // white) is still supplied by GpuResources on the main thread, which is
        // where that policy lives today.
        bool decodeSlot(const std::string& path, const std::vector<uint8_t>& embedded,
                        DecodedImage::Slot slot, std::vector<DecodedImage>& out)
        {
            const DecodedTexture tex = decodeMaterialTexture(path, embedded);
            if (!tex) return false;  // the fallback is supplied by GpuResources on the main thread

            DecodedImage img;
            img.slot = slot;
            img.w    = tex.w;
            img.h    = tex.h;
            img.pixels.assign(tex.pixels.get(),
                              tex.pixels.get() + static_cast<size_t>(tex.w) * tex.h * 4);
            img.colorSpace = tex.colorSpace;
            img.mips       = tex.mips;
            out.push_back(std::move(img));
            return true;
        }
    }

    void discardOverriddenDecodedImages(std::vector<DecodedImage>& images,
                                        const std::vector<MaterialOverride>& overrides)
    {
        // r.images only decodes DonTopo::Mesh::material (the singular field,
        // not SkinnedMesh::materials), so only the index 0 override affects
        // it; see the big comment in runJob(), further down. A different index
        // has nothing to discard here.
        for (const MaterialOverride& ov : overrides)
        {
            if (ov.index != 0) continue;
            // The .mat counts the same as an own override: if it provides that
            // texture, the one decoded from the FBX is no longer the one that will be used.
            MaterialAsset matAsset;
            if (!ov.matAsset.empty()) matAsset = loadMaterialAsset(ov.matAsset);
            if (!ov.albedo.empty() || !matAsset.albedo.empty())
                std::erase_if(images, [](const DecodedImage& d) { return d.slot == DecodedImage::Albedo; });
            if (!ov.normal.empty() || !matAsset.normal.empty())
                std::erase_if(images, [](const DecodedImage& d) { return d.slot == DecodedImage::Normal; });
            if (!ov.orm.empty() || !matAsset.orm.empty())
                std::erase_if(images, [](const DecodedImage& d) { return d.slot == DecodedImage::ORM; });
        }
    }

    JobSystem::JobId AsyncAssetLoader::requestMesh(const std::string& path, uint64_t targetId, int piece)
    {
        // The id is reserved BEFORE touching the group: the first waiter of a path
        // enqueues the job with THIS id, and the group stores it so it can
        // cancel it later even if its original waiter disappears. Reserving
        // outside the lock is safe: reserveId() takes the JobSystem's lock, not
        // ours.
        const JobSystem::JobId id = m_jobs.reserveId();

        bool needsJob = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_pending;

            PendingGroup& group = m_groups[path];
            // The first one to request a path starts the job; those that arrive
            // while it is still in flight join the same one. The dominant cost is
            // Assimp's ReadFile, so deduplicating it captures almost all the
            // gain even if the Mesh is then copied per target.
            needsJob = group.waiters.empty();
            if (needsJob)
                group.jobId = id;
            group.waiters.push_back({ id, targetId, piece });
        }

        if (needsJob)
        {
            // path by copy: by reference it would dangle as soon as the caller
            // left scope, and it would not show until the worker started.
            m_jobs.submitWithId(id, [this, path] { runJob(path); });
        }
        return id;
    }

    void AsyncAssetLoader::cancel(JobSystem::JobId id)
    {
        // cancel() only receives an id, with no path: its waiter has to be located
        // by walking the groups under the lock. The groups are few (the loader
        // is drained every frame) and the only caller of cancel(id) is the test
        // (production cancels in bulk with cancelAllPending()), so a linear
        // sweep is plenty.
        //
        // cancel() vs runJob() race, resolved by m_mutex: a waiter leaves
        // m_pending through EXACTLY ONE of two mutually exclusive paths,
        // both under this mutex:
        //   1) runJob() takes the waiters out of the group (move + erase) -> then
        //      pumpCompleted() delivers its result and decrements THERE.
        //   2) cancel() finds the waiter still in its group -> removes it and
        //      decrements HERE; that target produces no result.
        // Since the move-out in runJob() and the erase in cancel() both happen
        // under m_mutex, a waiter either is still in the group (case 2) or is already in
        // results (case 1), never both, with no tombstones or double
        // decrement. That is why this design can do without the m_started /
        // m_cancelledBeforeStart of Task 2.
        JobSystem::JobId jobToCancel = 0;
        bool             cancelJob   = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto it = m_groups.begin(); it != m_groups.end(); ++it)
            {
                PendingGroup& group = it->second;
                auto w = std::find_if(group.waiters.begin(), group.waiters.end(),
                                      [id](const Waiter& waiter) { return waiter.job == id; });
                if (w == group.waiters.end())
                    continue;

                group.waiters.erase(w);
                --m_pending;

                if (group.waiters.empty())
                {
                    // Nobody else is waiting for this ReadFile: we can try to stop the
                    // job (best-effort; if it already started, JobSystem ignores it and
                    // runJob() will end up finding the group empty/absent and will not
                    // build anything). If waiters remain, the job MUST keep going:
                    // the others need the ReadFile.
                    jobToCancel = group.jobId;
                    cancelJob   = true;
                    m_groups.erase(it);
                }
                break;
            }
            // id not found in any group: the result was already built and
            // moved to results/mailbox. pumpCompleted() owns that
            // decrement; m_pending is not touched here.
        }

        // m_jobs.cancel() takes the JobSystem's lock, not ours. Calling it
        // inside our lock would be a crossed acquisition order with the
        // worker (which takes the JobSystem's first and then ours):
        // classic deadlock. That is why it is done OUTSIDE the lock.
        if (cancelJob)
            m_jobs.cancel(jobToCancel);
    }

    LoadedMesh AsyncAssetLoader::buildResultFor(const LoadedMesh& src, const StaticModel* model,
                                                const Waiter& w,
                                                const std::vector<DecodedImage>* decodedImages,
                                                const std::vector<std::shared_ptr<const Mesh>>& pieceMeshes)
    {
        LoadedMesh out;
        out.job      = w.job;
        out.targetId = w.targetId;
        out.path     = src.path;
        out.error    = src.error;
        out.piece    = w.piece;

        if (model)
        {
            // Static: src.mesh is not used (runJob does not fill it in for this
            // path). The runJob error (e.g. "has no meshes") takes
            // priority over the range one: checking it BEFORE avoids overwriting a
            // more precise message with "has no piece 0" when the file
            // does not even contain meshes.
            if (!src.error.empty())
                return out;
            if (w.piece < 0 || static_cast<size_t>(w.piece) >= model->meshes.size())
            {
                out.error = "'" + src.path + "' has no piece " + std::to_string(w.piece);
                return out;
            }
            // OWN copy of the Mesh for this waiter: the same ownership contract as
            // the character branch further down (two GameObjects cannot
            // share a mutable Mesh). decodedImages, on the other hand, already
            // comes computed by runJob ONCE per distinct piece; here
            // only the pixel vector is copied, it is never decoded again.
            out.mesh = std::make_shared<Mesh>(model->meshes[w.piece]);
            if (decodedImages) out.images = *decodedImages;   // copy: each waiter uploads its own texture
            if (model->pieces.size() > 1)
            {
                out.pieces      = model->pieces;   // small copy: a few occurrences
                // Copy of the VECTOR OF POINTERS, not of the meshes: pieceMeshes
                // already holds the built Meshes (once per job, in runJob) and
                // here they are shared via shared_ptr among all the waiters of the
                // group, instead of duplicating the file's N meshes for each
                // of the N waiters.
                out.pieceMeshes = pieceMeshes;
            }
            return out;
        }

        out.images = src.images;   // copy: each target uploads its own texture

        // Deep copy of the Mesh, not of the shared_ptr. Sharing it would leave two
        // GameObjects pointing at the same mutable Mesh, changing the ownership
        // semantics that exist today in Scene.cpp:721 (one make_shared per node).
        // It would not save VRAM either: addStaticMesh uploads each Mesh to its own pair
        // of buffers.
        if (src.mesh)
        {
            if (const SkinnedMesh* sk = dynamic_cast<const SkinnedMesh*>(src.mesh.get()))
                out.mesh = std::make_shared<SkinnedMesh>(*sk);
            else
                out.mesh = std::make_shared<Mesh>(*src.mesh);
        }
        return out;
    }

    void AsyncAssetLoader::runJob(const std::string& path)
    {
        LoadedMesh loaded;
        loaded.path = path;
        std::shared_ptr<StaticModel> model;   // only if the file has no bones

        try
        {
            if (ModelLoader::hasBones(path))
            {
                // Character: whole, as always. SkinnedMesh keeps its
                // textures per submesh in materials[] (plural), which is deliberately not
                // touched here; the decision is deferred (see the comment of
                // testTexturesArriveDecoded). loaded.images stays empty and the
                // texture is resolved on the main thread via the existing
                // synchronous path (the buildRenderObject fallback, Task 6).
                // loadSkinned directly, not loadAuto: hasBones was already asked
                // above, and loadAuto would repeat it (another full ReadFile).
                loaded.mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned(path));
                if (!loaded.mesh) loaded.error = "Could not load the model: " + path;
            }
            else
            {
                // Static: ONE ReadFile for all the file's meshes,
                // regardless of how many pieces are waiting. Each
                // waiter decodes its own texture in buildResultFor.
                model = std::make_shared<StaticModel>(ModelLoader::loadStatic(path));
                if (model->meshes.empty()) loaded.error = "'" + path + "' has no meshes";
            }
        }
        catch (const std::exception& e)
        {
            // An exception cannot cross the thread boundary: escaping from a
            // worker is std::terminate. It travels as a string.
            loaded.mesh  = nullptr;
            loaded.error = e.what();
            model        = nullptr;
        }
        catch (...)
        {
            loaded.mesh  = nullptr;
            loaded.error = "Unknown error loading " + path;
            model        = nullptr;
        }

        std::vector<Waiter> waiters;
        uint64_t myEpoch = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_readFileCount;
            myEpoch = m_epoch;   // current generation when the waiters are taken out
            auto it = m_groups.find(path);
            if (it != m_groups.end())
            {
                // Taking the waiters out under the lock closes the race with
                // cancel(): from here on that group no longer exists, so a later
                // cancel() will not find the id and will not touch m_pending
                // (pumpCompleted will do it on delivery).
                waiters = std::move(it->second.waiters);
                m_groups.erase(it);
            }
            // If the group is no longer there, cancel() emptied and erased the group before
            // this job (already started, uncancelable) got here: there are no waiters
            // to serve.
        }

        // The copies (and the decoding) are done OUTSIDE the lock: with
        // dozens of objects of the same path, the main thread would end up
        // waiting on the mutex just while it tries to draw.
        //
        // Two caches LOCAL to this job, built ONCE and shared by
        // ALL the waiters (never once per waiter):
        //  - decodedByPiece: if two waiters ask for the SAME piece (100
        //    instances of the same static model), decoding its texture only
        //    once and copying the already decoded pixel vector is much
        //    cheaper than decoding N times; stbi_load is half the
        //    cost of loading a model (see the DecodedImage comment in
        //    the header).
        //  - pieceMeshesShared: the vector of pointers to ALL the meshes of the
        //    file (only if pieces.size() > 1) is built once; each
        //    waiter receives a copy of the shared_ptr VECTOR, which shares
        //    the Meshes (const, immutable) instead of duplicating them. With a model
        //    of 200 pieces loaded as 200 children, building the 200 meshes
        //    per waiter would be 40000 copies; this way it is 200, shared.
        std::unordered_map<int, std::vector<DecodedImage>> decodedByPiece;
        std::vector<std::shared_ptr<const Mesh>>            pieceMeshesShared;
        if (model && loaded.error.empty() && model->pieces.size() > 1)
            for (const Mesh& m : model->meshes)
                pieceMeshesShared.push_back(std::make_shared<const Mesh>(m));

        std::vector<LoadedMesh> results;
        results.reserve(waiters.size());
        for (const Waiter& w : waiters)
        {
            const std::vector<DecodedImage>* decoded = nullptr;
            if (model && loaded.error.empty()
                && w.piece >= 0 && static_cast<size_t>(w.piece) < model->meshes.size())
            {
                auto [it, inserted] = decodedByPiece.try_emplace(w.piece);
                if (inserted)
                {
                    const Material& mat = model->meshes[w.piece].material;
                    decodeSlot(mat.texturePath,           mat.embeddedTexture,           DecodedImage::Albedo, it->second);
                    decodeSlot(mat.normalMapPath,         mat.embeddedNormalMap,         DecodedImage::Normal, it->second);
                    decodeSlot(mat.metallicRoughnessPath, mat.embeddedMetallicRoughness, DecodedImage::ORM,    it->second);
                }
                decoded = &it->second;
            }
            results.push_back(buildResultFor(loaded, model.get(), w, decoded, pieceMeshesShared));
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        // If a cancelAllPending() happened while we were copying outside the lock, the
        // generation changed: these waiters were already cancelled (their m_pending was
        // set to 0 there) and their targets are garbage. Discard the results;
        // posting them would leave m_pending negative forever at the next
        // pumpCompleted (-= out.size()) and would deliver meshes for dead objects.
        if (myEpoch != m_epoch)
            return;
        for (auto& r : results)
            m_inbox.push_back(std::move(r));
    }

    std::vector<LoadedMesh> AsyncAssetLoader::pumpCompleted(float budgetMs)
    {
        std::vector<LoadedMesh> ready;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_inbox.empty()) return ready;
            // Swap-and-drain: everything is taken out under the lock and processed outside. With
            // the budget exhausted, what is left goes back to the mailbox; it is never
            // discarded.
            ready.swap(m_inbox);
        }

        const auto start = std::chrono::steady_clock::now();
        std::vector<LoadedMesh> out;
        std::vector<LoadedMesh> leftover;

        for (auto& r : ready)
        {
            const float elapsedMs = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - start).count();

            // The budget is checked BEFORE accepting each element. With
            // budgetMs == 0 none comes out, which is exactly what the test asks for.
            if (elapsedMs >= budgetMs && !out.empty())
            {
                leftover.push_back(std::move(r));
                continue;
            }
            if (budgetMs <= 0.0f)
            {
                leftover.push_back(std::move(r));
                continue;
            }
            out.push_back(std::move(r));
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& r : leftover)
                m_inbox.push_back(std::move(r));
            // One decrement per DELIVERED result. Each result was born from a
            // waiter that added +1 in requestMesh() and that runJob() took out of its
            // group (cancel() never cancelled it, or it would not be here). Waiters
            // cancelled before being built already decremented in cancel() and do not
            // reach 'out'. No tombstones: the group-vs-result exclusion
            // under m_mutex guarantees there is no double counting (see cancel()).
            m_pending -= static_cast<int>(out.size());
        }
        return out;
    }

    int AsyncAssetLoader::pending() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pending;
    }

    int AsyncAssetLoader::readFileCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_readFileCount;
    }

    bool applyLoadedMesh(LoadedMesh& r, Scene& scene, EditorRenderer& renderer,
                         std::string* outError, std::vector<std::string>* outWarnings)
    {
        // Live traversal, not a cached list: the editor allows deleting
        // GameObjects on any frame, so a pointer stored in the
        // request would dangle. Same reason as the liveCube in main.cpp:293.
        GameObject* target = nullptr;
        scene.traverse([&](GameObject* go) { if (go->id == r.targetId) target = go; });

        // Deleted while loading: the worker's work is thrown away and that is it. Without
        // touching freed memory, which is exactly what resolving by id avoids.
        if (!target) return false;

        target->pendingMeshJob = 0;

        if (!r.error.empty())
        {
            if (outError) *outError = "Error loading '" + r.path + "': " + r.error;
            return false;
        }
        if (!r.mesh) return false;

        // ENFORCED precondition, not just documented: today's two callers
        // (PropertiesPanel::loadMeshForSelected, and the async branch of
        // Scene::nodeFromJson when it enqueues loader->requestMesh) enqueue the
        // request with the target still having no mesh, but nothing forces that to
        // stay true tomorrow. Saving the previous mesh here and restoring it
        // in the catch covers the failure AND stops being a contract that the
        // next caller could break without anything revealing it.
        const std::shared_ptr<const Mesh> previousMesh = target->getMesh();

        // The overrides overwrite mesh.material further down (applyMaterialOverrides),
        // but r.images still carries the pixels that decodeSlot took from the FBX
        // IN THE WORKER (runJob(), further up in this file), BEFORE
        // anyone overwrote anything. Renderer::createSharedGpuMesh (Vulkan) PREFERS
        // those already decoded pixels over the material's path: without this
        // filter, an override on an FBX that carries its own texture (the
        // normal case) would upload the FBX's to the GPU despite the override, exactly the
        // symptom that the reordering below claims to avoid. Worse
        // still: the SharedGpuMesh key DOES read the already overwritten path
        // (makeSharedMeshKey), so the entry would be registered with the
        // override's key but the FBX's pixels inside; any other
        // object that shares the FBX and the same override would reuse that
        // poisoned entry, and not even rebuildStaticMesh fixes it (its acquire()
        // finds the key already alive and does not upload a single byte). D3D12 does not suffer from it
        // (its addStaticMesh ignores the decoded images parameter)
        // but the filter is applied here, before calling any backend,
        // so that the result does not depend on which one is active.
        //
        // Order: setMesh -> applyMaterialOverrides -> decoded filter ->
        // registration in the Renderer. BEFORE, the order was registration -> setMesh (the
        // registration went first so that, if it threw, target->setMesh would never
        // get executed and the GameObject would stay intact). It is reversed
        // because addSkinnedMesh/addStaticMesh UPLOAD TO THE GPU the material and the
        // decoded images as they are at THAT instant; the filter
        // has to go AFTER applyMaterialOverrides (to know what the
        // override overwrote) and BEFORE the registration (so that what is filtered never
        // gets uploaded). applyMaterialOverrides operates on the GameObject (reads
        // target->materialOverrides and writes into target->getMesh()->material),
        // so it needs setMesh already done; it cannot go loose on
        // r.mesh before target is linked.
        //
        // The guarantee of "GameObject intact if the registration throws" is
        // kept by reversing the repair instead of the order: in the catch the
        // previous mesh is restored (target->setMesh(previousMesh), saved
        // above) instead of assuming it was always nullptr.
        try
        {
            target->setMesh(r.mesh);
            // BEFORE applying, which is when `mats` still describes what
            // the user saved: the warning only needs the number of materials
            // of the mesh just set, and applyMaterialOverrides does not change that
            // number, so the order does not matter for the content; it is placed
            // here because reading it right next to setMesh says which mesh it talks about.
            if (outWarnings)
                collectMaterialOverrideWarnings(*target, *outWarnings);
            applyMaterialOverrides(*target);
            discardOverriddenDecodedImages(r.images, target->materialOverrides);

            if (SkinnedMesh* sk = dynamic_cast<SkinnedMesh*>(r.mesh.get()))
                target->skinnedRenderIndex = renderer.addSkinnedMesh(*sk, &r.images);
            else
                target->staticRenderIndex  = renderer.addStaticMesh(*r.mesh, &r.images);
        }
        catch (const std::exception& e)
        {
            target->setMesh(previousMesh);
            if (outError) *outError = std::string("Error uploading to the GPU '") + r.path + "': " + e.what();
            return false;
        }
        return true;
    }

    void AsyncAssetLoader::cancelAllPending()
    {
        std::vector<JobSystem::JobId> toCancel;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // Generation bump: an already started (uncancelable) job that took out
            // its waiters BEFORE this bump and is still copying outside the
            // lock will see the epoch changed when it goes to post and will discard its
            // results (see runJob()). Without this, those posts would leave
            // m_pending negative forever and would deliver cancelled meshes.
            ++m_epoch;
            // Only the id enqueued per group: the ids reserved by non-first
            // waiters were never sent to the JobSystem, so cancelling them would only
            // dirty its m_cancelled set (which is not cleaned until a job
            // with that id is taken off the queue, which would never happen).
            for (const auto& [path, group] : m_groups)
                toCancel.push_back(group.jobId);
            m_groups.clear();
            m_inbox.clear();
            m_pending = 0;
        }

        // cancel() takes the JobSystem's lock, not ours. Calling it inside
        // our lock would be a crossed acquisition order with the worker, which
        // takes the JobSystem's first and then ours: classic deadlock.
        for (JobSystem::JobId id : toCancel)
            m_jobs.cancel(id);
    }
}
