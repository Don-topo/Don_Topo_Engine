# RigidBody Editor Sync — Design

**Goal:** Permitir editar posición/rotación de un `GameObject` con `RigidBody` dinámico desde el panel Properties, sin que la simulación de PhysX lo pise inmediatamente.

## Problema

`RigidBody` solo sincroniza en una dirección: física → GameObject. Cada frame, `sandbox/src/main.cpp` hace:

```cpp
if (go->hasRigidBody())
    go->worldTransform = go->getRigidBody()->getWorldTransform();
```

Esto ocurre **después** de que `EditorUI::drawProperties()` haya podido escribir `m_selected->localTransform` en el frame anterior. El resultado: cualquier edit sobre un objeto con `RigidBody` se sobreescribe antes de llegar a pantalla — el objeto parece "bloqueado".

`BoxCollider` (estático) no tiene este problema: sincroniza GameObject → PhysX cada frame (`syncTransform`), así que mover/rotar el suelo ya funciona hoy sin cambios.

## Alcance

- Posición y rotación de objetos con `RigidBody` dinámico: **sí**, editable.
- Escala: **no** — queda solo visual (no se toca geometría de PhysX). Acordado explícitamente: fuera de alcance de este cambio.
- `BoxCollider` estático: sin cambios, ya funciona.

## Diseño

### 1. `RigidBody::setWorldTransform` (nuevo método)

`engine/include/DonTopo/RigidBody.h` / `engine/src/RigidBody.cpp`:

```cpp
// Teletransporta el actor a la pose dada (pos+rot, escala ignorada) y
// resetea su velocidad lineal/angular, para que la simulación siga
// desde ahí con normalidad. autowake=true por defecto en PhysX saca
// al actor del sleep si estaba dormido.
void setWorldTransform(const glm::mat4& worldTransform);
```

Implementación: decompone `worldTransform` (glm::decompose, igual patrón que `BoxCollider::syncTransform` y `RigidBody::getWorldTransform`), llama `PxRigidDynamic::setGlobalPose(pose)`, luego `setLinearVelocity(PxVec3(0))` y `setAngularVelocity(PxVec3(0))`.

### 2. `EditorUI::drawProperties` — aplicar el edit a physics

Tras el bloque existente:

```cpp
if (changed)
{
    ...
    m_selected->localTransform = t * r * s;
}
```

Añadir:

```cpp
if (changed && m_selected->hasRigidBody())
{
    m_selected->updateWorldTransforms(m_selected->parent ? m_selected->parent->worldTransform
                                                           : glm::mat4(1.0f));
    m_selected->getRigidBody()->setWorldTransform(m_selected->worldTransform);
}
```

`updateWorldTransforms` recalcula `worldTransform` de ese nodo (y sus hijos) a partir del `localTransform` recién editado y el `worldTransform` del padre (ya actualizado este mismo frame por el loop principal, antes de que se dibuje `EditorUI`). Así `setWorldTransform` recibe la pose correcta en espacio de mundo, no la local.

### 3. Flujo resultante por frame

1. `physics.stepSimulation(dt)` — simula desde la última pose conocida del actor (incluyendo la que el usuario haya fijado el frame anterior vía edit).
2. `root.updateWorldTransforms()` — recompone `worldTransform` desde `localTransform` (sin efecto para nodos con RigidBody, se sobreescribe en el paso 3).
3. `traverse`: para nodos con RigidBody, `worldTransform` se lee de PhysX (pose ya avanzada un step desde donde el usuario la dejó).
4. `renderer.drawFrame` → `EditorUI::drawProperties`: si el usuario arrastra la posición, se actualiza `localTransform`, se recalcula `worldTransform` de ese nodo, y se llama `setWorldTransform` sobre el actor — listo para que el próximo `stepSimulation` continúe desde ahí.

## Verificación

Sin framework de tests (igual que specs previas de física). Verificación = build + smoke test manual:
- Seleccionar el cubo en el editor mientras está en reposo sobre el suelo, arrastrar su posición en Y hacia arriba en Properties → debe soltarse y caer de nuevo con gravedad normal, sin vibrar al aterrizar (el fix de `PxTolerancesScale` de la sesión anterior ya cubre eso).
- Arrastrar en X/Z mientras cae → debe desplazarse instantáneamente a la nueva posición y seguir cayendo desde ahí.
- Rotar mientras está en el aire → debe aplicar la rotación y seguir cayendo/rotando por física a partir de ahí (sin conservar momentum angular previo, ya que se resetea `angularVelocity`).
