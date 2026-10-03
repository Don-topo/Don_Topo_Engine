#pragma once
#include "DonTopo/Core/JobSystem.h"
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/TextureImport.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace DonTopo
{
    // A texture already decoded to RGBA8 by the worker. The main thread only
    // does the upload: the stbi_load, which is half the cost of loading a
    // model, already happened elsewhere.
    struct DecodedImage
    {
        enum Slot { Albedo, Normal, ORM };

        Slot                 slot = Albedo;
        int                  w    = 0;
        int                  h    = 0;
        std::vector<uint8_t> pixels;   // w*h*4, RGBA8, no padding
        // Import settings of the texture (sidecar): whoever uploads to the GPU
        // resolves the format with resolveSrgb and uploads the levels.
        ColorSpaceOverride      colorSpace = ColorSpaceOverride::Auto;
        std::vector<TextureMip> mips;
    };

    // Result of a request. It travels by value from the worker to the main thread:
    // no mutable shared pointer, no GameObject*.
    struct LoadedMesh
    {
        JobSystem::JobId                         job      = 0;
        uint64_t                                 targetId = 0;   // GameObject::id, never a pointer
        std::string                              path;
        std::shared_ptr<Mesh>                    mesh;           // may be a SkinnedMesh (loadAuto)
        std::vector<DecodedImage>                images;
        std::string                              error;          // not empty = failed
        int                                       piece = 0;     // which piece of mesh->sourcePath this is
        std::vector<ModelPiece>                  pieces;        // occurrences, if static with > 1
        std::vector<std::shared_ptr<const Mesh>> pieceMeshes;   // all the meshes of the file, if pieces is not empty
    };

    struct MaterialOverride;   // GameObject.h; reference only, see below

    // Removes from `images` the slots that an ACTIVE override (index == 0, the
    // only one decodeSlot gets to populate; see the runJob comment in the
    // .cpp) has overridden. Pure data function, no GPU or Scene, so that the
    // seam between "what the worker decoded" and "what the user asked for" can be
    // tested without EditorRenderer: applyLoadedMesh is its only caller, and it
    // calls it after applyMaterialOverrides and before uploading anything to the GPU, because
    // Renderer::createSharedGpuMesh (Vulkan) PREFERS an already decoded image
    // over the material's path. Without this filter, an override on an FBX
    // that brings its own texture would upload the FBX's to the GPU, not the override's.
    void discardOverriddenDecodedImages(std::vector<DecodedImage>& images,
                                        const std::vector<MaterialOverride>& overrides);

    // Translates asset requests into jobs and stores the results in a mailbox that
    // the main thread drains once per frame.
    //
    // It does not know Vulkan: it produces bytes in RAM. Whoever uploads them is the Renderer.
    class AsyncAssetLoader
    {
        public:
            explicit AsyncAssetLoader(JobSystem& jobs) : m_jobs(jobs) {}
            // Cancels what is still queued and WAITS for the jobs already started:
            // they run runJob() on `this`, so returning earlier would let a worker
            // lock a destroyed mutex and post into a freed mailbox. A cancelled job
            // still queued releases its lease when the worker drops it.
            // The JobSystem must OUTLIVE the loader (or be shut down before it):
            // the wait relies on its workers draining or dropping our jobs.
            ~AsyncAssetLoader();
            AsyncAssetLoader(const AsyncAssetLoader&)            = delete;
            AsyncAssetLoader& operator=(const AsyncAssetLoader&) = delete;

            // targetId is the GameObject::id to assign the mesh to. The pump
            // resolves by id on the live scene: if the object was deleted
            // while loading, the result is discarded without touching freed
            // memory. piece is the piece of the file (ModelLoader::load(path,
            // piece) for the synchronous path); 0 = the whole file or its
            // first/only mesh, same as today.
            // Returns 0 if the JobSystem refused the job (not started or
            // stopping): the request is undone and nothing will be delivered.
            JobSystem::JobId requestMesh(const std::string& path, uint64_t targetId, int piece = 0);

            void cancel(JobSystem::JobId id);

            // Main thread. Returns the ready results, stopping when
            // budgetMs runs out. What is not returned stays in the mailbox for the next
            // frame; it is never discarded because of the budget.
            std::vector<LoadedMesh> pumpCompleted(float budgetMs);

            // Requests not yet collected by pumpCompleted (queued, in flight or
            // in the mailbox). It is what the progress modal reads.
            int pending() const;

            // Tests only: how many real ReadFile calls have been made. It is the
            // only way to check the dedup from outside: counting results
            // does not distinguish "one shared ReadFile" from "four ReadFile".
            int readFileCount() const;

            // Cancels all live requests and empties the mailbox. Called by the
            // Cancel button of the loading modal (Task 9). Jobs already started
            // finish anyway (a ReadFile cannot be stopped halfway), but their
            // results are discarded.
            void cancelAllPending();

            // True while the request `id` (what requestMesh returned) can still
            // deliver a result: queued, in flight, or in the mailbox. False once
            // pumpCompleted handed its result out, after cancel/cancelAllPending,
            // and for 0 or an id this loader never issued. A GameObject whose
            // pendingMeshJob is not live is NOT waiting for anything, whoever
            // forgot to clear its field (cancelAllPending cannot: it has no scene).
            bool isRequestLive(JobSystem::JobId id) const;

        private:
            // Ids of the requests isRequestLive answers true for. Under m_mutex.
            std::unordered_set<JobSystem::JobId> m_liveRequests;
            // Requests grouped by path while the job is in flight. The
            // first one to ask for a path starts ONE job (jobId); those that arrive
            // while it is still in flight are recorded as waiters of the same one. When
            // it finishes, the worker builds a LoadedMesh for each waiter
            // (copying the Mesh) and empties the group.
            //
            // jobId is stored apart from the waiters on purpose: it is the id the
            // job was enqueued with, which stays valid even if its original waiter
            // is cancelled while others keep waiting for the ReadFile.
            //
            // A waiter is no longer just (job, targetId): two requests for the same
            // path can ask for different pieces (two GameObjects of the same
            // static model), and that piece has to travel with the waiter so
            // that buildResultFor knows which of the ReadFile's meshes to serve it.
            struct Waiter { JobSystem::JobId job; uint64_t targetId; int piece; };

            struct PendingGroup
            {
                JobSystem::JobId     jobId = 0;   // id enqueued in the JobSystem (first waiter)
                std::vector<Waiter>  waiters;
            };

            void      runJob(const std::string& path);

            // decodedImages and pieceMeshes already come computed by runJob,
            // ONCE per job, not once per waiter: decodedImages is the
            // texture of THIS waiter's piece (null if not applicable), and
            // pieceMeshes is the vector of ALL the file's meshes,
            // shared (shared_ptr) among all the waiters of the group. This
            // method only copies; it never decodes nor builds new Mesh
            // except the one for w.piece itself, which is a per-waiter copy (see
            // the comment of the character branch, in the .cpp).
            LoadedMesh buildResultFor(const LoadedMesh& src, const StaticModel* model,
                                      const Waiter& w,
                                      const std::vector<DecodedImage>* decodedImages,
                                      const std::vector<std::shared_ptr<const Mesh>>& pieceMeshes);

            JobSystem&              m_jobs;
            mutable std::mutex      m_mutex;
            std::vector<LoadedMesh> m_inbox;
            int                     m_pending = 0;

            std::unordered_map<std::string, PendingGroup> m_groups;
            int                                           m_readFileCount = 0;

            // Bulk cancellation generation. cancelAllPending() increments
            // it; a job already started (uncancellable) captures the
            // generation when taking its waiters out of the group and, when about to post its
            // results, discards them if the generation changed while it was copying
            // outside the lock: those targets were cancelled and their m_pending was already
            // set to 0. Without this, posting after a cancelAllPending would leave
            // m_pending negative forever (the loader is long-lived) and
            // would deliver meshes of objects already cancelled. See runJob().
            uint64_t                                      m_epoch = 0;

            // Jobs handed to the JobSystem whose std::function has not been destroyed
            // yet (queued, running, or about to be dropped as cancelled). Each job
            // captures a JobLease; its destructor decrements under m_mutex, so the
            // count reaches 0 only when no worker can touch `this` any more.
            struct JobLease
            {
                explicit JobLease(AsyncAssetLoader* s) : self(s) {}
                JobLease(const JobLease&)            = delete;   // one lease = one decrement
                JobLease& operator=(const JobLease&) = delete;
                ~JobLease();
                AsyncAssetLoader* self;
            };
            int                                           m_liveJobs = 0;
            std::condition_variable                       m_liveJobsCv;
    };

    class Scene;
    class Renderer;
    class EditorRenderer;

    // Applies a result to the scene, resolving by targetId on the LIVE
    // scene. Returns false if the GameObject no longer exists (deleted while
    // loading) or if the result carries an error; in that case outError, if it is not
    // null, receives the message for the log.
    //
    // It does not call flushPendingUploads: the caller decides when to close the batch,
    // because the point of all this is to group N results into ONE submit.
    //
    // outWarnings is a SEPARATE channel from outError, and not a second message through
    // the same one: a material index out of range does not make the load fail
    // (the mesh goes in anyway and the rest of the overrides are applied), so
    // putting it in outError, which the caller reads as "this failed", would say
    // something that is not true. Null = not interested. This is the normal path of a large
    // scene: without it, the warning that Scene::fromJson does give is lost entirely.
    bool applyLoadedMesh(LoadedMesh& r, Scene& scene, EditorRenderer& renderer,
                         std::string* outError,
                         std::vector<std::string>* outWarnings = nullptr);
}
