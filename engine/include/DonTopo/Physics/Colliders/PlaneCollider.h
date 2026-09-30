#pragma once
#include <glm/glm.hpp>
#include "DonTopo/Physics/Colliders/Collider.h"

namespace DonTopo {

// Unique physics component of infinite plane type. Unlike
// Box/Sphere/Capsule, it never carries a Rigidbody (a "falling" plane makes no
// physical sense): its actor is always kinematic. The engine pushes the
// GameObject pose to PhysX (teleport) and never reads it back.
class PlaneCollider : public Collider {
public:
    // actor/shape already created by PhysicsManager, with localPose already set from
    // center + the fixed rotation that maps the default normal to
    // +Y (same axis trick as CapsuleCollider).
    PlaneCollider(void* actor, void* shape, const glm::vec3& center);
    ~PlaneCollider();

    PlaneCollider(const PlaneCollider&)            = delete;
    PlaneCollider& operator=(const PlaneCollider&) = delete;

    // Local offset of the shape inside the actor. It always reapplies the
    // fixed axis-correction rotation together with the translation.
    void setCenter(const glm::vec3& center);

    glm::vec3 getCenter() const { return m_center; }

    void* actorHandle() const override;
    void  setActorHandle(void* actor) override;

    glm::mat4 getWorldTransform() const override;
    void syncTransform(const glm::mat4& worldTransform) override;
    void teleport(const glm::mat4& worldTransform) override;

protected:
    // Note: PhysX may reject eTRIGGER_SHAPE on infinite plane geometry
    // (triggers are usually limited to box/sphere/capsule/convex).
    // It is exposed anyway for uniformity; marking a PlaneCollider as a trigger is
    // an edge case to validate if it is used.
    void* triggerShape() const override;

private:
#ifdef DT_PHYSX_ENABLED
    void* m_actor = nullptr; // physx::PxRigidDynamic*
    void* m_shape = nullptr; // physx::PxShape*
#endif
    glm::vec3 m_center;
};

} // namespace DonTopo
