#pragma once
#include <vector>
#include <string>
#include "DonTopo/Vertex.h"
#include "DonTopo/Material.h"

namespace DonTopo
{
    struct Mesh
    {
        std::string             name;
        std::vector<Vertex>     vertices;
        std::vector<uint32_t>   indices;
        Material                material;

        virtual ~Mesh() = default;
    };
}