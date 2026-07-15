#include "DonTopo/Physics/Colliders/CapsuleCollider.h"

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
                                 const glm::vec3& center)
    : m_radius(radius)
    , m_halfHeight(halfHeight)
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

CapsuleCollider::~CapsuleCollider()
{
#ifdef DT_PHYSX_ENABLED
    // release() vía base PxActor: funciona para static y dynamic.
    if (m_actor) static_cast<PxActor*>(m_actor)->release();
#endif
}

void* CapsuleCollider::actorHandle() const
{
#ifdef DT_PHYSX_ENABLED
    return m_actor;
#else
    return nullptr;
#endif
}

void CapsuleCollider::setActorHandle(void* actor)
{
#ifdef DT_PHYSX_ENABLED
    m_actor = actor;
#else
    (void)actor;
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

glm::mat4 CapsuleCollider::getWorldTransform() const
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
    auto* dyn = static_cast<PxRigidActor*>(m_actor)->is<PxRigidDynamic>();
    if (dyn && (dyn->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC))
        dyn->setKinematicTarget(pose);
    else
        static_cast<PxRigidActor*>(m_actor)->setGlobalPose(pose);
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

    auto* actor = static_cast<PxRigidActor*>(m_actor);
    actor->setGlobalPose(pose);
    // Reset de velocidad solo en cuerpo dinámico real (no static/kinematic).
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

void* CapsuleCollider::triggerShape() const
{
#ifdef DT_PHYSX_ENABLED
    return m_shape;
#else
    return nullptr;
#endif
}

} // namespace DonTopo
