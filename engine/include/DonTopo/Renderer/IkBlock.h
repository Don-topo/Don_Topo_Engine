#pragma once
#include "DonTopo/Core/AnimationIk.h"
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace DonTopo
{
    // Bloque de IK por personaje que lee bone_ik.comp (binding 11), en uints
    // (los floats por sus bits). Una copia por frame en vuelo, con su offset en
    // el push constant, igual que el bloque de pose.
    //   [0] count  [1..3] relleno
    //   restricción k, en 4 + 16k:
    //     type, bone, parent, grandParent, weight,
    //     targetX, targetY, targetZ, poleX, poleY, poleZ, hasPole,
    //     aimX, aimY, aimZ, maxAngle
    // Si cambia, cambia a la vez en bone_ik.comp.
    constexpr uint32_t kIkBlockSolves = 4;
    constexpr uint32_t kIkSolveUints  = 16;

    inline uint32_t ikBlockUints() { return kIkBlockSolves + kIkSolveUints * (uint32_t)kMaxIkPose; }

    inline uint32_t ikBlockBits(float f)
    {
        uint32_t u;
        std::memcpy(&u, &f, sizeof(u));
        return u;
    }

    // Escribe ik en dst (ikBlockUints() uints). Los índices de hueso viajan
    // como uint y el shader los lee con int(): un -1 llega como 0xFFFFFFFF.
    inline void writeIkBlock(const AnimationIk& ik, uint32_t* dst)
    {
        const int n = std::clamp(ik.count, 0, kMaxIkPose);
        dst[0] = (uint32_t)n;
        dst[1] = dst[2] = dst[3] = 0u;
        for (int k = 0; k < kMaxIkPose; k++)
        {
            uint32_t* s = dst + kIkBlockSolves + kIkSolveUints * (uint32_t)k;
            if (k >= n)
            {
                for (uint32_t i = 0; i < kIkSolveUints; i++) s[i] = 0u;
                continue;
            }
            const IkSolve& v = ik.solves[k];
            s[0]  = v.type;
            s[1]  = (uint32_t)v.bone;
            s[2]  = (uint32_t)v.parent;
            s[3]  = (uint32_t)v.grandParent;
            s[4]  = ikBlockBits(v.weight);
            s[5]  = ikBlockBits(v.target.x);
            s[6]  = ikBlockBits(v.target.y);
            s[7]  = ikBlockBits(v.target.z);
            s[8]  = ikBlockBits(v.pole.x);
            s[9]  = ikBlockBits(v.pole.y);
            s[10] = ikBlockBits(v.pole.z);
            s[11] = v.hasPole;
            s[12] = ikBlockBits(v.aimAxis.x);
            s[13] = ikBlockBits(v.aimAxis.y);
            s[14] = ikBlockBits(v.aimAxis.z);
            s[15] = ikBlockBits(v.maxAngle);
        }
    }
}
