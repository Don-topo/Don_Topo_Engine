#include "DonTopo/Physics/Colliders/BoxCollider.h"

#ifdef DT_PHYSX_ENABLED
#define GLM_ENABLE_EXPERIMENTAL
#include <PxPhysicsAPI.h>
#include "../PxPose.h"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <cmath>

using namespace physx;

namespace {
    // Positive minimum of the half-size: PhysX rejects a degenerate PxBoxGeometry,
    // and with scale 0 on an axis the product would give exactly that.
    constexpr float kMinHalfExtent = 1e-4f;

    // abs(): a negative scale is a mirror, it does not thin the box.
    float scaledExtent(float halfExtent, float scale)
    {
        const float v = halfExtent * std::fabs(scale);
        return v > kMinHalfExtent ? v : kMinHalfExtent;
    }

    // Tolerance, not equality: glm::decompose returns 1±1e-7 on matrices with
    // rotation, and that noise must not rewrite the geometry of an unscaled scene
    // (with scale 1 setGeometry is never called).
    bool sameScale(const glm::vec3& a, const glm::vec3& b)
    {
        return std::fabs(a.x - b.x) < 1e-6f
            && std::fabs(a.y - b.y) < 1e-6f
            && std::fabs(a.z - b.z) < 1e-6f;
    }
}
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
    // release() through the PxActor base: works for both PxRigidStatic and
    // PxRigidDynamic (the concrete type depends on whether there is a Rigidbody).
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
    applyScaledGeometry();
#endif
}

void BoxCollider::setWorldScale(const glm::vec3& scale)
{
#ifdef DT_PHYSX_ENABLED
    if (sameScale(scale, m_worldScale)) return;
    m_worldScale = scale;
    if (!m_shape) return;
    applyScaledGeometry();
#else
    m_worldScale = scale;
#endif
}

#ifdef DT_PHYSX_ENABLED
void BoxCollider::applyScaledGeometry()
{
    static_cast<PxShape*>(m_shape)->setGeometry(PxBoxGeometry(
        scaledExtent(m_halfExtents.x, m_worldScale.x),
        scaledExtent(m_halfExtents.y, m_worldScale.y),
        scaledExtent(m_halfExtents.z, m_worldScale.z)));
}
#endif

glm::mat4 BoxCollider::getWorldTransform() const
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return glm::mat4(1.0f);

    PxTransform pose = static_cast<PxRigidActor*>(m_actor)->getGlobalPose();

    glm::mat4 translation = glm::translate(glm::mat4(1.0f),
        glm::vec3(pose.p.x, pose.p.y, pose.p.z));
    glm::quat rotation(pose.q.w, pose.q.x, pose.q.y, pose.q.z);
    glm::mat4 rotationMat = glm::mat4_cast(rotation);

    // With interpolation off (default) it returns the raw actor pose.
    return blendWithPreviousPose(translation * rotationMat);
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
    const PxTransform pose = poseFromWorld(worldTransform, &scale);
    // The scale does not fit in the PxTransform: it is baked into the geometry. No-op if
    // it did not change since last time (the normal case, scale 1).
    setWorldScale(scale);
    // setKinematicTarget only exists on a kinematic PxRigidDynamic; for static
    // (or non-kinematic dynamic) it falls back to setGlobalPose.
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
    const PxTransform pose = poseFromWorld(worldTransform, &scale);

    setWorldScale(scale); // see note in syncTransform

    auto* actor = static_cast<PxRigidActor*>(m_actor);
    actor->setGlobalPose(pose);
    // Velocity reset only makes sense on a real dynamic body
    // (not static, not kinematic): PhysX forbids set{Linear,Angular}Velocity
    // on kinematic and PxRigidStatic does not even expose them.
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
