#pragma once
#include "DonTopo/Core/FileStamp.h"
#include "DonTopo/Renderer/ThumbnailAtlas.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <istream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace DonTopo {

// More than this many source pixels and decoding is not even attempted: a 16k x
// 16k PNG is 1 GB transient in a worker.
constexpr uint64_t kThumbMaxSourcePixels = 100'000'000;

enum class ThumbnailStatus { Ok, Unreadable, TooLarge, AnimationOnly };

// A file a thumbnail depends on and its state when it was generated.
using ThumbnailDependency = FileStamp;

struct ThumbnailResult
{
    ThumbnailStatus                  status = ThumbnailStatus::Unreadable;
    std::vector<uint8_t>             rgba;   // kThumbCell*kThumbCell*4 if status == Ok; empty if not
    // What the decoder declares, preferably SEALED before reading each
    // file (stampFile). stampDependencies seals whatever arrives unsealed and
    // puts the asset itself first.
    std::vector<ThumbnailDependency> dependencies;
};

inline constexpr float kNeutralAlbedo = 0.6f;   // linear: the gray of an inheriting .mat

// Decodes path and reduces it to ONE kThumbCell x kThumbCell RGBA8 cell:
// keeps the proportion (alpha-weighted box filter), does not enlarge what
// already fits, centers it and leaves the rest transparent. Pure CPU: it is called from a
// worker. Never throws.
ThumbnailResult makeThumbnail(const std::filesystem::path& path);

// The same from a freshly opened stream (at its position 0). It only rewinds
// (seekg) if the image passes the size filter and has to be loaded. It exists
// separately to be able to test that rejecting a huge image costs its HEADER and not
// reading the whole file.
ThumbnailResult makeThumbnailFromStream(std::istream& in);

// Puts `self` (the asset, sealed BEFORE decoding) first, keeps the
// seal of the dependencies the decoder already sealed and seals those that are not.
// Removes duplicates and the asset itself if the decoder repeated it.
void stampDependencies(ThumbnailResult& r, const ThumbnailDependency& self);

// .fbx/.obj: the ones that take seconds to decode (see the ThumbnailCache cap).
bool isModelThumbnailPath(const std::filesystem::path& path);

// Sphere with the .mat's albedo, metallic and roughness; what is inherited, neutral.
ThumbnailResult makeMaterialThumbnail(const std::filesystem::path& mat);

// Decoder by extension: image -> makeThumbnail; .fbx/.obj -> rasterized
// preview; .mat -> sphere. Anything else, Unreadable. Never throws.
ThumbnailResult makeAssetThumbnail(const std::filesystem::path& path);

// Distribution of the atlas cells among keys (one per thumbnail), with
// LRU eviction. "Use" = requesting the cell in the current frame (assign/find).
class ThumbnailSlots
{
public:
    static constexpr uint32_t kNone = 0xFFFFFFFFu;

    explicit ThumbnailSlots(uint32_t capacity = kThumbSlotCount);

    // Once per frame, before any assign/find of that frame.
    void beginFrame() { ++m_frame; }

    // Cell of key: the one it already had (marked as used this frame) or a
    // new one. With no free slot it evicts the least recently used one that will NOT be used
    // this frame (tie: the lowest index) and, if `evicted` is not null, says
    // which. kNone if all cells were used this frame.
    uint32_t assign(uint64_t key, std::optional<uint64_t>* evicted = nullptr);

    // Cell of key without reserving any (and marking it as used); kNone if it is not there.
    uint32_t find(uint64_t key);

    // Frees key's cell, if it had one. Evicts nobody.
    void release(uint64_t key);

    uint32_t capacity() const { return static_cast<uint32_t>(m_slots.size()); }

private:
    struct Slot
    {
        uint64_t key       = 0;
        bool     used      = false;
        uint64_t lastFrame = 0;
    };

    std::vector<Slot>                      m_slots;
    std::unordered_map<uint64_t, uint32_t> m_byKey;
    uint64_t                               m_frame = 1;
};

class ThumbnailDiskCache;

// Orchestrates the thumbnails: requests from the grid, asynchronous decoding and
// upload to the atlas. It does NOT know GPU or JobSystem: it receives a Runner (to launch
// work off the main thread) and an Uploader (to copy cells to the
// atlas), so it is tested entirely without either. All the state lives on
// the main thread; the workers only run makeThumbnail and leave the
// result in a queue with a mutex.
class ThumbnailCache
{
public:
    // Launches the job off the main thread. false = the pool rejected it (stopped):
    // the entry ends up Failed and the in-flight slot is returned.
    using Runner   = std::function<bool(std::function<void()>)>;
    // Copies the batch of cells to the atlas. false = it could not (the whole batch fails).
    using Uploader = std::function<bool(const ThumbnailTile* tiles, size_t count)>;
    // Decodes ONE asset into a cell. By default makeAssetThumbnail; it exists as a
    // parameter to be able to test a decoder that throws.
    using Decoder  = std::function<ThumbnailResult(const std::filesystem::path&)>;

    // disk: optional; the worker consults it before decoding and stores what it
    // decodes. maxModelsInFlight: of the maxInFlight, how many may be
    // models (isModelThumbnailPath), which take seconds.
    ThumbnailCache(Runner run, Uploader upload, uint32_t maxInFlight = 4,
                   uint32_t slotCapacity = kThumbSlotCount, Decoder decode = {},
                   std::shared_ptr<const ThumbnailDiskCache> disk = {},
                   uint32_t maxModelsInFlight = 2);
    ThumbnailCache(const ThumbnailCache&)            = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    // Once per frame, before the request() calls of that frame.
    void beginFrame();

    // Thumbnail of path if it is already in the atlas; nullopt = not yet (pending,
    // failed or evicted) and the requester keeps its icon. The first time
    // a path is requested its decoding is ENQUEUED; it never blocks.
    std::optional<UvRect> request(const std::filesystem::path& path);

    // Collects what was decoded, uploads at most maxUploads cells in ONE call
    // to the Uploader and launches decodes up to maxInFlight. Once per frame.
    void pump(int maxUploads = 8);

    // Re-reads the mtime of what was requested the previous frame or this one and discards what
    // changed, so that it is regenerated. Called by the panel's polling, not every frame.
    void refreshStamps();

    // Folder change: discards what is pending (queued, in flight, decoded but not
    // uploaded). What was already uploaded and what failed is kept; a late result from the
    // previous generation is ignored.
    void newGeneration();

    // Final state of path: Ok if it is in the atlas; the reason if it failed
    // (Unreadable, TooLarge, AnimationOnly); nullopt if it is not known yet.
    std::optional<ThumbnailStatus> status(const std::filesystem::path& path) const;

    // Launched decodes whose result has not been collected yet.
    uint32_t inFlight() const { return m_inFlight; }
    // Of those, the model ones.
    uint32_t modelsInFlight() const { return m_modelsInFlight; }

private:
    enum class State { Queued, Running, Decoded, Ready, Failed };

    struct Entry
    {
        std::filesystem::path            path;
        std::vector<ThumbnailDependency> deps;              // [0] = the asset itself
        uint64_t                         key = 0;
        State                            state = State::Queued;
        ThumbnailStatus                  status = ThumbnailStatus::Ok;   // reason if Failed
        bool                             model = false;     // counts toward the model cap
        std::vector<uint8_t>             pixels;            // solo en Decoded
        uint64_t                         lastRequestFrame = 0;
    };

    struct Done
    {
        uint64_t        generation = 0;
        std::string     path;
        bool            model = false;
        ThumbnailResult result;
    };

    // The only thing the workers share with the main thread. In a shared_ptr:
    // a result that arrives after the cache is destroyed lands here and does not touch dead memory.
    struct Shared
    {
        std::mutex        mutex;
        std::vector<Done> done;
    };

    static uint64_t makeKey(const std::filesystem::path& path, int64_t mtime);
    void            startJobs();

    Runner                                    m_run;
    Uploader                                  m_upload;
    Decoder                                   m_decode;
    std::shared_ptr<const ThumbnailDiskCache> m_disk;
    uint32_t                                  m_maxInFlight;
    uint32_t                                  m_maxModelsInFlight;
    uint32_t                                  m_modelsInFlight = 0;
    uint32_t                                  m_inFlight   = 0;
    uint64_t                               m_frame      = 1;
    uint64_t                               m_generation = 1;
    ThumbnailSlots                         m_slots;
    std::unordered_map<std::string, Entry> m_entries;
    std::deque<std::string>                m_queue;
    std::shared_ptr<Shared>                m_shared = std::make_shared<Shared>();
};

} // namespace DonTopo
