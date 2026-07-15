# Rigidbody Component (Unity-style) — Design

Date: 2026-07-15
Status: Approved (design)

## Goal

Add a Unity-equivalent `Rigidbody` component that separates rigid-body **dynamics**
(mass, gravity, drag, velocity, forces, constraints) from the **shape** provided by
colliders. Today the physics-body behavior is baked into each `Collider`
(`useGravity`, `isDynamic`, actor creation). This design splits the two, matching
Unity's model: a collider by itself is static; adding a `Rigidbody` makes it dynamic.

Non-goals for v1: `interpolation` (None/Interpolate/Extrapolate) and
`collisionDetection` (Discrete/Continuous CCD). Deferred to a later iteration.

## Architecture & Ownership

PhysX cannot convert `PxRigidStatic` <-> `PxRigidDynamic` in place. Adding or removing
a `Rigidbody` therefore requires destroying the actor and recreating it as the other
type, re-attaching the existing shape.

Ownership (chosen to minimize churn and preserve current lifetime rules):

- **Collider owns the actor** (as today) and the shape. It still releases the actor in
  its destructor, so the existing trigger-destruction logic
  (`~Collider` -> `PhysicsManager::onColliderDestroyed`) is untouched.
- **Rigidbody is "config + API" over the collider's actor** via a non-owning `void*`.
  It stores mass/gravity/kinematic/drag/constraints and exposes velocity/forces.
- **PhysicsManager is the orchestrator.** It picks the actor type at creation and
  rebuilds it on Add/Remove of a `Rigidbody`.

Rules:

- Collider **without** Rigidbody -> `PxRigidStatic` + shape.
- Collider **with** Rigidbody -> `PxRigidDynamic` + shape; the Rigidbody binds to that
  actor and applies its config.
- Add/Remove Rigidbody in editor -> `PhysicsManager::rebuildActor(...)`: create the
  other actor type, detach+re-attach the shape (preserving geometry, localPose and
  trigger flags), update the collider's actor pointer, and re-bind the Rigidbody.

Both `Rigidbody` and `Collider` remain **agnostic of GameObject** (dependency stays
Core -> Physics, never the reverse). PhysX types never leak into headers reachable
from `GameObject.h`; actor/shape are held as `void*` under `DT_PHYSX_ENABLED`, matching
the existing collider pattern. Without `DT_PHYSX_ENABLED`, Rigidbody stores plain state
and all actor operations are no-ops.

## Components

### `Rigidbody` (engine/include/DonTopo/Physics/Rigidbody.h + src/Physics/Rigidbody.cpp)

State (serialized, editable):

- `float mass` (default 1.0)
- `bool useGravity` (default true)
- `bool isKinematic` (default false)
- `float drag` (linear damping, default 0.0)
- `float angularDrag` (default 0.05, Unity default)
- `constraints` — bitmask over 6 axes: freeze position X/Y/Z, freeze rotation X/Y/Z

API:

- `getVelocity() / setVelocity(glm::vec3)`
- `getAngularVelocity() / setAngularVelocity(glm::vec3)`
- `addForce(glm::vec3)` — accumulated by PhysX between steps (`PxForceMode::eFORCE`)
- `addTorque(glm::vec3)`
- `addImpulse(glm::vec3)` (`PxForceMode::eIMPULSE`)
- `bindActor(void* actor)` — store the actor pointer and push the full config to the
  `PxRigidDynamic`: `updateMassAndInertia`/`setMass`, `setLinearDamping`,
  `setAngularDamping`, `PxActorFlag::eDISABLE_GRAVITY`, `PxRigidBodyFlag::eKINEMATIC`,
  `setRigidDynamicLockFlags` from the constraints bitmask.
- Setters write through to the bound actor immediately when present.

Velocity get/set and forces are no-ops when the actor is kinematic (PhysX ignores them)
— guarded to avoid warnings, matching Unity behavior.

### Collider refactor

- Drops the **policy** it currently carries: `useGravity`, `isDynamic`, `getUseGravity`,
  `setUseGravity`. That state moves to `Rigidbody`.
- Keeps the **actor mechanics** it already has, now used generically by the sync loop:
  `getWorldTransform()` (read `PxRigidActor::getGlobalPose`), `syncTransform()`
  (`setKinematicTarget` on dynamic, `setGlobalPose` on static), `teleport()`.
- Keeps shape/geometry/trigger responsibilities unchanged (`setCenter`,
  `setHalfExtents`, `applyTriggerFlag`, `triggerShape`).
- Actor creation no longer assumes `PxRigidDynamic`. PhysicsManager decides the type;
  the collider stores whatever actor it is handed.

### GameObject

- New slot `std::shared_ptr<Rigidbody> m_rigidbody` with `setRigidbody` / `getRigidbody`
  / `hasRigidbody`. One rigidbody per object.

## Per-frame flow (Scene::update)

The current `isDynamic()` branch is replaced by rigidbody-driven policy, per collider:

- GO has Rigidbody, **not kinematic** -> read pose actor->GO
  (`worldTransform = collider->getWorldTransform()`, then recompute localTransform).
- GO has Rigidbody, **kinematic** -> push GO->actor (`collider->syncTransform(world)` ->
  `setKinematicTarget`).
- GO has **collider only** (static) -> push pose when it changed
  (`collider->teleport(world)` / `setGlobalPose`); the actor is not simulated.

`stepSimulation` is unchanged in structure: PhysX applies gravity, damping and queued
forces during `simulate(dt)`. `addForce`/`addImpulse` queue onto the actor between
steps.

## Serialization & migration

- New `rigidbody` object in each GameObject's JSON: `mass`, `useGravity`, `isKinematic`,
  `drag`, `angularDrag`, and `constraints` (single bitmask int over the 6 axes).
- Colliders **stop serializing** `useGravity`.
- **Back-compat:** when loading, if a collider block still carries a legacy `useGravity`
  field (scenes saved before this change) and there is no `rigidbody` block, synthesize
  a `Rigidbody` with `useGravity` inherited from that legacy value. Old scenes that were
  dynamic stay dynamic; old static colliders (useGravity=false) load as a bare static
  collider. This keeps every previously saved scene loading correctly.

## Editor UI (Properties panel)

- New "Rigidbody" section, hidden until the user presses **Add** in the component popup,
  matching the collider Add-gate convention (see `feedback_component_ui_add_gate`).
- Fields: `mass` (drag float), checkboxes `useGravity` / `isKinematic`,
  `drag` / `angularDrag` (drag floats), and six checkboxes for the constraints
  (freeze position X/Y/Z, freeze rotation X/Y/Z).
- **Remove** triggers `rebuildActor` back to a static actor.
- Add/Remove are wired through the existing command/undo system where colliders are.

## Lua bindings

Unity-style access to the current GameObject's rigidbody from a script:

- Properties: `rb.velocity`, `rb.angularVelocity`, `rb.mass`, `rb.useGravity`,
  `rb.isKinematic`, `rb.drag`, `rb.angularDrag`.
- Methods: `rb:AddForce(x,y,z)`, `rb:AddTorque(x,y,z)`, `rb:AddImpulse(x,y,z)`.
- Resolved through the script's GameObject (same wiring path as trigger listeners).
- Absent rigidbody -> `nil`, so scripts can guard with `if rb then ... end`.

## Play Mode snapshot/restore

The snapshot system already restores transforms on Stop. Extend restore to:

- Reset `velocity` and `angularVelocity` to zero on the bound actor.
- Re-apply the saved pose (`teleport`) so a fallen dynamic body returns to its edit-time
  position.

## Testing

Headless unit tests (no GUI; GUI verification stays manual and pending, see
`project_gui_manual_verification_pending`):

- Free fall: dynamic body with `useGravity=true` loses Y over successive `stepSimulation`.
- Kinematic body does not fall.
- Constraint freeze-Y keeps Y constant under gravity.
- `addForce` / `addImpulse` change velocity in the expected direction.
- Rebuild static<->dynamic preserves shape geometry, localPose and trigger flags.
- Back-compat load: a legacy scene JSON with collider `useGravity=true` yields a
  GameObject with a Rigidbody after load.

## Out of scope (v1)

- `interpolation` (None/Interpolate/Extrapolate)
- `collisionDetection` (Discrete/Continuous CCD)
- Joints / articulations
