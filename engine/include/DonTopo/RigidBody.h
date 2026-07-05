#pragma once
#include <glm/glm.hpp>

namespace DonTopo {

class RigidBody {
public:
    // actor: physx::PxRigidDynamic* ya creado y añadido a la escena por PhysicsManager.
    explicit RigidBody(void* actor);
    ~RigidBody();

    RigidBody(const RigidBody&)            = delete;
    RigidBody& operator=(const RigidBody&) = delete;

    // Lee PxRigidDynamic::getGlobalPose() (traslación + rotación, sin escala).
    glm::mat4 getWorldTransform() const;

private:
#ifdef DT_PHYSX_ENABLED
    void* m_actor = nullptr; // physx::PxRigidDynamic*
#endif
};

} // namespace DonTopo
