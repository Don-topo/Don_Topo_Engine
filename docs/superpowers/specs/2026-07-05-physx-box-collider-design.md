# Diseño: Integración PhysX — Box Collider

## Motivación

El motor no tiene físicas. Primer paso: integrar el SDK PhysX de NVIDIA (descargado
y compilado desde `CMakeLists.txt`), con una clase dedicada a manejar el mundo
físico (`PhysicsManager`) y un primer collider (`BoxCollider`) que se pueda añadir
a un `GameObject`.

## Alcance

**Incluido:**
- Descarga + compilación de PhysX 5.x vía `FetchContent` desde el `CMakeLists.txt`
  raíz, en un módulo `cmake/PhysX.cmake`.
- `PhysicsManager`: clase que posee el ciclo de vida de PhysX (foundation, physics,
  scene, material) y crea colliders.
- `BoxCollider`: envoltorio de un `PxRigidStatic` + `PxBoxGeometry`, con sync de
  transform por frame.
- Integración en `GameObject`: un collider opcional por objeto, mismo patrón que
  `Mesh`.
- Smoke test end-to-end (raycast) en `sandbox/main.cpp` para verificar que el
  collider realmente responde a queries de PhysX.

**Fuera de alcance (siguientes pasos):**
- Rigid bodies dinámicos (masa, gravedad, fuerzas) — solo estático por ahora.
- UI de editor (crear/editar collider desde Scene panel o Properties).
- Debug wireframe render del collider en el viewport.
- Otras formas de collider (esfera, cápsula, mesh).
- Soporte macOS (PhysX de NVIDIA no lo soporta oficialmente).

## Arquitectura

### Build system — `cmake/PhysX.cmake`

PhysX no es un proyecto CMake "plug and play" vía `FetchContent_MakeAvailable`
como GLFW/Assimp: su build usa variables de cache propias y no expone un target
raíz consumible directo con `add_subdirectory` simple. Esto es un punto conocido
de fragilidad — puede requerir ajuste al implementar según la versión exacta.

- Guardado condicional: el módulo solo se incluye si `WIN32` o (`UNIX AND NOT APPLE)`.
  En cualquier otra plataforma, `PHYSX_FOUND` queda `FALSE`, se emite un
  `message(WARNING ...)` y el motor compila sin física (mismo patrón que FMOD).
- `FetchContent_Declare(physx GIT_REPOSITORY https://github.com/NVIDIA-Omniverse/PhysX.git GIT_TAG 110.0-omni-and-physx-5.8.0 GIT_SHALLOW TRUE)`
  seguido de `FetchContent_Populate` (no `MakeAvailable`).
- Antes de `add_subdirectory`, fijar en cache las variables que
  `physx/compiler/public/CMakeLists.txt` exige (verificado leyendo el archivo
  real del repo, tag `110.0-omni-and-physx-5.8.0`):
  `PHYSX_ROOT_DIR` (= `${physx_SOURCE_DIR}/physx`, debe existir),
  `TARGET_BUILD_PLATFORM` (`"windows"` o `"linux"`), `PX_GENERATE_STATIC_LIBRARIES ON`.
  El propio CMakeLists de PhysX se encarga de añadir su carpeta de módulos
  a `CMAKE_MODULE_PATH` — no hace falta hacerlo manualmente. GPU/CUDA
  (`PX_GENERATE_GPU_PROJECTS`) queda OFF por defecto, así que no depende de
  `packman` ni CUDA Toolkit para este build CPU-only.
- `add_subdirectory(${physx_SOURCE_DIR}/physx/compiler/public ...)`.
- Target agregado `PhysX::SDK` (`INTERFACE`) que linkea las libs necesarias para
  colliders estáticos: `PhysX`, `PhysXCommon`, `PhysXFoundation`, `PhysXExtensions`,
  `PhysXPvdSDK` (nombres confirmados existen como archivos
  `physx/source/compiler/cmake/windows/*.cmake` en el repo). Si el linker pide
  símbolos de otras libs del SDK (`PhysXCooking`, `PhysXTask`, etc.), se añaden
  durante la implementación.
- `DonTopoEngine` linkea `PhysX::SDK` solo si `PHYSX_FOUND`, y define
  `DT_PHYSX_ENABLED` (mismo patrón que `DT_FMOD_ENABLED`).

### `PhysicsManager`

`engine/include/DonTopo/PhysicsManager.h` + `engine/src/PhysicsManager.cpp`.
Mismo estilo que `AudioManager`: miembros PhysX ocultos tras `void*` bajo
`#ifdef DT_PHYSX_ENABLED`, para que el header público no dependa de incluir
headers de PhysX.

```cpp
class PhysicsManager {
public:
    PhysicsManager() = default;
    ~PhysicsManager();
    PhysicsManager(const PhysicsManager&) = delete;
    PhysicsManager& operator=(const PhysicsManager&) = delete;

    void init();
    void shutdown();

    std::shared_ptr<BoxCollider> createBoxCollider(const glm::vec3& halfExtents,
                                                    const glm::mat4& worldTransform);

private:
#ifdef DT_PHYSX_ENABLED
    void* m_foundation = nullptr; // PxFoundation*
    void* m_physics    = nullptr; // PxPhysics*
    void* m_scene      = nullptr; // PxScene*
    void* m_dispatcher = nullptr; // PxDefaultCpuDispatcher*
    void* m_material   = nullptr; // PxMaterial*
#endif
};
```

- `init()`: crea foundation (allocator + error callback por defecto), physics
  (`PxTolerancesScale` por defecto), scene (gravedad estándar `-9.81` en Y,
  `PxDefaultCpuDispatcher`, `PxDefaultSimulationFilterShader`), material default
  (fricción estática/dinámica y restitución con valores razonables, sin exponer
  parámetros todavía).
- `shutdown()`: libera scene, dispatcher, physics, foundation, en ese orden.
- `createBoxCollider(...)`: crea `PxRigidStatic` en la pose dada por
  `worldTransform`, le añade un shape `PxBoxGeometry(halfExtents)` con el
  material default, lo agrega a la scene, y devuelve un `BoxCollider` que
  envuelve el actor.
- **No incluye `update()`/`step()` en esta pasada**: los actores son estáticos;
  `PxRigidStatic::setGlobalPose` aplica de inmediato sin necesitar
  `PxScene::simulate()`. Se añadirá cuando haya rigid bodies dinámicos.

### `BoxCollider`

`engine/include/DonTopo/BoxCollider.h` + `engine/src/BoxCollider.cpp`. Mismo
patrón opaque-pointer, no copiable.

```cpp
class BoxCollider {
public:
    explicit BoxCollider(void* actor, glm::vec3 halfExtents); // construido por PhysicsManager
    ~BoxCollider(); // libera el actor PhysX

    BoxCollider(const BoxCollider&) = delete;
    BoxCollider& operator=(const BoxCollider&) = delete;

    glm::vec3 getHalfExtents() const { return m_halfExtents; }
    void syncTransform(const glm::mat4& worldTransform);

private:
#ifdef DT_PHYSX_ENABLED
    void* m_actor = nullptr; // PxRigidStatic*
#endif
    glm::vec3 m_halfExtents;
};
```

- `syncTransform`: convierte `glm::mat4` a `PxTransform` (posición + rotación) y
  llama `actor->setGlobalPose(...)`.
- Destructor: `actor->release()` (esto también lo remueve de la scene).

### Integración en `GameObject`

Mismo patrón que `Mesh` (`engine/include/DonTopo/GameObject.h`):

```cpp
void setCollider(std::shared_ptr<BoxCollider> collider) { m_collider = std::move(collider); }
const std::shared_ptr<BoxCollider>& getCollider() const { return m_collider; }
bool hasCollider() const { return m_collider != nullptr; }
// ...
private:
    std::shared_ptr<BoxCollider> m_collider;
```

Un collider por `GameObject`. Sin sistema de componentes genérico — fuera de
alcance por ahora.

### Integración en `sandbox/main.cpp`

- Instanciar `DonTopo::PhysicsManager physics; physics.init();` junto al resto
  de sistemas (igual que `AudioManager`).
- Añadir un `BoxCollider` al `cube` de prueba ya existente vía
  `physics.createBoxCollider(...)`.
- En el loop principal, dentro del `root.traverse(...)` que ya sincroniza
  `renderer.setTransform`, añadir: si `go->hasCollider()`, llamar
  `go->getCollider()->syncTransform(go->worldTransform)`.
- `physics.shutdown()` al final, junto a `renderer.shutdown()`/`window.shutdown()`.

### Verificación

Sin debug-render todavía (fuera de alcance). Smoke test end-to-end: un
`PxScene::raycast` disparado desde arriba del cubo hacia abajo, verificando que
impacta el `BoxCollider` recién creado (assert/log del resultado). Esto prueba
que foundation/physics/scene/actor/shape están correctamente inicializados y
sincronizados, sin depender de visualización.

## Manejo de errores

- Si `PHYSX_FOUND` es falso (plataforma no soportada o fetch/build falla): el
  motor compila sin `DT_PHYSX_ENABLED`, `PhysicsManager`/`BoxCollider` quedan
  como no-ops seguros (mismo patrón que `AudioManager` sin FMOD), y se emite
  warning en configure time. No debe romper el build en otras plataformas.
