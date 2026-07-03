#pragma once
#include <vector>
#include <string>
#include "DonTopo/Vertex.h"

namespace DonTopo
{
    struct Mesh
    {
        std::string             name;
        std::vector<Vertex>     vertices;
        std::vector<uint32_t>   indices;
        std::string             texturePath;
        std::vector<uint8_t>    embeddedTexture;
        std::string             normalMapPath;
        std::vector<uint8_t>    embeddedNormalMap;
        std::string             metallicRoughnessPath;
        std::vector<uint8_t>    embeddedMetallicRoughness;
        float                   metallic  = 0.0f;
        float                   roughness = 0.5f;
    };
}