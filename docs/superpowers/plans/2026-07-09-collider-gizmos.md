# Collider Gizmos Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Dibujar el wireframe del collider (Box/Sphere/Capsule/Plane) del GameObject seleccionado en el editor, actualizado en vivo y ocultado al quitar el collider o deseleccionar.

**Architecture:** Extender `Gizmos::drawWireBox` con parámetro `center` (bug fix — hoy lo ignora) y añadir `Gizmos::drawWirePlane` (grid 10×10). Enganchar ambos + los ya existentes `drawWireSphere`/`drawWireCapsule` en `EditorUI::drawSelectionGizmo()`, en cascada por tipo de collider del `m_selected` actual. Eliminar el demo hardcodeado en `sandbox/main.cpp` que hoy es el único sitio que dibuja un wireframe de collider.

**Tech Stack:** C++20, Vulkan (vía `Gizmos` line-list pipeline ya existente), GLM, Dear ImGui (panel Properties, sin cambios aquí)

## Global Constraints

- Color de wireframe: amarillo `(1.0f, 1.0f, 0.0f)` fijo para las 4 formas
- Grid del Plane Collider: tamaño fijo 10×10 unidades (`kPlaneGizmoHalfSize = 5.0f`), no configurable
- No se toca `Renderer` ni el contrato de `GameObject`/colliders — solo `Gizmos` y `EditorUI`
- Build command: `cmake --build --preset debug`
- No hay suite de tests automatizada en este repo (motor C++/Vulkan) — verificación es compilación limpia + smoke test manual del editor

---

## File Map

| Action | File | Responsibility |
|--------|------|-----------------|
| Modify | `engine/include/DonTopo/Gizmos.h` | Firma `drawWireBox` +center, declarar `drawWirePlane` |
| Modify | `engine/src/Gizmos.cpp` | Implementar el fix de `drawWireBox` y `drawWirePlane` |
| Modify | `engine/include/DonTopo/EditorUI.h` | Ninguno (no requiere nuevo estado ni miembro) |
| Modify | `engine/src/EditorUI.cpp` | Cascada por tipo de collider en `drawSelectionGizmo()` |
| Modify | `sandbox/src/main.cpp` | Eliminar el bloque demo `drawWireBox(liveCube, ...)` |

---

## Task 1: Extender Gizmos (drawWireBox +center, drawWirePlane)

**Files:**
- Modify: `engine/include/DonTopo/Gizmos.h:32-37`
- Modify: `engine/src/Gizmos.cpp:292-310` (drawWireBox), añadir función nueva tras `drawWireCapsule` (línea ~399)

**Interfaces:**
- Produces:
  - `Gizmos::drawWireBox(const glm::mat4& transform, const glm::vec3& center, const glm::vec3& halfExtents, const glm::vec3& color)` — firma cambiada, gana `center` como 2º parámetro
  - `Gizmos::drawWirePlane(const glm::mat4& transform, const glm::vec3& center, const glm::vec3& color)` — nueva

- [ ] **Step 1: Cambiar la firma de `drawWireBox` en `Gizmos.h`**

En `engine/include/DonTopo/Gizmos.h`, reemplazar:

```cpp
    static void drawWireBox(const glm::mat4& transform, const glm::vec3& halfExtents,
                            const glm::vec3& color);
```

por:

```cpp
    static void drawWireBox(const glm::mat4& transform, const glm::vec3& center,
                            const glm::vec3& halfExtents, const glm::vec3& color);
    static void drawWirePlane(const glm::mat4& transform, const glm::vec3& center,
                              const glm::vec3& color);
```

- [ ] **Step 2: Actualizar la implementación de `drawWireBox` en `Gizmos.cpp`**

Reemplazar la función completa (líneas 292-310):

```cpp
void Gizmos::drawWireBox(const glm::mat4& transform, const glm::vec3& halfExtents, const glm::vec3& color)
{
    if (!get().m_enabled) return;
    glm::vec3 h = halfExtents;
    std::array<glm::vec3, 8> local = {
        glm::vec3(-h.x, -h.y, -h.z),
        glm::vec3( h.x, -h.y, -h.z),
        glm::vec3( h.x,  h.y, -h.z),
        glm::vec3(-h.x,  h.y, -h.z),
        glm::vec3(-h.x, -h.y,  h.z),
        glm::vec3( h.x, -h.y,  h.z),
        glm::vec3( h.x,  h.y,  h.z),
        glm::vec3(-h.x,  h.y,  h.z),
    };
    std::array<glm::vec3, 8> corners;
    for (int i = 0; i < 8; i++)
        corners[i] = glm::vec3(transform * glm::vec4(local[i], 1.0f));
    get().addBoxEdges(corners, color);
}
```

por:

```cpp
void Gizmos::drawWireBox(const glm::mat4& transform, const glm::vec3& center,
                          const glm::vec3& halfExtents, const glm::vec3& color)
{
    if (!get().m_enabled) return;
    glm::vec3 h = halfExtents;
    std::array<glm::vec3, 8> local = {
        center + glm::vec3(-h.x, -h.y, -h.z),
        center + glm::vec3( h.x, -h.y, -h.z),
        center + glm::vec3( h.x,  h.y, -h.z),
        center + glm::vec3(-h.x,  h.y, -h.z),
        center + glm::vec3(-h.x, -h.y,  h.z),
        center + glm::vec3( h.x, -h.y,  h.z),
        center + glm::vec3( h.x,  h.y,  h.z),
        center + glm::vec3(-h.x,  h.y,  h.z),
    };
    std::array<glm::vec3, 8> corners;
    for (int i = 0; i < 8; i++)
        corners[i] = glm::vec3(transform * glm::vec4(local[i], 1.0f));
    get().addBoxEdges(corners, color);
}
```

- [ ] **Step 3: Añadir `drawWirePlane` en `Gizmos.cpp`**

Añadir justo después del cierre de `drawWireCapsule` (tras la línea 399, antes de `} // namespace DonTopo`):

```cpp
void Gizmos::drawWirePlane(const glm::mat4& transform, const glm::vec3& center, const glm::vec3& color)
{
    if (!get().m_enabled) return;
    constexpr float kHalfSize   = 5.0f;
    constexpr int   kDivisions  = 10;
    constexpr float kStep       = (kHalfSize * 2.0f) / (float)kDivisions;

    for (int i = 0; i <= kDivisions; i++)
    {
        float offset = -kHalfSize + kStep * (float)i;

        glm::vec3 aX = center + glm::vec3(offset, 0.0f, -kHalfSize);
        glm::vec3 bX = center + glm::vec3(offset, 0.0f,  kHalfSize);
        get().addLine(glm::vec3(transform * glm::vec4(aX, 1.0f)),
                      glm::vec3(transform * glm::vec4(bX, 1.0f)), color);

        glm::vec3 aZ = center + glm::vec3(-kHalfSize, 0.0f, offset);
        glm::vec3 bZ = center + glm::vec3( kHalfSize, 0.0f, offset);
        get().addLine(glm::vec3(transform * glm::vec4(aZ, 1.0f)),
                      glm::vec3(transform * glm::vec4(bZ, 1.0f)), color);
    }
}
```

- [ ] **Step 4: Actualizar el único call site existente (`sandbox/main.cpp`) para que compile**

Este paso es temporal — Task 3 elimina el bloque por completo. Por ahora, para que el proyecto compile tras el cambio de firma, en `sandbox/src/main.cpp` (bloque `if (liveCube->hasBoxCollider())`, línea ~282-284) añadir el argumento `center` que falta:

```cpp
                if (liveCube->hasBoxCollider())
                    DonTopo::Gizmos::drawWireBox(liveCube->worldTransform,
                        liveCube->getBoxCollider()->getCenter(),
                        liveCube->getBoxCollider()->getHalfExtents(), glm::vec3(1.0f, 1.0f, 0.0f));
```

- [ ] **Step 5: Compilar**

Run: `cmake --build --preset debug`
Expected: 0 errores.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Gizmos.h engine/src/Gizmos.cpp sandbox/src/main.cpp
git commit -m "fix(gizmos): drawWireBox respeta center + añade drawWirePlane

drawWireBox ignoraba el offset Center del Box Collider desde su creación.
drawWirePlane dibuja un grid 10x10 en el plano XZ local, mismo patrón que
drawWireSphere/drawWireCapsule (transform + center + addLine/addArc)."
```

---

## Task 2: Cascada de collider en drawSelectionGizmo + limpieza del demo

**Files:**
- Modify: `engine/src/EditorUI.cpp:482-486` (`drawSelectionGizmo`)
- Modify: `sandbox/src/main.cpp:280-289` (eliminar bloque demo completo)

**Interfaces:**
- Consumes: `Gizmos::drawWireBox/drawWireSphere/drawWireCapsule/drawWirePlane` (Task 1); `GameObject::hasBoxCollider/hasSphereCollider/hasCapsuleCollider/hasPlaneCollider` y `getBoxCollider/getSphereCollider/getCapsuleCollider/getPlaneCollider` (ya existentes); `BoxCollider::getCenter/getHalfExtents`, `SphereCollider::getCenter/getRadius`, `CapsuleCollider::getCenter/getRadius/getHalfHeight`, `PlaneCollider::getCenter` (ya existentes)

- [ ] **Step 1: Extender `EditorUI::drawSelectionGizmo()`**

En `engine/src/EditorUI.cpp`, reemplazar (líneas 482-486):

```cpp
void EditorUI::drawSelectionGizmo()
{
    if (!m_selected)
        return;
    Gizmos::drawAxes(m_selected->worldTransform, selectionAxisScale(m_selected));
}
```

por:

```cpp
void EditorUI::drawSelectionGizmo()
{
    if (!m_selected)
        return;
    Gizmos::drawAxes(m_selected->worldTransform, selectionAxisScale(m_selected));

    const glm::vec3 kColliderColor(1.0f, 1.0f, 0.0f);
    if (m_selected->hasBoxCollider())
    {
        BoxCollider* bc = m_selected->getBoxCollider().get();
        Gizmos::drawWireBox(m_selected->worldTransform, bc->getCenter(),
                             bc->getHalfExtents(), kColliderColor);
    }
    else if (m_selected->hasSphereCollider())
    {
        SphereCollider* sc = m_selected->getSphereCollider().get();
        Gizmos::drawWireSphere(m_selected->worldTransform, sc->getCenter(),
                                sc->getRadius(), kColliderColor);
    }
    else if (m_selected->hasCapsuleCollider())
    {
        CapsuleCollider* cc = m_selected->getCapsuleCollider().get();
        Gizmos::drawWireCapsule(m_selected->worldTransform, cc->getCenter(),
                                 cc->getRadius(), cc->getHalfHeight(), kColliderColor);
    }
    else if (m_selected->hasPlaneCollider())
    {
        PlaneCollider* pc = m_selected->getPlaneCollider().get();
        Gizmos::drawWirePlane(m_selected->worldTransform, pc->getCenter(), kColliderColor);
    }
}
```

(`BoxCollider.h`, `SphereCollider.h`, `CapsuleCollider.h`, `PlaneCollider.h` ya están incluidos en `EditorUI.cpp:6-9` — no hace falta añadir includes.)

- [ ] **Step 2: Eliminar el bloque demo de `sandbox/main.cpp`**

En `engine/src/../sandbox/src/main.cpp`, eliminar por completo (incluye el `if` añadido temporalmente en Task 1 Step 4):

```cpp
            if (liveCube)
            {
                if (liveCube->hasBoxCollider())
                    DonTopo::Gizmos::drawWireBox(liveCube->worldTransform,
                        liveCube->getBoxCollider()->getCenter(),
                        liveCube->getBoxCollider()->getHalfExtents(), glm::vec3(1.0f, 1.0f, 0.0f));

                DonTopo::Gizmos::drawRay(
                    glm::vec3(liveCube->worldTransform[3].x, liveCube->worldTransform[3].y + 200.0f, liveCube->worldTransform[3].z),
                    glm::vec3(0.0f, -1.0f, 0.0f), 400.0f, glm::vec3(1.0f, 0.0f, 1.0f));
            }
```

por (se conserva `drawRay`, solo se quita el `drawWireBox` del collider):

```cpp
            if (liveCube)
            {
                DonTopo::Gizmos::drawRay(
                    glm::vec3(liveCube->worldTransform[3].x, liveCube->worldTransform[3].y + 200.0f, liveCube->worldTransform[3].z),
                    glm::vec3(0.0f, -1.0f, 0.0f), 400.0f, glm::vec3(1.0f, 0.0f, 1.0f));
            }
```

- [ ] **Step 3: Compilar**

Run: `cmake --build --preset debug`
Expected: 0 errores.

- [ ] **Step 4: Verificación manual**

Ejecutar `Sandbox.exe` y en el editor:

1. Seleccionar un GameObject con Box Collider → aparece wireframe amarillo del tamaño/posición correctos.
2. Cambiar Size/Center en Properties → el wireframe se actualiza sin retraso.
3. Repetir con Sphere Collider (Radius/Center) y Capsule Collider (Radius/HalfHeight/Center).
4. Seleccionar un GameObject con Plane Collider → aparece grid 10×10 en el plano XZ local.
5. Pulsar "x" (remove collider) → el wireframe desaparece inmediatamente.
6. Click en zona vacía del panel Scene → wireframe y ejes desaparecen juntos.
7. Cambiar de un objeto con collider a otro sin collider → no queda wireframe "pegado" del anterior.

- [ ] **Step 5: Commit**

```bash
git add engine/src/EditorUI.cpp sandbox/src/main.cpp
git commit -m "feat(editor): wireframe de collider para las 4 formas al seleccionar

Extiende drawSelectionGizmo() con una cascada por tipo de collider del
GameObject seleccionado (Box/Sphere/Capsule/Plane), en vez del demo
hardcodeado en sandbox/main.cpp que solo cubría un objeto fijo con Box
Collider. Se actualiza en vivo (getters leídos cada frame) y desaparece al
quitar el collider o deseleccionar."
```
