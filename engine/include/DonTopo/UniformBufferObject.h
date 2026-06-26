#pragma once
#include <glm/glm.hpp>

namespace DonTopo 
{
    struct UniformBufferObject
    {
        glm::mat4 view;
        glm::mat4 proj;
    };
}