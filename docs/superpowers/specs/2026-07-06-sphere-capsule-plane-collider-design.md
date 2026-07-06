# Diseño: componentes Sphere Collider, Capsule Collider y Plane Collider

## Objetivo

Extender el editor con tres tipos de collider nuevos (Sphere, Capsule, Plane),
replicando el patrón ya establecido por `BoxCollider` (ver
`docs/superpowers/specs/2026-07-06-boxcollider-component-design.md`): cada uno
es un componente de física fusionado con RigidBody (un único `PxRigidDynamic`
por dentro, alternando estático/dinámico vía flags kinematic/gravity),
añadible/editable/borrable desde el panel Properties del editor ImGui.

## Contexto actual (post BoxCollider)

- `BoxCollider` (`engine/include/DonTopo/BoxCollider.h`,
  `engine/src/BoxCollider.cpp`) es la referencia completa: ctor recibe
  `actor`/`shape` ya creados por `PhysicsManager`, no copiable, dueño único vía
  `shared_ptr` en `GameObject`. PhysX queda oculto tras `void*` en el header,
  casteado en el `.cpp` bajo `#ifdef DT_PHYSX_ENABLED`.
- `PhysicsManager::createBoxColliderComponent` crea el `PxRigidDynamic` vacío,
  el shape exclusivo (`PxRigidActorExt::createExclusiveShape`), aplica
  `localPose` = center, `updateMassAndInertia`, flags kinematic/gravity, y
  `scene->addActor`.
- `GameObject` almacena `m_boxCollider` como `shared_ptr<BoxCollider>` con
  `has/get/setBoxCollider()` — **no** hay enum ni variant de tipo de collider;
  cada tipo es un slot independiente.
- `EditorUI` tiene `drawBoxColliderSection()` (cache de edición
  `m_colliderCachedFor`/`m_editColliderCenter`/`m_editColliderSize`/
  `m_editUseGravity`, resync al cambiar de selección o en vivo si es dinámico
  y no se arrastra) y `drawAddComponentButton()` (popup con un único
  `Selectable("Box Collider")`, deshabilitado si `hasBoxCollider()`).
- Orden de destrucción crítico (documentado en `sandbox/src/main.cpp`):
  `PhysicsManager` se declara antes que `GameObject root` para que el árbol se
  destruya primero (liberando colliders) y `physics` después. Antes de
  `physics.shutdown()`, un traverse explícito hace `setBoxCollider(nullptr)`
  en cada GameObject para forzar el release del actor mientras la `PxScene`
  todavía existe.
- No existe serialización a disco de ningún componente.

## Decisiones de diseño (confirmadas)

1. **Exclusividad mutua entre los 4 tipos**: un GameObject solo puede tener
   *un* collider de física a la vez (Box, Sphere, Capsule o Plane). El popup
   "Add" deshabilita las 4 entradas si `hasAnyCollider()` ya es true.
2. **Capsule alineada a Y**: PhysX orienta `PxCapsuleGeometry` por defecto a
   lo largo del eje X local del shape. Para que el editor sea intuitivo
   (cápsula "de pie", tipo personaje), se compone una rotación fija de 90°
   sobre Z en el `localPose` del shape, igual que hacen otros motores/plugins
   sobre PhysX crudo.
3. **Plane siempre estático**: no tiene checkbox "Use Gravity" ni concepto de
   Size (plano infinito). `isDynamic()` retorna `false` hardcoded — el motor
   siempre empuja la pose del GameObject hacia PhysX (`syncTransform`), nunca
   lee de vuelta.

## Arquitectura: nuevas clases de collider

### `SphereCollider`

```cpp
class SphereCollider {
public:
    SphereCollider(void* actor, void* shape, float radius,
                   const glm::vec3& center, bool useGravity);
    ~SphereCollider(); // actor->release()

    void setCenter(const glm::vec3& center);   // PxShape::setLocalPose
    void setRadius(float radius);              // PxShape::setGeometry(PxSphereGeometry)
    void setUseGravity(bool enabled);

    glm::vec3 getCenter() const;
    float     getRadius() const;
    bool      getUseGravity() const;
    bool      isDynamic() const;

    glm::mat4 getWorldTransform() const;
    void syncTransform(const glm::mat4& worldTransform);
    void teleport(const glm::mat4& worldTransform);
};
```

### `CapsuleCollider`

```cpp
class CapsuleCollider {
public:
    CapsuleCollider(void* actor, void* shape, float radius, float halfHeight,
                     const glm::vec3& center, bool useGravity);
    ~CapsuleCollider();

    void setCenter(const glm::vec3& center);   // localPose = center + quat(90°, Z)
    void setRadius(float radius);              // PxCapsuleGeometry nueva
    void setHalfHeight(float halfHeight);       // PxCapsuleGeometry nueva
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
```

Nota UI: el panel edita "Height" (total), igual que Box edita "Size" (total)
— se divide entre 2 al llamar `setHalfHeight`.

### `PlaneCollider`

```cpp
class PlaneCollider {
public:
    PlaneCollider(void* actor, void* shape, const glm::vec3& center);
    ~PlaneCollider();

    void setCenter(const glm::vec3& center);   // reposiciona el plano

    glm::vec3 getCenter() const;
    bool      isDynamic() const { return false; } // siempre estático/kinematic

    glm::mat4 getWorldTransform() const;
    void syncTransform(const glm::mat4& worldTransform);
    void teleport(const glm::mat4& worldTransform);
};
```

Sin `useGravity`: el actor se crea siempre kinematic + gravedad desactivada,
sin exponer el flag.

## `PhysicsManager`: nuevas funciones de creación

Mismo flujo que `createBoxColliderComponent` (decompose worldTransform →
`createRigidDynamic` vacío → shape exclusivo con geometría → localPose center
→ mass/inertia → flags → `addActor`), variando solo la geometría:

```cpp
std::shared_ptr<SphereCollider> createSphereColliderComponent(
    float radius, const glm::vec3& center,
    const glm::mat4& worldTransform, bool useGravity, float density = 1.0f);

std::shared_ptr<CapsuleCollider> createCapsuleColliderComponent(
    float radius, float halfHeight, const glm::vec3& center,
    const glm::mat4& worldTransform, bool useGravity, float density = 1.0f);

std::shared_ptr<PlaneCollider> createPlaneColliderComponent(
    const glm::vec3& center, const glm::mat4& worldTransform);
```

- Sphere/Capsule: idénticas a Box salvo `PxSphereGeometry(radius)` /
  `PxCapsuleGeometry(radius, halfHeight)`, y en Capsule el `localPose` del
  shape compone `center` con el quat de corrección de eje (90° sobre Z).
- Plane: geometría vía `PxTransformFromPlaneEquation` para mapear la normal
  por defecto (+Y en espacio local) al convenio de PhysX (normal = eje X
  local del shape). Actor siempre creado con `eKINEMATIC` +
  `eDISABLE_GRAVITY` fijos (sin parámetro `useGravity`). Sin
  `updateMassAndInertia` (no aplica a un actor puramente kinematic sin masa
  relevante).

## `GameObject`: nuevos slots

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

bool hasAnyCollider() const; // OR de los 4 has*Collider()
```

Mismo patrón de slot independiente que `m_boxCollider` — no se introduce
enum/variant. `hasAnyCollider()` es la única pieza nueva de lógica compartida,
usada por la exclusividad mutua en `EditorUI` y opcionalmente por el loop de
sync en `main.cpp`.

## UI del editor (`EditorUI`)

### Nuevas secciones en `drawProperties()`

Tras `drawBoxColliderSection()`, se añaden en el mismo orden que aparecerán en
el popup Add:

```cpp
drawSphereColliderSection();
drawCapsuleColliderSection();
drawPlaneColliderSection();
drawAddComponentButton();
```

Cada una replica el patrón de `drawBoxColliderSection()`: estado de cache
propio (`m_sphereColliderCachedFor`/`m_editSphereCenter`/`m_editSphereRadius`/
`m_editSphereUseGravity`, y análogos para Capsule y Plane), header
`TreeNodeEx` con botón "x" para borrar, resync al cambiar de selección o en
vivo si es dinámico y no se arrastra.

Campos por tipo:

- **Sphere Collider**: Center (3 DragFloat), Radius (1 DragFloat, clamp
  mínimo 0.01), Use Gravity (checkbox).
- **Capsule Collider**: Center (3 DragFloat), Radius (1 DragFloat, mínimo
  0.01), Height (1 DragFloat, mínimo 0.01, total — se divide entre 2 al
  llamar `setHalfHeight`), Use Gravity (checkbox).
- **Plane Collider**: solo Center (3 DragFloat). Sin Size, sin Use Gravity.

Al cambiar cualquier campo: `setCenter`/`setRadius`/`setHalfHeight`/
`setUseGravity` sobre el componente ya existente. Al click en "x":
`m_selected->setXxxCollider(nullptr)` + reset del cache correspondiente.

### Botón "Add"

`drawAddComponentButton()` gana 3 `Selectable` más en el mismo popup
(`AddComponentPopup`), junto al de Box Collider:

```cpp
bool alreadyHasAny = m_selected->hasAnyCollider();
ImGui::BeginDisabled(alreadyHasAny);

if (ImGui::Selectable("Box Collider") && !alreadyHasAny && m_physics)
    m_selected->setBoxCollider(m_physics->createBoxColliderComponent(
        {25,25,25}, {0,0,0}, m_selected->worldTransform, false));

if (ImGui::Selectable("Sphere Collider") && !alreadyHasAny && m_physics)
    m_selected->setSphereCollider(m_physics->createSphereColliderComponent(
        25.0f, {0,0,0}, m_selected->worldTransform, false));

if (ImGui::Selectable("Capsule Collider") && !alreadyHasAny && m_physics)
    m_selected->setCapsuleCollider(m_physics->createCapsuleColliderComponent(
        15.0f, 25.0f, {0,0,0}, m_selected->worldTransform, false));

if (ImGui::Selectable("Plane Collider") && !alreadyHasAny && m_physics)
    m_selected->setPlaneCollider(m_physics->createPlaneColliderComponent(
        {0,0,0}, m_selected->worldTransform));

ImGui::EndDisabled();
```

(El guard de Box Collider pasa de `hasBoxCollider()` a `hasAnyCollider()` —
único cambio retroactivo sobre el código existente.)

## `sandbox/src/main.cpp`: loop de sync y limpieza

El bloque del loop principal que hoy hace `hasBoxCollider()` se extiende con
los 3 tipos nuevos, mismo patrón `isDynamic()`/`getWorldTransform()`/
`syncTransform()` (Plane siempre toma la rama `else` porque `isDynamic()`
retorna `false`):

```cpp
if (go->hasBoxCollider())     { auto& c = go->getBoxCollider();     if (c->isDynamic()) go->worldTransform = c->getWorldTransform(); else c->syncTransform(go->worldTransform); }
if (go->hasSphereCollider())  { auto& c = go->getSphereCollider();  if (c->isDynamic()) go->worldTransform = c->getWorldTransform(); else c->syncTransform(go->worldTransform); }
if (go->hasCapsuleCollider()) { auto& c = go->getCapsuleCollider(); if (c->isDynamic()) go->worldTransform = c->getWorldTransform(); else c->syncTransform(go->worldTransform); }
if (go->hasPlaneCollider())   { auto& c = go->getPlaneCollider();   c->syncTransform(go->worldTransform); }
```

El traverse de limpieza antes de `physics.shutdown()` añade
`setSphereCollider(nullptr)`, `setCapsuleCollider(nullptr)`,
`setPlaneCollider(nullptr)` junto al `setBoxCollider(nullptr)` existente —
mismo orden crítico de destrucción (root se limpia mientras la `PxScene`
sigue viva).

## `engine/CMakeLists.txt`

Añadir `src/SphereCollider.cpp`, `src/CapsuleCollider.cpp`,
`src/PlaneCollider.cpp` a la lista de fuentes, junto a `src/BoxCollider.cpp`.

## Fuera de alcance

- Enum/variant unificado de tipo de collider (se mantiene el patrón de slots
  independientes, igual que Box).
- Otros tipos de componente en el menú Add (Mesh Collider, Audio Source,
  etc.).
- Múltiples colliders simultáneos por GameObject (explícitamente excluido por
  la decisión de exclusividad mutua).
- Masa/densidad editable desde UI (se mantiene `density=1.0f` por defecto en
  Sphere/Capsule; Plane no aplica).
- Undo/redo de la adición/edición/borrado de componentes.
- Serialización a disco.

## Plan de verificación manual

1. Seleccionar un GameObject sin física → "Add" → las 4 opciones están
   habilitadas.
2. Añadir "Sphere Collider" → aparece sección con Center (0,0,0), Radius 25,
   Use Gravity desactivado. Las otras 3 opciones del popup Add quedan
   deshabilitadas (grayed out) mientras el Sphere exista.
3. Activar "Use Gravity" en el Sphere → cae con la gravedad global; editar
   Radius/Center mientras cae no lo congela ni resetea posición.
4. Borrar el Sphere (icono "x") → las 4 opciones de Add vuelven a habilitarse.
5. Repetir 2-4 con "Capsule Collider": verificar que la cápsula se ve/orienta
   vertical (eje Y), no acostada en X; Height y Radius editables en vivo.
6. Añadir "Plane Collider" a un GameObject → sección solo con Center, sin
   checkbox Use Gravity; el plano no cae nunca aunque se edite Center.
7. Añadir cualquiera de los 4 tipos y luego borrar el GameObject entero
   (Delete/menú contextual) → no crash al cerrar la app ni al seguir
   ejecutando.
8. Cerrar la app con un Sphere/Capsule/Plane collider activo en la escena →
   no crash (confirma orden de destrucción replicado en el traverse de
   `main.cpp`).
