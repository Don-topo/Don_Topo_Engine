#pragma once
#include <string>
#include <vector>
#include "DonTopo/Renderer/SkinnedMesh.h"

namespace DonTopo
{
    // Capa de merge de clips: sin Vulkan a propósito, igual que
    // SkinnedMeshPacking. Dentro del Renderer solo se podría probar con un
    // VkDevice vivo, es decir, no se podría probar.

    // Devuelve base si ningún clip de existing lo usa; si no, base + " (N)" con
    // el primer N libre. base vacío -> "Animation": el Animator resuelve los
    // clips por nombre, así que un nombre vacío o repetido deja clips
    // inalcanzables.
    std::string uniqueClipName(const std::vector<AnimationClip>& existing,
                               const std::string& base);
}
