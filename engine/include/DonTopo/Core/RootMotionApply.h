#pragma once
#include <glm/glm.hpp>

namespace DonTopo
{
    class GameObject;

    // Applies a root motion delta (object model space) in world space,
    // horizontal only. With a dynamic Rigidbody, as velocity in X and Z
    // (it keeps Y: gravity and jumps), so that physics collides. Without a
    // Rigidbody or with a kinematic one, on the transform: Scene::update already
    // pushes the pose of a kinematic to PhysX with setKinematicTarget.
    void applyRootMotion(GameObject& go, const glm::vec3& deltaModel, float dt);
}
