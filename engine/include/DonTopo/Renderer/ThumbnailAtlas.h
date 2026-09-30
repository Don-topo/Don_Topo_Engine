#pragma once
#include <cstddef>
#include <cstdint>

// What the editor and the backends share about the Content Browser's shared
// thumbnail atlas: dimensions, and nothing else. The logic lives in the editor.
namespace DonTopo {

constexpr uint32_t kThumbCell       = 64;                                    // side of a cell
constexpr uint32_t kThumbAtlasCells = 32;                                    // cells per side
constexpr uint32_t kThumbAtlasSize  = kThumbCell * kThumbAtlasCells;         // 2048 px
constexpr uint32_t kThumbSlotCount  = kThumbAtlasCells * kThumbAtlasCells;   // 1024

struct UvRect { float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f; };

// A cell ready to upload: `rgba` points to kThumbCell*kThumbCell*4 bytes.
struct ThumbnailTile
{
    uint32_t       slot = 0;
    const uint8_t* rgba = nullptr;
};

// UV of a cell, with half a texel of margin on each side so that linear
// filtering does not bleed the color of the neighboring cell.
inline UvRect thumbnailUv(uint32_t slot)
{
    const uint32_t x = (slot % kThumbAtlasCells) * kThumbCell;
    const uint32_t y = (slot / kThumbAtlasCells) * kThumbCell;
    const float    s = static_cast<float>(kThumbAtlasSize);
    return { (x + 0.5f) / s, (y + 0.5f) / s,
             (x + kThumbCell - 0.5f) / s, (y + kThumbCell - 0.5f) / s };
}

} // namespace DonTopo
