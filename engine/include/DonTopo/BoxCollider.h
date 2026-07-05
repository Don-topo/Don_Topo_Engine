#pragma once
#include <glm/glm.hpp>

namespace DonTopo {

class BoxCollider {
public:
    // actor: physx::PxRigidStatic* ya creado y añadido a la escena por PhysicsManager.
    BoxCollider(void* actor, const glm::vec3& halfExtents);
    ~BoxCollider();

    BoxCollider(const BoxCollider&)            = delete;
    BoxCollider& operator=(const BoxCollider&) = delete;

    glm::vec3 getHalfExtents() const { return m_halfExtents; }
    void syncTransform(const glm::mat4& worldTransform);

private:
#ifdef DT_PHYSX_ENABLED
    void* m_actor = nullptr; // physx::PxRigidStatic*
#endif
    glm::vec3 m_halfExtents;
};

} // namespace DonTopo
