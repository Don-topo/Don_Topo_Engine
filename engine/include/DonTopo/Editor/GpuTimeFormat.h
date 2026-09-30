#pragma once
#include <cstdio>
#include <cstddef>

namespace DonTopo
{
    // How a per-pass GPU time is written, in ONE place only.
    //
    // The rule that matters is the one for the unmeasured value. A pass returns <= 0
    // when it has not run this frame: either its effect is off, or the capture does
    // not yet have the two frames it needs. The Performance panel already drew it as
    // "--", but the eight lines of the View menu did a plain "%.3f" and printed
    // "0.000 ms" (H57).
    //
    // And that is not a style detail: "0.000 ms" reads as "this effect is free", which
    // is exactly the opposite conclusion of "there is no measurement". Worse, it is
    // confused with a pass that really costs nothing. The number is there to decide
    // whether an effect is expensive, so lying in that case empties the rest of
    // meaning.
    //
    // Returns `buf` so it can be used directly in an ImGui::Text.
    inline const char* gpuMsText(float ms, char* buf, std::size_t n)
    {
        if (n == 0) return buf;
        if (ms > 0.0f) std::snprintf(buf, n, "%.3f", (double)ms);
        else           std::snprintf(buf, n, "--");
        return buf;
    }

    // More than enough size for "%.3f" of any representable float.
    constexpr std::size_t kGpuMsTextSize = 32;
}
