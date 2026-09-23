#include "DonTopo/Renderer/TextureImport.h"

#include <algorithm>

namespace DonTopo
{
    uint32_t mipLevelCount(uint32_t w, uint32_t h)
    {
        if (w == 0 || h == 0) return 1;
        uint32_t levels = 1;
        for (uint32_t d = std::max(w, h); d > 1; d >>= 1) ++levels;
        return levels;
    }

    std::vector<TextureMip> buildMipChain(const uint8_t* rgba, uint32_t w, uint32_t h)
    {
        std::vector<TextureMip> chain;
        if (!rgba || w == 0 || h == 0) return chain;

        const uint32_t levels = mipLevelCount(w, h);
        chain.reserve(levels - 1);

        const uint8_t* src = rgba;
        uint32_t       sw  = w, sh = h;
        for (uint32_t level = 1; level < levels; ++level)
        {
            const uint32_t dw = std::max(1u, sw / 2);
            const uint32_t dh = std::max(1u, sh / 2);
            TextureMip mip;
            mip.w = dw;
            mip.h = dh;
            mip.rgba.assign(static_cast<size_t>(dw) * dh * 4, 0);

            for (uint32_t y = 0; y < dh; ++y)
            {
                const uint32_t y0 = static_cast<uint32_t>(static_cast<uint64_t>(y) * sh / dh);
                uint32_t       y1 = static_cast<uint32_t>(static_cast<uint64_t>(y + 1) * sh / dh);
                if (y1 <= y0) y1 = y0 + 1;
                for (uint32_t x = 0; x < dw; ++x)
                {
                    const uint32_t x0 = static_cast<uint32_t>(static_cast<uint64_t>(x) * sw / dw);
                    uint32_t       x1 = static_cast<uint32_t>(static_cast<uint64_t>(x + 1) * sw / dw);
                    if (x1 <= x0) x1 = x0 + 1;

                    uint64_t sumR = 0, sumG = 0, sumB = 0, sumA = 0;
                    for (uint32_t sy = y0; sy < y1; ++sy)
                        for (uint32_t sx = x0; sx < x1; ++sx)
                        {
                            const uint8_t* p = src + (static_cast<size_t>(sy) * sw + sx) * 4;
                            const uint64_t a = p[3];
                            sumR += p[0] * a;
                            sumG += p[1] * a;
                            sumB += p[2] * a;
                            sumA += a;
                        }
                    const uint64_t count = static_cast<uint64_t>(x1 - x0) * (y1 - y0);
                    uint8_t* d = &mip.rgba[(static_cast<size_t>(y) * dw + x) * 4];
                    if (sumA > 0)
                    {
                        d[0] = static_cast<uint8_t>((sumR + sumA / 2) / sumA);
                        d[1] = static_cast<uint8_t>((sumG + sumA / 2) / sumA);
                        d[2] = static_cast<uint8_t>((sumB + sumA / 2) / sumA);
                    }
                    d[3] = static_cast<uint8_t>((sumA + count / 2) / count);
                }
            }
            chain.push_back(std::move(mip));
            src = chain.back().rgba.data();
            sw  = dw;
            sh  = dh;
        }
        return chain;
    }

    bool resolveSrgb(TextureKind kind, ColorSpaceOverride o)
    {
        switch (o)
        {
            case ColorSpaceOverride::Srgb:   return true;
            case ColorSpaceOverride::Linear: return false;
            case ColorSpaceOverride::Auto:   break;
        }
        return kind == TextureKind::BaseColor;
    }

    std::string textureKeySuffix(const std::string& path)
    {
        if (path.empty()) return {};
        const TextureImportSettings s = loadTextureImportSettings(path);
        if (isDefault(s)) return {};
        std::string out = "#";
        out += s.colorSpace == ColorSpaceOverride::Srgb ? 's'
             : s.colorSpace == ColorSpaceOverride::Linear ? 'l' : 'a';
        if (s.mipmaps) out += 'm';
        return out;
    }
}
