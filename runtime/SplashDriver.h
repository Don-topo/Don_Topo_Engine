#pragma once
#include <algorithm>

// Pure computation of the splash alpha per phase. No GPU and no state: given the elapsed
// time and whether loading finished (and when), it returns the alpha and which
// phase it is in. Testable headless (engine/tests/splash_tests.cpp).
struct SplashTimings { float fadeIn = 0.3f; float minTotal = 1.5f; float fadeOut = 0.3f; };
struct SplashState   { float alpha; bool crossfading; bool done; };

inline SplashState splashStateAt(const SplashTimings& t, float elapsed,
                                 bool loadingDone, float loadingDoneAt)
{
    // Phase 1: fade-in.
    if (elapsed < t.fadeIn)
        return { std::clamp(elapsed / t.fadeIn, 0.0f, 1.0f), false, false };

    // The fade-out cannot start until loading finished AND the total
    // minimum has elapsed. Until then, hold at alpha 1.
    if (!loadingDone)
        return { 1.0f, false, false };

    const float fadeOutStart = std::max(loadingDoneAt, t.minTotal);
    if (elapsed < fadeOutStart)
        return { 1.0f, false, false }; // hold until the minimum

    // Phase 3: fade-out (crossfade). alpha 1 -> 0.
    const float k = (elapsed - fadeOutStart) / t.fadeOut;
    if (k >= 1.0f) return { 0.0f, true, true };
    return { 1.0f - k, true, false };
}
