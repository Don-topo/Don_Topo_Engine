// Tests headless de las miniaturas del Content Browser (sin GPU ni ImGui).
// Plain main + CHECK, mismo patron que content_browser_tests.cpp.
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

// TGA sin comprimir de 32 bits, origen arriba a la izquierda: el formato mas
// simple que stb_image lee con alfa, y no hace falta stb_image_write.
static void writeTga(const fs::path& p, int w, int h, const std::function<Rgba(int, int)>& pixel)
{
    std::ofstream f(p, std::ios::binary);
    uint8_t hdr[18] = {};
    hdr[2]  = 2;                               // truecolor sin comprimir
    hdr[12] = static_cast<uint8_t>(w & 0xFF);
    hdr[13] = static_cast<uint8_t>((w >> 8) & 0xFF);
    hdr[14] = static_cast<uint8_t>(h & 0xFF);
    hdr[15] = static_cast<uint8_t>((h >> 8) & 0xFF);
    hdr[16] = 32;                              // bits por pixel
    hdr[17] = 0x28;                            // origen arriba-izquierda, 8 bits de alfa
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

// Apaisada 128x64: se reduce a 64x32 y se centra (16 filas transparentes arriba y abajo).
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

// Vertical 32x128: escala 0.5 -> 16x64, centrada en horizontal.
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

// Mas pequena que la casilla: NO se amplia, se centra tal cual.
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

// Exactamente del tamano de la casilla: pasa sin tocar.
static void test_exact_size_fills_the_tile(const fs::path& dir)
{
    writeTga(dir / "exact.tga", 64, 64, [](int, int) { return kBlue; });
    ThumbnailResult r = makeThumbnail(dir / "exact.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    CHECK(isBlue(pixelAt(r, 0, 0)));
    CHECK(isBlue(pixelAt(r, 63, 63)));
}

// Reduccion 2:1 con caja: mitad izquierda roja, derecha azul, sin mezcla en el centro.
static void test_downscale_keeps_the_halves(const fs::path& dir)
{
    writeTga(dir / "halves.tga", 128, 128, [](int x, int) { return x < 64 ? kRed : kBlue; });
    ThumbnailResult r = makeThumbnail(dir / "halves.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    CHECK(isRed(pixelAt(r, 10, 32)));
    CHECK(isBlue(pixelAt(r, 53, 32)));
}

// Alfa 128: el color NO debe oscurecerse (promedio ponderado por alfa); sin eso
// aparece un halo oscuro en los bordes de cualquier sprite recortado.
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

// Alfa a cero en una zona: el color de esos pixeles es irrelevante pero no debe
// contaminar a los vecinos opacos al promediar.
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

// Solo la cabecera de un TGA de 20000x20000 (400 MP): se rechaza por las
// dimensiones, SIN decodificar (no hay cuerpo que decodificar).
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

// thumbnailUv: casilla -> rect UV con medio texel de margen por lado.
static void test_thumbnail_uv()
{
    const float size = static_cast<float>(kThumbAtlasSize);

    const UvRect first = thumbnailUv(0);
    CHECK(nearF(first.u0, 0.5f / size));
    CHECK(nearF(first.v0, 0.5f / size));
    CHECK(nearF(first.u1, 63.5f / size));
    CHECK(nearF(first.v1, 63.5f / size));

    const UvRect second = thumbnailUv(1);              // siguiente columna, misma fila
    CHECK(nearF(second.u0, 64.5f / size));
    CHECK(nearF(second.v0, 0.5f / size));

    const UvRect row1 = thumbnailUv(kThumbAtlasCells); // primera columna, segunda fila
    CHECK(nearF(row1.u0, 0.5f / size));
    CHECK(nearF(row1.v0, 64.5f / size));

    const UvRect last = thumbnailUv(kThumbSlotCount - 1);
    CHECK(nearF(last.u1, 2047.5f / size));
    CHECK(nearF(last.v1, 2047.5f / size));
}

// ThumbnailSlots: reparto, reutilizacion de la misma clave, desalojo LRU y
// "no desalojar lo usado este frame".
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

    CHECK(s.assign(101) == a);        // misma clave: misma casilla
    CHECK(s.find(102) == b);
    CHECK(s.find(999) == ThumbnailSlots::kNone);
    CHECK(s.capacity() == 4);
}

// Todo el atlas usado ESTE frame: no hay a quien desalojar -> kNone, y nada se pierde.
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

// Frame nuevo: se desaloja la menos usada recientemente y se avisa de cual.
static void test_slots_evict_least_recently_used()
{
    ThumbnailSlots s(4);
    s.beginFrame();                                     // frame A
    const uint32_t slot1 = s.assign(1);
    s.assign(2); s.assign(3); s.assign(4);

    s.beginFrame();                                     // frame B: solo se toca la 2
    s.find(2);

    s.beginFrame();                                     // frame C
    std::optional<uint64_t> evicted;
    const uint32_t slot5 = s.assign(5, &evicted);
    CHECK(slot5 != ThumbnailSlots::kNone);
    CHECK(evicted.has_value());
    // Las candidatas (1, 3, 4) se usaron en el frame A; la 2 en el B. Empate en
    // la mas antigua: gana la de indice de casilla menor, o sea la 1.
    CHECK(evicted && *evicted == 1);
    CHECK(slot5 == slot1);
    CHECK(s.find(1) == ThumbnailSlots::kNone);
    CHECK(s.find(2) != ThumbnailSlots::kNone);          // la tocada en B sobrevive
}

// Una clave usada en el frame actual nunca se desaloja aunque sea la mas antigua en indice.
static void test_slots_never_evict_current_frame()
{
    ThumbnailSlots s(2);
    s.beginFrame();
    s.assign(1); s.assign(2);
    s.beginFrame();
    s.find(1);                                          // 1 usada ahora
    std::optional<uint64_t> evicted;
    s.assign(3, &evicted);
    CHECK(evicted && *evicted == 2);
    CHECK(s.find(1) != ThumbnailSlots::kNone);
}

// release() libera la casilla sin desalojar a nadie.
static void test_slots_release_frees_a_slot()
{
    ThumbnailSlots s(2);
    s.beginFrame();
    s.assign(1);
    const uint32_t b = s.assign(2);
    s.release(2);
    CHECK(s.find(2) == ThumbnailSlots::kNone);
    std::optional<uint64_t> evicted;
    CHECK(s.assign(3, &evicted) == b);                  // reutiliza el hueco liberado
    CHECK(!evicted.has_value());
    s.release(12345);                                   // clave inexistente: no pasa nada
}

// ── ThumbnailCache ───────────────────────────────────────────────────────────
// El "worker" es una cola manual: el test decide cuando termina cada job, asi
// que todo es determinista. El Uploader es un doble que registra cada lote.
struct CacheHarness
{
    std::vector<std::function<void()>>  pending;   // jobs encolados sin ejecutar
    std::vector<std::vector<uint32_t>>  uploads;   // slots de cada llamada al Uploader
    bool                                uploadOk = true;
    bool                                runnerAccepts = true;   // false: el pool rechaza el job
    size_t                              maxPendingSeen = 0;
    ThumbnailCache                      cache;

    explicit CacheHarness(uint32_t maxInFlight = 4, uint32_t slotCapacity = kThumbSlotCount,
                          ThumbnailCache::Decoder decoder = {})
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
              maxInFlight, slotCapacity, std::move(decoder))
    {}

    // Termina todos los jobs encolados (como si los workers acabaran a la vez).
    // Igual que el worker del JobSystem, TRAGA las excepciones de un job.
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

// Ciclo normal: pedir -> (nullopt) -> job -> subida -> pedir de nuevo -> UV.
static void test_cache_request_decode_upload_ready(const fs::path& dir)
{
    CacheHarness h;
    const fs::path f = makeImage(dir, "cache_a.tga");

    h.cache.beginFrame();
    CHECK(!h.cache.request(f));                 // primera vez: nada que ensenar
    h.cache.pump();                             // lanza la decodificacion
    CHECK(h.pending.size() == 1);
    CHECK(h.cache.inFlight() == 1);

    h.runAll();
    h.cache.pump();                             // recoge y sube
    CHECK(h.uploads.size() == 1);
    CHECK(h.tilesUploaded() == 1);
    CHECK(h.cache.inFlight() == 0);

    h.cache.beginFrame();
    const std::optional<UvRect> uv = h.cache.request(f);
    CHECK(uv.has_value());
    if (uv && !h.uploads.empty() && !h.uploads[0].empty())
        CHECK(nearF(uv->u0, thumbnailUv(h.uploads[0][0]).u0));
}

// Review Focus 3: 6 imagenes con tope de 4 en vuelo -> nunca mas de 4 jobs vivos.
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
    h.cache.pump();                             // sube 4 y lanza los 2 que faltaban
    CHECK(h.tilesUploaded() == 4);
    CHECK(h.pending.size() == 2);

    h.runAll();
    h.cache.pump();
    CHECK(h.tilesUploaded() == 6);
    CHECK(h.maxPendingSeen <= 4);
}

// Review Focus 3: tope de subidas por frame y UNA sola llamada al Uploader por lote.
static void test_cache_caps_uploads_per_frame_in_one_batch(const fs::path& dir)
{
    CacheHarness h(8);
    for (int i = 0; i < 5; ++i)
        h.cache.request(makeImage(dir, ("up_" + std::to_string(i) + ".tga").c_str()));
    h.cache.beginFrame();
    h.cache.pump(2);                            // lanza los 5 jobs
    h.runAll();

    h.cache.pump(2);
    CHECK(h.uploads.size() == 1);
    CHECK(h.uploads[0].size() == 2);            // dos casillas en UNA llamada
    h.cache.pump(2);
    CHECK(h.uploads.size() == 2 && h.uploads[1].size() == 2);
    h.cache.pump(2);
    CHECK(h.tilesUploaded() == 5);
}

// Fallo de decodificacion (fichero corrupto o inexistente): se cachea, sin reintentar.
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
    CHECK(h.pending.empty());                   // no se reencola

    // Un fichero que no existe ni siquiera llega a lanzar un job.
    CHECK(!h.cache.request(dir / "fantasma.png"));
    h.cache.pump();
    CHECK(h.pending.empty());
}

// Fallo de subida (el backend dice que no): Failed, sin reintento.
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
    CHECK(h.pending.empty());                   // no se reintenta hasta que cambie el mtime
    CHECK(h.uploads.size() == 1);
}

// Cambiar el contenido (mtime nuevo) regenera la miniatura al pasar refreshStamps.
static void test_cache_regenerates_when_mtime_changes(const fs::path& dir)
{
    CacheHarness h;
    const fs::path f = makeImage(dir, "mtime.tga");

    h.cache.beginFrame();
    h.cache.request(f);
    h.cache.pump(); h.runAll(); h.cache.pump();
    CHECK(h.tilesUploaded() == 1);

    // Mismo nombre, otro contenido, mtime posterior.
    writeTga(f, 8, 8, [](int, int) { return kBlue; });
    std::error_code ec;
    fs::last_write_time(f, fs::last_write_time(f, ec) + std::chrono::seconds(10), ec);

    h.cache.beginFrame();
    h.cache.refreshStamps();                    // detecta el cambio y descarta la entrada
    CHECK(!h.cache.request(f));                 // ahora es una peticion nueva
    h.cache.pump(); h.runAll(); h.cache.pump();
    CHECK(h.tilesUploaded() == 2);

    h.cache.beginFrame();
    CHECK(h.cache.request(f).has_value());
}

// refreshStamps sin cambios no regenera nada.
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

// Review Focus 4: cambio de carpeta a mitad de carga -> el resultado tardio se ignora.
static void test_cache_new_generation_discards_pending(const fs::path& dir)
{
    CacheHarness h;
    const fs::path f = makeImage(dir, "gen.tga");

    h.cache.beginFrame();
    h.cache.request(f);
    h.cache.pump();                             // job en vuelo
    CHECK(h.cache.inFlight() == 1);

    h.cache.newGeneration();                    // el usuario cambia de carpeta
    h.runAll();                                 // el job termina despues
    h.cache.pump();
    CHECK(h.uploads.empty());                   // su resultado no se sube
    CHECK(h.cache.inFlight() == 0);             // pero el hueco en vuelo se libera

    // Volver a pedirlo funciona con normalidad.
    h.cache.beginFrame();
    CHECK(!h.cache.request(f));
    h.cache.pump(); h.runAll(); h.cache.pump();
    CHECK(h.tilesUploaded() == 1);
}

// newGeneration conserva lo ya subido: volver a la carpeta anterior no decodifica otra vez.
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

// Review Focus 6: la casilla la reutiliza otra imagen -> se vuelve a decodificar, no se pinta la ajena.
static void test_cache_evicted_slot_is_requested_again(const fs::path& dir)
{
    CacheHarness h(4, /*slotCapacity=*/2);
    const fs::path a = makeImage(dir, "ev_a.tga");
    const fs::path b = makeImage(dir, "ev_b.tga");
    const fs::path c = makeImage(dir, "ev_c.tga");

    h.cache.beginFrame();
    h.cache.request(a); h.cache.request(b);
    h.cache.pump(); h.runAll(); h.cache.pump();          // a y b ocupan las 2 casillas
    CHECK(h.tilesUploaded() == 2);

    h.cache.beginFrame();
    h.cache.request(c);                                  // nueva: hace falta desalojar una
    h.cache.pump(); h.runAll();
    h.cache.beginFrame();                                // frame siguiente: a y b ya no se usan
    h.cache.pump();                                      // c entra y desaloja a una de las dos
    CHECK(h.tilesUploaded() == 3);

    h.cache.beginFrame();
    const bool aReady = h.cache.request(a).has_value();
    const bool bReady = h.cache.request(b).has_value();
    CHECK(aReady != bReady);                             // exactamente una perdio su casilla
    CHECK(h.cache.request(c).has_value());
}

// Review Focus 5: el resultado llega DESPUES de destruir el cache -> no toca memoria liberada.
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
    }                                                     // el cache muere con el job sin ejecutar
    CHECK(orphan.size() == 1);
    for (auto& job : orphan) job();                       // no debe caerse
}

// ── Fix pass de la revision final ────────────────────────────────────────────

// Fichero virtual: cabecera de un TGA de 20000x20000 a 32 bpp y despues ceros,
// generados al vuelo (sin memoria). Cuenta cuantos bytes se le pidieron.
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

// Review Focus 1, de verdad: rechazar por dimensiones debe costar la CABECERA, no
// leer el fichero entero a memoria. Un TGA de 20000x20000 son 1,6 GB.
static void test_huge_image_does_not_read_the_whole_file()
{
    VirtualHugeTga buf(200ull * 1024 * 1024);          // 200 MB de "fichero"
    std::istream   in(&buf);
    ThumbnailResult r = makeThumbnailFromStream(in);
    CHECK(r.status == ThumbnailStatus::TooLarge);
    CHECK(buf.served() <= 1024 * 1024);                // solo la cabecera, no el fichero
}

// Un job que lanza NO informa (el worker del JobSystem se traga la excepcion): si
// el cache no se protege, m_inFlight queda colgado y con tope 1 no carga nada mas.
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
    CHECK(h.pending.size() == 1);                       // tope 1: solo lanza uno
    h.runAll();                                         // ese job lanza
    h.cache.pump();
    CHECK(h.cache.inFlight() == 1);                     // ya lanzo el SEGUNDO: el hueco se libero
    CHECK(h.pending.size() == 1);
    h.runAll();
    h.cache.pump();
    CHECK(h.cache.inFlight() == 0);
    CHECK(h.uploads.empty());
    h.cache.beginFrame();
    CHECK(!h.cache.request(a));                         // Failed: no se reintenta
}

// El pool rechaza el job (parado): el hueco en vuelo se devuelve y la entrada queda
// en Failed, sin reintentar en bucle.
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
    CHECK(h.pending.empty());                           // Failed: no se vuelve a intentar
}

// El id de un boton de ImGui sale del HASH de su etiqueta entera: pasar de "IMG" a
// "" cuando llega la miniatura cambia el id y ImGui pierde el clic o el arrastre en
// curso. La etiqueta estable lleva "###icon".
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
    // Control: con las etiquetas a pelo el id SI cambia, o el test no probaria nada.
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

// Encuadre: el cubo ocupa la casilla con margen y no toca el borde.
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
    CHECK(minX >= 1 && minY >= 1 && maxX <= 62 && maxY <= 62);    // margen
    CHECK(std::max(maxX - minX, maxY - minY) >= 52);               // y aun asi llena la casilla
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

// Las dos caras de un triangulo se pintan igual (la miniatura no hace culling).
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

    // Y se ILUMINAN igual: normales hacia atras se invierten, no dejan la cara a oscuras.
    PreviewPart back = tri(false);
    for (glm::vec3& n : back.normals) n = -n;
    CHECK(rasterizeThumbnail({ back }).rgba == a.rgba);
}

// Review Focus 5: basura -> Unreadable, sin NaN ni crash.
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

    // Un triangulo bueno y uno con NaN: el bueno se pinta y no hay NaN en la salida.
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

// Una imagen pasa por el decodificador de siempre: mismos bytes.
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

// Heredado -> gris neutro. Y los valores LEIDOS del .mat cuentan: se prueban
// roughness 0.1 frente a 0.9 y metallic 1, ninguno el default.
static void test_material_thumbnail_inherits_neutral_and_reads_factors(const fs::path& dir)
{
    writeMat(dir / "neutral.mat", MaterialAsset{});
    const ThumbnailResult n = makeAssetThumbnail(dir / "neutral.mat");
    CHECK(n.status == ThumbnailStatus::Ok);
    if (!n.rgba.empty())
    {
        const Rgba c = pixelAt(n, 32, 32);
        CHECK(std::abs(int(c[0]) - int(c[1])) <= 2 && std::abs(int(c[1]) - int(c[2])) <= 12);   // gris (el ambiente tinta un poco el azul)
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

// Textura que no existe: esfera neutra (es lo que pinta el motor) y la ruta sigue
// siendo dependencia, para regenerar cuando aparezca.
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
    r.dependencies = { { dir / "no_hay.tga" }, { f }, { dir / "no_hay.tga" } };   // con duplicado y con el propio asset
    stampDependencies(r, s);
    CHECK(r.dependencies.size() == 2);                       // self delante, sin duplicados
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
    CHECK(!isModelThumbnailPath("x.glb"));
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

// Review Focus 2: cambia una dependencia (no el asset), o aparece una que no estaba.
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

// Review Focus 3: fichero de otra version, truncado o de otra ruta con el mismo nombre.
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
    otherVersion[4] = static_cast<char>(otherVersion[4] + 1);          // version, tras la magia
    writeAll(disk.fileFor(a), otherVersion);
    CHECK(!disk.load(a).has_value());

    writeAll(disk.fileFor(a), good.substr(0, good.size() - 10));
    CHECK(!disk.load(a).has_value());

    writeAll(disk.fileFor(a), good + "x");                            // bytes de mas
    CHECK(!disk.load(a).has_value());

    writeAll(disk.fileFor(b), good);                                  // colision de hash simulada
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
        CHECK(disk.store(a, stampedResult(a)));                         // sobrescribe
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

// Sin dependencias selladas no se guarda nada: no se sabria cuando invalidar.
static void test_disk_refuses_unstamped_results(const fs::path& dir)
{
    const ThumbnailDiskCache disk(dir / "cache_unstamped");
    const fs::path a = makeImage(dir, "disk_f.tga");
    ThumbnailResult r = stampedResult(a);
    r.dependencies.clear();
    CHECK(!disk.store(a, r));
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
    test_icon_button_id_is_stable_when_thumbnail_appears();
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (g_failures == 0) std::printf("ALL THUMBNAIL TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
