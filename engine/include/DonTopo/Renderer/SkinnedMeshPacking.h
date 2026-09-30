#pragma once
#include <string>
#include <vector>
#include "DonTopo/Renderer/SkinnedMesh.h"

namespace DonTopo
{
    // Keyframes of ALL the clips concatenated in the same vectors, ready
    // to upload to the 3 SSBOs in one go when building the object. Switching
    // clip at runtime does not touch VRAM again: only the clipBase of the push
    // constant changes.
    //
    // boneInfos goes in [clip][bone] layout: the entry of bone b in clip c
    // is at boneInfos[c * boneCount + b], and c * boneCount is exactly the
    // clipBase that bone_eval.comp consumes.
    //
    // parentIndex and inverseBindPose belong to the SKELETON, not the clip, so they are
    // replicated identically in each block. That costs 96 B per bone and clip (2.4 %
    // over the keyframes of a typical character) and in exchange leaves the block of
    // clip 0 serving as a valid hierarchy for any clip, which is why
    // bone_hierarchy.comp does not need to know anything about clips.
    struct PackedClips
    {
        std::vector<GpuPosKey>   pos;
        std::vector<GpuRotKey>   rot;
        std::vector<GpuPosKey>   scale;
        std::vector<GpuBoneInfo> boneInfos;
    };

    // Free and pure function (no Vulkan) on purpose: inside
    // Renderer::addSkinnedMesh this packing could only be tested with a
    // live VkDevice, that is, it could not be tested.
    PackedClips packSkinnedClips(const SkinnedMesh& mesh);

    // Content key of the read-only GPU buffers of a character (B6): input
    // vertices, indices and the packed clips. Two clones of the same model give
    // the same key and share those buffers. Hashed 8 bytes at a time: FNV byte by
    // byte over the ~13 MB of a real character cost ~10 ms per spawn.
    std::string skinnedGeometryKey(const SkinnedMesh& mesh, const PackedClips& packed);

    // Clip blocks that the BoneInfos SSBO carries. Never 0: without animations a
    // block is packed anyway, so clip 0 is always valid.
    inline uint32_t skinnedClipCount(const SkinnedMesh& mesh)
    {
        return mesh.animationClips.empty() ? 1u : (uint32_t)mesh.animationClips.size();
    }

    // Clip index ready to multiply by boneCount. Out of range it falls back to
    // clip 0: the clip list may have shrunk, or a -1 may arrive from an
    // unresolved state (0xFFFFFFFF after the cast), and clip * boneCount
    // would point outside the SSBO with the compute silently reading garbage.
    //
    // It lives here and not in each backend because the guard existed in Vulkan and
    // was missing in D3D12: a single function keeps them from drifting apart again.
    inline uint32_t clampClipIndex(uint32_t clip, uint32_t clipCount)
    {
        return clip < clipCount ? clip : 0u;
    }
}
