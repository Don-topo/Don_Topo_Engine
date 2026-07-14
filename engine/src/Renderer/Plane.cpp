#include "DonTopo/Plane.h"

namespace DonTopo
{
    Mesh Plane::create(float size, float y, glm::vec3 color, float uvScale)
    {
        Mesh mesh;
        mesh.name = "plane";

        const float s = size * 0.5f;

        Vertex v0{}, v1{}, v2{}, v3{};
        for (Vertex* v : { &v0, &v1, &v2, &v3 })
        {
            v->color   = color;
            v->normal  = {0.0f, 1.0f, 0.0f};
            v->tangent = {1.0f, 0.0f, 0.0f};
        }
        v0.pos = {-s, y, -s}; v0.uv = {0, 0};
        v1.pos = { s, y, -s}; v1.uv = {uvScale, 0};
        v2.pos = { s, y,  s}; v2.uv = {uvScale, uvScale};
        v3.pos = {-s, y,  s}; v3.uv = {0, uvScale};

        mesh.vertices = {v0, v1, v2, v3};
        mesh.indices  = {0, 2, 1, 0, 3, 2};

        return mesh;
    }
}
