#include "DonTopo/Renderer/Sphere.h"
#include <cmath>

namespace DonTopo
{
    Mesh Sphere::create(float radius, uint32_t segments, uint32_t rings, glm::vec3 color)
    {
        Mesh mesh;
        mesh.name = "sphere";

        const float PI = 3.14159265358979323846f;

        for (uint32_t r = 0; r <= rings; ++r)
        {
            float theta = static_cast<float>(r) * PI / static_cast<float>(rings);
            float sinTheta = std::sin(theta);
            float cosTheta = std::cos(theta);

            for (uint32_t c = 0; c <= segments; ++c)
            {
                float phi = static_cast<float>(c) * 2.0f * PI / static_cast<float>(segments);
                float sinPhi = std::sin(phi);
                float cosPhi = std::cos(phi);

                glm::vec3 dir{ sinTheta * cosPhi, cosTheta, sinTheta * sinPhi };

                Vertex v{};
                v.pos     = radius * dir;
                v.normal  = dir;
                v.color   = color;
                v.uv      = { static_cast<float>(c) / static_cast<float>(segments),
                               static_cast<float>(r) / static_cast<float>(rings) };
                v.tangent = { -sinPhi, 0.0f, cosPhi };

                mesh.vertices.push_back(v);
            }
        }

        const uint32_t cols = segments + 1;
        for (uint32_t r = 0; r < rings; ++r)
        {
            for (uint32_t c = 0; c < segments; ++c)
            {
                uint32_t i0 = r * cols + c;
                uint32_t i1 = r * cols + c + 1;
                uint32_t i2 = (r + 1) * cols + c + 1;
                uint32_t i3 = (r + 1) * cols + c;

                mesh.indices.insert(mesh.indices.end(), { i0, i1, i2, i0, i2, i3 });
            }
        }

        return mesh;
    }
}
