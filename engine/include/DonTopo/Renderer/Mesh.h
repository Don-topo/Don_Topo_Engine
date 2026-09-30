#pragma once
#include <vector>
#include <string>
#include "DonTopo/Renderer/Vertex.h"
#include "DonTopo/Renderer/Material.h"

namespace DonTopo
{
    struct Mesh
    {
        std::string             name;
        std::vector<Vertex>     vertices;
        std::vector<uint32_t>   indices;
        Material                material;
        // Path of the source .fbx (empty for procedural meshes: Cube/Sphere/
        // Plane/Capsule created from "Basic Shapes"). The Content Browser uses it
        // to find which GameObjects reference a file when renaming/deleting.
        std::string             sourcePath;
        // Index of the mesh inside the file (scene->mMeshes[piece]). 0 = the
        // first one, which is the only one that was loaded before pieces existed.
        int                     piece = 0;

        virtual ~Mesh() = default;
    };
}
