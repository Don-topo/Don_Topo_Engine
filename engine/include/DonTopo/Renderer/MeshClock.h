#pragma once

#include <cmath>

namespace DonTopo
{
    // The clock of the path WITHOUT an Animator: a character with no component
    // plays its clip 0 in a loop and the backend is the one that keeps the time.
    //
    // It lives here, and not in each backend, because it was written twice and the two
    // copies had diverged (row A13 of the animation audit): Vulkan
    // multiplied by ticksPerSecond and froze the clock of a hidden mesh;
    // D3D12 did not even store ticksPerSecond (it added the frame's seconds to a
    // clock that the compute reads in TICKS, so it ran between 24 and 30 times
    // slower) and did not look at visibility either. With an Animator nothing showed,
    // because there the time is computed by the AnimatorComponent and arrives already in ticks.
    //
    // animTime and durationTicks are in Assimp TICKS (`aiAnimation::mDuration`,
    // what AnimationClip::duration stores); dt is in seconds.
    inline float advanceMeshClock(float animTime, float dt, float ticksPerSecond,
                                  float durationTicks, bool visible)
    {
        // Without a rate or without a duration there is no clip to sample: moving the clock
        // would only make it so the wrap could never bound it.
        if (ticksPerSecond <= 0.0f || durationTicks <= 0.0f) return animTime;
        // Hidden: it is not seen, so its clock does not run either. When it is marked
        // visible again it resumes where it left off instead of jumping forward.
        if (!visible) return animTime;
        float t = animTime + dt * ticksPerSecond;
        if (t > durationTicks) t = std::fmod(t, durationTicks);
        return t;
    }
}
