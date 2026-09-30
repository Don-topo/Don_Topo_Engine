#pragma once
#include <glm/glm.hpp>
#include "DonTopo/Physics/Colliders/Collider.h"

namespace DonTopo {

// Unique physics component of box type. Backed by a physx::PxRigidStatic
// (without Rigidbody) or a physx::PxRigidDynamic (with Rigidbody). The collider OWNS
// its actor (it releases it in the dtor); PhysicsManager rebuilds the actor when
// going from static to dynamic or vice versa (attach/detachRigidbody). The
// gravity/kinematic policy NO LONGER lives here: it lives in the Rigidbody.
class BoxCollider : public Collider {
public:
    // actor: physx::PxRigidStatic* or PxRigidDynamic* already created and added to the
    // scene by PhysicsManager. shape: physx::PxShape* of box geometry
    // attached to that actor, with localPose already set from center.
    BoxCollider(void* actor, void* shape, const glm::vec3& halfExtents,
                const glm::vec3& center);
    ~BoxCollider();

    BoxCollider(const BoxCollider&)            = delete;
    BoxCollider& operator=(const BoxCollider&) = delete;

    // Local offset of the shape inside the actor (PxShape::setLocalPose).
    void setCenter(const glm::vec3& center);
    // Half-size of the box (PxShape::setGeometry with a new PxBoxGeometry).
    void setHalfExtents(const glm::vec3& halfExtents);
    // Scale of the GameObject's Transform. PxTransform does not support scale, so
    // it is baked into the geometry (halfExtents * abs(scale), component by
    // component). m_halfExtents —what the inspector sees and what is
    // serialized— does not change. Idempotent: with the same scale it touches nothing.
    void setWorldScale(const glm::vec3& scale);

    glm::vec3 getCenter() const       { return m_center; }
    glm::vec3 getHalfExtents() const  { return m_halfExtents; }

    void* actorHandle() const override;
    void  setActorHandle(void* actor) override;

    // Reads the global pose of the actor (translation + rotation, no scale). The
    // engine reads it back into the GameObject when there is a simulated Rigidbody.
    glm::mat4 getWorldTransform() const override;

    // Pushes worldTransform to PhysX. If the actor is dynamic-kinematic it uses
    // setKinematicTarget; in any other case it falls back to setGlobalPose.
    void syncTransform(const glm::mat4& worldTransform) override;

    // Teleports the actor (setGlobalPose, not setKinematicTarget) whatever
    // the mode, and resets its velocity to zero. Valid in both
    // modes (dynamic or kinematic) — meant for one-off edits from
    // the editor Transform panel, not for the continuous per-frame push
    // (that is syncTransform).
    void teleport(const glm::mat4& worldTransform) override;

protected:
    void* triggerShape() const override;

private:
#ifdef DT_PHYSX_ENABLED
    // Pushes m_halfExtents to PhysX with m_worldScale already applied. Requires m_shape.
    void applyScaledGeometry();

    void* m_actor = nullptr; // physx::PxRigidActor* (static o dynamic)
    void* m_shape = nullptr; // physx::PxShape*
#endif
    glm::vec3 m_halfExtents;
    glm::vec3 m_center;
    // m_worldScale (the already baked scale) lives in the Collider base: Scene
    // queries it through the base pointer to know whether it must be reapplied.
};

} // namespace DonTopo
