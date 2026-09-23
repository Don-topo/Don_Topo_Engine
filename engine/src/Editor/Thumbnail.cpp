#include "DonTopo/Editor/Thumbnail.h"

#include <stb_image.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <fstream>
#include <iterator>
#include <memory>

namespace DonTopo {

ThumbnailResult makeThumbnail(const std::filesystem::path& path)
{
    ThumbnailResult out;

    // Se lee el fichero entero y se decodifica desde memoria: stbi_load recibe un
    // char* en la codepage local y falla con rutas Unicode en Windows; ifstream no.
    std::ifstream in(path, std::ios::binary);
    if (!in) return out;
    const std::vector<unsigned char> bytes{ std::istreambuf_iterator<char>(in),
                                            std::istreambuf_iterator<char>() };
    if (bytes.empty() || bytes.size() > static_cast<size_t>(INT_MAX)) return out;

    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp) ||
        w <= 0 || h <= 0)
        return out;
    if (static_cast<uint64_t>(w) * static_cast<uint64_t>(h) > kThumbMaxSourcePixels)
    {
        out.status = ThumbnailStatus::TooLarge;
        return out;
    }

    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> px(
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4),
        &stbi_image_free);
    if (!px) return out;

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
                    const unsigned char* p = px.get() + (static_cast<size_t>(sy) * w + sx) * 4;
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
                               uint32_t slotCapacity)
    : m_run(std::move(run))
    , m_upload(std::move(upload))
    , m_maxInFlight(maxInFlight)
    , m_slots(slotCapacity)
{}

void ThumbnailCache::beginFrame()
{
    ++m_frame;
    m_slots.beginFrame();
}

uint64_t ThumbnailCache::makeKey(const std::filesystem::path& path,
                                 std::filesystem::file_time_type mtime)
{
    // Ruta + mtime: cambiar el contenido con el mismo nombre cambia la clave.
    const uint64_t h = std::hash<std::string>{}(path.string());
    const uint64_t t = static_cast<uint64_t>(mtime.time_since_epoch().count());
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
        std::error_code ec;
        e.mtime            = std::filesystem::last_write_time(path, ec);
        e.key              = makeKey(path, e.mtime);
        e.state            = ec ? State::Failed : State::Queued;
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
        if (d.generation != m_generation) continue;          // carpeta anterior: se ignora
        const auto it = m_entries.find(d.path);
        if (it == m_entries.end() || it->second.state != State::Running) continue;
        Entry& e = it->second;
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
                e->state = State::Failed;
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
    while (m_inFlight < m_maxInFlight && !m_queue.empty())
    {
        const std::string id = m_queue.front();
        m_queue.pop_front();
        const auto it = m_entries.find(id);
        if (it == m_entries.end() || it->second.state != State::Queued) continue;

        it->second.state = State::Running;
        ++m_inFlight;
        const std::shared_ptr<Shared>     shared = m_shared;
        const uint64_t                    gen    = m_generation;
        const std::filesystem::path       path   = it->second.path;
        m_run([shared, gen, id, path]() {
            Done d;
            d.generation = gen;
            d.path       = id;
            d.result     = makeThumbnail(path);
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->done.push_back(std::move(d));
        });
    }
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
        std::error_code ec;
        const auto now = std::filesystem::last_write_time(e.path, ec);
        if (ec || now == e.mtime)
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
