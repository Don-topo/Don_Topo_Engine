#pragma once
#include "DonTopo/Mesh.h"
#include <glm/glm.hpp>

namespace DonTopo
{
    class Cube
    {
        public:
            static Mesh create(float size = 1.0f, glm::vec3 color = {0.8f, 0.8f, 0.8f});
    };
}
