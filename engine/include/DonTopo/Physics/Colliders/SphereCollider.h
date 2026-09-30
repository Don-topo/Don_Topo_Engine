#pragma once
#include <glm/glm.hpp>
#include "DonTopo/Physics/Colliders/Collider.h"

namespace DonTopo {

// Unique physics component of sphere type. Same pattern as BoxCollider:
// backed by a physx::PxRigidStatic (without Rigidbody) or PxRigidDynamic (with
// Rigidbody). The collider owns its actor; the gravity/kinematic policy
// lives in the Rigidbody, not here.
class SphereCollider : public Collider {
public:
    // actor: physx::PxRigidStatic* or PxRigidDynamic* already created and added to the
    // scene by PhysicsManager. shape: physx::PxShape* of sphere geometry
    // attached to that actor, with localPose already set from center.
    SphereCollider(void* actor, void* shape, float radius,
                   const glm::vec3& center);
    ~SphereCollider();

    SphereCollider(const SphereCollider&)            = delete;
    SphereCollider& operator=(const SphereCollider&) = delete;

    // Local offset of the shape inside the actor (PxShape::setLocalPose).
    void setCenter(const glm::vec3& center);
    // Radius of the sphere (PxShape::setGeometry with a new PxSphereGeometry).
    void setRadius(float radius);
    // Scale of the GameObject's Transform. PxTransform does not support scale, so
    // it is baked into the geometry: a sphere cannot be an ellipsoid, the largest
    // of the three axes is used (radius * max(abs(x), abs(y), abs(z))), just
    // as Unity does. m_radius —what the inspector sees and what is serialized—
    // does not change. Idempotent: with the same scale it touches nothing.
    void setWorldScale(const glm::vec3& scale);

    glm::vec3 getCenter() const      { return m_center; }
    float     getRadius() const      { return m_radius; }

    void* actorHandle() const override;
    void  setActorHandle(void* actor) override;

    // Reads the global pose of the actor (translation + rotation, no scale).
    glm::mat4 getWorldTransform() const override;

    // Pushes worldTransform to PhysX (setKinematicTarget if dynamic-kinematic,
    // otherwise setGlobalPose).
    void syncTransform(const glm::mat4& worldTransform) override;

    // Teleports the actor (setGlobalPose, not setKinematicTarget) whatever
    // the mode, and resets its velocity to zero. Meant for
    // one-off edits from the editor Transform panel.
    void teleport(const glm::mat4& worldTransform) override;

protected:
    void* triggerShape() const override;

private:
#ifdef DT_PHYSX_ENABLED
    // Pushes m_radius to PhysX with m_worldScale already applied. Requires m_shape.
    void applyScaledGeometry();

    void* m_actor = nullptr; // physx::PxRigidActor* (static o dynamic)
    void* m_shape = nullptr; // physx::PxShape*
#endif
    float     m_radius;
    glm::vec3 m_center;
    // m_worldScale lives in the Collider base; see note in BoxCollider.h.
};

} // namespace DonTopo
