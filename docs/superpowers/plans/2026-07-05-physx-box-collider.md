# PhysX Box Collider Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Integrar el SDK PhysX de NVIDIA (compilado desde fuente vía CMake FetchContent) y añadir un `BoxCollider` estático que se pueda adjuntar a un `GameObject`.

**Architecture:** `PhysicsManager` posee el ciclo de vida de PhysX (foundation/physics/scene/material); `BoxCollider` envuelve un `PxRigidStatic` y expone `syncTransform`; `GameObject` gana un miembro opcional `m_collider` (mismo patrón que `m_mesh`). Todo el acceso a tipos de PhysX queda oculto tras punteros `void*` en los headers públicos, igual que `AudioManager` hace con FMOD.

**Tech Stack:** C++20, CMake 3.25 + FetchContent, PhysX 5.8.0 (`NVIDIA-Omniverse/PhysX`, tag `110.0-omni-and-physx-5.8.0`), GLM.

## Global Constraints

- PhysX se integra vía `FetchContent` + `add_subdirectory`, no `find_package` de un SDK preinstalado.
- Solo Windows/Linux (`if(WIN32 OR (UNIX AND NOT APPLE))`); en otras plataformas el motor compila sin física, con warning en configure time — mismo patrón que `FMOD_FOUND`.
- Guard de compilación: `DT_PHYSX_ENABLED` (mismo patrón que `DT_FMOD_ENABLED`).
- Headers públicos (`PhysicsManager.h`, `BoxCollider.h`) no incluyen ningún header de PhysX — tipos ocultos tras `void*`, igual que `AudioManager.h`.
- Este pase: solo colliders estáticos (`PxRigidStatic`). Sin rigid body dinámico, sin UI de editor, sin debug-render — confirmado en el spec.
- Un collider por `GameObject` (miembro único, no sistema de componentes genérico).
- No hay framework de tests en el repo (sin gtest/ctest). Verificación = build + ejecutar `sandbox` app + revisar output de consola, igual que el resto del proyecto.

---

### Task 1: Build system — PhysX vía FetchContent

**Files:**
- Create: `cmake/PhysX.cmake`
- Modify: `CMakeLists.txt:143-145` (reemplazar comentario placeholder de PhysX)
- Modify: `engine/CMakeLists.txt` (link condicional + fuentes nuevas se añaden en Task 2/3)

**Interfaces:**
- Produces: variable `PHYSX_FOUND` (bool), target `PhysX::SDK` (INTERFACE, solo existe si `PHYSX_FOUND`), define de compilación `DT_PHYSX_ENABLED` disponible para `DonTopoEngine` cuando `PHYSX_FOUND`.

- [ ] **Step 1: Crear `cmake/PhysX.cmake` con el fetch + guard de plataforma**

```cmake
# cmake/PhysX.cmake
# Descarga y compila PhysX 5.x (NVIDIA-Omniverse/PhysX) desde fuente vía FetchContent.
# Solo soportado en Windows/Linux (PhysX de NVIDIA no soporta macOS oficialmente).
#
# Define:
#   PHYSX_FOUND    - TRUE si PhysX se descargó y configuró correctamente
#   PhysX::SDK     - target INTERFACE con las libs necesarias (solo si PHYSX_FOUND)

set(PHYSX_FOUND FALSE)

if(WIN32 OR (UNIX AND NOT APPLE))
    if(WIN32)
        set(_PHYSX_TARGET_PLATFORM "windows")
    else()
        set(_PHYSX_TARGET_PLATFORM "linux")
    endif()

    include(FetchContent)
    FetchContent_Declare(
        physx
        GIT_REPOSITORY https://github.com/NVIDIA-Omniverse/PhysX.git
        GIT_TAG        110.0-omni-and-physx-5.8.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(physx)
    if(NOT physx_POPULATED)
        FetchContent_Populate(physx)
    endif()

    set(PHYSX_ROOT_DIR "${physx_SOURCE_DIR}/physx" CACHE INTERNAL "PhysX SDK root")
    set(TARGET_BUILD_PLATFORM "${_PHYSX_TARGET_PLATFORM}" CACHE INTERNAL "PhysX target platform")
    set(PX_GENERATE_STATIC_LIBRARIES ON CACHE BOOL "" FORCE)

    if(EXISTS "${PHYSX_ROOT_DIR}")
        add_subdirectory(${PHYSX_ROOT_DIR}/compiler/public ${CMAKE_BINARY_DIR}/physx_build)
        set(PHYSX_FOUND TRUE)
    else()
        message(WARNING "PhysX source no se descargó correctamente en ${PHYSX_ROOT_DIR}")
    endif()
endif()

if(PHYSX_FOUND)
    add_library(PhysX::SDK INTERFACE IMPORTED)
    target_link_libraries(PhysX::SDK INTERFACE
        PhysX
        PhysXCommon
        PhysXFoundation
        PhysXExtensions
        PhysXPvdSDK
    )
    target_include_directories(PhysX::SDK INTERFACE
        ${PHYSX_ROOT_DIR}/include
    )
else()
    message(WARNING
        "PhysX SDK no disponible en esta plataforma — física no estará disponible.\n"
        "  Soportado solo en Windows/Linux."
    )
endif()
```

- [ ] **Step 2: Incluir el módulo desde `CMakeLists.txt`**

Reemplazar en `CMakeLists.txt` (líneas 143-145):

```cmake
# Future dependencies go here:
# find_package(PhysX REQUIRED)   # physics
# FetchContent_Declare(gainput ...) # dedicated input
```

por:

```cmake
# PhysX SDK — física (compilado desde fuente, ver cmake/PhysX.cmake)
include(cmake/PhysX.cmake)

# Future dependencies go here:
# FetchContent_Declare(gainput ...) # dedicated input
```

- [ ] **Step 3: Linkear condicionalmente en `engine/CMakeLists.txt`**

Añadir después del bloque `if(FMOD_FOUND)` existente (línea 35-38 de `engine/CMakeLists.txt`):

```cmake
if(PHYSX_FOUND)
    target_link_libraries(DonTopoEngine PUBLIC PhysX::SDK)
    target_compile_definitions(DonTopoEngine PUBLIC DT_PHYSX_ENABLED)
endif()
```

- [ ] **Step 4: Configurar el proyecto y verificar que PhysX se descarga y compila**

Run: `cmake --preset debug` (usar el preset que ya usa el proyecto, ver `CMakePresets.json`)
Expected: el configure imprime `Added PhysX` (mensaje propio del CMakeLists de PhysX), termina sin error, y `PHYSX_FOUND` es `TRUE` (verificar con `cmake --preset debug -LA | grep -i physx` o revisando el log).

- [ ] **Step 5: Compilar y verificar que las libs de PhysX se generan**

Run: `cmake --build --preset debug`
Expected: build termina sin error. Verificar que existen artefactos de PhysX en el directorio de build (ej. `physx_build/**/PhysXFoundation*.lib` en Windows o `.a` en Linux) y que `DonTopoEngine` linkea sin error de símbolos faltantes.

Si el linker pide símbolos de otras libs del SDK PhysX no listadas en `PhysX::SDK` (ej. `PhysXCooking`, `PhysXTask`), añadirlas a `target_link_libraries(PhysX::SDK INTERFACE ...)` en `cmake/PhysX.cmake` y repetir este step.

- [ ] **Step 6: Commit**

```bash
git add cmake/PhysX.cmake CMakeLists.txt engine/CMakeLists.txt
git commit -m "build: integrar PhysX SDK vía FetchContent"
```

---

### Task 2: `PhysicsManager` — ciclo de vida de PhysX

**Files:**
- Create: `engine/include/DonTopo/PhysicsManager.h`
- Create: `engine/src/PhysicsManager.cpp`
- Modify: `engine/CMakeLists.txt:1-16` (añadir `src/PhysicsManager.cpp` a la lista de fuentes)
- Modify: `sandbox/src/main.cpp` (instanciar, init, shutdown)

**Interfaces:**
- Consumes: `PhysX::SDK` target y `DT_PHYSX_ENABLED` de Task 1.
- Produces: `class DonTopo::PhysicsManager` con `init()`, `shutdown()`, y método privado/protegido para obtener el `PxScene*`/`PxPhysics*`/`PxMaterial*` internos que usará `BoxCollider` creation en Task 3 (expuestos vía `createBoxCollider`, implementado en Task 3 — este task deja el esqueleto de la clase con esos miembros ya creados en `init()`).

- [ ] **Step 1: Escribir `engine/include/DonTopo/PhysicsManager.h`**

```cpp
#pragma once

namespace DonTopo {

class PhysicsManager {
public:
    PhysicsManager() = default;
    ~PhysicsManager();
    PhysicsManager(const PhysicsManager&)            = delete;
    PhysicsManager& operator=(const PhysicsManager&) = delete;

    void init();
    void shutdown();

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

- [ ] **Step 2: Escribir `engine/src/PhysicsManager.cpp`**

```cpp
#include "DonTopo/PhysicsManager.h"

#ifdef DT_PHYSX_ENABLED
#include <PxPhysicsAPI.h>

using namespace physx;

namespace {
    PxDefaultAllocator      g_allocator;
    PxDefaultErrorCallback  g_errorCallback;
}
#endif

namespace DonTopo {

PhysicsManager::~PhysicsManager() { shutdown(); }

void PhysicsManager::init()
{
#ifdef DT_PHYSX_ENABLED
    auto* foundation = PxCreateFoundation(PX_PHYSICS_VERSION, g_allocator, g_errorCallback);
    m_foundation = foundation;

    PxTolerancesScale scale;
    auto* physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation, scale);
    m_physics = physics;

    auto* dispatcher = PxDefaultCpuDispatcherCreate(2);
    m_dispatcher = dispatcher;

    PxSceneDesc sceneDesc(physics->getTolerancesScale());
    sceneDesc.gravity       = PxVec3(0.0f, -9.81f, 0.0f);
    sceneDesc.cpuDispatcher = dispatcher;
    sceneDesc.filterShader  = PxDefaultSimulationFilterShader;
    m_scene = physics->createScene(sceneDesc);

    m_material = physics->createMaterial(0.5f, 0.5f, 0.1f);
#endif
}

void PhysicsManager::shutdown()
{
#ifdef DT_PHYSX_ENABLED
    if (m_scene)      { static_cast<PxScene*>(m_scene)->release();      m_scene = nullptr; }
    if (m_dispatcher) { static_cast<PxDefaultCpuDispatcher*>(m_dispatcher)->release(); m_dispatcher = nullptr; }
    if (m_physics)    { static_cast<PxPhysics*>(m_physics)->release();  m_physics = nullptr; }
    if (m_foundation) { static_cast<PxFoundation*>(m_foundation)->release(); m_foundation = nullptr; }
    m_material = nullptr; // liberado implícitamente por PxPhysics::release()
#endif
}

} // namespace DonTopo
```

- [ ] **Step 3: Añadir la fuente a `engine/CMakeLists.txt`**

En `engine/CMakeLists.txt`, añadir `src/PhysicsManager.cpp` a la lista de `add_library(DonTopoEngine STATIC ...)` (junto a `src/AudioManager.cpp`).

- [ ] **Step 4: Instanciar en `sandbox/src/main.cpp`**

Añadir el include junto a los demás (tras `#include "DonTopo/AudioManager.h"`):

```cpp
#include "DonTopo/PhysicsManager.h"
```

Añadir tras la creación de `DonTopo::AudioManager audio; audio.init();` (línea ~84-85):

```cpp
        DonTopo::PhysicsManager physics;
        physics.init();
```

Añadir tras `audio.shutdown();` (línea ~184), antes de `renderer.shutdown();`:

```cpp
        physics.shutdown();
```

- [ ] **Step 5: Compilar y ejecutar la app para verificar que no crashea**

Run: `cmake --build --preset debug`
Expected: compila sin error.

Ejecutar el binario de `sandbox` (ruta según preset, ej. `build-ninja/sandbox/DonTopoSandbox.exe`).
Expected: la ventana abre normal, la escena renderiza igual que antes de este cambio, y al cerrar la ventana (Escape) el proceso termina sin crash ni mensajes de error de PhysX en consola (`PxGetFoundation` warnings, etc.). Esto confirma que `init()`/`shutdown()` de foundation/physics/scene funcionan correctamente.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/PhysicsManager.h engine/src/PhysicsManager.cpp engine/CMakeLists.txt sandbox/src/main.cpp
git commit -m "feat(physics): añadir PhysicsManager con ciclo de vida de PhysX"
```

---

### Task 3: `BoxCollider` + integración en `GameObject` + smoke test

**Files:**
- Create: `engine/include/DonTopo/BoxCollider.h`
- Create: `engine/src/BoxCollider.cpp`
- Modify: `engine/include/DonTopo/PhysicsManager.h` (añadir `createBoxCollider`)
- Modify: `engine/src/PhysicsManager.cpp` (implementar `createBoxCollider`)
- Modify: `engine/CMakeLists.txt` (añadir `src/BoxCollider.cpp`)
- Modify: `engine/include/DonTopo/GameObject.h` (añadir `m_collider`/`setCollider`/`getCollider`/`hasCollider`)
- Modify: `sandbox/src/main.cpp` (crear collider en el cubo, sync por frame, raycast smoke test)

**Interfaces:**
- Consumes: `PhysicsManager` de Task 2 (miembros `m_physics`, `m_scene`, `m_material`).
- Produces: `class DonTopo::BoxCollider` con `glm::vec3 getHalfExtents() const` y `void syncTransform(const glm::mat4&)`; `PhysicsManager::createBoxCollider(const glm::vec3& halfExtents, const glm::mat4& worldTransform) -> std::shared_ptr<BoxCollider>`; `GameObject::setCollider(std::shared_ptr<BoxCollider>)`, `GameObject::getCollider() -> const std::shared_ptr<BoxCollider>&`, `GameObject::hasCollider() -> bool`.

- [ ] **Step 1: Escribir `engine/include/DonTopo/BoxCollider.h`**

```cpp
#pragma once
#include <glm/glm.hpp>

namespace DonTopo {

class BoxCollider {
public:
    // actor: physx::PxRigidStatic* ya creado y añadido a la escena por PhysicsManager.
    BoxCollider(void* actor, const glm::vec3& halfExtents);
    ~BoxCollider();

    BoxCollider(const BoxCollider&)            = delete;
    BoxCollider& operator=(const BoxCollider&) = delete;

    glm::vec3 getHalfExtents() const { return m_halfExtents; }
    void syncTransform(const glm::mat4& worldTransform);

private:
#ifdef DT_PHYSX_ENABLED
    void* m_actor = nullptr; // physx::PxRigidStatic*
#endif
    glm::vec3 m_halfExtents;
};

} // namespace DonTopo
```

- [ ] **Step 2: Escribir `engine/src/BoxCollider.cpp`**

```cpp
#include "DonTopo/BoxCollider.h"

#ifdef DT_PHYSX_ENABLED
#include <PxPhysicsAPI.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>

using namespace physx;
#endif

namespace DonTopo {

BoxCollider::BoxCollider(void* actor, const glm::vec3& halfExtents)
    : m_halfExtents(halfExtents)
{
#ifdef DT_PHYSX_ENABLED
    m_actor = actor;
#else
    (void)actor;
#endif
}

BoxCollider::~BoxCollider()
{
#ifdef DT_PHYSX_ENABLED
    if (m_actor) static_cast<PxRigidStatic*>(m_actor)->release();
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
    static_cast<PxRigidStatic*>(m_actor)->setGlobalPose(pose);
#else
    (void)worldTransform;
#endif
}

} // namespace DonTopo
```

`glm::decompose` requiere `#include <glm/gtx/matrix_decompose.hpp>` — añadirlo al include list de arriba junto a `glm/gtx/quaternion.hpp`.

- [ ] **Step 3: Añadir `createBoxCollider` a `PhysicsManager`**

En `engine/include/DonTopo/PhysicsManager.h`, añadir tras el include existente y antes de `private:`:

```cpp
#include <glm/glm.hpp>
#include <memory>

class BoxCollider; // forward decl, ver DonTopo/BoxCollider.h
```

(mover el `namespace DonTopo {` para que el forward-decl quede dentro, o añadir `namespace DonTopo { class BoxCollider; }` antes de la clase — usar esta segunda forma para no reordenar el archivo).

Añadir el método público:

```cpp
    std::shared_ptr<BoxCollider> createBoxCollider(const glm::vec3& halfExtents,
                                                    const glm::mat4& worldTransform);
```

En `engine/src/PhysicsManager.cpp`, añadir el include `#include "DonTopo/BoxCollider.h"` y la implementación (requiere el mismo include de `glm/gtx/matrix_decompose.hpp` y `glm/gtx/quaternion.hpp` que usa `BoxCollider.cpp` para descomponer `worldTransform`):

```cpp
std::shared_ptr<BoxCollider> PhysicsManager::createBoxCollider(const glm::vec3& halfExtents,
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

    PxBoxGeometry geometry(halfExtents.x, halfExtents.y, halfExtents.z);
    PxRigidStatic* actor = PxCreateStatic(*physics, pose, geometry, *material);
    scene->addActor(*actor);

    return std::make_shared<BoxCollider>(actor, halfExtents);
#else
    (void)worldTransform;
    return std::make_shared<BoxCollider>(nullptr, halfExtents);
#endif
}
```

- [ ] **Step 4: Añadir `src/BoxCollider.cpp` a `engine/CMakeLists.txt`**

Añadir `src/BoxCollider.cpp` junto a `src/PhysicsManager.cpp` en la lista de fuentes de `add_library(DonTopoEngine STATIC ...)`.

- [ ] **Step 5: Integrar collider en `GameObject`**

En `engine/include/DonTopo/GameObject.h`, añadir el include tras `#include "DonTopo/SkinnedMesh.h"`:

```cpp
#include "DonTopo/BoxCollider.h"
```

Añadir junto a los métodos de mesh (tras `getSkinnedMesh()`):

```cpp
            void setCollider(std::shared_ptr<BoxCollider> collider) { m_collider = std::move(collider); }
            const std::shared_ptr<BoxCollider>& getCollider() const { return m_collider; }
            bool hasCollider() const { return m_collider != nullptr; }
```

Añadir el miembro privado junto a `m_mesh`:

```cpp
        private:
            std::shared_ptr<Mesh> m_mesh;
            std::shared_ptr<BoxCollider> m_collider;
```

- [ ] **Step 6: Crear collider en el cubo de prueba y sincronizar cada frame en `sandbox/src/main.cpp`**

Tras `cube->localTransform = glm::translate(...)` (línea ~58) y antes de la siguiente sección (`sphere`), añadir:

```cpp
        cube->updateWorldTransforms();
        auto cubeCollider = physics.createBoxCollider(glm::vec3(25.0f, 25.0f, 25.0f), cube->worldTransform);
        cube->setCollider(cubeCollider);
```

(el cubo se creó con `Cube::create(50.0f)` — asumiendo que ese `50.0f` es el lado completo, medio-extent = 25.0f; si `Cube::create` usa otra convención de tamaño, ajustar `halfExtents` para que coincida con el mesh visible).

En el loop principal, dentro del `root.traverse(...)` que ya sincroniza transforms (línea ~169-178), añadir:

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

- [ ] **Step 7: Smoke test — raycast contra el collider del cubo**

Añadir el include `#include <PxPhysicsAPI.h>` guardado tras `#ifdef DT_PHYSX_ENABLED` en `sandbox/src/main.cpp` (junto a los demás includes), y tras crear `cubeCollider` (Step 6), añadir el raycast de verificación:

```cpp
#ifdef DT_PHYSX_ENABLED
        {
            physx::PxRaycastBuffer hit;
            physx::PxVec3 origin(cube->worldTransform[3].x, cube->worldTransform[3].y + 200.0f, cube->worldTransform[3].z);
            physx::PxVec3 dir(0.0f, -1.0f, 0.0f);
            bool didHit = physics.raycast(origin, dir, 400.0f, hit);
            std::cout << "[PhysX smoke test] raycast al cubo: " << (didHit ? "HIT" : "MISS") << std::endl;
        }
#endif
```

Esto requiere exponer un método de conveniencia en `PhysicsManager` para el smoke test. Añadir a `engine/include/DonTopo/PhysicsManager.h` (declaración pública, después de `createBoxCollider`):

```cpp
#ifdef DT_PHYSX_ENABLED
    bool raycast(const physx::PxVec3& origin, const physx::PxVec3& dir, float maxDistance, physx::PxRaycastBuffer& hit);
#endif
```

Esto obliga a incluir `<PxPhysicsAPI.h>` en el header — acorde a que este método solo existe bajo `DT_PHYSX_ENABLED` (el resto de la API pública sigue oculta tras `void*`). Añadir el include correspondiente arriba del todo en `PhysicsManager.h`:

```cpp
#ifdef DT_PHYSX_ENABLED
#include <PxPhysicsAPI.h>
#endif
```

Implementación en `engine/src/PhysicsManager.cpp`:

```cpp
#ifdef DT_PHYSX_ENABLED
bool PhysicsManager::raycast(const PxVec3& origin, const PxVec3& dir, float maxDistance, PxRaycastBuffer& hit)
{
    return static_cast<PxScene*>(m_scene)->raycast(origin, dir, maxDistance, hit);
}
#endif
```

- [ ] **Step 8: Compilar y ejecutar para verificar el smoke test end-to-end**

Run: `cmake --build --preset debug`
Expected: compila sin error.

Ejecutar el binario de `sandbox`.
Expected: en consola aparece `[PhysX smoke test] raycast al cubo: HIT` (el rayo baja desde 200 unidades arriba del cubo y debe impactar el `BoxCollider` de 25 de medio-extent). Si aparece `MISS`, revisar que `halfExtents` coincide con el tamaño real de `Cube::create(50.0f)` y que la posición del cubo (`glm::vec3(0.0f, 50.0f, -200.0f)`) está dentro del rango del rayo.

Confirmar también visualmente que la app sigue renderizando el cubo en su posición normal (el collider no afecta el render, solo corre en paralelo).

- [ ] **Step 9: Commit**

```bash
git add engine/include/DonTopo/BoxCollider.h engine/src/BoxCollider.cpp \
        engine/include/DonTopo/PhysicsManager.h engine/src/PhysicsManager.cpp \
        engine/CMakeLists.txt engine/include/DonTopo/GameObject.h sandbox/src/main.cpp
git commit -m "feat(physics): añadir BoxCollider, integrarlo en GameObject y smoke test de raycast"
```
