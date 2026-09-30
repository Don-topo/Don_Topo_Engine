#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace DonTopo {

class EditorRenderer;
class GameObject;

struct ModelReimportResult {
    int                      reimported = 0;   // objects whose mesh has already been replaced
    int                      skipped    = 0;   // not touched (load in flight, or the FBX does not load)
    std::vector<std::string> warnings;         // already formatted for the Log Console
};

// Re-imports `fbx` (with whatever settings its sidecar has now: the
// ModelLoader reads them on its own) and replaces the mesh of EVERY object in the scene that
// comes from that file (or uses it as an external animation source),
// keeping transform, children, colliders, material overrides and Animator.
//
// One load per distinct sourcePath: shared among the static ones, one copy per
// object for the skinned ones (that copy gets the old mesh's animation-source
// config re-applied and the Animator's rebindClips is done).
//
// If the FBX no longer loads, the objects stay INTACT with their previous mesh and
// a warning is issued: an object is never left without a mesh by a failed reimport.
//
// `renderer` can be nullptr (headless tests): then only the CPU part is changed.
// With a renderer, each object goes through removeMeshComponent -> setMesh ->
// addStaticMesh/addSkinnedMesh (the MeshComponentCommand recipe), with a SINGLE
// flushUploadsAndWait at the end: rebuildStaticMesh does NOT support changing vertices.
ModelReimportResult reimportModelUsers(GameObject* sceneRoot,
                                       const std::filesystem::path& fbx,
                                       EditorRenderer* renderer,
                                       float scaleRatio = 1.0f);

} // namespace DonTopo
