#pragma once
#include "DonTopo/Core/ImportSettings.h"
#include "DonTopo/Renderer/SharedTextureCache.h"   // TextureKind

#include <cstdint>
#include <string>
#include <vector>

namespace DonTopo
{
    // One RGBA8 mip level. Level 0 is the image itself and is not repeated here.
    struct TextureMip
    {
        uint32_t             w = 0, h = 0;
        std::vector<uint8_t> rgba;
    };

    // Levels of a full chain down to 1x1: 1 + floor(log2(max(w, h))). 1 if
    // either dimension is 0.
    uint32_t mipLevelCount(uint32_t w, uint32_t h);

    // Levels 1..N-1 of the chain (dimension max(1, d/2) each time, same as
    // Vulkan and D3D12), with an ALPHA-WEIGHTED box filter: a transparent pixel
    // does not darken its opaque neighbors. Empty if there is nothing to do (1x1,
    // null pointer, dimension 0). Does not modify the input. The encoded value is
    // averaged, also in sRGB (known limitation, see the spec).
    std::vector<TextureMip> buildMipChain(const uint8_t* rgba, uint32_t w, uint32_t h);

    // Auto = decided by the slot (only the base color is sRGB); Srgb/Linear override it.
    // It is the ONLY place that decides the format of a material texture.
    bool resolveSrgb(TextureKind kind, ColorSpaceOverride o);

    // Cache key suffix with the import settings of `path`. Empty if the path
    // is empty or the settings are the default ones, so today's keys do not
    // change. Reads the sidecar from disk.
    std::string textureKeySuffix(const std::string& path);
}
