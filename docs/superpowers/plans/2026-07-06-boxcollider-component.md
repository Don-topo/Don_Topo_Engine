# Componente Box Collider desde editor — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Añadir un botón "Add" en el panel Properties del editor que permita crear un componente Box Collider en el GameObject seleccionado, editar su Center/Size/Use Gravity, y eliminarlo — integrado en tiempo real con PhysX vía `PhysicsManager`.

**Architecture:** Fusionar las clases `BoxCollider` (estático) y `RigidBody` (dinámico) en una única `BoxCollider` que siempre usa `physx::PxRigidDynamic`, alternando `kinematic`/`disableGravity` flags según el toggle "Use Gravity". `GameObject` pasa a tener un solo slot de física. `EditorUI` gana acceso a `PhysicsManager*` (wiring vía `Renderer`, mismo patrón que `setOnAxisSelected`) para poder crear el componente al pulsar "Add".

**Tech Stack:** C++20, PhysX (`DT_PHYSX_ENABLED`), Dear ImGui, GLM.

## Global Constraints

- Spec de referencia: `docs/superpowers/specs/2026-07-06-boxcollider-component-design.md`.
- Un solo Box Collider por GameObject (no lista de componentes).
- `Center` es offset local dentro del actor (`PxShape::localPose`), nunca se mezcla con la pose del GameObject.
- Togglear "Use Gravity" NO destruye/recrea el `PxRigidActor`, solo cambia flags.
- Sin framework de tests unitarios en este repo (proyecto de motor gráfico/físico); verificación es build limpio (`.\build.bat`) + prueba manual descrita en el spec.
- No hay soporte `#ifdef DT_PHYSX_ENABLED`-off a verificar en este plan: se asume PhysX habilitado (como hoy en el resto del engine); el código debe seguir compilando en ambos casos porque las clases existentes ya usan ese patrón — replicarlo.

---

### Task 1: Clase `BoxCollider` unificada (fusiona `RigidBody`)

**Files:**
- Modify: `engine/include/DonTopo/BoxCollider.h`
- Modify: `engine/src/BoxCollider.cpp`
- Delete: `engine/include/DonTopo/RigidBody.h`
- Delete: `engine/src/RigidBody.cpp`
- Modify: `engine/CMakeLists.txt:16` (quitar línea `src/RigidBody.cpp`)

**Interfaces:**
- Consumes: nada de otras tasks.
- Produces: la clase `BoxCollider` que consumirán `GameObject` (Task 2), `PhysicsManager` (Task 3) y `EditorUI` (Task 6):
  ```cpp
  namespace DonTopo {
  class BoxCollider {
  public:
      BoxCollider(void* actor, void* shape, const glm::vec3& halfExtents,
                  const glm::vec3& center, bool useGravity);
      ~BoxCollider();
      BoxCollider(const BoxCollider&) = delete;
      BoxCollider& operator=(const BoxCollider&) = delete;

      void setCenter(const glm::vec3& center);
      void setHalfExtents(const glm::vec3& halfExtents);
      void setUseGravity(bool enabled);

      glm::vec3 getCenter() const { return m_center; }
      glm::vec3 getHalfExtents() const { return m_halfExtents; }
      bool getUseGravity() const { return m_useGravity; }
      bool isDynamic() const { return m_useGravity; }

      glm::mat4 getWorldTransform() const;
      void syncTransform(const glm::mat4& worldTransform);
      void teleport(const glm::mat4& worldTransform);
  private: ... };
  }
  ```

- [ ] **Step 1: Escribir el nuevo header `BoxCollider.h`**

```cpp
#pragma once
#include <glm/glm.hpp>

namespace DonTopo {

// Componente único de física de tipo caja. Siempre respaldado por un
// physx::PxRigidDynamic (nunca PxRigidStatic): con useGravity=false el actor
// queda kinematic + gravedad desactivada (se mueve empujado desde el
// GameObject); con useGravity=true PhysX lo simula normal y su pose se lee
// de vuelta hacia el GameObject cada frame. Togglear useGravity solo cambia
// flags del actor existente, nunca lo destruye/recrea.
class BoxCollider {
public:
    // actor: physx::PxRigidDynamic* ya creado y añadido a la escena por
    // PhysicsManager. shape: physx::PxShape* de geometría caja adjunta a ese
    // actor, con localPose ya puesto a partir de center.
    BoxCollider(void* actor, void* shape, const glm::vec3& halfExtents,
                const glm::vec3& center, bool useGravity);
    ~BoxCollider();

    BoxCollider(const BoxCollider&)            = delete;
    BoxCollider& operator=(const BoxCollider&) = delete;

    // Offset local de la shape dentro del actor (PxShape::setLocalPose).
    void setCenter(const glm::vec3& center);
    // Medio-tamaño de la caja (PxShape::setGeometry con nueva PxBoxGeometry).
    void setHalfExtents(const glm::vec3& halfExtents);
    // true: actor dinámico normal (cae con la gravedad de la escena).
    // false: actor kinematic + gravedad desactivada (no cae, se empuja
    // desde el GameObject vía syncTransform).
    void setUseGravity(bool enabled);

    glm::vec3 getCenter() const       { return m_center; }
    glm::vec3 getHalfExtents() const  { return m_halfExtents; }
    bool      getUseGravity() const   { return m_useGravity; }

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
    // cual sea el modo, y resetea su velocidad a cero. Válido en ambos
    // modos (dinámico o kinematic) — pensado para ediciones puntuales desde
    // el Transform panel del editor, no para el empuje continuo por frame
    // (eso es syncTransform).
    void teleport(const glm::mat4& worldTransform);

private:
#ifdef DT_PHYSX_ENABLED
    void* m_actor = nullptr; // physx::PxRigidDynamic*
    void* m_shape = nullptr; // physx::PxShape*
#endif
    glm::vec3 m_halfExtents;
    glm::vec3 m_center;
    bool      m_useGravity;
};

} // namespace DonTopo
```

- [ ] **Step 2: Escribir la nueva implementación `BoxCollider.cpp`**

```cpp
#include "DonTopo/BoxCollider.h"

#ifdef DT_PHYSX_ENABLED
#define GLM_ENABLE_EXPERIMENTAL
#include <PxPhysicsAPI.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

using namespace physx;
#endif

namespace DonTopo {

BoxCollider::BoxCollider(void* actor, void* shape, const glm::vec3& halfExtents,
                         const glm::vec3& center, bool useGravity)
    : m_halfExtents(halfExtents)
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

BoxCollider::~BoxCollider()
{
#ifdef DT_PHYSX_ENABLED
    if (m_actor) static_cast<PxRigidDynamic*>(m_actor)->release();
#endif
}

void BoxCollider::setCenter(const glm::vec3& center)
{
    m_center = center;
#ifdef DT_PHYSX_ENABLED
    if (!m_shape) return;
    PxTransform local(PxVec3(center.x, center.y, center.z));
    static_cast<PxShape*>(m_shape)->setLocalPose(local);
#endif
}

void BoxCollider::setHalfExtents(const glm::vec3& halfExtents)
{
    m_halfExtents = halfExtents;
#ifdef DT_PHYSX_ENABLED
    if (!m_shape) return;
    static_cast<PxShape*>(m_shape)->setGeometry(
        PxBoxGeometry(halfExtents.x, halfExtents.y, halfExtents.z));
#endif
}

void BoxCollider::setUseGravity(bool enabled)
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

glm::mat4 BoxCollider::getWorldTransform() const
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

void BoxCollider::syncTransform(const glm::mat4& worldTransform)
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

void BoxCollider::teleport(const glm::mat4& worldTransform)
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

- [ ] **Step 3: Borrar `RigidBody.h` y `RigidBody.cpp`**

```bash
rm engine/include/DonTopo/RigidBody.h engine/src/RigidBody.cpp
```

- [ ] **Step 4: Quitar `src/RigidBody.cpp` de `engine/CMakeLists.txt`**

Editar `engine/CMakeLists.txt` línea 16 (dentro de la lista de fuentes que incluye `src/BoxCollider.cpp` en la línea 15): borrar la línea `    src/RigidBody.cpp`.

- [ ] **Step 5: Commit**

Este task por sí solo no compila (Task 2/3/4 aún referencian la API vieja) — no hacer commit todavía. Continuar directo a Task 2.

---

### Task 2: `GameObject` — slot único de física

**Files:**
- Modify: `engine/include/DonTopo/GameObject.h`

**Interfaces:**
- Consumes: `DonTopo::BoxCollider` (Task 1).
- Produces:
  ```cpp
  void setBoxCollider(std::shared_ptr<BoxCollider> bc);
  const std::shared_ptr<BoxCollider>& getBoxCollider() const;
  bool hasBoxCollider() const;
  ```
  (usado por `PhysicsManager`-callers en Task 4 y por `EditorUI` en Task 6).

- [ ] **Step 1: Editar `GameObject.h`**

Reemplazar el include de `RigidBody.h` y los tres bloques de
`m_collider`/`m_rigidBody` por un único slot. El archivo completo queda:

```cpp
#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <memory>
#include "DonTopo/Mesh.h"
#include "DonTopo/SkinnedMesh.h"
#include "DonTopo/BoxCollider.h"

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
    };
}
```

- [ ] **Step 2: Commit**

No compilará aún (Task 3/4 pendientes) — seguir directo a Task 3.

---

### Task 3: `PhysicsManager` — factory único `createBoxColliderComponent`

**Files:**
- Modify: `engine/include/DonTopo/PhysicsManager.h`
- Modify: `engine/src/PhysicsManager.cpp`

**Interfaces:**
- Consumes: `DonTopo::BoxCollider` (Task 1), constructor
  `BoxCollider(void* actor, void* shape, const glm::vec3& halfExtents, const glm::vec3& center, bool useGravity)`.
- Produces:
  ```cpp
  std::shared_ptr<BoxCollider> createBoxColliderComponent(
      const glm::vec3& halfExtents,
      const glm::vec3& center,
      const glm::mat4& worldTransform,
      bool useGravity,
      float density = 1.0f);
  ```
  (usado por `sandbox/main.cpp` en Task 4 y por `EditorUI` en Task 6).

- [ ] **Step 1: Editar `PhysicsManager.h`**

Reemplazar las dos declaraciones `createBoxCollider`/`createDynamicBoxCollider`
por una sola. El bloque público queda:

```cpp
    void init();
    void shutdown();

    std::shared_ptr<BoxCollider> createBoxColliderComponent(const glm::vec3& halfExtents,
                                                              const glm::vec3& center,
                                                              const glm::mat4& worldTransform,
                                                              bool useGravity,
                                                              float density = 1.0f);

    void stepSimulation(float dt);
```

Quitar el forward-declare `class RigidBody;` (ya no existe).

- [ ] **Step 2: Editar `PhysicsManager.cpp`**

Reemplazar `createBoxCollider` y `createDynamicBoxCollider` (y quitar el
`#include "DonTopo/RigidBody.h"`) por una única función. Se crea el actor
vacío con `PxPhysics::createRigidDynamic` (sin geometría) y se le adjunta
la shape caja aparte vía `PxRigidActorExt::createExclusiveShape`, para
poder quedarnos con el puntero `PxShape*` que `BoxCollider` necesita para
`setCenter`/`setHalfExtents`:

```cpp
std::shared_ptr<BoxCollider> PhysicsManager::createBoxColliderComponent(
    const glm::vec3& halfExtents,
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

    PxBoxGeometry geometry(halfExtents.x, halfExtents.y, halfExtents.z);
    PxShape* shape = PxRigidActorExt::createExclusiveShape(*actor, geometry, *material);
    physxCheck(shape, "PxRigidActorExt::createExclusiveShape");
    shape->setLocalPose(PxTransform(PxVec3(center.x, center.y, center.z)));

    PxRigidBodyExt::updateMassAndInertia(*actor, density);

    actor->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !useGravity);
    actor->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, !useGravity);

    scene->addActor(*actor);

    return std::make_shared<BoxCollider>(actor, shape, halfExtents, center, useGravity);
#else
    (void)worldTransform;
    (void)density;
    return std::make_shared<BoxCollider>(nullptr, nullptr, halfExtents, center, useGravity);
#endif
}
```

(`PxRigidActorExt` y `PxRigidBodyExt` están en `<PxPhysicsAPI.h>`, ya
incluido en este archivo — no hace falta un include nuevo.)

- [ ] **Step 3: Commit**

Aún no compila del todo (Task 4 pendiente en `sandbox/main.cpp`) — seguir
directo a Task 4.

---

### Task 4: Migrar demo `sandbox/main.cpp` y loop principal

**Files:**
- Modify: `sandbox/src/main.cpp`

**Interfaces:**
- Consumes: `GameObject::setBoxCollider/hasBoxCollider/getBoxCollider` (Task 2),
  `PhysicsManager::createBoxColliderComponent` (Task 3),
  `BoxCollider::isDynamic()/getWorldTransform()/syncTransform()` (Task 1).

- [ ] **Step 1: Migrar creación del suelo (línea 67-68)**

Reemplazar:

```cpp
        glm::mat4 floorColliderPose = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, floorY - 0.5f, 0.0f));
        floorNode->setCollider(physics.createBoxCollider(glm::vec3(500.0f, 0.5f, 500.0f), floorColliderPose));
```

por:

```cpp
        glm::mat4 floorColliderPose = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, floorY - 0.5f, 0.0f));
        floorNode->setBoxCollider(physics.createBoxColliderComponent(
            glm::vec3(500.0f, 0.5f, 500.0f), glm::vec3(0.0f), floorColliderPose, /*useGravity=*/false));
```

- [ ] **Step 2: Migrar creación del cubo (línea 75)**

Reemplazar:

```cpp
        cube->setRigidBody(physics.createDynamicBoxCollider(glm::vec3(25.0f, 25.0f, 25.0f), cube->worldTransform));
```

por:

```cpp
        cube->setBoxCollider(physics.createBoxColliderComponent(
            glm::vec3(25.0f, 25.0f, 25.0f), glm::vec3(0.0f), cube->worldTransform, /*useGravity=*/true));
```

- [ ] **Step 3: Migrar el traverse del loop principal (líneas 197-212)**

Reemplazar:

```cpp
            root.traverse([&](DonTopo::GameObject* go) {
                if (go->hasRigidBody())
                    go->worldTransform = go->getRigidBody()->getWorldTransform();

                if (go->staticRenderIndex >= 0)
                    renderer.setTransform(go->staticRenderIndex, go->worldTransform);

                if (go->skinnedRenderIndex >= 0)
                {
                    renderer.updateAnimation(go->skinnedRenderIndex, dt);
                    renderer.setSkinnedTransform(go->skinnedRenderIndex, go->worldTransform);
                }

                if (go->hasCollider())
                    go->getCollider()->syncTransform(go->worldTransform);
            });
```

por:

```cpp
            root.traverse([&](DonTopo::GameObject* go) {
                if (go->hasBoxCollider())
                {
                    if (go->getBoxCollider()->isDynamic())
                        go->worldTransform = go->getBoxCollider()->getWorldTransform();
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

- [ ] **Step 4: Migrar limpieza al cierre (líneas 224-227)**

Reemplazar:

```cpp
        root.traverse([](DonTopo::GameObject* go) {
            go->setCollider(nullptr);
            go->setRigidBody(nullptr);
        });
```

por:

```cpp
        root.traverse([](DonTopo::GameObject* go) {
            go->setBoxCollider(nullptr);
        });
```

- [ ] **Step 5: Compilar**

Run: `.\build.bat`
Expected: build limpio, sin errores (esto cierra la cadena Task 1→4: ya no
queda ninguna referencia a `RigidBody`, `hasCollider`, `hasRigidBody`,
`createBoxCollider`/`createDynamicBoxCollider` en el repo).

- [ ] **Step 6: Prueba manual rápida**

Ejecutar `build-ninja/sandbox/DonTopoSandbox.exe` (o el binario que genere
`build.bat` — comprobar el nombre exacto en `build-ninja/sandbox/` si
difiere). Confirmar en consola/ventana: el cubo demo sigue cayendo sobre el
suelo igual que antes de este cambio (mismo comportamiento, API nueva por
debajo).

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/BoxCollider.h engine/src/BoxCollider.cpp \
        engine/include/DonTopo/GameObject.h \
        engine/include/DonTopo/PhysicsManager.h engine/src/PhysicsManager.cpp \
        engine/CMakeLists.txt sandbox/src/main.cpp
git rm engine/include/DonTopo/RigidBody.h engine/src/RigidBody.cpp
git commit -m "feat(physics): fusionar BoxCollider y RigidBody en un componente único"
```

---

### Task 5: Wiring `PhysicsManager*` en `Renderer`/`EditorUI`

**Files:**
- Modify: `engine/include/DonTopo/EditorUI.h`
- Modify: `engine/src/EditorUI.cpp`
- Modify: `engine/include/DonTopo/Renderer.h`
- Modify: `sandbox/src/main.cpp`

**Interfaces:**
- Consumes: `DonTopo::PhysicsManager` (clase existente, Task 3 solo le añadió
  un método).
- Produces: `EditorUI::setPhysicsManager(PhysicsManager*)` y
  `Renderer::setPhysicsManager(PhysicsManager*)`, y el miembro
  `PhysicsManager* m_physics` en `EditorUI`, consumido por Task 6.

- [ ] **Step 1: Declarar el forward-declare y setter en `EditorUI.h`**

Añadir tras `class GameObject;` (línea 11):

```cpp
class PhysicsManager;
```

Añadir el setter público, junto a `setOnDelete`/`setOnAxisSelected` (tras
línea 28):

```cpp
    // Puntero no-propietario: PhysicsManager vive en main.cpp, fuera del
    // ciclo de vida del EditorUI. Necesario para crear el actor PhysX al
    // pulsar "Add > Box Collider" desde el panel Properties.
    void setPhysicsManager(PhysicsManager* physics) { m_physics = physics; }
```

Añadir el miembro privado, junto a los demás miembros (tras
`m_onAxisSelected` en línea 56):

```cpp
    PhysicsManager* m_physics = nullptr;
```

- [ ] **Step 2: Incluir `PhysicsManager.h` en `EditorUI.cpp`**

Añadir tras el include de `GameObject.h` (línea 2):

```cpp
#include "DonTopo/PhysicsManager.h"
#include "DonTopo/BoxCollider.h"
```

- [ ] **Step 3: Forward setter en `Renderer.h`**

Añadir forward-declare junto a `class GameObject;` en `Renderer.h` (buscar
dónde está declarado y añadir al lado):

```cpp
class PhysicsManager;
```

Añadir el forwarder junto a `setOnAxisSelected` (`Renderer.h:34`):

```cpp
            void setPhysicsManager(PhysicsManager* physics) { m_editorUI.setPhysicsManager(physics); }
```

- [ ] **Step 4: Llamar al setter desde `main.cpp`**

En `sandbox/src/main.cpp`, justo después de `renderer.setSceneRoot(&root);`
(línea 117), añadir:

```cpp
        renderer.setPhysicsManager(&physics);
```

- [ ] **Step 5: Compilar**

Run: `.\build.bat`
Expected: build limpio, sin errores.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/EditorUI.h engine/src/EditorUI.cpp \
        engine/include/DonTopo/Renderer.h sandbox/src/main.cpp
git commit -m "feat(editor): conectar PhysicsManager al EditorUI"
```

---

### Task 6: UI Box Collider en Properties (Add / editar / borrar)

**Files:**
- Modify: `engine/include/DonTopo/EditorUI.h`
- Modify: `engine/src/EditorUI.cpp`

**Interfaces:**
- Consumes: `GameObject::hasBoxCollider/getBoxCollider/setBoxCollider` (Task 2),
  `BoxCollider::getCenter/getHalfExtents/getUseGravity/setCenter/setHalfExtents/setUseGravity/isDynamic/teleport`
  (Task 1), `PhysicsManager::createBoxColliderComponent` (Task 3),
  `EditorUI::m_physics` (Task 5).
- Produces: nada consumido por otras tasks (última task del plan).

- [ ] **Step 1: Añadir cache de edición del Box Collider en `EditorUI.h`**

Añadir tras los campos de cache de Transform (`m_transformDragActive`,
línea 79):

```cpp
    // Box Collider – mismo patrón de cache que Transform: persiste entre
    // frames para que los DragFloat acumulen el delta del arrastre, y se
    // resincroniza con el BoxCollider real al cambiar de selección o (si es
    // dinámico y no se está arrastrando) cada frame para reflejar cambios
    // externos de tamaño/gravedad.
    BoxCollider* m_colliderCachedFor = nullptr;
    glm::vec3    m_editColliderCenter{0.0f};
    glm::vec3    m_editColliderSize{50.0f};
    bool         m_editUseGravity = false;
    bool         m_colliderDragActive = false;
```

Este archivo ya incluye `"DonTopo/BoxCollider.h"` indirectamente vía
`GameObject.h` (que lo incluye desde Task 2) — pero como `EditorUI.h` no
incluye `GameObject.h` directamente (solo forward-declara `class
GameObject;`), hace falta forward-declarar también `BoxCollider`. Añadir
junto a `class GameObject;` (línea 11):

```cpp
class BoxCollider;
```

- [ ] **Step 2: Añadir sección "Box Collider" y botón "Add" en
  `drawProperties()` (`EditorUI.cpp`)**

Reemplazar el final de la función (desde el cierre del `if (changed)` del
Transform, línea 466, hasta `ImGui::End();` en línea 468):

```cpp
    if (changed)
    {
        glm::mat4 t = glm::translate(glm::mat4(1.0f), m_editPosition);
        glm::mat4 r = glm::mat4_cast(glm::quat(glm::radians(m_editRotationDeg)));
        glm::mat4 s = glm::scale(glm::mat4(1.0f), m_editScale);
        m_selected->localTransform = t * r * s;

        if (m_selected->hasBoxCollider())
        {
            m_selected->updateWorldTransforms(m_selected->parent ? m_selected->parent->worldTransform
                                                                   : glm::mat4(1.0f));
            // teleport() (no syncTransform): funciona tanto si el actor es
            // dinámico (isDynamic()==true, cayendo con gravedad) como si es
            // kinematic (isDynamic()==false) — syncTransform usa
            // setKinematicTarget, que solo es válido en modo kinematic.
            m_selected->getBoxCollider()->teleport(m_selected->worldTransform);
        }
    }

    drawBoxColliderSection();
    drawAddComponentButton();

    ImGui::End();
}

void EditorUI::drawBoxColliderSection()
{
    if (!m_selected->hasBoxCollider())
    {
        m_colliderCachedFor = nullptr;
        return;
    }

    BoxCollider* bc = m_selected->getBoxCollider().get();

    if (m_colliderCachedFor != bc)
    {
        m_editColliderCenter = bc->getCenter();
        m_editColliderSize   = bc->getHalfExtents() * 2.0f;
        m_editUseGravity     = bc->getUseGravity();
        m_colliderCachedFor  = bc;
    }
    else if (bc->isDynamic() && !m_colliderDragActive)
    {
        // Solo Center/Size se refrescan (son estables bajo simulación); el
        // toggle de gravedad lo controla el usuario y no cambia solo.
        m_editColliderCenter = bc->getCenter();
        m_editColliderSize   = bc->getHalfExtents() * 2.0f;
    }

    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Box Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##c1", &m_editColliderCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##c1", &m_editColliderCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##c1", &m_editColliderCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        ImGui::Text("Size  ");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##c2", &m_editColliderSize.x, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##c2", &m_editColliderSize.y, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##c2", &m_editColliderSize.z, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        if (ImGui::Checkbox("Use Gravity", &m_editUseGravity))
            colliderChanged = true;

        ImGui::TreePop();
    }

    m_colliderDragActive = dragActive;

    if (colliderChanged)
    {
        bc->setCenter(m_editColliderCenter);
        bc->setHalfExtents(m_editColliderSize * 0.5f);
        bc->setUseGravity(m_editUseGravity);
    }

    if (removeClicked)
    {
        m_selected->setBoxCollider(nullptr);
        m_colliderCachedFor = nullptr;
    }
}

void EditorUI::drawAddComponentButton()
{
    ImGui::Separator();
    if (ImGui::Button("Add"))
        ImGui::OpenPopup("AddComponentPopup");

    if (ImGui::BeginPopup("AddComponentPopup"))
    {
        bool alreadyHasCollider = m_selected->hasBoxCollider();
        ImGui::BeginDisabled(alreadyHasCollider);
        if (ImGui::Selectable("Box Collider") && !alreadyHasCollider && m_physics)
        {
            m_selected->setBoxCollider(m_physics->createBoxColliderComponent(
                glm::vec3(25.0f, 25.0f, 25.0f), glm::vec3(0.0f),
                m_selected->worldTransform, /*useGravity=*/false));
            m_colliderCachedFor = nullptr;
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
}
```

- [ ] **Step 3: Declarar los dos métodos privados nuevos en `EditorUI.h`**

Añadir junto a `void drawProperties();` (línea 37):

```cpp
    void drawProperties();
    void drawBoxColliderSection();
    void drawAddComponentButton();
```

- [ ] **Step 4: Compilar**

Run: `.\build.bat`
Expected: build limpio, sin errores.

- [ ] **Step 5: Prueba manual (sigue el plan de verificación del spec)**

Ejecutar el binario del sandbox y comprobar, en este orden:

1. Seleccionar `floor` en la jerarquía → botón "Add" al fondo de
   Properties → click "Box Collider" → aparece sección "Box Collider" con
   Center (0,0,0), Size (50,50,50), Use Gravity desmarcado. El suelo no se
   mueve (ya tenía su propio collider desde la demo, pero al ser un
   GameObject distinto del suelo original esto es solo para probar la UI —
   usar mejor `sphere`, que no tiene collider en la demo).
2. Con `sphere` seleccionado: Add → Box Collider → activar "Use Gravity" →
   la esfera empieza a caer.
3. Mientras cae, cambiar Size a (30,30,30) → sigue cayendo con la nueva
   forma, sin teletransportarse ni congelarse.
4. Click en "x" de la sección Box Collider → la sección desaparece, la
   esfera dejar de colisionar (puede quedar flotando en el aire donde
   estaba, sin física — comportamiento esperado, ya no tiene collider).
5. Repetir Add → Box Collider sobre `sphere`, luego borrar el GameObject
   completo (click derecho → Delete, o tecla Delete) → sin crash, ni al
   seguir usando el editor ni al cerrar la aplicación.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/EditorUI.h engine/src/EditorUI.cpp
git commit -m "feat(editor): UI para añadir/editar/borrar Box Collider desde Properties"
```
