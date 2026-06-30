#pragma once
#include <glm/glm.hpp>

namespace DonTopo 
{
    constexpr int MAX_LIGHTS = 4;

    struct Light
    {
        glm::vec4 position {0.0f, 0.0f, 0.0f, 0.0f};    // w unused
        glm::vec4 color { 1.0f, 1.0f, 1.0f, 1.0f};      // rgb = color, a = intensity
    };

    struct UniformBufferObject
    {
        glm::mat4   view;
        glm::mat4   proj;
        Light       lights[MAX_LIGHTS];
        glm::vec4   viewPos;
        int         numLights = 0;
        float       _pad[3]{};              // std140: alinear a 16 bytes tras el int
    };

    /*
        (Light ocupa 2×vec4=32 bytes, ya alineado a 16. 
        El int numLights tras el array necesita padding de 12 
        bytes para que el siguiente miembro—si lo hubiera—respete std140; 
        aquí es el último campo así que el padding solo asegura sizeof(UBO) 
        múltiplo de 16, lo cual ya cumple mat4+mat4+4*32+16+4 = ...)
    */
}