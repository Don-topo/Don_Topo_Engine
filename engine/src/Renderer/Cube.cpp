#include "DonTopo/Cube.h"
#include <array>

namespace DonTopo
{
    Mesh Cube::create(float size, glm::vec3 color)
    {
        Mesh mesh;
        mesh.name = "cube";

        const float h = size * 0.5f;

        struct FaceDef {
            glm::vec3 normal;
            glm::vec3 tangent;
            glm::vec3 v0, v1, v2, v3; // CCW seen from outside
        };

        const std::array<FaceDef, 6> faces = { {
            // +X
            { { 1, 0, 0}, {0, 1, 0}, { h,-h,-h}, { h, h,-h}, { h, h, h}, { h,-h, h} },
            // -X
            { {-1, 0, 0}, {0, 1, 0}, {-h,-h, h}, {-h, h, h}, {-h, h,-h}, {-h,-h,-h} },
            // +Y
            { { 0, 1, 0}, {0, 0, 1}, {-h, h,-h}, {-h, h, h}, { h, h, h}, { h, h,-h} },
            // -Y
            { { 0,-1, 0}, {0, 0,-1}, {-h,-h, h}, {-h,-h,-h}, { h,-h,-h}, { h,-h, h} },
            // +Z
            { { 0, 0, 1}, {1, 0, 0}, {-h,-h, h}, { h,-h, h}, { h, h, h}, {-h, h, h} },
            // -Z
            { { 0, 0,-1}, {-1, 0, 0}, { h,-h,-h}, {-h,-h,-h}, {-h, h,-h}, { h, h,-h} },
        } };

        for (const auto& face : faces)
        {
            uint32_t base = static_cast<uint32_t>(mesh.vertices.size());

            Vertex v0{}, v1{}, v2{}, v3{};
            v0.pos = face.v0; v1.pos = face.v1; v2.pos = face.v2; v3.pos = face.v3;
            v0.uv = {0, 1}; v1.uv = {1, 1}; v2.uv = {1, 0}; v3.uv = {0, 0};

            for (Vertex* v : { &v0, &v1, &v2, &v3 })
            {
                v->color   = color;
                v->normal  = face.normal;
                v->tangent = face.tangent;
            }

            mesh.vertices.insert(mesh.vertices.end(), { v0, v1, v2, v3 });
            mesh.indices.insert(mesh.indices.end(), {
                base + 0, base + 1, base + 2,
                base + 0, base + 2, base + 3
            });
        }

        return mesh;
    }
}
