#pragma once
#include <algorithm>
#include <cmath>

// Calculo puro del alpha del splash por fase. Sin GPU ni estado: dado el tiempo
// transcurrido y si la carga termino (y cuando), devuelve el alpha y en que
// fase esta. Testeable en headless (engine/tests/splash_tests.cpp).
struct SplashTimings { float fadeIn = 0.3f; float minTotal = 1.5f; float fadeOut = 0.3f; };
struct SplashState   { float alpha; bool crossfading; bool done; };

inline SplashState splashStateAt(const SplashTimings& t, float elapsed,
                                 bool loadingDone, float loadingDoneAt)
{
    // Fase 1: fade-in.
    if (elapsed < t.fadeIn)
        return { std::clamp(elapsed / t.fadeIn, 0.0f, 1.0f), false, false };

    // El fade-out no puede empezar hasta que la carga termino Y se cumplio el
    // minimo total. Mientras no, hold a alpha 1.
    if (!loadingDone)
        return { 1.0f, false, false };

    const float fadeOutStart = std::max(loadingDoneAt, t.minTotal);
    if (elapsed < fadeOutStart)
        return { 1.0f, false, false }; // hold hasta el minimo

    // Fase 3: fade-out (crossfade). alpha 1 -> 0.
    const float k = (elapsed - fadeOutStart) / t.fadeOut;
    if (k >= 1.0f) return { 0.0f, true, true };
    return { 1.0f - k, true, false };
}
