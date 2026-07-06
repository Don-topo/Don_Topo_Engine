# Sphere/Capsule/Plane Collider — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Añadir Sphere Collider, Capsule Collider y Plane Collider como componentes de física seleccionables desde el botón "Add" del panel Properties, replicando el patrón ya establecido por `BoxCollider`, con exclusividad mutua entre los 4 tipos por GameObject.

**Architecture:** Cada tipo nuevo es una clase independiente (`SphereCollider`, `CapsuleCollider`, `PlaneCollider`) que sigue el mismo patrón que `BoxCollider`: siempre respaldada por un `physx::PxRigidDynamic`, alternando `kinematic`/`disableGravity` según el flag `useGravity` (excepto Plane, siempre kinematic sin ese flag). `GameObject` gana 3 slots `shared_ptr` nuevos (uno por tipo) más `hasAnyCollider()` para exclusividad mutua. `PhysicsManager` gana 3 funciones factory paralelas a `createBoxColliderComponent`. `EditorUI` gana 3 secciones de edición nuevas y extiende el popup "Add" con 3 entradas más, todas deshabilitadas si el GameObject ya tiene cualquiera de los 4 tipos.

**Tech Stack:** C++20, PhysX (`DT_PHYSX_ENABLED`), Dear ImGui, GLM.

## Global Constraints

- Spec de referencia: `docs/superpowers/specs/2026-07-06-sphere-capsule-plane-collider-design.md`.
- Exclusividad mutua: un GameObject solo puede tener **un** collider de física a la vez entre Box/Sphere/Capsule/Plane. `GameObject::hasAnyCollider()` es el guard único usado por el popup "Add".
- Capsule alineada a Y: PhysX orienta `PxCapsuleGeometry` por defecto a lo largo del eje X local del shape. Se compone una rotación fija `PxQuat(PxHalfPi, PxVec3(0,0,1))` (90° sobre Z, mapea X→Y) en el `localPose` del shape, tanto en la creación (`PhysicsManager`) como en cada `setCenter` (`CapsuleCollider`) — la rotación no cambia, solo el offset de traslación.
- Plane siempre estático: sin checkbox "Use Gravity" en UI, sin parámetro `useGravity` en su constructor/factory. `isDynamic()` retorna `false` hardcoded. Reutiliza el mismo truco de eje (`PxQuat(PxHalfPi, PxVec3(0,0,1))`) para que la normal por defecto del plano infinito sea +Y en vez del X nativo de PhysX.
- Togglear "Use Gravity" (Sphere/Capsule) NO destruye/recrea el `PxRigidActor`, solo cambia flags — igual que Box.
- `Center` es offset local dentro del actor (`PxShape::localPose`), nunca se mezcla con la pose del GameObject — igual que Box.
- Sin framework de tests unitarios en este repo (proyecto de motor gráfico/físico); verificación es build limpio (`.\build.bat`) + prueba manual descrita en el spec.
- El código debe seguir compilando con y sin `DT_PHYSX_ENABLED` — replicar el patrón `#ifdef` ya usado en `BoxCollider.cpp`.

---

### Task 1: Clase `SphereCollider`

**Files:**
- Create: `engine/include/DonTopo/SphereCollider.h`
- Create: `engine/src/SphereCollider.cpp`

**Interfaces:**
- Consumes: nada de otras tasks.
- Produces: la clase `SphereCollider`, consumida por `GameObject` (Task 4), `PhysicsManager` (Task 5) y `EditorUI` (Task 6):
  ```cpp
  namespace DonTopo {
  class SphereCollider {
  public:
      SphereCollider(void* actor, void* shape, float radius,
                     const glm::vec3& center, bool useGravity);
      ~SphereCollider();
      void setCenter(const glm::vec3& center);
      void setRadius(float radius);
      void setUseGravity(bool enabled);
      glm::vec3 getCenter() const;
      float     getRadius() const;
      bool      getUseGravity() const;
      bool      isDynamic() const;
      glm::mat4 getWorldTransform() const;
      void syncTransform(const glm::mat4& worldTransform);
      void teleport(const glm::mat4& worldTransform);
  };
  }
  ```

- [ ] **Step 1: Escribir `engine/include/DonTopo/SphereCollider.h`**

```cpp
#pragma once
#include <glm/glm.hpp>

namespace DonTopo {

// Componente único de física de tipo esfera. Mismo patrón que BoxCollider:
// siempre respaldado por un physx::PxRigidDynamic (nunca PxRigidStatic); con
// useGravity=false el actor queda kinematic + gravedad desactivada (se mueve
// empujado desde el GameObject); con useGravity=true PhysX lo simula normal
// y su pose se lee de vuelta hacia el GameObject cada frame. Togglear
// useGravity solo cambia flags del actor existente, nunca lo destruye/recrea.
class SphereCollider {
public:
    // actor: physx::PxRigidDynamic* ya creado y añadido a la escena por
    // PhysicsManager. shape: physx::PxShape* de geometría esfera adjunta a
    // ese actor, con localPose ya puesto a partir de center.
    SphereCollider(void* actor, void* shape, float radius,
                   const glm::vec3& center, bool useGravity);
    ~SphereCollider();

    SphereCollider(const SphereCollider&)            = delete;
    SphereCollider& operator=(const SphereCollider&) = delete;

    // Offset local de la shape dentro del actor (PxShape::setLocalPose).
    void setCenter(const glm::vec3& center);
    // Radio de la esfera (PxShape::setGeometry con nueva PxSphereGeometry).
    void setRadius(float radius);
    // true: actor dinámico normal (cae con la gravedad de la escena).
    // false: actor kinematic + gravedad desactivada (no cae, se empuja
    // desde el GameObject vía syncTransform).
    void setUseGravity(bool enabled);

    glm::vec3 getCenter() const      { return m_center; }
    float     getRadius() const      { return m_radius; }
    bool      getUseGravity() const  { return m_useGravity; }

    // true si el motor debe LEER la pose de PhysX hacia el GameObject
    // (getWorldTransform); false si debe EMPUJAR la pose del GameObject
    // hacia PhysX (syncTransform).
    bool isDynamic() const { return m_useGravity; }

    // Lee physx::PxRigidDynamic::getGlobalPose() (traslación + rotación,
    // sin escala). Solo válido cuando isDynamic() es true.
    glm::mat4 getWorldTransform() const;

    // Empuja worldTransform hacia PhysX vía setKinematicTarget (traslación +
    // rotación; la escala se ignora). Solo válido cuando isDynamic() es
    // false.
    void syncTransform(const glm::mat4& worldTransform);

    // Teletransporta el actor (setGlobalPose, no setKinematicTarget) sea
    // cual sea el modo, y resetea su velocidad a cero. Pensado para
    // ediciones puntuales desde el Transform panel del editor.
    void teleport(const glm::mat4& worldTransform);

private:
#ifdef DT_PHYSX_ENABLED
    void* m_actor = nullptr; // physx::PxRigidDynamic*
    void* m_shape = nullptr; // physx::PxShape*
#endif
    float     m_radius;
    glm::vec3 m_center;
    bool      m_useGravity;
};

} // namespace DonTopo
```

- [ ] **Step 2: Escribir `engine/src/SphereCollider.cpp`**

```cpp
#include "DonTopo/SphereCollider.h"

#ifdef DT_PHYSX_ENABLED
#define GLM_ENABLE_EXPERIMENTAL
#include <PxPhysicsAPI.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

using namespace physx;
#endif

namespace DonTopo {

SphereCollider::SphereCollider(void* actor, void* shape, float radius,
                               const glm::vec3& center, bool useGravity)
    : m_radius(radius)
    , m_center(center)
    , m_useGravity(useGravity)
{
#ifdef DT_PHYSX_ENABLED
    m_actor = actor;
    m_shape = shape;
#else
    (void)actor;
    (void)shape;
#endif
}

SphereCollider::~SphereCollider()
{
#ifdef DT_PHYSX_ENABLED
    if (m_actor) static_cast<PxRigidDynamic*>(m_actor)->release();
#endif
}

void SphereCollider::setCenter(const glm::vec3& center)
{
    m_center = center;
#ifdef DT_PHYSX_ENABLED
    if (!m_shape) return;
    PxTransform local(PxVec3(center.x, center.y, center.z));
    static_cast<PxShape*>(m_shape)->setLocalPose(local);
#endif
}

void SphereCollider::setRadius(float radius)
{
    m_radius = radius;
#ifdef DT_PHYSX_ENABLED
    if (!m_shape) return;
    static_cast<PxShape*>(m_shape)->setGeometry(PxSphereGeometry(radius));
#endif
}

void SphereCollider::setUseGravity(bool enabled)
{
    m_useGravity = enabled;
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;
    auto* actor = static_cast<PxRigidDynamic*>(m_actor);
    actor->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !enabled);
    actor->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, !enabled);
    if (enabled)
    {
        actor->setLinearVelocity(PxVec3(0.0f));
        actor->setAngularVelocity(PxVec3(0.0f));
        actor->wakeUp();
    }
#else
    (void)enabled;
#endif
}

glm::mat4 SphereCollider::getWorldTransform() const
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return glm::mat4(1.0f);

    PxTransform pose = static_cast<PxRigidDynamic*>(m_actor)->getGlobalPose();

    glm::mat4 translation = glm::translate(glm::mat4(1.0f),
        glm::vec3(pose.p.x, pose.p.y, pose.p.z));
    glm::quat rotation(pose.q.w, pose.q.x, pose.q.y, pose.q.z);
    glm::mat4 rotationMat = glm::mat4_cast(rotation);

    return translation * rotationMat;
#else
    return glm::mat4(1.0f);
#endif
}

void SphereCollider::syncTransform(const glm::mat4& worldTransform)
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;

    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    glm::decompose(worldTransform, scale, rotation, translation, skew, perspective);

    PxTransform pose(
        PxVec3(translation.x, translation.y, translation.z),
        PxQuat(rotation.x, rotation.y, rotation.z, rotation.w)
    );
    static_cast<PxRigidDynamic*>(m_actor)->setKinematicTarget(pose);
#else
    (void)worldTransform;
#endif
}

void SphereCollider::teleport(const glm::mat4& worldTransform)
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;

    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    glm::decompose(worldTransform, scale, rotation, translation, skew, perspective);

    PxTransform pose(
        PxVec3(translation.x, translation.y, translation.z),
        PxQuat(rotation.x, rotation.y, rotation.z, rotation.w)
    );

    auto* actor = static_cast<PxRigidDynamic*>(m_actor);
    actor->setGlobalPose(pose);
    actor->setLinearVelocity(PxVec3(0.0f));
    actor->setAngularVelocity(PxVec3(0.0f));
#else
    (void)worldTransform;
#endif
}

} // namespace DonTopo
```

- [ ] **Step 3: Commit**

Este task por sí solo no compila el `add_library` (falta añadir el `.cpp` a `engine/CMakeLists.txt`, ver Task 5) — no hacer commit todavía. Continuar directo a Task 2.

---

### Task 2: Clase `CapsuleCollider`

**Files:**
- Create: `engine/include/DonTopo/CapsuleCollider.h`
- Create: `engine/src/CapsuleCollider.cpp`

**Interfaces:**
- Consumes: nada de otras tasks.
- Produces: la clase `CapsuleCollider`, consumida por `GameObject` (Task 4), `PhysicsManager` (Task 5) y `EditorUI` (Task 6):
  ```cpp
  namespace DonTopo {
  class CapsuleCollider {
  public:
      CapsuleCollider(void* actor, void* shape, float radius, float halfHeight,
                       const glm::vec3& center, bool useGravity);
      ~CapsuleCollider();
      void setCenter(const glm::vec3& center);
      void setRadius(float radius);
      void setHalfHeight(float halfHeight);
      void setUseGravity(bool enabled);
      glm::vec3 getCenter() const;
      float     getRadius() const;
      float     getHalfHeight() const;
      bool      getUseGravity() const;
      bool      isDynamic() const;
      glm::mat4 getWorldTransform() const;
      void syncTransform(const glm::mat4& worldTransform);
      void teleport(const glm::mat4& worldTransform);
  };
  }
  ```

- [ ] **Step 1: Escribir `engine/include/DonTopo/CapsuleCollider.h`**

```cpp
#pragma once
#include <glm/glm.hpp>

namespace DonTopo {

// Componente único de física de tipo cápsula. Mismo patrón que BoxCollider
// (PxRigidDynamic siempre, useGravity alterna kinematic/dinámico). PhysX
// orienta PxCapsuleGeometry por defecto a lo largo del eje X local del
// shape; aquí se compone una rotación fija de 90° sobre Z en el localPose
// para que la "altura" quede en Y (cápsula de pie, tipo personaje).
class CapsuleCollider {
public:
    // actor/shape ya creados por PhysicsManager, con localPose ya puesto a
    // partir de center + la rotación fija de corrección de eje.
    CapsuleCollider(void* actor, void* shape, float radius, float halfHeight,
                     const glm::vec3& center, bool useGravity);
    ~CapsuleCollider();

    CapsuleCollider(const CapsuleCollider&)            = delete;
    CapsuleCollider& operator=(const CapsuleCollider&) = delete;

    // Offset local de la shape dentro del actor. Reaplica siempre la
    // rotación fija de corrección de eje junto con la traslación.
    void setCenter(const glm::vec3& center);
    // Radio de la cápsula (PxShape::setGeometry con nueva PxCapsuleGeometry).
    void setRadius(float radius);
    // Medio-alto de la cápsula (distancia entre los centros de las dos
    // semiesferas; PxShape::setGeometry con nueva PxCapsuleGeometry).
    void setHalfHeight(float halfHeight);
    // true: actor dinámico normal. false: actor kinematic + gravedad
    // desactivada, empujado desde el GameObject vía syncTransform.
    void setUseGravity(bool enabled);

    glm::vec3 getCenter() const      { return m_center; }
    float     getRadius() const      { return m_radius; }
    float     getHalfHeight() const  { return m_halfHeight; }
    bool      getUseGravity() const  { return m_useGravity; }

    bool isDynamic() const { return m_useGravity; }

    glm::mat4 getWorldTransform() const;
    void syncTransform(const glm::mat4& worldTransform);
    void teleport(const glm::mat4& worldTransform);

private:
#ifdef DT_PHYSX_ENABLED
    void* m_actor = nullptr; // physx::PxRigidDynamic*
    void* m_shape = nullptr; // physx::PxShape*
#endif
    float     m_radius;
    float     m_halfHeight;
    glm::vec3 m_center;
    bool      m_useGravity;
};

} // namespace DonTopo
```

- [ ] **Step 2: Escribir `engine/src/CapsuleCollider.cpp`**

```cpp
#include "DonTopo/CapsuleCollider.h"

#ifdef DT_PHYSX_ENABLED
#define GLM_ENABLE_EXPERIMENTAL
#include <PxPhysicsAPI.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

using namespace physx;

namespace {
    // PxCapsuleGeometry se orienta por defecto a lo largo del eje X local del
    // shape; esta rotación fija (90° sobre Z) mapea ese eje X a Y, dejando la
    // cápsula "de pie" en el espacio local del actor. Constante: nunca
    // cambia, solo se recompone con distintas traslaciones (center).
    PxQuat axisCorrection() { return PxQuat(PxHalfPi, PxVec3(0.0f, 0.0f, 1.0f)); }
}
#endif

namespace DonTopo {

CapsuleCollider::CapsuleCollider(void* actor, void* shape, float radius, float halfHeight,
                                 const glm::vec3& center, bool useGravity)
    : m_radius(radius)
    , m_halfHeight(halfHeight)
    , m_center(center)
    , m_useGravity(useGravity)
{
#ifdef DT_PHYSX_ENABLED
    m_actor = actor;
    m_shape = shape;
#else
    (void)actor;
    (void)shape;
#endif
}

CapsuleCollider::~CapsuleCollider()
{
#ifdef DT_PHYSX_ENABLED
    if (m_actor) static_cast<PxRigidDynamic*>(m_actor)->release();
#endif
}

void CapsuleCollider::setCenter(const glm::vec3& center)
{
    m_center = center;
#ifdef DT_PHYSX_ENABLED
    if (!m_shape) return;
    PxTransform local(PxVec3(center.x, center.y, center.z), axisCorrection());
    static_cast<PxShape*>(m_shape)->setLocalPose(local);
#endif
}

void CapsuleCollider::setRadius(float radius)
{
    m_radius = radius;
#ifdef DT_PHYSX_ENABLED
    if (!m_shape) return;
    static_cast<PxShape*>(m_shape)->setGeometry(PxCapsuleGeometry(radius, m_halfHeight));
#endif
}

void CapsuleCollider::setHalfHeight(float halfHeight)
{
    m_halfHeight = halfHeight;
#ifdef DT_PHYSX_ENABLED
    if (!m_shape) return;
    static_cast<PxShape*>(m_shape)->setGeometry(PxCapsuleGeometry(m_radius, halfHeight));
#endif
}

void CapsuleCollider::setUseGravity(bool enabled)
{
    m_useGravity = enabled;
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;
    auto* actor = static_cast<PxRigidDynamic*>(m_actor);
    actor->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !enabled);
    actor->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, !enabled);
    if (enabled)
    {
        actor->setLinearVelocity(PxVec3(0.0f));
        actor->setAngularVelocity(PxVec3(0.0f));
        actor->wakeUp();
    }
#else
    (void)enabled;
#endif
}

glm::mat4 CapsuleCollider::getWorldTransform() const
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return glm::mat4(1.0f);

    PxTransform pose = static_cast<PxRigidDynamic*>(m_actor)->getGlobalPose();

    glm::mat4 translation = glm::translate(glm::mat4(1.0f),
        glm::vec3(pose.p.x, pose.p.y, pose.p.z));
    glm::quat rotation(pose.q.w, pose.q.x, pose.q.y, pose.q.z);
    glm::mat4 rotationMat = glm::mat4_cast(rotation);

    return translation * rotationMat;
#else
    return glm::mat4(1.0f);
#endif
}

void CapsuleCollider::syncTransform(const glm::mat4& worldTransform)
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;

    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    glm::decompose(worldTransform, scale, rotation, translation, skew, perspective);

    PxTransform pose(
        PxVec3(translation.x, translation.y, translation.z),
        PxQuat(rotation.x, rotation.y, rotation.z, rotation.w)
    );
    static_cast<PxRigidDynamic*>(m_actor)->setKinematicTarget(pose);
#else
    (void)worldTransform;
#endif
}

void CapsuleCollider::teleport(const glm::mat4& worldTransform)
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;

    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    glm::decompose(worldTransform, scale, rotation, translation, skew, perspective);

    PxTransform pose(
        PxVec3(translation.x, translation.y, translation.z),
        PxQuat(rotation.x, rotation.y, rotation.z, rotation.w)
    );

    auto* actor = static_cast<PxRigidDynamic*>(m_actor);
    actor->setGlobalPose(pose);
    actor->setLinearVelocity(PxVec3(0.0f));
    actor->setAngularVelocity(PxVec3(0.0f));
#else
    (void)worldTransform;
#endif
}

} // namespace DonTopo
```

- [ ] **Step 3: Commit**

No compila aún (falta CMakeLists.txt, Task 5) — continuar directo a Task 3.

---

### Task 3: Clase `PlaneCollider`

**Files:**
- Create: `engine/include/DonTopo/PlaneCollider.h`
- Create: `engine/src/PlaneCollider.cpp`

**Interfaces:**
- Consumes: nada de otras tasks.
- Produces: la clase `PlaneCollider`, consumida por `GameObject` (Task 4), `PhysicsManager` (Task 5) y `EditorUI` (Task 6):
  ```cpp
  namespace DonTopo {
  class PlaneCollider {
  public:
      PlaneCollider(void* actor, void* shape, const glm::vec3& center);
      ~PlaneCollider();
      void setCenter(const glm::vec3& center);
      glm::vec3 getCenter() const;
      bool isDynamic() const; // siempre false
      glm::mat4 getWorldTransform() const;
      void syncTransform(const glm::mat4& worldTransform);
      void teleport(const glm::mat4& worldTransform);
  };
  }
  ```

- [ ] **Step 1: Escribir `engine/include/DonTopo/PlaneCollider.h`**

```cpp
#pragma once
#include <glm/glm.hpp>

namespace DonTopo {

// Componente único de física de tipo plano infinito. A diferencia de
// Box/Sphere/Capsule, siempre es kinematic + gravedad desactivada (un plano
// "cayendo" no tiene sentido físico) — no expone toggle Use Gravity.
// isDynamic() retorna false hardcoded: el motor siempre empuja la pose del
// GameObject hacia PhysX (syncTransform), nunca lee de vuelta.
class PlaneCollider {
public:
    // actor/shape ya creados por PhysicsManager, con localPose ya puesto a
    // partir de center + la rotación fija que mapea la normal por defecto a
    // +Y (mismo truco de eje que CapsuleCollider).
    PlaneCollider(void* actor, void* shape, const glm::vec3& center);
    ~PlaneCollider();

    PlaneCollider(const PlaneCollider&)            = delete;
    PlaneCollider& operator=(const PlaneCollider&) = delete;

    // Offset local de la shape dentro del actor. Reaplica siempre la
    // rotación fija de corrección de eje junto con la traslación.
    void setCenter(const glm::vec3& center);

    glm::vec3 getCenter() const { return m_center; }
    bool isDynamic() const { return false; }

    glm::mat4 getWorldTransform() const;
    void syncTransform(const glm::mat4& worldTransform);
    void teleport(const glm::mat4& worldTransform);

private:
#ifdef DT_PHYSX_ENABLED
    void* m_actor = nullptr; // physx::PxRigidDynamic*
    void* m_shape = nullptr; // physx::PxShape*
#endif
    glm::vec3 m_center;
};

} // namespace DonTopo
```

- [ ] **Step 2: Escribir `engine/src/PlaneCollider.cpp`**

```cpp
#include "DonTopo/PlaneCollider.h"

#ifdef DT_PHYSX_ENABLED
#define GLM_ENABLE_EXPERIMENTAL
#include <PxPhysicsAPI.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

using namespace physx;

namespace {
    // Mismo truco que CapsuleCollider: PxPlaneGeometry define la normal del
    // plano como el eje X local del shape. Esta rotación fija (90° sobre Z)
    // mapea ese eje X a Y, dejando la normal por defecto apuntando "arriba"
    // en el espacio local del actor.
    PxQuat axisCorrection() { return PxQuat(PxHalfPi, PxVec3(0.0f, 0.0f, 1.0f)); }
}
#endif

namespace DonTopo {

PlaneCollider::PlaneCollider(void* actor, void* shape, const glm::vec3& center)
    : m_center(center)
{
#ifdef DT_PHYSX_ENABLED
    m_actor = actor;
    m_shape = shape;
#else
    (void)actor;
    (void)shape;
#endif
}

PlaneCollider::~PlaneCollider()
{
#ifdef DT_PHYSX_ENABLED
    if (m_actor) static_cast<PxRigidDynamic*>(m_actor)->release();
#endif
}

void PlaneCollider::setCenter(const glm::vec3& center)
{
    m_center = center;
#ifdef DT_PHYSX_ENABLED
    if (!m_shape) return;
    PxTransform local(PxVec3(center.x, center.y, center.z), axisCorrection());
    static_cast<PxShape*>(m_shape)->setLocalPose(local);
#endif
}

glm::mat4 PlaneCollider::getWorldTransform() const
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return glm::mat4(1.0f);

    PxTransform pose = static_cast<PxRigidDynamic*>(m_actor)->getGlobalPose();

    glm::mat4 translation = glm::translate(glm::mat4(1.0f),
        glm::vec3(pose.p.x, pose.p.y, pose.p.z));
    glm::quat rotation(pose.q.w, pose.q.x, pose.q.y, pose.q.z);
    glm::mat4 rotationMat = glm::mat4_cast(rotation);

    return translation * rotationMat;
#else
    return glm::mat4(1.0f);
#endif
}

void PlaneCollider::syncTransform(const glm::mat4& worldTransform)
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;

    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    glm::decompose(worldTransform, scale, rotation, translation, skew, perspective);

    PxTransform pose(
        PxVec3(translation.x, translation.y, translation.z),
        PxQuat(rotation.x, rotation.y, rotation.z, rotation.w)
    );
    static_cast<PxRigidDynamic*>(m_actor)->setKinematicTarget(pose);
#else
    (void)worldTransform;
#endif
}

void PlaneCollider::teleport(const glm::mat4& worldTransform)
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;

    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    glm::decompose(worldTransform, scale, rotation, translation, skew, perspective);

    PxTransform pose(
        PxVec3(translation.x, translation.y, translation.z),
        PxQuat(rotation.x, rotation.y, rotation.z, rotation.w)
    );

    auto* actor = static_cast<PxRigidDynamic*>(m_actor);
    actor->setGlobalPose(pose);
    actor->setLinearVelocity(PxVec3(0.0f));
    actor->setAngularVelocity(PxVec3(0.0f));
#else
    (void)worldTransform;
#endif
}

} // namespace DonTopo
```

- [ ] **Step 3: Commit**

No compila aún — continuar directo a Task 4.

---

### Task 4: `GameObject` — 3 slots nuevos + `hasAnyCollider()`

**Files:**
- Modify: `engine/include/DonTopo/GameObject.h`

**Interfaces:**
- Consumes: `DonTopo::SphereCollider` (Task 1), `DonTopo::CapsuleCollider` (Task 2), `DonTopo::PlaneCollider` (Task 3).
- Produces:
  ```cpp
  void setSphereCollider(std::shared_ptr<SphereCollider> sc);
  const std::shared_ptr<SphereCollider>& getSphereCollider() const;
  bool hasSphereCollider() const;

  void setCapsuleCollider(std::shared_ptr<CapsuleCollider> cc);
  const std::shared_ptr<CapsuleCollider>& getCapsuleCollider() const;
  bool hasCapsuleCollider() const;

  void setPlaneCollider(std::shared_ptr<PlaneCollider> pc);
  const std::shared_ptr<PlaneCollider>& getPlaneCollider() const;
  bool hasPlaneCollider() const;

  bool hasAnyCollider() const;
  ```
  (usado por `PhysicsManager`-callers en Task 7 y por `EditorUI` en Task 6).

- [ ] **Step 1: Editar `engine/include/DonTopo/GameObject.h`**

El archivo completo queda:

```cpp
#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <memory>
#include "DonTopo/Mesh.h"
#include "DonTopo/SkinnedMesh.h"
#include "DonTopo/BoxCollider.h"
#include "DonTopo/SphereCollider.h"
#include "DonTopo/CapsuleCollider.h"
#include "DonTopo/PlaneCollider.h"

namespace DonTopo
{
    class GameObject
    {
        public:
            explicit GameObject(std::string name = "");

            GameObject* addChild(std::string childName);

            void setMesh(std::shared_ptr<Mesh> mesh) { m_mesh = std::move(mesh); }
            const std::shared_ptr<Mesh>& getMesh() const { return m_mesh; }
            bool hasMesh()   const { return m_mesh != nullptr; }
            bool isSkinned() const { return m_mesh && dynamic_cast<SkinnedMesh*>(m_mesh.get()) != nullptr; }
            SkinnedMesh* getSkinnedMesh() const { return m_mesh ? dynamic_cast<SkinnedMesh*>(m_mesh.get()) : nullptr; }

            void setBoxCollider(std::shared_ptr<BoxCollider> bc) { m_boxCollider = std::move(bc); }
            const std::shared_ptr<BoxCollider>& getBoxCollider() const { return m_boxCollider; }
            bool hasBoxCollider() const { return m_boxCollider != nullptr; }

            void setSphereCollider(std::shared_ptr<SphereCollider> sc) { m_sphereCollider = std::move(sc); }
            const std::shared_ptr<SphereCollider>& getSphereCollider() const { return m_sphereCollider; }
            bool hasSphereCollider() const { return m_sphereCollider != nullptr; }

            void setCapsuleCollider(std::shared_ptr<CapsuleCollider> cc) { m_capsuleCollider = std::move(cc); }
            const std::shared_ptr<CapsuleCollider>& getCapsuleCollider() const { return m_capsuleCollider; }
            bool hasCapsuleCollider() const { return m_capsuleCollider != nullptr; }

            void setPlaneCollider(std::shared_ptr<PlaneCollider> pc) { m_planeCollider = std::move(pc); }
            const std::shared_ptr<PlaneCollider>& getPlaneCollider() const { return m_planeCollider; }
            bool hasPlaneCollider() const { return m_planeCollider != nullptr; }

            // true si tiene cualquiera de los 4 tipos de collider — los 4 son
            // mutuamente excluyentes (impuesto por EditorUI, no por esta clase),
            // usado como guard único en el popup "Add".
            bool hasAnyCollider() const
            {
                return m_boxCollider || m_sphereCollider || m_capsuleCollider || m_planeCollider;
            }

            void updateWorldTransforms(const glm::mat4& parentWorld = glm::mat4(1.0f));

            template <typename Fn>
            void traverse(Fn fn)
            {
                fn(this);
                for (auto& c : children) c->traverse(fn);
            }

            std::string name;
            glm::mat4   localTransform {1.0f};
            glm::mat4   worldTransform {1.0f};
            GameObject* parent = nullptr;
            std::vector<std::unique_ptr<GameObject>> children;

            // El Renderer mantiene dos colecciones/pipelines separados (estático vs skinned),
            // por eso hacen falta dos índices en vez de un único meshIndex plano.
            int staticRenderIndex  = -1;
            int skinnedRenderIndex = -1;

        private:
            std::shared_ptr<Mesh> m_mesh;
            std::shared_ptr<BoxCollider> m_boxCollider;
            std::shared_ptr<SphereCollider> m_sphereCollider;
            std::shared_ptr<CapsuleCollider> m_capsuleCollider;
            std::shared_ptr<PlaneCollider> m_planeCollider;
    };
}
```

- [ ] **Step 2: Commit**

No compila aún (Task 5/6/7 pendientes) — continuar directo a Task 5.

---

### Task 5: `PhysicsManager` — 3 factories nuevas + `CMakeLists.txt`

**Files:**
- Modify: `engine/include/DonTopo/PhysicsManager.h`
- Modify: `engine/src/PhysicsManager.cpp`
- Modify: `engine/CMakeLists.txt`

**Interfaces:**
- Consumes: `DonTopo::SphereCollider`, `DonTopo::CapsuleCollider`, `DonTopo::PlaneCollider` (Tasks 1-3), sus constructores.
- Produces:
  ```cpp
  std::shared_ptr<SphereCollider> createSphereColliderComponent(
      float radius, const glm::vec3& center, const glm::mat4& worldTransform,
      bool useGravity, float density = 1.0f);

  std::shared_ptr<CapsuleCollider> createCapsuleColliderComponent(
      float radius, float halfHeight, const glm::vec3& center,
      const glm::mat4& worldTransform, bool useGravity, float density = 1.0f);

  std::shared_ptr<PlaneCollider> createPlaneColliderComponent(
      const glm::vec3& center, const glm::mat4& worldTransform);
  ```
  (usado por `sandbox/main.cpp` en Task 7 y por `EditorUI` en Task 6).

- [ ] **Step 1: Editar `engine/include/DonTopo/PhysicsManager.h`**

Reemplazar la línea 9 (`namespace DonTopo { class BoxCollider; }`) por:

```cpp
namespace DonTopo { class BoxCollider; class SphereCollider; class CapsuleCollider; class PlaneCollider; }
```

Añadir tras la declaración de `createBoxColliderComponent` (línea 27):

```cpp
    std::shared_ptr<SphereCollider> createSphereColliderComponent(float radius,
                                                                    const glm::vec3& center,
                                                                    const glm::mat4& worldTransform,
                                                                    bool useGravity,
                                                                    float density = 1.0f);

    std::shared_ptr<CapsuleCollider> createCapsuleColliderComponent(float radius,
                                                                      float halfHeight,
                                                                      const glm::vec3& center,
                                                                      const glm::mat4& worldTransform,
                                                                      bool useGravity,
                                                                      float density = 1.0f);

    std::shared_ptr<PlaneCollider> createPlaneColliderComponent(const glm::vec3& center,
                                                                  const glm::mat4& worldTransform);
```

- [ ] **Step 2: Editar `engine/src/PhysicsManager.cpp`**

Añadir includes tras la línea 2 (`#include "DonTopo/BoxCollider.h"`):

```cpp
#include "DonTopo/SphereCollider.h"
#include "DonTopo/CapsuleCollider.h"
#include "DonTopo/PlaneCollider.h"
```

Añadir el helper de corrección de eje dentro del bloque anónimo existente
(líneas 14-17, junto a `g_allocator`/`g_errorCallback`, dentro del
`#ifdef DT_PHYSX_ENABLED`):

```cpp
namespace {
    PxDefaultAllocator      g_allocator;
    PxDefaultErrorCallback  g_errorCallback;

    // Mismo truco usado en CapsuleCollider.cpp/PlaneCollider.cpp: PhysX
    // orienta PxCapsuleGeometry a lo largo de X y define la normal de
    // PxPlaneGeometry como el eje X local del shape. Esta rotación fija
    // (90° sobre Z) mapea ese eje X a Y en ambos casos.
    PxQuat axisCorrection() { return PxQuat(PxHalfPi, PxVec3(0.0f, 0.0f, 1.0f)); }
}
```

Añadir las 3 funciones nuevas justo después del cierre de
`createBoxColliderComponent` (tras la línea `}` que cierra esa función,
antes del bloque `#ifdef DT_PHYSX_ENABLED` de `raycast`):

```cpp
std::shared_ptr<SphereCollider> PhysicsManager::createSphereColliderComponent(
    float radius,
    const glm::vec3& center,
    const glm::mat4& worldTransform,
    bool useGravity,
    float density)
{
#ifdef DT_PHYSX_ENABLED
    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    glm::decompose(worldTransform, scale, rotation, translation, skew, perspective);

    PxTransform pose(
        PxVec3(translation.x, translation.y, translation.z),
        PxQuat(rotation.x, rotation.y, rotation.z, rotation.w)
    );

    auto* physics = static_cast<PxPhysics*>(m_physics);
    auto* material = static_cast<PxMaterial*>(m_material);
    auto* scene = static_cast<PxScene*>(m_scene);

    PxRigidDynamic* actor = physics->createRigidDynamic(pose);
    physxCheck(actor, "PxPhysics::createRigidDynamic");

    PxSphereGeometry geometry(radius);
    PxShape* shape = PxRigidActorExt::createExclusiveShape(*actor, geometry, *material);
    physxCheck(shape, "PxRigidActorExt::createExclusiveShape");
    shape->setLocalPose(PxTransform(PxVec3(center.x, center.y, center.z)));

    PxRigidBodyExt::updateMassAndInertia(*actor, density);

    actor->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !useGravity);
    actor->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, !useGravity);

    scene->addActor(*actor);

    return std::make_shared<SphereCollider>(actor, shape, radius, center, useGravity);
#else
    (void)worldTransform;
    (void)density;
    return std::make_shared<SphereCollider>(nullptr, nullptr, radius, center, useGravity);
#endif
}

std::shared_ptr<CapsuleCollider> PhysicsManager::createCapsuleColliderComponent(
    float radius,
    float halfHeight,
    const glm::vec3& center,
    const glm::mat4& worldTransform,
    bool useGravity,
    float density)
{
#ifdef DT_PHYSX_ENABLED
    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    glm::decompose(worldTransform, scale, rotation, translation, skew, perspective);

    PxTransform pose(
        PxVec3(translation.x, translation.y, translation.z),
        PxQuat(rotation.x, rotation.y, rotation.z, rotation.w)
    );

    auto* physics = static_cast<PxPhysics*>(m_physics);
    auto* material = static_cast<PxMaterial*>(m_material);
    auto* scene = static_cast<PxScene*>(m_scene);

    PxRigidDynamic* actor = physics->createRigidDynamic(pose);
    physxCheck(actor, "PxPhysics::createRigidDynamic");

    PxCapsuleGeometry geometry(radius, halfHeight);
    PxShape* shape = PxRigidActorExt::createExclusiveShape(*actor, geometry, *material);
    physxCheck(shape, "PxRigidActorExt::createExclusiveShape");
    shape->setLocalPose(PxTransform(PxVec3(center.x, center.y, center.z), axisCorrection()));

    PxRigidBodyExt::updateMassAndInertia(*actor, density);

    actor->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !useGravity);
    actor->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, !useGravity);

    scene->addActor(*actor);

    return std::make_shared<CapsuleCollider>(actor, shape, radius, halfHeight, center, useGravity);
#else
    (void)worldTransform;
    (void)density;
    return std::make_shared<CapsuleCollider>(nullptr, nullptr, radius, halfHeight, center, useGravity);
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
    glm::decompose(worldTransform, scale, rotation, translation, skew, perspective);

    PxTransform pose(
        PxVec3(translation.x, translation.y, translation.z),
        PxQuat(rotation.x, rotation.y, rotation.z, rotation.w)
    );

    auto* physics = static_cast<PxPhysics*>(m_physics);
    auto* material = static_cast<PxMaterial*>(m_material);
    auto* scene = static_cast<PxScene*>(m_scene);

    PxRigidDynamic* actor = physics->createRigidDynamic(pose);
    physxCheck(actor, "PxPhysics::createRigidDynamic");

    PxPlaneGeometry geometry;
    PxShape* shape = PxRigidActorExt::createExclusiveShape(*actor, geometry, *material);
    physxCheck(shape, "PxRigidActorExt::createExclusiveShape");
    shape->setLocalPose(PxTransform(PxVec3(center.x, center.y, center.z), axisCorrection()));

    // Sin updateMassAndInertia: un plano no tiene volumen, PhysX no puede
    // calcular masa/inercia sobre esa geometría. No hace falta — el actor
    // siempre queda kinematic (nunca se simula como cuerpo dinámico).
    actor->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, true);
    actor->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);

    scene->addActor(*actor);

    return std::make_shared<PlaneCollider>(actor, shape, center);
#else
    (void)worldTransform;
    return std::make_shared<PlaneCollider>(nullptr, nullptr, center);
#endif
}
```

- [ ] **Step 3: Añadir los 3 `.cpp` nuevos a `engine/CMakeLists.txt`**

Editar `engine/CMakeLists.txt` línea 15 (`src/BoxCollider.cpp`), añadir
justo debajo:

```cmake
    src/BoxCollider.cpp
    src/SphereCollider.cpp
    src/CapsuleCollider.cpp
    src/PlaneCollider.cpp
```

- [ ] **Step 4: Compilar**

Run: `.\build.bat`
Expected: build limpio, sin errores (cierra la cadena Task 1→5: las 3
clases nuevas ya tienen factory en `PhysicsManager` y están en el
`add_library`; solo falta consumirlas desde `GameObject`/`EditorUI`/`main.cpp`,
que ya compila porque Task 4 solo añadió slots sin uso todavía).

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/SphereCollider.h engine/src/SphereCollider.cpp \
        engine/include/DonTopo/CapsuleCollider.h engine/src/CapsuleCollider.cpp \
        engine/include/DonTopo/PlaneCollider.h engine/src/PlaneCollider.cpp \
        engine/include/DonTopo/GameObject.h \
        engine/include/DonTopo/PhysicsManager.h engine/src/PhysicsManager.cpp \
        engine/CMakeLists.txt
git commit -m "feat(physics): añadir componentes Sphere/Capsule/Plane Collider"
```

---

### Task 6: UI en `EditorUI` — secciones nuevas + popup "Add" con exclusividad

**Files:**
- Modify: `engine/include/DonTopo/EditorUI.h`
- Modify: `engine/src/EditorUI.cpp`

**Interfaces:**
- Consumes: `GameObject::has/get/setSphereCollider/CapsuleCollider/PlaneCollider`,
  `GameObject::hasAnyCollider()` (Task 4); `SphereCollider`/`CapsuleCollider`/
  `PlaneCollider` getters/setters (Tasks 1-3); `PhysicsManager::createSphere/
  Capsule/PlaneColliderComponent` (Task 5).
- Produces: nada consumido por otras tasks.

- [ ] **Step 1: Includes explícitos en `EditorUI.cpp`**

Añadir tras la línea 4 (`#include "DonTopo/BoxCollider.h"`):

```cpp
#include "DonTopo/SphereCollider.h"
#include "DonTopo/CapsuleCollider.h"
#include "DonTopo/PlaneCollider.h"
```

(Redundante con lo que ya arrastra `GameObject.h` desde Task 4, pero replica
el patrón explícito ya usado con `BoxCollider.h` en la misma línea.)

- [ ] **Step 2: Forward-declares y cache de edición en `EditorUI.h`**

Reemplazar la línea 13 (`class BoxCollider;`) por:

```cpp
class BoxCollider;
class SphereCollider;
class CapsuleCollider;
class PlaneCollider;
```

Añadir las 3 declaraciones de método privado junto a `drawBoxColliderSection()`
(línea 44):

```cpp
    void drawBoxColliderSection();
    void drawSphereColliderSection();
    void drawCapsuleColliderSection();
    void drawPlaneColliderSection();
    void drawAddComponentButton();
```

Añadir el estado de cache tras el bloque "Box Collider" existente (tras
`m_colliderDragActive` en línea 98, antes de `PhysicsManager* m_physics`):

```cpp
    // Sphere Collider – mismo patrón de cache que Box Collider.
    SphereCollider* m_sphereColliderCachedFor = nullptr;
    glm::vec3       m_editSphereCenter{0.0f};
    float           m_editSphereRadius{25.0f};
    bool            m_editSphereUseGravity = false;
    bool            m_sphereColliderDragActive = false;

    // Capsule Collider – mismo patrón de cache que Box Collider.
    CapsuleCollider* m_capsuleColliderCachedFor = nullptr;
    glm::vec3        m_editCapsuleCenter{0.0f};
    float            m_editCapsuleRadius{15.0f};
    float            m_editCapsuleHeight{50.0f};
    bool             m_editCapsuleUseGravity = false;
    bool             m_capsuleColliderDragActive = false;

    // Plane Collider – solo Center (sin Size/Use Gravity, siempre estático).
    PlaneCollider* m_planeColliderCachedFor = nullptr;
    glm::vec3      m_editPlaneCenter{0.0f};
    bool           m_planeColliderDragActive = false;
```

- [ ] **Step 2: Reemplazar el bloque de teleport en `drawProperties()` (`EditorUI.cpp`)**

Reemplazar (líneas 463-473):

```cpp
        if (m_selected->hasBoxCollider())
        {
            m_selected->updateWorldTransforms(m_selected->parent ? m_selected->parent->worldTransform
                                                                   : glm::mat4(1.0f));
            // teleport() (no syncTransform): funciona tanto si el actor es
            // dinámico (isDynamic()==true) como si es kinematic
            // (isDynamic()==false) — syncTransform usa setKinematicTarget,
            // que solo es válido en modo kinematic.
            m_selected->getBoxCollider()->teleport(m_selected->worldTransform);
        }
```

por:

```cpp
        if (m_selected->hasAnyCollider())
        {
            m_selected->updateWorldTransforms(m_selected->parent ? m_selected->parent->worldTransform
                                                                   : glm::mat4(1.0f));
            // teleport() (no syncTransform): funciona tanto si el actor es
            // dinámico (isDynamic()==true) como si es kinematic
            // (isDynamic()==false) — syncTransform usa setKinematicTarget,
            // que solo es válido en modo kinematic. hasAnyCollider() cubre
            // los 4 tipos porque son mutuamente excluyentes.
            if (m_selected->hasBoxCollider())
                m_selected->getBoxCollider()->teleport(m_selected->worldTransform);
            else if (m_selected->hasSphereCollider())
                m_selected->getSphereCollider()->teleport(m_selected->worldTransform);
            else if (m_selected->hasCapsuleCollider())
                m_selected->getCapsuleCollider()->teleport(m_selected->worldTransform);
            else if (m_selected->hasPlaneCollider())
                m_selected->getPlaneCollider()->teleport(m_selected->worldTransform);
        }
```

- [ ] **Step 3: Añadir las llamadas a las 3 secciones nuevas en `drawProperties()`**

Reemplazar (líneas 475-476):

```cpp
    drawBoxColliderSection();
    drawAddComponentButton();
```

por:

```cpp
    drawBoxColliderSection();
    drawSphereColliderSection();
    drawCapsuleColliderSection();
    drawPlaneColliderSection();
    drawAddComponentButton();
```

- [ ] **Step 4: Añadir `drawSphereColliderSection()`, `drawCapsuleColliderSection()`, `drawPlaneColliderSection()`**

Insertar estas 3 funciones nuevas justo después del cierre de
`drawBoxColliderSection()` (tras la línea `}` de esa función, línea 564),
antes de `void EditorUI::drawAddComponentButton()`:

```cpp
void EditorUI::drawSphereColliderSection()
{
    if (!m_selected->hasSphereCollider())
    {
        m_sphereColliderCachedFor = nullptr;
        return;
    }

    SphereCollider* sc = m_selected->getSphereCollider().get();

    if (m_sphereColliderCachedFor != sc)
    {
        m_editSphereCenter        = sc->getCenter();
        m_editSphereRadius        = sc->getRadius();
        m_editSphereUseGravity    = sc->getUseGravity();
        m_sphereColliderCachedFor = sc;
    }
    else if (sc->isDynamic() && !m_sphereColliderDragActive)
    {
        m_editSphereCenter = sc->getCenter();
        m_editSphereRadius = sc->getRadius();
    }

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Sphere Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##s1", &m_editSphereCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##s1", &m_editSphereCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##s1", &m_editSphereCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        ImGui::Text("Radius");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("##s2", &m_editSphereRadius, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        if (ImGui::Checkbox("Use Gravity", &m_editSphereUseGravity))
            colliderChanged = true;

        ImGui::TreePop();
    }

    m_sphereColliderDragActive = dragActive;

    if (colliderChanged)
    {
        sc->setCenter(m_editSphereCenter);
        sc->setRadius(m_editSphereRadius);
        sc->setUseGravity(m_editSphereUseGravity);
    }

    if (removeClicked)
    {
        m_selected->setSphereCollider(nullptr);
        m_sphereColliderCachedFor = nullptr;
    }
}

void EditorUI::drawCapsuleColliderSection()
{
    if (!m_selected->hasCapsuleCollider())
    {
        m_capsuleColliderCachedFor = nullptr;
        return;
    }

    CapsuleCollider* cc = m_selected->getCapsuleCollider().get();

    if (m_capsuleColliderCachedFor != cc)
    {
        m_editCapsuleCenter        = cc->getCenter();
        m_editCapsuleRadius        = cc->getRadius();
        m_editCapsuleHeight        = cc->getHalfHeight() * 2.0f;
        m_editCapsuleUseGravity    = cc->getUseGravity();
        m_capsuleColliderCachedFor = cc;
    }
    else if (cc->isDynamic() && !m_capsuleColliderDragActive)
    {
        m_editCapsuleCenter = cc->getCenter();
        m_editCapsuleRadius = cc->getRadius();
        m_editCapsuleHeight = cc->getHalfHeight() * 2.0f;
    }

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Capsule Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##k1", &m_editCapsuleCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##k1", &m_editCapsuleCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##k1", &m_editCapsuleCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        ImGui::Text("Radius");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("##k2", &m_editCapsuleRadius, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        ImGui::Text("Height");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("##k3", &m_editCapsuleHeight, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        if (ImGui::Checkbox("Use Gravity", &m_editCapsuleUseGravity))
            colliderChanged = true;

        ImGui::TreePop();
    }

    m_capsuleColliderDragActive = dragActive;

    if (colliderChanged)
    {
        cc->setCenter(m_editCapsuleCenter);
        cc->setRadius(m_editCapsuleRadius);
        cc->setHalfHeight(m_editCapsuleHeight * 0.5f);
        cc->setUseGravity(m_editCapsuleUseGravity);
    }

    if (removeClicked)
    {
        m_selected->setCapsuleCollider(nullptr);
        m_capsuleColliderCachedFor = nullptr;
    }
}

void EditorUI::drawPlaneColliderSection()
{
    if (!m_selected->hasPlaneCollider())
    {
        m_planeColliderCachedFor = nullptr;
        return;
    }

    PlaneCollider* pc = m_selected->getPlaneCollider().get();

    if (m_planeColliderCachedFor != pc)
    {
        m_editPlaneCenter        = pc->getCenter();
        m_planeColliderCachedFor = pc;
    }

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Plane Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##p1", &m_editPlaneCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##p1", &m_editPlaneCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##p1", &m_editPlaneCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        ImGui::TreePop();
    }

    m_planeColliderDragActive = dragActive;

    if (colliderChanged)
        pc->setCenter(m_editPlaneCenter);

    if (removeClicked)
    {
        m_selected->setPlaneCollider(nullptr);
        m_planeColliderCachedFor = nullptr;
    }
}
```

- [ ] **Step 5: Actualizar `drawAddComponentButton()` — exclusividad mutua + 3 entradas nuevas**

Reemplazar la función completa (líneas 566-586):

```cpp
void EditorUI::drawAddComponentButton()
{
    ImGui::Separator();
    if (ImGui::Button("Add"))
        ImGui::OpenPopup("AddComponentPopup");

    if (ImGui::BeginPopup("AddComponentPopup"))
    {
        bool alreadyHasAny = m_selected->hasAnyCollider();
        ImGui::BeginDisabled(alreadyHasAny);

        if (ImGui::Selectable("Box Collider") && !alreadyHasAny && m_physics)
        {
            m_selected->setBoxCollider(m_physics->createBoxColliderComponent(
                glm::vec3(25.0f, 25.0f, 25.0f), glm::vec3(0.0f),
                m_selected->worldTransform, /*useGravity=*/false));
            m_colliderCachedFor = nullptr;
        }

        if (ImGui::Selectable("Sphere Collider") && !alreadyHasAny && m_physics)
        {
            m_selected->setSphereCollider(m_physics->createSphereColliderComponent(
                25.0f, glm::vec3(0.0f), m_selected->worldTransform, /*useGravity=*/false));
            m_sphereColliderCachedFor = nullptr;
        }

        if (ImGui::Selectable("Capsule Collider") && !alreadyHasAny && m_physics)
        {
            m_selected->setCapsuleCollider(m_physics->createCapsuleColliderComponent(
                15.0f, 25.0f, glm::vec3(0.0f), m_selected->worldTransform, /*useGravity=*/false));
            m_capsuleColliderCachedFor = nullptr;
        }

        if (ImGui::Selectable("Plane Collider") && !alreadyHasAny && m_physics)
        {
            m_selected->setPlaneCollider(m_physics->createPlaneColliderComponent(
                glm::vec3(0.0f), m_selected->worldTransform));
            m_planeColliderCachedFor = nullptr;
        }

        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
}
```

- [ ] **Step 6: Compilar**

Run: `.\build.bat`
Expected: build limpio, sin errores.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/EditorUI.h engine/src/EditorUI.cpp
git commit -m "feat(editor): UI para añadir/editar/borrar Sphere/Capsule/Plane Collider"
```

---

### Task 7: `sandbox/main.cpp` — loop de sync y limpieza

**Files:**
- Modify: `sandbox/src/main.cpp`

**Interfaces:**
- Consumes: `GameObject::hasSphereCollider/getSphereCollider/setSphereCollider`,
  `...Capsule...`, `...Plane...` (Task 4); `SphereCollider::isDynamic()/
  getWorldTransform()/syncTransform()` (Task 1), análogos Capsule (Task 2) y
  Plane (Task 3, `syncTransform` únicamente — `isDynamic()` siempre false).

- [ ] **Step 1: Extender el traverse del loop principal (líneas 200-217)**

Reemplazar:

```cpp
            root.traverse([&](DonTopo::GameObject* go) {
                if (go->hasBoxCollider())
                {
                    if (go->getBoxCollider()->isDynamic())
                    {
                        go->worldTransform = go->getBoxCollider()->getWorldTransform();
                        // Mantener localTransform sincronizado con la pose física:
                        // si luego se desactiva la gravedad (toggle a kinematic),
                        // updateWorldTransforms() recalculará worldTransform a
                        // partir de localTransform, y sin este refresco usaría el
                        // valor stale de antes de caer, provocando un salto de
                        // vuelta a esa posición vieja.
                        glm::mat4 parentWorld = go->parent ? go->parent->worldTransform : glm::mat4(1.0f);
                        go->localTransform = glm::inverse(parentWorld) * go->worldTransform;
                    }
                    else
                        go->getBoxCollider()->syncTransform(go->worldTransform);
                }

                if (go->staticRenderIndex >= 0)
                    renderer.setTransform(go->staticRenderIndex, go->worldTransform);

                if (go->skinnedRenderIndex >= 0)
                {
                    renderer.updateAnimation(go->skinnedRenderIndex, dt);
                    renderer.setSkinnedTransform(go->skinnedRenderIndex, go->worldTransform);
                }
            });
```

por:

```cpp
            root.traverse([&](DonTopo::GameObject* go) {
                if (go->hasBoxCollider())
                {
                    if (go->getBoxCollider()->isDynamic())
                    {
                        go->worldTransform = go->getBoxCollider()->getWorldTransform();
                        glm::mat4 parentWorld = go->parent ? go->parent->worldTransform : glm::mat4(1.0f);
                        go->localTransform = glm::inverse(parentWorld) * go->worldTransform;
                    }
                    else
                        go->getBoxCollider()->syncTransform(go->worldTransform);
                }

                if (go->hasSphereCollider())
                {
                    if (go->getSphereCollider()->isDynamic())
                    {
                        go->worldTransform = go->getSphereCollider()->getWorldTransform();
                        glm::mat4 parentWorld = go->parent ? go->parent->worldTransform : glm::mat4(1.0f);
                        go->localTransform = glm::inverse(parentWorld) * go->worldTransform;
                    }
                    else
                        go->getSphereCollider()->syncTransform(go->worldTransform);
                }

                if (go->hasCapsuleCollider())
                {
                    if (go->getCapsuleCollider()->isDynamic())
                    {
                        go->worldTransform = go->getCapsuleCollider()->getWorldTransform();
                        glm::mat4 parentWorld = go->parent ? go->parent->worldTransform : glm::mat4(1.0f);
                        go->localTransform = glm::inverse(parentWorld) * go->worldTransform;
                    }
                    else
                        go->getCapsuleCollider()->syncTransform(go->worldTransform);
                }

                // Plane Collider siempre es kinematic (isDynamic()==false
                // hardcoded) — nunca lee pose de PhysX, solo empuja la del
                // GameObject.
                if (go->hasPlaneCollider())
                    go->getPlaneCollider()->syncTransform(go->worldTransform);

                if (go->staticRenderIndex >= 0)
                    renderer.setTransform(go->staticRenderIndex, go->worldTransform);

                if (go->skinnedRenderIndex >= 0)
                {
                    renderer.updateAnimation(go->skinnedRenderIndex, dt);
                    renderer.setSkinnedTransform(go->skinnedRenderIndex, go->worldTransform);
                }
            });
```

- [ ] **Step 2: Extender la limpieza al cierre (líneas 239-241)**

Reemplazar:

```cpp
        root.traverse([](DonTopo::GameObject* go) {
            go->setBoxCollider(nullptr);
        });
```

por:

```cpp
        root.traverse([](DonTopo::GameObject* go) {
            go->setBoxCollider(nullptr);
            go->setSphereCollider(nullptr);
            go->setCapsuleCollider(nullptr);
            go->setPlaneCollider(nullptr);
        });
```

- [ ] **Step 3: Compilar**

Run: `.\build.bat`
Expected: build limpio, sin errores.

- [ ] **Step 4: Prueba manual (sigue el plan de verificación del spec)**

Ejecutar el binario del sandbox y comprobar, en este orden (ver también
sección "Plan de verificación manual" en
`docs/superpowers/specs/2026-07-06-sphere-capsule-plane-collider-design.md`):

1. Seleccionar un GameObject sin física (p.ej. `sphere`) → botón "Add" →
   las 4 opciones (Box/Sphere/Capsule/Plane) están habilitadas.
2. Click "Sphere Collider" → aparece sección con Center (0,0,0), Radius 25,
   Use Gravity desactivado. Reabrir "Add" → las otras 3 opciones quedan
   deshabilitadas (grayed out).
3. Activar "Use Gravity" → la esfera cae con la gravedad global; editar
   Radius/Center mientras cae no la congela ni resetea posición.
4. Click en "x" de la sección Sphere Collider → sección desaparece; "Add"
   vuelve a mostrar las 4 opciones habilitadas.
5. Repetir con "Capsule Collider" sobre el mismo GameObject → verificar que
   se ve/orienta vertical (eje Y), no acostada en X; Radius/Height editables
   en vivo mientras cae.
6. Añadir "Plane Collider" a un GameObject → sección solo con Center, sin
   checkbox Use Gravity; el plano no cae nunca aunque se edite Center.
7. Añadir cualquiera de los 4 tipos y borrar el GameObject entero
   (Delete/menú contextual) → sin crash, ni siguiendo en el editor ni al
   cerrar la app.
8. Cerrar la app con un Sphere/Capsule/Plane collider activo en la escena →
   sin crash (confirma orden de destrucción replicado en el traverse de
   limpieza de `main.cpp`).

- [ ] **Step 5: Commit**

```bash
git add sandbox/src/main.cpp
git commit -m "feat(sandbox): sincronizar Sphere/Capsule/Plane Collider en el loop principal"
```
