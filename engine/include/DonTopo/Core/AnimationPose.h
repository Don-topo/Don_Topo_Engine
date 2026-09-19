#pragma once
#include <cstdint>

namespace DonTopo
{
    // Lo que el Animator pide a bone_eval cada frame: hasta 4 clips con su
    // tiempo (ticks) y su peso, más la pose congelada con el suyo. Los pesos
    // suman 1. Vive fuera de AnimatorComponent.h para que la interfaz del
    // backend no tenga que incluir el Animator.
    struct PoseSample { int clip = 0; float time = 0.0f; float weight = 0.0f; };
    // 3 por estado en un fade entre dos estados con blend 2D.
    static constexpr int kMaxPoseSamples = 6;

    struct AnimationPose
    {
        PoseSample samples[kMaxPoseSamples] = {};
        int        count = 0;
        float      frozenWeight = 0.0f;   // 0 = no se usa la congelada
        // Este frame: copiar la pose actual a la congelada ANTES de evaluar.
        // Lo apaga quien la manda al backend (applySkinnedFrame), con
        // AnimatorComponent::clearFreezeRequest.
        bool       freezeNow = false;
        uint32_t   rootMotionMode = 0;
    };
}
