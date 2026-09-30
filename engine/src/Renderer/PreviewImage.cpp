// Decoding of a model's preview textures (Content Browser thumbnails).
// It lives apart from ModelLoader.cpp on purpose: the test
// test_no_uploader_decodes_embedded_on_its_own checks that no material
// uploader calls stbi_load_from_memory on its own, and this file is its
// only exception in Renderer. Here there is no path-or-embedded choice to make (the
// loader fills in one or the other), the header is inspected before decoding and no
// mips are generated: decodeMaterialTexture does neither of those.
#include "DonTopo/Renderer/ModelLoader.h"

#include <stb_image.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

namespace DonTopo
{
    PreviewImage downscalePreviewImage(const uint8_t* px, int w, int h)
    {
        PreviewImage out;
        if (!px || w <= 0 || h <= 0) return out;
        int dw = w, dh = h;
        if (std::max(w, h) > kPreviewMaxTexture)
        {
            const double s = static_cast<double>(kPreviewMaxTexture) / std::max(w, h);
            dw = std::clamp(static_cast<int>(w * s + 0.5), 1, kPreviewMaxTexture);
            dh = std::clamp(static_cast<int>(h * s + 0.5), 1, kPreviewMaxTexture);
        }
        out.w = dw;
        out.h = dh;
        out.rgba.assign(static_cast<size_t>(dw) * dh * 4, 0);
        for (int y = 0; y < dh; ++y)
        {
            const int y0 = static_cast<int>(static_cast<int64_t>(y) * h / dh);
            const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<int64_t>(y + 1) * h / dh));
            for (int x = 0; x < dw; ++x)
            {
                const int x0 = static_cast<int>(static_cast<int64_t>(x) * w / dw);
                const int x1 = std::max(x0 + 1, static_cast<int>(static_cast<int64_t>(x + 1) * w / dw));
                uint64_t sum[4] = {};
                for (int sy = y0; sy < y1; ++sy)
                    for (int sx = x0; sx < x1; ++sx)
                        for (int c = 0; c < 4; ++c)
                            sum[c] += px[(static_cast<size_t>(sy) * w + sx) * 4 + c];
                const uint64_t n = static_cast<uint64_t>(x1 - x0) * (y1 - y0);
                for (int c = 0; c < 4; ++c)
                    out.rgba[(static_cast<size_t>(y) * dw + x) * 4 + c] = static_cast<uint8_t>((sum[c] + n / 2) / n);
            }
        }
        return out;
    }

    PreviewImage ModelLoader::decodePreviewImage(const uint8_t* bytes, size_t size)
    {
        if (!bytes || size == 0 || size > static_cast<size_t>(INT_MAX)) return {};
        int w = 0, h = 0, comp = 0;
        if (!stbi_info_from_memory(bytes, static_cast<int>(size), &w, &h, &comp) || w <= 0 || h <= 0)
            return {};
        if (static_cast<uint64_t>(w) * static_cast<uint64_t>(h) > kPreviewMaxSourcePixels) return {};
        std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> px(
            stbi_load_from_memory(bytes, static_cast<int>(size), &w, &h, &comp, 4), &stbi_image_free);
        if (!px) return {};
        return downscalePreviewImage(px.get(), w, h);
    }

    PreviewImage ModelLoader::loadPreviewImage(const std::filesystem::path& path)
    {
        try
        {
            // ifstream and not stbi_load: stbi_load takes a char* in the local
            // codepage and fails with Unicode paths.
            std::ifstream in(path, std::ios::binary);
            if (!in) return {};
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            return decodePreviewImage(bytes.data(), bytes.size());
        }
        catch (...) { return {}; }
    }
}
