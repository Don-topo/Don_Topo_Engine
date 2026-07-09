# Diseño: líneas de collider para todas las formas al seleccionar

## Objetivo

Hoy solo el Box Collider dibuja su wireframe (`Gizmos::drawWireBox`), y solo
para un objeto demo hardcodeado (`liveCube` en `sandbox/main.cpp`), no para
el objeto realmente seleccionado en el editor. Extender el dibujo a las 4
formas de collider (Box, Sphere, Capsule, Plane), atado al `GameObject`
seleccionado en el panel Scene — igual que ya funciona `drawSelectionGizmo()`
para los ejes de coordenadas (ver
`docs/superpowers/specs/2026-07-07-selection-axes-gizmo-design.md`).

- Al seleccionar un GameObject con collider, se dibuja el wireframe de su
  forma en amarillo `(1,1,0)`.
- Al editar Center/Size/Radius/HalfHeight desde el panel Properties, el
  wireframe se actualiza en vivo, sin retraso de un frame.
- Al pulsar el botón "x" de remove del collider, el wireframe desaparece ese
  mismo frame.
- Un GameObject solo puede tener un collider a la vez (invariante ya
  existente en `GameObject`), así que nunca hay ambigüedad sobre cuál dibujar.

## Contexto actual

- `Gizmos::drawWireBox/drawWireSphere/drawWireCapsule` ya existen
  (`engine/include/DonTopo/Gizmos.h`). No existe `drawWirePlane`.
- `drawWireBox(transform, halfExtents, color)` **no recibe `center`** — bug
  existente: el offset de Center del Box Collider nunca se refleja en el
  wireframe. `drawWireSphere`/`drawWireCapsule` sí reciben `center`.
- `EditorUI::drawSelectionGizmo()` (`engine/src/EditorUI.cpp:482`) ya se
  llama cada frame para `m_selected`, hoy solo dibuja ejes.
- `sandbox/src/main.cpp:280-289` dibuja `drawWireBox` para `liveCube`
  (objeto demo fijo desde el plan de Gizmos original) — queda redundante y
  además es el único sitio donde se dibuja hoy. Se elimina.
- Getters ya disponibles: `BoxCollider::getHalfExtents/getCenter`,
  `SphereCollider::getRadius/getCenter`,
  `CapsuleCollider::getRadius/getHalfHeight/getCenter`,
  `PlaneCollider::getCenter` (sin tamaño — plano infinito).

## Arquitectura

Todo el cambio vive en `Gizmos` (nueva función + fix de firma) y
`EditorUI::drawSelectionGizmo()`. No se toca `Renderer` ni el contrato de
`GameObject`.

### `Gizmos::drawWireBox` — añadir `center`

```cpp
static void drawWireBox(const glm::mat4& transform, const glm::vec3& center,
                        const glm::vec3& halfExtents, const glm::vec3& color);
```

Los 8 vértices locales pasan de `(±h.x, ±h.y, ±h.z)` a
`center + (±h.x, ±h.y, ±h.z)`, igual que ya hace `addArc` con su parámetro
`center` para sphere/capsule. Único call site existente
(`sandbox/main.cpp`) se elimina en este mismo cambio, así que no hay
callers que migrar.

### `Gizmos::drawWirePlane` — nueva

```cpp
static void drawWirePlane(const glm::mat4& transform, const glm::vec3& center,
                          const glm::vec3& color);
```

Grid 10×10 unidades (constante `kPlaneGizmoHalfSize = 5.0f`, 10 divisiones)
en el plano local XZ (`y = center.y`), coherente con la rotación fija que
`PlaneCollider` ya aplica internamente para mapear su normal a `+Y` local.
Implementación: líneas paralelas al eje X y al eje Z, cada una transformada
por `transform` igual que `addBoxEdges`/`addArc`.

### `EditorUI::drawSelectionGizmo()` — cascada por tipo

```cpp
void EditorUI::drawSelectionGizmo()
{
    if (!m_selected) return;
    Gizmos::drawAxes(m_selected->worldTransform, selectionAxisScale(m_selected));

    const glm::vec3 kColliderColor(1.0f, 1.0f, 0.0f);
    if (m_selected->hasBoxCollider())
    {
        auto* bc = m_selected->getBoxCollider().get();
        Gizmos::drawWireBox(m_selected->worldTransform, bc->getCenter(),
                             bc->getHalfExtents(), kColliderColor);
    }
    else if (m_selected->hasSphereCollider())
    {
        auto* sc = m_selected->getSphereCollider().get();
        Gizmos::drawWireSphere(m_selected->worldTransform, sc->getCenter(),
                                sc->getRadius(), kColliderColor);
    }
    else if (m_selected->hasCapsuleCollider())
    {
        auto* cc = m_selected->getCapsuleCollider().get();
        Gizmos::drawWireCapsule(m_selected->worldTransform, cc->getCenter(),
                                 cc->getRadius(), cc->getHalfHeight(), kColliderColor);
    }
    else if (m_selected->hasPlaneCollider())
    {
        auto* pc = m_selected->getPlaneCollider().get();
        Gizmos::drawWirePlane(m_selected->worldTransform, pc->getCenter(), kColliderColor);
    }
}
```

Se leen los getters en vivo cada frame — igual que ya hace
`selectionAxisScale`. Esto cubre los tres requisitos sin estado ni caché
extra:

- **Actualización en vivo al editar tamaño**: el frame siguiente al drag en
  Properties, el getter ya devuelve el nuevo valor.
- **Desaparece al quitar el collider**: tras `setBoxCollider(nullptr)` (o
  el equivalente de cada tipo), `hasXCollider()` es `false` ese mismo frame
  → ninguna rama de la cascada dibuja nada.
- **Aparece solo al seleccionar**: si no hay `m_selected`, la función
  retorna antes de cualquier dibujo (igual que hoy).

## Fuera de alcance

- Tamaño configurable del grid del Plane Collider (fijo 10×10).
- Color distinto por tipo de collider (amarillo único para las 4 formas).
- Multi-selección.
- Cualquier collider "Cylinder" separado — no existe ese tipo en el motor;
  "cilindro" del pedido de usuario se cubre con Capsule Collider (única
  forma tipo cilindro existente).

## Limpieza relacionada

Se elimina de `sandbox/src/main.cpp` el bloque:

```cpp
if (liveCube->hasBoxCollider())
    DonTopo::Gizmos::drawWireBox(liveCube->worldTransform,
        liveCube->getBoxCollider()->getHalfExtents(), glm::vec3(1.0f, 1.0f, 0.0f));
```

Queda redundante y con la firma antigua de `drawWireBox` (sin `center`) una
vez que la selección dibuja el wireframe de cualquier collider
automáticamente. El resto del bloque (`drawRay`, `drawFrustum`) no cambia.

## Plan de verificación manual

1. Crear/seleccionar un GameObject con Box Collider → aparece wireframe
   amarillo ajustado a Size, en la posición de Center.
2. Cambiar Size o Center desde Properties → el wireframe se mueve/ajusta en
   el mismo frame (sin retraso visible).
3. Repetir 1-2 con Sphere Collider (Radius, Center) y Capsule Collider
   (Radius, HalfHeight, Center).
4. Seleccionar un GameObject con Plane Collider → aparece grid 10×10 en el
   plano XZ local, en Center.
5. Pulsar "x" para quitar el collider (cualquier tipo) → el wireframe
   desaparece inmediatamente, sin esperar a deseleccionar.
6. Deseleccionar (click en zona vacía) → wireframe y ejes desaparecen juntos.
7. Cambiar de un GameObject con collider a otro sin collider → el wireframe
   del anterior no queda "pegado" (solo se dibuja para `m_selected` actual).
