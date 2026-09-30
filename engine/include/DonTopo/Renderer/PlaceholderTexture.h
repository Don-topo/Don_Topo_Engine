#pragma once
#include <cstdint>
#include <vector>

namespace DonTopo {

// The two fillers used when a material does not end up giving a texture.
// There are TWO and not one on purpose, because the two situations are not the same:
//
//  - A procedural primitive (cube, sphere, plane...) carries no texture because
//    it does not need one. There the right filler is white: the shader multiplies
//    by it and the object comes out with its base color, decorating nothing.
//
//  - A texture that the material DOES declare but that could not be read (the
//    file is missing, or corrupt) is a failure, and it has to be visible. There goes the
//    checkerboard, which is the "something is missing here" convention.
//
// Confusing them is what each backend did on its own and in opposite
// directions: Vulkan also painted the checkerboard in the legitimate case, and DirectX 12
// also painted white in the failure case, so a missing file
// went unnoticed. Besides, a plain white surface does not let you judge anything that
// depends on detail (for example whether the shadow map changed resolution).

// Side of the checkerboard, in texels, and side of each tile.
constexpr int kMissingTextureSize = 64;
constexpr int kMissingTextureTile = 8;

// RGBA8, kMissingTextureSize x kMissingTextureSize. Grays and not magenta: saturated
// magenta is confused with an emissive material, and the goal is for it to
// read as "missing texture", not as a color of the scene.
inline std::vector<uint8_t> makeMissingTextureRgba()
{
    std::vector<uint8_t> px(static_cast<size_t>(kMissingTextureSize) * kMissingTextureSize * 4);
    for (int y = 0; y < kMissingTextureSize; ++y) {
        for (int x = 0; x < kMissingTextureSize; ++x) {
            const bool claro = ((x / kMissingTextureTile) + (y / kMissingTextureTile)) % 2 == 0;
            uint8_t*   p     = px.data() + (static_cast<size_t>(y) * kMissingTextureSize + x) * 4;
            p[0] = p[1] = p[2] = claro ? 0xCC : 0x88;
            p[3]               = 0xFF;
        }
    }
    return px;
}

}  // namespace DonTopo
