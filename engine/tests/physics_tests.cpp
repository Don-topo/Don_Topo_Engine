// Headless test of the physics core (no GUI). Plain main + asserts, no
// framework, consistent with a C++/CMake/Ninja project without test infrastructure.
//
// PhysX only supports ONE PxFoundation per process (creating it twice, even if it is
// released in between, crashes). That is why a single PhysicsManager is shared
// among all the tests: each test creates its colliders as locals, which on
// leaving the function release their actor from the scene, so there is only one body
// alive at a time and the tests do not interfere with each other.
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Physics/Rigidbody.h"
#include "DonTopo/Physics/Colliders/BoxCollider.h"
#include "DonTopo/Physics/Colliders/SphereCollider.h"
#include "DonTopo/Physics/Colliders/CapsuleCollider.h"
#include "DonTopo/Physics/Colliders/PlaneCollider.h"
#include "DonTopo/Physics/Colliders/Collider.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Advances the simulation n steps of dt seconds.
static void step(PhysicsManager& pm, int n, float dt) { for (int i = 0; i < n; ++i) pm.stepSimulation(dt); }

// A dynamic body with gravity must fall (Y decreases).
static void test_free_fall(PhysicsManager& pm)
{
    auto rb = std::make_shared<Rigidbody>();
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    float y0 = col->getWorldTransform()[3].y;
    step(pm, 30, 1.0f / 60.0f);
    float y1 = col->getWorldTransform()[3].y;
    CHECK(y1 < y0 - 1.0f);
}

// Kinematic no cae.
static void test_kinematic_no_fall(PhysicsManager& pm)
{
    auto rb = std::make_shared<Rigidbody>();
    rb->setIsKinematic(true);
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    float y0 = col->getWorldTransform()[3].y;
    step(pm, 30, 1.0f / 60.0f);
    float y1 = col->getWorldTransform()[3].y;
    CHECK(std::fabs(y1 - y0) < 0.001f);
}

// Freeze-Y keeps Y even with gravity.
static void test_freeze_position_y(PhysicsManager& pm)
{
    auto rb = std::make_shared<Rigidbody>();
    rb->setConstraints(RB_FreezePositionY);
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    float y0 = col->getWorldTransform()[3].y;
    step(pm, 30, 1.0f / 60.0f);
    float y1 = col->getWorldTransform()[3].y;
    CHECK(std::fabs(y1 - y0) < 0.001f);
}

// addImpulse changes the velocity in the expected direction.
static void test_add_impulse(PhysicsManager& pm)
{
    auto rb = std::make_shared<Rigidbody>();
    rb->setUseGravity(false);
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    rb->addImpulse(glm::vec3(100.0f, 0.0f, 0.0f));
    step(pm, 1, 1.0f / 60.0f);
    CHECK(rb->getVelocity().x > 0.0f);
}

// --- ForceMode ---------------------------------------------------------------
//
// Applies the SAME vector with two different modes on two identical bodies
// (mass 1, no gravity, no drag) and returns the velocity after ONE step.
// Force integrates over the step: v = F*dt/m. VelocityChange writes the
// velocity at once: v = F. With dt = 1/60 that is a 60x difference, so a
// badly done mapping cannot slip through by rounding.
static float velocityAfterOneStep(PhysicsManager& pm, ForceMode mode)
{
    auto rb = std::make_shared<Rigidbody>();
    rb->setUseGravity(false);
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    CHECK(std::fabs(rb->getMass() - 1.0f) < 1e-6f); // the formula below assumes mass 1
    rb->addForce(glm::vec3(100.0f, 0.0f, 0.0f), mode);
    step(pm, 1, 1.0f / 60.0f);
    return rb->getVelocity().x;
}

// Force vs VelocityChange with the same vector: the resulting velocity differs
// by the expected factor dt.
static void test_force_mode_changes_magnitude(PhysicsManager& pm)
{
    const float dt = 1.0f / 60.0f;
    float vForce  = velocityAfterOneStep(pm, ForceMode::Force);
    float vVelCh  = velocityAfterOneStep(pm, ForceMode::VelocityChange);
    std::printf("  ForceMode: Force -> %.4f | VelocityChange -> %.4f\n", vForce, vVelCh);
    CHECK(std::fabs(vForce - 100.0f * dt) < 0.01f); // F*dt/m
    CHECK(std::fabs(vVelCh - 100.0f)      < 0.01f); // v at once
    CHECK(vVelCh > vForce * 10.0f);                 // unmistakably different
}

// The default mode is Force: addForce(v) without a mode == addForce(v, Force).
// Without this, a default changed by oversight would silently break all the
// three-argument code that already exists.
static void test_force_mode_default_is_force(PhysicsManager& pm)
{
    // Separated in Z so that they do not push each other: two boxes at the same
    // origin depenetrate and contaminate the measured velocity.
    auto rbA = std::make_shared<Rigidbody>();
    rbA->setUseGravity(false);
    auto colA = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(colA, rbA);
    rbA->addForce(glm::vec3(100.0f, 0.0f, 0.0f)); // without a mode

    auto rbB = std::make_shared<Rigidbody>();
    rbB->setUseGravity(false);
    auto colB = pm.createBoxColliderComponent(
        glm::vec3(1.0f), glm::vec3(0.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 100.0f)), /*dynamic=*/true);
    pm.attachRigidbody(colB, rbB);
    rbB->addForce(glm::vec3(100.0f, 0.0f, 0.0f), ForceMode::Force);

    step(pm, 1, 1.0f / 60.0f);
    CHECK(rbA->getVelocity().x > 0.0f); // so that they are not equal just by both being zero
    CHECK(std::fabs(rbA->getVelocity().x - rbB->getVelocity().x) < 1e-5f);
}

// Rebuild static <-> dynamic preserves the shape. After detach the collider is still
// alive and static (it does not fall) and its geometry is left intact.
static void test_rebuild_preserves_shape(PhysicsManager& pm)
{
    auto rb = std::make_shared<Rigidbody>();
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    pm.detachRigidbody(col); // goes back to static
    float y0 = col->getWorldTransform()[3].y;
    step(pm, 30, 1.0f / 60.0f);
    float y1 = col->getWorldTransform()[3].y;
    CHECK(std::fabs(y1 - y0) < 0.001f);       // static no cae
    CHECK(col->getHalfExtents() == glm::vec3(1.0f)); // geometry intact
}

// --- Triggers: the "at least one Rigidbody" rule ---------------------------
//
// PhysX does NOT generate pairs for two static actors: they cannot move relative
// to each other, so it does not even call the filter shader. A collider without a
// Rigidbody is a PxRigidStatic since the dynamics were separated from the Collider, so
// a trigger without a Rigidbody does NOT detect objects that do not have one either.
//
// It is the same rule as Unity ("at least one of the two needs a Rigidbody"),
// but here there was nothing that said it: Is Trigger was checked and nothing
// happened, with no diagnostic. These tests pin it down in executable code, and along the way
// measure which combinations really work instead of assuming it.

// Test listener: counts the Enter/Exit events it receives.
struct CountingListener : ITriggerListener {
    int enters = 0;
    int exits  = 0;
    void onTriggerEnter(const TriggerEvent&) override { ++enters; }
    void onTriggerExit (const TriggerEvent&) override { ++exits;  }
};

// Sets up an OVERLAPPING trigger and object (same origin) and simulates. Returns the Enter
// events that the trigger received. withRbTrigger/withRbOther decide whether each side carries a
// Rigidbody, which is the only thing that changes between the three tests below.
static int entersWith(PhysicsManager& pm, bool withRbTrigger, bool withRbOther)
{
    auto trigger = pm.createBoxColliderComponent(glm::vec3(10.0f), glm::vec3(0.0f),
                                                  glm::mat4(1.0f), withRbTrigger);
    auto rbT = std::make_shared<Rigidbody>();
    if (withRbTrigger)
    {
        pm.attachRigidbody(trigger, rbT);
        rbT->setIsKinematic(true);   // so that it does not fall during the test
    }
    pm.setTrigger(trigger, true);

    auto other = pm.createBoxColliderComponent(glm::vec3(5.0f), glm::vec3(0.0f),
                                                glm::mat4(1.0f), withRbOther);
    auto rbO = std::make_shared<Rigidbody>();
    if (withRbOther)
    {
        pm.attachRigidbody(other, rbO);
        rbO->setIsKinematic(true);
    }

    CountingListener listener;
    trigger->addListener(&listener);
    step(pm, 5, 1.0f / 60.0f);
    trigger->removeListener(&listener);
    pm.setTrigger(trigger, false);
    return listener.enters;
}

// THE CASE THAT BROKE: both without a Rigidbody, that is, both PxRigidStatic.
// PhysX does not form the pair and the trigger is not aware of anything.
static void test_trigger_needs_a_rigidbody(PhysicsManager& pm)
{
    CHECK(entersWith(pm, /*trigger*/false, /*other*/false) == 0);
}

// It is enough for THE OBJECT that enters to have one: static vs dynamic does form a pair.
static void test_trigger_fires_when_other_has_rigidbody(PhysicsManager& pm)
{
    CHECK(entersWith(pm, /*trigger*/false, /*other*/true) > 0);
}

// Or for the trigger itself to have one, which is the symmetric case.
static void test_trigger_fires_when_trigger_has_rigidbody(PhysicsManager& pm)
{
    CHECK(entersWith(pm, /*trigger*/true, /*other*/false) > 0);
}

// --- Physics material per collider ----------------------------------------
//
// Each collider has its own PxMaterial (before, they all shared one), so
// two spheres with different restitution must bounce differently. The PhysX
// combine mode is eAVERAGE: the effective restitution of the contact is the
// average between that of the sphere and that of the plane (0.1 by default).

// Tracks the lowest point reached and the maximum height AFTER that minimum,
// which is exactly the apex of the first bounce.
struct BounceTrack {
    float minY = 1e9f;
    float apex = -1e9f;
    void feed(float y) { if (y < minY) { minY = y; apex = y; } else if (y > apex) apex = y; }
    float rise() const { return apex - minY; }
};

// Drops two spheres (restitution 0.0 and 0.9) onto a PlaneCollider and
// returns how much each one bounced. They are separated in X so that they hit only
// the plane and not each other.
//
// createDynamic=false creates the spheres as static and promotes them with
// attachRigidbody, which goes through rebuildActor: that path re-attaches the SAME
// shape to the new actor, and with it its PxMaterial. It is the check that the
// material survives the rebuild LOOKING AT THE SIMULATION, not at the C++ copy
// returned by the getters.
static void dropTwoSpheres(PhysicsManager& pm, bool createDynamic,
                            float& riseDead, float& riseBouncy)
{
    auto plane = pm.createPlaneColliderComponent(glm::vec3(0.0f), glm::mat4(1.0f));

    auto rbDead   = std::make_shared<Rigidbody>();
    auto rbBouncy = std::make_shared<Rigidbody>();

    auto dead = pm.createSphereColliderComponent(
        25.0f, glm::vec3(0.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(-200.0f, 300.0f, 0.0f)), createDynamic);
    dead->setBounciness(0.0f);
    pm.attachRigidbody(dead, rbDead);

    auto bouncy = pm.createSphereColliderComponent(
        25.0f, glm::vec3(0.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(200.0f, 300.0f, 0.0f)), createDynamic);
    bouncy->setBounciness(0.9f);
    pm.attachRigidbody(bouncy, rbBouncy);

    BounceTrack tDead, tBouncy;
    for (int i = 0; i < 240; ++i)
    {
        pm.stepSimulation(1.0f / 60.0f);
        tDead.feed(dead->getWorldTransform()[3].y);
        tBouncy.feed(bouncy->getWorldTransform()[3].y);
    }
    riseDead   = tDead.rise();
    riseBouncy = tBouncy.rise();
}

// Two dynamic spheres with different restitution bounce to different heights.
static void test_restitution_changes_bounce_height(PhysicsManager& pm)
{
    float riseDead = 0.0f, riseBouncy = 0.0f;
    dropTwoSpheres(pm, /*createDynamic=*/true, riseDead, riseBouncy);
    std::printf("  rebote: restitution 0.0 -> %.2f | restitution 0.9 -> %.2f\n", riseDead, riseBouncy);
    // The "dead" one does not bounce to zero: the combine mode is eAVERAGE, so
    // against the plane (0.1 by default) it is left with an effective restitution of
    // 0.05. What is pinned down here is that the elastic one bounces an order of magnitude
    // more, not an exact absolute value.
    CHECK(riseBouncy > 20.0f);             // the elastic one really bounces
    CHECK(riseDead < 15.0f);               // the dead one barely lifts off
    CHECK(riseBouncy > riseDead * 3.0f);   // and the difference is unmistakable
}

// setFriction/setBounciness return what was written and survive the Rigidbody
// attach/detach (which rebuilds the actor internally).
static void test_material_survives_rebuild(PhysicsManager& pm)
{
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f),
                                              glm::mat4(1.0f), /*dynamic=*/false);
    // Defaults when freshly created: the same ones the old global PxMaterial had,
    // that is, what a saved scene without these fields sees.
    CHECK(std::fabs(col->getStaticFriction()  - 0.5f) < 1e-6f);
    CHECK(std::fabs(col->getDynamicFriction() - 0.5f) < 1e-6f);
    CHECK(std::fabs(col->getBounciness()      - 0.1f) < 1e-6f);

    col->setFriction(0.31f, 0.22f);
    col->setBounciness(0.77f);
    CHECK(std::fabs(col->getStaticFriction()  - 0.31f) < 1e-6f);
    CHECK(std::fabs(col->getDynamicFriction() - 0.22f) < 1e-6f);
    CHECK(std::fabs(col->getBounciness()      - 0.77f) < 1e-6f);

    auto rb = std::make_shared<Rigidbody>();
    pm.attachRigidbody(col, rb);   // static -> dynamic (rebuildActor)
    CHECK(std::fabs(col->getStaticFriction()  - 0.31f) < 1e-6f);
    CHECK(std::fabs(col->getDynamicFriction() - 0.22f) < 1e-6f);
    CHECK(std::fabs(col->getBounciness()      - 0.77f) < 1e-6f);

    pm.detachRigidbody(col);       // dynamic -> static (rebuildActor again)
    CHECK(std::fabs(col->getStaticFriction()  - 0.31f) < 1e-6f);
    CHECK(std::fabs(col->getDynamicFriction() - 0.22f) < 1e-6f);
    CHECK(std::fabs(col->getBounciness()      - 0.77f) < 1e-6f);

    // The getters only read the C++ copy: if rebuildActor lost the
    // PxMaterial, they would keep saying the right thing. This measures it in the
    // simulation: the bounciness is written BEFORE the rebuild.
    float riseDead = 0.0f, riseBouncy = 0.0f;
    dropTwoSpheres(pm, /*createDynamic=*/false, riseDead, riseBouncy);
    std::printf("  rebote tras rebuildActor: restitution 0.0 -> %.2f | restitution 0.9 -> %.2f\n",
                riseDead, riseBouncy);
    CHECK(riseBouncy > 20.0f);
    CHECK(riseDead < 15.0f);
    CHECK(riseBouncy > riseDead * 3.0f);
}

// --- Fixed step with accumulator -----------------------------------------------

// Leaves the accumulator of the shared PhysicsManager at 0: a huge dt exhausts the
// sub-steps and the remainder is discarded, so each test starts the same even if the
// previous one left a remainder.
static void flushAccumulator(PhysicsManager& pm) { pm.stepSimulation(1000.0f); }

// Free fall of a new body after `calls` calls of `dt` seconds.
// The collider is local: on exit it takes its actor with it, it does not interfere with the rest.
static float fallDistance(PhysicsManager& pm, int calls, float dt)
{
    auto rb  = std::make_shared<Rigidbody>();
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    float y0 = col->getWorldTransform()[3].y;
    for (int i = 0; i < calls; ++i) pm.stepSimulation(dt);
    return y0 - col->getWorldTransform()[3].y;
}

// One frame of 3 fixed steps advances the same as 3 frames of one step.
static void test_fixed_step_is_framerate_independent(PhysicsManager& pm)
{
    // The real caller passes the frame's measured dt, not an exact float multiple of
    // the fixed step: 3.0f/60.0f ends up a hair BELOW adding
    // 1.0f/60.0f three times, and without a rounding margin the loop would swallow the
    // third sub-step. That is why the dt below is written like this and not as 3*fixed.
    CHECK(std::fabs(pm.getFixedDeltaTime() - 1.0f / 60.0f) < 1e-9f);
    const float fixed = 1.0f / 60.0f;
    flushAccumulator(pm);
    float three = fallDistance(pm, 3, fixed);
    flushAccumulator(pm);
    float once  = fallDistance(pm, 1, 3.0f / 60.0f);
    std::printf("  determinismo: 3x1 paso -> %.6f | 1x3 pasos -> %.6f\n", three, once);
    CHECK(std::fabs(three - once) < 1e-4f);
    CHECK(three > 0.0f);
}

// A giant dt does not simulate more than maxSubSteps sub-steps (nor hang).
static void test_giant_dt_clamped_to_max_substeps(PhysicsManager& pm)
{
    const float fixed = pm.getFixedDeltaTime();
    const int   maxSs = pm.getMaxSubSteps();
    flushAccumulator(pm);
    float reference = fallDistance(pm, maxSs, fixed);  // exactly maxSubSteps
    flushAccumulator(pm);
    float giant     = fallDistance(pm, 1, 5.0f);       // 300 sub-steps if there is no clamp
    std::printf("  dt gigante: %d pasos -> %.6f | dt=5s -> %.6f\n", maxSs, reference, giant);
    CHECK(std::fabs(giant - reference) < 1e-4f);
}

// After exhausting the sub-steps the remainder is thrown away: the next frame goes back to
// costing a single sub-step, not another whole batch of pending debt.
static void test_giant_dt_leaves_no_debt(PhysicsManager& pm)
{
    const float fixed = pm.getFixedDeltaTime();
    const int   maxSs = pm.getMaxSubSteps();
    flushAccumulator(pm);
    float reference = fallDistance(pm, maxSs + 1, fixed);  // maxSubSteps + 1 sub-steps

    flushAccumulator(pm);
    auto rb  = std::make_shared<Rigidbody>();
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    float y0 = col->getWorldTransform()[3].y;
    pm.stepSimulation(5.0f);    // maxSubSteps sub-steps, remainder discarded
    pm.stepSimulation(fixed);   // with carried debt it would be maxSubSteps more
    float measured = y0 - col->getWorldTransform()[3].y;
    std::printf("  sin deuda: %d pasos -> %.6f | 5s + 1 paso -> %.6f\n", maxSs + 1, reference, measured);
    CHECK(std::fabs(measured - reference) < 1e-4f);
}

// The setters reject what would hang the sub-step loop.
static void test_fixed_step_setters_reject_bad_values(PhysicsManager& pm)
{
    const float originalDt    = pm.getFixedDeltaTime();
    const int   originalSteps = pm.getMaxSubSteps();

    pm.setFixedDeltaTime(0.0f);
    CHECK(pm.getFixedDeltaTime() == originalDt);
    pm.setFixedDeltaTime(-1.0f);
    CHECK(pm.getFixedDeltaTime() == originalDt);
    pm.setFixedDeltaTime(1.0f / 120.0f);            // valid: it does change
    CHECK(std::fabs(pm.getFixedDeltaTime() - 1.0f / 120.0f) < 1e-9f);
    pm.setFixedDeltaTime(originalDt);

    pm.setMaxSubSteps(0);
    CHECK(pm.getMaxSubSteps() == 1);
    pm.setMaxSubSteps(-5);
    CHECK(pm.getMaxSubSteps() == 1);
    pm.setMaxSubSteps(originalSteps);
    CHECK(pm.getMaxSubSteps() == originalSteps);
}

// ---------------------------------------------------------------------------
// Sweeps and overlaps (Physics.SphereCast / OverlapSphere / OverlapBox
// underneath). Read-only queries: there is no need to simulate even one step, it is enough
// for the actors to be in the scene.
// ---------------------------------------------------------------------------

// Prefilter that accepts EVERYTHING returning eBLOCK: exactly what the
// RaycastFilter of the Lua binding does when the shape passes the filters. Without it the
// query would not exercise the eNO_BLOCK that overlapSphere/overlapBox add (without a
// prefilter, PhysX already reports all overlaps as touch and the test would pass
// even if that line did not exist).
class AcceptAllBlocking : public physx::PxQueryFilterCallback
{
public:
    physx::PxQueryHitType::Enum preFilter(const physx::PxFilterData&,
                                          const physx::PxShape*,
                                          const physx::PxRigidActor*,
                                          physx::PxHitFlags&) override
    {
        return physx::PxQueryHitType::eBLOCK;
    }
    physx::PxQueryHitType::Enum postFilter(const physx::PxFilterData&,
                                           const physx::PxQueryHit&,
                                           const physx::PxShape*,
                                           const physx::PxRigidActor*) override
    {
        return physx::PxQueryHitType::eBLOCK;
    }
};

static AcceptAllBlocking g_queryFilter;

// Same flags that the binding sets up: prefilter + static + dynamic.
static physx::PxQueryFilterData defaultQueryFilter()
{
    physx::PxQueryFilterData fd;
    fd.flags = physx::PxQueryFlag::ePREFILTER | physx::PxQueryFlag::eSTATIC |
               physx::PxQueryFlag::eDYNAMIC;
    return fd;
}

// A zero-thickness ray passes 120 away from a sphere of radius 100 (misses by 20),
// but sweeping a sphere of radius 50 does hit it. It pins down that the radius reaches
// PhysX: with radius 0 the sweep would behave like the raycast and would
// hit nothing.
static void test_sphere_cast_uses_the_radius(PhysicsManager& pm)
{
    auto diana = pm.createSphereColliderComponent(
        100.0f, glm::vec3(0.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 500.0f)), /*dynamic=*/false);

    physx::PxSweepBuffer gordo, fino;
    const bool tocaGordo = pm.sphereCast(physx::PxVec3(0.0f, 120.0f, 0.0f),
                                          physx::PxVec3(0.0f, 0.0f, 1.0f),
                                          50.0f, 1000.0f, gordo, defaultQueryFilter(), &g_queryFilter);
    const bool tocaFino  = pm.sphereCast(physx::PxVec3(0.0f, 120.0f, 0.0f),
                                          physx::PxVec3(0.0f, 0.0f, 1.0f),
                                          1.0f, 1000.0f, fino, defaultQueryFilter(), &g_queryFilter);
    CHECK(tocaGordo == true);
    CHECK(tocaFino == false);
    // Contact in front of the sphere: less than the 500 of the center.
    if (tocaGordo) CHECK(gordo.block.distance > 0.0f && gordo.block.distance < 500.0f);
}

// Sweep that passes by: same sphere, opposite direction, and another one that
// falls short because of maxDistance.
static void test_sphere_cast_miss(PhysicsManager& pm)
{
    auto diana = pm.createSphereColliderComponent(
        100.0f, glm::vec3(0.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 500.0f)), /*dynamic=*/false);

    physx::PxSweepBuffer alReves, corto, directo;
    CHECK(pm.sphereCast(physx::PxVec3(0.0f), physx::PxVec3(0.0f, 0.0f, -1.0f),
                        50.0f, 1000.0f, alReves, defaultQueryFilter(), &g_queryFilter) == false);
    CHECK(pm.sphereCast(physx::PxVec3(0.0f), physx::PxVec3(0.0f, 0.0f, 1.0f),
                        50.0f, 100.0f, corto, defaultQueryFilter(), &g_queryFilter) == false);
    // And the positive control case: head-on it does hit, at 500-100-50 = 350.
    CHECK(pm.sphereCast(physx::PxVec3(0.0f), physx::PxVec3(0.0f, 0.0f, 1.0f),
                        50.0f, 1000.0f, directo, defaultQueryFilter(), &g_queryFilter) == true);
    CHECK(std::fabs(directo.block.distance - 350.0f) < 1.0f);
}

// 0, 1 and N overlaps with the same scene, only changing the radius. The N case is
// the one that breaks if eNO_BLOCK is missing: without it the query closes on the first
// overlap and returns 1.
static void test_overlap_sphere_counts(PhysicsManager& pm)
{
    auto a = pm.createSphereColliderComponent(
        50.0f, glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/false);
    auto b = pm.createSphereColliderComponent(
        50.0f, glm::vec3(0.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 300.0f)), /*dynamic=*/false);
    auto c = pm.createSphereColliderComponent(
        50.0f, glm::vec3(0.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 600.0f)), /*dynamic=*/false);

    physx::PxOverlapBufferN<16> vacio, uno, tres;
    pm.overlapSphere(physx::PxVec3(0.0f, 5000.0f, 0.0f), 10.0f, vacio, defaultQueryFilter(), &g_queryFilter);
    pm.overlapSphere(physx::PxVec3(0.0f), 10.0f, uno, defaultQueryFilter(), &g_queryFilter);
    pm.overlapSphere(physx::PxVec3(0.0f, 0.0f, 300.0f), 400.0f, tres, defaultQueryFilter(), &g_queryFilter);

    CHECK(vacio.getNbTouches() == 0);
    CHECK(uno.getNbTouches() == 1);
    CHECK(tres.getNbTouches() == 3);
}

// The overlap box is ORIENTED: a box long in Z sees the target at z=300,
// and the same box rotated 90° about Y (long in X) no longer does. Without passing the rotation
// to PhysX, both queries would give 1.
static void test_overlap_box_honours_rotation(PhysicsManager& pm)
{
    auto diana = pm.createSphereColliderComponent(
        10.0f, glm::vec3(0.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 300.0f)), /*dynamic=*/false);

    const physx::PxVec3 half(50.0f, 50.0f, 1000.0f);
    physx::PxOverlapBufferN<16> alineada, girada;
    pm.overlapBox(physx::PxVec3(0.0f), half, physx::PxQuat(physx::PxIdentity),
                  alineada, defaultQueryFilter(), &g_queryFilter);
    pm.overlapBox(physx::PxVec3(0.0f), half,
                  physx::PxQuat(physx::PxHalfPi, physx::PxVec3(0.0f, 1.0f, 0.0f)),
                  girada, defaultQueryFilter(), &g_queryFilter);

    CHECK(alineada.getNbTouches() == 1);
    CHECK(girada.getNbTouches() == 0);
}

// --- Collision layers -------------------------------------------------------
//
// The matrix starts ENTIRELY at true and the PhysicsManager is shared: each test
// touches only its cell and restores it on exit, otherwise the later tests
// would simulate with another matrix. Layers != 0 are used on purpose, so that the
// colliders of the rest of the tests (all on layer 0) are not affected.

// Sets up a static floor on layer 'capaSuelo' and a dynamic box on
// 'capaCaja' at a height of 1000. Returns both, alive, so the
// matrix can be touched with the scene ALREADY built.
struct EscenaDeCapas {
    std::shared_ptr<BoxCollider> suelo;
    std::shared_ptr<BoxCollider> caja;
    std::shared_ptr<Rigidbody>   rb;
};

static EscenaDeCapas montarEscenaDeCapas(PhysicsManager& pm, int capaSuelo, int capaCaja)
{
    flushAccumulator(pm);
    EscenaDeCapas e;
    e.suelo = pm.createBoxColliderComponent(glm::vec3(500.0f, 10.0f, 500.0f), glm::vec3(0.0f),
                                            glm::mat4(1.0f), /*dynamic=*/false);
    e.suelo->setLayer(capaSuelo);

    e.caja = pm.createBoxColliderComponent(
        glm::vec3(10.0f), glm::vec3(0.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1000.0f, 0.0f)), /*dynamic=*/true);
    e.caja->setLayer(capaCaja);
    e.rb = std::make_shared<Rigidbody>();
    pm.attachRigidbody(e.caja, e.rb);
    return e;
}

// With cell (1,2) turned off the pair is not even formed: the box goes through the floor.
// The same scene with the default matrix leaves it resting on top (control:
// test_layer_reenabled_at_runtime_makes_contact).
static void test_layer_off_suppresses_contact(PhysicsManager& pm)
{
    pm.setLayerCollision(1, 2, false);
    {
        EscenaDeCapas e = montarEscenaDeCapas(pm, /*suelo=*/1, /*caja=*/2);
        step(pm, 240, 1.0f / 60.0f);
        const float y = e.caja->getWorldTransform()[3].y;
        std::printf("  capas (1,2) off: y final = %.2f\n", y);
        CHECK(y < -100.0f); // it has gone past the floor (which would rest at 20)
    }
    pm.setLayerCollision(1, 2, true);
}

// Re-enabling the cell AT RUNTIME, with the shapes already created and falling, has to
// bring back the contact: it is what pins down that setLayerCollision rewrites the
// PxFilterData of the live colliders and not only the matrix.
static void test_layer_reenabled_at_runtime_makes_contact(PhysicsManager& pm)
{
    pm.setLayerCollision(1, 2, false);
    {
        EscenaDeCapas e = montarEscenaDeCapas(pm, /*suelo=*/1, /*caja=*/2);

        // 0.2 s with the layer filtered: it falls ~20, still VERY far above the floor.
        step(pm, 12, 1.0f / 60.0f);
        const float yAntes = e.caja->getWorldTransform()[3].y;
        CHECK(yAntes > 500.0f);

        pm.setLayerCollision(1, 2, true);
        step(pm, 240, 1.0f / 60.0f);
        const float yDespues = e.caja->getWorldTransform()[3].y;
        std::printf("  capas (1,2) on en runtime: y = %.2f -> %.2f\n", yAntes, yDespues);
        CHECK(yDespues > 15.0f);  // rests on the floor (10 of floor + 10 of half box)
        CHECK(yDespues < 25.0f);
    }
    pm.setLayerCollision(1, 2, true);
}

// A trigger does not see what the matrix filters either: the layer check goes
// BEFORE the trigger branch of the filter shader.
static int entersWithLayers(PhysicsManager& pm, int capaTrigger, int capaOtro)
{
    auto trigger = pm.createBoxColliderComponent(glm::vec3(10.0f), glm::vec3(0.0f),
                                                 glm::mat4(1.0f), /*dynamic=*/false);
    trigger->setLayer(capaTrigger);
    pm.setTrigger(trigger, true);

    // The one that enters carries a kinematic Rigidbody: it is the combination that DOES fire
    // with the default matrix (see test_trigger_fires_when_other_has_rigidbody).
    auto other = pm.createBoxColliderComponent(glm::vec3(5.0f), glm::vec3(0.0f),
                                               glm::mat4(1.0f), /*dynamic=*/true);
    other->setLayer(capaOtro);
    auto rbO = std::make_shared<Rigidbody>();
    pm.attachRigidbody(other, rbO);
    rbO->setIsKinematic(true);

    CountingListener listener;
    trigger->addListener(&listener);
    step(pm, 5, 1.0f / 60.0f);
    trigger->removeListener(&listener);
    pm.setTrigger(trigger, false);
    return listener.enters;
}

static void test_trigger_respects_layer_matrix(PhysicsManager& pm)
{
    pm.setLayerCollision(3, 4, false);
    const int filtrado = entersWithLayers(pm, /*trigger=*/3, /*other=*/4);
    pm.setLayerCollision(3, 4, true);
    const int abierto = entersWithLayers(pm, /*trigger=*/3, /*other=*/4);

    std::printf("  trigger con capas: filtrado -> %d enters | abierto -> %d enters\n",
                filtrado, abierto);
    CHECK(filtrado == 0);
    CHECK(abierto > 0);
}

// The default matrix filters nothing and invalid indices do not blow up.
static void test_layer_matrix_defaults_and_bounds(PhysicsManager& pm)
{
    for (int i = 0; i < PhysicsManager::kLayerCount; ++i)
        CHECK(pm.layerMask(i) == 0xFFFFFFFFu);
    CHECK(pm.getLayerCollision(0, 31));
    CHECK(pm.getLayerName(0) == "Default");
    CHECK(pm.getLayerName(1).empty());

    // Out of range: no-op, without overflowing the matrix or the shift.
    pm.setLayerCollision(-1, 0, false);
    pm.setLayerCollision(0, 32, false);
    CHECK(pm.layerMask(0) == 0xFFFFFFFFu);
    CHECK(!pm.getLayerCollision(-1, 0));
    CHECK(pm.layerMask(32) == 0u);
    CHECK(pm.getLayerName(32).empty());

    // And an invalid layer on the collider keeps the one it had.
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f),
                                             glm::mat4(1.0f), /*dynamic=*/false);
    col->setLayer(7);
    col->setLayer(99);
    col->setLayer(-3);
    CHECK(col->getLayer() == 7);
}

// Adding and removing layers. Deleting COMPACTS: it reassigns the colliders and shifts the
// matrix, which is what keeps the list from having gaps.
static void test_add_and_remove_layers(PhysicsManager& pm)
{
    CHECK(pm.layerCount() == 1);          // only "Default" at startup
    CHECK(pm.addLayer("Suelo")   == 1);
    CHECK(pm.addLayer("Enemigos") == 2);
    CHECK(pm.addLayer("Balas")   == 3);
    CHECK(pm.layerCount() == 4);

    // Layer 0 is not deleted; neither is an index that does not exist.
    CHECK(!pm.removeLayer(0));
    CHECK(!pm.removeLayer(4));

    // Matrix: (1,3) is turned off, which after deleting 2 will become (1,2).
    pm.setLayerCollision(1, 3, false);

    auto enLaQueMuere = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f),
                                                      glm::mat4(1.0f), /*dynamic=*/false);
    enLaQueMuere->setLayer(2);
    auto porEncima = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f),
                                                   glm::mat4(1.0f), /*dynamic=*/false);
    porEncima->setLayer(3);

    CHECK(pm.removeLayer(2));
    CHECK(pm.layerCount() == 3);

    // The colliders: the one on the dead layer falls to 0, the one above moves down one.
    CHECK(enLaQueMuere->getLayer() == 0);
    CHECK(porEncima->getLayer() == 2);

    // Shifted names and the freed slot, empty.
    CHECK(pm.getLayerName(1) == "Suelo");
    CHECK(pm.getLayerName(2) == "Balas");
    CHECK(pm.getLayerName(3).empty());

    // The turned-off cell travels with the layer: it was (1,3), now it is (1,2).
    CHECK(!pm.getLayerCollision(1, 2));
    CHECK(pm.getLayerCollision(1, 1));
    // And the freed layer goes back to "collides with everything".
    CHECK(pm.layerMask(3) == 0xFFFFFFFFu);

    // Restore for the following tests (the PhysicsManager is shared).
    pm.setLayerCollision(1, 2, true);
    while (pm.layerCount() > 1) pm.removeLayer(pm.layerCount() - 1);
    CHECK(pm.layerCount() == 1);
}

// --- Collision callbacks (non-trigger pairs) --------------------------------
//
// Twins of the trigger ones, but through the onContact path. Key difference:
// the Stay is NATIVE (eNOTIFY_TOUCH_PERSISTS), not synthesized per frame, so
// here what PhysX emits is counted as is.

// Analogous to CountingListener, with the 3 phases.
struct CountingCollisionListener : ICollisionListener {
    int enters = 0;
    int stays  = 0;
    int exits  = 0;
    void onCollisionEnter(const CollisionEvent&) override { ++enters; }
    void onCollisionStay (const CollisionEvent&) override { ++stays;  }
    void onCollisionExit (const CollisionEvent&) override { ++exits;  }
};

// Static floor flush with the ground and dynamic box 200 above, on the given layers. The
// box falls ~200 in 0.64 s, so 60 sub-steps are enough for the impact.
static EscenaDeCapas montarImpacto(PhysicsManager& pm, int capaSuelo, int capaCaja)
{
    flushAccumulator(pm);
    EscenaDeCapas e;
    e.suelo = pm.createBoxColliderComponent(glm::vec3(500.0f, 10.0f, 500.0f), glm::vec3(0.0f),
                                            glm::mat4(1.0f), /*dynamic=*/false);
    e.suelo->setLayer(capaSuelo);

    e.caja = pm.createBoxColliderComponent(
        glm::vec3(10.0f), glm::vec3(0.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 200.0f, 0.0f)), /*dynamic=*/true);
    e.caja->setLayer(capaCaja);
    e.rb = std::make_shared<Rigidbody>();
    pm.attachRigidbody(e.caja, e.rb);
    return e;
}

// The complete cycle on the SAME pair: Enter on touching the floor, Stay while it
// keeps resting, Exit on teleporting it far away.
static void test_collision_enter_stay_exit(PhysicsManager& pm)
{
    EscenaDeCapas e = montarImpacto(pm, /*suelo=*/0, /*caja=*/0);
    CountingCollisionListener l;
    e.caja->addCollisionListener(&l);

    step(pm, 90, 1.0f / 60.0f);   // falls, impacts and stays resting
    std::printf("  colision: enters=%d stays=%d exits=%d (tras el impacto)\n",
                l.enters, l.stays, l.exits);
    CHECK(l.enters > 0);   // TOUCH_FOUND
    CHECK(l.stays  > 0);   // TOUCH_PERSISTS, while they remain in contact
    CHECK(l.exits == 0);   // it has not separated from anything yet

    // Separate at once: setGlobalPose + autowake, PhysX breaks the pair.
    const int staysAntes = l.stays;
    e.caja->teleport(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 3000.0f, 0.0f)));
    step(pm, 10, 1.0f / 60.0f);
    std::printf("  colision: exits=%d tras separar (stays %d -> %d)\n",
                l.exits, staysAntes, l.stays);
    CHECK(l.exits > 0);              // TOUCH_LOST
    CHECK(l.stays == staysAntes);    // and there is no contact left to persist

    e.caja->removeCollisionListener(&l);
}

// The layer matrix also rules over collisions: the filtered pair generates NOT A
// SINGLE event, and the same setup with the cell open does (control).
static void test_collision_respects_layer_matrix(PhysicsManager& pm)
{
    pm.setLayerCollision(5, 6, false);
    CountingCollisionListener filtrado;
    {
        EscenaDeCapas e = montarImpacto(pm, /*suelo=*/5, /*caja=*/6);
        e.caja->addCollisionListener(&filtrado);
        step(pm, 90, 1.0f / 60.0f);
        e.caja->removeCollisionListener(&filtrado);
        // And by the way: without a pair, the box goes through the floor.
        CHECK(e.caja->getWorldTransform()[3].y < 0.0f);
    }

    pm.setLayerCollision(5, 6, true);
    CountingCollisionListener abierto;
    {
        EscenaDeCapas e = montarImpacto(pm, /*suelo=*/5, /*caja=*/6);
        e.caja->addCollisionListener(&abierto);
        step(pm, 90, 1.0f / 60.0f);
        e.caja->removeCollisionListener(&abierto);
    }

    std::printf("  colision con capas: filtrado -> %d/%d/%d | abierto -> %d/%d/%d\n",
                filtrado.enters, filtrado.stays, filtrado.exits,
                abierto.enters, abierto.stays, abierto.exits);
    CHECK(filtrado.enters == 0);
    CHECK(filtrado.stays  == 0);
    CHECK(filtrado.exits  == 0);
    CHECK(abierto.enters > 0);
    CHECK(abierto.stays  > 0);
}

// A trigger does NOT emit collision events: PhysX does not generate contacts for an
// eTRIGGER_SHAPE shape and the filter shader does not even ask it for eNOTIFY_TOUCH_*.
// What it receives, it receives through the trigger path (which stays intact).
static void test_trigger_emits_no_collision_events(PhysicsManager& pm)
{
    EscenaDeCapas e = montarImpacto(pm, /*suelo=*/0, /*caja=*/0);
    pm.setTrigger(e.suelo, true);

    CountingCollisionListener colision;
    CountingListener          trigger;
    e.caja->addCollisionListener(&colision);
    e.suelo->addListener(&trigger);

    step(pm, 90, 1.0f / 60.0f);

    e.caja->removeCollisionListener(&colision);
    e.suelo->removeListener(&trigger);
    pm.setTrigger(e.suelo, false);

    std::printf("  suelo como trigger: colision=%d/%d/%d, trigger enters=%d\n",
                colision.enters, colision.stays, colision.exits, trigger.enters);
    CHECK(colision.enters == 0);
    CHECK(colision.stays  == 0);
    CHECK(trigger.enters > 0);   // control: the trigger path is still alive
}

// --- CCD and interpolation per Rigidbody ---------------------------------------

// The eENABLE_CCD flag as PhysX sees it on the actor (not the C++ copy
// of the Rigidbody: getCcd() would return that without proving anything).
static bool actorHasCcdFlag(const std::shared_ptr<Collider>& col)
{
    auto* actor = static_cast<physx::PxRigidActor*>(col->actorHandle());
    auto* dyn   = actor ? actor->is<physx::PxRigidDynamic>() : nullptr;
    return dyn && (dyn->getRigidBodyFlags() & physx::PxRigidBodyFlag::eENABLE_CCD);
}

// X of the actor's RAW pose, skipping getWorldTransform: it is the reference
// against which the interpolated pose is compared.
static float rawActorX(const std::shared_ptr<Collider>& col)
{
    auto* actor = static_cast<physx::PxRigidActor*>(col->actorHandle());
    return actor ? actor->getGlobalPose().p.x : 0.0f;
}

// setCcd writes the flag on the PhysX actor, and only when asked.
static void test_ccd_flag_reaches_the_actor(PhysicsManager& pm)
{
    auto rb  = std::make_shared<Rigidbody>();
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);

    // Default: off on the component and on the actor (no existing scene
    // changes behavior).
    CHECK(!rb->getCcd());
    CHECK(!actorHasCcdFlag(col));

    rb->setCcd(true);
    CHECK(rb->getCcd());
    CHECK(actorHasCcdFlag(col));

    // PhysX does not support CCD on kinematic: the effective flag drops by itself...
    rb->setIsKinematic(true);
    CHECK(rb->getCcd());              // the user's intent is preserved
    CHECK(!actorHasCcdFlag(col));     // but the actor does not carry it
    // ...and comes back on leaving kinematic.
    rb->setIsKinematic(false);
    CHECK(actorHasCcdFlag(col));

    rb->setCcd(false);
    CHECK(!actorHasCcdFlag(col));
}

// How far a very fast sphere goes through a thin floor, with and without CCD. Returns
// the final Y: above the floor = stopped, far below = tunnel.
static float dropFastSphereOnThinFloor(PhysicsManager& pm, bool ccd)
{
    flushAccumulator(pm);

    // Thin floor: 4 units thick (halfExtent Y = 2) and plenty wide.
    auto floorCol = pm.createBoxColliderComponent(glm::vec3(500.0f, 2.0f, 500.0f), glm::vec3(0.0f),
                                                   glm::mat4(1.0f), /*dynamic=*/false);

    auto rb  = std::make_shared<Rigidbody>();
    rb->setUseGravity(false);       // we set the velocity ourselves, without accumulating gravity
    rb->setCcd(ccd);
    auto ball = pm.createSphereColliderComponent(
        10.0f, glm::vec3(0.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 500.0f, 0.0f)), /*dynamic=*/true);
    pm.attachRigidbody(ball, rb);
    // 30000 u/s at 1/60 = 500 units per sub-step: the discrete test compares
    // two poses that fall on both sides of the floor and sees nothing between them.
    rb->setVelocity(glm::vec3(0.0f, -30000.0f, 0.0f));

    for (int i = 0; i < 10; ++i) pm.stepSimulation(1.0f / 60.0f);
    return ball->getWorldTransform()[3].y;
}

// With CCD the sphere does not go through the thin floor; without CCD it does (control).
static void test_ccd_prevents_tunneling(PhysicsManager& pm)
{
    float without = dropFastSphereOnThinFloor(pm, /*ccd=*/false);
    float with    = dropFastSphereOnThinFloor(pm, /*ccd=*/true);
    std::printf("  túnel: sin CCD y=%.1f | con CCD y=%.1f\n", without, with);
    CHECK(without < -100.0f);   // control: without CCD it slips through and keeps falling
    CHECK(with    >   0.0f);    // with CCD it stays on top of the floor
}

// With interpolate, getWorldTransform returns the blend prev_pose/current_pose
// according to the accumulator's alpha, not the raw pose of the actor.
static void test_interpolate_returns_intermediate_pose(PhysicsManager& pm)
{
    const float fixed = pm.getFixedDeltaTime();
    flushAccumulator(pm);

    auto rb = std::make_shared<Rigidbody>();
    rb->setUseGravity(false);       // uniform rectilinear motion: the expected
    rb->setInterpolate(true);       // intermediate pose is exactly the average
    auto col = pm.createSphereColliderComponent(10.0f, glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    const float speed = 600.0f;     // units/s
    rb->setVelocity(glm::vec3(speed, 0.0f, 0.0f));

    // A frame of exactly one fixed step: the accumulator ends at 0 -> alpha 0
    // -> the visible pose is the one PRIOR to the sub-step (the interpolation runs one step
    // behind, that is its price).
    pm.stepSimulation(fixed);
    const float rawAfterStep = rawActorX(col);
    const float shownAtAlpha0 = col->getWorldTransform()[3].x;
    CHECK(rawAfterStep > 5.0f);                             // the actor DID move
    CHECK(std::fabs(shownAtAlpha0 - 0.0f) < 0.5f);          // what is visible is still at the origin
    CHECK(std::fabs(shownAtAlpha0 - rawAfterStep) > 5.0f);  // and it is NOT the raw pose

    // Half a step more of real time: another sub-step does not fit, so the actor does not
    // move and only the alpha rises to 0.5 -> exact average between the two poses.
    pm.stepSimulation(fixed * 0.5f);
    const float rawAfterHalf   = rawActorX(col);
    const float shownAtAlphaHalf = col->getWorldTransform()[3].x;
    const float expected = 0.5f * (0.0f + rawAfterHalf);
    std::printf("  interpolación: crudo=%.3f | alpha=0 -> %.3f | alpha=0.5 -> %.3f (esperado %.3f)\n",
                rawAfterHalf, shownAtAlpha0, shownAtAlphaHalf, expected);
    CHECK(std::fabs(rawAfterHalf - rawAfterStep) < 1e-3f);  // the actor did not advance
    CHECK(std::fabs(shownAtAlphaHalf - expected) < 0.1f);
}

// Without interpolate (the default), getWorldTransform returns the raw pose: the
// behavior of every existing scene does not change.
static void test_interpolate_off_returns_raw_pose(PhysicsManager& pm)
{
    const float fixed = pm.getFixedDeltaTime();
    flushAccumulator(pm);

    auto rb = std::make_shared<Rigidbody>();
    rb->setUseGravity(false);
    auto col = pm.createSphereColliderComponent(10.0f, glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    CHECK(!rb->getInterpolate());
    rb->setVelocity(glm::vec3(600.0f, 0.0f, 0.0f));

    pm.stepSimulation(fixed);
    pm.stepSimulation(fixed * 0.5f);   // alpha 0.5: if it interpolated, it would be noticeable
    CHECK(std::fabs(col->getWorldTransform()[3].x - rawActorX(col)) < 1e-4f);
    CHECK(rawActorX(col) > 5.0f);      // and the body really moved
}

// --- Transform scale ----------------------------------------------------
// PxTransform does not support scale, so the GameObject's is baked into the
// geometry of the shape. These helpers read what REALLY ended up in PhysX (not
// the collider's configured value, which must never change).
static physx::PxVec3 shapeHalfExtents(const std::shared_ptr<Collider>& col)
{
    auto* shape = static_cast<physx::PxShape*>(col->geometryShape());
    return static_cast<const physx::PxBoxGeometry&>(shape->getGeometry()).halfExtents;
}

static float shapeSphereRadius(const std::shared_ptr<Collider>& col)
{
    auto* shape = static_cast<physx::PxShape*>(col->geometryShape());
    return static_cast<const physx::PxSphereGeometry&>(shape->getGeometry()).radius;
}

static physx::PxCapsuleGeometry shapeCapsule(const std::shared_ptr<Collider>& col)
{
    auto* shape = static_cast<physx::PxShape*>(col->geometryShape());
    return static_cast<const physx::PxCapsuleGeometry&>(shape->getGeometry());
}

// Uniform 2x scale on a box: the geometry doubles on the three axes and the
// configured size (what the inspector sees and what is serialized) does NOT change.
static void test_scale_uniform_box(PhysicsManager& pm)
{
    const glm::mat4 xform = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(0.0f),
                                             xform, /*dynamic=*/false);
    const physx::PxVec3 he = shapeHalfExtents(col);
    CHECK(std::fabs(he.x - 2.0f) < 1e-4f);
    CHECK(std::fabs(he.y - 4.0f) < 1e-4f);
    CHECK(std::fabs(he.z - 6.0f) < 1e-4f);
    CHECK(col->getHalfExtents() == glm::vec3(1.0f, 2.0f, 3.0f));
}

// Uniform 2x scale on a sphere: the radius doubles, m_radius does not.
static void test_scale_uniform_sphere(PhysicsManager& pm)
{
    const glm::mat4 xform = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
    auto col = pm.createSphereColliderComponent(10.0f, glm::vec3(0.0f), xform, /*dynamic=*/false);
    CHECK(std::fabs(shapeSphereRadius(col) - 20.0f) < 1e-4f);
    CHECK(col->getRadius() == 10.0f);
}

// NON-uniform scale on a box: each axis goes its own way.
static void test_scale_non_uniform_box(PhysicsManager& pm)
{
    const glm::mat4 xform = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 3.0f, 4.0f));
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f),
                                             xform, /*dynamic=*/false);
    const physx::PxVec3 he = shapeHalfExtents(col);
    CHECK(std::fabs(he.x - 2.0f) < 1e-4f);
    CHECK(std::fabs(he.y - 3.0f) < 1e-4f);
    CHECK(std::fabs(he.z - 4.0f) < 1e-4f);
}

// Mirror and zero scale. They are tested through setWorldScale and not through a matrix: a
// matrix with an axis at 0 is singular and glm::decompose gets from it a rotation
// with NaN that PhysX rejects when creating the actor (a separate problem, prior to
// this). With negative scale the absolute value rules (a mirror does not thin the
// box) and with 0 it is clamped to a positive minimum, because PhysX rejects extents <= 0.
static void test_scale_mirror_and_zero_are_clamped(PhysicsManager& pm)
{
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f),
                                             glm::mat4(1.0f), /*dynamic=*/false);
    col->setWorldScale(glm::vec3(-2.0f, 3.0f, 0.0f));
    const physx::PxVec3 he = shapeHalfExtents(col);
    CHECK(std::fabs(he.x - 2.0f) < 1e-4f);
    CHECK(std::fabs(he.y - 3.0f) < 1e-4f);
    CHECK(he.z > 0.0f && he.z < 1e-3f);
}

// A MATRIX with an axis at 0 (Scale.Y = 0 in the inspector, to name one) is
// singular: glm::decompose normalizes the columns by dividing by their length, and
// with length 0 it produces a quaternion with NaN. PhysX rejects the actor, physxCheck
// throws, nobody catches it and the process DIES: exit code 3 and zero output, not
// even the printfs of the tests that had already passed.
//
// This test exists because the one above had to work around the problem (it tested
// scale 0 with setWorldScale, not with a matrix). The process still being alive at the
// end IS the result: if the guard is not there, there is no assert that fails
// because nothing else gets to run.
//
// The FOUR collider types are tested because each one does its own
// conversion to PxTransform: the guard has to be in all of them.
static void test_matriz_singular_no_mata_el_proceso(PhysicsManager& pm)
{
    // Scale 0 in Y, which is the case that comes from putting a 0 in the inspector.
    //
    // WITH TRANSLATION, and not at the origin: the position is what has to be
    // preserved when the decomposition fails, and with the object at (0,0,0) the
    // test would not distinguish preserving it from losing it.
    const glm::mat4 singular = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 20.0f, -7.0f)) *
                               glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 2.0f));

    auto caja = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f),
                                              singular, /*dynamic=*/false);
    CHECK(caja != nullptr);

    auto esfera = pm.createSphereColliderComponent(1.0f, glm::vec3(0.0f),
                                                   singular, /*dynamic=*/false);
    CHECK(esfera != nullptr);

    auto capsula = pm.createCapsuleColliderComponent(1.0f, 1.0f, glm::vec3(0.0f),
                                                     singular, /*dynamic=*/false);
    CHECK(capsula != nullptr);

    // The plane carries no dynamic: it is always static.
    auto plano = pm.createPlaneColliderComponent(glm::vec3(0.0f), singular);
    CHECK(plano != nullptr);

    // And that the simulation keeps running with them inside: an actor with a NaN
    // pose that slipped through creation would poison the entire scene.
    step(pm, 5, 1.0f / 60.0f);

    // And NOW the case that really kills the editor: giving it a Rigidbody, which is what
    // happens on entering Play. PxRigidBodyExt::setMassAndUpdateInertia
    // recomputes the inertia tensor from the shapes, and with a degenerate
    // shape that tensor comes out non-finite: the PX_ASSERT of
    // ExtInertiaTensor.h fires, which in Debug opens the CRT modal dialog (the app
    // freezes) and then dies with 0x80000003.
    auto dinamica = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f),
                                                  singular, /*dynamic=*/true);
    CHECK(dinamica != nullptr);
    // Through syncTransform, which is the editor's path: the scale is taken by the
    // matrix's own decompose, a clean vector is not passed by hand.
    dinamica->syncTransform(singular);

    // The geometry has to come from the REAL size of the object. With the scale that
    // glm::decompose returned for that matrix, 1.07374e+08 came out on all three axes,
    // and a 1e8 box overflows the inertia tensor as soon as it is given
    // mass. A generous ceiling is asserted: what is being caught here is not a pixel of
    // difference, it is an absurd value.
    const physx::PxVec3 he = shapeHalfExtents(dinamica);
    CHECK(std::isfinite(he.x) && std::isfinite(he.y) && std::isfinite(he.z));
    CHECK(he.x < 1e3f && he.y < 1e3f && he.z < 1e3f);
    // The flattened axis is clamped to a POSITIVE minimum (PhysX rejects extents <=
    // 0) and the other two really keep their scale.
    CHECK(he.y > 0.0f && he.y < 1e-3f);
    CHECK(std::fabs(he.x - 2.0f) < 1e-4f);
    CHECK(std::fabs(he.z - 2.0f) < 1e-4f);

    auto rb = std::make_shared<Rigidbody>();
    pm.attachRigidbody(dinamica, rb);
    step(pm, 5, 1.0f / 60.0f);

    // The why of all this, pinned down here so that nobody uses
    // glm::decompose again without looking at what it returns: with a singular matrix it
    // returns FALSE and writes NONE of its outputs. Whoever does not check it
    // is left with uninitialized locals; in Debug, the CRT's 0xCDCDCDCD
    // pattern, which as a float is -1.07e8.
    {
        glm::vec3 s2{7.0f}, t2{7.0f}, sk2{7.0f};
        glm::vec4 pe2{7.0f};
        glm::quat r2{7.0f, 7.0f, 7.0f, 7.0f};
        const bool ok = glm::decompose(singular, s2, r2, t2, sk2, pe2);
        CHECK(!ok);
        // And it does not even touch them: they still hold the 7 they were initialized with. It is
        // what shows that the garbage values are not a calculation that
        // gets out of hand, but memory that nobody wrote.
        CHECK(s2.x == 7.0f && t2.x == 7.0f && r2.x == 7.0f);
    }

    // And what really matters: what the editor reads back to write it
    // into the GameObject has to be a sane transform. Before it came out as
    // pos = -1.07e8 and the object went off into the blue as soon as its scale was touched.
    const glm::mat4 vuelta = dinamica->getWorldTransform();
    for (int c = 0; c < 4; ++c)
        for (int f = 0; f < 4; ++f)
            CHECK(std::isfinite(vuelta[c][f]));
    // The POSITION is preserved: it is the fourth column of the matrix and does not depend on
    // the decomposition coming out right. X and Z are asserted exact (gravity
    // only touches Y) and that is what distinguishes preserving it from losing it: if it
    // were taken from the failed decompose it would come out 0 (or garbage), not 3 and -7.
    CHECK(std::fabs(vuelta[3][0] - 3.0f) < 1e-3f);
    CHECK(std::fabs(vuelta[3][2] + 7.0f) < 1e-3f);
    // And in Y it has fallen from 20, which is its thing with gravity. The range comes out of the
    // real number: this engine's gravity is -981 (centimeter units),
    // so in 5 steps of 1/60 that is 0.5*981*(5/60)^2 = 3.4 approximately.
    // Slack is left for the integrator, but bounded on both sides: what
    // is caught here is both that it does not fall and that it shoots off.
    // The Y is asserted with slack and on purpose: this test first creates the four
    // static colliders at the SAME point, so the dynamic one is born overlapping
    // them and PhysX pushes it when resolving the penetration (it comes out ~22, not 20).
    // Modeling that would tie the test to a solver detail; what matters here
    // is that the Y is still that of a scene object and not 1e8.
    CHECK(std::fabs(vuelta[3][1] - 20.0f) < 5.0f);
}

// A sphere with non-uniform scale is still a sphere: the major axis rules.
// The capsule splits: radius with max(|x|,|z|), half-height with |y|.
static void test_scale_non_uniform_sphere_and_capsule(PhysicsManager& pm)
{
    const glm::mat4 xform = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 5.0f, 3.0f));
    auto sph = pm.createSphereColliderComponent(10.0f, glm::vec3(0.0f), xform, /*dynamic=*/false);
    CHECK(std::fabs(shapeSphereRadius(sph) - 50.0f) < 1e-4f);

    auto cap = pm.createCapsuleColliderComponent(10.0f, 20.0f, glm::vec3(0.0f),
                                                 xform, /*dynamic=*/false);
    const physx::PxCapsuleGeometry g = shapeCapsule(cap);
    CHECK(std::fabs(g.radius - 30.0f) < 1e-4f);      // max(|2|, |3|) = 3
    CHECK(std::fabs(g.halfHeight - 100.0f) < 1e-4f); // |5| en Y
    CHECK(cap->getRadius() == 10.0f && cap->getHalfHeight() == 20.0f);
}

// Scale 1: the geometry is EXACTLY the configured one (equality, not
// tolerance). It is the guardian of "no existing scene changes".
static void test_scale_one_is_identical(PhysicsManager& pm)
{
    auto box = pm.createBoxColliderComponent(glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(0.0f),
                                             glm::mat4(1.0f), /*dynamic=*/false);
    const physx::PxVec3 he = shapeHalfExtents(box);
    CHECK(he.x == 1.0f && he.y == 2.0f && he.z == 3.0f);

    auto sph = pm.createSphereColliderComponent(10.0f, glm::vec3(0.0f), glm::mat4(1.0f),
                                                /*dynamic=*/false);
    CHECK(shapeSphereRadius(sph) == 10.0f);

    // And a pure rotation does not touch it either, even though glm::decompose returns
    // 1±1e-7 on its axes (the scale comparison uses a tolerance).
    const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), 0.7f, glm::vec3(0.3f, 0.9f, 0.2f));
    auto rotated = pm.createBoxColliderComponent(glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(0.0f),
                                                 rot, /*dynamic=*/false);
    const physx::PxVec3 rhe = shapeHalfExtents(rotated);
    CHECK(rhe.x == 1.0f && rhe.y == 2.0f && rhe.z == 3.0f);

    // Nor scale noise below the tolerance: comparing with == instead of
    // with a tolerance would leave a 1.0000005 factor and the geometry would change.
    rotated->setWorldScale(glm::vec3(1.0f + 5e-7f));
    const physx::PxVec3 nhe = shapeHalfExtents(rotated);
    CHECK(nhe.x == 1.0f && nhe.y == 2.0f && nhe.z == 3.0f);
}

// The scale is re-applied when it changes, not only on creation: the same path that
// already pushes the pose (syncTransform) carries it.
static void test_scale_reapplied_on_change(PhysicsManager& pm)
{
    auto rb = std::make_shared<Rigidbody>();
    rb->setIsKinematic(true);
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f),
                                             glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    CHECK(shapeHalfExtents(col).x == 1.0f);

    col->syncTransform(glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 2.0f, 2.0f)));
    CHECK(std::fabs(shapeHalfExtents(col).x - 2.0f) < 1e-4f);

    // And back to 1: it does not stay stuck at the last value.
    col->syncTransform(glm::mat4(1.0f));
    CHECK(shapeHalfExtents(col).x == 1.0f);
}

// The scaled collider collides with what its configured size would not reach:
// the scale reaches the simulation, not just the shape's data.
static void test_scaled_box_actually_collides(PhysicsManager& pm)
{
    // Thin static floor at y=0, scaled 10x in X/Z (100 half-width).
    auto floor = pm.createBoxColliderComponent(glm::vec3(10.0f, 1.0f, 10.0f), glm::vec3(0.0f),
                                               glm::scale(glm::mat4(1.0f), glm::vec3(10.0f, 1.0f, 10.0f)),
                                               /*dynamic=*/false);
    // Body that falls at 50 in X: outside the unscaled floor (10), inside with it.
    auto rb  = std::make_shared<Rigidbody>();
    auto body = pm.createSphereColliderComponent(5.0f, glm::vec3(0.0f),
                                                 glm::translate(glm::mat4(1.0f), glm::vec3(50.0f, 40.0f, 0.0f)),
                                                 /*dynamic=*/true);
    pm.attachRigidbody(body, rb);
    step(pm, 120, 1.0f / 60.0f);
    CHECK(body->getWorldTransform()[3].y > 0.0f); // it stayed on top of the floor
    (void)floor;
}

// Changing the scale DURING Play, on a collider without a Rigidbody, reaches
// PhysX. It is the static path of Scene::update, which decides whether to push the pose
// by comparing the actor's with the GameObject's NORMALIZED one (without scale): without
// a separate comparison of the scale, a scale-only change does not move a bit
// of that matrix and the geometry stays at the size of the previous frame.
static void test_scale_change_during_play_reaches_physx(PhysicsManager& pm)
{
    Scene scene("escala");
    GameObject* go = scene.addGameObject("caja");
    go->localTransform = glm::mat4(1.0f);
    go->updateWorldTransforms(glm::mat4(1.0f));

    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f),
                                             go->worldTransform, /*dynamic=*/false);
    go->setBoxCollider(col);

    scene.update(1.0f / 60.0f);
    CHECK(shapeHalfExtents(col).x == 1.0f); // untouched: scale 1

    // Two updates: the traverse reads worldTransform and the local->world
    // propagation runs AT THE END of update, so the change enters in the
    // next frame (same latency a Lua script would see).
    go->localTransform = glm::scale(glm::mat4(1.0f), glm::vec3(3.0f));
    scene.update(1.0f / 60.0f);
    scene.update(1.0f / 60.0f);
    CHECK(std::fabs(shapeHalfExtents(col).x - 3.0f) < 1e-4f);

    // And with the scale already stable the geometry stays still: it is not re-scaled
    // on top of itself frame after frame. (That teleport is ADDITIONALLY not called cannot
    // be seen from here: the collider keeps no count of teleports.)
    scene.update(1.0f / 60.0f);
    CHECK(std::fabs(shapeHalfExtents(col).x - 3.0f) < 1e-4f);

    // The GameObject dies with the Scene; the collider is still held by the local
    // shared_ptr, which releases its actor on leaving the function.
    go->setBoxCollider(nullptr);
}

int main()
{
    PhysicsManager pm;
    pm.init();
    test_free_fall(pm);
    test_kinematic_no_fall(pm);
    test_freeze_position_y(pm);
    test_add_impulse(pm);
    test_force_mode_changes_magnitude(pm);
    test_force_mode_default_is_force(pm);
    test_rebuild_preserves_shape(pm);
    test_trigger_needs_a_rigidbody(pm);
    test_trigger_fires_when_other_has_rigidbody(pm);
    test_trigger_fires_when_trigger_has_rigidbody(pm);
    test_restitution_changes_bounce_height(pm);
    test_material_survives_rebuild(pm);
    test_fixed_step_is_framerate_independent(pm);
    test_giant_dt_clamped_to_max_substeps(pm);
    test_giant_dt_leaves_no_debt(pm);
    test_fixed_step_setters_reject_bad_values(pm);
    test_sphere_cast_uses_the_radius(pm);
    test_sphere_cast_miss(pm);
    test_overlap_sphere_counts(pm);
    test_overlap_box_honours_rotation(pm);
    test_layer_matrix_defaults_and_bounds(pm);
    test_layer_off_suppresses_contact(pm);
    test_layer_reenabled_at_runtime_makes_contact(pm);
    test_trigger_respects_layer_matrix(pm);
    test_add_and_remove_layers(pm);
    test_collision_enter_stay_exit(pm);
    test_collision_respects_layer_matrix(pm);
    test_trigger_emits_no_collision_events(pm);
    test_ccd_flag_reaches_the_actor(pm);
    test_ccd_prevents_tunneling(pm);
    test_interpolate_returns_intermediate_pose(pm);
    test_interpolate_off_returns_raw_pose(pm);
    test_scale_uniform_box(pm);
    test_scale_uniform_sphere(pm);
    test_scale_non_uniform_box(pm);
    test_scale_mirror_and_zero_are_clamped(pm);
    test_matriz_singular_no_mata_el_proceso(pm);
    test_scale_non_uniform_sphere_and_capsule(pm);
    test_scale_one_is_identical(pm);
    test_scale_reapplied_on_change(pm);
    test_scaled_box_actually_collides(pm);
    test_scale_change_during_play_reaches_physx(pm);
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL PHYSICS TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
