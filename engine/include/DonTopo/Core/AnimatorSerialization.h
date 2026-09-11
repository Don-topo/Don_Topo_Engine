#pragma once
#include <nlohmann/json_fwd.hpp>

namespace DonTopo
{
    class AnimatorComponent;

    // El bloque "animator" del .scene. La definición vive en Scene.cpp, porque el
    // formato es de la escena. Se declara aquí para que el undo del editor
    // compare grafos con la MISMA vara con la que se guardan.
    nlohmann::json animatorToJson(const AnimatorComponent& a);

    // animatorToJson sin la posición de los nodos ("pos" de cada estado): lo
    // que el undo del AnimatorPanel considera una edición. Mover un nodo no
    // lo es (ver docs/superpowers/specs/2026-09-11-animator-graph-undo-design.md).
    // Cualquier campo que se añada al .scene entra aquí sin tocar nada más.
    nlohmann::json animatorGraphKey(const AnimatorComponent& a);
}
