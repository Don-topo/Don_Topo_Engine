#pragma once
#include <glm/glm.hpp>

namespace DonTopo
{
    struct Vertex 
    {
        glm::vec3 pos;
        glm::vec3 color;
        glm::vec2 uv;
        glm::vec3 normal;
        glm::vec3 tangent;
    };
};