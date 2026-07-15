#include "DonTopo/Physics/Rigidbody.h"

#ifdef DT_PHYSX_ENABLED
#include <PxPhysicsAPI.h>
using namespace physx;

namespace {
    physx::PxRigidDynamicLockFlags toLockFlags(uint32_t c) {
        using namespace DonTopo;
        physx::PxRigidDynamicLockFlags f(0);
        if (c & RB_FreezePositionX) f |= PxRigidDynamicLockFlag::eLOCK_LINEAR_X;
        if (c & RB_FreezePositionY) f |= PxRigidDynamicLockFlag::eLOCK_LINEAR_Y;
        if (c & RB_FreezePositionZ) f |= PxRigidDynamicLockFlag::eLOCK_LINEAR_Z;
        if (c & RB_FreezeRotationX) f |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_X;
        if (c & RB_FreezeRotationY) f |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_Y;
        if (c & RB_FreezeRotationZ) f |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_Z;
        return f;
    }
}
#endif

namespace DonTopo {

void Rigidbody::bindActor(void* actor)
{
    m_actor = actor;
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;
    auto* a = static_cast<PxRigidDynamic*>(m_actor);
    // setMassAndUpdateInertia recalcula la inercia a partir de las shapes.
    PxRigidBodyExt::setMassAndUpdateInertia(*a, m_mass);
    a->setLinearDamping(m_drag);
    a->setAngularDamping(m_angularDrag);
    a->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !m_useGravity);
    a->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, m_isKinematic);
    a->setRigidDynamicLockFlags(toLockFlags(m_constraints));
    if (!m_isKinematic) a->wakeUp();
#endif
}

void Rigidbody::setMass(float mass)
{
    m_mass = mass;
#ifdef DT_PHYSX_ENABLED
    if (m_actor) PxRigidBodyExt::setMassAndUpdateInertia(*static_cast<PxRigidDynamic*>(m_actor), m_mass);
#endif
}

void Rigidbody::setUseGravity(bool enabled)
{
    m_useGravity = enabled;
#ifdef DT_PHYSX_ENABLED
    if (m_actor) static_cast<PxRigidDynamic*>(m_actor)->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !enabled);
#endif
}

void Rigidbody::setIsKinematic(bool enabled)
{
    m_isKinematic = enabled;
#ifdef DT_PHYSX_ENABLED
    if (m_actor) static_cast<PxRigidDynamic*>(m_actor)->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, enabled);
#endif
}

void Rigidbody::setDrag(float drag)
{
    m_drag = drag;
#ifdef DT_PHYSX_ENABLED
    if (m_actor) static_cast<PxRigidDynamic*>(m_actor)->setLinearDamping(drag);
#endif
}

void Rigidbody::setAngularDrag(float drag)
{
    m_angularDrag = drag;
#ifdef DT_PHYSX_ENABLED
    if (m_actor) static_cast<PxRigidDynamic*>(m_actor)->setAngularDamping(drag);
#endif
}

void Rigidbody::setConstraints(uint32_t mask)
{
    m_constraints = mask;
#ifdef DT_PHYSX_ENABLED
    if (m_actor) static_cast<PxRigidDynamic*>(m_actor)->setRigidDynamicLockFlags(toLockFlags(mask));
#endif
}

glm::vec3 Rigidbody::getVelocity() const
{
#ifdef DT_PHYSX_ENABLED
    if (m_actor) { PxVec3 v = static_cast<PxRigidDynamic*>(m_actor)->getLinearVelocity(); return { v.x, v.y, v.z }; }
#endif
    return glm::vec3(0.0f);
}

void Rigidbody::setVelocity(const glm::vec3& v)
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;
    auto* a = static_cast<PxRigidDynamic*>(m_actor);
    if (a->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC) return; // PhysX prohíbe setear velocidad a kinematic
    a->setLinearVelocity(PxVec3(v.x, v.y, v.z));
#else
    (void)v;
#endif
}

glm::vec3 Rigidbody::getAngularVelocity() const
{
#ifdef DT_PHYSX_ENABLED
    if (m_actor) { PxVec3 v = static_cast<PxRigidDynamic*>(m_actor)->getAngularVelocity(); return { v.x, v.y, v.z }; }
#endif
    return glm::vec3(0.0f);
}

void Rigidbody::setAngularVelocity(const glm::vec3& v)
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;
    auto* a = static_cast<PxRigidDynamic*>(m_actor);
    if (a->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC) return;
    a->setAngularVelocity(PxVec3(v.x, v.y, v.z));
#else
    (void)v;
#endif
}

void Rigidbody::addForce(const glm::vec3& f)
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;
    auto* a = static_cast<PxRigidDynamic*>(m_actor);
    if (a->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC) return;
    a->addForce(PxVec3(f.x, f.y, f.z), PxForceMode::eFORCE);
#else
    (void)f;
#endif
}

void Rigidbody::addTorque(const glm::vec3& t)
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;
    auto* a = static_cast<PxRigidDynamic*>(m_actor);
    if (a->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC) return;
    a->addTorque(PxVec3(t.x, t.y, t.z), PxForceMode::eFORCE);
#else
    (void)t;
#endif
}

void Rigidbody::addImpulse(const glm::vec3& f)
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;
    auto* a = static_cast<PxRigidDynamic*>(m_actor);
    if (a->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC) return;
    a->addForce(PxVec3(f.x, f.y, f.z), PxForceMode::eIMPULSE);
#else
    (void)f;
#endif
}

} // namespace DonTopo
