#include "DonTopo/Renderer/SkinnedMeshPacking.h"

namespace DonTopo
{
    PackedClips packSkinnedClips(const SkinnedMesh& mesh)
    {
        const Skeleton& skel      = mesh.skeleton;
        const int       boneCount = (int)skel.names.size();
        // Malla sin animaciones: un bloque igualmente, con todos los counts a 0.
        const size_t clipCount = mesh.animationClips.empty() ? 1u : mesh.animationClips.size();

        // Sin clips, bone_eval deja la identidad en cada localXform y la pasada 1
        // de bone_hierarchy propaga identidades, pero la pasada 2 multiplica por
        // inverseBindPose: finalBones acabaría valiendo la inverse bind pose y
        // skinning.comp deformaría la malla. Como skinnedVertices YA está en bind
        // pose, la matriz correcta sin animación es la identidad, así que aquí se
        // codifica la identidad en el propio campo inverseBindPose para que la
        // pasada 2 salga identidad * identidad, exacta y sin tocar shaders. Ojo:
        // en este caso el campo NO contiene la inverse bind pose real.
        const bool bindPoseEstatica = mesh.animationClips.empty();

        PackedClips out;
        out.boneInfos.resize(clipCount * (size_t)boneCount);

        for (size_t c = 0; c < clipCount; c++)
        {
            const AnimationClip* clip = mesh.animationClips.empty() ? nullptr : &mesh.animationClips[c];

            for (int b = 0; b < boneCount; b++)
            {
                GpuBoneInfo& bi    = out.boneInfos[c * (size_t)boneCount + (size_t)b];
                bi.parentIndex     = skel.parentIndex[b];
                bi.inverseBindPose = bindPoseEstatica ? glm::mat4(1.0f) : skel.inverseBindPose[b];
                bi.pad             = 0;

                const BoneChannel* ch = nullptr;
                if (clip)
                    for (auto& cc : clip->channels)
                        if (cc.boneIndex == b) { ch = &cc; break; }

                bi.posOffset = (int)out.pos.size();
                bi.posCount  = ch ? (int)ch->posKeys.size() : 0;
                for (int k = 0; k < bi.posCount; k++)
                {
                    GpuPosKey pk{};
                    pk.timePad = { ch->posKeys[k].time, 0, 0, 0 };
                    pk.value   = { ch->posKeys[k].value.x, ch->posKeys[k].value.y, ch->posKeys[k].value.z, 0 };
                    out.pos.push_back(pk);
                }

                bi.rotOffset = (int)out.rot.size();
                bi.rotCount  = ch ? (int)ch->rotKeys.size() : 0;
                for (int k = 0; k < bi.rotCount; k++)
                {
                    const glm::quat& q = ch->rotKeys[k].value;
                    GpuRotKey rk{};
                    rk.timePad = { ch->rotKeys[k].time, 0, 0, 0 };
                    rk.value   = { q.x, q.y, q.z, q.w };
                    out.rot.push_back(rk);
                }

                bi.scaleOffset = (int)out.scale.size();
                bi.scaleCount  = ch ? (int)ch->scaleKeys.size() : 0;
                for (int k = 0; k < bi.scaleCount; k++)
                {
                    GpuPosKey sk{};
                    sk.timePad = { ch->scaleKeys[k].time, 0, 0, 0 };
                    sk.value   = { ch->scaleKeys[k].value.x, ch->scaleKeys[k].value.y, ch->scaleKeys[k].value.z, 0 };
                    out.scale.push_back(sk);
                }
            }
        }

        // Vulkan no acepta buffers de tamaño 0
        if (out.pos.empty())   out.pos.push_back({});
        if (out.rot.empty())   out.rot.push_back({});
        if (out.scale.empty()) out.scale.push_back({});

        return out;
    }
}
