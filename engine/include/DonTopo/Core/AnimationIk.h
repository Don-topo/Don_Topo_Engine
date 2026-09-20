#pragma once
#include <cstdint>
#include <glm/glm.hpp>

namespace DonTopo
{
    // Lo que el Animator pide a bone_ik.comp cada frame: las restricciones de
    // IK ya resueltas contra la escena (hueso, cadena y objetivo en espacio del
    // MODELO). Vive fuera de AnimatorComponent.h para que la interfaz del
    // backend no tenga que incluir el Animator, igual que AnimationPose.
    static constexpr int kMaxIkPose = 4;

    struct IkSolve
    {
        uint32_t  type = 0;               // 0 LookAt, 1 TwoBone
        // LookAt usa solo bone; TwoBone la cadena entera: grandParent (hombro),
        // parent (codo) y bone (mano).
        int       bone = -1, parent = -1, grandParent = -1;
        float     weight = 0.0f;
        glm::vec3 target{ 0.0f };
        glm::vec3 pole{ 0.0f };
        uint32_t  hasPole = 0;
        glm::vec3 aimAxis{ 0.0f, 0.0f, 1.0f };
        float     maxAngle = 80.0f;       // grados
    };

    struct AnimationIk
    {
        IkSolve solves[kMaxIkPose] = {};
        int     count = 0;
    };
}
