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
        // Path del .fbx de origen (vacío para meshes procedurales: Cube/Sphere/
        // Plane/Capsule creados desde "Basic Shapes"). Content Browser lo usa
        // para localizar qué GameObjects referencian un fichero al hacer
        // rename/delete.
        std::string             sourcePath;

        virtual ~Mesh() = default;
    };
}
