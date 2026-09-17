#pragma once
#include <glm/glm.hpp>

namespace DonTopo
{
    struct SkinnedMesh;
    class AnimatorComponent;

    // Root motion en CPU, sin GPU: posición y desplazamiento de la raíz de un
    // clip a partir de sus keyframes. La raíz es el primer hueso sin padre con
    // claves de posición en ese clip; sus claves están en espacio de modelo (la
    // jerarquía de la GPU no aplica los nodos que hay por encima de la raíz).

    // Posición de la raíz en t ticks, interpolada linealmente como bone_eval y
    // acotada a la primera y la última clave. Sin raíz con claves: (0,0,0).
    glm::vec3 sampleRootPosition(const SkinnedMesh& mesh, int clipIndex, double t);

    // Desplazamiento desde 0 hasta T ticks acumulados de un reloj de duración
    // `duration`. En loop suma un P(duration) − P(0) por cada ciclo completo.
    glm::vec3 rootDisplacement(const SkinnedMesh& mesh, int clipIndex, double T, double duration, bool loop);

    // Delta horizontal (y = 0) del último update del Animator, en espacio de
    // modelo, ponderando sus rootMotionSamples.
    glm::vec3 rootMotionDelta(const SkinnedMesh& mesh, const AnimatorComponent& anim);
}
