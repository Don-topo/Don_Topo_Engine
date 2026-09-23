// Tests headless de las miniaturas del Content Browser (sin GPU ni ImGui).
// Plain main + CHECK, mismo patron que content_browser_tests.cpp.
#include "DonTopo/Editor/Thumbnail.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
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
    size_t                              maxPendingSeen = 0;
    ThumbnailCache                      cache;

    explicit CacheHarness(uint32_t maxInFlight = 4, uint32_t slotCapacity = kThumbSlotCount)
        : cache(
              [this](std::function<void()> job) {
                  pending.push_back(std::move(job));
                  maxPendingSeen = std::max(maxPendingSeen, pending.size());
              },
              [this](const ThumbnailTile* tiles, size_t count) {
                  std::vector<uint32_t> slots;
                  for (size_t i = 0; i < count; ++i) slots.push_back(tiles[i].slot);
                  uploads.push_back(std::move(slots));
                  return uploadOk;
              },
              maxInFlight, slotCapacity)
    {}

    // Termina todos los jobs encolados (como si los workers acabaran a la vez).
    void runAll()
    {
        std::vector<std::function<void()>> jobs = std::move(pending);
        pending.clear();
        for (auto& job : jobs) job();
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
            [&orphan](std::function<void()> job) { orphan.push_back(std::move(job)); },
            [](const ThumbnailTile*, size_t) { return true; });
        c.beginFrame();
        c.request(f);
        c.pump();
    }                                                     // el cache muere con el job sin ejecutar
    CHECK(orphan.size() == 1);
    for (auto& job : orphan) job();                       // no debe caerse
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
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (g_failures == 0) std::printf("ALL THUMBNAIL TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
