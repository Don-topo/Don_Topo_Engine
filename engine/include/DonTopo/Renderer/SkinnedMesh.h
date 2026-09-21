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

    // Reparte los pesos de un vértice para que sumen 1 (A9). Devuelve false
    // cuando el FBX no pesó ese vértice contra NINGÚN hueso: entonces los
    // cuatro salen a 0 y el shader lo deja donde está, porque una matriz de
    // skinning a cero mandaría el vértice al origen.
    //
    // Vive aquí y no dentro del bucle del cargador para poder probar el caso
    // degenerado, que ningún asset del repo produce.
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
        // Todas las animaciones del fichero de origen, en el orden de
        // scene->mAnimations. El Animator las referencia por nombre (no por
        // índice): reexportar el modelo puede reordenarlas.
        std::vector<AnimationClip>   animationClips;
        // Origen de cada clip. Invariante: la concatenación de los clipNames de
        // todas las fuentes es una permutación de los nombres de animationClips.
        std::vector<AnimationSource> animationSources;
        std::vector<SubMeshRange>    subMeshRanges;
        std::vector<Material>        materials;
        // Vértices que el FBX no pesó contra ningún hueso (A9). No los mueve
        // nadie: el shader los deja donde están (identidad) en vez de mandarlos
        // al origen, pero se ven quietos mientras el resto anima. Es un defecto
        // del modelo, y sin este contador no hay forma de distinguirlo de un
        // fallo del motor.
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

    // Espejo exacto del struct BoneInfo de shaders/bone_eval.comp y
    // shaders/bone_hierarchy.comp (std430). Cualquier campo que se añada aquí
    // hay que añadirlo en LOS DOS shaders y en el mismo sitio: un desajuste no
    // da error de compilación, sólo lecturas desplazadas.
    struct GpuBoneInfo
    {
        int32_t posOffset, posCount;
        int32_t rotOffset, rotCount;
        int32_t scaleOffset, scaleCount;
        int32_t parentIndex;
        // Niveles por debajo de su raíz (0 = raíz). bone_hierarchy.comp evalúa
        // la jerarquía en paralelo nivel a nivel, así que exige que cada hueso
        // valga exactamente uno más que su padre. Ocupa el hueco que antes era
        // relleno: el layout std430 no cambia.
        int32_t depth;
        glm::mat4 inverseBindPose;
        // Transform local del hueso en bind pose, o sea el que tiene respecto a
        // su padre tal y como lo rigearon. Es el valor por defecto de un hueso
        // del que el clip activo no dice nada: la identidad no vale, porque
        // borraría su offset con el padre y lo colapsaría encima de él.
        glm::mat4 bindLocal;
    };

    // Offsets y tamaño que glslc genera para el BoneInfo std430 de los dos
    // shaders (verificado con spirv-dis: Offset 0/4/8/12/16/20/24/28/32/96,
    // ArrayStride 160). Un desajuste de layout entre CPU y GPU no da error de
    // compilación en ningún lado, sólo lecturas desplazadas y basura en pantalla
    // — así que se comprueba aquí, donde sí puede fallar el build.
    static_assert(offsetof(GpuBoneInfo, parentIndex)     == 24, "layout std430 de BoneInfo roto");
    static_assert(offsetof(GpuBoneInfo, inverseBindPose) == 32, "layout std430 de BoneInfo roto");
    static_assert(offsetof(GpuBoneInfo, bindLocal)       == 96, "layout std430 de BoneInfo roto");
    static_assert(sizeof(GpuBoneInfo)                    == 160, "layout std430 de BoneInfo roto");
}
