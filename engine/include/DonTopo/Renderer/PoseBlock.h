#pragma once
#include "DonTopo/Core/AnimationPose.h"
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace DonTopo
{
    // Per-character pose block read by bone_eval.comp (binding 10), in
    // uints (floats by their bits). The backend keeps one copy per frame in
    // flight and passes the shader the offset of this frame's copy by push
    // constant: this way it is rewritten every frame without overwriting the one the GPU still reads.
    //   [0] layerCount  [1] sampleCount  [2..3] padding
    //   layers  [4 + 4L]              weight, mode, frozenWeight, hasMask
    //   samples [36 + 4k]             clipBase, time, weight, layer   (k < 48)
    //   masks   [228 + L*boneCount + bone]   0 / 1
    // If it changes, it changes at the same time in bone_eval.comp (kLayers/kSamples/kMasks).
    constexpr uint32_t kPoseBlockLayers  = 4;
    constexpr uint32_t kPoseBlockSamples = kPoseBlockLayers + 4 * kMaxLayersPose;
    constexpr uint32_t kPoseBlockMasks   = kPoseBlockSamples + 4 * kMaxLayersPose * kMaxPoseSamplesPerLayer;
    static_assert(kPoseBlockSamples == 36 && kPoseBlockMasks == 228,
                  "bone_eval.comp reads the pose block at these offsets");

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

    // Writes the pose into dst (poseBlockUints(boneCount) uints). Each sample's
    // clip comes out as clipBase = clip * boneCount, the BoneInfos block.
    // The masks of layers without a mask are not written: the shader does not read them.
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
