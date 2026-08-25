#include "DonTopo/Physics/Colliders/Collider.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include <algorithm>

#ifdef DT_PHYSX_ENABLED
#include <PxPhysicsAPI.h>
using namespace physx;
#endif

namespace DonTopo {

Collider::~Collider()
{
    // Avisa al manager para que purgue este collider de los sets de overlap de
    // todos los triggers vivos (evita punteros colgantes antes del siguiente
    // dispatchStay). Requisito: los colliders mueren antes que el
    // PhysicsManager (mismo contrato que el release() del actor en los dtor
    // derivados, que asume la escena PhysX todavía viva).
    if (m_manager) m_manager->onColliderDestroyed(this);
}

void Collider::applyTriggerFlag(bool enabled)
{
    m_isTrigger = enabled;
#ifdef DT_PHYSX_ENABLED
    auto* shape = static_cast<PxShape*>(triggerShape());
    if (!shape) return;
    // PhysX prohíbe que una shape sea simulation y trigger a la vez: hay que
    // quitar una antes de poner la otra.
    if (enabled)
    {
        shape->setFlag(PxShapeFlag::eSIMULATION_SHAPE, false);
        shape->setFlag(PxShapeFlag::eTRIGGER_SHAPE, true);
    }
    else
    {
        shape->setFlag(PxShapeFlag::eTRIGGER_SHAPE, false);
        shape->setFlag(PxShapeFlag::eSIMULATION_SHAPE, true);
    }
#endif
}

#ifdef DT_PHYSX_ENABLED
namespace {
// Devuelve el PxMaterial exclusivo de la shape (índice 0). PhysX admite N
// materiales por shape (uno por triángulo en mallas), pero las 4 factorías
// crean la shape con uno solo, así que el 0 es SIEMPRE el de este collider.
PxMaterial* shapeMaterial(void* shapeHandle)
{
    auto* shape = static_cast<PxShape*>(shapeHandle);
    if (!shape || shape->getNbMaterials() == 0) return nullptr;
    PxMaterial* material = nullptr;
    if (shape->getMaterials(&material, 1) != 1) return nullptr;
    return material;
}
} // namespace
#endif

void Collider::setFriction(float staticF, float dynamicF)
{
    m_staticFriction  = staticF;
    m_dynamicFriction = dynamicF;
#ifdef DT_PHYSX_ENABLED
    if (auto* material = shapeMaterial(triggerShape()))
    {
        material->setStaticFriction(staticF);
        material->setDynamicFriction(dynamicF);
    }
#endif
}

void Collider::setBounciness(float restitution)
{
    m_restitution = restitution;
#ifdef DT_PHYSX_ENABLED
    if (auto* material = shapeMaterial(triggerShape()))
        material->setRestitution(restitution);
#endif
}

void Collider::addListener(ITriggerListener* listener)
{
    if (!listener) return;
    if (std::find(m_listeners.begin(), m_listeners.end(), listener) == m_listeners.end())
        m_listeners.push_back(listener);
}

void Collider::removeListener(ITriggerListener* listener)
{
    m_listeners.erase(std::remove(m_listeners.begin(), m_listeners.end(), listener),
                      m_listeners.end());
}

void Collider::beginOverlap(Collider* other)
{
    if (!other) return;
    if (!m_overlaps.insert(other).second) return; // ya solapaba: no re-disparar Enter
    TriggerEvent e{ other->getOwner(), other };
    for (auto* l : m_listeners) l->onTriggerEnter(e);
}

void Collider::endOverlap(Collider* other)
{
    if (m_overlaps.erase(other) == 0) return; // no estaba: nada que hacer
    TriggerEvent e{ other->getOwner(), other };
    for (auto* l : m_listeners) l->onTriggerExit(e);
}

void Collider::removeOverlapSilent(Collider* other)
{
    m_overlaps.erase(other); // sin disparar Exit: el otro se está destruyendo
}

void Collider::dispatchStay()
{
    for (auto* other : m_overlaps)
    {
        TriggerEvent e{ other->getOwner(), other };
        for (auto* l : m_listeners) l->onTriggerStay(e);
    }
}

} // namespace DonTopo
