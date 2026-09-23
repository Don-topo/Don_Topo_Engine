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

} // namespace DonTopo
