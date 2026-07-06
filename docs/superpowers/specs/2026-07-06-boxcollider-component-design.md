# Diseño: componente Box Collider desde el editor

## Objetivo

Permitir añadir/editar/eliminar un componente de física (Box Collider) a un
GameObject desde el panel Properties del editor ImGui, integrado en tiempo
real con el sistema de físicas (PhysX vía `PhysicsManager`).

## Contexto actual

- `GameObject` tiene dos slots de física separados: `m_collider`
  (`BoxCollider`, estático, `PxRigidStatic`) y `m_rigidBody` (`RigidBody`,
  dinámico, `PxRigidDynamic`). Se usan hoy solo desde `sandbox/src/main.cpp`
  (suelo = collider estático, cubo = rigidbody dinámico) — no hay UI para
  crearlos.
- `EditorUI::drawProperties()` ya edita Transform (Position/Rotation/Scale)
  de GameObject con un patrón de cache (`m_editPosition` etc.), resincronizado
  al cambiar de selección, y con refresco en vivo cuando el objeto tiene
  RigidBody (para reflejar la caída física).
- El borrado de GameObject (`m_pendingDelete` → erase del `unique_ptr` en
  `parent->children`) ya libera correctamente cualquier recurso de física
  vía RAII: el destructor de `GameObject` destruye sus `shared_ptr` de
  colliders, y `~BoxCollider()`/`~RigidBody()` llaman `actor->release()`.
  Esto se mantiene igual con la clase fusionada — no requiere cambios.

## Arquitectura: fusión de BoxCollider + RigidBody

`RigidBody.h/.cpp` se elimina. `BoxCollider` pasa a ser el único componente
de física de tipo caja, y usa siempre un `physx::PxRigidDynamic` por dentro
(nunca `PxRigidStatic`), alternando comportamiento vía flags:

- **Use Gravity = true** → actor dinámico normal. PhysX mueve su pose cada
  `stepSimulation`; el motor lee `actor->getGlobalPose()` cada frame y lo
  aplica al `worldTransform` del GameObject (igual que el `RigidBody` actual).
- **Use Gravity = false** → mismo actor, pero con
  `PxRigidBodyFlag::eKINEMATIC = true` y `PxActorFlag::eDISABLE_GRAVITY = true`.
  No cae ni es empujado; el GameObject controla su pose y el motor la empuja
  hacia PhysX cada frame vía `setKinematicTarget` (igual que el `BoxCollider`
  estático actual, pero implementado sobre un actor kinematic en vez de
  static).

Togglear "Use Gravity" solo cambia flags del actor existente — no
destruye/recrea el `PxRigidActor`. Cambiar Size si recrea la geometría de la
shape (`PxShape::setGeometry`); cambiar Center actualiza el offset de la
shape dentro del actor (`PxShape::setLocalPose`), **no** la pose del actor —
así el offset nunca se mezcla con el cálculo de posición del GameObject.

### API `BoxCollider` (nueva)

```cpp
class BoxCollider {
public:
    BoxCollider(void* actor, void* shape, const glm::vec3& halfExtents,
                const glm::vec3& center, bool useGravity);
    ~BoxCollider(); // actor->release()

    void setCenter(const glm::vec3& center);        // PxShape::setLocalPose
    void setHalfExtents(const glm::vec3& half);      // PxShape::setGeometry
    void setUseGravity(bool enabled);                // toggle kinematic/disableGravity

    glm::vec3 getCenter() const;
    glm::vec3 getHalfExtents() const;
    bool      getUseGravity() const;

    // true si dinámico (useGravity=true): el motor debe LEER pose de PhysX
    // hacia el GameObject. false: el motor debe EMPUJAR pose del GameObject
    // hacia PhysX (setKinematicTarget).
    bool isDynamic() const;

    glm::mat4 getWorldTransform() const;             // lee actor->getGlobalPose()
    void syncTransform(const glm::mat4& worldTransform); // setKinematicTarget
};
```

### API `PhysicsManager` (reemplaza `createBoxCollider`/`createDynamicBoxCollider`)

```cpp
std::shared_ptr<BoxCollider> createBoxColliderComponent(
    const glm::vec3& halfExtents,
    const glm::vec3& center,
    const glm::mat4& worldTransform,
    bool useGravity,
    float density = 1.0f);
```

Siempre crea `PxRigidDynamic` + shape con `localPose` = `center`, y aplica
los flags kinematic/gravity según `useGravity`.

### API `GameObject`

Sustituye `m_collider`+`m_rigidBody` (y sus getters/setters) por un único
slot:

```cpp
void setBoxCollider(std::shared_ptr<BoxCollider> bc);
const std::shared_ptr<BoxCollider>& getBoxCollider() const;
bool hasBoxCollider() const;
```

### `sandbox/src/main.cpp` (migración de la demo)

- Suelo: `createBoxColliderComponent(halfExtents, {0,0,0}, floorPose, /*useGravity=*/false)`.
- Cubo: `createBoxColliderComponent({25,25,25}, {0,0,0}, cubeTransform, /*useGravity=*/true)`.
- Loop principal: el bloque que hoy hace `hasRigidBody()`/`hasCollider()`
  por separado pasa a:

```cpp
if (go->hasBoxCollider()) {
    auto& bc = go->getBoxCollider();
    if (bc->isDynamic())
        go->worldTransform = bc->getWorldTransform();
    else
        bc->syncTransform(go->worldTransform);
}
```

- Limpieza al cierre (`root.traverse(...)` antes de `physics.shutdown()`):
  `go->setBoxCollider(nullptr);` en vez de las dos llamadas actuales.

## UI del editor (`EditorUI`)

### Wiring `PhysicsManager`

- `EditorUI` gana `void setPhysicsManager(PhysicsManager* pm)` y guarda el
  puntero (no ownership).
- `Renderer` reenvía: `void setPhysicsManager(PhysicsManager* pm) { m_editorUI.setPhysicsManager(pm); }`
  (mismo patrón que `setOnAxisSelected`).
- `sandbox/src/main.cpp` llama `renderer.setPhysicsManager(&physics);` justo
  después de `physics.init()`.

### Sección "Box Collider" en Properties

Tras la sección Transform, si `m_selected->hasBoxCollider()`:

- Header (`TreeNodeEx`, abierto por defecto) con botón pequeño "x" alineado
  a la derecha del mismo row. Click → `m_selected->setBoxCollider(nullptr)`
  directo (no hace falta borrado diferido: no estamos en medio de un
  traverse recursivo del árbol de GameObjects).
- Campos, con el mismo patrón de cache que Transform (`m_colliderEditCenter`,
  `m_colliderEditSize`, resincronizados al cambiar de selección o — si
  `isDynamic()` y no se está arrastrando — cada frame para reflejar cambios
  de tamaño/gravedad hechos desde fuera):
  - **Center** (Position): 3 `DragFloat`.
  - **Size**: 3 `DragFloat`, clamp mínimo 0.01 (tamaño total, no half-extent
    — se divide entre 2 al pasarlo a `setHalfExtents`).
  - **Use Gravity**: checkbox.
- Al cambiar cualquier campo: `setCenter`/`setHalfExtents`/`setUseGravity`
  sobre el `BoxCollider` ya existente.

### Botón "Add"

Al final de `drawProperties()` (debajo de todas las secciones de
componentes): botón "Add". Click abre popup con una entrada:

- **"Box Collider"** — deshabilitada (grayed out) si
  `m_selected->hasBoxCollider()` ya es true (un solo Box Collider por
  GameObject, no es una lista).
- Click en la entrada habilitada:
  `m_selected->setBoxCollider(m_physics->createBoxColliderComponent(halfExtents={25,25,25}, center={0,0,0}, m_selected->worldTransform, /*useGravity=*/false));`

## Fuera de alcance

- Otros tipos de componente en el menú Add (Sphere Collider, Mesh Collider,
  Audio Source, etc.) — solo Box Collider por ahora.
- Múltiples Box Collider por GameObject.
- Masa/densidad editable desde UI (se mantiene `density=1.0f` por defecto).
- Undo/redo de la adición/edición/borrado de componentes.

## Plan de verificación manual

1. Seleccionar el suelo (o cualquier GameObject sin física) → botón "Add" →
   "Box Collider" → aparece sección con Center (0,0,0), Size (50,50,50),
   Use Gravity desactivado. El objeto no cae.
2. Activar "Use Gravity" → el objeto empieza a caer con la gravedad global
   de la escena.
3. Editar Size/Center mientras Use Gravity está activo → el objeto sigue
   cayendo con la nueva forma/offset (no se congela ni resetea posición).
4. Click en el icono "x" de la sección Box Collider → sección desaparece,
   objeto deja de colisionar/caer (verificar que no vibra ni crashea).
5. Añadir Box Collider a un GameObject y luego borrar el GameObject entero
   (Delete/menú contextual) → no crash al cerrar la app ni al seguir
   ejecutando (confirma que el release del actor PhysX ocurre correctamente
   vía RAII).
