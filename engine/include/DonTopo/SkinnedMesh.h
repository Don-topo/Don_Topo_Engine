#pragma once
#include <vector>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "DonTopo/Mesh.h"

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

    struct SkinnedMesh : Mesh
    {
        std::vector<SkinnedVertex>  skinnedVertices;
        Skeleton                    skeleton;
        AnimationClip               animationClip;
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

    struct GpuBoneInfo
    {
        int32_t posOffset, posCount;
        int32_t rotOffset, rotCount;
        int32_t scaleOffset, scaleCount;
        int32_t parentIndex;
        int32_t pad;
        glm::mat4 inverseBindPose;
    };
}
