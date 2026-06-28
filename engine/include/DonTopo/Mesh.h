#pragma once
#include <vector>
#include <string>
#include "DonTopo/Vertex.h"

namespace DonTopo
{
    struct Mesh
    {
        std::vector<Vertex>     vertices;
        std::vector<uint32_t>   indices;
        std::string             texturePath;
    };
}