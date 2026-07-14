#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace DonTopo
{
    struct Material
    {
        std::string           texturePath;
        std::vector<uint8_t>  embeddedTexture;
        std::string           normalMapPath;
        std::vector<uint8_t>  embeddedNormalMap;
        std::string           metallicRoughnessPath;
        std::vector<uint8_t>  embeddedMetallicRoughness;
        float                 metallic  = 0.0f;
        float                 roughness = 0.5f;
    };
}
