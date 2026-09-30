#pragma once
#include <glm/glm.hpp>

namespace DonTopo
{
    // Thickness of the selection outline, PROPORTIONAL to the object's size.
    //
    // With a fixed thickness, a large object would barely show a border and a small one
    // would be swallowed by it. The minimum of one unit keeps a tiny mesh
    // from ending up without an outline.
    //
    // Shared by both backends: it was written twice with the same
    // factor. If they drift apart nothing breaks (the outline just looks thicker
    // or thinner in one backend), and for that very reason it would go unnoticed.
    constexpr float OUTLINE_FACTOR = 0.009f;

    // localExtent = half the largest dimension of the mesh in its local space,
    // unscaled. transform = its world matrix, from which the effective scale is
    // taken: the largest of the three columns, so that a non-uniform scale
    // does not thin the outline along the short axis.
    inline float outlineThickness(float localExtent, const glm::mat4& transform)
    {
        const float escala = (glm::max)(
            glm::length(glm::vec3(transform[0])),
            (glm::max)(glm::length(glm::vec3(transform[1])),
                       glm::length(glm::vec3(transform[2]))));
        return (glm::max)(localExtent * escala, 1.0f) * OUTLINE_FACTOR;
    }
}
