#pragma once

#include <glm/glm.hpp>
#include <vector>

namespace DonTopo
{
    // 2D blend by triangulation (row 14a of the animation audit). Pure: no
    // Animator or clips, only points. Used by the Animator (weights) and the
    // panel (it draws the triangulation).
    struct Blend2DTriangle { int a, b, c; };          // indices into pts, CCW
    struct Blend2DWeight   { int point; float weight; };

    // Delaunay of pts. Repeated points (within 1e-5 of a PREVIOUS one)
    // are not included. With concyclic points the first triangulation in
    // lexicographic order (i, j, k) is kept: never two overlapping triangles.
    std::vector<Blend2DTriangle> triangulate2D(const std::vector<glm::vec2>& pts);

    // Weights of the value p: inside, barycentric of the triangle that contains it;
    // outside, the closest point of the triangulation; without triangles (all
    // collinear or fewer than 3), the closest consecutive segment along
    // the line. Up to 3, all > 0, summing to 1. Returns how many (0 with no points).
    int blend2DWeights(const std::vector<glm::vec2>& pts, glm::vec2 p, Blend2DWeight out[3]);
}
