#include "DonTopo/Physics/Rigidbody.h"
#include "DonTopo/Physics/Colliders/Collider.h"

#ifdef DT_PHYSX_ENABLED
#include <PxPhysicsAPI.h>
using namespace physx;

namespace {
    // El collider dueño del actor. PhysicsManager deja en userData el Collider*
    // base al crear/reconstruir el actor, así que es el camino de vuelta
    // Rigidbody -> Collider sin guardar un puntero extra ni tocar bindActor.
    // Lo usa la interpolación, que es propiedad del Rigidbody pero la ejecuta
    // el collider.
    DonTopo::Collider* colliderOf(void* actor)
    {
        if (!actor) return nullptr;
        return static_cast<DonTopo::Collider*>(static_cast<PxRigidDynamic*>(actor)->userData);
    }

    // eENABLE_CCD real del actor. PhysX no soporta CCD en cuerpos kinematic
    // (avisa y lo ignora), así que el flag efectivo es "lo que pidió el usuario
    // Y no es kinematic". La intención se guarda en Rigidbody::m_ccd y se
    // re-aplica cada vez que cambia el modo kinematic.
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

    // ForceMode (Rigidbody.h) -> PxForceMode. Cualquier valor fuera del enum
    // cae en eFORCE, que es el comportamiento histórico.
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
    // setMassAndUpdateInertia recalcula la inercia a partir de las shapes.
    PxRigidBodyExt::setMassAndUpdateInertia(*a, m_mass);
    a->setLinearDamping(m_drag);
    a->setAngularDamping(m_angularDrag);
    a->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !m_useGravity);
    a->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, m_isKinematic);
    a->setRigidDynamicLockFlags(toLockFlags(m_constraints));
    applyCcdFlag(a, m_ccd, m_isKinematic);
    // La interpolación la ejecuta el collider: al (re)enlazar el actor hay que
    // volver a empujársela, porque tras un rebuild static<->dynamic el collider
    // sigue siendo el mismo pero la config vive aquí.
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
    // Reactivar gravedad sobre un cuerpo dormido no lo despierta solo: sin
    // wakeUp se queda congelado hasta que algo lo perturbe.
    if (enabled && !m_isKinematic) a->wakeUp();
#endif
}

void Rigidbody::setIsKinematic(bool enabled)
{
    m_isKinematic = enabled;
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;
    auto* a = static_cast<PxRigidDynamic*>(m_actor);
    // El CCD efectivo depende del modo, y el ORDEN importa: PhysX valida el
    // par (kinematic, CCD) en cuanto se toca cualquiera de los dos, así que el
    // flag de CCD se quita ANTES de entrar en kinematic y se devuelve DESPUÉS
    // de salir. Al revés funciona igual pero escupe
    // "kinematic bodies with CCD enabled are not supported" por el error stream.
    if (enabled) applyCcdFlag(a, m_ccd, true);
    a->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, enabled);
    if (!enabled) applyCcdFlag(a, m_ccd, false);
    // Al pasar de kinematic a dinámico, despertarlo para que la simulación lo
    // retome (si no, cae sólo tras la primera perturbación).
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
