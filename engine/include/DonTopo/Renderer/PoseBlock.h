#pragma once
#include "DonTopo/Core/AnimationPose.h"
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace DonTopo
{
    // Bloque de pose por personaje que lee bone_eval.comp (binding 10), en
    // uints (los floats con sus bits). El backend guarda una copia por frame en
    // vuelo y le pasa al shader el offset de la de este frame por push
    // constant: así se reescribe cada frame sin pisar la que la GPU aún lee.
    //   [0] layerCount  [1] sampleCount  [2..3] relleno
    //   capas    [4 + 4L]              weight, mode, frozenWeight, hasMask
    //   muestras [36 + 4k]             clipBase, time, weight, layer   (k < 48)
    //   máscaras [228 + L*boneCount + hueso]   0 / 1
    // Si cambia, cambia a la vez en bone_eval.comp (kLayers/kSamples/kMasks).
    constexpr uint32_t kPoseBlockLayers  = 4;
    constexpr uint32_t kPoseBlockSamples = kPoseBlockLayers + 4 * kMaxLayersPose;
    constexpr uint32_t kPoseBlockMasks   = kPoseBlockSamples + 4 * kMaxLayersPose * kMaxPoseSamplesPerLayer;
    static_assert(kPoseBlockSamples == 36 && kPoseBlockMasks == 228,
                  "bone_eval.comp lee el bloque de pose en estos offsets");

    inline uint32_t poseBlockUints(uint32_t boneCount)
    {
        return kPoseBlockMasks + (uint32_t)kMaxLayersPose * boneCount;
    }

    inline uint32_t poseBlockBits(float f)
    {
        uint32_t u;
        std::memcpy(&u, &f, sizeof(u));
        return u;
    }

    // Escribe la pose en dst (poseBlockUints(boneCount) uints). El clip de cada
    // muestra sale como clipBase = clip * boneCount, el bloque de BoneInfos.
    // Las máscaras de las capas sin máscara no se escriben: el shader no las lee.
    inline void writePoseBlock(const AnimationPose& pose, uint32_t boneCount, uint32_t* dst)
    {
        const int nL = std::clamp(pose.layerCount, 1, kMaxLayersPose);
        const int nS = std::clamp(pose.count, 0, kMaxLayersPose * kMaxPoseSamplesPerLayer);
        dst[0] = (uint32_t)nL;
        dst[1] = (uint32_t)nS;
        dst[2] = dst[3] = 0u;
        for (int L = 0; L < kMaxLayersPose; L++)
        {
            uint32_t* c = dst + kPoseBlockLayers + 4 * L;
            if (L >= nL) { c[0] = c[1] = c[2] = c[3] = 0u; continue; }
            const PoseLayer& pl = pose.layers[L];
            c[0] = poseBlockBits(pl.weight);
            c[1] = pl.mode;
            c[2] = poseBlockBits(pl.frozenWeight);
            c[3] = pl.mask ? 1u : 0u;
            if (!pl.mask) continue;
            uint32_t* m = dst + kPoseBlockMasks + (uint32_t)L * boneCount;
            for (uint32_t b = 0; b < boneCount; b++)
                m[b] = (b < pl.mask->size() && (*pl.mask)[b]) ? 1u : 0u;
        }
        for (int k = 0; k < kMaxLayersPose * kMaxPoseSamplesPerLayer; k++)
        {
            uint32_t* s = dst + kPoseBlockSamples + 4 * k;
            if (k >= nS) { s[0] = s[1] = s[2] = s[3] = 0u; continue; }
            const PoseSample& ps = pose.samples[k];
            s[0] = (uint32_t)std::max(0, ps.clip) * boneCount;
            s[1] = poseBlockBits(ps.time);
            s[2] = poseBlockBits(ps.weight);
            s[3] = (uint32_t)std::max(0, ps.layer);
        }
    }
}
