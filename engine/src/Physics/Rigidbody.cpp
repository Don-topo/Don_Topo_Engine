#include "DonTopo/Physics/Rigidbody.h"
#include "DonTopo/Physics/Colliders/Collider.h"

#ifdef DT_PHYSX_ENABLED
#include <PxPhysicsAPI.h>
using namespace physx;

namespace {
    // The collider that owns the actor. PhysicsManager leaves the base Collider* in userData
    // when creating/rebuilding the actor, so it is the way back
    // Rigidbody -> Collider without storing an extra pointer or touching bindActor.
    // It is used by interpolation, which is a property of the Rigidbody but is executed
    // by the collider.
    DonTopo::Collider* colliderOf(void* actor)
    {
        if (!actor) return nullptr;
        return static_cast<DonTopo::Collider*>(static_cast<PxRigidDynamic*>(actor)->userData);
    }

    // Real eENABLE_CCD of the actor. PhysX does not support CCD on kinematic bodies
    // (it warns and ignores it), so the effective flag is "what the user asked for
    // AND it is not kinematic". The intent is stored in Rigidbody::m_ccd and
    // re-applied every time the kinematic mode changes.
    void applyCcdFlag(PxRigidDynamic* actor, bool ccd, bool kinematic)
    {
        actor->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD, ccd && !kinematic);
    }

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

    // ForceMode (Rigidbody.h) -> PxForceMode. Any value outside the enum
    // falls to eFORCE, which is the historical behavior.
    physx::PxForceMode::Enum toPxForceMode(DonTopo::ForceMode m) {
        switch (m) {
            case DonTopo::ForceMode::Acceleration:   return PxForceMode::eACCELERATION;
            case DonTopo::ForceMode::Impulse:        return PxForceMode::eIMPULSE;
            case DonTopo::ForceMode::VelocityChange: return PxForceMode::eVELOCITY_CHANGE;
            case DonTopo::ForceMode::Force:
            default:                                 return PxForceMode::eFORCE;
        }
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
    // setMassAndUpdateInertia recomputes the inertia from the shapes.
    PxRigidBodyExt::setMassAndUpdateInertia(*a, m_mass);
    a->setLinearDamping(m_drag);
    a->setAngularDamping(m_angularDrag);
    a->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !m_useGravity);
    a->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, m_isKinematic);
    a->setRigidDynamicLockFlags(toLockFlags(m_constraints));
    applyCcdFlag(a, m_ccd, m_isKinematic);
    // Interpolation is executed by the collider: when (re)linking the actor it has to be
    // pushed to it again, because after a static<->dynamic rebuild the collider
    // is still the same one but the config lives here.
    if (auto* col = colliderOf(m_actor)) col->setInterpolate(m_interpolate);
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
    if (!m_actor) return;
    auto* a = static_cast<PxRigidDynamic*>(m_actor);
    a->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !enabled);
    // Re-enabling gravity on a sleeping body does not wake it by itself: without
    // wakeUp it stays frozen until something disturbs it.
    if (enabled && !m_isKinematic) a->wakeUp();
#endif
}

void Rigidbody::setIsKinematic(bool enabled)
{
    m_isKinematic = enabled;
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;
    auto* a = static_cast<PxRigidDynamic*>(m_actor);
    // The effective CCD depends on the mode, and the ORDER matters: PhysX validates the
    // (kinematic, CCD) pair as soon as either of the two is touched, so the
    // CCD flag is removed BEFORE entering kinematic and given back AFTER
    // leaving. The other way round it works the same but spits out
    // "kinematic bodies with CCD enabled are not supported" on the error stream.
    if (enabled) applyCcdFlag(a, m_ccd, true);
    a->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, enabled);
    if (!enabled) applyCcdFlag(a, m_ccd, false);
    // When going from kinematic to dynamic, wake it up so that the simulation
    // picks it up again (otherwise it only falls after the first disturbance).
    if (!enabled) a->wakeUp();
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

void Rigidbody::setCcd(bool enabled)
{
    m_ccd = enabled;
#ifdef DT_PHYSX_ENABLED
    if (m_actor) applyCcdFlag(static_cast<PxRigidDynamic*>(m_actor), enabled, m_isKinematic);
#endif
}

void Rigidbody::setInterpolate(bool enabled)
{
    m_interpolate = enabled;
#ifdef DT_PHYSX_ENABLED
    if (auto* col = colliderOf(m_actor)) col->setInterpolate(enabled);
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
    if (a->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC) return; // PhysX forbids setting velocity on a kinematic
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

void Rigidbody::addForce(const glm::vec3& f, ForceMode mode)
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;
    auto* a = static_cast<PxRigidDynamic*>(m_actor);
    if (a->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC) return;
    a->addForce(PxVec3(f.x, f.y, f.z), toPxForceMode(mode));
#else
    (void)f;
    (void)mode;
#endif
}

void Rigidbody::addTorque(const glm::vec3& t, ForceMode mode)
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;
    auto* a = static_cast<PxRigidDynamic*>(m_actor);
    if (a->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC) return;
    a->addTorque(PxVec3(t.x, t.y, t.z), toPxForceMode(mode));
#else
    (void)t;
    (void)mode;
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
