#pragma once
#include <vector>
#include "DonTopo/Renderer/SkinnedMesh.h"

namespace DonTopo
{
    // Keyframes de TODOS los clips concatenados en los mismos vectores, listos
    // pa subir a los 3 SSBOs de una sola vez al construir el objeto. Cambiar de
    // clip en runtime no vuelve a tocar VRAM: solo cambia el clipBase del push
    // constant.
    //
    // boneInfos va en layout [clip][hueso]: la entrada del hueso b en el clip c
    // está en boneInfos[c * boneCount + b], y c * boneCount es exactamente el
    // clipBase que consume bone_eval.comp.
    //
    // parentIndex e inverseBindPose son del ESQUELETO, no del clip, así que se
    // replican idénticos en cada bloque. Eso cuesta 96 B por hueso y clip (2,4 %
    // sobre los keyframes de un personaje típico) y a cambio deja el bloque del
    // clip 0 sirviendo de jerarquía válida pa cualquier clip — por eso
    // bone_hierarchy.comp no necesita saber nada de clips.
    struct PackedClips
    {
        std::vector<GpuPosKey>   pos;
        std::vector<GpuRotKey>   rot;
        std::vector<GpuPosKey>   scale;
        std::vector<GpuBoneInfo> boneInfos;
    };

    // Función libre y pura (sin Vulkan) a propósito: dentro de
    // Renderer::addSkinnedMesh este empaquetado solo se podría probar con un
    // VkDevice vivo, es decir, no se podría probar.
    PackedClips packSkinnedClips(const SkinnedMesh& mesh);

    // Bloques de clip que lleva el SSBO de BoneInfos. Nunca 0: sin animaciones
    // se empaqueta igual un bloque, así que el clip 0 siempre es válido.
    inline uint32_t skinnedClipCount(const SkinnedMesh& mesh)
    {
        return mesh.animationClips.empty() ? 1u : (uint32_t)mesh.animationClips.size();
    }

    // Índice de clip listo para multiplicar por boneCount. Fuera de rango cae al
    // clip 0: la lista de clips puede haber encogido, o llegar un -1 de un
    // estado sin resolver (0xFFFFFFFF tras el cast), y clip * boneCount
    // apuntaría fuera del SSBO con el compute leyendo basura en silencio.
    //
    // Vive aquí y no en cada backend porque la guarda existía en Vulkan y
    // faltaba en D3D12: una sola función impide que vuelvan a separarse.
    inline uint32_t clampClipIndex(uint32_t clip, uint32_t clipCount)
    {
        return clip < clipCount ? clip : 0u;
    }
}
