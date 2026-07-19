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
    // Un forcedName ya en uso (por un clip existente o por otro forcedName
    // anterior de esta misma llamada) NO se duplica: cae en uniqueClipName y se
    // añade un warning. Duplicar el nombre dejaría uno de los dos clips
    // inalcanzable, porque el Animator resuelve por nombre.
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

    // Aplica savedNames POSICIONALMENTE, de una sola vez, a los primeros
    // min(savedNames.size(), source.clipNames.size()) clips de source —
    // pensado para restaurar los renames guardados de una escena sobre la
    // fuente builtin recién reconstruida por loadSkinned.
    //
    // A diferencia de encadenar renameClip clip a clip, esto resuelve
    // CUALQUIER permutación correctamente: un swap de dos nombres con
    // renameClip secuencial colisiona consigo mismo (el segundo rename choca
    // con el nombre que el primero acaba de dejar libre, en el hueco
    // equivocado) y no aplica nada — los clips se quedan con el nombre que
    // trae el FBX y un Animator que referencia el nombre guardado bindea al
    // clip EQUIVOCADO en silencio, que es peor que un huérfano.
    //
    // Guarda el invariante de nombres únicos: si savedNames trae duplicados
    // entre sí, o un nombre guardado ya pertenece a un clip que NO es parte
    // de este mismo lote (otra fuente, u otro clip fuera de los n primeros),
    // ESE índice no se aplica —se deja el clip con el nombre que ya tenía— y
    // se avisa nombrando el clip. Un nombre guardado igual al que el clip ya
    // tiene es un no-op válido, no una colisión.
    void applyClipNamesPositionally(SkinnedMesh& mesh, AnimationSource& source,
                                    const std::vector<std::string>& savedNames,
                                    std::vector<std::string>& warnings);
}
