#pragma once

#include <glm/glm.hpp>
#include <vector>

namespace DonTopo
{
    // Blend 2D por triangulación (fila 14a del audit de animación). Pura: ni
    // Animator ni clips, solo puntos. La usan el Animator (pesos) y el panel
    // (dibuja la triangulación).
    struct Blend2DTriangle { int a, b, c; };          // índices a pts, en CCW
    struct Blend2DWeight   { int point; float weight; };

    // Delaunay de pts. Los puntos repetidos (a menos de 1e-5 de uno ANTERIOR)
    // no entran. Con puntos concíclicos se queda la primera triangulación en
    // orden lexicográfico (i, j, k): nunca dos triángulos que se solapen.
    std::vector<Blend2DTriangle> triangulate2D(const std::vector<glm::vec2>& pts);

    // Pesos del valor p: dentro, baricéntricas del triángulo que lo contiene;
    // fuera, el punto más cercano de la triangulación; sin triángulos (todos
    // alineados o menos de 3), el segmento consecutivo más cercano a lo largo
    // de la recta. Hasta 3, todos > 0, suman 1. Devuelve cuántos (0 sin puntos).
    int blend2DWeights(const std::vector<glm::vec2>& pts, glm::vec2 p, Blend2DWeight out[3]);
}
