#pragma once
#include <glm/glm.hpp>
#include <cstdint>

namespace DonTopo
{
    // Subpixel jitter of the TAA. Shared by both backends on purpose:
    // it was written twice (AaPass.cpp and D3D12Renderer.cpp) with the same
    // sequence, the same cycle and the same way of putting it into the projection.
    //
    // If they drift apart no error is given: the TAA keeps converging, only that
    // to a slightly different image depending on the backend, and that is only seen by
    // placing the two captures one on top of the other.

    // Halton sequence in base b: the low-discrepancy sequence with which
    // the TAA spreads the samples within the pixel. It covers the area much more
    // uniformly than a random one, which is what makes the temporal
    // average converge to a real supersampling.
    inline float halton(uint32_t index, uint32_t base)
    {
        float result = 0.0f;
        float f      = 1.0f;
        while (index > 0)
        {
            f      /= static_cast<float>(base);
            result += f * static_cast<float>(index % base);
            index  /= base;
        }
        return result;
    }

    // How many positions before repeating. Enough for the average to be
    // stable, and few enough that the cycle is not noticed when the camera stops.
    constexpr uint32_t TAA_JITTER_CYCLE = 16;

    // Offset of this frame, in PIXELS, within [-0.5, 0.5] * scale.
    // It advances the index, so it is called ONCE per frame.
    inline glm::vec2 taaJitterPixels(uint32_t& index, float scale)
    {
        const glm::vec2 j((halton(index + 1, 2) - 0.5f) * scale,
                          (halton(index + 1, 3) - 0.5f) * scale);
        index = (index + 1) % TAA_JITTER_CYCLE;
        return j;
    }

    // Puts the jitter into the projection. In clip space the full width is 2, hence
    // the factor. It goes on the Z column so that the offset is
    // constant on screen at any depth; on the translation one it
    // would depend on distance and the TAA would average samples that do not cover the
    // same area.
    //
    // width/height are those of the INTERNAL render, not the window's: with SSAA
    // they are not the same and the jitter has to be measured in the pixel that is drawn.
    inline void applyTaaJitter(glm::mat4& proj, const glm::vec2& jitterPx,
                               float width, float height)
    {
        if (width <= 0.0f || height <= 0.0f) return;
        proj[2][0] += 2.0f * jitterPx.x / width;
        proj[2][1] += 2.0f * jitterPx.y / height;
    }
}
