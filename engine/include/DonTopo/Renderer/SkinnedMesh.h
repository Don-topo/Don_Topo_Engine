#pragma once
#include <cstddef>
#include <vector>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "DonTopo/Renderer/Mesh.h"

namespace DonTopo
{
    struct SkinnedVertex
    {
        glm::vec4 position;
        glm::vec4 normal;
        glm::vec4 tangent;
        glm::vec4 uv_pad;
        glm::vec4 color;
        glm::ivec4 boneIndices;
        glm::vec4 boneWeights;
    };

    struct BoneKeyframe { float time; glm::vec3 value; };
    struct BoneKeyframeQ { float time; glm::quat value; };

    struct BoneChannel
    {
        int boneIndex;
        std::vector<BoneKeyframe> posKeys, scaleKeys;
        std::vector<BoneKeyframeQ> rotKeys;
    };

    struct AnimationClip
    {
        std::string                 name;
        float                       duration;
        float                       ticksPerSecond;
        std::vector<BoneChannel>    channels;
    };

    struct Skeleton
    {
        std::vector<std::string>                names;
        std::vector<int>                        parentIndex;
        std::vector<glm::mat4>                  inverseBindPose;
        std::unordered_map<std::string, int>    boneMap;
    };

    struct SubMeshRange {
        uint32_t indexStart;
        uint32_t indexCount;
        uint32_t materialIndex;
    };

    // File that one or more clips came from. The list exists so that clips can be
    // shown grouped by origin in the Animator Panel and so that a whole file can be
    // removed; the GPU evaluation never looks at it, it keeps
    // consuming the flat animationClips.
    //
    // builtin marks the FBX that provided the mesh and the skeleton: it cannot be
    // removed (removing it would remove the model) and the scene rebuilds it via
    // Mesh::sourcePath, not via addAnimationSource.
    struct AnimationSource
    {
        std::string              path;
        bool                     builtin = false;
        std::vector<std::string> clipNames; // final names, in the order they were added
    };

    // Distributes the weights of a vertex so that they sum to 1 (A9). Returns false
    // when the FBX did not weight that vertex against ANY bone: then all
    // four come out as 0 and the shader leaves it where it is, because a zero
    // skinning matrix would send the vertex to the origin.
    //
    // It lives here and not inside the loader loop so that the degenerate case
    // can be tested, which no asset in the repo produces.
    inline bool normalizeBoneWeights(const float in[4], float out[4])
    {
        float total = 0.0f;
        for (int i = 0; i < 4; i++) total += in[i];
        for (int i = 0; i < 4; i++) out[i] = (total > 0.0f) ? in[i] / total : 0.0f;
        return total > 0.0f;
    }

    struct SkinnedMesh : Mesh
    {
        std::vector<SkinnedVertex>   skinnedVertices;
        Skeleton                     skeleton;
        // All the animations of the source file, in the order of
        // scene->mAnimations. The Animator references them by name (not by
        // index): re-exporting the model can reorder them.
        std::vector<AnimationClip>   animationClips;
        // Origin of each clip. Invariant: the concatenation of the clipNames of
        // all the sources is a permutation of the names of animationClips.
        std::vector<AnimationSource> animationSources;
        std::vector<SubMeshRange>    subMeshRanges;
        std::vector<Material>        materials;
        // Vertices that the FBX did not weight against any bone (A9). Nobody moves them:
        // the shader leaves them where they are (identity) instead of sending them
        // to the origin, but they look still while the rest animates. It is a defect
        // of the model, and without this counter there is no way to tell it apart from an
        // engine failure.
        int                          verticesWithoutWeights = 0;
    };

    struct GpuPosKey
    {
        glm::vec4 timePad;
        glm::vec4 value;
    };

    struct GpuRotKey
    {
        glm::vec4 timePad;
        glm::vec4 value;
    };

    // Exact mirror of the BoneInfo struct in shaders/bone_eval.comp and
    // shaders/bone_hierarchy.comp (std430). Any field added here
    // must be added in BOTH shaders and in the same place: a mismatch does not
    // give a compile error, only shifted reads.
    struct GpuBoneInfo
    {
        int32_t posOffset, posCount;
        int32_t rotOffset, rotCount;
        int32_t scaleOffset, scaleCount;
        int32_t parentIndex;
        // Levels below its root (0 = root). bone_hierarchy.comp evaluates
        // the hierarchy in parallel level by level, so it requires each bone to
        // be worth exactly one more than its parent. It takes the slot that used to be
        // padding: the std430 layout does not change.
        int32_t depth;
        glm::mat4 inverseBindPose;
        // Local transform of the bone in bind pose, that is, the one relative to
        // its parent as they rigged it. It is the default value for a bone
        // the active clip says nothing about: identity is not valid, because it
        // would erase its offset from the parent and collapse it on top of it.
        glm::mat4 bindLocal;
    };

    // Offsets and size that glslc generates for the std430 BoneInfo of both
    // shaders (verified with spirv-dis: Offset 0/4/8/12/16/20/24/28/32/96,
    // ArrayStride 160). A layout mismatch between CPU and GPU gives no compile
    // error anywhere, only shifted reads and garbage on screen,
    // so it is checked here, where the build can fail.
    static_assert(offsetof(GpuBoneInfo, parentIndex)     == 24, "BoneInfo std430 layout broken");
    static_assert(offsetof(GpuBoneInfo, inverseBindPose) == 32, "BoneInfo std430 layout broken");
    static_assert(offsetof(GpuBoneInfo, bindLocal)       == 96, "BoneInfo std430 layout broken");
    static_assert(sizeof(GpuBoneInfo)                    == 160, "BoneInfo std430 layout broken");
}
