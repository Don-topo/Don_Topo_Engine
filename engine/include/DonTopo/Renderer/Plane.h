#pragma once
#include "DonTopo/Renderer/Mesh.h"
#include <glm/glm.hpp>

namespace DonTopo
{
    class Plane
    {
        public:
            static Mesh create(float size = 1000.0f, float y = 0.0f, glm::vec3 color = {0.6f, 0.6f, 0.6f}, float uvScale = 10.0f);
    };
}
