#pragma once
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

    // Fichero del que salieron uno o más clips. La lista existe para poder
    // mostrar los clips agrupados por origen en el Animator Panel y para poder
    // quitar un fichero entero; la evaluación en GPU no la mira nunca, sigue
    // consumiendo animationClips plano.
    //
    // builtin marca el FBX que aportó la malla y el esqueleto: no se puede
    // quitar (quitarlo sería quitar el modelo) y la escena lo reconstruye vía
    // Mesh::sourcePath, no vía addAnimationSource.
    struct AnimationSource
    {
        std::string              path;
        bool                     builtin = false;
        std::vector<std::string> clipNames; // nombres finales, en el orden en que se añadieron
    };

    struct SkinnedMesh : Mesh
    {
        std::vector<SkinnedVertex>   skinnedVertices;
        Skeleton                     skeleton;
        // Todas las animaciones del fichero de origen, en el orden de
        // scene->mAnimations. El Animator las referencia por nombre (no por
        // índice): reexportar el modelo puede reordenarlas.
        std::vector<AnimationClip>   animationClips;
        // Origen de cada clip. Invariante: la concatenación de los clipNames de
        // todas las fuentes es una permutación de los nombres de animationClips.
        std::vector<AnimationSource> animationSources;
        std::vector<SubMeshRange>    subMeshRanges;
        std::vector<Material>        materials;
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
