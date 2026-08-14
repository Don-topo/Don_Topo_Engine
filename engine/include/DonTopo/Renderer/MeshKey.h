#pragma once
#include <string>

namespace DonTopo
{
    struct Mesh;

    // Clave de contenido: dos Mesh que produzcan la misma clave generan
    // exactamente los mismos recursos GPU. Mezcla discriminantes exactos
    // (tamaños, paths de textura, metallic/roughness) con un FNV-1a de los
    // bytes de vértices, índices y texturas embebidas. El nombre del mesh y su
    // sourcePath NO entran: no afectan a un solo byte de lo que sube a GPU.
    //
    // Vive en su propia cabecera y no en SharedGpuMesh.h porque no depende de
    // ningún backend: la usan el Renderer de Vulkan y el de DirectX 12, y
    // arrastrar vulkan.h al segundo por una función de hashing no tiene sentido.
    std::string makeSharedMeshKey(const Mesh& mesh);
}
