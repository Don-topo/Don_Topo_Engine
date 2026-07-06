#include "DonTopo/CapsuleCollider.h"

#ifdef DT_PHYSX_ENABLED
#define GLM_ENABLE_EXPERIMENTAL
#include <PxPhysicsAPI.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

using namespace physx;

namespace {
    // PxCapsuleGeometry se orienta por defecto a lo largo del eje X local del
    // shape; esta rotación fija (90° sobre Z) mapea ese eje X a Y, dejando la
    // cápsula "de pie" en el espacio local del actor. Constante: nunca
    // cambia, solo se recompone con distintas traslaciones (center).
    PxQuat axisCorrection() { return PxQuat(PxHalfPi, PxVec3(0.0f, 0.0f, 1.0f)); }
}
#endif

namespace DonTopo {

CapsuleCollider::CapsuleCollider(void* actor, void* shape, float radius, float halfHeight,
                                 const glm::vec3& center, bool useGravity)
    : m_radius(radius)
    , m_halfHeight(halfHeight)
    , m_center(center)
    , m_useGravity(useGravity)
{
#ifdef DT_PHYSX_ENABLED
    m_actor = actor;
    m_shape = shape;
#else
    (void)actor;
    (void)shape;
#endif
}

CapsuleCollider::~CapsuleCollider()
{
#ifdef DT_PHYSX_ENABLED
    if (m_actor) static_cast<PxRigidDynamic*>(m_actor)->release();
#endif
}

void CapsuleCollider::setCenter(const glm::vec3& center)
{
    m_center = center;
#ifdef DT_PHYSX_ENABLED
    if (!m_shape) return;
    PxTransform local(PxVec3(center.x, center.y, center.z), axisCorrection());
    static_cast<PxShape*>(m_shape)->setLocalPose(local);
#endif
}

void CapsuleCollider::setRadius(float radius)
{
    m_radius = radius;
#ifdef DT_PHYSX_ENABLED
    if (!m_shape) return;
    static_cast<PxShape*>(m_shape)->setGeometry(PxCapsuleGeometry(radius, m_halfHeight));
#endif
}

void CapsuleCollider::setHalfHeight(float halfHeight)
{
    m_halfHeight = halfHeight;
#ifdef DT_PHYSX_ENABLED
    if (!m_shape) return;
    static_cast<PxShape*>(m_shape)->setGeometry(PxCapsuleGeometry(m_radius, halfHeight));
#endif
}

void CapsuleCollider::setUseGravity(bool enabled)
{
    m_useGravity = enabled;
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;
    auto* actor = static_cast<PxRigidDynamic*>(m_actor);
    actor->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !enabled);
    actor->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, !enabled);
    if (enabled)
    {
        actor->setLinearVelocity(PxVec3(0.0f));
        actor->setAngularVelocity(PxVec3(0.0f));
        actor->wakeUp();
    }
#else
    (void)enabled;
#endif
}

glm::mat4 CapsuleCollider::getWorldTransform() const
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

void CapsuleCollider::syncTransform(const glm::mat4& worldTransform)
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
    static_cast<PxRigidDynamic*>(m_actor)->setKinematicTarget(pose);
#else
    (void)worldTransform;
#endif
}

void CapsuleCollider::teleport(const glm::mat4& worldTransform)
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

    auto* actor = static_cast<PxRigidDynamic*>(m_actor);
    actor->setGlobalPose(pose);
    // PhysX prohíbe set{Linear,Angular}Velocity sobre un actor kinematic
    // (useGravity=false) — solo tiene sentido resetear velocidad cuando es
    // un cuerpo dinámico real.
    if (m_useGravity)
    {
        actor->setLinearVelocity(PxVec3(0.0f));
        actor->setAngularVelocity(PxVec3(0.0f));
    }
#else
    (void)worldTransform;
#endif
}

} // namespace DonTopo
