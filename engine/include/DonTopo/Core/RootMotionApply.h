#pragma once
#include <glm/glm.hpp>

namespace DonTopo
{
    class GameObject;

    // Aplica un delta de root motion (espacio de modelo del objeto) en mundo,
    // solo en horizontal. Con Rigidbody dinámico, como velocidad en X y Z
    // (conserva la Y: gravedad y saltos), para que la física colisione. Sin
    // Rigidbody o con uno kinematic, sobre el transform: Scene::update ya
    // empuja la pose de un kinematic a PhysX con setKinematicTarget.
    void applyRootMotion(GameObject& go, const glm::vec3& deltaModel, float dt);
}
