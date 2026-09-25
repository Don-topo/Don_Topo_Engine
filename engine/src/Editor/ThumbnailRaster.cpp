#include "DonTopo/Editor/ThumbnailRaster.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace DonTopo {

namespace {

constexpr int kSS  = 4;                                     // supersampling por eje
constexpr int kRes = static_cast<int>(kThumbCell) * kSS;    // 256 internos

bool finite3(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

float toLinear(uint8_t c) { return std::pow(c / 255.0f, 2.2f); }

uint8_t toSrgb8(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(std::pow(v, 1.0f / 2.2f) * 255.0f));
}

float clamp01(float v, float fallback) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : fallback; }

glm::vec3 sampleAlbedo(const PreviewImage& img, glm::vec2 uv)
{
    const float u = std::isfinite(uv.x) ? uv.x - std::floor(uv.x) : 0.0f;
    const float v = std::isfinite(uv.y) ? uv.y - std::floor(uv.y) : 0.0f;
    const int   x = std::clamp(static_cast<int>(u * img.w), 0, img.w - 1);
    const int   y = std::clamp(static_cast<int>(v * img.h), 0, img.h - 1);
    const uint8_t* p = &img.rgba[(static_cast<size_t>(y) * img.w + x) * 4];
    return { toLinear(p[0]), toLinear(p[1]), toLinear(p[2]) };
}

} // namespace

ThumbnailResult rasterizeThumbnail(const std::vector<PreviewPart>& parts)
{
    ThumbnailResult out;   // Unreadable

    constexpr float kMax = std::numeric_limits<float>::max();
    glm::vec3 lo(kMax), hi(-kMax);
    bool any = false;
    for (const PreviewPart& part : parts)
        for (const glm::vec3& p : part.positions)
            if (finite3(p)) { lo = glm::min(lo, p); hi = glm::max(hi, p); any = true; }
    if (!any) return out;

    // Vista 3/4 desde arriba, ortografica, ajustada a las 8 esquinas del bbox.
    const glm::vec3 center = (lo + hi) * 0.5f;
    const glm::mat4 view = glm::rotate(glm::mat4(1.0f), glm::radians(25.0f), glm::vec3(1, 0, 0)) *
                           glm::rotate(glm::mat4(1.0f), glm::radians(-35.0f), glm::vec3(0, 1, 0)) *
                           glm::translate(glm::mat4(1.0f), -center);
    float extent = 1e-6f;
    for (int i = 0; i < 8; ++i)
    {
        const glm::vec3 corner((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
        const glm::vec4 q = view * glm::vec4(corner, 1.0f);
        extent = std::max({ extent, std::abs(q.x), std::abs(q.y) });
    }
    const float half = extent * 1.08f;                       // 8 % de margen

    const glm::mat3 normalView(view);
    const glm::vec3 key     = glm::normalize(glm::vec3(-0.5f, 0.8f, 0.6f));   // espacio de vista
    const glm::vec3 fill    = glm::normalize(glm::vec3(0.7f, 0.1f, 0.5f));
    const glm::vec3 halfVec = glm::normalize(key + glm::vec3(0, 0, 1));

    std::vector<glm::vec3> color(static_cast<size_t>(kRes) * kRes);
    std::vector<float>     depth(static_cast<size_t>(kRes) * kRes, kMax);
    std::vector<uint8_t>   covered(static_cast<size_t>(kRes) * kRes, 0);
    bool drew = false;

    for (const PreviewPart& part : parts)
    {
        const size_t n = part.positions.size();
        std::vector<glm::vec3> screen(n), normal(n);
        for (size_t i = 0; i < n; ++i)
        {
            const glm::vec4 q = view * glm::vec4(part.positions[i], 1.0f);
            screen[i] = { (q.x / half * 0.5f + 0.5f) * kRes, (0.5f - q.y / half * 0.5f) * kRes, -q.z };
            const glm::vec3 nn = i < part.normals.size() ? normalView * part.normals[i] : glm::vec3(0, 0, 1);
            normal[i] = (finite3(nn) && glm::length(nn) > 1e-6f) ? glm::normalize(nn) : glm::vec3(0, 0, 1);
        }
        const PreviewImage& img = part.albedo;
        const bool textured = img.w > 0 && img.h > 0 &&
                              img.rgba.size() == static_cast<size_t>(img.w) * img.h * 4;
        const float metallic  = clamp01(part.metallic, 0.0f);
        const float roughness = clamp01(part.roughness, 0.5f);
        const float shininess = std::exp2(10.0f * (1.0f - roughness) + 1.0f);

        for (size_t t = 0; t + 2 < part.indices.size(); t += 3)
        {
            const uint32_t i0 = part.indices[t], i1 = part.indices[t + 1], i2 = part.indices[t + 2];
            if (i0 >= n || i1 >= n || i2 >= n) continue;
            const glm::vec3 a = screen[i0], b = screen[i1], d = screen[i2];
            if (!finite3(a) || !finite3(b) || !finite3(d)) continue;
            const float area = (b.x - a.x) * (d.y - a.y) - (b.y - a.y) * (d.x - a.x);
            if (!(std::abs(area) > 1e-12f)) continue;

            const int x0 = std::max(0, static_cast<int>(std::floor(std::min({ a.x, b.x, d.x }))));
            const int x1 = std::min(kRes - 1, static_cast<int>(std::ceil(std::max({ a.x, b.x, d.x }))));
            const int y0 = std::max(0, static_cast<int>(std::floor(std::min({ a.y, b.y, d.y }))));
            const int y1 = std::min(kRes - 1, static_cast<int>(std::ceil(std::max({ a.y, b.y, d.y }))));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    const float px = x + 0.5f, py = y + 0.5f;
                    const float w0 = ((b.x - px) * (d.y - py) - (b.y - py) * (d.x - px)) / area;
                    const float w1 = ((d.x - px) * (a.y - py) - (d.y - py) * (a.x - px)) / area;
                    const float w2 = 1.0f - w0 - w1;
                    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                    const float  z = w0 * a.z + w1 * b.z + w2 * d.z;
                    const size_t o = static_cast<size_t>(y) * kRes + x;
                    if (z >= depth[o]) continue;

                    glm::vec3 albedo(1.0f);
                    if (textured)
                    {
                        const glm::vec2 uv0 = i0 < part.uvs.size() ? part.uvs[i0] : glm::vec2(0.0f);
                        const glm::vec2 uv1 = i1 < part.uvs.size() ? part.uvs[i1] : glm::vec2(0.0f);
                        const glm::vec2 uv2 = i2 < part.uvs.size() ? part.uvs[i2] : glm::vec2(0.0f);
                        albedo = sampleAlbedo(img, w0 * uv0 + w1 * uv1 + w2 * uv2);
                    }
                    else if (i0 < part.colors.size() && i1 < part.colors.size() && i2 < part.colors.size())
                    {
                        albedo = w0 * part.colors[i0] + w1 * part.colors[i1] + w2 * part.colors[i2];
                    }

                    glm::vec3 nrm = w0 * normal[i0] + w1 * normal[i1] + w2 * normal[i2];
                    nrm = glm::length(nrm) > 1e-6f ? glm::normalize(nrm) : glm::vec3(0, 0, 1);
                    if (nrm.z < 0.0f) nrm = -nrm;                // doble cara

                    const glm::vec3 diffuse = albedo * (1.0f - metallic);
                    const glm::vec3 f0      = glm::mix(glm::vec3(0.04f), albedo, metallic);
                    const float     spec    = std::pow(std::max(0.0f, glm::dot(nrm, halfVec)), shininess) *
                                              (shininess + 8.0f) / 25.0f;
                    // Ambiente cielo/suelo: imita el IBL lo justo para dar volumen.
                    const glm::vec3 ambient = glm::mix(glm::vec3(0.18f, 0.17f, 0.16f),
                                                       glm::vec3(0.35f, 0.38f, 0.45f), nrm.y * 0.5f + 0.5f);
                    const glm::vec3 lit =
                        diffuse * (ambient + 1.1f * std::max(0.0f, glm::dot(nrm, key)) +
                                   0.3f * std::max(0.0f, glm::dot(nrm, fill))) +
                        f0 * (spec + ambient * 1.5f);
                    if (!finite3(lit)) continue;

                    color[o]   = lit;
                    depth[o]   = z;
                    covered[o] = 1;
                    drew       = true;
                }
        }
    }
    if (!drew) return out;

    // Reduccion 4x4: color medio de lo cubierto, alfa = fraccion cubierta.
    std::vector<uint8_t> tile(static_cast<size_t>(kThumbCell) * kThumbCell * 4, 0);
    for (uint32_t y = 0; y < kThumbCell; ++y)
        for (uint32_t x = 0; x < kThumbCell; ++x)
        {
            glm::vec3 sum(0.0f);
            int       count = 0;
            for (int j = 0; j < kSS; ++j)
                for (int i = 0; i < kSS; ++i)
                {
                    const size_t o = static_cast<size_t>(y * kSS + j) * kRes + x * kSS + i;
                    if (covered[o]) { sum += color[o]; ++count; }
                }
            if (count == 0) continue;
            const glm::vec3 rgb = sum / static_cast<float>(count);
            uint8_t* p = &tile[(static_cast<size_t>(y) * kThumbCell + x) * 4];
            p[0] = toSrgb8(rgb.r);
            p[1] = toSrgb8(rgb.g);
            p[2] = toSrgb8(rgb.b);
            p[3] = static_cast<uint8_t>((count * 255 + kSS * kSS / 2) / (kSS * kSS));
        }

    // Borde oscuro de 1 px alrededor de la silueta: la separa del fondo del boton.
    out.rgba = tile;
    for (uint32_t y = 0; y < kThumbCell; ++y)
        for (uint32_t x = 0; x < kThumbCell; ++x)
        {
            if (tile[(static_cast<size_t>(y) * kThumbCell + x) * 4 + 3] > 0) continue;
            int neighbour = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const int xx = static_cast<int>(x) + dx, yy = static_cast<int>(y) + dy;
                    if (xx < 0 || yy < 0 || xx >= static_cast<int>(kThumbCell) || yy >= static_cast<int>(kThumbCell))
                        continue;
                    neighbour = std::max<int>(neighbour, tile[(static_cast<size_t>(yy) * kThumbCell + xx) * 4 + 3]);
                }
            if (neighbour == 0) continue;
            uint8_t* p = &out.rgba[(static_cast<size_t>(y) * kThumbCell + x) * 4];
            p[0] = p[1] = p[2] = 15;
            p[3] = static_cast<uint8_t>(neighbour * 0.6f);
        }
    out.status = ThumbnailStatus::Ok;
    return out;
}

PreviewPart makePreviewSphere()
{
    constexpr int kSeg = 48, kRings = 24;
    PreviewPart p;
    for (int j = 0; j <= kRings; ++j)
        for (int i = 0; i <= kSeg; ++i)
        {
            const float th = glm::pi<float>() * j / kRings;
            const float ph = 2.0f * glm::pi<float>() * i / kSeg;
            const glm::vec3 n(std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph));
            p.positions.push_back(n);
            p.normals.push_back(n);
            p.uvs.emplace_back(2.0f * i / kSeg, static_cast<float>(j) / kRings);
            p.colors.emplace_back(1.0f);
        }
    for (int j = 0; j < kRings; ++j)
        for (int i = 0; i < kSeg; ++i)
        {
            const uint32_t k = static_cast<uint32_t>(j * (kSeg + 1) + i);
            p.indices.insert(p.indices.end(), { k, k + kSeg + 1, k + 1, k + 1, k + kSeg + 1, k + kSeg + 2 });
        }
    return p;
}

} // namespace DonTopo
