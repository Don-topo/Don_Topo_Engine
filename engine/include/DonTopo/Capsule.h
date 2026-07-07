// engine/include/DonTopo/Capsule.h
#pragma once
#include "DonTopo/Mesh.h"
#include <glm/glm.hpp>
#include <cstdint>

namespace DonTopo
{
    class Capsule
    {
        public:
            // height = longitud del cilindro central (sin contar las semiesferas);
            // altura total del mesh = height + 2*radius. Mismo eje (Y) y misma
            // semántica de radio que CapsuleCollider.
            static Mesh create(float radius = 0.5f, float height = 1.0f,
                                uint32_t segments = 32, uint32_t capRings = 8,
                                glm::vec3 color = {0.8f, 0.8f, 0.8f});
    };
}
