#include "DonTopo/Physics/Colliders/BoxCollider.h"

#ifdef DT_PHYSX_ENABLED
#define GLM_ENABLE_EXPERIMENTAL
#include <PxPhysicsAPI.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

using namespace physx;
#endif

namespace DonTopo {

BoxCollider::BoxCollider(void* actor, void* shape, const glm::vec3& halfExtents,
                         const glm::vec3& center)
    : m_halfExtents(halfExtents)
    , m_center(center)
{
#ifdef DT_PHYSX_ENABLED
    m_actor = actor;
    m_shape = shape;
#else
    (void)actor;
    (void)shape;
#endif
}

BoxCollider::~BoxCollider()
{
#ifdef DT_PHYSX_ENABLED
    // release() vía base PxActor: funciona tanto para PxRigidStatic como
    // PxRigidDynamic (el tipo concreto depende de si hay Rigidbody).
    if (m_actor) static_cast<PxActor*>(m_actor)->release();
#endif
}

void* BoxCollider::actorHandle() const
{
#ifdef DT_PHYSX_ENABLED
    return m_actor;
#else
    return nullptr;
#endif
}

void BoxCollider::setActorHandle(void* actor)
{
#ifdef DT_PHYSX_ENABLED
    m_actor = actor;
#else
    (void)actor;
#endif
}

void BoxCollider::setCenter(const glm::vec3& center)
{
    m_center = center;
#ifdef DT_PHYSX_ENABLED
    if (!m_shape) return;
    PxTransform local(PxVec3(center.x, center.y, center.z));
    static_cast<PxShape*>(m_shape)->setLocalPose(local);
#endif
}

void BoxCollider::setHalfExtents(const glm::vec3& halfExtents)
{
    m_halfExtents = halfExtents;
#ifdef DT_PHYSX_ENABLED
    if (!m_shape) return;
    static_cast<PxShape*>(m_shape)->setGeometry(
        PxBoxGeometry(halfExtents.x, halfExtents.y, halfExtents.z));
#endif
}

glm::mat4 BoxCollider::getWorldTransform() const
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return glm::mat4(1.0f);

    PxTransform pose = static_cast<PxRigidActor*>(m_actor)->getGlobalPose();

    glm::mat4 translation = glm::translate(glm::mat4(1.0f),
        glm::vec3(pose.p.x, pose.p.y, pose.p.z));
    glm::quat rotation(pose.q.w, pose.q.x, pose.q.y, pose.q.z);
    glm::mat4 rotationMat = glm::mat4_cast(rotation);

    return translation * rotationMat;
#else
    return glm::mat4(1.0f);
#endif
}

void BoxCollider::syncTransform(const glm::mat4& worldTransform)
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;

    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    glm::decompose(worldTransform, scale, rotation, translation, skew, perspective);

    PxTransform pose(
        PxVec3(translation.x, translation.y, translation.z),
        PxQuat(rotation.x, rotation.y, rotation.z, rotation.w)
    );
    // setKinematicTarget solo existe en PxRigidDynamic kinematic; para static
    // (o dynamic no-kinematic) cae a setGlobalPose.
    auto* dyn = static_cast<PxRigidActor*>(m_actor)->is<PxRigidDynamic>();
    if (dyn && (dyn->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC))
        dyn->setKinematicTarget(pose);
    else
        static_cast<PxRigidActor*>(m_actor)->setGlobalPose(pose);
#else
    (void)worldTransform;
#endif
}

void BoxCollider::teleport(const glm::mat4& worldTransform)
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;

    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    glm::decompose(worldTransform, scale, rotation, translation, skew, perspective);

    PxTransform pose(
        PxVec3(translation.x, translation.y, translation.z),
        PxQuat(rotation.x, rotation.y, rotation.z, rotation.w)
    );

    auto* actor = static_cast<PxRigidActor*>(m_actor);
    actor->setGlobalPose(pose);
    // Reset de velocidad solo tiene sentido en un cuerpo dinámico real
    // (no static, no kinematic): PhysX prohíbe set{Linear,Angular}Velocity
    // sobre kinematic y PxRigidStatic ni siquiera las expone.
    if (auto* dyn = actor->is<PxRigidDynamic>())
        if (!(dyn->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC))
        {
            dyn->setLinearVelocity(PxVec3(0.0f));
            dyn->setAngularVelocity(PxVec3(0.0f));
        }
#else
    (void)worldTransform;
#endif
}

void* BoxCollider::triggerShape() const
{
#ifdef DT_PHYSX_ENABLED
    return m_shape;
#else
    return nullptr;
#endif
}

} // namespace DonTopo
