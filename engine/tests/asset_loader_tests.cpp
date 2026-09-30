// Tests of AsyncAssetLoader. Headless: it does not create a Vulkan device, it only checks
// that the worker produces the same data in RAM as the synchronous path.
//
// The assertions compare against ModelLoader::load / loadAuto, NOT against
// hardcoded constants: if the Assimp flags change tomorrow, the test remains
// valid instead of turning into a constant to readjust.
//
// Each case runs kIters times, same as jobsystem_tests.cpp: a race that
// shows up 1 out of 20 runs is not caught in a single pass.
#include "DonTopo/Core/JobSystem.h"
#include "DonTopo/Renderer/AsyncAssetLoader.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "gltf_fixtures.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kIters = 50;

// Path to the test FBX. The test skips the cases that depend on it if it does not
// exist, instead of failing: a clone without assets must be able to run the rest.
//
// There is no assets/models/cube.fbx in the repo; assets/modelTexture.fbx does exist
// and also carries an albedo texture, which exercises testTexturesArriveDecoded.
// Several depths are tried because the test exe runs headless from
// build-ninja/engine/tests/.
std::string findTestFbx()
{
    for (const char* rel : { "assets/modelTexture.fbx", "../assets/modelTexture.fbx",
                             "../../assets/modelTexture.fbx", "../../../assets/modelTexture.fbx",
                             "../../../../assets/modelTexture.fbx",
                             "assets/model.fbx", "../assets/model.fbx",
                             "../../assets/model.fbx", "../../../assets/model.fbx",
                             "../../../../assets/model.fbx" })
        if (std::filesystem::exists(rel)) return rel;
    return {};
}

// Pumps until `expected` results arrive or the deadline runs out. Returns
// what was collected. Without a deadline, a loader failure would hang the test forever.
std::vector<DonTopo::LoadedMesh> drain(DonTopo::AsyncAssetLoader& loader,
                                       size_t expected, int timeoutMs = 30000)
{
    std::vector<DonTopo::LoadedMesh> out;
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeoutMs);
    while (out.size() < expected && std::chrono::steady_clock::now() < deadline)
    {
        for (auto& r : loader.pumpCompleted(1000.0f))
            out.push_back(std::move(r));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return out;
}

// The mesh that comes out of the worker is the same one the synchronous path gives.
// Sabotage: in the job, replace loadAuto(path) with loadAuto(path) and empty
// mesh->indices; the indices assert fires.
void testAsyncMatchesSync(const std::string& fbx)
{
    for (int it = 0; it < kIters; ++it)
    {
        const std::shared_ptr<DonTopo::Mesh> expected = DonTopo::ModelLoader::loadAuto(fbx);
        assert(expected && "la ruta sincrona debe cargar el FBX de pruebas");

        DonTopo::JobSystem js;
        js.start();
        DonTopo::AsyncAssetLoader loader(js);

        loader.requestMesh(fbx, /*targetId=*/42);
        std::vector<DonTopo::LoadedMesh> got = drain(loader, 1);

        assert(got.size() == 1 && "el buzon debe entregar exactamente un resultado");
        assert(got[0].error.empty() && "un FBX valido no debe reportar error");
        assert(got[0].targetId == 42 && "el targetId debe viajar intacto");
        assert(got[0].mesh != nullptr);
        assert(got[0].mesh->vertices.size() == expected->vertices.size());
        assert(got[0].mesh->indices.size()  == expected->indices.size());
        assert(got[0].mesh->name            == expected->name);

        js.shutdown();
    }
}

// A nonexistent path does NOT throw: it returns a non-empty error and a null mesh.
// Sabotage: remove the job's try/catch; the process dies from std::terminate (or, with
// the JobSystem::workerLoop safety net already in place, the result never
// reaches the mailbox and drain() exhausts the timeout: the size assert fires anyway).
void testMissingFileReportsError()
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start();
        DonTopo::AsyncAssetLoader loader(js);

        loader.requestMesh("no/existe/ningun/fichero.fbx", 7);
        std::vector<DonTopo::LoadedMesh> got = drain(loader, 1);

        assert(got.size() == 1);
        assert(!got[0].error.empty() && "un path invalido debe llenar error");
        assert(got[0].mesh == nullptr && "sin mesh cuando hay error");

        js.shutdown();
    }
}

// pumpCompleted(0) processes nothing AND loses nothing: the next pump delivers
// everything. Sabotage: make pumpCompleted empty the mailbox before looking at the
// budget; the second drain stays at zero and the test hangs until the
// timeout, failing the size assert.
//
// kIters at 50 like the rest of the concurrency cases (pump/leftover/cancel):
// the previous version waited for "pending() == 0" before touching
// pumpCompleted, but pending() ONLY decrements when pumpCompleted delivers
// something (see the AsyncAssetLoader header); that while never exited before the
// deadline, so each pass burned a fixed 30s by design, not because of the real cost
// of the assertion. The fix is to detect "it is ready" by CONSUMING with
// a real budget (pumpCompleted(1000ms)) inside the wait loop itself,
// instead of looking at pending(): each iteration ends as soon as the
// worker posts, on the order of milliseconds, and 50 passes end up dominated
// only by 50 real loads of the FBX.
void testZeroBudgetKeepsResults(const std::string& fbx)
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start();
        DonTopo::AsyncAssetLoader loader(js);

        loader.requestMesh(fbx, 1);

        bool everReady = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            // Budget 0: ALWAYS empty, whether this result is already ready or
            // not. It is the central assertion of the test, checked on EVERY
            // turn of the polling, not just once at the end.
            assert(loader.pumpCompleted(0.0f).empty() && "presupuesto 0 no procesa nada");

            // Detects that the worker has already finished by consuming with a real
            // budget. If something arrives, it is the result we were looking for: neither
            // pumpCompleted(0) returned it earlier (assert above) nor
            // lost it (otherwise this pump would stay empty forever and the
            // everReady assert further down would fire when the deadline runs out).
            std::vector<DonTopo::LoadedMesh> got = loader.pumpCompleted(1000.0f);
            if (!got.empty())
            {
                assert(got.size() == 1 && "un pump con presupuesto 0 no puede perder resultados");
                everReady = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(everReady && "el resultado debe llegar; presupuesto 0 no puede perderlo");

        js.shutdown();
    }
}

// The material textures arrive DECODED from the worker. It is the point
// of the feature: if stbi_load stayed on the main thread, most of the
// gain would be lost. Sabotage: in the job, do not fill images; the
// w/h assert fires.
//
// It only applies if the test FBX carries a texture; otherwise the case is skipped.
//
// NOTE: the only two FBX tracked in this repo (model.fbx and
// modelTexture.fbx) are Mixamo characters WITH a rig; ModelLoader::loadAuto
// always returns a SkinnedMesh for them, and SkinnedMesh stores its
// textures in materials[] (plural, per submesh), not in the Mesh::material
// (singular) that decodeSlot() reads here. With the assets of this repo this
// case is always skipped; see the comment of runJob() in
// AsyncAssetLoader.cpp and the Task 2 report (documented finding,
// decision: deferred).
void testTexturesArriveDecoded(const std::string& fbx)
{
    for (int it = 0; it < kIters; ++it)
    {
        const std::shared_ptr<DonTopo::Mesh> sync = DonTopo::ModelLoader::loadAuto(fbx);
        if (sync->material.texturePath.empty() && sync->material.embeddedTexture.empty())
        {
            if (it == 0) std::printf("  (saltado: el FBX de pruebas no trae textura)\n");
            continue;
        }

        DonTopo::JobSystem js;
        js.start();
        DonTopo::AsyncAssetLoader loader(js);

        loader.requestMesh(fbx, 3);
        std::vector<DonTopo::LoadedMesh> got = drain(loader, 1);
        assert(got.size() == 1 && got[0].error.empty());

        bool foundAlbedo = false;
        for (const auto& img : got[0].images)
            if (img.slot == DonTopo::DecodedImage::Albedo)
            {
                foundAlbedo = true;
                assert(img.w > 1 && img.h > 1 && "una textura real no es 1x1");
                assert(img.pixels.size() == static_cast<size_t>(img.w) * img.h * 4
                       && "los pixeles llegan en RGBA8, sin padding");
            }
        assert(foundAlbedo && "el worker debe decodificar el albedo, no dejarlo al main");

        js.shutdown();
    }
}

// cancel() of a request still in the queue must not leave pending() hanging
// forever. With only 1 worker and a blocking job ahead of it, the
// requestMesh() request cannot have started when cancel() arrives:
// JobSystem skips it in workerLoop (same as testCancelPreventsQueuedJob in
// jobsystem_tests.cpp) and AsyncAssetLoader::runJob() is never executed for that
// id; nobody else was going to decrement m_pending. Sabotage: in cancel(), do not touch
// m_pending (leave only m_jobs.cancel(id)); pending() stays at 1 forever
// and the last assert fires.
void testCancelBeforeStartDropsPending()
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start(1);   // 1 thread: guarantees that the request stays in the queue

        std::atomic<bool> release{false};
        js.submit([&release] { while (!release.load(std::memory_order_acquire)) {} });

        DonTopo::AsyncAssetLoader loader(js);
        assert(loader.pending() == 0 && "sin peticiones, pending() debe empezar en 0");

        const DonTopo::JobSystem::JobId id =
            loader.requestMesh("no/importa/no/deberia/cargar/nunca.fbx", 99);
        assert(loader.pending() == 1 && "requestMesh debe contar como pendiente de inmediato");

        loader.cancel(id);

        release.store(true, std::memory_order_release);
        js.shutdown();   // drains the queue: the cancelled job is skipped there

        // Nothing must arrive for this id: the worker never executed it.
        std::vector<DonTopo::LoadedMesh> got = loader.pumpCompleted(1000.0f);
        assert(got.empty() && "un job cancelado antes de arrancar no debe entregar resultado");
        assert(loader.pending() == 0 &&
               "cancel() debe liberar el contador de pending, no dejarlo colgado");
    }
}

// Four requests for the same path with different targetIds share ONE ReadFile
// and produce four LoadedMesh with identical content but DISTINCT pointers.
//
// Both sides matter. Checking only the content would also pass while
// sharing the shared_ptr, which is exactly what the design rules out: today each
// node of Scene::nodeFromJson has its own make_shared<Mesh> (Scene.cpp:721),
// and two GameObjects over the same mutable Mesh would change that semantics.
//
// Fresh JobSystem+loader per iteration, like the rest of the cases: readFileCount()
// is cumulative per loader, so with a new loader per turn the
// "== 1" assertion holds on every pass. Four deduplicated requests = a single
// ReadFile per iteration, cheaper than testAsyncMatchesSync (which loads twice
// per turn), so the 50 iterations do not penalize.
//
// Sabotage: return the same shared_ptr to all four (remove the copy in
// buildResultFor); the distinct pointers assert fires.
void testDedupSharesReadFileNotPointers(const std::string& fbx)
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start();
        DonTopo::AsyncAssetLoader loader(js);

        for (uint64_t t = 100; t < 104; ++t)
            loader.requestMesh(fbx, t);

        std::vector<DonTopo::LoadedMesh> got = drain(loader, 4);
        assert(got.size() == 4 && "cuatro peticiones, cuatro resultados");

        assert(loader.readFileCount() == 1 && "cuatro peticiones del mismo path = un solo ReadFile");

        std::vector<uint64_t> targets;
        for (const auto& r : got)
        {
            assert(r.error.empty());
            assert(r.mesh != nullptr);
            assert(r.mesh->vertices.size() == got[0].mesh->vertices.size());
            assert(r.mesh->indices.size()  == got[0].mesh->indices.size());
            targets.push_back(r.targetId);
        }

        // The four targetIds are distinct and all four expected ones are there.
        std::sort(targets.begin(), targets.end());
        assert((targets == std::vector<uint64_t>{100, 101, 102, 103}));

        // And the pointers are NOT shared.
        for (size_t i = 0; i < got.size(); ++i)
            for (size_t k = i + 1; k < got.size(); ++k)
                assert(got[i].mesh.get() != got[k].mesh.get()
                       && "cada target recibe su propia copia del Mesh");

        js.shutdown();
    }
}

// Race of cancelAllPending() vs runJob posting results. With N waiters of the
// same path, runJob takes the N out of the group and increments readFileCount under the
// lock (section 1), COPIES the N meshes OUTSIDE the lock, and re-locks to post
// (section 2). If cancelAllPending() lands between the two sections, those results
// belong to targets already cancelled (their m_pending was set to 0 in the cancel): posting
// would leave pending() negative FOREVER (the loader is long-lived, one per
// Renderer, and pending() is the modal's "load finished" signal) and
// would deliver meshes of dead objects. The epoch counter discards them.
//
// It synchronizes on readFileCount()==1 (which runJob increments in section
// 1, under the lock, right before copying) to call cancelAllPending()
// while the job is copying: this way the window is attacked reliably instead
// of by blind luck. Even so, kIters=50 covers the interleavings in which the job
// finishes copying earlier (mailbox cleaned by the cancel, also correct).
//
// Invariant after a bulk cancel with no new requests: NOTHING is delivered and
// pending() stays at 0, never negative.
//
// Sabotage: remove the epoch check in section 2 of runJob (always
// post); in the turns that fall in the window, pumpCompleted delivers the
// orphans and makes pending() negative; both asserts fire.
void testCancelAllPendingDropsInflightResults(const std::string& fbx)
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start();
        DonTopo::AsyncAssetLoader loader(js);

        constexpr uint64_t kWaiters = 40;
        for (uint64_t t = 0; t < kWaiters; ++t)
            loader.requestMesh(fbx, t);

        // Wait for section 1 of runJob to run (readFileCount goes to 1):
        // from there on the job is copying the meshes outside the lock, which
        // is the window we want to attack.
        const auto d1 = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (loader.readFileCount() == 0 && std::chrono::steady_clock::now() < d1)
            std::this_thread::sleep_for(std::chrono::microseconds(50));

        // Cancel in bulk, ideally while the job is still copying.
        loader.cancelAllPending();

        // After the cancel, with no new requests: NOTHING must be delivered and
        // pending() can never end up negative (or different from 0). It is checked
        // on EVERY pump, not just at the end.
        const auto d2 = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < d2)
        {
            std::vector<DonTopo::LoadedMesh> got = loader.pumpCompleted(1000.0f);
            assert(got.empty() && "tras cancelAllPending no debe entregarse ningun resultado");
            assert(loader.pending() == 0 &&
                   "pending() debe quedar en 0, nunca negativo, tras cancelAllPending");
            if (js.idle()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // Drain the pool completely and reconfirm: no late result, and
        // pending() is still 0.
        js.shutdown();
        std::vector<DonTopo::LoadedMesh> tail = loader.pumpCompleted(1000.0f);
        assert(tail.empty() && "nada tardio debe llegar tras shutdown");
        assert(loader.pending() == 0 && "pending() final debe ser 0, nunca negativo");
    }
}

// Two pieces of the same file: ONE ReadFile, each object its piece, and the list of
// occurrences travels with the result.
void testPiecesShareOneReadFile()
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "dt_loader_pieces";
    std::filesystem::create_directories(dir);
    const std::string gltf = (dir / "casa.gltf").string();
    dt_fixture::writeThreePieceGltf(gltf);

    DonTopo::JobSystem js;
    js.start();
    DonTopo::AsyncAssetLoader loader(js);
    loader.requestMesh(gltf, 10, 0);
    loader.requestMesh(gltf, 11, 1);
    std::vector<DonTopo::LoadedMesh> got = drain(loader, 2);
    assert(got.size() == 2);
    assert(loader.readFileCount() == 1 && "dos piezas del mismo fichero = un solo ReadFile");
    for (const auto& r : got)
    {
        assert(r.error.empty());
        assert(r.mesh != nullptr);
        const int esperada = r.targetId == 11 ? 1 : 0;
        assert(r.piece == esperada && r.mesh->piece == esperada);
        assert(r.pieces.size() == 3);
        assert(r.pieceMeshes.size() == 2);
    }
    js.shutdown();
}

// Two waiters of the SAME piece share the decoding (one decoded
// texture, not two) but NOT the Mesh (each one has its own copy,
// same contract as the character branch), and all the waiters of the group, regardless
// of their piece, share the SAME pointers in pieceMeshes (they are
// built once per job, not once per waiter).
//
// Task 3 sabotage (cost review): share the shared_ptr<Mesh>
// between the two waiters of piece 0 instead of copying it; the mesh
// distinct pointers assert fires. Or: rebuild pieceMeshes per waiter
// instead of sharing the vector; the pieceMeshes equal pointers assert
// fires (they share CONTENT but not IDENTITY).
void testSamePieceSharesDecodeDistinctMesh()
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "dt_loader_pieces";
    std::filesystem::create_directories(dir);
    const std::string gltf = (dir / "casa.gltf").string();
    dt_fixture::writeThreePieceGltf(gltf);

    DonTopo::JobSystem js;
    js.start();
    DonTopo::AsyncAssetLoader loader(js);
    loader.requestMesh(gltf, 20, 0);   // piece 0, first waiter
    loader.requestMesh(gltf, 21, 0);   // piece 0, SECOND waiter, same piece
    loader.requestMesh(gltf, 22, 1);   // piece 1, for variety
    std::vector<DonTopo::LoadedMesh> got = drain(loader, 3);
    assert(got.size() == 3);
    assert(loader.readFileCount() == 1 && "tres peticiones del mismo fichero = un solo ReadFile");

    const DonTopo::LoadedMesh* r20 = nullptr;
    const DonTopo::LoadedMesh* r21 = nullptr;
    for (const auto& r : got)
    {
        assert(r.error.empty());
        assert(r.mesh != nullptr);
        assert(r.pieces.size() == 3);
        assert(r.pieceMeshes.size() == 2);
        if (r.targetId == 20) r20 = &r;
        if (r.targetId == 21) r21 = &r;
    }
    assert(r20 && r21);

    // Same piece, but each waiter has its OWN Mesh: sharing the
    // pointer would break the usual ownership contract (two GameObjects
    // over the same mutable Mesh).
    assert(r20->mesh.get() != r21->mesh.get()
           && "dos waiters de la misma pieza no deben compartir el Mesh");

    // pieceMeshes, on the other hand, IS shared among all the waiters of the
    // group (it is built once per job): same pointers, not just same
    // content.
    for (size_t i = 0; i < r20->pieceMeshes.size(); ++i)
        assert(r20->pieceMeshes[i].get() == r21->pieceMeshes[i].get()
               && "pieceMeshes debe compartir los mismos Mesh entre waiters del mismo job");

    js.shutdown();
}

} // namespace

int main()
{
    const std::string fbx = findTestFbx();

    testPiecesShareOneReadFile();
    testSamePieceSharesDecodeDistinctMesh();

    testMissingFileReportsError();
    testCancelBeforeStartDropsPending();

    if (fbx.empty())
    {
        std::printf("asset_loader_tests OK (casos con FBX saltados: no se encontro el asset)\n");
        return 0;
    }

    testAsyncMatchesSync(fbx);
    testZeroBudgetKeepsResults(fbx);
    testTexturesArriveDecoded(fbx);
    testDedupSharesReadFileNotPointers(fbx);
    testCancelAllPendingDropsInflightResults(fbx);

    std::printf("asset_loader_tests OK\n");
    return 0;
}
