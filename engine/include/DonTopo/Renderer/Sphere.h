#pragma once
#include "DonTopo/Mesh.h"
#include <glm/glm.hpp>
#include <cstdint>

namespace DonTopo
{
    class Sphere
    {
        public:
            static Mesh create(float radius = 0.5f, uint32_t segments = 32, uint32_t rings = 16, glm::vec3 color = {0.8f, 0.8f, 0.8f});
    };
}
