// Headless tests of the Content Browser thumbnails (no GPU or ImGui).
// Plain main + CHECK, same pattern as content_browser_tests.cpp.
#include "DonTopo/Core/ImportSettings.h"
#include "DonTopo/Core/MaterialAsset.h"
#include "DonTopo/Editor/ContentBrowserPanel.h"
#include "DonTopo/Editor/Thumbnail.h"
#include "DonTopo/Editor/ThumbnailDiskCache.h"
#include "DonTopo/Editor/ThumbnailRaster.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <istream>
#include <limits>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <system_error>
#include <vector>

using namespace DonTopo;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

using Rgba = std::array<uint8_t, 4>;

// Uncompressed 32-bit TGA, origin at the top left: the simplest format
// that stb_image reads with alpha, and stb_image_write is not needed.
static void writeTga(const fs::path& p, int w, int h, const std::function<Rgba(int, int)>& pixel)
{
    std::ofstream f(p, std::ios::binary);
    uint8_t hdr[18] = {};
    hdr[2]  = 2;                               // uncompressed truecolor
    hdr[12] = static_cast<uint8_t>(w & 0xFF);
    hdr[13] = static_cast<uint8_t>((w >> 8) & 0xFF);
    hdr[14] = static_cast<uint8_t>(h & 0xFF);
    hdr[15] = static_cast<uint8_t>((h >> 8) & 0xFF);
    hdr[16] = 32;                              // bits per pixel
    hdr[17] = 0x28;                            // origin top-left, 8 bits of alpha
    f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const Rgba c = pixel(x, y);
            const uint8_t bgra[4] = { c[2], c[1], c[0], c[3] };
            f.write(reinterpret_cast<const char*>(bgra), 4);
        }
}

static Rgba pixelAt(const ThumbnailResult& r, uint32_t x, uint32_t y)
{
    const size_t i = (static_cast<size_t>(y) * kThumbCell + x) * 4;
    return { r.rgba[i], r.rgba[i + 1], r.rgba[i + 2], r.rgba[i + 3] };
}

static bool isRed(const Rgba& c)   { return c[0] == 255 && c[1] == 0 && c[2] == 0 && c[3] == 255; }
static bool isBlue(const Rgba& c)  { return c[0] == 0 && c[1] == 0 && c[2] == 255 && c[3] == 255; }
static bool isClear(const Rgba& c) { return c[3] == 0; }

static const Rgba kRed  = { 255, 0, 0, 255 };
static const Rgba kBlue = { 0, 0, 255, 255 };

static fs::path makeDir()
{
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / "dt_thumbnail_test";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

// Landscape 128x64: it is reduced to 64x32 and centered (16 transparent rows above and below).
static void test_landscape_is_letterboxed(const fs::path& dir)
{
    writeTga(dir / "land.tga", 128, 64, [](int, int) { return kRed; });
    ThumbnailResult r = makeThumbnail(dir / "land.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    CHECK(r.rgba.size() == static_cast<size_t>(kThumbCell) * kThumbCell * 4);
    if (r.rgba.size() != static_cast<size_t>(kThumbCell) * kThumbCell * 4) return;
    CHECK(isRed(pixelAt(r, 32, 32)));
    CHECK(isRed(pixelAt(r, 0, 16)));
    CHECK(isRed(pixelAt(r, 63, 47)));
    CHECK(isClear(pixelAt(r, 32, 15)));
    CHECK(isClear(pixelAt(r, 32, 48)));
}

// Portrait 32x128: scale 0.5 -> 16x64, centered horizontally.
static void test_portrait_is_pillarboxed(const fs::path& dir)
{
    writeTga(dir / "port.tga", 32, 128, [](int, int) { return kRed; });
    ThumbnailResult r = makeThumbnail(dir / "port.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    CHECK(isRed(pixelAt(r, 24, 0)));
    CHECK(isRed(pixelAt(r, 39, 63)));
    CHECK(isClear(pixelAt(r, 23, 32)));
    CHECK(isClear(pixelAt(r, 40, 32)));
}

// Smaller than the cell: it is NOT enlarged, it is centered as is.
static void test_small_image_is_not_upscaled(const fs::path& dir)
{
    writeTga(dir / "small.tga", 16, 16, [](int, int) { return kRed; });
    ThumbnailResult r = makeThumbnail(dir / "small.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    CHECK(isRed(pixelAt(r, 24, 24)));
    CHECK(isRed(pixelAt(r, 39, 39)));
    CHECK(isClear(pixelAt(r, 23, 24)));
    CHECK(isClear(pixelAt(r, 40, 40)));
}

// Exactly the size of the cell: it passes through untouched.
static void test_exact_size_fills_the_tile(const fs::path& dir)
{
    writeTga(dir / "exact.tga", 64, 64, [](int, int) { return kBlue; });
    ThumbnailResult r = makeThumbnail(dir / "exact.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    CHECK(isBlue(pixelAt(r, 0, 0)));
    CHECK(isBlue(pixelAt(r, 63, 63)));
}

// 2:1 reduction with box filter: left half red, right half blue, no blending at the center.
static void test_downscale_keeps_the_halves(const fs::path& dir)
{
    writeTga(dir / "halves.tga", 128, 128, [](int x, int) { return x < 64 ? kRed : kBlue; });
    ThumbnailResult r = makeThumbnail(dir / "halves.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    CHECK(isRed(pixelAt(r, 10, 32)));
    CHECK(isBlue(pixelAt(r, 53, 32)));
}

// Alpha 128: the color must NOT darken (alpha-weighted average); without that
// a dark halo appears at the edges of any cut-out sprite.
static void test_alpha_does_not_darken_color(const fs::path& dir)
{
    writeTga(dir / "alpha.tga", 128, 128, [](int, int) { return Rgba{ 0, 255, 0, 128 }; });
    ThumbnailResult r = makeThumbnail(dir / "alpha.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    const Rgba c = pixelAt(r, 32, 32);
    CHECK(c[0] == 0);
    CHECK(c[1] == 255);
    CHECK(c[2] == 0);
    CHECK(c[3] == 128);
}

// Alpha at zero in an area: the color of those pixels is irrelevant but must not
// contaminate the opaque neighbors when averaging.
static void test_transparent_neighbours_do_not_bleed(const fs::path& dir)
{
    writeTga(dir / "edge.tga", 128, 128, [](int x, int) {
        return x < 64 ? Rgba{ 255, 0, 0, 255 } : Rgba{ 0, 255, 0, 0 };
    });
    ThumbnailResult r = makeThumbnail(dir / "edge.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    CHECK(isRed(pixelAt(r, 31, 32)));
    CHECK(pixelAt(r, 32, 32)[3] == 0);
}

static void test_unreadable_files(const fs::path& dir)
{
    CHECK(makeThumbnail(dir / "no_existe.png").status == ThumbnailStatus::Unreadable);

    std::ofstream(dir / "corrupta.png", std::ios::binary) << "esto no es una imagen, solo texto";
    CHECK(makeThumbnail(dir / "corrupta.png").status == ThumbnailStatus::Unreadable);

    std::ofstream(dir / "vacia.png", std::ios::binary).flush();
    CHECK(makeThumbnail(dir / "vacia.png").status == ThumbnailStatus::Unreadable);
}

// Only the header of a 20000x20000 TGA (400 MP): it is rejected by
// dimensions, WITHOUT decoding (there is no body to decode).
static void test_huge_image_is_rejected_without_decoding(const fs::path& dir)
{
    {
        std::ofstream f(dir / "enorme.tga", std::ios::binary);
        uint8_t hdr[18] = {};
        hdr[2] = 2; hdr[12] = 20000 & 0xFF; hdr[13] = 20000 >> 8;
        hdr[14] = 20000 & 0xFF; hdr[15] = 20000 >> 8; hdr[16] = 32; hdr[17] = 0x28;
        f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    }
    ThumbnailResult r = makeThumbnail(dir / "enorme.tga");
    CHECK(r.status == ThumbnailStatus::TooLarge);
    CHECK(r.rgba.empty());
}

static bool nearF(float a, float b) { return std::fabs(a - b) < 1e-6f; }

// thumbnailUv: cell -> UV rect with half a texel of margin per side.
static void test_thumbnail_uv()
{
    const float size = static_cast<float>(kThumbAtlasSize);

    const UvRect first = thumbnailUv(0);
    CHECK(nearF(first.u0, 0.5f / size));
    CHECK(nearF(first.v0, 0.5f / size));
    CHECK(nearF(first.u1, 63.5f / size));
    CHECK(nearF(first.v1, 63.5f / size));

    const UvRect second = thumbnailUv(1);              // next column, same row
    CHECK(nearF(second.u0, 64.5f / size));
    CHECK(nearF(second.v0, 0.5f / size));

    const UvRect row1 = thumbnailUv(kThumbAtlasCells); // first column, second row
    CHECK(nearF(row1.u0, 0.5f / size));
    CHECK(nearF(row1.v0, 64.5f / size));

    const UvRect last = thumbnailUv(kThumbSlotCount - 1);
    CHECK(nearF(last.u1, 2047.5f / size));
    CHECK(nearF(last.v1, 2047.5f / size));
}

// ThumbnailSlots: distribution, reuse of the same key, LRU eviction and
// "do not evict what was used this frame".
static void test_slots_basic_assignment()
{
    ThumbnailSlots s(4);
    s.beginFrame();
    const uint32_t a = s.assign(101);
    const uint32_t b = s.assign(102);
    const uint32_t c = s.assign(103);
    const uint32_t d = s.assign(104);
    CHECK(a != ThumbnailSlots::kNone && b != ThumbnailSlots::kNone);
    CHECK(a != b && a != c && a != d && b != c && b != d && c != d);
    CHECK(a < 4 && b < 4 && c < 4 && d < 4);

    CHECK(s.assign(101) == a);        // same key: same cell
    CHECK(s.find(102) == b);
    CHECK(s.find(999) == ThumbnailSlots::kNone);
    CHECK(s.capacity() == 4);
}

// The whole atlas used THIS frame: there is no one to evict -> kNone, and nothing is lost.
static void test_slots_full_this_frame_returns_none()
{
    ThumbnailSlots s(2);
    s.beginFrame();
    const uint32_t a = s.assign(1);
    const uint32_t b = s.assign(2);
    CHECK(s.assign(3) == ThumbnailSlots::kNone);
    CHECK(s.find(1) == a);
    CHECK(s.find(2) == b);
}

// New frame: the least recently used one is evicted and which one is reported.
static void test_slots_evict_least_recently_used()
{
    ThumbnailSlots s(4);
    s.beginFrame();                                     // frame A
    const uint32_t slot1 = s.assign(1);
    s.assign(2); s.assign(3); s.assign(4);

    s.beginFrame();                                     // frame B: only 2 is touched
    s.find(2);

    s.beginFrame();                                     // frame C
    std::optional<uint64_t> evicted;
    const uint32_t slot5 = s.assign(5, &evicted);
    CHECK(slot5 != ThumbnailSlots::kNone);
    CHECK(evicted.has_value());
    // The candidates (1, 3, 4) were used in frame A; 2 in B. Tie on
    // the oldest: the one with the lower cell index wins, that is, 1.
    CHECK(evicted && *evicted == 1);
    CHECK(slot5 == slot1);
    CHECK(s.find(1) == ThumbnailSlots::kNone);
    CHECK(s.find(2) != ThumbnailSlots::kNone);          // the one touched in B survives
}

// A key used in the current frame is never evicted even if it is the oldest by index.
static void test_slots_never_evict_current_frame()
{
    ThumbnailSlots s(2);
    s.beginFrame();
    s.assign(1); s.assign(2);
    s.beginFrame();
    s.find(1);                                          // 1 used now
    std::optional<uint64_t> evicted;
    s.assign(3, &evicted);
    CHECK(evicted && *evicted == 2);
    CHECK(s.find(1) != ThumbnailSlots::kNone);
}

// release() frees the cell without evicting anyone.
static void test_slots_release_frees_a_slot()
{
    ThumbnailSlots s(2);
    s.beginFrame();
    s.assign(1);
    const uint32_t b = s.assign(2);
    s.release(2);
    CHECK(s.find(2) == ThumbnailSlots::kNone);
    std::optional<uint64_t> evicted;
    CHECK(s.assign(3, &evicted) == b);                  // reuses the freed slot
    CHECK(!evicted.has_value());
    s.release(12345);                                   // nonexistent key: nothing happens
}

// ── ThumbnailCache ───────────────────────────────────────────────────────────
// The "worker" is a manual queue: the test decides when each job finishes, so
// everything is deterministic. The Uploader is a double that records each batch.
struct CacheHarness
{
    std::vector<std::function<void()>>  pending;   // jobs enqueued without running
    std::vector<std::vector<uint32_t>>  uploads;   // slots of each call to the Uploader
    bool                                uploadOk = true;
    bool                                runnerAccepts = true;   // false: the pool rejects the job
    size_t                              maxPendingSeen = 0;
    ThumbnailCache                      cache;

    explicit CacheHarness(uint32_t maxInFlight = 4, uint32_t slotCapacity = kThumbSlotCount,
                          ThumbnailCache::Decoder decoder = {},
                          std::shared_ptr<const ThumbnailDiskCache> disk = {},
                          uint32_t maxModelsInFlight = 2)
        : cache(
              [this](std::function<void()> job) {
                  if (!runnerAccepts) return false;
                  pending.push_back(std::move(job));
                  maxPendingSeen = std::max(maxPendingSeen, pending.size());
                  return true;
              },
              [this](const ThumbnailTile* tiles, size_t count) {
                  std::vector<uint32_t> slots;
                  for (size_t i = 0; i < count; ++i) slots.push_back(tiles[i].slot);
                  uploads.push_back(std::move(slots));
                  return uploadOk;
              },
              maxInFlight, slotCapacity, std::move(decoder), std::move(disk), maxModelsInFlight)
    {}

    // Finishes all the enqueued jobs (as if the workers finished at the same time).
    // Like the JobSystem worker, it SWALLOWS the exceptions of a job.
    void runAll()
    {
        std::vector<std::function<void()>> jobs = std::move(pending);
        pending.clear();
        for (auto& job : jobs)
        {
            try { job(); } catch (...) {}
        }
    }

    size_t tilesUploaded() const
    {
        size_t n = 0;
        for (const auto& u : uploads) n += u.size();
        return n;
    }
};

static fs::path makeImage(const fs::path& dir, const char* name, Rgba color = kRed)
{
    const fs::path p = dir / name;
    writeTga(p, 8, 8, [color](int, int) { return color; });
    return p;
}

// Normal cycle: request -> (nullopt) -> job -> upload -> request again -> UV.
static void test_cache_request_decode_upload_ready(const fs::path& dir)
{
    CacheHarness h;
    const fs::path f = makeImage(dir, "cache_a.tga");

    h.cache.beginFrame();
    CHECK(!h.cache.request(f));                 // first time: nothing to show
    h.cache.pump();                             // launches the decoding
    CHECK(h.pending.size() == 1);
    CHECK(h.cache.inFlight() == 1);

    h.runAll();
    h.cache.pump();                             // collects and uploads
    CHECK(h.uploads.size() == 1);
    CHECK(h.tilesUploaded() == 1);
    CHECK(h.cache.inFlight() == 0);

    h.cache.beginFrame();
    const std::optional<UvRect> uv = h.cache.request(f);
    CHECK(uv.has_value());
    if (uv && !h.uploads.empty() && !h.uploads[0].empty())
        CHECK(nearF(uv->u0, thumbnailUv(h.uploads[0][0]).u0));
}

// Review Focus 3: 6 images with a cap of 4 in flight -> never more than 4 live jobs.
static void test_cache_caps_jobs_in_flight(const fs::path& dir)
{
    CacheHarness h(4);
    std::vector<fs::path> files;
    for (int i = 0; i < 6; ++i)
        files.push_back(makeImage(dir, ("cap_" + std::to_string(i) + ".tga").c_str()));

    h.cache.beginFrame();
    for (const auto& f : files) h.cache.request(f);
    h.cache.pump();
    CHECK(h.pending.size() == 4);
    CHECK(h.cache.inFlight() == 4);

    h.runAll();
    h.cache.pump();                             // uploads 4 and launches the 2 that were missing
    CHECK(h.tilesUploaded() == 4);
    CHECK(h.pending.size() == 2);

    h.runAll();
    h.cache.pump();
    CHECK(h.tilesUploaded() == 6);
    CHECK(h.maxPendingSeen <= 4);
}

// Review Focus 3: cap of uploads per frame and ONE single call to the Uploader per batch.
static void test_cache_caps_uploads_per_frame_in_one_batch(const fs::path& dir)
{
    CacheHarness h(8);
    for (int i = 0; i < 5; ++i)
        h.cache.request(makeImage(dir, ("up_" + std::to_string(i) + ".tga").c_str()));
    h.cache.beginFrame();
    h.cache.pump(2);                            // launches the 5 jobs
    h.runAll();

    h.cache.pump(2);
    CHECK(h.uploads.size() == 1);
    CHECK(h.uploads[0].size() == 2);            // two cells in ONE call
    h.cache.pump(2);
    CHECK(h.uploads.size() == 2 && h.uploads[1].size() == 2);
    h.cache.pump(2);
    CHECK(h.tilesUploaded() == 5);
}

// Decoding failure (corrupt or nonexistent file): it is cached, without retrying.
static void test_cache_failed_decode_is_cached(const fs::path& dir)
{
    CacheHarness h;
    std::ofstream(dir / "mala.png", std::ios::binary) << "basura";

    h.cache.beginFrame();
    CHECK(!h.cache.request(dir / "mala.png"));
    h.cache.pump();
    h.runAll();
    h.cache.pump();
    CHECK(h.uploads.empty());

    h.cache.beginFrame();
    CHECK(!h.cache.request(dir / "mala.png"));
    h.cache.pump();
    CHECK(h.pending.empty());                   // it is not re-enqueued

    // A file that does not exist does not even get to launch a job.
    CHECK(!h.cache.request(dir / "fantasma.png"));
    h.cache.pump();
    CHECK(h.pending.empty());
}

// Upload failure (the backend says no): Failed, without retry.
static void test_cache_failed_upload_is_cached(const fs::path& dir)
{
    CacheHarness h;
    h.uploadOk = false;
    const fs::path f = makeImage(dir, "upfail.tga");

    h.cache.beginFrame();
    h.cache.request(f);
    h.cache.pump();
    h.runAll();
    h.cache.pump();
    CHECK(h.uploads.size() == 1);

    h.uploadOk = true;
    h.cache.beginFrame();
    CHECK(!h.cache.request(f));
    h.cache.pump();
    CHECK(h.pending.empty());                   // it is not retried until the mtime changes
    CHECK(h.uploads.size() == 1);
}

// Changing the content (new mtime) regenerates the thumbnail when passing refreshStamps.
static void test_cache_regenerates_when_mtime_changes(const fs::path& dir)
{
    CacheHarness h;
    const fs::path f = makeImage(dir, "mtime.tga");

    h.cache.beginFrame();
    h.cache.request(f);
    h.cache.pump(); h.runAll(); h.cache.pump();
    CHECK(h.tilesUploaded() == 1);

    // Same name, other content, later mtime.
    writeTga(f, 8, 8, [](int, int) { return kBlue; });
    std::error_code ec;
    fs::last_write_time(f, fs::last_write_time(f, ec) + std::chrono::seconds(10), ec);

    h.cache.beginFrame();
    h.cache.refreshStamps();                    // detects the change and discards the entry
    CHECK(!h.cache.request(f));                 // now it is a new request
    h.cache.pump(); h.runAll(); h.cache.pump();
    CHECK(h.tilesUploaded() == 2);

    h.cache.beginFrame();
    CHECK(h.cache.request(f).has_value());
}

// refreshStamps without changes regenerates nothing.
static void test_cache_refresh_without_changes_is_a_noop(const fs::path& dir)
{
    CacheHarness h;
    const fs::path f = makeImage(dir, "same.tga");
    h.cache.beginFrame();
    h.cache.request(f);
    h.cache.pump(); h.runAll(); h.cache.pump();

    h.cache.beginFrame();
    h.cache.refreshStamps();
    CHECK(h.cache.request(f).has_value());
    h.cache.pump();
    CHECK(h.pending.empty());
    CHECK(h.tilesUploaded() == 1);
}

// Review Focus 4: folder change halfway through loading -> the late result is ignored.
static void test_cache_new_generation_discards_pending(const fs::path& dir)
{
    CacheHarness h;
    const fs::path f = makeImage(dir, "gen.tga");

    h.cache.beginFrame();
    h.cache.request(f);
    h.cache.pump();                             // job in flight
    CHECK(h.cache.inFlight() == 1);

    h.cache.newGeneration();                    // the user changes folder
    h.runAll();                                 // the job finishes afterwards
    h.cache.pump();
    CHECK(h.uploads.empty());                   // its result is not uploaded
    CHECK(h.cache.inFlight() == 0);             // but the in-flight slot is freed

    // Requesting it again works normally.
    h.cache.beginFrame();
    CHECK(!h.cache.request(f));
    h.cache.pump(); h.runAll(); h.cache.pump();
    CHECK(h.tilesUploaded() == 1);
}

// newGeneration keeps what was already uploaded: going back to the previous folder does not decode again.
static void test_cache_new_generation_keeps_ready_entries(const fs::path& dir)
{
    CacheHarness h;
    const fs::path f = makeImage(dir, "keep.tga");
    h.cache.beginFrame();
    h.cache.request(f);
    h.cache.pump(); h.runAll(); h.cache.pump();

    h.cache.newGeneration();
    h.cache.beginFrame();
    CHECK(h.cache.request(f).has_value());
    h.cache.pump();
    CHECK(h.pending.empty());
}

// Review Focus 6: the cell is reused by another image -> it is decoded again, the other one's is not drawn.
static void test_cache_evicted_slot_is_requested_again(const fs::path& dir)
{
    CacheHarness h(4, /*slotCapacity=*/2);
    const fs::path a = makeImage(dir, "ev_a.tga");
    const fs::path b = makeImage(dir, "ev_b.tga");
    const fs::path c = makeImage(dir, "ev_c.tga");

    h.cache.beginFrame();
    h.cache.request(a); h.cache.request(b);
    h.cache.pump(); h.runAll(); h.cache.pump();          // a and b occupy the 2 cells
    CHECK(h.tilesUploaded() == 2);

    h.cache.beginFrame();
    h.cache.request(c);                                  // new: one has to be evicted
    h.cache.pump(); h.runAll();
    h.cache.beginFrame();                                // next frame: a and b are no longer used
    h.cache.pump();                                      // c comes in and evicts one of the two
    CHECK(h.tilesUploaded() == 3);

    h.cache.beginFrame();
    const bool aReady = h.cache.request(a).has_value();
    const bool bReady = h.cache.request(b).has_value();
    CHECK(aReady != bReady);                             // exactly one lost its cell
    CHECK(h.cache.request(c).has_value());
}

// Review Focus 5: the result arrives AFTER destroying the cache -> it does not touch freed memory.
static void test_cache_late_result_after_destruction_is_harmless(const fs::path& dir)
{
    const fs::path f = makeImage(dir, "late.tga");
    std::vector<std::function<void()>> orphan;
    {
        ThumbnailCache c(
            [&orphan](std::function<void()> job) { orphan.push_back(std::move(job)); return true; },
            [](const ThumbnailTile*, size_t) { return true; });
        c.beginFrame();
        c.request(f);
        c.pump();
    }                                                     // the cache dies with the job not run
    CHECK(orphan.size() == 1);
    for (auto& job : orphan) job();                       // it must not crash
}

// ── Fix pass of the final review ────────────────────────────────────────────

// Virtual file: header of a 20000x20000 TGA at 32 bpp and then zeros,
// generated on the fly (no memory). It counts how many bytes were requested from it.
class VirtualHugeTga : public std::streambuf
{
public:
    explicit VirtualHugeTga(uint64_t totalBytes) : m_total(totalBytes) {}
    uint64_t served() const { return m_served; }

protected:
    int_type underflow() override
    {
        if (m_served >= m_total) return traits_type::eof();
        const uint64_t n = std::min<uint64_t>(sizeof(m_chunk), m_total - m_served);
        for (uint64_t i = 0; i < n; ++i)
        {
            const uint64_t at = m_served + i;
            m_chunk[i] = at < sizeof(kHeader) ? static_cast<char>(kHeader[at]) : 0;
        }
        m_served += n;
        setg(m_chunk, m_chunk, m_chunk + n);
        return traits_type::to_int_type(m_chunk[0]);
    }

private:
    // 20000 = 0x4E20 en little endian.
    static constexpr uint8_t kHeader[18] = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                             0x20, 0x4E, 0x20, 0x4E, 32, 0x28 };
    char     m_chunk[65536];
    uint64_t m_total;
    uint64_t m_served = 0;
};

// Review Focus 1, for real: rejecting by dimensions must cost the HEADER, not
// reading the whole file into memory. A 20000x20000 TGA is 1.6 GB.
static void test_huge_image_does_not_read_the_whole_file()
{
    VirtualHugeTga buf(200ull * 1024 * 1024);          // 200 MB of "file"
    std::istream   in(&buf);
    ThumbnailResult r = makeThumbnailFromStream(in);
    CHECK(r.status == ThumbnailStatus::TooLarge);
    CHECK(buf.served() <= 1024 * 1024);                // only the header, not the file
}

// A job that throws does NOT report (the JobSystem worker swallows the exception): if
// the cache does not protect itself, m_inFlight is left hanging and with cap 1 nothing else loads.
static void test_cache_throwing_decoder_does_not_leak_in_flight(const fs::path& dir)
{
    CacheHarness h(1, kThumbSlotCount,
                   [](const fs::path&) -> ThumbnailResult { throw std::runtime_error("boom"); });
    const fs::path a = makeImage(dir, "boom_a.tga");
    const fs::path b = makeImage(dir, "boom_b.tga");

    h.cache.beginFrame();
    h.cache.request(a);
    h.cache.request(b);
    h.cache.pump();
    CHECK(h.pending.size() == 1);                       // cap 1: it only launches one
    h.runAll();                                         // that job throws
    h.cache.pump();
    CHECK(h.cache.inFlight() == 1);                     // it already launched the SECOND: the slot was freed
    CHECK(h.pending.size() == 1);
    h.runAll();
    h.cache.pump();
    CHECK(h.cache.inFlight() == 0);
    CHECK(h.uploads.empty());
    h.cache.beginFrame();
    CHECK(!h.cache.request(a));                         // Failed: it is not retried
}

// The pool rejects the job (stopped): the in-flight slot is given back and the entry is left
// in Failed, without retrying in a loop.
static void test_cache_rejected_job_does_not_leak_in_flight(const fs::path& dir)
{
    CacheHarness h(1);
    h.runnerAccepts = false;
    const fs::path a = makeImage(dir, "rej_a.tga");

    h.cache.beginFrame();
    h.cache.request(a);
    h.cache.pump();
    CHECK(h.cache.inFlight() == 0);

    h.runnerAccepts = true;
    h.cache.beginFrame();
    CHECK(!h.cache.request(a));
    h.cache.pump();
    CHECK(h.pending.empty());                           // Failed: it is not tried again
}

// An ImGui button's id comes from the HASH of its whole label: going from "IMG" to
// "" when the thumbnail arrives changes the id and ImGui loses the click or the drag in
// progress. The stable label carries "###icon".
static void test_icon_button_id_is_stable_when_thumbnail_appears()
{
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename  = nullptr;
    io.LogFilename  = nullptr;
    io.DisplaySize  = ImVec2(800.0f, 600.0f);
    io.DeltaTime    = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontDefault();

    auto idOf = [&](const std::string& label) {
        ImGui::NewFrame();
        ImGui::Begin("Test", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::PushID("asset");
        ImGui::Button(label.c_str());
        const ImGuiID id = ImGui::GetItemID();
        ImGui::PopID();
        ImGui::End();
        ImGui::Render();
        return id;
    };

    const ImGuiID withoutThumb = idOf(assetIconButtonLabel("IMG", false));
    const ImGuiID withThumb    = idOf(assetIconButtonLabel("IMG", true));
    CHECK(withoutThumb == withThumb);
    // Control: with the bare labels the id DOES change, or the test would prove nothing.
    CHECK(idOf("IMG") != idOf(""));

    ImGui::DestroyContext(ctx);
}

// ── rasterizeThumbnail ───────────────────────────────────────────────────────

static PreviewImage solidImage(Rgba c)
{
    PreviewImage img;
    img.w = img.h = 1;
    img.rgba = { c[0], c[1], c[2], c[3] };
    return img;
}

static PreviewPart cubePart()
{
    PreviewPart p;
    const glm::vec3 n[6] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
    for (const glm::vec3& f : n)
    {
        const glm::vec3 u = std::abs(f.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        const glm::vec3 v = glm::cross(f, u);
        const uint32_t base = static_cast<uint32_t>(p.positions.size());
        for (const glm::vec2 c : { glm::vec2(-1, -1), glm::vec2(1, -1), glm::vec2(1, 1), glm::vec2(-1, 1) })
        {
            p.positions.push_back(f + u * c.x + v * c.y);
            p.normals.push_back(f);
            p.uvs.emplace_back(0.5f);
            p.colors.emplace_back(1.0f);
        }
        p.indices.insert(p.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
    return p;
}

static bool reddish(const Rgba& c) { return c[3] == 255 && c[0] > c[1] + 40 && c[0] > c[2] + 40; }

static void test_raster_sphere_takes_the_albedo_color()
{
    PreviewPart s = makePreviewSphere();
    s.albedo = solidImage({ 255, 0, 0, 255 });
    const ThumbnailResult r = rasterizeThumbnail({ s });
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    CHECK(reddish(pixelAt(r, 32, 32)));
    CHECK(isClear(pixelAt(r, 0, 0)));
}

static void test_raster_uses_vertex_color_without_texture()
{
    PreviewPart s = makePreviewSphere();
    for (glm::vec3& c : s.colors) c = glm::vec3(0.0f, 1.0f, 0.0f);
    const ThumbnailResult r = rasterizeThumbnail({ s });
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    const Rgba c = pixelAt(r, 32, 32);
    CHECK(c[1] > c[0] + 40 && c[1] > c[2] + 40);
}

// Framing: the cube fills the cell with a margin and does not touch the edge.
static void test_raster_frames_the_bbox_with_a_margin()
{
    const ThumbnailResult r = rasterizeThumbnail({ cubePart() });
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    int minX = 64, maxX = -1, minY = 64, maxY = -1;
    for (uint32_t y = 0; y < kThumbCell; ++y)
        for (uint32_t x = 0; x < kThumbCell; ++x)
            if (pixelAt(r, x, y)[3] > 200)
            {
                minX = std::min<int>(minX, x); maxX = std::max<int>(maxX, x);
                minY = std::min<int>(minY, y); maxY = std::max<int>(maxY, y);
            }
    CHECK(minX >= 1 && minY >= 1 && maxX <= 62 && maxY <= 62);    // margin
    CHECK(std::max(maxX - minX, maxY - minY) >= 52);               // and even so it fills the cell
}

static void test_raster_is_deterministic()
{
    PreviewPart s = makePreviewSphere();
    s.albedo = solidImage({ 10, 200, 90, 255 });
    CHECK(rasterizeThumbnail({ s }).rgba == rasterizeThumbnail({ s }).rgba);
}

static void test_raster_metallic_and_roughness_change_the_result()
{
    PreviewPart shiny = makePreviewSphere();
    shiny.metallic = 1.0f; shiny.roughness = 0.2f;
    PreviewPart matte = makePreviewSphere();
    matte.metallic = 0.0f; matte.roughness = 0.9f;
    CHECK(rasterizeThumbnail({ shiny }).rgba != rasterizeThumbnail({ matte }).rgba);
}

// The two faces of a triangle are drawn the same (the thumbnail does no culling).
static void test_raster_draws_back_faces()
{
    auto tri = [](bool flip) {
        PreviewPart p;
        p.positions = { { -1, -1, 0 }, { 1, -1, 0 }, { 0, 1, 0 } };
        p.normals   = { { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 } };
        p.uvs       = { {}, {}, {} };
        p.colors    = { glm::vec3(1), glm::vec3(1), glm::vec3(1) };
        p.indices   = flip ? std::vector<uint32_t>{ 0, 2, 1 } : std::vector<uint32_t>{ 0, 1, 2 };
        return p;
    };
    auto covered = [](const ThumbnailResult& r) {
        int n = 0;
        for (size_t i = 3; i < r.rgba.size(); i += 4) n += r.rgba[i] == 255;
        return n;
    };
    const ThumbnailResult a = rasterizeThumbnail({ tri(false) });
    const ThumbnailResult b = rasterizeThumbnail({ tri(true) });
    CHECK(covered(a) > 100);
    CHECK(covered(a) == covered(b));

    // And they are LIT the same: backward normals are flipped, they do not leave the face dark.
    PreviewPart back = tri(false);
    for (glm::vec3& n : back.normals) n = -n;
    CHECK(rasterizeThumbnail({ back }).rgba == a.rgba);
}

// Review Focus 5: garbage -> Unreadable, without NaN or a crash.
static void test_raster_degenerate_input_is_unreadable()
{
    CHECK(rasterizeThumbnail({}).status == ThumbnailStatus::Unreadable);

    PreviewPart point;
    point.positions = { { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 } };
    point.indices   = { 0, 1, 2 };
    CHECK(rasterizeThumbnail({ point }).status == ThumbnailStatus::Unreadable);

    PreviewPart nan = point;
    const float q = std::numeric_limits<float>::quiet_NaN();
    nan.positions = { { q, 0, 0 }, { 0, q, 0 }, { 0, 0, q } };
    CHECK(rasterizeThumbnail({ nan }).status == ThumbnailStatus::Unreadable);

    PreviewPart outOfRange = cubePart();
    outOfRange.indices = { 0, 1, 999 };
    CHECK(rasterizeThumbnail({ outOfRange }).status == ThumbnailStatus::Unreadable);

    // One good triangle and one with NaN: the good one is drawn and there is no NaN in the output.
    PreviewPart mixed = cubePart();
    mixed.positions.push_back({ q, q, q });
    const uint32_t bad = static_cast<uint32_t>(mixed.positions.size() - 1);
    mixed.normals.push_back({ 0, 1, 0 }); mixed.uvs.emplace_back(0.0f); mixed.colors.emplace_back(1.0f);
    mixed.indices.insert(mixed.indices.end(), { 0, 1, bad });
    CHECK(rasterizeThumbnail({ mixed }).status == ThumbnailStatus::Ok);
}

// ── makeAssetThumbnail ───────────────────────────────────────────────────────

static bool hasDep(const ThumbnailResult& r, const fs::path& p)
{
    for (const ThumbnailDependency& d : r.dependencies)
        if (fs::path(d.path).lexically_normal() == p.lexically_normal()) return true;
    return false;
}

static void writeMat(const fs::path& p, const MaterialAsset& a)
{
    std::string err;
    CHECK(saveMaterialAsset(p, a, &err));
}

// An image goes through the usual decoder: same bytes.
static void test_asset_thumbnail_image_is_unchanged(const fs::path& dir)
{
    writeTga(dir / "img_same.tga", 20, 10, [](int x, int) { return x < 10 ? kRed : kBlue; });
    const ThumbnailResult a = makeAssetThumbnail(dir / "img_same.tga");
    const ThumbnailResult b = makeThumbnail(dir / "img_same.tga");
    CHECK(a.status == ThumbnailStatus::Ok);
    CHECK(a.rgba == b.rgba);
}

static void test_asset_thumbnail_model_declares_its_dependencies(const fs::path& dir)
{
    writeTga(dir / "obj_tex.tga", 4, 4, [](int, int) { return kRed; });
    std::ofstream(dir / "quad.mtl") << "newmtl m\nmap_Kd obj_tex.tga\n";
    std::ofstream(dir / "quad.obj") << "mtllib quad.mtl\nusemtl m\n"
                                       "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                                       "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
                                       "f 1/1 2/2 3/3\nf 1/1 3/3 4/4\n";
    const ThumbnailResult r = makeAssetThumbnail(dir / "quad.obj");
    CHECK(r.status == ThumbnailStatus::Ok);
    CHECK(hasDep(r, dir / "obj_tex.tga"));
    CHECK(hasDep(r, importSidecarPath(dir / "quad.obj")));
}

static void test_asset_thumbnail_animation_only_fbx()
{
    const ThumbnailResult r = makeAssetThumbnail("assets/animatedCharacter/standing idle 01.fbx");
    CHECK(r.status == ThumbnailStatus::AnimationOnly);
    CHECK(r.rgba.empty());
}

static void test_material_thumbnail_takes_its_albedo(const fs::path& dir)
{
    writeTga(dir / "mat_red.tga", 4, 4, [](int, int) { return kRed; });
    MaterialAsset a;
    a.albedo = (dir / "mat_red.tga").string();
    writeMat(dir / "red.mat", a);
    const ThumbnailResult r = makeAssetThumbnail(dir / "red.mat");
    CHECK(r.status == ThumbnailStatus::Ok);
    if (!r.rgba.empty()) CHECK(reddish(pixelAt(r, 32, 32)));
    CHECK(hasDep(r, dir / "mat_red.tga"));
}

// Inherited -> neutral gray. And the values READ from the .mat count: they are tested with
// roughness 0.1 against 0.9 and metallic 1, none of them the default.
static void test_material_thumbnail_inherits_neutral_and_reads_factors(const fs::path& dir)
{
    writeMat(dir / "neutral.mat", MaterialAsset{});
    const ThumbnailResult n = makeAssetThumbnail(dir / "neutral.mat");
    CHECK(n.status == ThumbnailStatus::Ok);
    if (!n.rgba.empty())
    {
        const Rgba c = pixelAt(n, 32, 32);
        CHECK(std::abs(int(c[0]) - int(c[1])) <= 2 && std::abs(int(c[1]) - int(c[2])) <= 12);   // gray (the ambient tints the blue a bit)
    }

    MaterialAsset smooth; smooth.roughness = 0.1f;
    MaterialAsset rough;  rough.roughness  = 0.9f;
    MaterialAsset metal;  metal.metallic   = 1.0f;
    writeMat(dir / "smooth.mat", smooth);
    writeMat(dir / "rough.mat", rough);
    writeMat(dir / "metal.mat", metal);
    CHECK(makeAssetThumbnail(dir / "smooth.mat").rgba != makeAssetThumbnail(dir / "rough.mat").rgba);
    CHECK(makeAssetThumbnail(dir / "metal.mat").rgba != n.rgba);
}

// Texture that does not exist: neutral sphere (it is what the engine draws) and the path is still
// a dependency, to regenerate when it shows up.
static void test_material_thumbnail_missing_texture_is_neutral(const fs::path& dir)
{
    MaterialAsset a;
    a.albedo = (dir / "todavia_no.tga").string();
    writeMat(dir / "pending.mat", a);
    writeMat(dir / "neutral2.mat", MaterialAsset{});
    const ThumbnailResult r = makeAssetThumbnail(dir / "pending.mat");
    CHECK(r.status == ThumbnailStatus::Ok);
    CHECK(r.rgba == makeAssetThumbnail(dir / "neutral2.mat").rgba);
    CHECK(hasDep(r, dir / "todavia_no.tga"));
}

static void test_stamp_file_and_dependencies(const fs::path& dir)
{
    const ThumbnailDependency missing = stampFile(dir / "no_hay.tga");
    CHECK(!missing.exists && missing.mtime == 0);

    const fs::path f = makeImage(dir, "stamp.tga");
    const ThumbnailDependency s = stampFile(f);
    std::error_code ec;
    CHECK(s.exists && s.mtime == static_cast<int64_t>(fs::last_write_time(f, ec).time_since_epoch().count()));

    ThumbnailResult r;
    r.dependencies = { { dir / "no_hay.tga" }, { f }, { dir / "no_hay.tga" } };   // with a duplicate and with the asset itself
    stampDependencies(r, s);
    CHECK(r.dependencies.size() == 2);                       // self first, without duplicates
    if (r.dependencies.size() == 2)
    {
        CHECK(r.dependencies[0] == s);
        CHECK(!r.dependencies[1].exists);
    }
}

static void test_is_model_thumbnail_path()
{
    CHECK(isModelThumbnailPath("a/b/Hero.FBX"));
    CHECK(isModelThumbnailPath("x.obj"));
    CHECK(!isModelThumbnailPath("x.mat"));
    CHECK(!isModelThumbnailPath("x.png"));
    CHECK(isModelThumbnailPath("x.glb"));
    CHECK(isModelThumbnailPath("x.GLTF"));
    CHECK(!isModelThumbnailPath("x.dae"));
}

// ── ThumbnailDiskCache ───────────────────────────────────────────────────────

static ThumbnailResult stampedResult(const fs::path& asset, std::vector<fs::path> deps = {})
{
    ThumbnailResult r;
    r.status = ThumbnailStatus::Ok;
    r.rgba.resize(static_cast<size_t>(kThumbCell) * kThumbCell * 4);
    for (size_t i = 0; i < r.rgba.size(); ++i) r.rgba[i] = static_cast<uint8_t>(i * 7);
    for (const fs::path& d : deps) r.dependencies.push_back({ d });
    stampDependencies(r, stampFile(asset));
    return r;
}

static void bumpMtime(const fs::path& p)
{
    std::error_code ec;
    fs::last_write_time(p, fs::last_write_time(p, ec) + std::chrono::seconds(10), ec);
}

static void test_disk_roundtrip(const fs::path& dir)
{
    const ThumbnailDiskCache disk(dir / "cache_rt");
    const fs::path asset = makeImage(dir, "disk_a.tga");
    const fs::path dep   = makeImage(dir, "disk_a_dep.tga");
    const ThumbnailResult r = stampedResult(asset, { dep, dir / "disk_a_absent.tga" });
    CHECK(disk.store(asset, r));
    const std::optional<ThumbnailResult> back = disk.load(asset);
    CHECK(back.has_value());
    if (!back) return;
    CHECK(back->status == ThumbnailStatus::Ok);
    CHECK(back->rgba == r.rgba);
    CHECK(back->dependencies.size() == 3);
    if (back->dependencies.size() == 3) CHECK(back->dependencies[0] == r.dependencies[0]);

    ThumbnailResult anim;
    anim.status = ThumbnailStatus::AnimationOnly;
    stampDependencies(anim, stampFile(dep));
    CHECK(disk.store(dep, anim));
    const auto animBack = disk.load(dep);
    CHECK(animBack && animBack->status == ThumbnailStatus::AnimationOnly && animBack->rgba.empty());
}

// Review Focus 2: a dependency changes (not the asset), or one appears that was not there.
static void test_disk_dependency_change_is_a_miss(const fs::path& dir)
{
    const ThumbnailDiskCache disk(dir / "cache_dep");
    const fs::path asset = makeImage(dir, "disk_b.tga");
    const fs::path dep   = makeImage(dir, "disk_b_dep.tga");
    const fs::path later = dir / "disk_b_later.tga";
    CHECK(disk.store(asset, stampedResult(asset, { dep, later })));
    CHECK(disk.load(asset).has_value());

    bumpMtime(dep);
    CHECK(!disk.load(asset).has_value());

    CHECK(disk.store(asset, stampedResult(asset, { dep, later })));
    makeImage(dir, "disk_b_later.tga");
    CHECK(!disk.load(asset).has_value());

    CHECK(disk.store(asset, stampedResult(asset, { dep, later })));
    bumpMtime(asset);
    CHECK(!disk.load(asset).has_value());
}

// Review Focus 3: file from another version, truncated or from another path with the same name.
static void test_disk_hostile_files_are_a_miss(const fs::path& dir)
{
    const ThumbnailDiskCache disk(dir / "cache_bad");
    const fs::path a = makeImage(dir, "disk_c.tga");
    const fs::path b = makeImage(dir, "disk_d.tga");
    auto readAll  = [](const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), {}); };
    auto writeAll = [](const fs::path& p, const std::string& s) { std::ofstream(p, std::ios::binary | std::ios::trunc) << s; };

    CHECK(disk.store(a, stampedResult(a)));
    const std::string good = readAll(disk.fileFor(a));

    std::string otherVersion = good;
    otherVersion[4] = static_cast<char>(otherVersion[4] + 1);          // version, after the magic
    writeAll(disk.fileFor(a), otherVersion);
    CHECK(!disk.load(a).has_value());

    writeAll(disk.fileFor(a), good.substr(0, good.size() - 10));
    CHECK(!disk.load(a).has_value());

    writeAll(disk.fileFor(a), good + "x");                            // extra bytes
    CHECK(!disk.load(a).has_value());

    writeAll(disk.fileFor(b), good);                                  // simulated hash collision
    CHECK(!disk.load(b).has_value());

    writeAll(disk.fileFor(a), "");
    CHECK(!disk.load(a).has_value());
}

static void test_disk_unwritable_dir_does_not_throw(const fs::path& dir)
{
    std::ofstream(dir / "soy_un_fichero") << "x";
    const ThumbnailDiskCache disk(dir / "soy_un_fichero");
    const fs::path a = makeImage(dir, "disk_e.tga");
    CHECK(!disk.store(a, stampedResult(a)));
    CHECK(!disk.load(a).has_value());
}

static void test_disk_leaves_no_temporaries(const fs::path& dir)
{
    const ThumbnailDiskCache disk(dir / "cache_tmp");
    for (int i = 0; i < 3; ++i)
    {
        const fs::path a = makeImage(dir, ("disk_t" + std::to_string(i) + ".tga").c_str());
        CHECK(disk.store(a, stampedResult(a)));
        CHECK(disk.store(a, stampedResult(a)));                         // overwrites
    }
    int files = 0, temps = 0;
    for (const auto& e : fs::directory_iterator(dir / "cache_tmp"))
    {
        ++files;
        if (e.path().extension() == ".tmp") ++temps;
    }
    CHECK(files == 3);
    CHECK(temps == 0);
}

// Without sealed dependencies nothing is saved: it would not be known when to invalidate.
static void test_disk_refuses_unstamped_results(const fs::path& dir)
{
    const ThumbnailDiskCache disk(dir / "cache_unstamped");
    const fs::path a = makeImage(dir, "disk_f.tga");
    ThumbnailResult r = stampedResult(a);
    r.dependencies.clear();
    CHECK(!disk.store(a, r));
}

// ── ThumbnailCache: dependencies, disk, state and model cap ────────────

static ThumbnailResult okTile()
{
    ThumbnailResult r;
    r.status = ThumbnailStatus::Ok;
    r.rgba.assign(static_cast<size_t>(kThumbCell) * kThumbCell * 4, 128);
    return r;
}

// Review Focus 2: the texture the thumbnail depends on changes, not the asset.
static void test_cache_dependency_change_regenerates(const fs::path& dir)
{
    const fs::path asset = makeImage(dir, "dep_asset.tga");
    const fs::path dep   = makeImage(dir, "dep_texture.tga");
    int calls = 0;
    CacheHarness h(4, kThumbSlotCount, [&](const fs::path&) {
        ++calls;
        ThumbnailResult r = okTile();
        r.dependencies.push_back({ dep });
        return r;
    });
    h.cache.beginFrame();
    h.cache.request(asset);
    h.cache.pump(); h.runAll(); h.cache.pump();
    CHECK(calls == 1);

    h.cache.beginFrame();
    h.cache.refreshStamps();
    CHECK(h.cache.request(asset).has_value());          // no changes: it stays ready

    bumpMtime(dep);
    h.cache.beginFrame();
    h.cache.refreshStamps();
    CHECK(!h.cache.request(asset).has_value());
    h.cache.pump(); h.runAll(); h.cache.pump();
    CHECK(calls == 2);
}

// Review Focus 1: the asset changes while it is being decoded. The mtime was taken BEFORE,
// so the next refreshStamps sees it and regenerates.
static void test_cache_change_during_decode_regenerates(const fs::path& dir)
{
    const fs::path asset = makeImage(dir, "racy.tga");
    int calls = 0;
    CacheHarness h(4, kThumbSlotCount, [&](const fs::path& p) {
        if (++calls == 1) bumpMtime(p);                  // "re-exported" halfway through decoding
        return okTile();
    });
    h.cache.beginFrame();
    h.cache.request(asset);
    h.cache.pump(); h.runAll(); h.cache.pump();
    h.cache.beginFrame();
    h.cache.refreshStamps();
    CHECK(!h.cache.request(asset).has_value());
    h.cache.pump(); h.runAll(); h.cache.pump();
    CHECK(calls == 2);
}

static void test_cache_status_reports_animation_only(const fs::path& dir)
{
    const fs::path f = makeImage(dir, "anim_only.fbx");     // the content does not matter: fake decoder
    CacheHarness h(4, kThumbSlotCount, [](const fs::path&) {
        ThumbnailResult r;
        r.status = ThumbnailStatus::AnimationOnly;
        return r;
    });
    h.cache.beginFrame();
    CHECK(!h.cache.status(f).has_value());               // pending: not known yet
    h.cache.request(f);
    h.cache.pump(); h.runAll(); h.cache.pump();
    CHECK(h.cache.status(f) == ThumbnailStatus::AnimationOnly);
    h.cache.beginFrame();
    CHECK(!h.cache.request(f).has_value());
    h.cache.pump();
    CHECK(h.pending.empty());                            // it is not retried
    CHECK(h.uploads.empty());
}

// Review Focus 4: 4 models and 2 images at once -> 2 models + 2 images in flight.
static void test_cache_caps_models_in_flight(const fs::path& dir)
{
    CacheHarness h(4, kThumbSlotCount, [](const fs::path&) { return okTile(); });
    h.cache.beginFrame();
    for (int i = 0; i < 4; ++i) h.cache.request(makeImage(dir, ("m" + std::to_string(i) + ".fbx").c_str()));
    for (int i = 0; i < 2; ++i) h.cache.request(makeImage(dir, ("i" + std::to_string(i) + ".tga").c_str()));
    h.cache.pump();
    CHECK(h.cache.inFlight() == 4);
    CHECK(h.cache.modelsInFlight() == 2);

    h.runAll();
    h.cache.pump();                                      // the 2 models remain
    CHECK(h.cache.modelsInFlight() == 2);
    h.runAll();
    h.cache.pump();
    CHECK(h.tilesUploaded() == 6);
    CHECK(h.cache.modelsInFlight() == 0);
}

static void test_cache_disk_hit_skips_the_decoder(const fs::path& dir)
{
    auto disk = std::make_shared<ThumbnailDiskCache>(dir / "cache_hit");
    const fs::path f = makeImage(dir, "hit.tga");
    ThumbnailResult stored = okTile();
    stampDependencies(stored, stampFile(f));
    CHECK(disk->store(f, stored));

    int calls = 0;
    CacheHarness h(4, kThumbSlotCount, [&](const fs::path&) { ++calls; return okTile(); }, disk);
    h.cache.beginFrame();
    h.cache.request(f);
    h.cache.pump(); h.runAll(); h.cache.pump();
    CHECK(calls == 0);
    CHECK(h.tilesUploaded() == 1);
}

static void test_cache_stores_decoded_results_on_disk(const fs::path& dir)
{
    auto disk = std::make_shared<ThumbnailDiskCache>(dir / "cache_store");
    const fs::path f = makeImage(dir, "store.tga");
    CacheHarness h(4, kThumbSlotCount, {}, disk);         // real decoder
    h.cache.beginFrame();
    h.cache.request(f);
    h.cache.pump(); h.runAll(); h.cache.pump();
    const auto back = disk->load(f);
    CHECK(back.has_value());
    if (back) CHECK(back->rgba == makeThumbnail(f).rgba);
}

// Final review, Important 1: the TEXTURE changes while it is being decoded. The
// decoder sealed it BEFORE reading it; that seal cannot be replaced by one
// taken afterwards, or the thumbnail is left stale forever (also on disk).
static void test_cache_dependency_changed_during_decode_regenerates(const fs::path& dir)
{
    const fs::path asset = makeImage(dir, "racy_dep_asset.tga");
    const fs::path dep   = makeImage(dir, "racy_dep_texture.tga");
    int calls = 0;
    CacheHarness h(4, kThumbSlotCount, [&](const fs::path&) {
        ThumbnailResult r = okTile();
        r.dependencies.push_back(stampFile(dep));          // sealed before "reading it"
        if (++calls == 1) bumpMtime(dep);                  // it is saved halfway through decoding
        return r;
    });
    h.cache.beginFrame();
    h.cache.request(asset);
    h.cache.pump(); h.runAll(); h.cache.pump();
    h.cache.beginFrame();
    h.cache.refreshStamps();
    CHECK(!h.cache.request(asset).has_value());
    h.cache.pump(); h.runAll(); h.cache.pump();
    CHECK(calls == 2);
}

// Final review: a failure due to NOT BEING ABLE TO OPEN the asset (locked by another
// program, OneDrive placeholder) is transient: it is not saved to disk, or it
// would stay Unreadable forever even though the file does not change.
static void test_cache_does_not_persist_unopenable_assets(const fs::path& dir)
{
    auto disk = std::make_shared<ThumbnailDiskCache>(dir / "cache_transient");
    const fs::path f = makeImage(dir, "vanishing.tga");
    CacheHarness h(4, kThumbSlotCount, [](const fs::path& p) {
        std::error_code ec;
        fs::remove(p, ec);                                 // cannot be opened during decoding
        return ThumbnailResult{};
    }, disk);
    h.cache.beginFrame();
    h.cache.request(f);
    h.cache.pump(); h.runAll(); h.cache.pump();
    CHECK(h.cache.status(f) == ThumbnailStatus::Unreadable);
    CHECK(!fs::exists(disk->fileFor(f)));
}

// A .gltf declares its .bin: re-exporting the buffer regenerates the thumbnail.
static void test_thumbnail_gltf_declares_its_bin(const fs::path& dir)
{
    std::ofstream(dir / "tri.bin", std::ios::binary) << std::string(60, '\0');
    std::ofstream(dir / "tri.gltf") << R"({"asset":{"version":"2.0"},"buffers":[{"uri":"tri.bin","byteLength":60}]})";
    const ThumbnailResult r = makeAssetThumbnail(dir / "tri.gltf");
    CHECK(hasDep(r, dir / "tri.bin"));
}

static void test_wants_thumbnail_kinds()
{
    CHECK(wantsThumbnail(AssetKind::Image));
    CHECK(wantsThumbnail(AssetKind::Model3D));
    CHECK(wantsThumbnail(AssetKind::Material));
    for (AssetKind k : { AssetKind::Folder, AssetKind::Audio, AssetKind::Font, AssetKind::Scene,
                         AssetKind::Script, AssetKind::Shader, AssetKind::Other })
        CHECK(!wantsThumbnail(k));
}

int main()
{
    fs::path dir = makeDir();
    test_landscape_is_letterboxed(dir);
    test_portrait_is_pillarboxed(dir);
    test_small_image_is_not_upscaled(dir);
    test_exact_size_fills_the_tile(dir);
    test_downscale_keeps_the_halves(dir);
    test_alpha_does_not_darken_color(dir);
    test_transparent_neighbours_do_not_bleed(dir);
    test_unreadable_files(dir);
    test_huge_image_is_rejected_without_decoding(dir);
    test_thumbnail_uv();
    test_slots_basic_assignment();
    test_slots_full_this_frame_returns_none();
    test_slots_evict_least_recently_used();
    test_slots_never_evict_current_frame();
    test_slots_release_frees_a_slot();
    test_cache_request_decode_upload_ready(dir);
    test_cache_caps_jobs_in_flight(dir);
    test_cache_caps_uploads_per_frame_in_one_batch(dir);
    test_cache_failed_decode_is_cached(dir);
    test_cache_failed_upload_is_cached(dir);
    test_cache_regenerates_when_mtime_changes(dir);
    test_cache_refresh_without_changes_is_a_noop(dir);
    test_cache_new_generation_discards_pending(dir);
    test_cache_new_generation_keeps_ready_entries(dir);
    test_cache_evicted_slot_is_requested_again(dir);
    test_cache_late_result_after_destruction_is_harmless(dir);
    test_huge_image_does_not_read_the_whole_file();
    test_cache_throwing_decoder_does_not_leak_in_flight(dir);
    test_cache_rejected_job_does_not_leak_in_flight(dir);
    test_raster_sphere_takes_the_albedo_color();
    test_raster_uses_vertex_color_without_texture();
    test_raster_frames_the_bbox_with_a_margin();
    test_raster_is_deterministic();
    test_raster_metallic_and_roughness_change_the_result();
    test_raster_draws_back_faces();
    test_raster_degenerate_input_is_unreadable();
    test_asset_thumbnail_image_is_unchanged(dir);
    test_asset_thumbnail_model_declares_its_dependencies(dir);
    test_asset_thumbnail_animation_only_fbx();
    test_material_thumbnail_takes_its_albedo(dir);
    test_material_thumbnail_inherits_neutral_and_reads_factors(dir);
    test_material_thumbnail_missing_texture_is_neutral(dir);
    test_stamp_file_and_dependencies(dir);
    test_is_model_thumbnail_path();
    test_disk_roundtrip(dir);
    test_disk_dependency_change_is_a_miss(dir);
    test_disk_hostile_files_are_a_miss(dir);
    test_disk_unwritable_dir_does_not_throw(dir);
    test_disk_leaves_no_temporaries(dir);
    test_disk_refuses_unstamped_results(dir);
    test_cache_dependency_change_regenerates(dir);
    test_cache_change_during_decode_regenerates(dir);
    test_cache_status_reports_animation_only(dir);
    test_cache_caps_models_in_flight(dir);
    test_cache_disk_hit_skips_the_decoder(dir);
    test_cache_stores_decoded_results_on_disk(dir);
    test_cache_dependency_changed_during_decode_regenerates(dir);
    test_cache_does_not_persist_unopenable_assets(dir);
    test_thumbnail_gltf_declares_its_bin(dir);
    test_wants_thumbnail_kinds();
    test_icon_button_id_is_stable_when_thumbnail_appears();
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (g_failures == 0) std::printf("ALL THUMBNAIL TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
