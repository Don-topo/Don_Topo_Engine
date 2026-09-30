#include "DonTopo/Physics/Colliders/PlaneCollider.h"

#ifdef DT_PHYSX_ENABLED
#define GLM_ENABLE_EXPERIMENTAL
#include <PxPhysicsAPI.h>
#include "../PxPose.h"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

using namespace physx;

namespace {
    // Same trick as CapsuleCollider: PxPlaneGeometry defines the plane normal as
    // the local X axis of the shape. This fixed rotation (90° about Z)
    // maps that X axis to Y, leaving the default normal pointing "up"
    // in the local space of the actor.
    PxQuat axisCorrection() { return PxQuat(PxHalfPi, PxVec3(0.0f, 0.0f, 1.0f)); }
}
#endif

namespace DonTopo {

PlaneCollider::PlaneCollider(void* actor, void* shape, const glm::vec3& center)
    : m_center(center)
{
#ifdef DT_PHYSX_ENABLED
    m_actor = actor;
    m_shape = shape;
#else
    (void)actor;
    (void)shape;
#endif
}

PlaneCollider::~PlaneCollider()
{
#ifdef DT_PHYSX_ENABLED
    // release() through the PxActor base (uniform with the rest of the colliders).
    if (m_actor) static_cast<PxActor*>(m_actor)->release();
#endif
}

void* PlaneCollider::actorHandle() const
{
#ifdef DT_PHYSX_ENABLED
    return m_actor;
#else
    return nullptr;
#endif
}

void PlaneCollider::setActorHandle(void* actor)
{
#ifdef DT_PHYSX_ENABLED
    m_actor = actor;
#else
    (void)actor;
#endif
}

void PlaneCollider::setCenter(const glm::vec3& center)
{
    m_center = center;
#ifdef DT_PHYSX_ENABLED
    if (!m_shape) return;
    PxTransform local(PxVec3(center.x, center.y, center.z), axisCorrection());
    static_cast<PxShape*>(m_shape)->setLocalPose(local);
#endif
}

glm::mat4 PlaneCollider::getWorldTransform() const
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return glm::mat4(1.0f);

    PxTransform pose = static_cast<PxRigidDynamic*>(m_actor)->getGlobalPose();

    glm::mat4 translation = glm::translate(glm::mat4(1.0f),
        glm::vec3(pose.p.x, pose.p.y, pose.p.z));
    glm::quat rotation(pose.q.w, pose.q.x, pose.q.y, pose.q.z);
    glm::mat4 rotationMat = glm::mat4_cast(rotation);

    return translation * rotationMat;
#else
    return glm::mat4(1.0f);
#endif
}

void PlaneCollider::syncTransform(const glm::mat4& worldTransform)
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;

    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    const PxTransform pose = poseFromWorld(worldTransform, &scale);
    static_cast<PxRigidDynamic*>(m_actor)->setKinematicTarget(pose);
#else
    (void)worldTransform;
#endif
}

void PlaneCollider::teleport(const glm::mat4& worldTransform)
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;

    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    const PxTransform pose = poseFromWorld(worldTransform, &scale);

    // No velocity reset: PlaneCollider is always kinematic, and PhysX
    // forbids set{Linear,Angular}Velocity on a kinematic actor.
    static_cast<PxRigidDynamic*>(m_actor)->setGlobalPose(pose);
#else
    (void)worldTransform;
#endif
}

void* PlaneCollider::triggerShape() const
{
#ifdef DT_PHYSX_ENABLED
    return m_shape;
#else
    return nullptr;
#endif
}

} // namespace DonTopo
