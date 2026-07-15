# Rigidbody Component (Unity-style) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Unity-equivalent `Rigidbody` component that separates rigid-body dynamics (mass, gravity, drag, velocity, forces, constraints) from the shape provided by colliders.

**Architecture:** `Collider` keeps owning its PhysX actor and shape. `Rigidbody` is a "config + API" object holding a non-owning `void*` to that actor. `PhysicsManager` orchestrates actor-type selection (static without Rigidbody, dynamic with) and rebuilds the actor when a Rigidbody is added/removed. Both stay agnostic of `GameObject` (dependency Core -> Physics only).

**Tech Stack:** C++20, PhysX 5 (guarded by `DT_PHYSX_ENABLED`), glm, nlohmann::json, sol2 (Lua), Dear ImGui, CMake + Ninja + MSVC.

## Global Constraints

- Build via `configure.bat` / `build.bat` through PowerShell (vcvarsall + Ninja), never raw cmake in Bash. See memory `project_build_commands`.
- All PhysX usage MUST be behind `#ifdef DT_PHYSX_ENABLED`. Headers reachable from `GameObject.h` (i.e. `Rigidbody.h`, all `Collider` headers) MUST NOT include `PxPhysicsAPI.h`; PhysX handles are stored as `void*`.
- Without `DT_PHYSX_ENABLED`, every actor operation is a no-op and the code still compiles and links.
- New physics-component sections in the Properties panel stay hidden until the user presses **Add**, same gate as colliders. See memory `feedback_component_ui_add_gate`.
- The world is in centimetres; gravity is `-981`. Do not reintroduce metre-scale assumptions.
- After a stale-header symptom (garbage-pointer crash), suspect a stale build: delete our own `.obj` and rebuild, do not "fix" new code. See memory `ninja_stale_header_deps`.
- GUI verification remains manual and out of scope for these tasks (no agent has a GUI). See memory `project_gui_manual_verification_pending`.

## File Structure

**New files:**
- `engine/include/DonTopo/Physics/Rigidbody.h` — Rigidbody component (state + API, `void*` actor).
- `engine/src/Physics/Rigidbody.cpp` — Rigidbody implementation (PhysX writes behind guard).
- `engine/tests/physics_tests.cpp` — headless test executable (plain `main` + asserts, no framework).
- `engine/tests/CMakeLists.txt` — defines `dt_physics_tests` target linking `DonTopoEngine`.

**Modified files:**
- `engine/CMakeLists.txt` — add `Rigidbody.cpp` to the library; `add_subdirectory(tests)`.
- `engine/include/DonTopo/Physics/PhysicsManager.h` / `engine/src/Physics/PhysicsManager.cpp` — actor-type selection, `attachRigidbody`, `detachRigidbody`, `rebuildActor`; create* colliders default to static.
- All 4 collider headers + cpp (`Box/Sphere/Capsule/Plane`) — strip `useGravity`/`isDynamic` policy; keep actor mechanics + shape/trigger.
- `engine/include/DonTopo/Core/GameObject.h` — `m_rigidbody` slot + accessors.
- `engine/src/Core/Scene.cpp` — sync loop rewrite; serialization + back-compat migration.
- `engine/src/Editor/PropertiesPanel.cpp` / `engine/include/DonTopo/Editor/PropertiesPanel.h` — Rigidbody section + Add/Remove.
- `engine/src/Scripting/ScriptBindings.cpp` — Lua `Rigidbody` usertype + `self.gameObject.rigidbody` access.

## Notes on strategy

The refactor is cross-cutting: `PhysicsManager::create*` today take a `useGravity` bool and colliders own the "am I dynamic" policy. To keep the build green at every task boundary, we go **additive first** (Tasks 1-3 add new code without changing existing behavior), then flip the model atomically in Task 4 (sync) + Task 5 (serialization), then surface it (Tasks 6-8).

Play-mode restore already round-trips through JSON (`EditorUI`: `m_playSnapshot = m_scene->toJson()` on Play, `reloadSceneFromJson(m_playSnapshot)` on Stop). Because velocity/angularVelocity are **not** serialized, a stopped body is rebuilt at rest at its edit-time pose for free — Task 8 only verifies this, it adds no new restore mechanism.

---

### Task 1: Rigidbody component + headless test target

**Files:**
- Create: `engine/include/DonTopo/Physics/Rigidbody.h`
- Create: `engine/src/Physics/Rigidbody.cpp`
- Create: `engine/tests/physics_tests.cpp`
- Create: `engine/tests/CMakeLists.txt`
- Modify: `engine/CMakeLists.txt` (add source + `add_subdirectory(tests)`)

**Interfaces:**
- Produces:
  - `class DonTopo::Rigidbody` with:
    - ctor `Rigidbody() = default;` (non-copyable, like `Collider`)
    - `void bindActor(void* actor);` — store actor + push full config
    - `void* actor() const;`
    - getters/setters: `float getMass()/setMass(float)`, `bool getUseGravity()/setUseGravity(bool)`, `bool getIsKinematic()/setIsKinematic(bool)`, `float getDrag()/setDrag(float)`, `float getAngularDrag()/setAngularDrag(float)`
    - constraints: `uint32_t getConstraints()/setConstraints(uint32_t)` plus `enum RigidbodyConstraints` bit flags
    - dynamics: `glm::vec3 getVelocity()/setVelocity(glm::vec3)`, `glm::vec3 getAngularVelocity()/setAngularVelocity(glm::vec3)`, `void addForce(glm::vec3)`, `void addTorque(glm::vec3)`, `void addImpulse(glm::vec3)`

- [ ] **Step 1: Write the Rigidbody header**

Create `engine/include/DonTopo/Physics/Rigidbody.h`:

```cpp
#pragma once
#include <glm/glm.hpp>
#include <cstdint>

namespace DonTopo {

// Constraints estilo Unity: congelar traslación/rotación por eje. Bitmask que
// se traduce a physx::PxRigidDynamicLockFlags en bindActor().
enum RigidbodyConstraints : uint32_t {
    RB_None            = 0,
    RB_FreezePositionX = 1u << 0,
    RB_FreezePositionY = 1u << 1,
    RB_FreezePositionZ = 1u << 2,
    RB_FreezeRotationX = 1u << 3,
    RB_FreezeRotationY = 1u << 4,
    RB_FreezeRotationZ = 1u << 5,
};

// Componente de dinámica de cuerpo rígido (equivalente a Unity Rigidbody). NO
// posee el actor PhysX: lo posee el Collider (mismo contrato de vida de
// siempre). Este componente guarda un puntero NO-dueño al PxRigidDynamic y
// actúa como "config + API". Agnóstico de GameObject: la dependencia va
// Core -> Physics, nunca al revés. No incluye PxPhysicsAPI.h para no filtrar
// PhysX en headers alcanzables desde GameObject.h.
class Rigidbody {
public:
    Rigidbody() = default;
    Rigidbody(const Rigidbody&)            = delete;
    Rigidbody& operator=(const Rigidbody&) = delete;

    // Guarda el actor (physx::PxRigidDynamic* como void*) y empuja TODA la
    // config actual al actor. Lo llama PhysicsManager tras crear/reconstruir el
    // actor dinámico. Sin DT_PHYSX_ENABLED solo guarda el puntero.
    void  bindActor(void* actor);
    void* actor() const { return m_actor; }

    // Config (los setters escriben al actor enlazado si existe).
    float getMass() const        { return m_mass; }
    void  setMass(float mass);
    bool  getUseGravity() const  { return m_useGravity; }
    void  setUseGravity(bool enabled);
    bool  getIsKinematic() const { return m_isKinematic; }
    void  setIsKinematic(bool enabled);
    float getDrag() const        { return m_drag; }
    void  setDrag(float drag);
    float getAngularDrag() const { return m_angularDrag; }
    void  setAngularDrag(float drag);
    uint32_t getConstraints() const { return m_constraints; }
    void     setConstraints(uint32_t mask);

    // Dinámica. Velocidad y fuerzas son no-op si el actor es kinematic (PhysX
    // las ignora / avisa); se guardan/aplican solo cuando tiene sentido.
    glm::vec3 getVelocity() const;
    void      setVelocity(const glm::vec3& v);
    glm::vec3 getAngularVelocity() const;
    void      setAngularVelocity(const glm::vec3& v);
    void addForce(const glm::vec3& f);   // PxForceMode::eFORCE
    void addTorque(const glm::vec3& t);  // PxForceMode::eFORCE
    void addImpulse(const glm::vec3& f); // PxForceMode::eIMPULSE

private:
    void* m_actor = nullptr; // physx::PxRigidDynamic* (no-dueño)

    float    m_mass        = 1.0f;
    bool     m_useGravity  = true;
    bool     m_isKinematic = false;
    float    m_drag        = 0.0f;
    float    m_angularDrag = 0.05f; // default de Unity
    uint32_t m_constraints = RB_None;
};

} // namespace DonTopo
```

- [ ] **Step 2: Write the Rigidbody implementation**

Create `engine/src/Physics/Rigidbody.cpp`:

```cpp
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
```

- [ ] **Step 3: Add Rigidbody.cpp to the engine library and register the tests subdir**

In `engine/CMakeLists.txt`, add inside the `add_library(DonTopoEngine STATIC ...)` list, right after the `src/Physics/PhysicsManager.cpp` line:

```cmake
    src/Physics/Rigidbody.cpp
```

Then at the end of the file (after the MSVC block) add:

```cmake
add_subdirectory(tests)
```

- [ ] **Step 4: Create the headless test target**

Create `engine/tests/CMakeLists.txt`:

```cmake
add_executable(dt_physics_tests physics_tests.cpp)
# DonTopoEngine expone PhysX y DT_PHYSX_ENABLED como PUBLIC: el test los hereda.
target_link_libraries(dt_physics_tests PRIVATE DonTopoEngine)
target_compile_features(dt_physics_tests PRIVATE cxx_std_20)
```

Create `engine/tests/physics_tests.cpp` with the failing test (write-then-run-then-implement is already satisfied for the class; this is the behavioral test for the whole physics core):

```cpp
// Test headless del núcleo de física (sin GUI). Plain main + asserts, sin
// framework — coherente con un proyecto C++/CMake/Ninja sin infra de tests.
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Physics/Rigidbody.h"
#include "DonTopo/Physics/Colliders/BoxCollider.h"

#include <glm/glm.hpp>
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Avanza la simulación n pasos de dt segundos.
static void step(PhysicsManager& pm, int n, float dt) { for (int i = 0; i < n; ++i) pm.stepSimulation(dt); }

// Un cuerpo dinámico con gravedad debe caer (Y decrece).
static void test_free_fall()
{
    PhysicsManager pm; pm.init();
    auto rb = std::make_shared<Rigidbody>();
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    float y0 = col->getWorldTransform()[3].y;
    step(pm, 30, 1.0f / 60.0f);
    float y1 = col->getWorldTransform()[3].y;
    CHECK(y1 < y0 - 1.0f);
    pm.shutdown();
}

// Kinematic no cae.
static void test_kinematic_no_fall()
{
    PhysicsManager pm; pm.init();
    auto rb = std::make_shared<Rigidbody>();
    rb->setIsKinematic(true);
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    float y0 = col->getWorldTransform()[3].y;
    step(pm, 30, 1.0f / 60.0f);
    float y1 = col->getWorldTransform()[3].y;
    CHECK(std::fabs(y1 - y0) < 0.001f);
    pm.shutdown();
}

// Freeze-Y mantiene Y aunque haya gravedad.
static void test_freeze_position_y()
{
    PhysicsManager pm; pm.init();
    auto rb = std::make_shared<Rigidbody>();
    rb->setConstraints(RB_FreezePositionY);
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    float y0 = col->getWorldTransform()[3].y;
    step(pm, 30, 1.0f / 60.0f);
    float y1 = col->getWorldTransform()[3].y;
    CHECK(std::fabs(y1 - y0) < 0.001f);
    pm.shutdown();
}

// addImpulse cambia la velocidad en la dirección esperada.
static void test_add_impulse()
{
    PhysicsManager pm; pm.init();
    auto rb = std::make_shared<Rigidbody>();
    rb->setUseGravity(false);
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    rb->addImpulse(glm::vec3(100.0f, 0.0f, 0.0f));
    step(pm, 1, 1.0f / 60.0f);
    CHECK(rb->getVelocity().x > 0.0f);
    pm.shutdown();
}

// Rebuild static <-> dynamic conserva el shape (trigger sigue funcionando: el
// shape mantiene su geometría/localPose). Aquí comprobamos que attach/detach no
// crashea y que tras detach el collider sigue vivo y estático (no cae).
static void test_rebuild_preserves_shape()
{
    PhysicsManager pm; pm.init();
    auto rb = std::make_shared<Rigidbody>();
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    pm.detachRigidbody(col); // vuelve a static
    float y0 = col->getWorldTransform()[3].y;
    step(pm, 30, 1.0f / 60.0f);
    float y1 = col->getWorldTransform()[3].y;
    CHECK(std::fabs(y1 - y0) < 0.001f); // static no cae
    CHECK(col->getHalfExtents() == glm::vec3(1.0f)); // geometría intacta
    pm.shutdown();
}

int main()
{
    test_free_fall();
    test_kinematic_no_fall();
    test_freeze_position_y();
    test_add_impulse();
    test_rebuild_preserves_shape();
    if (g_failures == 0) std::printf("ALL PHYSICS TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
```

Note: these tests consume `PhysicsManager::attachRigidbody`, `detachRigidbody`, and a `createBoxColliderComponent(..., bool dynamic)` overload — all delivered in Task 2. They will not compile until Task 2 lands; that is intended (Task 2's Step "run tests" is where they first build and pass). If your executor requires a green build at the end of every task, comment out the test bodies here and uncomment them in Task 2 Step 1.

- [ ] **Step 5: Configure + build to verify Rigidbody compiles into the library**

Run (PowerShell): `./build.bat`
Expected: `DonTopoEngine` builds with `Rigidbody.cpp`. `dt_physics_tests` may fail to link/compile until Task 2 (expected). Verify no errors in `Rigidbody.cpp` itself.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Physics/Rigidbody.h engine/src/Physics/Rigidbody.cpp engine/tests/ engine/CMakeLists.txt
git commit -m "feat(physics): componente Rigidbody (config+API sobre actor) + target de tests headless"
```

---

### Task 2: PhysicsManager orchestration — static/dynamic actors, attach/detach, rebuild

**Files:**
- Modify: `engine/include/DonTopo/Physics/PhysicsManager.h`
- Modify: `engine/src/Physics/PhysicsManager.cpp`
- Modify: all 4 collider headers + cpp (`Box/Sphere/Capsule/Plane`) — add a `bool dynamic` notion and strip `useGravity` policy (see below).
- Test: `engine/tests/physics_tests.cpp` (from Task 1)

**Interfaces:**
- Consumes: `Rigidbody` (Task 1).
- Produces:
  - `std::shared_ptr<BoxCollider> PhysicsManager::createBoxColliderComponent(const glm::vec3& halfExtents, const glm::vec3& center, const glm::mat4& worldTransform, bool dynamic);` (replaces the `bool useGravity, float density` params; `dynamic` selects `PxRigidDynamic` vs `PxRigidStatic`). Same shape of change for Sphere/Capsule. Plane stays static-only (unchanged signature).
  - `void PhysicsManager::attachRigidbody(const std::shared_ptr<Collider>& collider, const std::shared_ptr<Rigidbody>& rb);` — ensure the collider's actor is a `PxRigidDynamic` (rebuild if currently static), then `rb->bindActor(actor)`.
  - `void PhysicsManager::detachRigidbody(const std::shared_ptr<Collider>& collider);` — rebuild the collider's actor as `PxRigidStatic`.
  - `Collider`: new `void* actorHandle() const;`, `void setActorHandle(void* actor);`, `void* geometryShape() const { return triggerShape(); }` reuse, and `void rebindActor(void* actor);` that updates the stored actor and re-applies `userData`. `Collider` loses `isDynamic()`, `getUseGravity()`, `setUseGravity()`.

**Design of the collider strip + rebuild**

Today each collider stores `m_actor`/`m_shape` and creates a `PxRigidDynamic` in `PhysicsManager::create*`. New model:

1. `create*` builds either `PxRigidStatic` (dynamic=false) or `PxRigidDynamic` (dynamic=true) and attaches the shape. For static, skip `updateMassAndInertia` and the kinematic/gravity flags.
2. The collider keeps `m_actor` (now possibly a `PxRigidStatic*` or `PxRigidDynamic*`) and `m_shape`. Its destructor still `release()`s `m_actor` — but must release the correct base type. Use `PxActor::release()` (base) instead of `PxRigidDynamic::release()` so it works for both. Change every collider dtor accordingly.
3. `getWorldTransform()`/`syncTransform()`/`teleport()` use `PxRigidActor` (base of both static and dynamic) for `getGlobalPose`/`setGlobalPose`. `setKinematicTarget` only exists on `PxRigidDynamic`, so `syncTransform` must cast to `PxRigidDynamic` — it is only ever called on dynamic-kinematic colliders (guaranteed by the Scene loop in Task 4). Guard it: if the actor is not a `PxRigidDynamic`, fall back to `setGlobalPose`.
4. `setUseGravity`/`isDynamic`/`getUseGravity` are removed from all colliders.

`rebuildActor(collider, dynamic)` (private helper) steps:
- Read current global pose from the old actor.
- Detach the shape from the old actor (`oldActor->detachShape(*shape)` keeps the shape alive because the collider holds a reference via `m_shape`; PhysX shapes are refcounted — grab an extra ref with `shape->acquireReference()` before detach to be safe, release after re-attach).
- Remove old actor from scene, `release()` it.
- Create the new actor of the requested type at the saved pose, `attachShape(*shape)`, add to scene, set `userData = collider.get()`.
- `collider->rebindActor(newActor)`; if dynamic, caller binds the Rigidbody.
- Preserve trigger state: if `collider->isTrigger()`, re-apply `applyTriggerFlag(true)` after re-attach (shape flags survive on the shape, but re-assert to be safe).

- [ ] **Step 1: Enable the Task 1 tests (if they were commented out) and update collider headers**

In each collider header (`BoxCollider.h`, `SphereCollider.h`, `CapsuleCollider.h`):
- Change the constructor: drop the trailing `bool useGravity` param.
- Remove `setUseGravity`, `getUseGravity`, `isDynamic`, and the `m_useGravity` member.
- Keep `getWorldTransform`, `syncTransform`, `teleport`, `setCenter`, `setHalfExtents`/`setRadius`/etc., `triggerShape`.

`PlaneCollider.h`: already static-only; just ensure its dtor releases via `PxActor` (Step 3).

Example for `BoxCollider.h` — new constructor line and removed methods:

```cpp
    // actor: physx::PxRigidStatic* o PxRigidDynamic* ya creado y añadido a la
    // escena por PhysicsManager. shape: PxShape* de geometría caja adjunta.
    BoxCollider(void* actor, void* shape, const glm::vec3& halfExtents, const glm::vec3& center);
    ~BoxCollider();
```
Delete from `BoxCollider.h`: `void setUseGravity(bool);`, `bool getUseGravity() const`, `bool isDynamic() const`, and `bool m_useGravity;`.

- [ ] **Step 2: Update collider cpp files (ctor, dtor via PxActor, syncTransform guard)**

For `BoxCollider.cpp` (apply the analogous edit to Sphere/Capsule; Plane only gets the dtor change):

Constructor — drop `useGravity`:
```cpp
BoxCollider::BoxCollider(void* actor, void* shape, const glm::vec3& halfExtents,
                         const glm::vec3& center)
    : m_halfExtents(halfExtents)
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
```

Destructor — release via base `PxActor` (works for static and dynamic):
```cpp
BoxCollider::~BoxCollider()
{
#ifdef DT_PHYSX_ENABLED
    if (m_actor) static_cast<PxActor*>(m_actor)->release();
#endif
}
```

Delete the whole `BoxCollider::setUseGravity(...)` function.

`syncTransform` — guard for static actors (setKinematicTarget is dynamic-only):
```cpp
void BoxCollider::syncTransform(const glm::mat4& worldTransform)
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;
    glm::vec3 scale, translation, skew; glm::vec4 perspective; glm::quat rotation;
    glm::decompose(worldTransform, scale, rotation, translation, skew, perspective);
    PxTransform pose(PxVec3(translation.x, translation.y, translation.z),
                     PxQuat(rotation.x, rotation.y, rotation.z, rotation.w));
    auto* dyn = static_cast<PxRigidActor*>(m_actor)->is<PxRigidDynamic>();
    if (dyn && (dyn->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC))
        dyn->setKinematicTarget(pose);
    else
        static_cast<PxRigidActor*>(m_actor)->setGlobalPose(pose);
#else
    (void)worldTransform;
#endif
}
```

`getWorldTransform` and `teleport`: replace `static_cast<PxRigidDynamic*>(m_actor)` with `static_cast<PxRigidActor*>(m_actor)` for `getGlobalPose`/`setGlobalPose`. In `teleport`, the velocity reset must first check the actor is a non-kinematic `PxRigidDynamic`:
```cpp
    auto* actor = static_cast<PxRigidActor*>(m_actor);
    actor->setGlobalPose(pose);
    if (auto* dyn = actor->is<PxRigidDynamic>())
        if (!(dyn->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC)) {
            dyn->setLinearVelocity(PxVec3(0.0f));
            dyn->setAngularVelocity(PxVec3(0.0f));
        }
```

- [ ] **Step 3: Add actor-handle plumbing to the Collider base**

In `engine/include/DonTopo/Physics/Colliders/Collider.h`, add public methods (near `setManager`):
```cpp
    // Actor PhysX subyacente (PxRigidStatic* o PxRigidDynamic*), como void*.
    // Lo usa PhysicsManager::rebuildActor pa reasignar el actor tras cambiar
    // de tipo (static <-> dynamic). El collider sigue siendo el DUEÑO: lo
    // libera en su dtor.
    virtual void* actorHandle() const = 0;
    virtual void  setActorHandle(void* actor) = 0;
```
In each collider header add overrides:
```cpp
    void* actorHandle() const override;
    void  setActorHandle(void* actor) override;
```
In each collider cpp:
```cpp
void* BoxCollider::actorHandle() const
{
#ifdef DT_PHYSX_ENABLED
    return m_actor;
#else
    return nullptr;
#endif
}
void BoxCollider::setActorHandle(void* actor)
{
#ifdef DT_PHYSX_ENABLED
    m_actor = actor;
#else
    (void)actor;
#endif
}
```

- [ ] **Step 4: Rewrite PhysicsManager create* to select actor type + add attach/detach/rebuild**

In `PhysicsManager.h`, change the create signatures (drop `useGravity`/`density`, add `bool dynamic`) and add the new methods:
```cpp
    std::shared_ptr<BoxCollider> createBoxColliderComponent(const glm::vec3& halfExtents,
                                                            const glm::vec3& center,
                                                            const glm::mat4& worldTransform,
                                                            bool dynamic);
    std::shared_ptr<SphereCollider> createSphereColliderComponent(float radius,
                                                                  const glm::vec3& center,
                                                                  const glm::mat4& worldTransform,
                                                                  bool dynamic);
    std::shared_ptr<CapsuleCollider> createCapsuleColliderComponent(float radius, float halfHeight,
                                                                    const glm::vec3& center,
                                                                    const glm::mat4& worldTransform,
                                                                    bool dynamic);
    // Plane sin cambios (siempre static).
    std::shared_ptr<PlaneCollider> createPlaneColliderComponent(const glm::vec3& center,
                                                                const glm::mat4& worldTransform);

    void attachRigidbody(const std::shared_ptr<Collider>& collider, const std::shared_ptr<Rigidbody>& rb);
    void detachRigidbody(const std::shared_ptr<Collider>& collider);
```
Add `#include <memory>` already present; forward declare `class Rigidbody;` in the `namespace DonTopo { ... }` fwd block at top.

In `PhysicsManager.cpp`, add a private free helper (in the anonymous/`DonTopo` scope) that builds an actor of the requested type and attaches a shape. Refactor `createBoxColliderComponent`:
```cpp
std::shared_ptr<BoxCollider> PhysicsManager::createBoxColliderComponent(
    const glm::vec3& halfExtents, const glm::vec3& center,
    const glm::mat4& worldTransform, bool dynamic)
{
#ifdef DT_PHYSX_ENABLED
    glm::vec3 scale, translation, skew; glm::vec4 perspective; glm::quat rotation;
    glm::decompose(worldTransform, scale, rotation, translation, skew, perspective);
    PxTransform pose(PxVec3(translation.x, translation.y, translation.z),
                     PxQuat(rotation.x, rotation.y, rotation.z, rotation.w));

    auto* physics  = static_cast<PxPhysics*>(m_physics);
    auto* material = static_cast<PxMaterial*>(m_material);
    auto* scene    = static_cast<PxScene*>(m_scene);

    PxRigidActor* actor = dynamic
        ? static_cast<PxRigidActor*>(physics->createRigidDynamic(pose))
        : static_cast<PxRigidActor*>(physics->createRigidStatic(pose));
    physxCheck(actor, "createRigidActor(box)");

    PxBoxGeometry geometry(halfExtents.x, halfExtents.y, halfExtents.z);
    PxShape* shape = PxRigidActorExt::createExclusiveShape(*actor, geometry, *material);
    physxCheck(shape, "createExclusiveShape(box)");
    shape->setLocalPose(PxTransform(PxVec3(center.x, center.y, center.z)));

    if (dynamic) {
        // Masa por defecto: Rigidbody la recalcula en bindActor. Sin gravedad ni
        // kinematic aquí; los pone attachRigidbody -> Rigidbody::bindActor.
        PxRigidBodyExt::updateMassAndInertia(*static_cast<PxRigidDynamic*>(actor), 1.0f);
    }
    scene->addActor(*actor);

    auto collider = std::make_shared<BoxCollider>(actor, shape, halfExtents, center);
    collider->setManager(this);
    actor->userData = static_cast<Collider*>(collider.get());
    return collider;
#else
    (void)worldTransform; (void)dynamic;
    auto collider = std::make_shared<BoxCollider>(nullptr, nullptr, halfExtents, center);
    collider->setManager(this);
    return collider;
#endif
}
```
Apply the same transformation to Sphere and Capsule (drop density, add `dynamic`, `createRigidStatic` when false, guard mass/inertia to dynamic). For Capsule keep the `axisCorrection()` localPose. `PlaneCollider` create: unchanged except the ctor no longer takes params it didn't take anyway.

Add `attachRigidbody` / `detachRigidbody` and the private `rebuildActor`:
```cpp
void PhysicsManager::attachRigidbody(const std::shared_ptr<Collider>& collider,
                                     const std::shared_ptr<Rigidbody>& rb)
{
    if (!collider || !rb) return;
#ifdef DT_PHYSX_ENABLED
    void* actor = collider->actorHandle();
    if (actor && !static_cast<PxRigidActor*>(actor)->is<PxRigidDynamic>())
        actor = rebuildActor(collider, /*dynamic=*/true);
    rb->bindActor(collider->actorHandle());
#else
    (void)collider; rb->bindActor(nullptr);
#endif
}

void PhysicsManager::detachRigidbody(const std::shared_ptr<Collider>& collider)
{
    if (!collider) return;
#ifdef DT_PHYSX_ENABLED
    void* actor = collider->actorHandle();
    if (actor && static_cast<PxRigidActor*>(actor)->is<PxRigidDynamic>())
        rebuildActor(collider, /*dynamic=*/false);
#else
    (void)collider;
#endif
}
```
Private `rebuildActor` (declare `void* rebuildActor(const std::shared_ptr<Collider>&, bool dynamic);` in the header's private section, guarded by `DT_PHYSX_ENABLED`):
```cpp
#ifdef DT_PHYSX_ENABLED
void* PhysicsManager::rebuildActor(const std::shared_ptr<Collider>& collider, bool dynamic)
{
    auto* physics = static_cast<PxPhysics*>(m_physics);
    auto* scene   = static_cast<PxScene*>(m_scene);
    auto* oldActor = static_cast<PxRigidActor*>(collider->actorHandle());
    if (!oldActor) return nullptr;

    PxTransform pose = oldActor->getGlobalPose();
    auto* shape = static_cast<PxShape*>(collider->triggerShape());
    bool wasTrigger = collider->isTrigger();

    shape->acquireReference();          // sobrevive al detach
    oldActor->detachShape(*shape);
    scene->removeActor(*oldActor);
    oldActor->release();

    PxRigidActor* newActor = dynamic
        ? static_cast<PxRigidActor*>(physics->createRigidDynamic(pose))
        : static_cast<PxRigidActor*>(physics->createRigidStatic(pose));
    newActor->attachShape(*shape);
    shape->release();                   // suelta la ref extra
    if (dynamic) PxRigidBodyExt::updateMassAndInertia(*static_cast<PxRigidDynamic*>(newActor), 1.0f);
    scene->addActor(*newActor);
    newActor->userData = collider.get();

    collider->setActorHandle(newActor);
    if (wasTrigger) collider->applyTriggerFlag(true);
    return newActor;
}
#endif
```

- [ ] **Step 5: Build + run the physics tests**

Run: `./build.bat`
Then run the test binary (path under the build dir): `./build-ninja/engine/tests/dt_physics_tests.exe`
Expected: `ALL PHYSICS TESTS PASSED`, exit code 0.

If a garbage-pointer crash appears right after these header changes, suspect a stale build (memory `ninja_stale_header_deps`): delete our physics `.obj` under `build-ninja/engine/CMakeFiles/DonTopoEngine.dir/src/Physics/` and rebuild before touching the new code.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Physics engine/src/Physics
git commit -m "feat(physics): PhysicsManager orquesta actor static/dynamic + attach/detach/rebuild de Rigidbody"
```

Note: this task leaves `Scene.cpp`, `PropertiesPanel.cpp` and `Scene` serialization calling the OLD create* signatures and removed collider methods — they will not compile yet. Tasks 3-5 fix all call sites. If your executor requires green builds per task, fold Tasks 3-5 into this commit's branch and build once at the end of Task 5. The task boundaries below remain useful for review.

---

### Task 3: GameObject rigidbody slot

**Files:**
- Modify: `engine/include/DonTopo/Core/GameObject.h`

**Interfaces:**
- Consumes: `Rigidbody` (Task 1).
- Produces: `GameObject::setRigidbody`, `getRigidbody`, `hasRigidbody`.

- [ ] **Step 1: Add the include and slot**

In `GameObject.h`, add near the other physics includes:
```cpp
#include "DonTopo/Physics/Rigidbody.h"
```
Add accessors (near the collider accessors):
```cpp
            void setRigidbody(std::shared_ptr<Rigidbody> rb) { m_rigidbody = std::move(rb); }
            const std::shared_ptr<Rigidbody>& getRigidbody() const { return m_rigidbody; }
            bool hasRigidbody() const { return m_rigidbody != nullptr; }
```
Add the member (near `m_planeCollider`):
```cpp
            std::shared_ptr<Rigidbody> m_rigidbody;
```

- [ ] **Step 2: Build**

Run: `./build.bat` (still red on Scene/Editor call sites — that is Tasks 4-5). Verify `GameObject.h` itself has no errors (e.g. compile `GameObject.cpp` target if possible, or proceed).

- [ ] **Step 3: Commit**

```bash
git add engine/include/DonTopo/Core/GameObject.h
git commit -m "feat(core): slot Rigidbody en GameObject"
```

---

### Task 4: Scene per-frame sync driven by Rigidbody

**Files:**
- Modify: `engine/src/Core/Scene.cpp` (the `update` loop around lines 437-478)

**Interfaces:**
- Consumes: `GameObject::hasRigidbody/getRigidbody`, `Rigidbody::getIsKinematic`, `Collider::getWorldTransform/syncTransform/teleport`.

- [ ] **Step 1: Replace the collider sync block**

Replace the per-collider `isDynamic()` branches with a single helper applied to whichever collider the GO has. New logic per GameObject:

```cpp
    m_root.traverse([](GameObject* go) {
        auto col = go->anyCollider();
        if (!col) return;

        const bool hasRb      = go->hasRigidbody();
        const bool kinematic  = hasRb && go->getRigidbody()->getIsKinematic();
        const bool simulated  = hasRb && !kinematic; // cuerpo dinámico real

        if (simulated) {
            // PhysX manda: leer pose actor -> GameObject.
            go->worldTransform = col->getWorldTransform();
            glm::mat4 parentWorld = go->parent ? go->parent->worldTransform : glm::mat4(1.0f);
            go->localTransform = glm::inverse(parentWorld) * go->worldTransform;
        } else if (kinematic) {
            // Kinematic: empujar pose GameObject -> actor (setKinematicTarget).
            col->syncTransform(go->worldTransform);
        } else {
            // Solo collider (static): empujar pose por si el editor la movió.
            col->teleport(go->worldTransform);
        }
    });
```
`anyCollider()` already returns the single `std::shared_ptr<Collider>` (colliders are mutually exclusive). Remove the four old per-type blocks entirely.

- [ ] **Step 2: Build + run physics tests (regression)**

Run: `./build.bat` then `./build-ninja/engine/tests/dt_physics_tests.exe`
Expected: still `ALL PHYSICS TESTS PASSED` (this task doesn't touch the tested paths but confirms nothing regressed at link time). Note: `Scene.cpp` still references old serialization signatures fixed in Task 5, so a full build may remain red until Task 5 — build the `dt_physics_tests` after Task 5 if needed.

- [ ] **Step 3: Commit**

```bash
git add engine/src/Core/Scene.cpp
git commit -m "feat(physics): sync por frame dirigido por Rigidbody (simulated/kinematic/static)"
```

---

### Task 5: Serialization + back-compat migration

**Files:**
- Modify: `engine/src/Core/Scene.cpp` (serialize block ~82-112, deserialize block ~264-298)

**Interfaces:**
- Consumes: `GameObject::getRigidbody/setRigidbody`, `Rigidbody` getters/setters, `PhysicsManager::create*(..., bool dynamic)`, `attachRigidbody`.
- Produces: JSON `rigidbody` block `{mass, useGravity, isKinematic, drag, angularDrag, constraints}`; colliders no longer carry `useGravity`.

- [ ] **Step 1: Serialize the Rigidbody + drop collider useGravity**

In the `toJson` node writer, remove `{"useGravity", c->getUseGravity()}` from the box/sphere/capsule collider blocks (those getters no longer exist). After the collider blocks, add:
```cpp
        if (node.hasRigidbody())
        {
            const auto& rb = node.getRigidbody();
            j["rigidbody"] = { {"mass", rb->getMass()},
                               {"useGravity", rb->getUseGravity()},
                               {"isKinematic", rb->getIsKinematic()},
                               {"drag", rb->getDrag()},
                               {"angularDrag", rb->getAngularDrag()},
                               {"constraints", rb->getConstraints()} };
        }
```

- [ ] **Step 2: Deserialize colliders as static, then apply Rigidbody / legacy migration**

Change each collider recreation to pass `dynamic=false` (colliders load static; a Rigidbody, if present, promotes them). For the box block:
```cpp
        if (j.contains("boxCollider"))
        {
            const auto& c = j["boxCollider"];
            node->setBoxCollider(physics.createBoxColliderComponent(
                jsonToVec3(c.at("halfExtents")), jsonToVec3(c.at("center")),
                node->worldTransform, /*dynamic=*/false));
            node->getBoxCollider()->setOwner(node);
            physics.setTrigger(node->getBoxCollider(), c.value("isTrigger", false));
        }
```
Do the same for sphere/capsule (pass `false`). Plane already static.

After ALL collider blocks and before/after audio, add the Rigidbody load with legacy migration:
```cpp
        // Rigidbody: bloque nuevo. Back-compat: escenas viejas guardaban
        // useGravity DENTRO del collider; si no hay bloque rigidbody pero un
        // collider trae useGravity legacy, sintetizamos un Rigidbody heredando
        // ese valor (useGravity=true -> cuerpo dinámico como antes).
        auto legacyGravity = [&](const char* key) -> int {
            // -1: sin campo; 0/1: valor legacy
            if (j.contains(key) && j[key].contains("useGravity"))
                return j[key]["useGravity"].get<bool>() ? 1 : 0;
            return -1;
        };

        if (j.contains("rigidbody"))
        {
            const auto& r = j["rigidbody"];
            auto rb = std::make_shared<Rigidbody>();
            rb->setMass(r.value("mass", 1.0f));
            rb->setUseGravity(r.value("useGravity", true));
            rb->setIsKinematic(r.value("isKinematic", false));
            rb->setDrag(r.value("drag", 0.0f));
            rb->setAngularDrag(r.value("angularDrag", 0.05f));
            rb->setConstraints(r.value("constraints", 0u));
            node->setRigidbody(rb);
            if (auto col = node->anyCollider()) physics.attachRigidbody(col, rb);
        }
        else
        {
            int g = legacyGravity("boxCollider");
            if (g < 0) g = legacyGravity("sphereCollider");
            if (g < 0) g = legacyGravity("capsuleCollider");
            if (g == 1) // legacy dinámico -> Rigidbody con gravedad
            {
                auto rb = std::make_shared<Rigidbody>();
                rb->setUseGravity(true);
                node->setRigidbody(rb);
                if (auto col = node->anyCollider()) physics.attachRigidbody(col, rb);
            }
            // g == 0 (legacy kinematic sin gravedad) -> se queda collider static,
            // que es el comportamiento equivalente (no caía).
        }
```
Add `#include "DonTopo/Physics/Rigidbody.h"` at the top of `Scene.cpp` if not transitively available (it is, via `GameObject.h`).

Also apply the same Rigidbody load logic inside `insertFromJson`/node reconstruction if that is a separate code path from `fromJson`'s node reader — search `Scene.cpp` for the second collider-recreation site (the clone/insert path shares `readNode`; verify there is a single node reader and both `fromJson` and `insertFromJson` route through it. If they don't, replicate the Rigidbody block in both).

- [ ] **Step 3: Full build + physics tests + a round-trip smoke check**

Run: `./build.bat` — expected: FULL build green now (all call sites updated).
Run: `./build-ninja/engine/tests/dt_physics_tests.exe` — expected `ALL PHYSICS TESTS PASSED`.
Manual round-trip (optional, in-app): save a scene with a dynamic body, reload, confirm it still falls.

- [ ] **Step 4: Commit**

```bash
git add engine/src/Core/Scene.cpp
git commit -m "feat(physics): serializa Rigidbody + migracion back-compat de useGravity legacy"
```

---

### Task 6: Editor UI — Rigidbody section + Add/Remove

**Files:**
- Modify: `engine/include/DonTopo/Editor/PropertiesPanel.h`
- Modify: `engine/src/Editor/PropertiesPanel.cpp`

**Interfaces:**
- Consumes: `GameObject::hasRigidbody/getRigidbody/setRigidbody`, `Rigidbody` setters, `PhysicsManager::attachRigidbody/detachRigidbody`, `EditorContext` (`selected`, `physics`, `undo`, `pushLog`).
- Produces: `PropertiesPanel::drawRigidbodySection(EditorContext&)`, called from the same place the collider sections are drawn; a new "Rigidbody" entry in the Add-component popup.

- [ ] **Step 1: Declare the section method + cache state in the header**

In `PropertiesPanel.h`, next to `drawBoxColliderSection`, declare:
```cpp
    void drawRigidbodySection(EditorContext& ctx);
```
Add cached edit state members (mirroring the collider caches):
```cpp
    const void* m_rigidbodyCachedFor = nullptr;
    float       m_editRbMass = 1.0f;
    bool        m_editRbUseGravity = true;
    bool        m_editRbKinematic = false;
    float       m_editRbDrag = 0.0f;
    float       m_editRbAngularDrag = 0.05f;
    uint32_t    m_editRbConstraints = 0;
```

- [ ] **Step 2: Implement drawRigidbodySection**

In `PropertiesPanel.cpp`, add the include `#include "DonTopo/Physics/Rigidbody.h"` and implement (immediate-apply model like the trigger checkbox; no per-field undo command to keep scope tight — matches how `isTrigger` applies directly):
```cpp
void PropertiesPanel::drawRigidbodySection(EditorContext& ctx)
{
    if (!ctx.selected || !ctx.selected->hasRigidbody()) { m_rigidbodyCachedFor = nullptr; return; }
    Rigidbody* rb = ctx.selected->getRigidbody().get();
    if (m_rigidbodyCachedFor != rb) {
        m_editRbMass        = rb->getMass();
        m_editRbUseGravity  = rb->getUseGravity();
        m_editRbKinematic   = rb->getIsKinematic();
        m_editRbDrag        = rb->getDrag();
        m_editRbAngularDrag = rb->getAngularDrag();
        m_editRbConstraints = rb->getConstraints();
        m_rigidbodyCachedFor = rb;
    }

    if (!ImGui::CollapsingHeader("Rigidbody", ImGuiTreeNodeFlags_DefaultOpen)) return;

    if (ImGui::DragFloat("Mass", &m_editRbMass, 0.1f, 0.0001f, FLT_MAX, "%.3f"))
        rb->setMass(m_editRbMass);
    if (ImGui::DragFloat("Drag", &m_editRbDrag, 0.01f, 0.0f, FLT_MAX, "%.3f"))
        rb->setDrag(m_editRbDrag);
    if (ImGui::DragFloat("Angular Drag", &m_editRbAngularDrag, 0.01f, 0.0f, FLT_MAX, "%.3f"))
        rb->setAngularDrag(m_editRbAngularDrag);
    if (ImGui::Checkbox("Use Gravity", &m_editRbUseGravity))
        rb->setUseGravity(m_editRbUseGravity);
    if (ImGui::Checkbox("Is Kinematic", &m_editRbKinematic))
        rb->setIsKinematic(m_editRbKinematic);

    ImGui::TextUnformatted("Freeze Position");
    bool px = m_editRbConstraints & RB_FreezePositionX;
    bool py = m_editRbConstraints & RB_FreezePositionY;
    bool pz = m_editRbConstraints & RB_FreezePositionZ;
    bool changed = false;
    changed |= ImGui::Checkbox("PX", &px); ImGui::SameLine();
    changed |= ImGui::Checkbox("PY", &py); ImGui::SameLine();
    changed |= ImGui::Checkbox("PZ", &pz);
    ImGui::TextUnformatted("Freeze Rotation");
    bool rx = m_editRbConstraints & RB_FreezeRotationX;
    bool ry = m_editRbConstraints & RB_FreezeRotationY;
    bool rz = m_editRbConstraints & RB_FreezeRotationZ;
    changed |= ImGui::Checkbox("RX", &rx); ImGui::SameLine();
    changed |= ImGui::Checkbox("RY", &ry); ImGui::SameLine();
    changed |= ImGui::Checkbox("RZ", &rz);
    if (changed) {
        uint32_t mask = 0;
        if (px) mask |= RB_FreezePositionX; if (py) mask |= RB_FreezePositionY; if (pz) mask |= RB_FreezePositionZ;
        if (rx) mask |= RB_FreezeRotationX; if (ry) mask |= RB_FreezeRotationY; if (rz) mask |= RB_FreezeRotationZ;
        m_editRbConstraints = mask;
        rb->setConstraints(mask);
    }

    if (ImGui::Button("Remove Rigidbody")) {
        if (auto col = ctx.selected->anyCollider(); col && ctx.physics)
            ctx.physics->detachRigidbody(col);
        ctx.selected->setRigidbody(nullptr);
        m_rigidbodyCachedFor = nullptr;
        ctx.pushLog("Componente Rigidbody quitado de '" + ctx.selected->name + "'");
    }
}
```

- [ ] **Step 3: Call the section + add it to the Add-component popup**

Where the collider sections are invoked (`drawBoxColliderSection(ctx); ...`), add:
```cpp
            drawRigidbodySection(ctx);
```
In `drawAddComponentButton(ctx)` (the Add popup), add a menu entry that is disabled unless the object has a collider (a Rigidbody needs a shape) and hidden when it already has one:
```cpp
        if (!ctx.selected->hasRigidbody() && ctx.selected->hasAnyCollider())
        {
            if (ImGui::MenuItem("Rigidbody"))
            {
                auto rb = std::make_shared<Rigidbody>();
                ctx.selected->setRigidbody(rb);
                if (auto col = ctx.selected->anyCollider(); col && ctx.physics)
                    ctx.physics->attachRigidbody(col, rb);
                ctx.pushLog("Componente Rigidbody anadido a '" + ctx.selected->name + "'");
            }
        }
```
(Follow the exact style/guards used by the existing collider entries in that popup — this snippet matches their pattern.)

- [ ] **Step 4: Build**

Run: `./build.bat`
Expected: green. GUI behavior stays manual-verify (memory `project_gui_manual_verification_pending`).

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/Editor/PropertiesPanel.h engine/src/Editor/PropertiesPanel.cpp
git commit -m "feat(editor): seccion Rigidbody en Properties + Add/Remove con gate de collider"
```

---

### Task 7: Lua bindings

**Files:**
- Modify: `engine/src/Scripting/ScriptBindings.cpp`
- Modify: `engine/src/Scripting/LuaApiReference.cpp` (autocomplete/reference, follow existing pattern)

**Interfaces:**
- Consumes: `Rigidbody`, `GameObject::getRigidbody/hasRigidbody`.
- Produces: sol2 usertype `Rigidbody` and `gameObject.rigidbody` accessor (nil when absent).

- [ ] **Step 1: Register the Rigidbody usertype**

In `ScriptBindings.cpp`, where other usertypes are registered, add (`lua` is the `sol::state`; match the local variable name used in that function):
```cpp
    lua.new_usertype<Rigidbody>("Rigidbody",
        "mass",            sol::property(&Rigidbody::getMass, &Rigidbody::setMass),
        "useGravity",      sol::property(&Rigidbody::getUseGravity, &Rigidbody::setUseGravity),
        "isKinematic",     sol::property(&Rigidbody::getIsKinematic, &Rigidbody::setIsKinematic),
        "drag",            sol::property(&Rigidbody::getDrag, &Rigidbody::setDrag),
        "angularDrag",     sol::property(&Rigidbody::getAngularDrag, &Rigidbody::setAngularDrag),
        "velocity",        sol::property(&Rigidbody::getVelocity, &Rigidbody::setVelocity),
        "angularVelocity", sol::property(&Rigidbody::getAngularVelocity, &Rigidbody::setAngularVelocity),
        "AddForce",   [](Rigidbody& r, float x, float y, float z){ r.addForce({x,y,z}); },
        "AddTorque",  [](Rigidbody& r, float x, float y, float z){ r.addTorque({x,y,z}); },
        "AddImpulse", [](Rigidbody& r, float x, float y, float z){ r.addImpulse({x,y,z}); }
    );
```
Add `#include "DonTopo/Physics/Rigidbody.h"`.

- [ ] **Step 2: Expose rigidbody on the GameObject usertype**

On the existing `GameObject` usertype registration, add a `rigidbody` property returning a raw pointer (nil when absent so scripts can guard `if self.gameObject.rigidbody then`):
```cpp
        "rigidbody", sol::property([](GameObject& go) -> Rigidbody* {
            return go.hasRigidbody() ? go.getRigidbody().get() : nullptr;
        }),
```
(Insert into the existing `new_usertype<GameObject>(...)` call, matching how `anyCollider`/other members are exposed there.)

- [ ] **Step 3: Update the Lua API reference/autocomplete**

In `LuaApiReference.cpp`, add `Rigidbody` entries following the existing entry format (the same file that lists collider/trigger API). Mirror an existing multi-property usertype entry; list `mass, useGravity, isKinematic, drag, angularDrag, velocity, angularVelocity` and methods `AddForce(x,y,z), AddTorque(x,y,z), AddImpulse(x,y,z)`, plus `gameObject.rigidbody`.

- [ ] **Step 4: Build + a Lua smoke script**

Run: `./build.bat` (note `ScriptBindings.cpp` uses `/bigobj`; expect longer compile).
Smoke (manual, in-app): a script with
```lua
function Update(dt)
  local rb = self.gameObject.rigidbody
  if rb then rb:AddForce(0, 500, 0) end
end
```
on a dynamic body pushes it upward. Compile-only verification is sufficient for this task; runtime is manual (memory `project_gui_manual_verification_pending`).

- [ ] **Step 5: Commit**

```bash
git add engine/src/Scripting/ScriptBindings.cpp engine/src/Scripting/LuaApiReference.cpp
git commit -m "feat(scripting): bindings Lua de Rigidbody (velocity/mass/AddForce/...) + referencia"
```

---

### Task 8: Play-mode verification + final polish

**Files:**
- Modify (only if a gap is found): `engine/src/Editor/EditorUI.cpp`
- Modify: `README.md` (document the component briefly if the repo lists components there)

**Interfaces:** none new.

- [ ] **Step 1: Verify Play/Stop restore**

Reasoning check: on Play, `m_playSnapshot = m_scene->toJson()` captures the Rigidbody block (Task 5). On Stop, `reloadSceneFromJson(m_playSnapshot)` rebuilds colliders static then re-attaches the Rigidbody via `attachRigidbody`; velocity/angularVelocity are NOT serialized, so the body restarts at rest at its edit-time pose. No new code expected.

Confirm the Play-start traverse (EditorUI.cpp around line 158, `m_scene->traverse([](GameObject* go){ ... })`) does not special-case colliders in a way that now needs the Rigidbody (e.g. an initial `teleport`); if it iterates colliders to seed poses, leave it — static/kinematic seeding still holds. Only add code if a concrete gap is observed.

- [ ] **Step 2: Full build + physics tests**

Run: `./build.bat` then `./build-ninja/engine/tests/dt_physics_tests.exe`
Expected: green build, `ALL PHYSICS TESTS PASSED`.

- [ ] **Step 3: Commit (if any change)**

```bash
git add -A
git commit -m "docs(physics): documenta Rigidbody; verificacion Play/Stop restore"
```

---

## Self-Review notes (for the executor)

- **Spec coverage:** core props (Task 1), static/dynamic + rebuild orchestration (Task 2), GameObject slot (Task 3), per-frame sync (Task 4), serialization + migration (Task 5), editor UI + Add-gate (Task 6), Lua (Task 7), Play restore (Task 8). All spec sections mapped.
- **Type consistency:** `bool dynamic` (not `useGravity`) on `create*`; `getUseGravity`/`getIsKinematic`/`getConstraints` used identically in Rigidbody, serialization, editor, and Lua; `RB_Freeze*` flag names identical across `Rigidbody.h`, editor, and `toLockFlags`.
- **Known interlock:** Tasks 2-5 form one compile unit (create* signature change ripples through Scene/editor). Build green is guaranteed only at the end of Task 5; earlier per-task builds are partial by design. Review can still gate each task independently.
