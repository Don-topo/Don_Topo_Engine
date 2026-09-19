#pragma once
#include <cstdint>
#include <vector>

namespace DonTopo
{
    // Lo que el Animator pide a bone_eval cada frame, por capas: cada muestra
    // (clip, tiempo en ticks, peso) lleva su capa; cada capa, su peso, su modo,
    // su máscara y su pose congelada. Dentro de una capa los pesos (con la
    // congelada) suman 1. Vive fuera de AnimatorComponent.h para que la
    // interfaz del backend no tenga que incluir el Animator.
    static constexpr int kMaxLayersPose          = 8;
    // 3 por estado en un fade entre dos estados con blend 2D.
    static constexpr int kMaxPoseSamplesPerLayer = 6;

    struct PoseSample { int clip = 0; float time = 0.0f; float weight = 0.0f; int layer = 0; };

    struct PoseLayer
    {
        float    weight       = 1.0f;
        uint32_t mode         = 0;      // 0 override, 1 additive
        float    frozenWeight = 0.0f;   // 0 = no se usa la congelada
        // Este frame: copiar la pose de la capa a su congelada ANTES de
        // evaluar. Lo apaga quien la manda al backend (applySkinnedFrame), con
        // AnimatorComponent::clearFreezeRequest.
        bool     freezeNow    = false;
        // Uno por hueso (1 = la capa lo toca); null = todo el cuerpo. Lo posee
        // el Animator y solo vale durante setAnimationPose: el backend lo copia.
        const std::vector<uint8_t>* mask = nullptr;
    };

    struct AnimationPose
    {
        PoseSample samples[kMaxLayersPose * kMaxPoseSamplesPerLayer] = {};
        int        count = 0;
        PoseLayer  layers[kMaxLayersPose] = {};
        int        layerCount = 1;
        uint32_t   rootMotionMode = 0;
    };
}
