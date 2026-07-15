#pragma once
#include <glm/glm.hpp>
#include "DonTopo/Physics/Colliders/Collider.h"

namespace DonTopo {

// Componente único de física de tipo esfera. Mismo patrón que BoxCollider:
// respaldado por un physx::PxRigidStatic (sin Rigidbody) o PxRigidDynamic (con
// Rigidbody). El collider posee su actor; la política de gravedad/kinematic
// vive en el Rigidbody, no aquí.
class SphereCollider : public Collider {
public:
    // actor: physx::PxRigidStatic* o PxRigidDynamic* ya creado y añadido a la
    // escena por PhysicsManager. shape: physx::PxShape* de geometría esfera
    // adjunta a ese actor, con localPose ya puesto a partir de center.
    SphereCollider(void* actor, void* shape, float radius,
                   const glm::vec3& center);
    ~SphereCollider();

    SphereCollider(const SphereCollider&)            = delete;
    SphereCollider& operator=(const SphereCollider&) = delete;

    // Offset local de la shape dentro del actor (PxShape::setLocalPose).
    void setCenter(const glm::vec3& center);
    // Radio de la esfera (PxShape::setGeometry con nueva PxSphereGeometry).
    void setRadius(float radius);

    glm::vec3 getCenter() const      { return m_center; }
    float     getRadius() const      { return m_radius; }

    void* actorHandle() const override;
    void  setActorHandle(void* actor) override;

    // Lee la pose global del actor (traslación + rotación, sin escala).
    glm::mat4 getWorldTransform() const override;

    // Empuja worldTransform hacia PhysX (setKinematicTarget si dynamic-kinematic,
    // si no setGlobalPose).
    void syncTransform(const glm::mat4& worldTransform) override;

    // Teletransporta el actor (setGlobalPose, no setKinematicTarget) sea
    // cual sea el modo, y resetea su velocidad a cero. Pensado para
    // ediciones puntuales desde el Transform panel del editor.
    void teleport(const glm::mat4& worldTransform) override;

protected:
    void* triggerShape() const override;

private:
#ifdef DT_PHYSX_ENABLED
    void* m_actor = nullptr; // physx::PxRigidActor* (static o dynamic)
    void* m_shape = nullptr; // physx::PxShape*
#endif
    float     m_radius;
    glm::vec3 m_center;
};

} // namespace DonTopo
