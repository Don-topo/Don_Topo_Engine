#include "DonTopo/Renderer/RootMotion.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Core/AnimatorComponent.h"
#include <algorithm>
#include <cmath>

namespace DonTopo
{
    namespace
    {
        const BoneChannel* rootChannel(const SkinnedMesh& mesh, int clipIndex)
        {
            if (clipIndex < 0 || clipIndex >= (int)mesh.animationClips.size()) return nullptr;
            for (const auto& ch : mesh.animationClips[clipIndex].channels)
            {
                if (ch.posKeys.empty()) continue;
                if (ch.boneIndex < 0 || ch.boneIndex >= (int)mesh.skeleton.parentIndex.size()) continue;
                if (mesh.skeleton.parentIndex[ch.boneIndex] < 0) return &ch;
            }
            return nullptr;
        }
    }

    glm::vec3 sampleRootPosition(const SkinnedMesh& mesh, int clipIndex, double t)
    {
        const BoneChannel* ch = rootChannel(mesh, clipIndex);
        if (!ch) return glm::vec3(0.0f);
        const auto& k = ch->posKeys;
        if (t <= k.front().time) return k.front().value;
        if (t >= k.back().time)  return k.back().value;
        for (size_t i = 0; i + 1 < k.size(); i++)
        {
            if (k[i + 1].time > t)
            {
                const double span = (double)k[i + 1].time - k[i].time;
                const float  f    = span > 0.0 ? (float)((t - k[i].time) / span) : 0.0f;
                return glm::mix(k[i].value, k[i + 1].value, f);
            }
        }
        return k.back().value;
    }

    glm::vec3 rootDisplacement(const SkinnedMesh& mesh, int clipIndex, double T, double duration, bool loop)
    {
        if (duration <= 0.0 || T <= 0.0) return glm::vec3(0.0f);
        const glm::vec3 p0 = sampleRootPosition(mesh, clipIndex, 0.0);
        if (!loop)
            return sampleRootPosition(mesh, clipIndex, std::min(T, duration)) - p0;
        // Ciclos completos + el resto: el wrap del loop no teletransporta.
        const double ciclos = std::floor(T / duration);
        const double resto  = T - ciclos * duration;
        const glm::vec3 porCiclo = sampleRootPosition(mesh, clipIndex, duration) - p0;
        return (float)ciclos * porCiclo + (sampleRootPosition(mesh, clipIndex, resto) - p0);
    }

    glm::vec3 rootMotionDelta(const SkinnedMesh& mesh, const AnimatorComponent& anim)
    {
        glm::vec3 d(0.0f);
        for (const auto& s : anim.rootMotionSamples())
            d += s.weight * (rootDisplacement(mesh, s.clip, s.ticks1, s.duration, s.loop)
                           - rootDisplacement(mesh, s.clip, s.ticks0, s.duration, s.loop));
        // Solo horizontal: la Y de la raíz se queda en la pose (bone_eval, modo 2).
        d.y = 0.0f;
        return d;
    }
}
