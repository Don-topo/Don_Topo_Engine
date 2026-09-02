#include "DonTopo/Physics/Colliders/SphereCollider.h"

#ifdef DT_PHYSX_ENABLED
#define GLM_ENABLE_EXPERIMENTAL
#include <PxPhysicsAPI.h>
#include "../PxPose.h"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <cmath>

using namespace physx;

namespace {
    // Mínimo positivo del radio: PhysX rechaza una PxSphereGeometry degenerada,
    // y con escala 0 el producto daría exactamente eso.
    constexpr float kMinRadius = 1e-4f;

    // Una esfera no puede deformarse en elipsoide: manda el eje mayor. abs()
    // porque una escala negativa es un espejo, no encoge la esfera.
    float scaledRadius(float radius, const glm::vec3& scale)
    {
        const float s = std::max({std::fabs(scale.x), std::fabs(scale.y), std::fabs(scale.z)});
        const float v = radius * s;
        return v > kMinRadius ? v : kMinRadius;
    }

    // Tolerancia, no igualdad: glm::decompose devuelve 1±1e-7 en matrices con
    // rotación, y ese ruido no debe reescribir la geometría de una escena sin
    // escalar (con escala 1 no se llama nunca a setGeometry).
    bool sameScale(const glm::vec3& a, const glm::vec3& b)
    {
        return std::fabs(a.x - b.x) < 1e-6f
            && std::fabs(a.y - b.y) < 1e-6f
            && std::fabs(a.z - b.z) < 1e-6f;
    }
}
#endif

namespace DonTopo {

SphereCollider::SphereCollider(void* actor, void* shape, float radius,
                               const glm::vec3& center)
    : m_radius(radius)
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

SphereCollider::~SphereCollider()
{
#ifdef DT_PHYSX_ENABLED
    // release() vía base PxActor: funciona para static y dynamic.
    if (m_actor) static_cast<PxActor*>(m_actor)->release();
#endif
}

void* SphereCollider::actorHandle() const
{
#ifdef DT_PHYSX_ENABLED
    return m_actor;
#else
    return nullptr;
#endif
}

void SphereCollider::setActorHandle(void* actor)
{
#ifdef DT_PHYSX_ENABLED
    m_actor = actor;
#else
    (void)actor;
#endif
}

void SphereCollider::setCenter(const glm::vec3& center)
{
    m_center = center;
#ifdef DT_PHYSX_ENABLED
    if (!m_shape) return;
    PxTransform local(PxVec3(center.x, center.y, center.z));
    static_cast<PxShape*>(m_shape)->setLocalPose(local);
#endif
}

void SphereCollider::setRadius(float radius)
{
    m_radius = radius;
#ifdef DT_PHYSX_ENABLED
    if (!m_shape) return;
    applyScaledGeometry();
#endif
}

void SphereCollider::setWorldScale(const glm::vec3& scale)
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
void SphereCollider::applyScaledGeometry()
{
    static_cast<PxShape*>(m_shape)->setGeometry(
        PxSphereGeometry(scaledRadius(m_radius, m_worldScale)));
}
#endif

glm::mat4 SphereCollider::getWorldTransform() const
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return glm::mat4(1.0f);

    PxTransform pose = static_cast<PxRigidActor*>(m_actor)->getGlobalPose();

    glm::mat4 translation = glm::translate(glm::mat4(1.0f),
        glm::vec3(pose.p.x, pose.p.y, pose.p.z));
    glm::quat rotation(pose.q.w, pose.q.x, pose.q.y, pose.q.z);
    glm::mat4 rotationMat = glm::mat4_cast(rotation);

    // Con interpolación apagada (default) devuelve la pose cruda del actor.
    return blendWithPreviousPose(translation * rotationMat);
#else
    return glm::mat4(1.0f);
#endif
}

void SphereCollider::syncTransform(const glm::mat4& worldTransform)
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;

    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    const PxTransform pose = poseFromWorld(worldTransform, &scale);
    // La escala no cabe en la PxTransform: se hornea en la geometría. No-op si
    // no cambió desde la última vez (el caso normal, escala 1).
    setWorldScale(scale);
    auto* dyn = static_cast<PxRigidActor*>(m_actor)->is<PxRigidDynamic>();
    if (dyn && (dyn->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC))
        dyn->setKinematicTarget(pose);
    else
        static_cast<PxRigidActor*>(m_actor)->setGlobalPose(pose);
#else
    (void)worldTransform;
#endif
}

void SphereCollider::teleport(const glm::mat4& worldTransform)
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;

    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    const PxTransform pose = poseFromWorld(worldTransform, &scale);

    setWorldScale(scale); // ver nota en syncTransform

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

void* SphereCollider::triggerShape() const
{
#ifdef DT_PHYSX_ENABLED
    return m_shape;
#else
    return nullptr;
#endif
}

} // namespace DonTopo
