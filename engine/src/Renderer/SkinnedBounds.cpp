#include "DonTopo/Renderer/SkinnedBounds.h"
#include "DonTopo/Renderer/SkinnedMesh.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace DonTopo::Culling
{
    // Largest factor by which the 3x3 of m can stretch a vector, that is, its
    // largest singular value. It is used to bound how far a bone matrix can move
    // a vertex from that bone's origin.
    //
    // It is computed exactly (eigenvalues of m^T·m via the closed form for a
    // symmetric 3x3) instead of with an easy bound: the Frobenius norm is
    // sqrt(3) for the IDENTITY, and since this is multiplied along the
    // bone chain, a ten-level skeleton would come out 240 times larger
    // than it is and would never be culled.
    static float operatorNorm3(const glm::mat4& m)
    {
        const glm::mat3 r  = glm::mat3(m);
        const glm::mat3 a  = glm::transpose(r) * r;   // symmetric and positive semidefinite
        // Any diagonal element is a LOWER bound of the largest eigenvalue
        // (Rayleigh quotient over the axes). It is used as a safety net: if the closed
        // form drifts due to rounding, the result still does not come out short.
        float lambda = std::max(a[0][0], std::max(a[1][1], a[2][2]));

        const float p1 = a[1][0]*a[1][0] + a[2][0]*a[2][0] + a[2][1]*a[2][1];
        if (p1 > 0.0f)
        {
            const float q  = (a[0][0] + a[1][1] + a[2][2]) / 3.0f;
            const float p2 = (a[0][0]-q)*(a[0][0]-q) + (a[1][1]-q)*(a[1][1]-q)
                           + (a[2][2]-q)*(a[2][2]-q) + 2.0f*p1;
            const float p  = std::sqrt(p2 / 6.0f);
            if (p > 0.0f)
            {
                const glm::mat3 b   = (a - q * glm::mat3(1.0f)) * (1.0f / p);
                const float     det = glm::determinant(b);
                const float     phi = std::acos(std::clamp(det * 0.5f, -1.0f, 1.0f)) / 3.0f;
                lambda = std::max(lambda, q + 2.0f * p * std::cos(phi));
            }
        }
        return (lambda > 0.0f) ? std::sqrt(lambda) : 0.0f;
    }

    float skinnedBoundRadius(const SkinnedMesh& mesh)
    {
        const Skeleton& skel      = mesh.skeleton;
        const int       boneCount = (int)skel.names.size();
        if (boneCount <= 0 || mesh.skinnedVertices.empty()) return 0.0f;
        if ((int)skel.parentIndex.size() < boneCount ||
            (int)skel.inverseBindPose.size() < boneCount) return 0.0f;

        // r[b]: radius of the cloud of vertices that bone b drags, measured in
        // the space of the bone ITSELF, hence the inverseBindPose. It is
        // pose-invariant: skinning.comp applies the bone's world matrix on top of it and
        // nothing else, so the distance to the bone's origin can only grow through scale,
        // which is accounted for separately.
        std::vector<float> radio((size_t)boneCount, 0.0f);
        // A vertex's weights should sum to 1, but an FBX can bring them
        // unnormalized and skinning.comp does not normalize them: summing more than 1
        // would push the vertex beyond the convex hull of the bones.
        float maxPeso = 1.0f;
        for (const SkinnedVertex& v : mesh.skinnedVertices)
        {
            float suma = 0.0f;
            for (int i = 0; i < 4; i++)
            {
                const float w = v.boneWeights[i];
                if (w <= 0.0f) continue;              // same condition as skinning.comp
                const int b = v.boneIndices[i];
                if (b < 0 || b >= boneCount) continue; // garbage index: the shader would already read out of bounds
                suma += w;
                const glm::vec3 p = glm::vec3(skel.inverseBindPose[b] *
                                              glm::vec4(glm::vec3(v.position), 1.0f));
                radio[(size_t)b] = std::max(radio[(size_t)b], glm::length(p));
            }
            maxPeso = std::max(maxPeso, suma);
        }

        // Bound of each bone's LOCAL transform, taking the worst clip. Only the
        // extremes of the keys are looked at: that is what makes the bound valid
        // between keyframes too, without sampling poses.
        std::vector<float> maxTrans((size_t)boneCount, 0.0f);
        std::vector<float> maxEscala((size_t)boneCount, 0.0f);
        const size_t clipCount = mesh.animationClips.empty() ? 1u : mesh.animationClips.size();
        for (int b = 0; b < boneCount; b++)
        {
            // Local bind pose, the default for a bone the clip says nothing
            // about (same computation and same reason as packSkinnedClips).
            const glm::mat4 globalBind = glm::inverse(skel.inverseBindPose[b]);
            const int       padre      = skel.parentIndex[b];
            const glm::mat4 bindLocal  = (padre < 0) ? globalBind
                                                     : skel.inverseBindPose[padre] * globalBind;
            // The largest of the column vectors is NOT valid as a bound: if the FBX brings
            // shear the columns are not orthogonal and it would come out short.
            const float bindTrans  = glm::length(glm::vec3(bindLocal[3]));
            const float bindEscala = operatorNorm3(bindLocal);

            for (size_t c = 0; c < clipCount; c++)
            {
                const BoneChannel* ch = nullptr;
                if (!mesh.animationClips.empty())
                    for (const BoneChannel& cc : mesh.animationClips[c].channels)
                        if (cc.boneIndex == b) { ch = &cc; break; }

                // No channel, or a channel with no keys at all: bone_eval.comp
                // falls back to the whole bindLocal.
                const bool sinKeys = !ch || (ch->posKeys.empty() && ch->rotKeys.empty() &&
                                             ch->scaleKeys.empty());
                if (sinKeys)
                {
                    maxTrans[(size_t)b]  = std::max(maxTrans[(size_t)b],  bindTrans);
                    maxEscala[(size_t)b] = std::max(maxEscala[(size_t)b], bindEscala);
                    continue;
                }
                // With keys from another channel but none of its own, bone_eval uses the
                // shader's neutrals: position 0 and scale 1, NOT the bind ones.
                float t = 0.0f;
                for (const BoneKeyframe& k : ch->posKeys) t = std::max(t, glm::length(k.value));
                float s = 1.0f;
                for (const BoneKeyframe& k : ch->scaleKeys)
                    s = std::max(s, std::max(std::abs(k.value.x),
                                  std::max(std::abs(k.value.y), std::abs(k.value.z))));
                maxTrans[(size_t)b]  = std::max(maxTrans[(size_t)b],  t);
                maxEscala[(size_t)b] = std::max(maxEscala[(size_t)b], s);
            }
        }

        // Propagation through the hierarchy. reach[b] = maximum distance from the bone's
        // origin to the model's origin; chain[b] = bound of the scale
        // accumulated from the root. The local translation of b is applied by the
        // linear part of the PARENT, hence it uses chain[parent] and not chain[b].
        //
        // It relies on the parent coming before the child, the same topological
        // order that bone_hierarchy.comp already depends on.
        std::vector<float> alcance((size_t)boneCount, 0.0f);
        std::vector<float> cadena((size_t)boneCount, 1.0f);
        float R = 0.0f;
        for (int b = 0; b < boneCount; b++)
        {
            const int   padre       = skel.parentIndex[b];
            const bool  tienePadre  = (padre >= 0 && padre < b);
            const float alcancePadre = tienePadre ? alcance[(size_t)padre] : 0.0f;
            const float cadenaPadre  = tienePadre ? cadena[(size_t)padre]  : 1.0f;
            alcance[(size_t)b] = alcancePadre + cadenaPadre * maxTrans[(size_t)b];
            cadena[(size_t)b]  = cadenaPadre * maxEscala[(size_t)b];
            R = std::max(R, alcance[(size_t)b] + cadena[(size_t)b] * radio[(size_t)b]);
        }

        R *= maxPeso;
        // A NaN or an infinity slipping in from the model would make the culling test pass
        // unpredictably: better to return "no bound".
        return std::isfinite(R) ? R : 0.0f;
    }
}  // namespace DonTopo::Culling
