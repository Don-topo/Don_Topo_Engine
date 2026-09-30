#pragma once
#include <glm/glm.hpp>

namespace DonTopo
{
    struct SkinnedMesh;
    class AnimatorComponent;

    // CPU root motion, no GPU: position and displacement of a clip's root from
    // its keyframes. The root is the first parentless bone with position keys
    // in that clip; its keys are in model space (the GPU hierarchy does not
    // apply the nodes above the root).

    // Root position at t ticks, linearly interpolated like bone_eval and
    // clamped to the first and last key. With no root with keys: (0,0,0).
    glm::vec3 sampleRootPosition(const SkinnedMesh& mesh, int clipIndex, double t);

    // Displacement from 0 to T accumulated ticks of a clock of duration
    // `duration`. When looping, it adds one P(duration) - P(0) per full cycle.
    glm::vec3 rootDisplacement(const SkinnedMesh& mesh, int clipIndex, double T, double duration, bool loop);

    // Horizontal delta (y = 0) of the Animator's last update, in model
    // space, weighting its rootMotionSamples.
    glm::vec3 rootMotionDelta(const SkinnedMesh& mesh, const AnimatorComponent& anim);
}
