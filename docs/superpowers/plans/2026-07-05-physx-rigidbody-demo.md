# PhysX RigidBody Demo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Añadir rigid body dinámico (`PxRigidDynamic`) al motor y avanzar la escena de PhysX cada frame, con una demo end-to-end: el cubo del sandbox cae por gravedad y aterriza sobre un collider estático nuevo del suelo.

**Architecture:** `RigidBody` (clase nueva, mismo patrón `void*` oculto que `BoxCollider`) envuelve un `PxRigidDynamic`. `PhysicsManager` gana `createDynamicBoxCollider(...)` y `stepSimulation(dt)` (primera vez que `PxScene::simulate`/`fetchResults` se llaman). `GameObject` gana `m_rigidBody` opcional, paralelo a `m_collider`. La dirección de sync se invierte para dinámicos: PhysX → GameObject (lectura de pose), al contrario que `BoxCollider` (GameObject → PhysX).

**Tech Stack:** C++20, PhysX 5.8.0 (ya integrado), GLM.

## Global Constraints

- No se modifica `BoxCollider`/`PhysicsManager::createBoxCollider`/`raycast` — ya revisados y mergeados, se reutilizan tal cual.
- Headers públicos (`RigidBody.h`) no incluyen ningún header de PhysX — tipos ocultos tras `void*`, mismo patrón que `BoxCollider.h`.
- Un `GameObject` tiene `m_collider` (estático) O `m_rigidBody` (dinámico) en este pase, nunca ambos — es convención de uso en `main.cpp`, no se valida en `GameObject`.
- Gravedad en `PhysicsManager::init()` pasa de `-9.81f` a `-981.0f` (escala "cm" del motor).
- No hay framework de tests en el repo (sin gtest/ctest). Verificación = build + ejecutar `sandbox` app + revisar consola + observación visual en el viewport.
- Suelo usa `createBoxCollider` existente (caja delgada), no una geometría nueva.

---

### Task 1: `RigidBody` — envoltorio de `PxRigidDynamic`

**Files:**
- Create: `engine/include/DonTopo/RigidBody.h`
- Create: `engine/src/RigidBody.cpp`
- Modify: `engine/CMakeLists.txt:1-18` (añadir `src/RigidBody.cpp` a la lista de fuentes)

**Interfaces:**
- Produces: `class DonTopo::RigidBody` con constructor `RigidBody(void* actor)`, destructor que libera el actor, y `glm::mat4 getWorldTransform() const`.

- [x] **Step 1: Escribir `engine/include/DonTopo/RigidBody.h`**

```cpp
#pragma once
#include <glm/glm.hpp>

namespace DonTopo {

class RigidBody {
public:
    // actor: physx::PxRigidDynamic* ya creado y añadido a la escena por PhysicsManager.
    explicit RigidBody(void* actor);
    ~RigidBody();

    RigidBody(const RigidBody&)            = delete;
    RigidBody& operator=(const RigidBody&) = delete;

    // Lee PxRigidDynamic::getGlobalPose() (traslación + rotación, sin escala).
    glm::mat4 getWorldTransform() const;

private:
#ifdef DT_PHYSX_ENABLED
    void* m_actor = nullptr; // physx::PxRigidDynamic*
#endif
};

} // namespace DonTopo
```

- [x] **Step 2: Escribir `engine/src/RigidBody.cpp`**

```cpp
#include "DonTopo/RigidBody.h"

#ifdef DT_PHYSX_ENABLED
#define GLM_ENABLE_EXPERIMENTAL
#include <PxPhysicsAPI.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

using namespace physx;
#endif

namespace DonTopo {

RigidBody::RigidBody(void* actor)
{
#ifdef DT_PHYSX_ENABLED
    m_actor = actor;
#else
    (void)actor;
#endif
}

RigidBody::~RigidBody()
{
#ifdef DT_PHYSX_ENABLED
    if (m_actor) static_cast<PxRigidDynamic*>(m_actor)->release();
#endif
}

glm::mat4 RigidBody::getWorldTransform() const
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

} // namespace DonTopo
```

- [x] **Step 3: Añadir la fuente a `engine/CMakeLists.txt`**

En `engine/CMakeLists.txt`, añadir `src/RigidBody.cpp` a la lista de `add_library(DonTopoEngine STATIC ...)`, junto a `src/BoxCollider.cpp`:

```cmake
add_library(DonTopoEngine STATIC
    src/Engine.cpp
    src/Window.cpp
    src/Renderer.cpp
    src/EditorUI.cpp
    src/Skybox.cpp
    src/ModelLoader.cpp
    src/Cube.cpp
    src/Sphere.cpp
    src/Plane.cpp
    src/Camera.cpp
    src/GameObject.cpp
    src/AudioManager.cpp
    src/PhysicsManager.cpp
    src/BoxCollider.cpp
    src/RigidBody.cpp
    src/GpuDevice.cpp
    src/GpuResources.cpp
)
```

- [x] **Step 4: Compilar para verificar que el archivo nuevo compila**

Run: `cmake --build --preset debug` (o `build.bat` si el entorno MSVC no está en el shell actual)
Expected: compila sin error. `RigidBody` no se usa todavía en ningún sitio (código muerto hasta Task 2/3), así que no debería haber ningún cambio de comportamiento.

- [x] **Step 5: Commit**

```bash
git add engine/include/DonTopo/RigidBody.h engine/src/RigidBody.cpp engine/CMakeLists.txt
git commit -m "feat(physics): añadir RigidBody, envoltorio de PxRigidDynamic"
```

---

### Task 2: `PhysicsManager` — rigid body dinámico + stepping de la escena

**Files:**
- Modify: `engine/include/DonTopo/PhysicsManager.h` (añadir `createDynamicBoxCollider`, `stepSimulation`, forward-decl de `RigidBody`)
- Modify: `engine/src/PhysicsManager.cpp` (implementar ambos métodos, ajustar gravedad)
- Modify: `engine/include/DonTopo/GameObject.h` (añadir `m_rigidBody`/`setRigidBody`/`getRigidBody`/`hasRigidBody`)

**Interfaces:**
- Consumes: `RigidBody` de Task 1 (constructor `RigidBody(void* actor)`).
- Produces: `PhysicsManager::createDynamicBoxCollider(const glm::vec3& halfExtents, const glm::mat4& worldTransform, float density = 1.0f) -> std::shared_ptr<RigidBody>`; `PhysicsManager::stepSimulation(float dt)`; `GameObject::setRigidBody(std::shared_ptr<RigidBody>)`, `GameObject::getRigidBody() -> const std::shared_ptr<RigidBody>&`, `GameObject::hasRigidBody() -> bool`.

- [x] **Step 1: Añadir forward-decl y firmas nuevas a `engine/include/DonTopo/PhysicsManager.h`**

Reemplazar la línea `namespace DonTopo { class BoxCollider; }` por:

```cpp
namespace DonTopo { class BoxCollider; }
namespace DonTopo { class RigidBody; }
```

Añadir el método público tras `createBoxCollider`:

```cpp
    std::shared_ptr<BoxCollider> createBoxCollider(const glm::vec3& halfExtents,
                                                    const glm::mat4& worldTransform);

    std::shared_ptr<RigidBody> createDynamicBoxCollider(const glm::vec3& halfExtents,
                                                         const glm::mat4& worldTransform,
                                                         float density = 1.0f);

    void stepSimulation(float dt);
```

El archivo completo de `engine/include/DonTopo/PhysicsManager.h` queda:

```cpp
#pragma once
#include <glm/glm.hpp>
#include <memory>

#ifdef DT_PHYSX_ENABLED
#include <PxPhysicsAPI.h>
#endif

namespace DonTopo { class BoxCollider; }
namespace DonTopo { class RigidBody; }

namespace DonTopo {

class PhysicsManager {
public:
    PhysicsManager() = default;
    ~PhysicsManager();
    PhysicsManager(const PhysicsManager&)            = delete;
    PhysicsManager& operator=(const PhysicsManager&) = delete;

    void init();
    void shutdown();

    std::shared_ptr<BoxCollider> createBoxCollider(const glm::vec3& halfExtents,
                                                    const glm::mat4& worldTransform);

    std::shared_ptr<RigidBody> createDynamicBoxCollider(const glm::vec3& halfExtents,
                                                         const glm::mat4& worldTransform,
                                                         float density = 1.0f);

    void stepSimulation(float dt);

#ifdef DT_PHYSX_ENABLED
    bool raycast(const physx::PxVec3& origin, const physx::PxVec3& dir, float maxDistance, physx::PxRaycastBuffer& hit);
#endif

private:
#ifdef DT_PHYSX_ENABLED
    void* m_foundation = nullptr; // physx::PxFoundation*
    void* m_physics    = nullptr; // physx::PxPhysics*
    void* m_scene      = nullptr; // physx::PxScene*
    void* m_dispatcher = nullptr; // physx::PxDefaultCpuDispatcher*
    void* m_material   = nullptr; // physx::PxMaterial*
#endif
};

} // namespace DonTopo
```

- [x] **Step 2: Ajustar gravedad en `engine/src/PhysicsManager.cpp`**

Cambiar en `init()`:

```cpp
    sceneDesc.gravity       = PxVec3(0.0f, -9.81f, 0.0f);
```

por:

```cpp
    sceneDesc.gravity       = PxVec3(0.0f, -981.0f, 0.0f);
```

- [x] **Step 3: Añadir `createDynamicBoxCollider` y `stepSimulation` a `engine/src/PhysicsManager.cpp`**

Añadir el include `#include "DonTopo/RigidBody.h"` junto a `#include "DonTopo/BoxCollider.h"` (línea 2). Añadir al final del archivo, antes del `} // namespace DonTopo` de cierre:

```cpp
std::shared_ptr<RigidBody> PhysicsManager::createDynamicBoxCollider(const glm::vec3& halfExtents,
                                                                     const glm::mat4& worldTransform,
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

    PxBoxGeometry geometry(halfExtents.x, halfExtents.y, halfExtents.z);
    PxRigidDynamic* actor = PxCreateDynamic(*physics, pose, geometry, *material, density);
    physxCheck(actor, "PxCreateDynamic");
    scene->addActor(*actor);

    return std::make_shared<RigidBody>(actor);
#else
    (void)worldTransform;
    (void)density;
    return std::make_shared<RigidBody>(nullptr);
#endif
}

void PhysicsManager::stepSimulation(float dt)
{
#ifdef DT_PHYSX_ENABLED
    static_cast<PxScene*>(m_scene)->simulate(dt);
    static_cast<PxScene*>(m_scene)->fetchResults(true);
#else
    (void)dt;
#endif
}
```

`PxCreateDynamic` está en `PxRigidBodyExt` (extensions), ya incluido por `<PxPhysicsAPI.h>` y ya linkeado (`PhysXExtensions` en `cmake/PhysX.cmake` desde Task 1 de la spec anterior) — no requiere cambios de build.

- [x] **Step 4: Añadir `m_rigidBody` a `engine/include/DonTopo/GameObject.h`**

Añadir el include tras `#include "DonTopo/BoxCollider.h"`:

```cpp
#include "DonTopo/RigidBody.h"
```

Añadir junto a los métodos de collider (tras `hasCollider()`):

```cpp
            void setRigidBody(std::shared_ptr<RigidBody> rb) { m_rigidBody = std::move(rb); }
            const std::shared_ptr<RigidBody>& getRigidBody() const { return m_rigidBody; }
            bool hasRigidBody() const { return m_rigidBody != nullptr; }
```

Añadir el miembro privado junto a `m_collider`:

```cpp
        private:
            std::shared_ptr<Mesh> m_mesh;
            std::shared_ptr<BoxCollider> m_collider;
            std::shared_ptr<RigidBody> m_rigidBody;
```

- [x] **Step 5: Compilar para verificar**

Run: `cmake --build --preset debug` (o `build.bat`)
Expected: compila sin error. `createDynamicBoxCollider`/`stepSimulation`/`m_rigidBody` no se usan todavía en `sandbox` (Task 3), así que no hay cambio de comportamiento observable aún.

- [x] **Step 6: Commit**

```bash
git add engine/include/DonTopo/PhysicsManager.h engine/src/PhysicsManager.cpp engine/include/DonTopo/GameObject.h
git commit -m "feat(physics): añadir rigid body dinámico y stepping de la escena a PhysicsManager"
```

---

### Task 3: Demo en `sandbox` — cubo cae y aterriza

**Files:**
- Modify: `sandbox/src/main.cpp`

**Interfaces:**
- Consumes: `PhysicsManager::createDynamicBoxCollider`/`stepSimulation` y `GameObject::setRigidBody`/`hasRigidBody`/`getRigidBody` de Task 2.

- [x] **Step 1: Cubo pasa de collider estático a rigid body dinámico**

Reemplazar en `sandbox/src/main.cpp:67-72`:

```cpp
        auto* cube = root.addChild("cube");
        cube->setMesh(cubeMesh);
        cube->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 50.0f, -200.0f));

        cube->updateWorldTransforms();
        cube->setCollider(physics.createBoxCollider(glm::vec3(25.0f, 25.0f, 25.0f), cube->worldTransform));
```

por:

```cpp
        auto* cube = root.addChild("cube");
        cube->setMesh(cubeMesh);
        cube->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 50.0f, -200.0f));

        cube->updateWorldTransforms();
        cube->setRigidBody(physics.createDynamicBoxCollider(glm::vec3(25.0f, 25.0f, 25.0f), cube->worldTransform));
```

- [x] **Step 2: Suelo gana un collider estático (caja delgada bajo la superficie visual)**

Tras la línea `floorNode->setMesh(floorMesh);` (`sandbox/src/main.cpp:65`), añadir:

```cpp
        auto* floorNode = root.addChild("floor");
        floorNode->setMesh(floorMesh);

        glm::mat4 floorColliderPose = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, floorY - 0.5f, 0.0f));
        floorNode->setCollider(physics.createBoxCollider(glm::vec3(500.0f, 0.5f, 500.0f), floorColliderPose));
```

- [x] **Step 3: `stepSimulation` una vez por frame, antes del traverse existente**

Reemplazar en `sandbox/src/main.cpp:189` (justo antes de `root.updateWorldTransforms();`):

```cpp
            root.updateWorldTransforms();
```

por:

```cpp
            physics.stepSimulation(dt);
            root.updateWorldTransforms();
```

- [x] **Step 4: Leer la pose del rigid body dentro del traverse existente**

Reemplazar en `sandbox/src/main.cpp:193-205`:

```cpp
            root.traverse([&](DonTopo::GameObject* go) {
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

- [x] **Step 5: Limpiar `m_rigidBody` en el shutdown junto a `m_collider`**

Reemplazar en `sandbox/src/main.cpp:217`:

```cpp
        root.traverse([](DonTopo::GameObject* go) { go->setCollider(nullptr); });
```

por:

```cpp
        root.traverse([](DonTopo::GameObject* go) {
            go->setCollider(nullptr);
            go->setRigidBody(nullptr);
        });
```

- [x] **Step 6: Compilar y ejecutar para verificar la demo end-to-end**

Run: `cmake --build --preset debug` (o `build.bat`)
Expected: compila sin error.

Ejecutar `Sandbox.exe`.
Expected en consola: `[PhysX smoke test] raycast al cubo: HIT` (sigue disparando contra la pose inicial del rigid body, antes de que corra ninguna simulación — comportamiento sin cambios).

Expected visual: el cubo, que arranca en `y=50`, debe caer visiblemente en el viewport y quedar en reposo sobre el suelo (sin atravesarlo, sin vibración perpetua tras unos segundos). Confirmar a ojo.

Cerrar la ventana normalmente (botón X o Escape — NO matar el proceso a la fuerza, para ejercitar el shutdown real) y confirmar que no hay crash ni warnings de PhysX en consola al cerrar.

- [x] **Step 7: Commit**

```bash
git add sandbox/src/main.cpp
git commit -m "feat(physics): demo de cubo dinámico cayendo sobre suelo estático"
```
