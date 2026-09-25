#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace DonTopo {

class EditorRenderer;
class GameObject;

struct ModelReimportResult {
    int                      reimported = 0;   // objetos con la malla ya sustituida
    int                      skipped    = 0;   // no se tocaron (carga en vuelo, o el FBX no carga)
    std::vector<std::string> warnings;         // ya formateados para el Log Console
};

// Vuelve a importar `fbx` (con los ajustes que su sidecar tenga ahora: el
// ModelLoader los lee solo) y sustituye la malla de CADA objeto de la escena que
// venga de ese fichero -o que lo use como fuente de animacion externa-,
// conservando transform, hijos, colliders, overrides de material y Animator.
//
// Una carga por sourcePath distinto: compartida entre los estaticos, una copia por
// objeto en los skinned (a esa copia se le reaplica la config de fuentes de
// animacion de la malla vieja y se hace rebindClips del Animator).
//
// Si el FBX ya no carga, los objetos se quedan INTACTOS con su malla anterior y
// hay un aviso: nunca queda un objeto sin malla por un reimport fallido.
//
// `renderer` puede ser nullptr (tests headless): entonces solo se cambia la parte
// de CPU. Con renderer, cada objeto pasa por removeMeshComponent -> setMesh ->
// addStaticMesh/addSkinnedMesh (la receta de MeshComponentCommand), con UN solo
// flushUploadsAndWait al final: rebuildStaticMesh NO soporta cambiar vertices.
ModelReimportResult reimportModelUsers(GameObject* sceneRoot,
                                       const std::filesystem::path& fbx,
                                       EditorRenderer* renderer);

} // namespace DonTopo
