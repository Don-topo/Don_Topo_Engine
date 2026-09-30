// engine/include/DonTopo/Renderer/Capsule.h
#pragma once
#include "DonTopo/Renderer/Mesh.h"
#include <glm/glm.hpp>
#include <cstdint>

namespace DonTopo
{
    class Capsule
    {
        public:
            // height = length of the central cylinder (not counting the hemispheres);
            // total mesh height = height + 2*radius. Same axis (Y) and same radius
            // semantics as CapsuleCollider.
            static Mesh create(float radius = 0.5f, float height = 1.0f,
                                uint32_t segments = 32, uint32_t capRings = 8,
                                glm::vec3 color = {0.8f, 0.8f, 0.8f});
    };
}
