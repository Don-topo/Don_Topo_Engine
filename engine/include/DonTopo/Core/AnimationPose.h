#pragma once
#include <cstdint>
#include <vector>

namespace DonTopo
{
    // What the Animator asks bone_eval every frame, by layers: each sample
    // (clip, time in ticks, weight) carries its layer; each layer, its weight, its mode,
    // its mask and its frozen pose. Within a layer the weights (with the
    // frozen one) sum to 1. It lives outside AnimatorComponent.h so that the
    // backend interface does not have to include the Animator.
    static constexpr int kMaxLayersPose          = 8;
    // 3 per state in a fade between two states with 2D blend.
    static constexpr int kMaxPoseSamplesPerLayer = 6;

    struct PoseSample { int clip = 0; float time = 0.0f; float weight = 0.0f; int layer = 0; };

    struct PoseLayer
    {
        float    weight       = 1.0f;
        uint32_t mode         = 0;      // 0 override, 1 additive
        float    frozenWeight = 0.0f;   // 0 = the frozen pose is not used
        // This frame: copy the layer pose to its frozen pose BEFORE
        // evaluating. Whoever sends it to the backend (applySkinnedFrame) turns it off, with
        // AnimatorComponent::clearFreezeRequest.
        bool     freezeNow    = false;
        // One per bone (1 = the layer touches it); null = whole body. Owned
        // by the Animator and only valid during setAnimationPose: the backend copies it.
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
