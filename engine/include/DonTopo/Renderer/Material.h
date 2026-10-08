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
        // hashEmbeddedTexture (SharedTextureCache.h) of each embedded block,
        // computed ONCE by whoever fills the bytes (ModelLoader). The texture
        // cache key of an embedded texture is built from it, so registering one
        // more instance of a mesh does not hash megabytes again. 0 = not
        // computed: the key hashes the bytes itself. A copied Material copies
        // bytes and hashes together; nothing rewrites the bytes after the loader
        // (material_texture_tests watches it, and Debug asserts every hash used).
        uint64_t              embeddedTextureHash           = 0;
        uint64_t              embeddedNormalMapHash         = 0;
        uint64_t              embeddedMetallicRoughnessHash = 0;
    };
}
