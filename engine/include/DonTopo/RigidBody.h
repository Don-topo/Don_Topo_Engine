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

    // Teletransporta el actor a la pose dada (traslación + rotación; la
    // escala de worldTransform se ignora) y resetea su velocidad lineal y
    // angular a cero, para que la simulación siga con normalidad desde la
    // nueva pose (p.ej. tras un edit desde el editor). autowake=true por
    // defecto en PhysX saca al actor del sleep si estaba dormido.
    void setWorldTransform(const glm::mat4& worldTransform);

private:
#ifdef DT_PHYSX_ENABLED
    void* m_actor = nullptr; // physx::PxRigidDynamic*
#endif
};

} // namespace DonTopo
