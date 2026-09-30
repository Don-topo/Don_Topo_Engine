#pragma once
#include <vector>
#include <unordered_set>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace DonTopo {

class Collider;
class PhysicsManager;

// Unity-style trigger event. `other` is an OPAQUE handle (void*) to the owner
// of the collider that caused the event: the physics module is agnostic of
// GameObject (the dependency goes Core -> Physics, never the other way), so Core
// sets the owner via Collider::setOwner and editor/scripting resolve it to a
// GameObject later. `otherCollider` is the concrete collider that overlapped.
struct TriggerEvent {
    void*     other         = nullptr;
    Collider* otherCollider = nullptr;
};

// Trigger callback interface. Empty methods by default: the consumer
// overrides only the ones it needs (just like Unity's OnTriggerEnter/Stay/Exit).
class ITriggerListener {
public:
    virtual ~ITriggerListener() = default;
    virtual void onTriggerEnter(const TriggerEvent&) {}
    virtual void onTriggerStay (const TriggerEvent&) {}
    virtual void onTriggerExit (const TriggerEvent&) {}
};

// COLLISION event: the pair is not a trigger and PhysX really resolves it
// (impulses, bounce). Same fields and same semantics of opaque `other` as
// TriggerEvent.
//
// It deliberately carries NO contact points: PhysX provides them
// (PxContactPair::extractContacts) but requires requesting
// eNOTIFY_CONTACT_POINTS in the filter shader, which copies the contact
// stream of EVERY pair into callback memory. Today nobody in the engine
// —scripting, editor, gameplay— consumes contact positions, so that cost
// will be paid the day it is needed.
struct CollisionEvent {
    void*     other         = nullptr;
    Collider* otherCollider = nullptr;
};

// Twin of ITriggerListener for non-trigger pairs. Empty methods by
// default, as there.
//
// Difference with triggers: here Stay is NOT synthesized per frame. PhysX
// emits eNOTIFY_TOUCH_PERSISTS natively in onContact while the
// contact stays alive, so no registry or overlap traversal is needed.
class ICollisionListener {
public:
    virtual ~ICollisionListener() = default;
    virtual void onCollisionEnter(const CollisionEvent&) {}
    virtual void onCollisionStay (const CollisionEvent&) {}
    virtual void onCollisionExit (const CollisionEvent&) {}
};

// Common base of the 4 colliders (Box/Sphere/Capsule/Plane). It provides the trigger
// state, the opaque owner, the listeners and the overlap set.
//
// PhysX reports Enter/Exit natively (eNOTIFY_TOUCH_FOUND/LOST via
// PxSimulationEventCallback::onTrigger); Stay is NOT provided by PhysX and is synthesized
// every frame by traversing m_overlaps (done by PhysicsManager::stepSimulation).
//
// PhysX limitation: it does not report trigger<->trigger overlaps; at least one side
// of the pair must be non-trigger to receive events.
class Collider {
public:
    virtual ~Collider();

    Collider() = default;
    Collider(const Collider&)            = delete;
    Collider& operator=(const Collider&) = delete;

    // Marks/unmarks the shape as trigger in PhysX (sets eTRIGGER_SHAPE and
    // removes eSIMULATION_SHAPE: a shape cannot be both). It does NOT manage
    // registration in the PhysicsManager Stay registry — use
    // PhysicsManager::setTrigger as the public entry point, which calls this and
    // updates the registry.
    void applyTriggerFlag(bool enabled);
    bool isTrigger() const { return m_isTrigger; }

    // Opaque owner. Set by Core when linking collider <-> GameObject; the physics
    // module never dereferences it, it only carries it in TriggerEvent.
    void  setOwner(void* owner) { m_owner = owner; }
    void* getOwner() const      { return m_owner; }

    void addListener(ITriggerListener* listener);
    void removeListener(ITriggerListener* listener);

    // Collision listeners (non-trigger pairs). A list separate from the
    // triggers one: a collider cannot be both things at once, but the same
    // consumer can register in both and we do not want an event of
    // one type to traverse the listeners of the other.
    void addCollisionListener(ICollisionListener* listener);
    void removeCollisionListener(ICollisionListener* listener);

    // Physics material PER COLLIDER. The defaults are the same as the previous
    // global PxMaterial had (0.5 / 0.5 / 0.1), so a scene that does not
    // store these fields simulates exactly as before.
    //
    // The setters write to the shape's PxMaterial (which is exclusive to this
    // collider: PhysicsManager creates it in the factory). Without DT_PHYSX_ENABLED
    // they only store the value, so that the editor and serialization work
    // the same in a build without PhysX.
    void setFriction(float staticF, float dynamicF);
    void setBounciness(float restitution);

    float getStaticFriction() const  { return m_staticFriction; }
    float getDynamicFriction() const { return m_dynamicFriction; }
    float getBounciness() const      { return m_restitution; }

    // Collision layer of the collider (0-31, 0 is "Default"). Which layers that layer
    // collides with is decided by the PhysicsManager matrix.
    //
    // The setter does NOT just store the number: PhysX filters by the shape's
    // PxFilterData, so it has to be rewritten
    // (PhysicsManager::refreshColliderFilter). An index outside [0,31] is
    // ignored and the previous layer is kept — 1u<<32 is UB and an invented layer has
    // no row in the matrix.
    void setLayer(int layer);
    int  getLayer() const { return m_layer; }

    // --- Pose interpolation (Rigidbody.interpolate) --------------------------
    //
    // Physics runs at a fixed step and rendering goes at frame rate: between two
    // sub-steps the actor pose does NOT change, and at 144 Hz with physics at 60 it
    // looks jerky. With interpolation, getWorldTransform does not return the raw
    // actor pose but the blend between the pose before the last sub-step and
    // the current one, according to how much of the next step the accumulator has consumed.
    //
    // It is purely VISUAL: it does not touch what PhysX simulates, so a raycast or a
    // trigger still see the real pose. Default OFF -> getWorldTransform
    // returns exactly what it always did.
    //
    // It is turned on by Rigidbody::setInterpolate (which gets here through the
    // actor's userData): the property is exposed by the Rigidbody, the mechanics
    // live in the collider, which is the one that has the pose.
    void setInterpolate(bool enabled);
    bool getInterpolate() const { return m_interpolate; }

    // Stores the actor pose BEFORE the sub-step; called by
    // PhysicsManager::stepSimulation once per sub-step. No-op if this
    // collider does not interpolate.
    void capturePreviousPose();

    // Fraction [0,1] of the fixed step already consumed. Pushed by
    // PhysicsManager::stepSimulation at the end of the frame.
    void setInterpolationAlpha(float alpha);

    // Set by PhysicsManager when creating the collider, so that ~Collider can
    // report its destruction and purge itself from the overlaps of other triggers.
    void setManager(PhysicsManager* manager) { m_manager = manager; }

    // Underlying PhysX actor (PxRigidStatic* or PxRigidDynamic*), as void*.
    // Used by PhysicsManager::rebuildActor to reassign the actor after changing
    // type (static <-> dynamic). The collider remains the OWNER: it
    // releases it in its dtor.
    virtual void* actorHandle() const = 0;
    virtual void  setActorHandle(void* actor) = 0;

    // physx::PxShape* of the concrete collider (as void*), reuses the internal
    // triggerShape(). Used by PhysicsManager::rebuildActor to
    // reattach the SAME shape to the new actor after the static<->dynamic swap.
    void* geometryShape() const { return triggerShape(); }

    // Transform scale already baked into the shape geometry (PxTransform
    // does not support scale). Whoever APPLIES it is the setWorldScale of each concrete
    // collider —each shape distributes it in its own way—; here it is only remembered, so
    // that whoever pushes the pose (Scene::update) can see that it changed without
    // branching by type. The plane ignores it: it is infinite.
    glm::vec3 getWorldScale() const { return m_worldScale; }

    // Actor pose mechanics, polymorphic: traversed by Scene::update over
    // the base collider (anyCollider) without branching by concrete type. Each
    // collider implements them identically on its m_actor.
    //   getWorldTransform: reads actor pose -> world (simulated body).
    //   syncTransform:     pushes world -> actor (setKinematicTarget if
    //                      dynamic-kinematic; setGlobalPose otherwise).
    //   teleport:          setGlobalPose + velocity reset if real dynamic.
    virtual glm::mat4 getWorldTransform() const = 0;
    virtual void      syncTransform(const glm::mat4& worldTransform) = 0;
    virtual void      teleport(const glm::mat4& worldTransform) = 0;

    // Overlap bookkeeping, invoked by the PhysicsManager dispatcher.
    void beginOverlap(Collider* other);        // TOUCH_FOUND: inserts + onTriggerEnter
    void endOverlap(Collider* other);          // TOUCH_LOST : erases  + onTriggerExit
    void removeOverlapSilent(Collider* other); // cleanup without firing (destruction of the other)
    void dispatchStay();                        // per frame: onTriggerStay of each live overlap

    // Collisions, invoked by the same dispatcher from onContact. There is no
    // bookkeeping to keep: PhysX emits all three
    // (eNOTIFY_TOUCH_FOUND/PERSISTS/LOST) and here they are only distributed.
    void dispatchCollisionEnter(Collider* other);
    void dispatchCollisionStay (Collider* other);
    void dispatchCollisionExit (Collider* other);

protected:
    // Blends `current` (the real actor pose, just read by the concrete
    // collider) with the captured previous pose, using the accumulator alpha.
    // Returns `current` as is if interpolation is off or there is no previous
    // pose yet. Called by the getWorldTransform of the colliders that can have a
    // Rigidbody (box/sphere/capsule); the plane is always static.
    glm::mat4 blendWithPreviousPose(const glm::mat4& current) const;

    // Returns physx::PxShape* of the concrete collider, as void* so as not to leak
    // PhysX into this header (which reaches Core via GameObject.h). Without
    // DT_PHYSX_ENABLED it returns nullptr.
    virtual void* triggerShape() const = 0;

    // Written by the setWorldScale of each derived class right before rebuilding its
    // geometry. It starts at 1: a scene without scaled objects never gets to
    // touch the shape.
    glm::vec3 m_worldScale{1.0f};

private:
    bool                           m_isTrigger = false;
    // Defaults identical to the global PxMaterial that existed before (0.5/0.5/0.1).
    float                          m_staticFriction  = 0.5f;
    float                          m_dynamicFriction = 0.5f;
    float                          m_restitution     = 0.1f;
    int                            m_layer     = 0; // "Default"
    // Visual interpolation. m_hasPrevPose avoids blending against a previous pose
    // that was never captured (the first frame after turning it on).
    bool                           m_interpolate  = false;
    bool                           m_hasPrevPose  = false;
    float                          m_interpAlpha  = 1.0f;
    glm::vec3                      m_prevPosition{0.0f};
    glm::quat                      m_prevRotation{1.0f, 0.0f, 0.0f, 0.0f};
    void*                          m_owner     = nullptr;
    PhysicsManager*                m_manager   = nullptr;
    std::vector<ITriggerListener*> m_listeners;
    std::vector<ICollisionListener*> m_collisionListeners;
    std::unordered_set<Collider*>  m_overlaps;
};

} // namespace DonTopo
