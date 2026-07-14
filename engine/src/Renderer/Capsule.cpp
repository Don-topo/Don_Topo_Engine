// engine/src/Capsule.cpp
#include "DonTopo/Renderer/Capsule.h"
#include <cmath>
#include <vector>

namespace DonTopo
{
    Mesh Capsule::create(float radius, float height, uint32_t segments, uint32_t capRings, glm::vec3 color)
    {
        Mesh mesh;
        mesh.name = "capsule";

        const float PI = 3.14159265358979323846f;
        const float halfHeight = height * 0.5f;

        // Perfil (r, y, normal_r, normal_y) de abajo a arriba: casquete inferior
        // (capRings+1 anillos, polo incluido), cilindro (2 anillos, mismo radio),
        // casquete superior (capRings+1 anillos, polo incluido). Se revoluciona
        // este perfil alrededor del eje Y (mismo enfoque que Sphere::create pero
        // con un perfil no-circular).
        struct ProfilePoint { float r, y, nr, ny; };
        std::vector<ProfilePoint> profile;

        for (uint32_t i = 0; i <= capRings; ++i)
        {
            float theta = PI - (float)i / (float)capRings * (PI * 0.5f); // PI -> PI/2
            float s = std::sin(theta), c = std::cos(theta);
            profile.push_back({ radius * s, -halfHeight + radius * c, s, c });
        }
        profile.push_back({ radius, -halfHeight, 1.0f, 0.0f });
        profile.push_back({ radius,  halfHeight, 1.0f, 0.0f });
        for (uint32_t i = 0; i <= capRings; ++i)
        {
            float theta = PI * 0.5f - (float)i / (float)capRings * (PI * 0.5f); // PI/2 -> 0
            float s = std::sin(theta), c = std::cos(theta);
            profile.push_back({ radius * s, halfHeight + radius * c, s, c });
        }

        const uint32_t ringsTotal = (uint32_t)profile.size();
        for (uint32_t r = 0; r < ringsTotal; ++r)
        {
            const ProfilePoint& p = profile[r];
            for (uint32_t c = 0; c <= segments; ++c)
            {
                float phi = (float)c / (float)segments * 2.0f * PI;
                float sinPhi = std::sin(phi), cosPhi = std::cos(phi);

                Vertex v{};
                v.pos     = { p.r * cosPhi, p.y, p.r * sinPhi };
                v.normal  = { p.nr * cosPhi, p.ny, p.nr * sinPhi };
                v.color   = color;
                v.uv      = { (float)c / (float)segments, (float)r / (float)(ringsTotal - 1) };
                v.tangent = { -sinPhi, 0.0f, cosPhi };

                mesh.vertices.push_back(v);
            }
        }

        const uint32_t cols = segments + 1;
        for (uint32_t r = 0; r < ringsTotal - 1; ++r)
        {
            for (uint32_t c = 0; c < segments; ++c)
            {
                uint32_t i0 = r * cols + c;
                uint32_t i1 = r * cols + c + 1;
                uint32_t i2 = (r + 1) * cols + c + 1;
                uint32_t i3 = (r + 1) * cols + c;

                mesh.indices.insert(mesh.indices.end(), { i0, i3, i2, i0, i2, i1 });
            }
        }

        return mesh;
    }
}
