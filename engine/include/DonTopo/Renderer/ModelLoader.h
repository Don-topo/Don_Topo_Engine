#pragma once
#include "DonTopo/Mesh.h"
#include "DonTopo/SkinnedMesh.h"
#include <string>

namespace DonTopo
{
    class ModelLoader
    {
        public:
            static Mesh load(const std::string& path);
            static SkinnedMesh loadSkinned(const std::string& path);
    };
}