#pragma once
#include <cstdint>

namespace DonTopo
{
    // Numbers that BOTH backends must see as equal and that do not depend on
    // any graphics API. They were declared twice (one copy in the
    // Vulkan path and another in D3D12Renderer.cpp, this one with a comment
    // admitting they were "the same values as the Vulkan path").
    //
    // That comment was the only defense: nothing forces the two copies to
    // match, and letting them drift apart gives no error in any validation layer. The
    // image comes out different depending on the backend and it only shows when comparing captures.

    // ── IBL ─────────────────────────────────────────────────────────────────
    // Side of the two precomputed ambient cubemaps.
    constexpr uint32_t IBL_IRRADIANCE_SIZE = 32;
    constexpr uint32_t IBL_PREFILTER_SIZE  = 128;
    // Mips of the specular prefilter: the prefilter spreads roughness across
    // them and pbr.frag takes it for granted.
    //
    // CAREFUL, this number lives in THREE places and only two can be shared: here,
    // and as `#define IBL_PREFILTER_MIPS` in shaders/pbr.frag. A shader cannot
    // include a C++ header, and putting it in the UBO block would silently shift it
    // for the six shaders that declare it (std140). If it changes
    // here, it has to be changed THERE by hand.
    constexpr uint32_t IBL_PREFILTER_MIPS  = 5;

    // ── Bloom ───────────────────────────────────────────────────────────────
    // Levels of the reduction chain. More levels = wider halo and cheaper
    // to compute, but below a few pixels the mip stops contributing and only
    // costs two dispatches.
    constexpr int BLOOM_MIPS = 5;
}
