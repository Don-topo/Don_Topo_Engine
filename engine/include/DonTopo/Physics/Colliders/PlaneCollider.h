#pragma once
#include <glm/glm.hpp>
#include "DonTopo/Physics/Colliders/Collider.h"

namespace DonTopo {

// Componente único de física de tipo plano infinito. A diferencia de
// Box/Sphere/Capsule, nunca lleva Rigidbody (un plano "cayendo" no tiene
// sentido físico): su actor siempre es kinematic. El motor empuja la pose del
// GameObject hacia PhysX (teleport), nunca lee de vuelta.
class PlaneCollider : public Collider {
public:
    // actor/shape ya creados por PhysicsManager, con localPose ya puesto a
    // partir de center + la rotación fija que mapea la normal por defecto a
    // +Y (mismo truco de eje que CapsuleCollider).
    PlaneCollider(void* actor, void* shape, const glm::vec3& center);
    ~PlaneCollider();

    PlaneCollider(const PlaneCollider&)            = delete;
    PlaneCollider& operator=(const PlaneCollider&) = delete;

    // Offset local de la shape dentro del actor. Reaplica siempre la
    // rotación fija de corrección de eje junto con la traslación.
    void setCenter(const glm::vec3& center);

    glm::vec3 getCenter() const { return m_center; }

    void* actorHandle() const override;
    void  setActorHandle(void* actor) override;

    glm::mat4 getWorldTransform() const override;
    void syncTransform(const glm::mat4& worldTransform) override;
    void teleport(const glm::mat4& worldTransform) override;

protected:
    // Nota: PhysX puede rechazar eTRIGGER_SHAPE sobre geometría de plano
    // infinito (los triggers suelen limitarse a box/sphere/capsule/convex).
    // Se expone igual por uniformidad; marcar un PlaneCollider como trigger es
    // un caso límite a validar si se usa.
    void* triggerShape() const override;

private:
#ifdef DT_PHYSX_ENABLED
    void* m_actor = nullptr; // physx::PxRigidDynamic*
    void* m_shape = nullptr; // physx::PxShape*
#endif
    glm::vec3 m_center;
};

} // namespace DonTopo
