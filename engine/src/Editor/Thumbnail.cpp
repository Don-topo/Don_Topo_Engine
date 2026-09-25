#include "DonTopo/Editor/Thumbnail.h"
#include "DonTopo/Core/MaterialAsset.h"
#include "DonTopo/Editor/ThumbnailDiskCache.h"
#include "DonTopo/Editor/ThumbnailRaster.h"
#include "DonTopo/Renderer/ModelLoader.h"

#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <fstream>
#include <iterator>
#include <memory>

namespace DonTopo {

ThumbnailResult makeThumbnail(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return makeThumbnailFromStream(in);
}

namespace {

// stb_image leyendo de un istream: la cabecera cuesta unos bytes, no el fichero.
int streamRead(void* user, char* data, int size)
{
    auto& in = *static_cast<std::istream*>(user);
    in.read(data, size);
    return static_cast<int>(in.gcount());
}

void streamSkip(void* user, int n)
{
    auto& in = *static_cast<std::istream*>(user);
    if (n >= 0)
    {
        in.ignore(n);
        return;
    }
    in.clear();
    in.seekg(n, std::ios::cur);
}

int streamEof(void* user)
{
    auto& in = *static_cast<std::istream*>(user);
    return in.peek() == std::char_traits<char>::eof() ? 1 : 0;
}

ThumbnailResult downscaleToCell(const unsigned char* px, int w, int h);

} // namespace

ThumbnailResult makeThumbnailFromStream(std::istream& in)
{
    ThumbnailResult out;
    try
    {
        // Callbacks en vez de leer el fichero entero: rechazar una imagen enorme
        // por su cabecera no puede costar leerla. Ademas stbi_load recibe un char*
        // en la codepage local y falla con rutas Unicode; el ifstream no.
        const stbi_io_callbacks cb{ streamRead, streamSkip, streamEof };
        int w = 0, h = 0, comp = 0;
        if (!stbi_info_from_callbacks(&cb, &in, &w, &h, &comp) || w <= 0 || h <= 0)
            return out;
        if (static_cast<uint64_t>(w) * static_cast<uint64_t>(h) > kThumbMaxSourcePixels)
        {
            out.status = ThumbnailStatus::TooLarge;
            return out;
        }

        in.clear();
        in.seekg(0);
        std::unique_ptr<unsigned char, decltype(&stbi_image_free)> px(
            stbi_load_from_callbacks(&cb, &in, &w, &h, &comp, 4), &stbi_image_free);
        if (!px) return out;
        return downscaleToCell(px.get(), w, h);
    }
    catch (...)
    {
        return ThumbnailResult{};
    }
}

bool operator==(const ThumbnailDependency& a, const ThumbnailDependency& b)
{
    return a.exists == b.exists && a.mtime == b.mtime && a.path == b.path;
}

ThumbnailDependency stampFile(const std::filesystem::path& path)
{
    ThumbnailDependency d;
    d.path = path;
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (!ec)
    {
        d.exists = true;
        d.mtime  = static_cast<int64_t>(t.time_since_epoch().count());
    }
    return d;
}

void stampDependencies(ThumbnailResult& r, const ThumbnailDependency& self)
{
    std::vector<ThumbnailDependency> out{ self };
    for (const ThumbnailDependency& d : r.dependencies)
    {
        const std::filesystem::path p = d.path.lexically_normal();
        const bool seen = std::any_of(out.begin(), out.end(), [&](const ThumbnailDependency& o) {
            return o.path.lexically_normal() == p;
        });
        if (!seen) out.push_back(stampFile(d.path));
    }
    r.dependencies = std::move(out);
}

namespace {

std::string lowerExt(const std::filesystem::path& p)
{
    std::string e = p.extension().string();
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

bool isImageExt(const std::string& e)
{
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp";
}

ThumbnailResult makeModelThumbnail(const std::filesystem::path& path)
{
    const ModelPreview preview = ModelLoader::loadPreview(path.string());
    ThumbnailResult r;
    if (preview.status == PreviewStatus::AnimationOnly) r.status = ThumbnailStatus::AnimationOnly;
    else if (preview.status == PreviewStatus::Ok)       r = rasterizeThumbnail(preview.parts);
    for (const std::filesystem::path& d : preview.dependencies) r.dependencies.push_back({ d });
    return r;
}

} // namespace

bool isModelThumbnailPath(const std::filesystem::path& path)
{
    const std::string e = lowerExt(path);
    return e == ".fbx" || e == ".obj";
}

ThumbnailResult makeMaterialThumbnail(const std::filesystem::path& mat)
{
    const MaterialAsset a = loadMaterialAsset(mat);     // nunca lanza; roto -> hereda
    PreviewPart sphere = makePreviewSphere();
    std::vector<ThumbnailDependency> deps;
    if (!a.albedo.empty())
    {
        deps.push_back({ std::filesystem::path(a.albedo) });
        sphere.albedo = ModelLoader::loadPreviewImage(a.albedo);
    }
    if (sphere.albedo.rgba.empty())
        std::fill(sphere.colors.begin(), sphere.colors.end(), glm::vec3(kNeutralAlbedo));
    sphere.metallic  = a.metallic  < 0.0f ? 0.0f : a.metallic;
    sphere.roughness = a.roughness < 0.0f ? 0.5f : a.roughness;
    ThumbnailResult r = rasterizeThumbnail({ sphere });
    r.dependencies = std::move(deps);
    return r;
}

ThumbnailResult makeAssetThumbnail(const std::filesystem::path& path)
{
    try
    {
        const std::string e = lowerExt(path);
        if (isImageExt(e))              return makeThumbnail(path);
        if (isModelThumbnailPath(path)) return makeModelThumbnail(path);
        if (e == ".mat")                return makeMaterialThumbnail(path);
    }
    catch (...) {}
    return ThumbnailResult{};
}

namespace {

ThumbnailResult downscaleToCell(const unsigned char* px, int w, int h)
{
    ThumbnailResult out;

    // Tamano destino: lo que ya cabe no se amplia; lo demas, a kThumbCell en el lado largo.
    uint32_t dw = static_cast<uint32_t>(w);
    uint32_t dh = static_cast<uint32_t>(h);
    if (dw > kThumbCell || dh > kThumbCell)
    {
        const double s = static_cast<double>(kThumbCell) / std::max(w, h);
        dw = std::clamp(static_cast<uint32_t>(std::lround(w * s)), 1u, kThumbCell);
        dh = std::clamp(static_cast<uint32_t>(std::lround(h * s)), 1u, kThumbCell);
    }

    out.rgba.assign(static_cast<size_t>(kThumbCell) * kThumbCell * 4, 0);
    const uint32_t offX = (kThumbCell - dw) / 2;
    const uint32_t offY = (kThumbCell - dh) / 2;

    for (uint32_t y = 0; y < dh; ++y)
    {
        const uint32_t y0 = static_cast<uint32_t>(static_cast<uint64_t>(y) * h / dh);
        uint32_t       y1 = static_cast<uint32_t>(static_cast<uint64_t>(y + 1) * h / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (uint32_t x = 0; x < dw; ++x)
        {
            const uint32_t x0 = static_cast<uint32_t>(static_cast<uint64_t>(x) * w / dw);
            uint32_t       x1 = static_cast<uint32_t>(static_cast<uint64_t>(x + 1) * w / dw);
            if (x1 <= x0) x1 = x0 + 1;

            // Promedio ponderado por alfa: sin ponderar, un pixel transparente
            // (normalmente negro) oscurece el color de sus vecinos opacos.
            uint64_t sumR = 0, sumG = 0, sumB = 0, sumA = 0;
            for (uint32_t sy = y0; sy < y1; ++sy)
                for (uint32_t sx = x0; sx < x1; ++sx)
                {
                    const unsigned char* p = px + (static_cast<size_t>(sy) * w + sx) * 4;
                    const uint64_t a = p[3];
                    sumR += p[0] * a;
                    sumG += p[1] * a;
                    sumB += p[2] * a;
                    sumA += a;
                }
            const uint64_t count = static_cast<uint64_t>(x1 - x0) * (y1 - y0);
            uint8_t* d = &out.rgba[(static_cast<size_t>(offY + y) * kThumbCell + offX + x) * 4];
            if (sumA > 0)
            {
                d[0] = static_cast<uint8_t>((sumR + sumA / 2) / sumA);
                d[1] = static_cast<uint8_t>((sumG + sumA / 2) / sumA);
                d[2] = static_cast<uint8_t>((sumB + sumA / 2) / sumA);
            }
            d[3] = static_cast<uint8_t>((sumA + count / 2) / count);
        }
    }

    out.status = ThumbnailStatus::Ok;
    return out;
}

} // namespace

ThumbnailSlots::ThumbnailSlots(uint32_t capacity) : m_slots(capacity) {}

uint32_t ThumbnailSlots::find(uint64_t key)
{
    const auto it = m_byKey.find(key);
    if (it == m_byKey.end()) return kNone;
    m_slots[it->second].lastFrame = m_frame;
    return it->second;
}

uint32_t ThumbnailSlots::assign(uint64_t key, std::optional<uint64_t>* evicted)
{
    if (evicted) evicted->reset();
    if (const uint32_t existing = find(key); existing != kNone)
        return existing;

    uint32_t target = kNone;
    for (uint32_t i = 0; i < m_slots.size(); ++i)
        if (!m_slots[i].used) { target = i; break; }

    if (target == kNone)
    {
        // Sin hueco libre: la menos usada recientemente que NO se uso este frame.
        uint64_t oldest = UINT64_MAX;
        for (uint32_t i = 0; i < m_slots.size(); ++i)
            if (m_slots[i].lastFrame < m_frame && m_slots[i].lastFrame < oldest)
            {
                oldest = m_slots[i].lastFrame;
                target = i;
            }
        if (target == kNone) return kNone;
        m_byKey.erase(m_slots[target].key);
        if (evicted) *evicted = m_slots[target].key;
    }

    m_slots[target] = { key, true, m_frame };
    m_byKey[key]    = target;
    return target;
}

void ThumbnailSlots::release(uint64_t key)
{
    const auto it = m_byKey.find(key);
    if (it == m_byKey.end()) return;
    m_slots[it->second].used = false;
    m_byKey.erase(it);
}

ThumbnailCache::ThumbnailCache(Runner run, Uploader upload, uint32_t maxInFlight,
                               uint32_t slotCapacity, Decoder decode,
                               std::shared_ptr<const ThumbnailDiskCache> disk,
                               uint32_t maxModelsInFlight)
    : m_run(std::move(run))
    , m_upload(std::move(upload))
    , m_decode(decode ? std::move(decode) : Decoder(makeAssetThumbnail))
    , m_disk(std::move(disk))
    , m_maxInFlight(maxInFlight)
    , m_maxModelsInFlight(maxModelsInFlight)
    , m_slots(slotCapacity)
{}

void ThumbnailCache::beginFrame()
{
    ++m_frame;
    m_slots.beginFrame();
}

uint64_t ThumbnailCache::makeKey(const std::filesystem::path& path, int64_t mtime)
{
    // Ruta + mtime del asset. Un cambio de dependencia no necesita otra clave:
    // refreshStamps libera la casilla antes de que la entrada se vuelva a pedir.
    const uint64_t h = std::hash<std::string>{}(path.string());
    const uint64_t t = static_cast<uint64_t>(mtime);
    return h ^ (t + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2));
}

std::optional<UvRect> ThumbnailCache::request(const std::filesystem::path& path)
{
    const std::string id = path.string();
    auto it = m_entries.find(id);
    if (it == m_entries.end())
    {
        Entry e;
        e.path = path;
        // Sellado AQUI, antes de decodificar: si el fichero cambia mientras el
        // worker lo lee, el siguiente refreshStamps lo ve (Review Focus 1).
        const ThumbnailDependency self = stampFile(path);
        e.deps             = { self };
        e.key              = makeKey(path, self.mtime);
        e.model            = isModelThumbnailPath(path);
        e.state            = self.exists ? State::Queued : State::Failed;
        e.status           = self.exists ? ThumbnailStatus::Ok : ThumbnailStatus::Unreadable;
        e.lastRequestFrame = m_frame;
        const bool queue   = (e.state == State::Queued);
        m_entries.emplace(id, std::move(e));
        if (queue) m_queue.push_back(id);
        return std::nullopt;
    }

    Entry& e = it->second;
    e.lastRequestFrame = m_frame;
    if (e.state == State::Ready)
    {
        const uint32_t slot = m_slots.find(e.key);
        if (slot != ThumbnailSlots::kNone)
            return thumbnailUv(slot);
        // Otra miniatura reutilizo su casilla: hay que decodificarla otra vez.
        e.state = State::Queued;
        m_queue.push_back(id);
    }
    return std::nullopt;
}

void ThumbnailCache::pump(int maxUploads)
{
    // 1. Resultados de los workers.
    std::vector<Done> done;
    {
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        done.swap(m_shared->done);
    }
    for (Done& d : done)
    {
        if (m_inFlight > 0) --m_inFlight;                    // el hueco se libera siempre
        if (d.model && m_modelsInFlight > 0) --m_modelsInFlight;
        if (d.generation != m_generation) continue;          // carpeta anterior: se ignora
        const auto it = m_entries.find(d.path);
        if (it == m_entries.end() || it->second.state != State::Running) continue;
        Entry& e = it->second;
        if (!d.result.dependencies.empty()) e.deps = std::move(d.result.dependencies);
        e.status = d.result.status;
        if (d.result.status != ThumbnailStatus::Ok)
        {
            e.state = State::Failed;
            continue;
        }
        e.pixels = std::move(d.result.rgba);
        e.state  = State::Decoded;
    }

    // 2. Subida: un lote, una llamada.
    std::vector<ThumbnailTile> tiles;
    std::vector<Entry*>        batch;
    for (auto& kv : m_entries)
    {
        if (static_cast<int>(tiles.size()) >= maxUploads) break;
        Entry& e = kv.second;
        if (e.state != State::Decoded) continue;
        const uint32_t slot = m_slots.assign(e.key);
        if (slot == ThumbnailSlots::kNone) continue;         // atlas lleno este frame: mas tarde
        tiles.push_back({ slot, e.pixels.data() });
        batch.push_back(&e);
    }
    if (!tiles.empty())
    {
        const bool ok = m_upload && m_upload(tiles.data(), tiles.size());
        for (Entry* e : batch)
        {
            if (ok)
            {
                e->state = State::Ready;
            }
            else
            {
                e->state  = State::Failed;
                e->status = ThumbnailStatus::Unreadable;
                m_slots.release(e->key);
            }
            e->pixels.clear();
            e->pixels.shrink_to_fit();
        }
    }

    // 3. Nuevas decodificaciones, hasta el tope.
    startJobs();
}

void ThumbnailCache::startJobs()
{
    if (!m_run) return;
    for (auto q = m_queue.begin(); q != m_queue.end() && m_inFlight < m_maxInFlight;)
    {
        const auto it = m_entries.find(*q);
        if (it == m_entries.end() || it->second.state != State::Queued)
        {
            q = m_queue.erase(q);
            continue;
        }
        Entry& e = it->second;
        // Un FBX puede tardar segundos: como mucho m_maxModelsInFlight a la vez, y
        // los que esperan no bloquean a las imagenes que vienen detras en la cola.
        if (e.model && m_modelsInFlight >= m_maxModelsInFlight)
        {
            ++q;
            continue;
        }
        const std::string id = *q;
        q = m_queue.erase(q);

        e.state = State::Running;
        ++m_inFlight;
        if (e.model) ++m_modelsInFlight;
        const std::shared_ptr<Shared>                   shared = m_shared;
        const std::shared_ptr<const ThumbnailDiskCache> disk   = m_disk;
        const uint64_t                                  gen    = m_generation;
        const std::filesystem::path                     path   = e.path;
        const ThumbnailDependency                       self   = e.deps.front();
        const bool                                      model  = e.model;
        const Decoder                                   decode = m_decode;
        const bool accepted = m_run([shared, disk, gen, id, path, self, model, decode]() {
            Done d;
            d.generation = gen;
            d.path       = id;
            d.model      = model;
            // Un Done SIEMPRE llega: si algo lanza, el hueco en vuelo se
            // recogeria nunca y tras maxInFlight fallos no habria mas miniaturas.
            try
            {
                std::optional<ThumbnailResult> hit;
                if (disk) hit = disk->load(path);
                if (hit)
                {
                    d.result = std::move(*hit);
                }
                else
                {
                    d.result = decode(path);
                    stampDependencies(d.result, self);
                    if (disk) disk->store(path, d.result);
                }
            }
            catch (...) { d.result = ThumbnailResult{}; }
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->done.push_back(std::move(d));
        });
        if (!accepted)
        {
            // El pool no lo ejecutara jamas (parado): sin esto el hueco no se devuelve.
            --m_inFlight;
            if (model) --m_modelsInFlight;
            e.state  = State::Failed;
            e.status = ThumbnailStatus::Unreadable;
        }
    }
}

std::optional<ThumbnailStatus> ThumbnailCache::status(const std::filesystem::path& path) const
{
    const auto it = m_entries.find(path.string());
    if (it == m_entries.end()) return std::nullopt;
    if (it->second.state == State::Ready)  return ThumbnailStatus::Ok;
    if (it->second.state == State::Failed) return it->second.status;
    return std::nullopt;
}

void ThumbnailCache::refreshStamps()
{
    for (auto it = m_entries.begin(); it != m_entries.end();)
    {
        Entry& e = it->second;
        // Solo lo que se ha estado viendo, y nunca lo que tiene un job en marcha.
        const bool recent  = e.lastRequestFrame + 1 >= m_frame;
        const bool pending = (e.state == State::Queued || e.state == State::Running);
        if (!recent || pending)
        {
            ++it;
            continue;
        }
        const bool changed = std::any_of(e.deps.begin(), e.deps.end(), [](const ThumbnailDependency& d) {
            return !(stampFile(d.path) == d);
        });
        if (!changed)
        {
            ++it;
            continue;
        }
        m_slots.release(e.key);      // no-op si nunca tuvo casilla
        it = m_entries.erase(it);    // la siguiente peticion la trata como nueva
    }
}

void ThumbnailCache::newGeneration()
{
    ++m_generation;
    m_queue.clear();
    for (auto it = m_entries.begin(); it != m_entries.end();)
    {
        const State s = it->second.state;
        if (s == State::Queued || s == State::Running || s == State::Decoded)
            it = m_entries.erase(it);
        else
            ++it;
    }
}

} // namespace DonTopo
