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
}
