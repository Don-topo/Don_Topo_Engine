#pragma once
#include <glm/glm.hpp>
#include "DonTopo/Physics/Colliders/Collider.h"

namespace DonTopo {

// Unique physics component of capsule type. Same pattern as BoxCollider
// (PxRigidStatic without Rigidbody, PxRigidDynamic with Rigidbody). PhysX orients
// PxCapsuleGeometry by default along the local X axis of the shape; here a
// fixed 90° rotation about Z is composed into the localPose so that the
// "height" ends up in Y (upright capsule, character style).
class CapsuleCollider : public Collider {
public:
    // actor/shape already created by PhysicsManager, with localPose already set from
    // center + the fixed axis-correction rotation.
    CapsuleCollider(void* actor, void* shape, float radius, float halfHeight,
                     const glm::vec3& center);
    ~CapsuleCollider();

    CapsuleCollider(const CapsuleCollider&)            = delete;
    CapsuleCollider& operator=(const CapsuleCollider&) = delete;

    // Local offset of the shape inside the actor. It always reapplies the
    // fixed axis-correction rotation together with the translation.
    void setCenter(const glm::vec3& center);
    // Radius of the capsule (PxShape::setGeometry with a new PxCapsuleGeometry).
    void setRadius(float radius);
    // Half-height of the capsule (distance between the centers of the two
    // hemispheres; PxShape::setGeometry with a new PxCapsuleGeometry).
    void setHalfHeight(float halfHeight);
    // Scale of the GameObject's Transform. PxTransform does not support scale, so
    // it is baked into the geometry: the radius goes with the larger of the two
    // transverse axes (max(abs(x), abs(z)) — the capsule stands upright in Y because of the
    // correction rotation) and the half-height with abs(y). m_radius/m_halfHeight
    // —what the inspector sees and what is serialized— do not change. Idempotent:
    // with the same scale it touches nothing.
    void setWorldScale(const glm::vec3& scale);

    glm::vec3 getCenter() const      { return m_center; }
    float     getRadius() const      { return m_radius; }
    float     getHalfHeight() const  { return m_halfHeight; }

    void* actorHandle() const override;
    void  setActorHandle(void* actor) override;

    glm::mat4 getWorldTransform() const override;
    void syncTransform(const glm::mat4& worldTransform) override;
    void teleport(const glm::mat4& worldTransform) override;

protected:
    void* triggerShape() const override;

private:
#ifdef DT_PHYSX_ENABLED
    // Pushes m_radius/m_halfHeight to PhysX with m_worldScale already applied.
    // Requires m_shape.
    void applyScaledGeometry();

    void* m_actor = nullptr; // physx::PxRigidActor* (static o dynamic)
    void* m_shape = nullptr; // physx::PxShape*
#endif
    float     m_radius;
    float     m_halfHeight;
    glm::vec3 m_center;
    // m_worldScale lives in the Collider base; see note in BoxCollider.h.
};

} // namespace DonTopo
