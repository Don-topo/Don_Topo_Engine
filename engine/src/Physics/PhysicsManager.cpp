#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Physics/Rigidbody.h"
#include "DonTopo/Physics/Colliders/Collider.h"
#include "DonTopo/Physics/Colliders/BoxCollider.h"
#include "DonTopo/Physics/Colliders/SphereCollider.h"
#include "DonTopo/Physics/Colliders/CapsuleCollider.h"
#include "DonTopo/Physics/Colliders/PlaneCollider.h"

#include <algorithm>

#ifdef DT_PHYSX_ENABLED
#define GLM_ENABLE_EXPERIMENTAL
#include <PxPhysicsAPI.h>
#include "PxPose.h"
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>
#include <stdexcept>
#include <string>

using namespace physx;

namespace {
    PxDefaultAllocator      g_allocator;
    PxDefaultErrorCallback  g_errorCallback;


    // Values of the PxMaterial created per collider. They match the
    // defaults of Collider (m_staticFriction/m_dynamicFriction/m_restitution) and
    // those of the global material that all colliders shared before, so
    // an existing scene simulates exactly the same.
    constexpr float kDefaultStaticFriction  = 0.5f;
    constexpr float kDefaultDynamicFriction = 0.5f;
    constexpr float kDefaultRestitution     = 0.1f;

    // Same trick used in CapsuleCollider.cpp/PlaneCollider.cpp: PhysX
    // orients PxCapsuleGeometry along X and defines the normal of
    // PxPlaneGeometry as the local X axis of the shape. This fixed rotation
    // (90° about Z) maps that X axis to Y in both cases.
    PxQuat axisCorrection() { return PxQuat(PxHalfPi, PxVec3(0.0f, 0.0f, 1.0f)); }

    // Receives the trigger pairs from PhysX and forwards them to the collider
    // callbacks. Each PxShape carries in its actor a userData = Collider* (set by
    // PhysicsManager when creating the collider), so who overlapped
    // whom is recovered. PhysX only emits Enter/Exit (TOUCH_FOUND/LOST); Stay is
    // synthesized by PhysicsManager::stepSimulation by traversing the overlaps.
    class TriggerDispatcher : public PxSimulationEventCallback {
    public:
        void onTrigger(PxTriggerPair* pairs, PxU32 count) override {
            for (PxU32 i = 0; i < count; ++i) {
                const PxTriggerPair& p = pairs[i];
                // shape already released (actor destroyed this frame): userData
                // would dangle, it is ignored.
                if (p.flags & (PxTriggerPairFlag::eREMOVED_SHAPE_TRIGGER |
                               PxTriggerPairFlag::eREMOVED_SHAPE_OTHER))
                    continue;

                PxRigidActor* tActor = p.triggerShape->getActor();
                PxRigidActor* oActor = p.otherShape->getActor();
                if (!tActor || !oActor) continue;

                auto* triggerCol = static_cast<DonTopo::Collider*>(tActor->userData);
                auto* otherCol   = static_cast<DonTopo::Collider*>(oActor->userData);
                if (!triggerCol || !otherCol) continue;

                if (p.status & PxPairFlag::eNOTIFY_TOUCH_FOUND)
                    triggerCol->beginOverlap(otherCol);
                else if (p.status & PxPairFlag::eNOTIFY_TOUCH_LOST)
                    triggerCol->endOverlap(otherCol);
            }
        }

        // NON-trigger pairs that really touch. Twin of onTrigger, with two
        // differences that come from PhysX, not from here:
        //  - Stay IS native (eNOTIFY_TOUCH_PERSISTS), so it does not have to be
        //    synthesized per frame as with triggers.
        //  - A collision has no "owner" side: BOTH colliders are notified,
        //    each with the other as `other` (just like Unity).
        // The notification flags are only requested for non-trigger pairs (see
        // dtTriggerFilterShader), so a trigger never arrives here.
        void onContact(const PxContactPairHeader& header,
                       const PxContactPair* pairs, PxU32 count) override
        {
            // Actor deleted this frame: its userData is already dangling.
            if (header.flags & (PxContactPairHeaderFlag::eREMOVED_ACTOR_0 |
                                PxContactPairHeaderFlag::eREMOVED_ACTOR_1))
                return;

            auto* colA = static_cast<DonTopo::Collider*>(header.actors[0]->userData);
            auto* colB = static_cast<DonTopo::Collider*>(header.actors[1]->userData);
            if (!colA || !colB) return;

            for (PxU32 i = 0; i < count; ++i)
            {
                const PxContactPair& cp = pairs[i];
                // Same guard as in onTrigger, at shape level.
                if (cp.flags & (PxContactPairFlag::eREMOVED_SHAPE_0 |
                                PxContactPairFlag::eREMOVED_SHAPE_1))
                    continue;

                if (cp.events & PxPairFlag::eNOTIFY_TOUCH_FOUND)
                {
                    colA->dispatchCollisionEnter(colB);
                    colB->dispatchCollisionEnter(colA);
                }
                else if (cp.events & PxPairFlag::eNOTIFY_TOUCH_PERSISTS)
                {
                    colA->dispatchCollisionStay(colB);
                    colB->dispatchCollisionStay(colA);
                }
                else if (cp.events & PxPairFlag::eNOTIFY_TOUCH_LOST)
                {
                    colA->dispatchCollisionExit(colB);
                    colB->dispatchCollisionExit(colA);
                }
            }
        }

        // Remaining simulation events: not used.
        void onConstraintBreak(PxConstraintInfo*, PxU32) override {}
        void onWake(PxActor**, PxU32) override {}
        void onSleep(PxActor**, PxU32) override {}
        void onAdvance(const PxRigidBody* const*, const PxTransform*, PxU32) override {}
    };

    // Filter shader: for pairs that involve a trigger, it requests Enter/Exit
    // notification (eTRIGGER_DEFAULT) WITHOUT suppressing by kinematic — the
    // PxDefaultSimulationFilterShader discards kinematic-kinematic and
    // kinematic-static pairs, and without this a trigger would not see objects with
    // kinematic Rigidbody, which are most of those moved by script.
    // NON-trigger pairs are delegated to the default shader to preserve
    // the previous collision behavior exactly.
    //
    // NOTE what this shader CANNOT fix: static-static pairs do not
    // get here. PhysX does not even form them —two static actors cannot
    // move relative to each other—, so a trigger without a
    // Rigidbody does not detect objects that do not have one either. It is Unity's rule
    // ("at least one of the two needs a Rigidbody"); it is warned by the editor in the
    // collider section and fixed by the trigger tests in physics_tests.cpp.
    // (This comment used to say that "almost all colliders are
    // kinematic": it stopped being true when dynamics were separated from the Collider
    // and a collider without Rigidbody became a PxRigidStatic.)
    PxFilterFlags dtTriggerFilterShader(
        PxFilterObjectAttributes attr0, PxFilterData fd0,
        PxFilterObjectAttributes attr1, PxFilterData fd1,
        PxPairFlags& pairFlags, const void* constantBlock, PxU32 constantBlockSize)
    {
        // LAYERS, before anything else: word0 = bit of its own layer, word1 = mask
        // of layers it collides with (written by
        // PhysicsManager::refreshColliderFilter). The pair only survives if EACH
        // side accepts the other. It goes before the trigger branch on purpose: a
        // filtered layer must not fire onTriggerEnter either.
        //
        // eSUPPRESS, not eKILL: the matrix can be re-enabled at runtime and PhysX
        // has to be able to form the pair again (eKILL would discard it
        // forever). With the default matrix word1 is 0xFFFFFFFF and no pair
        // is suppressed.
        if ((fd0.word1 & fd1.word0) == 0 || (fd1.word1 & fd0.word0) == 0)
            return PxFilterFlag::eSUPPRESS;

        if (PxFilterObjectIsTrigger(attr0) || PxFilterObjectIsTrigger(attr1)) {
            pairFlags = PxPairFlag::eTRIGGER_DEFAULT;
            return PxFilterFlag::eDEFAULT;
        }
        // NOTE: the default shader is passed an EMPTY PxFilterData on purpose,
        // not ours. PxDefaultSimulationFilterShader indexes its internal group table
        // with the raw word0 (gCollisionTable[word0][word0], 32x32) and
        // turns word2/word3 into a PxGroupsMask; with our word0 = 1<<layer
        // (up to 2^31) it would read outside the table. Emptying it, it sees exactly the
        // same thing it saw before layers existed —all zeros—, which is what
        // preserves the previous behavior bit by bit.
        PxFilterFlags flags =
            PxDefaultSimulationFilterShader(attr0, PxFilterData(), attr1, PxFilterData(),
                                            pairFlags, constantBlock, constantBlockSize);

        // And on top of whatever the default shader decided, contact notifications
        // are requested for OnCollisionEnter/Stay/Exit. It is done AFTER and only if the
        // pair survives: if the shader killed or suppressed it, pairFlags means
        // nothing and adding bits to it would not change the result, but it would
        // mask the reason when debugging.
        //
        // Only non-trigger arrives here: the trigger branch above returns
        // earlier with eTRIGGER_DEFAULT intact. It is on purpose — PhysX does not even
        // generate contacts for a shape marked eTRIGGER_SHAPE, so requesting
        // eNOTIFY_TOUCH_* there would be noise that never fires.
        //
        // Cost: PERSISTS makes PhysX call onContact every sub-step for each
        // pair in contact, even if nobody listens. In exchange, Stay is native
        // and there is no need to traverse registries per frame as with triggers.
        //
        // eDETECT_CCD_CONTACT goes in the same batch: without this bit on the pair, the
        // scene's continuous pass does NOT run for it and marking the body
        // with eENABLE_CCD would do nothing. Requesting it here does not activate CCD by itself
        // —PhysX skips the sweep if no actor of the pair carries the body
        // flag—, so the usual pairs keep resolving the same.
        if (!(flags & (PxFilterFlag::eKILL | PxFilterFlag::eSUPPRESS)))
            pairFlags |= PxPairFlag::eNOTIFY_TOUCH_FOUND
                       | PxPairFlag::eNOTIFY_TOUCH_LOST
                       | PxPairFlag::eNOTIFY_TOUCH_PERSISTS
                       | PxPairFlag::eDETECT_CCD_CONTACT;

        return flags;
    }
}

static void physxCheck(void* ptr, const char* ctx) {
    if (!ptr)
        throw std::runtime_error(std::string(ctx) + ": creation failed");
}
#endif

namespace DonTopo {

PhysicsManager::~PhysicsManager() { shutdown(); }

void PhysicsManager::init()
{
#ifdef DT_PHYSX_ENABLED
    auto* foundation = PxCreateFoundation(PX_PHYSICS_VERSION, g_allocator, g_errorCallback);
    physxCheck(foundation, "PxCreateFoundation");
    m_foundation = foundation;

    // The world uses centimeters (gravity -981 = -9.81 m/s² * 100), not meters.
    // The default PxTolerancesScale assumes 1 unit = 1 meter; with that default,
    // sleepThreshold/contactOffset/bounceThresholdVelocity end up ~100x
    // too small for velocities in cm/s, so an actor at rest
    // never reaches the sleep threshold and vibrates indefinitely.
    PxTolerancesScale scale;
    scale.length = 100.0f; // 100 units = 1 meter
    scale.speed  = 981.0f; // typical fall speed after 1s under this gravity
    auto* physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation, scale);
    physxCheck(physics, "PxCreatePhysics");
    m_physics = physics;

    auto* dispatcher = PxDefaultCpuDispatcherCreate(2);
    physxCheck(dispatcher, "PxDefaultCpuDispatcherCreate");
    m_dispatcher = dispatcher;

    PxSceneDesc sceneDesc(physics->getTolerancesScale());
    sceneDesc.gravity       = PxVec3(0.0f, -981.0f, 0.0f);
    sceneDesc.cpuDispatcher = dispatcher;
    sceneDesc.filterShader  = dtTriggerFilterShader;
    // CCD at SCENE level: a prerequisite, not a global switch. PhysX
    // requires the flag to reserve the continuous sweep pass, but that pass
    // only looks at bodies that also carry PxRigidBodyFlag::eENABLE_CCD
    // (set by Rigidbody::setCcd, default OFF). With no body marked the
    // cost is zero and the simulation is exactly the one from before.
    sceneDesc.flags |= PxSceneFlag::eENABLE_CCD;
    auto* scene = physics->createScene(sceneDesc);
    physxCheck(scene, "PxPhysics::createScene");
    m_scene = scene;

    // There is no global material any more: each collider creates its own in its factory
    // (per-collider physics material, see kDefault* above).

    // Callback that receives the trigger pairs and forwards them to the colliders.
    auto* triggerCallback = new TriggerDispatcher();
    scene->setSimulationEventCallback(triggerCallback);
    m_triggerCallback = triggerCallback;
#endif
}

void PhysicsManager::shutdown()
{
#ifdef DT_PHYSX_ENABLED
    if (m_scene)      { static_cast<PxScene*>(m_scene)->release();      m_scene = nullptr; }
    // After releasing the scene nobody else references the callback: it is deleted here.
    if (m_triggerCallback) { delete static_cast<TriggerDispatcher*>(m_triggerCallback); m_triggerCallback = nullptr; }
    if (m_dispatcher) { static_cast<PxDefaultCpuDispatcher*>(m_dispatcher)->release(); m_dispatcher = nullptr; }
    if (m_physics)    { static_cast<PxPhysics*>(m_physics)->release();  m_physics = nullptr; }
    if (m_foundation) { static_cast<PxFoundation*>(m_foundation)->release(); m_foundation = nullptr; }
#endif
}

std::shared_ptr<BoxCollider> PhysicsManager::createBoxColliderComponent(
    const glm::vec3& halfExtents,
    const glm::vec3& center,
    const glm::mat4& worldTransform,
    bool dynamic)
{
#ifdef DT_PHYSX_ENABLED
    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    const PxTransform pose = poseFromWorld(worldTransform, &scale);

    auto* physics = static_cast<PxPhysics*>(m_physics);
    auto* scene = static_cast<PxScene*>(m_scene);

    // EXCLUSIVE material of this collider (same values as the global one from
    // before, so no scene changes behavior). It is refcounted: the
    // shape keeps a reference in createExclusiveShape, so we release
    // ours right after and the material dies with the shape.
    PxMaterial* material = physics->createMaterial(kDefaultStaticFriction,
                                                   kDefaultDynamicFriction,
                                                   kDefaultRestitution);
    physxCheck(material, "PxPhysics::createMaterial(box)");

    PxRigidActor* actor = dynamic
        ? static_cast<PxRigidActor*>(physics->createRigidDynamic(pose))
        : static_cast<PxRigidActor*>(physics->createRigidStatic(pose));
    physxCheck(actor, "PxPhysics::createRigidActor(box)");

    PxBoxGeometry geometry(halfExtents.x, halfExtents.y, halfExtents.z);
    PxShape* shape = PxRigidActorExt::createExclusiveShape(*actor, geometry, *material);
    physxCheck(shape, "PxRigidActorExt::createExclusiveShape");
    material->release();
    shape->setLocalPose(PxTransform(PxVec3(center.x, center.y, center.z)));

    if (dynamic)
    {
        // Default mass: Rigidbody recomputes it in bindActor. No gravity or
        // kinematic here; they are set by attachRigidbody -> Rigidbody::bindActor.
        PxRigidBodyExt::updateMassAndInertia(*static_cast<PxRigidDynamic*>(actor), 1.0f);
    }

    scene->addActor(*actor);

    auto collider = std::make_shared<BoxCollider>(actor, shape, halfExtents, center);
    // The Transform scale does not fit in the actor's PxTransform: it is baked into
    // the geometry. The shape was created with the configured size, so with
    // scale 1 this touches nothing (setWorldScale exits before setGeometry).
    collider->setWorldScale(scale);
    collider->setManager(this);
    // Registration + initial PxFilterData of its layer (0 by default).
    registerCollider(collider);
    // userData of the actor = base Collider* (explicit upcast to respect
    // any offset of the base); read by the TriggerDispatcher to know
    // who overlapped whom.
    Collider* base = collider.get();
    actor->userData = base;
    return collider;
#else
    (void)worldTransform;
    (void)dynamic;
    auto collider = std::make_shared<BoxCollider>(nullptr, nullptr, halfExtents, center);
    collider->setManager(this);
    registerCollider(collider);
    return collider;
#endif
}

std::shared_ptr<SphereCollider> PhysicsManager::createSphereColliderComponent(
    float radius,
    const glm::vec3& center,
    const glm::mat4& worldTransform,
    bool dynamic)
{
#ifdef DT_PHYSX_ENABLED
    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    const PxTransform pose = poseFromWorld(worldTransform, &scale);

    auto* physics = static_cast<PxPhysics*>(m_physics);
    auto* scene = static_cast<PxScene*>(m_scene);

    // Exclusive material of the collider; see note in createBoxColliderComponent.
    PxMaterial* material = physics->createMaterial(kDefaultStaticFriction,
                                                   kDefaultDynamicFriction,
                                                   kDefaultRestitution);
    physxCheck(material, "PxPhysics::createMaterial(sphere)");

    PxRigidActor* actor = dynamic
        ? static_cast<PxRigidActor*>(physics->createRigidDynamic(pose))
        : static_cast<PxRigidActor*>(physics->createRigidStatic(pose));
    physxCheck(actor, "PxPhysics::createRigidActor(sphere)");

    PxSphereGeometry geometry(radius);
    PxShape* shape = PxRigidActorExt::createExclusiveShape(*actor, geometry, *material);
    physxCheck(shape, "PxRigidActorExt::createExclusiveShape");
    material->release();
    shape->setLocalPose(PxTransform(PxVec3(center.x, center.y, center.z)));

    if (dynamic)
        PxRigidBodyExt::updateMassAndInertia(*static_cast<PxRigidDynamic*>(actor), 1.0f);

    scene->addActor(*actor);

    auto collider = std::make_shared<SphereCollider>(actor, shape, radius, center);
    collider->setWorldScale(scale); // see note in createBoxColliderComponent
    collider->setManager(this);
    registerCollider(collider);
    Collider* base = collider.get();
    actor->userData = base;
    return collider;
#else
    (void)worldTransform;
    (void)dynamic;
    auto collider = std::make_shared<SphereCollider>(nullptr, nullptr, radius, center);
    collider->setManager(this);
    registerCollider(collider);
    return collider;
#endif
}

std::shared_ptr<CapsuleCollider> PhysicsManager::createCapsuleColliderComponent(
    float radius,
    float halfHeight,
    const glm::vec3& center,
    const glm::mat4& worldTransform,
    bool dynamic)
{
#ifdef DT_PHYSX_ENABLED
    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    const PxTransform pose = poseFromWorld(worldTransform, &scale);

    auto* physics = static_cast<PxPhysics*>(m_physics);
    auto* scene = static_cast<PxScene*>(m_scene);

    // Exclusive material of the collider; see note in createBoxColliderComponent.
    PxMaterial* material = physics->createMaterial(kDefaultStaticFriction,
                                                   kDefaultDynamicFriction,
                                                   kDefaultRestitution);
    physxCheck(material, "PxPhysics::createMaterial(capsule)");

    PxRigidActor* actor = dynamic
        ? static_cast<PxRigidActor*>(physics->createRigidDynamic(pose))
        : static_cast<PxRigidActor*>(physics->createRigidStatic(pose));
    physxCheck(actor, "PxPhysics::createRigidActor(capsule)");

    PxCapsuleGeometry geometry(radius, halfHeight);
    PxShape* shape = PxRigidActorExt::createExclusiveShape(*actor, geometry, *material);
    physxCheck(shape, "PxRigidActorExt::createExclusiveShape");
    material->release();
    shape->setLocalPose(PxTransform(PxVec3(center.x, center.y, center.z), axisCorrection()));

    if (dynamic)
        PxRigidBodyExt::updateMassAndInertia(*static_cast<PxRigidDynamic*>(actor), 1.0f);

    scene->addActor(*actor);

    auto collider = std::make_shared<CapsuleCollider>(actor, shape, radius, halfHeight, center);
    collider->setWorldScale(scale); // see note in createBoxColliderComponent
    collider->setManager(this);
    registerCollider(collider);
    Collider* base = collider.get();
    actor->userData = base;
    return collider;
#else
    (void)worldTransform;
    (void)dynamic;
    auto collider = std::make_shared<CapsuleCollider>(nullptr, nullptr, radius, halfHeight, center);
    collider->setManager(this);
    registerCollider(collider);
    return collider;
#endif
}

std::shared_ptr<PlaneCollider> PhysicsManager::createPlaneColliderComponent(
    const glm::vec3& center,
    const glm::mat4& worldTransform)
{
#ifdef DT_PHYSX_ENABLED
    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    const PxTransform pose = poseFromWorld(worldTransform, &scale);

    auto* physics = static_cast<PxPhysics*>(m_physics);
    auto* scene = static_cast<PxScene*>(m_scene);

    // Exclusive material of the collider; see note in createBoxColliderComponent.
    PxMaterial* material = physics->createMaterial(kDefaultStaticFriction,
                                                   kDefaultDynamicFriction,
                                                   kDefaultRestitution);
    physxCheck(material, "PxPhysics::createMaterial(plane)");

    PxRigidDynamic* actor = physics->createRigidDynamic(pose);
    physxCheck(actor, "PxPhysics::createRigidDynamic");

    // The actor must be kinematic BEFORE attaching the plane shape:
    // PhysX rejects (createExclusiveShape returns null) a plane/mesh
    // geometry shape as a simulation shape on a PxRigidDynamic that
    // is not yet kinematic at the moment of the attach.
    actor->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, true);
    actor->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);

    PxPlaneGeometry geometry;
    PxShape* shape = PxRigidActorExt::createExclusiveShape(*actor, geometry, *material);
    physxCheck(shape, "PxRigidActorExt::createExclusiveShape");
    material->release();
    shape->setLocalPose(PxTransform(PxVec3(center.x, center.y, center.z), axisCorrection()));

    // No updateMassAndInertia: a plane has no volume, PhysX cannot
    // compute mass/inertia on that geometry. It is not needed — the actor
    // always ends up kinematic (it is never simulated as a dynamic body).

    scene->addActor(*actor);

    auto collider = std::make_shared<PlaneCollider>(actor, shape, center);
    collider->setManager(this);
    registerCollider(collider);
    Collider* base = collider.get();
    actor->userData = base;
    return collider;
#else
    (void)worldTransform;
    auto collider = std::make_shared<PlaneCollider>(nullptr, nullptr, center);
    collider->setManager(this);
    registerCollider(collider);
    return collider;
#endif
}

void PhysicsManager::attachRigidbody(const std::shared_ptr<Collider>& collider,
                                     const std::shared_ptr<Rigidbody>& rb)
{
    if (!collider || !rb) return;
#ifdef DT_PHYSX_ENABLED
    void* actor = collider->actorHandle();
    // If the actor is still static, rebuild it as dynamic before linking.
    if (actor && !static_cast<PxRigidActor*>(actor)->is<PxRigidDynamic>())
        rebuildActor(collider, /*dynamic=*/true);
    rb->bindActor(collider->actorHandle());
#else
    (void)collider;
    rb->bindActor(nullptr);
#endif
}

void PhysicsManager::detachRigidbody(const std::shared_ptr<Collider>& collider)
{
    if (!collider) return;
#ifdef DT_PHYSX_ENABLED
    void* actor = collider->actorHandle();
    if (actor && static_cast<PxRigidActor*>(actor)->is<PxRigidDynamic>())
        rebuildActor(collider, /*dynamic=*/false);
    // Without a Rigidbody there is nothing to interpolate (the property lives there), and a
    // static that the editor moves through the Transform must not drag the previous
    // pose from when it was dynamic.
    collider->setInterpolate(false);
    // Note: the Rigidbody that pointed to this collider keeps an m_actor that
    // now dangles (the old dynamic was released by rebuildActor). Contract:
    // the callers (editor "Remove Rigidbody", Lua RemoveComponent) release the
    // shared_ptr<Rigidbody> immediately after detach, so nobody
    // dereferences it. If a caller shows up that reuses the Rigidbody, it must
    // re-bind (rb->bindActor(nullptr) or attach to another collider) before using it.
#else
    (void)collider;
#endif
}

#ifdef DT_PHYSX_ENABLED
void* PhysicsManager::rebuildActor(const std::shared_ptr<Collider>& collider, bool dynamic)
{
    auto* physics  = static_cast<PxPhysics*>(m_physics);
    auto* scene    = static_cast<PxScene*>(m_scene);
    auto* oldActor = static_cast<PxRigidActor*>(collider->actorHandle());
    if (!oldActor) return nullptr;

    PxTransform pose = oldActor->getGlobalPose();
    auto* shape = static_cast<PxShape*>(collider->geometryShape());
    bool wasTrigger = collider->isTrigger();

    // PxShape is refcounted: an extra ref is taken so that it survives the detach
    // from the old actor, and it is released after reattaching it to the new one.
    shape->acquireReference();
    oldActor->detachShape(*shape);
    scene->removeActor(*oldActor);
    oldActor->release();

    PxRigidActor* newActor = dynamic
        ? static_cast<PxRigidActor*>(physics->createRigidDynamic(pose))
        : static_cast<PxRigidActor*>(physics->createRigidStatic(pose));
    physxCheck(newActor, "rebuildActor: createRigidActor");
    newActor->attachShape(*shape);
    shape->release();
    if (dynamic)
        PxRigidBodyExt::updateMassAndInertia(*static_cast<PxRigidDynamic*>(newActor), 1.0f);
    scene->addActor(*newActor);
    newActor->userData = collider.get();

    collider->setActorHandle(newActor);
    // The trigger flags live in the shape (which survived), but it is re-ensured.
    if (wasTrigger) collider->applyTriggerFlag(true);
    // The PxFilterData also lives in the shape, so the actor swap keeps
    // it; it is rewritten anyway so that the layer remains the source of
    // truth even if someone touches the shape through another path.
    refreshColliderFilter(collider.get());
    return newActor;
}
#endif

#ifdef DT_PHYSX_ENABLED
bool PhysicsManager::raycast(const PxVec3& origin, const PxVec3& dir, float maxDistance, PxRaycastBuffer& hit)
{
    return static_cast<PxScene*>(m_scene)->raycast(origin, dir, maxDistance, hit);
}

bool PhysicsManager::raycast(const PxVec3& origin, const PxVec3& dir, float maxDistance,
                             PxRaycastBuffer& hit, const PxQueryFilterData& filterData,
                             PxQueryFilterCallback* filterCall)
{
    // Without a scene (the editor outside Play does not call init) there is nothing to
    // query: false instead of a null deref.
    if (!m_scene) return false;
    return static_cast<PxScene*>(m_scene)->raycast(
        origin, dir, maxDistance, hit,
        PxHitFlags(PxHitFlag::ePOSITION | PxHitFlag::eNORMAL),
        filterData, filterCall);
}

bool PhysicsManager::raycastAll(const PxVec3& origin, const PxVec3& dir, float maxDistance,
                                PxRaycastBuffer& hits, const PxQueryFilterData& filterData,
                                PxQueryFilterCallback* filterCall)
{
    if (!m_scene) return false;

    // eNO_BLOCK downgrades to eTOUCH everything the prefilter marks as eBLOCK,
    // so the ray passes through the first hit and keeps collecting the others.
    PxQueryFilterData fd = filterData;
    fd.flags |= PxQueryFlag::eNO_BLOCK;

    const bool any = static_cast<PxScene*>(m_scene)->raycast(
        origin, dir, maxDistance, hits,
        PxHitFlags(PxHitFlag::ePOSITION | PxHitFlag::eNORMAL),
        fd, filterCall);

    // PhysX delivers the touches in the order the spatial sweep finds them,
    // not by distance. It is sorted here (over the buffer's own
    // storage) so that the contract holds for all callers.
    if (hits.nbTouches > 1 && hits.touches)
        std::sort(hits.touches, hits.touches + hits.nbTouches,
                  [](const PxRaycastHit& a, const PxRaycastHit& b) { return a.distance < b.distance; });

    return any;
}

bool PhysicsManager::sphereCast(const PxVec3& origin, const PxVec3& dir, float radius,
                                float maxDistance, PxSweepBuffer& hit,
                                const PxQueryFilterData& filterData,
                                PxQueryFilterCallback* filterCall)
{
    if (!m_scene) return false;

    // The sweep geometry goes in its own pose; the sweep origin is the
    // initial position of the sphere center, not a point on its
    // surface.
    return static_cast<PxScene*>(m_scene)->sweep(
        PxSphereGeometry(radius), PxTransform(origin), dir, maxDistance, hit,
        PxHitFlags(PxHitFlag::ePOSITION | PxHitFlag::eNORMAL),
        filterData, filterCall);
}

bool PhysicsManager::overlapSphere(const PxVec3& center, float radius, PxOverlapBuffer& hits,
                                   const PxQueryFilterData& filterData,
                                   PxQueryFilterCallback* filterCall)
{
    if (!m_scene) return false;

    // Without eNO_BLOCK the prefilter (which returns eBLOCK) would close the query at
    // the first overlap and only one would be reported.
    PxQueryFilterData fd = filterData;
    fd.flags |= PxQueryFlag::eNO_BLOCK;

    return static_cast<PxScene*>(m_scene)->overlap(
        PxSphereGeometry(radius), PxTransform(center), hits, fd, filterCall);
}

bool PhysicsManager::overlapBox(const PxVec3& center, const PxVec3& halfExtents,
                                const PxQuat& rotation, PxOverlapBuffer& hits,
                                const PxQueryFilterData& filterData,
                                PxQueryFilterCallback* filterCall)
{
    if (!m_scene) return false;

    PxQueryFilterData fd = filterData;
    fd.flags |= PxQueryFlag::eNO_BLOCK;

    return static_cast<PxScene*>(m_scene)->overlap(
        PxBoxGeometry(halfExtents), PxTransform(center, rotation), hits, fd, filterCall);
}
#endif

void PhysicsManager::setFixedDeltaTime(float dt)
{
    // A step <= 0 would leave the sub-step loop subtracting 0 from the accumulator, or
    // adding to it: a sure hang. It is ignored and the previous value is kept.
    if (dt <= 0.0f) return;
    m_fixedDeltaTime = dt;
}

void PhysicsManager::setMaxSubSteps(int steps)
{
    // With 0 sub-steps physics would never advance and the accumulator would grow
    // until being discarded every frame: minimum one.
    m_maxSubSteps = (steps < 1) ? 1 : steps;
}

void PhysicsManager::stepSimulation(float dt)
{
#ifdef DT_PHYSX_ENABLED
    // The real frame dt only feeds the accumulator; PxScene::simulate is
    // always passed m_fixedDeltaTime (see why in the header). dt <= 0
    // is ignored: the first frame arrives with 0 (last is initialized == now) and
    // PxScene::simulate requires > 0. In fetchResults the Enter/Exit are dispatched
    // via TriggerDispatcher.
    if (dt > 0.0f) m_accumulator += dt;

    // Margin for floating-point error: 3 * (1.0f/60.0f) accumulated in
    // float ends up a hair below adding 1.0f/60.0f three times, and without a
    // margin that comparison would eat a sub-step (exactly what breaks the
    // determinism we are after).
    constexpr float kEpsilon = 1e-6f;

    int steps = 0;
    while (m_accumulator + kEpsilon >= m_fixedDeltaTime && steps < m_maxSubSteps)
    {
        // Starting pose of the sub-step, for the colliders that interpolate. It goes
        // BEFORE simulate: it is the "old" half of the blend that
        // getWorldTransform will do. No-op on those that do not interpolate (the default).
        for (auto& weak : m_colliders)
            if (auto c = weak.lock()) c->capturePreviousPose();

        static_cast<PxScene*>(m_scene)->simulate(m_fixedDeltaTime);
        static_cast<PxScene*>(m_scene)->fetchResults(true);
        m_accumulator -= m_fixedDeltaTime;
        ++steps;

        // Stay ONCE PER SUB-STEP, inside the loop, so that Enter/Stay/Exit
        // have the same cadence as the simulation (Enter/Exit are emitted by
        // fetchResults of this same sub-step). Accepted consequence: a slow
        // frame emits several Stay in a row.
        dispatchTriggerStay();
    }

    // If after exhausting the sub-steps there is still time left, it is THROWN AWAY instead of being
    // owed: carrying the debt after a stall makes the following frames
    // always go to the maximum sub-steps, take longer, and accumulate even more debt
    // (death spiral).
    if (m_accumulator + kEpsilon >= m_fixedDeltaTime) m_accumulator = 0.0f;

    // How much of the next fixed step real time has already consumed: it is the
    // alpha with which the interpolating colliders blend the previous pose with
    // the current one. It is pushed also when there has been no sub-step —that is
    // precisely where the alpha grows and the body keeps advancing on screen
    // instead of staying stuck until the next step.
    {
        const float alpha = (m_fixedDeltaTime > 0.0f)
            ? glm::clamp(m_accumulator / m_fixedDeltaTime, 0.0f, 1.0f)
            : 1.0f;
        for (auto& weak : m_colliders)
            if (auto c = weak.lock()) c->setInterpolationAlpha(alpha);
    }
#else
    (void)dt;
    dispatchTriggerStay();
#endif
}

void PhysicsManager::dispatchTriggerStay()
{
    // Synthesizes onTriggerStay: PhysX only gives Enter/Exit, so the live
    // triggers are traversed and a Stay is emitted for each current overlap. Expired
    // triggers (destroyed GameObject) are pruned on the fly. Without PhysX the registry
    // is always empty.
    for (auto it = m_triggerColliders.begin(); it != m_triggerColliders.end(); )
    {
        auto collider = it->lock();
        if (!collider) { it = m_triggerColliders.erase(it); continue; }
        collider->dispatchStay();
        ++it;
    }
}

void PhysicsManager::setTrigger(const std::shared_ptr<Collider>& collider, bool enabled)
{
    if (!collider) return;
    collider->applyTriggerFlag(enabled);

    // Prunes expired ones and detects whether it was already registered (a single pass).
    bool present = false;
    for (auto it = m_triggerColliders.begin(); it != m_triggerColliders.end(); )
    {
        auto existing = it->lock();
        if (!existing) { it = m_triggerColliders.erase(it); continue; }
        if (existing == collider) present = true;
        ++it;
    }

    if (enabled && !present)
    {
        m_triggerColliders.push_back(collider);
    }
    else if (!enabled && present)
    {
        m_triggerColliders.erase(
            std::remove_if(m_triggerColliders.begin(), m_triggerColliders.end(),
                [&](const std::weak_ptr<Collider>& w) {
                    auto s = w.lock();
                    return !s || s == collider;
                }),
            m_triggerColliders.end());
    }
}

void PhysicsManager::registerCollider(const std::shared_ptr<Collider>& collider)
{
    if (!collider) return;
    m_colliders.push_back(collider);
    // First dump of the filter: without it the shape would keep the
    // PxFilterData at zero and the layer shader would suppress all its pairs.
    refreshColliderFilter(collider.get());
}

void PhysicsManager::refreshAllColliderFilters()
{
    for (auto it = m_colliders.begin(); it != m_colliders.end(); )
    {
        auto collider = it->lock();
        if (!collider) { it = m_colliders.erase(it); continue; }
        refreshColliderFilter(collider.get());
        ++it;
    }
}

void PhysicsManager::refreshColliderFilter(Collider* collider)
{
#ifdef DT_PHYSX_ENABLED
    if (!collider) return;
    auto* shape = static_cast<PxShape*>(collider->geometryShape());
    if (!shape) return;
    const int layer = collider->getLayer();
    if (!isValidLayer(layer)) return;

    // word2/word3 stay at zero: neither this shader nor the query one uses them.
    PxFilterData fd = shape->getSimulationFilterData();
    fd.word0 = 1u << static_cast<uint32_t>(layer);
    fd.word1 = m_layerMasks[static_cast<size_t>(layer)];
    shape->setSimulationFilterData(fd);
#else
    (void)collider;
#endif
}

void PhysicsManager::setLayerCollision(int a, int b, bool enabled)
{
    if (!isValidLayer(a) || !isValidLayer(b)) return;

    const uint32_t bitA = 1u << static_cast<uint32_t>(a);
    const uint32_t bitB = 1u << static_cast<uint32_t>(b);
    if (enabled)
    {
        m_layerMasks[static_cast<size_t>(a)] |= bitB;
        m_layerMasks[static_cast<size_t>(b)] |= bitA;
    }
    else
    {
        m_layerMasks[static_cast<size_t>(a)] &= ~bitB;
        m_layerMasks[static_cast<size_t>(b)] &= ~bitA;
    }

    // The matrix is global but the filter is COPIED into each shape: without this
    // pass no already-created collider would see the change.
    refreshAllColliderFilters();
}

bool PhysicsManager::getLayerCollision(int a, int b) const
{
    if (!isValidLayer(a) || !isValidLayer(b)) return false;
    return (m_layerMasks[static_cast<size_t>(a)] & (1u << static_cast<uint32_t>(b))) != 0;
}

int PhysicsManager::addLayer(const std::string& name)
{
    if (m_layerCount >= kLayerCount) return -1;
    const int nueva = m_layerCount++;
    // The row was released: it is left as "collides with everything", which is the default of
    // a newly created layer. If it was occupied by a layer deleted earlier, removeLayer already
    // left it that way.
    m_layerNames[static_cast<size_t>(nueva)] = name;
    return nueva;
}

bool PhysicsManager::removeLayer(int layer)
{
    // 0 is the fallback one: if it could be deleted, the reassigned colliders would
    // have nowhere to go.
    if (layer <= 0 || layer >= m_layerCount) return false;

    // Colliders first, with the OLD numbering still in place: those of the layer
    // that dies fall to 0, those above go down one place so that the list
    // leaves no gaps.
    for (auto it = m_colliders.begin(); it != m_colliders.end(); )
    {
        auto collider = it->lock();
        if (!collider) { it = m_colliders.erase(it); continue; }
        const int suya = collider->getLayer();
        if (suya == layer)     collider->setLayer(0);
        else if (suya > layer) collider->setLayer(suya - 1);
        ++it;
    }

    // Matrix: remove row and column 'layer'. It is done on a boolean copy
    // because shifting bits in place overwrites itself.
    const int viejo = m_layerCount;
    bool tabla[kLayerCount][kLayerCount];
    for (int a = 0; a < viejo; ++a)
        for (int b = 0; b < viejo; ++b)
            tabla[a][b] = getLayerCollision(a, b);

    for (int a = 0; a < viejo - 1; ++a)
    {
        const int origenA = (a >= layer) ? a + 1 : a;
        uint32_t  fila    = 0xFFFFFFFFu; // the bits >= new layerCount: unfiltered
        for (int b = 0; b < viejo - 1; ++b)
        {
            const int origenB = (b >= layer) ? b + 1 : b;
            if (!tabla[origenA][origenB]) fila &= ~(1u << static_cast<uint32_t>(b));
        }
        m_layerMasks[static_cast<size_t>(a)] = fila;
    }

    // The last one is released: empty name and no filters, ready for the
    // next addLayer to reuse it.
    m_layerMasks[static_cast<size_t>(viejo - 1)] = 0xFFFFFFFFu;

    for (int i = layer; i < viejo - 1; ++i)
        m_layerNames[static_cast<size_t>(i)] = m_layerNames[static_cast<size_t>(i + 1)];
    m_layerNames[static_cast<size_t>(viejo - 1)].clear();

    m_layerCount = viejo - 1;

    // The colliders already carry their new index, but their word1 came from the OLD
    // matrix: it has to be rewritten with the compacted one.
    refreshAllColliderFilters();
    return true;
}

void PhysicsManager::setLayerName(int layer, const std::string& name)
{
    if (!isValidLayer(layer)) return;
    m_layerNames[static_cast<size_t>(layer)] = name;
}

std::string PhysicsManager::getLayerName(int layer) const
{
    if (!isValidLayer(layer)) return std::string();
    return m_layerNames[static_cast<size_t>(layer)];
}

uint32_t PhysicsManager::layerMask(int layer) const
{
    if (!isValidLayer(layer)) return 0u;
    return m_layerMasks[static_cast<size_t>(layer)];
}

void PhysicsManager::onColliderDestroyed(Collider* collider)
{
    // Pruning of the layer registry: the one that dies no longer blocks its weak_ptr
    // (refcount 0 during ~Collider), so it goes out here along with any other
    // expired one. Without this the vector would grow endlessly.
    m_colliders.erase(
        std::remove_if(m_colliders.begin(), m_colliders.end(),
                       [](const std::weak_ptr<Collider>& w) { return w.expired(); }),
        m_colliders.end());

    // The collider that dies may be in the overlaps of other triggers:
    // it is purged from all of them so as not to leave dangling pointers in the next Stay.
    // If the one that dies was itself a trigger, its weak_ptr no longer blocks
    // (refcount 0 during ~Collider) and it is pruned here.
    for (auto it = m_triggerColliders.begin(); it != m_triggerColliders.end(); )
    {
        auto trigger = it->lock();
        if (!trigger) { it = m_triggerColliders.erase(it); continue; }
        trigger->removeOverlapSilent(collider);
        ++it;
    }
}

} // namespace DonTopo
