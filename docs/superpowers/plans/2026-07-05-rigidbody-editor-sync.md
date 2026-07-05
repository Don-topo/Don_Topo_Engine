# RigidBody Editor Sync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Permitir editar posición/rotación de un `GameObject` con `RigidBody` dinámico desde el panel Properties del editor, teletransportando el actor de PhysX a la nueva pose (velocidad reseteada) para que la simulación siga con normalidad desde ahí.

**Architecture:** `RigidBody` gana `setWorldTransform(const glm::mat4&)` (decompone pos+rot, `PxRigidDynamic::setGlobalPose` + reset de velocidades). `EditorUI::drawProperties()` llama a ese método cuando el nodo seleccionado tiene RigidBody y el usuario edita Position/Rotation. Escala queda fuera de alcance (solo visual, ya acordado en spec).

**Tech Stack:** C++20, PhysX 5.8.0 (ya integrado), GLM, Dear ImGui.

## Global Constraints

- Escala NO se propaga a PhysX en este pase (ni para RigidBody ni para BoxCollider) — fuera de alcance, acordado en el spec.
- `BoxCollider` estático no se toca — ya sincroniza GameObject→PhysX cada frame, funciona sin cambios.
- No se modifica el patrón `void*` oculto de `RigidBody` (sigue sin exponer tipos de PhysX en el header público).
- No hay framework de tests en el repo (sin gtest/ctest). Verificación = build + smoke-test manual (arrastrar el cubo en el editor, observar el comportamiento en consola/y-position si hace falta instrumentar temporalmente).

---

### Task 1: `RigidBody::setWorldTransform` + wiring en `EditorUI`

**Files:**
- Modify: `engine/include/DonTopo/RigidBody.h` (declarar `setWorldTransform`)
- Modify: `engine/src/RigidBody.cpp` (implementar)
- Modify: `engine/src/EditorUI.cpp` (llamar al método tras editar Position/Rotation de un nodo con RigidBody)

**Interfaces:**
- Produces: `DonTopo::RigidBody::setWorldTransform(const glm::mat4& worldTransform)` — teletransporta el actor dinámico a la pose dada (pos+rot; escala ignorada) y resetea `linearVelocity`/`angularVelocity` a cero.
- Consumes: `GameObject::hasRigidBody()`, `GameObject::getRigidBody()`, `GameObject::updateWorldTransforms(const glm::mat4&)` (ya existen, ver `engine/include/DonTopo/GameObject.h`).

- [ ] **Step 1: Añadir la declaración a `engine/include/DonTopo/RigidBody.h`**

Archivo completo resultante:

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

    // Teletransporta el actor a la pose dada (traslación + rotación; la
    // escala de worldTransform se ignora) y resetea su velocidad lineal y
    // angular a cero, para que la simulación siga con normalidad desde la
    // nueva pose (p.ej. tras un edit desde el editor). autowake=true por
    // defecto en PhysX saca al actor del sleep si estaba dormido.
    void setWorldTransform(const glm::mat4& worldTransform);

private:
#ifdef DT_PHYSX_ENABLED
    void* m_actor = nullptr; // physx::PxRigidDynamic*
#endif
};

} // namespace DonTopo
```

- [ ] **Step 2: Implementar `setWorldTransform` en `engine/src/RigidBody.cpp`**

Añadir al final del archivo (antes de `} // namespace DonTopo`), tras `getWorldTransform`:

```cpp
void RigidBody::setWorldTransform(const glm::mat4& worldTransform)
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
```

El archivo ya incluye `<glm/gtc/matrix_transform.hpp>` y `<glm/gtc/quaternion.hpp>` desde `getWorldTransform`; `glm::decompose` necesita además `<glm/gtx/matrix_decompose.hpp>` (no incluido todavía en este archivo). Añadir ese include junto a los otros dos, bajo `#ifdef DT_PHYSX_ENABLED`:

```cpp
#ifdef DT_PHYSX_ENABLED
#define GLM_ENABLE_EXPERIMENTAL
#include <PxPhysicsAPI.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

using namespace physx;
#endif
```

- [ ] **Step 3: Compilar para verificar que el archivo modificado compila**

Run: `build.bat`
Expected: compila sin error. `setWorldTransform` no se usa todavía en ningún sitio (código muerto hasta el Step 4), así que no debería haber ningún cambio de comportamiento.

- [ ] **Step 4: Llamar a `setWorldTransform` desde `EditorUI::drawProperties`**

En `engine/src/EditorUI.cpp`, dentro de `drawProperties()`, reemplazar:

```cpp
    if (changed)
    {
        glm::mat4 t = glm::translate(glm::mat4(1.0f), m_editPosition);
        glm::mat4 r = glm::mat4_cast(glm::quat(glm::radians(m_editRotationDeg)));
        glm::mat4 s = glm::scale(glm::mat4(1.0f), m_editScale);
        m_selected->localTransform = t * r * s;
    }
```

por:

```cpp
    if (changed)
    {
        glm::mat4 t = glm::translate(glm::mat4(1.0f), m_editPosition);
        glm::mat4 r = glm::mat4_cast(glm::quat(glm::radians(m_editRotationDeg)));
        glm::mat4 s = glm::scale(glm::mat4(1.0f), m_editScale);
        m_selected->localTransform = t * r * s;

        if (m_selected->hasRigidBody())
        {
            m_selected->updateWorldTransforms(m_selected->parent ? m_selected->parent->worldTransform
                                                                   : glm::mat4(1.0f));
            m_selected->getRigidBody()->setWorldTransform(m_selected->worldTransform);
        }
    }
```

(`GameObject.h` ya incluye `DonTopo/RigidBody.h`, y `EditorUI.cpp` ya incluye `GameObject.h` transitivamente vía los headers existentes — no hace falta ningún include nuevo.)

- [ ] **Step 5: Compilar**

Run: `build.bat`
Expected: compila sin error.

- [ ] **Step 6: Smoke-test manual end-to-end**

Ejecutar `build-ninja\sandbox\Sandbox.exe`. En el editor:

1. Seleccionar el `cube` en la jerarquía (panel Scene) mientras está en reposo sobre el suelo (tras caer, ver sesión anterior — se asienta en y≈25.5 sin vibrar).
2. En Properties, arrastrar `Position Y` hacia arriba (p.ej. a 100). Expected: el cubo se teletransporta a esa altura instantáneamente y empieza a caer de nuevo con gravedad normal (sin conservar velocidad previa).
3. Mientras cae, arrastrar `Position X` o `Z`. Expected: el cubo se desplaza lateralmente al instante y sigue cayendo desde ahí.
4. Arrastrar algún eje de `Rotation` mientras está en el aire. Expected: la rotación se aplica y el cubo sigue cayendo/rotando por física a partir de ahí.
5. Dejar que aterrice de nuevo sobre el suelo. Expected: se asienta sin vibrar (ya cubierto por el fix de `PxTolerancesScale` de la sesión anterior).

Cerrar la ventana normalmente (botón X o Escape, no matar el proceso) y confirmar que no hay crash ni warnings de PhysX en consola.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/RigidBody.h engine/src/RigidBody.cpp engine/src/EditorUI.cpp
git commit -m "feat(editor): permitir mover/rotar GameObjects con RigidBody dinámico"
```
