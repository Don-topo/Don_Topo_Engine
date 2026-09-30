#include "DonTopo/Renderer/SkinnedMeshPacking.h"
#include <vector>

namespace DonTopo
{
    PackedClips packSkinnedClips(const SkinnedMesh& mesh)
    {
        const Skeleton& skel      = mesh.skeleton;
        const int       boneCount = (int)skel.names.size();
        // Mesh without animations: a block all the same, with all counts at 0.
        const size_t clipCount = mesh.animationClips.empty() ? 1u : mesh.animationClips.size();

        PackedClips out;
        out.boneInfos.resize(clipCount * (size_t)boneCount);

        // Depth of each bone, once per mesh (it belongs to the skeleton, not to
        // the clip). The order is topological (parent < child, ModelLoader), so
        // a single pass is enough. A parent that is out of range or later is treated
        // as a root: better one root too many than reading a depth not yet
        // computed.
        std::vector<int32_t> depth((size_t)boneCount, 0);
        for (int b = 0; b < boneCount; b++)
        {
            const int padre = skel.parentIndex[b];
            depth[b] = (padre >= 0 && padre < b) ? depth[padre] + 1 : 0;
        }

        for (size_t c = 0; c < clipCount; c++)
        {
            const AnimationClip* clip = mesh.animationClips.empty() ? nullptr : &mesh.animationClips[c];

            for (int b = 0; b < boneCount; b++)
            {
                GpuBoneInfo& bi    = out.boneInfos[c * (size_t)boneCount + (size_t)b];
                bi.parentIndex     = skel.parentIndex[b];
                bi.inverseBindPose = skel.inverseBindPose[b];
                bi.depth           = depth[b];

                // Local bind pose, the default value for a bone that the active clip
                // says nothing about. Identity does NOT work: it would erase the
                // bone's offset from its parent and collapse it onto
                // it, dragging its whole descendant chain along (an entire arm
                // ended up at head height).
                //
                //   globalBind[b] = inverse(inverseBindPose[b])
                //   localBind[b]  = inverse(globalBind[parent]) * globalBind[b]
                //
                // and since inverse(globalBind[parent]) IS inverseBindPose[parent],
                // one inversion per bone is enough instead of two. The whole matrix is
                // passed to the GPU instead of decomposing it into TRS: that way it is exact.
                const glm::mat4 globalBind = glm::inverse(skel.inverseBindPose[b]);
                const int       padre      = skel.parentIndex[b];
                bi.bindLocal = (padre < 0) ? globalBind
                                           : skel.inverseBindPose[padre] * globalBind;

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

        // Vulkan does not accept buffers of size 0
        if (out.pos.empty())   out.pos.push_back({});
        if (out.rot.empty())   out.rot.push_back({});
        if (out.scale.empty()) out.scale.push_back({});

        return out;
    }
}
