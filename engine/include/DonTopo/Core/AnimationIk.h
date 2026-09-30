#pragma once
#include <cstdint>
#include <glm/glm.hpp>

namespace DonTopo
{
    // What the Animator asks bone_ik.comp every frame: the IK constraints
    // already resolved against the scene (bone, chain and target in MODEL
    // space). It lives outside AnimatorComponent.h so that the backend interface
    // does not have to include the Animator, just like AnimationPose.
    static constexpr int kMaxIkPose = 4;

    struct IkSolve
    {
        uint32_t  type = 0;               // 0 LookAt, 1 TwoBone
        // LookAt uses only bone; TwoBone the whole chain: grandParent (shoulder),
        // parent (elbow) and bone (hand).
        int       bone = -1, parent = -1, grandParent = -1;
        float     weight = 0.0f;
        glm::vec3 target{ 0.0f };
        glm::vec3 pole{ 0.0f };
        uint32_t  hasPole = 0;
        glm::vec3 aimAxis{ 0.0f, 0.0f, 1.0f };
        float     maxAngle = 80.0f;       // degrees
    };

    struct AnimationIk
    {
        IkSolve solves[kMaxIkPose] = {};
        int     count = 0;
    };
}
