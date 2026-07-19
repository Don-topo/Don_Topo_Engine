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

    // Importa las animaciones de path y las añade a mesh.animationClips,
    // registrando la fuente en mesh.animationSources.
    //
    // Los clips se nombran por el basename del fichero (walk.fbx -> "walk",
    // "walk (1)"...): el nombre interno de un FBX de Mixamo es "mixamo.com"
    // para todos, y con eso la lista de clips no se puede leer.
    //
    // forcedNames, si no es nullptr, pisa esos nombres en orden hasta agotarse.
    // Lo usan la carga de escena y el undo de un remove: sin él, un clip
    // renombrado volvería con el nombre del fichero y los estados del grafo que
    // lo referencian quedarían huérfanos.
    //
    // Devuelve false y deja mesh INTACTO si el fichero no aporta nada (ilegible,
    // sin animaciones, o ningún hueso en común con mesh.skeleton).
    bool addAnimationSource(SkinnedMesh& mesh, const std::string& path,
                            std::vector<std::string>& warnings,
                            const std::vector<std::string>* forcedNames = nullptr);

    // Quita la fuente y los clips que aportó. false si el índice está fuera de
    // rango o apunta a la fuente builtin (esa es el modelo, no se puede quitar).
    //
    // Los índices de los clips supervivientes se recolocan; no hace falta
    // arreglar nada en el grafo porque los estados referencian por nombre y
    // AnimatorComponent::bindClips los vuelve a resolver.
    bool removeAnimationSource(SkinnedMesh& mesh, size_t sourceIndex);

    // Renombra un clip y actualiza el clipNames de su fuente. false si oldName
    // no existe, newName está vacío o newName ya está en uso.
    //
    // NO toca los estados del Animator: eso lo hace
    // AnimatorComponent::renameClipReferences, que vive en Core (este módulo es
    // Renderer y no debe depender de Core).
    bool renameClip(SkinnedMesh& mesh, const std::string& oldName,
                    const std::string& newName);
}
