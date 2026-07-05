# Diseño: Rigid Body dinámico — demo de caída y aterrizaje

## Motivación

La integración de PhysX (spec anterior, `2026-07-05-physx-box-collider-design.md`)
solo soporta colliders estáticos (`PxRigidStatic`), y la escena de PhysX nunca se
avanza (`PxScene::simulate`/`fetchResults` no se llaman en ningún sitio). No hay
forma de ver el sistema de físicas "funcionando" — el raycast smoke test prueba
que un collider responde a queries, pero no que haya simulación real. Este pase
añade rigid body dinámico y stepping de la escena, con una demo concreta: un
cubo cae por gravedad y aterriza sobre el suelo.

## Alcance

**Incluido:**
- `RigidBody`: nueva clase que envuelve un `PxRigidDynamic` (mismo patrón `void*`
  oculto que `BoxCollider`, no lo modifica).
- `PhysicsManager::createDynamicBoxCollider(halfExtents, worldTransform, density)`
  — crea el `PxRigidDynamic` + `PxBoxGeometry`, lo añade a la escena.
- `PhysicsManager::stepSimulation(dt)` — `PxScene::simulate(dt)` + `fetchResults(true)`.
  Primera vez que la escena de PhysX realmente avanza.
- `GameObject`: `m_rigidBody` opcional (paralelo a `m_collider`, mismo patrón
  setter/getter/has-check). Un GameObject tiene collider estático O rigid body
  dinámico en este pase, no ambos.
- Sync inverso para dinámicos: cada frame se lee la pose del `PxRigidDynamic` y
  se aplica al `GameObject` (PhysX → GameObject), al contrario que `BoxCollider`
  (GameObject → PhysX), que no cambia.
- Ajuste de `gravity` en `PhysicsManager::init()` de `-9.81f` a `-981.0f`
  (el motor usa escala tipo "cm": cubo=50, posiciones en cientos de unidades;
  -9.81 asumía metros y hacía la caída demasiado lenta para la demo).
- Demo en `sandbox/main.cpp`: cubo pasa de `BoxCollider` estático a `RigidBody`
  dinámico; el suelo (`floor`) gana un `BoxCollider` estático nuevo (caja
  delgada bajo la superficie visual del `Plane`).

**Fuera de alcance:**
- UI de editor para crear/editar rigid bodies.
- Debug wireframe render de colliders/rigid bodies.
- Otras formas dinámicas (esfera, cápsula).
- Fuerzas/impulsos aplicados manualmente (solo gravedad pasiva).
- Sleep/wake state tuning, CCD, u otras opciones avanzadas de `PxRigidDynamic`.

## Arquitectura

### `RigidBody` (`engine/include/DonTopo/RigidBody.h`, `engine/src/RigidBody.cpp`)

Mismo patrón que `BoxCollider`: el tipo `PxRigidDynamic*` queda oculto tras
`void*` en el header público, guardado bajo `DT_PHYSX_ENABLED`.

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

    // Lee PxRigidDynamic::getGlobalPose() y lo convierte a glm::mat4 (solo
    // traslación + rotación, sin escala — igual que BoxCollider::syncTransform
    // en su dirección inversa).
    glm::mat4 getWorldTransform() const;

private:
#ifdef DT_PHYSX_ENABLED
    void* m_actor = nullptr; // physx::PxRigidDynamic*
#endif
};

} // namespace DonTopo
```

En build no-PhysX, `getWorldTransform()` devuelve `glm::mat4(1.0f)` (identidad) —
no debería llamarse nunca en ese caso porque `createDynamicBoxCollider` tampoco
crea un actor real, pero se mantiene el mismo patrón defensivo que `BoxCollider`.

### `PhysicsManager` — dos métodos nuevos

```cpp
std::shared_ptr<RigidBody> createDynamicBoxCollider(const glm::vec3& halfExtents,
                                                      const glm::mat4& worldTransform,
                                                      float density = 1.0f);
void stepSimulation(float dt);
```

`createDynamicBoxCollider` sigue el mismo patrón que `createBoxCollider`:
descompone `worldTransform` con `glm::decompose` para obtener pose inicial,
crea `PxRigidDynamic` vía `PxCreateDynamic(*physics, pose, geometry, *material, density)`
(API de extensions, calcula masa/inercia automáticamente a partir de `density`),
valida con `physxCheck` (mismo helper de Task 2/3), añade a la escena con
`scene->addActor(*actor)`.

`stepSimulation(dt)` llama `m_scene->simulate(dt)` seguido de
`m_scene->fetchResults(true)` (bloqueante — sin multithreading de simulación en
este pase, coherente con "sin optimización prematura").

Ajuste en `init()`: `sceneDesc.gravity = PxVec3(0.0f, -981.0f, 0.0f)` (antes
`-9.81f`).

### `GameObject` — `m_rigidBody` opcional

```cpp
void setRigidBody(std::shared_ptr<RigidBody> rb) { m_rigidBody = std::move(rb); }
const std::shared_ptr<RigidBody>& getRigidBody() const { return m_rigidBody; }
bool hasRigidBody() const { return m_rigidBody != nullptr; }
private:
    std::shared_ptr<RigidBody> m_rigidBody;
```

Mismo patrón exacto que `m_collider`. Un `GameObject` puede tener `m_collider`
O `m_rigidBody`, nunca ambos en este pase — no se valida/impide programáticamente
(no hay setter que lo prohíba), es una convención de uso en `main.cpp`, igual
que el resto de reglas de "un pase a la vez" del proyecto.

### `sandbox/main.cpp` — demo

- El cubo deja de llamar `physics.createBoxCollider(...)` /
  `cube->setCollider(...)`. En su lugar:
  ```cpp
  cube->setRigidBody(physics.createDynamicBoxCollider(glm::vec3(25.0f, 25.0f, 25.0f), cube->worldTransform));
  ```
  Misma posición inicial `(0, 50, -200)`, mismos half-extents `25`.
- El suelo gana un collider estático nuevo, reusando `createBoxCollider` sin
  tocar `BoxCollider`/`PhysicsManager` estático:
  ```cpp
  glm::mat4 floorColliderPose = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, floorY - 0.5f, 0.0f));
  floorNode->setCollider(physics.createBoxCollider(glm::vec3(500.0f, 0.5f, 500.0f), floorColliderPose));
  ```
  Caja delgada (medio-grosor 0.5) centrada justo debajo de la superficie visual
  del `Plane` (que está exactamente en `y = floorY`, sin grosor).
- El raycast smoke test existente no cambia de posición/lógica — sigue
  disparando inmediatamente tras crear el rigid body del cubo, antes de que
  corra ninguna simulación, así que sigue siendo determinista (`HIT` esperado
  contra la pose inicial).
- Loop principal: `physics.stepSimulation(dt)` se llama una vez por frame,
  ANTES del `root.traverse(...)` existente. Dentro del traverse, antes de leer
  `go->worldTransform` para el renderer:
  ```cpp
  if (go->hasRigidBody())
      go->worldTransform = go->getRigidBody()->getWorldTransform();
  ```
  El push existente para estáticos (`if (go->hasCollider()) ...syncTransform(...)`)
  no cambia.
- Shutdown: el `root.traverse` de limpieza ya existente (antes de
  `physics.shutdown()`) también resetea `m_rigidBody` a `nullptr`, igual que ya
  hace con `m_collider`. El orden de declaración `physics` antes que `root` en
  `main.cpp` (fix del PR anterior) ya cubre el caso de excepción sin cambios
  adicionales.

## Flujo por frame

```
physics.stepSimulation(dt);              // nuevo

root.traverse([&](GameObject* go) {
    if (go->hasRigidBody())
        go->worldTransform = go->getRigidBody()->getWorldTransform();  // PhysX -> GameObject

    if (go->staticRenderIndex >= 0)
        renderer.setTransform(go->staticRenderIndex, go->worldTransform);
    // ... skinned igual que hoy

    if (go->hasCollider())
        go->getCollider()->syncTransform(go->worldTransform);          // GameObject -> PhysX (sin cambios)
});
```

## Manejo de errores

Igual que Task 2/3: `physxCheck` en la creación del `PxRigidDynamic`. Sin
manejo de errores nuevo para `stepSimulation` — `PxScene::simulate`/`fetchResults`
no tienen un modo de fallo recuperable relevante en este pase (no hay callbacks
de colisión custom, no hay multithreading de tareas).

## Testing / Verificación

Sin framework de tests (igual que el resto del proyecto). Verificación =
build + ejecutar `sandbox` + observación:
- Consola: smoke test de raycast sigue imprimiendo `HIT` (sin cambios de
  comportamiento en ese punto).
- Visual: el cubo debe caer desde `y=50` y quedar en reposo sobre el suelo
  (sin atravesarlo, sin vibración perpetua) — confirmado a ojo en el viewport.
- Cierre normal de ventana (WM_CLOSE, no kill forzado) sin crash ni warnings
  de PhysX — mismo método de verificación de shutdown que en la spec anterior.

## Riesgos conocidos

- Escala de unidades del motor no está formalmente definida en ningún sitio;
  `-981.0f` es una estimación basada en los tamaños existentes (cubo=50,
  distancias=200), no una constante documentada del engine. Si en el futuro se
  define una escala oficial, este valor debería revisarse.
- `PxCreateDynamic`/`density` requiere el módulo de extensions de PhysX
  (`PxRigidBodyExt`) — ya se linkea (`PhysXExtensions` está en
  `cmake/PhysX.cmake` desde Task 1), no se esperan cambios de build.
