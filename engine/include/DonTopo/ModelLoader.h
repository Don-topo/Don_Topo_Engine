#pragma once
#include <string>
#include "DonTopo/Mesh.h"

namespace DonTopo
{
    class ModelLoader
    {
        public:
            static Mesh load(const std::string& path);
    };
}