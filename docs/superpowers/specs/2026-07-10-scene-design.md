# Scene — Design Spec

## Problema

Hoy no existe clase `Scene`. La raíz del árbol de GameObjects vive como variable local (`DonTopo::GameObject root("root")`) en `sandbox/src/main.cpp:44`. `Renderer` guarda solo un puntero no-propietario (`m_sceneRoot`), `EditorUI::draw(...)` recibe `GameObject* sceneRoot` cada frame, y `PhysicsManager`/`AudioManager` son objetos hermanos coordinados a mano en `main.cpp`.

Dos problemas concretos derivan de esto:

1. **Sync física↔transform disperso**: el loop principal (`main.cpp:211-262`) hace un `root.traverse(...)` manual cada frame para leer/escribir poses de PhysX contra `worldTransform`. Es lógica de motor viviendo en el punto de entrada de la sandbox.
2. **Orden de destrucción frágil**: `physics`/`audio` se declaran antes que `root` en `main.cpp` (comentarios líneas 32-37) para garantizar que los colliders/audioclips (poseídos por los GameObjects) se liberen antes que los managers PhysX/FMOD. Este invariante es implícito — depende del orden textual de declaración de variables, invisible en el call site, y roto fácilmente por cualquier reordenamiento futuro.

## Objetivo

Introducir una clase `Scene` que:

- Posea el árbol de GameObjects (reemplaza el `GameObject root` local).
- Encapsule el traverse de sync física↔transform como método explícito.
- Haga explícito (no implícito por orden de declaración) el invariante de shutdown ordenado respecto a `PhysicsManager`/`AudioManager`.

Fuera de scope esta iteración: multi-escena, serialización a disco, ECS genérico de componentes (se mantiene composición fija por campos en `GameObject`, patrón ya establecido).

## Diseño

### 1. Estructura de datos

```cpp
// engine/include/DonTopo/Scene.h
class Scene {
public:
    explicit Scene(std::string name = "Scene");

    GameObject& getRoot();
    const GameObject& getRoot() const;

    GameObject* addGameObject(const std::string& name, GameObject* parent = nullptr);
    void removeGameObject(GameObject* node);

    template<typename Fn> void traverse(Fn&& fn);

    void update(float dt, PhysicsManager& physics);
    void shutdown(PhysicsManager& physics, AudioManager& audio);

private:
    std::string m_name;
    GameObject m_root; // ownership por valor, no puntero — mismo patrón que hoy
};
```

- `m_root` es un `GameObject` por valor (no `unique_ptr`), igual que el `root` local actual.
- `addGameObject(name, parent=nullptr)`: si `parent` es null, cuelga de `m_root` directo vía `m_root.addChild(name)`; si no, delega en `parent->addChild(name)`.
- `removeGameObject(node)`: recorre el árbol para localizar el padre real de `node` y lo borra de su vector de hijos (reemplaza la búsqueda/erase manual que hoy hace EditorUI).
- `traverse(fn)`: delega directo en `m_root.traverse(fn)`.

### 2. `update(dt, physics)` — sync física↔transform

Encapsula el traverse manual de `main.cpp:211-262`. Recorre `m_root`; por cada nodo con collider:

- `useGravity == true` (dinámico, simulado por PhysX): lee pose del `PxActor` → escribe en `worldTransform` del nodo.
- `useGravity == false` (kinemático): lee `worldTransform` del nodo → empuja pose al `PxActor`.
- Al final invoca `m_root.updateWorldTransforms(identity)` para propagar transforms locales en nodos sin collider.

`stepSimulation` del `PhysicsManager` se sigue llamando aparte en `main.cpp` (no se mueve dentro de `update`):

```cpp
physics.stepSimulation(dt);
scene.update(dt, physics);
```

### 3. `shutdown(physics, audio)` — limpieza ordenada explícita

Recorre `m_root` vía `traverse`; por cada nodo, limpia los `shared_ptr` de componentes que dependan de managers externos: `setBoxCollider(nullptr)`, `setSphereCollider(nullptr)`, `setCapsuleCollider(nullptr)`, `setPlaneCollider(nullptr)`, `setAudioClip(nullptr)` (los que apliquen por nodo).

Se llama explícitamente en `main.cpp` antes de que `physics`/`audio` salgan de scope:

```cpp
scene.shutdown(physics, audio);
// physics y audio se destruyen a continuación, con la PxScene/sistema
// de audio ya sin referencias vivas desde el árbol de GameObjects
```

Esto reemplaza el comentario-advertencia + loop manual actual (`main.cpp` líneas ~300-309) por una llamada explícita y visible en el call site. El orden de declaración de variables en `main.cpp` deja de ser un invariante crítico oculto.

### 4. Integración Renderer / EditorUI / main.cpp

- `Renderer::setSceneRoot(GameObject*)` no cambia de firma; se llama con `&scene.getRoot()`.
- `EditorUI::draw(...)` cambia de firma: en vez de (o además de) `GameObject* sceneRoot`, recibe `Scene&` — necesario porque el borrado desde la jerarquía pasa a usar `scene.removeGameObject(node)`.
- El mecanismo de borrado diferido de `EditorUI` (`m_pendingDelete`, hoy resuelto con `parent->removeChild(...)`/erase manual a fin de frame) pasa a invocar `scene.removeGameObject(node)` en ese mismo punto — sin cambiar el patrón de "diferir hasta fin de frame" (sigue siendo necesario porque la recursión de dibujo ocurre sobre el propio vector de hijos).
- `main.cpp`: `DonTopo::GameObject root("root");` se reemplaza por `DonTopo::Scene scene;`. Las llamadas de setup inicial (`root.addChild(...)`, o equivalentes con nombres reales de la escena demo) pasan a `scene.addGameObject(...)`.
- La recolección de `allNodes` (usada en init para separar meshes estáticos/skinned en dos pasadas del Renderer) sigue igual, alimentada desde `scene.traverse(...)` en vez de `root.traverse(...)`.

### 5. Testing / verificación

No hay framework de tests unitarios en el repo (motor verificado manualmente vía sandbox). Verificación de esta feature:

- Compila y corre el sandbox sin cambios de comportamiento visible.
- Scene panel de EditorUI muestra la jerarquía igual que antes.
- Física sigue sincronizando: soltar un objeto con RigidBody dinámico cae y colisiona correctamente.
- Borrar un GameObject desde la jerarquía (con collider/audioclip) no crashea.
- Cerrar la aplicación no crashea (verifica que `shutdown()` ordena correctamente la liberación de colliders/audioclips antes de destruir `physics`/`audio`).

## Riesgos / notas de implementación

- `removeGameObject` necesita localizar el padre de `node` recorriendo el árbol (no hay puntero `parent` fiable hacia atrás verificado en este spec — si `GameObject` ya expone `parent` de forma consistente, usarlo directo en vez de recorrer; confirmar en implementación).
- Cambiar la firma de `EditorUI::draw(...)` de `GameObject*` a `Scene&` toca todos los call sites existentes de esa función — revisar en implementación cuántos hay.
