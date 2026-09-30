#include "DonTopo/Physics/Colliders/Collider.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#ifdef DT_PHYSX_ENABLED
#include <PxPhysicsAPI.h>
using namespace physx;
#endif

namespace DonTopo {

Collider::~Collider()
{
    // Warns the manager so that it purges this collider from the overlap sets of
    // all live triggers (avoids dangling pointers before the next
    // dispatchStay). Requirement: colliders die before the
    // PhysicsManager (same contract as the actor's release() in the derived
    // dtors, which assumes the PhysX scene is still alive).
    if (m_manager) m_manager->onColliderDestroyed(this);
}

void Collider::applyTriggerFlag(bool enabled)
{
    m_isTrigger = enabled;
#ifdef DT_PHYSX_ENABLED
    auto* shape = static_cast<PxShape*>(triggerShape());
    if (!shape) return;
    // PhysX forbids a shape from being simulation and trigger at the same time: one has to be
    // removed before setting the other.
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
// Returns the shape's exclusive PxMaterial (index 0). PhysX supports N
// materials per shape (one per triangle in meshes), but the 4 factories
// create the shape with just one, so 0 is ALWAYS this collider's.
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

void Collider::setLayer(int layer)
{
    if (!PhysicsManager::isValidLayer(layer)) return;
    m_layer = layer;
    // Storing the number filters nothing: the one that decides is the shape's
    // PxFilterData, and the manager rewrites that with the new layer's mask.
    if (m_manager) m_manager->refreshColliderFilter(this);
}

void Collider::setInterpolate(bool enabled)
{
    m_interpolate = enabled;
    // When turned off the previous pose is forgotten: if it is turned on again later,
    // blending against a pose from a thousand frames ago would give a jump.
    if (!enabled) m_hasPrevPose = false;
}

void Collider::capturePreviousPose()
{
    if (!m_interpolate) return;
#ifdef DT_PHYSX_ENABLED
    auto* actor = static_cast<PxRigidActor*>(actorHandle());
    if (!actor) return;
    const PxTransform pose = actor->getGlobalPose();
    m_prevPosition = glm::vec3(pose.p.x, pose.p.y, pose.p.z);
    m_prevRotation = glm::quat(pose.q.w, pose.q.x, pose.q.y, pose.q.z);
    m_hasPrevPose  = true;
#endif
}

void Collider::setInterpolationAlpha(float alpha)
{
    m_interpAlpha = alpha;
}

glm::mat4 Collider::blendWithPreviousPose(const glm::mat4& current) const
{
    if (!m_interpolate || !m_hasPrevPose) return current;

    const glm::vec3 currentPosition(current[3]);
    const glm::quat currentRotation = glm::quat_cast(current);

    const glm::vec3 position = glm::mix(m_prevPosition, currentPosition, m_interpAlpha);
    // slerp and not mix on the quaternion: with fast rotations the linear
    // blend shortens the arc and the angular velocity comes out irregular. glm::slerp
    // already takes the short way (negates q2 if the dot product is negative).
    const glm::quat rotation = glm::slerp(m_prevRotation, currentRotation, m_interpAlpha);

    return glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation);
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

void Collider::addCollisionListener(ICollisionListener* listener)
{
    if (!listener) return;
    if (std::find(m_collisionListeners.begin(), m_collisionListeners.end(), listener)
        == m_collisionListeners.end())
        m_collisionListeners.push_back(listener);
}

void Collider::removeCollisionListener(ICollisionListener* listener)
{
    m_collisionListeners.erase(
        std::remove(m_collisionListeners.begin(), m_collisionListeners.end(), listener),
        m_collisionListeners.end());
}

// The three collision dispatches iterate by INDEX rereading size(), not with a
// range-for: a listener can unregister itself (or register another) inside its
// own callback, and that invalidates the iterators of a range-for. With an index, a
// remove during iteration only skips the element that took the gap, which
// is exactly what happens in Unity, instead of being UB.
void Collider::dispatchCollisionEnter(Collider* other)
{
    if (!other) return;
    CollisionEvent e{ other->getOwner(), other };
    for (size_t i = 0; i < m_collisionListeners.size(); ++i)
        m_collisionListeners[i]->onCollisionEnter(e);
}

void Collider::dispatchCollisionStay(Collider* other)
{
    if (!other) return;
    CollisionEvent e{ other->getOwner(), other };
    for (size_t i = 0; i < m_collisionListeners.size(); ++i)
        m_collisionListeners[i]->onCollisionStay(e);
}

void Collider::dispatchCollisionExit(Collider* other)
{
    if (!other) return;
    CollisionEvent e{ other->getOwner(), other };
    for (size_t i = 0; i < m_collisionListeners.size(); ++i)
        m_collisionListeners[i]->onCollisionExit(e);
}

void Collider::beginOverlap(Collider* other)
{
    if (!other) return;
    if (!m_overlaps.insert(other).second) return; // already overlapped: do not re-fire Enter
    TriggerEvent e{ other->getOwner(), other };
    for (auto* l : m_listeners) l->onTriggerEnter(e);
}

void Collider::endOverlap(Collider* other)
{
    if (m_overlaps.erase(other) == 0) return; // was not there: nothing to do
    TriggerEvent e{ other->getOwner(), other };
    for (auto* l : m_listeners) l->onTriggerExit(e);
}

void Collider::removeOverlapSilent(Collider* other)
{
    m_overlaps.erase(other); // without firing Exit: the other is being destroyed
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
