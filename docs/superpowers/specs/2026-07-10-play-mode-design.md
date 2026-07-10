# Play Mode — Design Spec

## Problema

Hoy no existe distinción entre "editar" y "ejecutar" la escena. `sandbox/src/main.cpp`
llama `physics.stepSimulation(dt)` y `scene.update(dt, physics)` en cada frame del
loop principal, sin condición alguna. Dos problemas concretos derivan de esto:

1. **No se puede editar un objeto con collider dinámico**: `Scene::update` sincroniza
   cada frame la pose de PhysX hacia el `GameObject` cuando el collider tiene
   `useGravity=true` (rama "dinámica"). Si el usuario intenta mover ese objeto desde
   el gizmo o el panel Properties, la física lo sobreescribe en el frame siguiente —
   la edición nunca "se pega".
2. **No hay forma de probar el comportamiento en runtime sin arrancar de cero**: cualquier
   cambio hecho durante una sesión de física/audio en marcha es permanente; no hay
   "soltar la simulación y ver qué pasa" sin persistir el resultado a mano.

## Objetivo

Introducir un **Play Mode** con botón toolbar Play↔Stop:

- **Edit Mode** (estado por defecto): física y audio no corren. Los transforms
  padre→hijo se siguen propagando (gizmo/Properties funcionan con normalidad, y
  ahora sin que la física los pelee).
- **Play Mode**: física (`physics.stepSimulation` + `Scene::update`) y audio
  (`AudioManager::update`, tracking del listener 3D) corren con normalidad, igual
  que hoy.
- Al pulsar **Stop**, la escena se restaura exactamente al estado que tenía justo
  antes de pulsar Play (tipo Unity Play-In-Editor) — cualquier cambio hecho durante
  Play (posiciones movidas por física, GameObjects añadidos/borrados, ediciones a
  mano) se descarta.

Fuera de scope esta iteración: bloquear/deshabilitar edición de la UI mientras Play
Mode está activo (se permite editar sin restricción; el restore al pulsar Stop
descarta cualquier cambio igual). Pausar (a diferencia de parar) Play Mode. Guardar
explícitamente el estado de una sesión de Play (eso ya lo cubre "Save Scene").

## Diseño

### 1. Snapshot en memoria — extensión de `Scene`

`Scene` gana dos métodos públicos nuevos, extraídos de la lógica ya existente en
`save`/`load` (sin cambiar su comportamiento observable):

```cpp
// engine/include/DonTopo/Scene.h
#include <nlohmann/json_fwd.hpp>   // solo el forward-declare, no el header completo

class Scene {
public:
    // ... (sin cambios: addGameObject, removeGameObject, traverse, update, shutdown)

    // Serializa el árbol completo a un nlohmann::json en memoria (mismo contenido
    // que save() escribe a disco). Usado por save() y por el snapshot de Play Mode.
    nlohmann::json toJson() const;
    // Reemplaza el árbol actual por el contenido de j (mismas garantías de
    // atomicidad y manejo de errores que load()). Usado por load() y por el
    // restore de Play Mode al pulsar Stop.
    bool fromJson(const nlohmann::json& j, PhysicsManager& physics, AudioManager& audio);

    bool save(const std::string& path) const;   // ahora: FileManager::writeJson(path, toJson())
    bool load(const std::string& path, PhysicsManager& physics, AudioManager& audio);
    // ahora: parsea path y delega en fromJson(...)

private:
    std::string m_name;
    GameObject  m_root;
};
```

Motivo de RAM sobre fichero temporal: ambas opciones pagan el mismo coste de
recorrer el árbol de `GameObject` y construir el `nlohmann::json`; la opción fichero
añade además un paso de texto (`dump()`/`parse()`) y I/O de disco en cada ciclo
Play/Stop. Para el tamaño actual del motor la diferencia es imperceptible, pero
RAM evita ese paso por completo y es igual de simple de implementar dado que
`toJson`/`fromJson` ya son una extracción natural del código existente.

### 2. Estado Play Mode — `EditorUI` + `Renderer`

El estado (`bool m_isPlaying = false;`) vive en `EditorUI` (mismo sitio que ya
orquesta Save/Load Scene, con acceso directo a `m_scene`/`m_physics`/`m_audio`/
`m_renderer`). `Renderer` expone un getter que reenvía, mismo patrón que
`isViewportHovered()`:

```cpp
// EditorUI.h
bool isPlaying() const { return m_isPlaying; }
```
```cpp
// Renderer.h
bool isPlaying() const { return m_editorUI.isPlaying(); }
```

**Botón toolbar** (primero, antes de "Wireframe" — convención de editor):
- Label "Play" cuando `!m_isPlaying`; al pulsar: `m_playSnapshot = m_scene->toJson(); m_isPlaying = true;`
- Label "Stop" cuando `m_isPlaying` (mismo resaltado de color que ya usa el botón
  Wireframe activo); al pulsar, se ejecuta el restore (sección 3) y `m_isPlaying = false;`.

Nuevo miembro `nlohmann::json m_playSnapshot;` en `EditorUI` — requiere que
`EditorUI.h` incluya `<nlohmann/json_fwd.hpp>` también (mismo motivo que `Scene.h`).

### 3. Restore al pulsar Stop — reutiliza el código de "Load Scene"

El flujo de "Stop" es idéntico al de "Load Scene" (limpiar GPU de la escena actual
→ `fromJson` → re-registrar GPU, incluida la rama skinned) salvo que la fuente es
`m_playSnapshot` (JSON en memoria) en vez de un fichero. Se factoriza el bloque
compartido en un método privado nuevo:

```cpp
// EditorUI — privado
// Limpia GPU de la escena actual, reemplaza su contenido con j (vía
// Scene::fromJson) y re-registra GPU (estático + skinned) de lo que quede.
// Devuelve lo que devuelva fromJson. Usado por drawSceneDialog (Load Scene)
// y por el handler de Stop.
bool reloadSceneFromJson(const nlohmann::json& j);
```

`drawSceneDialog`'s rama Load pasa a llamar `reloadSceneFromJson(*parsed)` en vez
de tener la lógica inline; el handler de Stop llama
`reloadSceneFromJson(m_playSnapshot)`. Ambos limpian `m_selected = nullptr;` tras
un restore con éxito (los punteros del árbol anterior ya no son válidos).

### 4. Gate del loop principal — `sandbox/src/main.cpp`

Reemplaza (líneas ~205-208 actuales):
```cpp
audio.update(camera.getPos(), camera.getFront(), camera.getUp());

physics.stepSimulation(dt);
scene.update(dt, physics);
```
por:
```cpp
if (renderer.isPlaying())
{
    audio.update(camera.getPos(), camera.getFront(), camera.getUp());
    physics.stepSimulation(dt);
    scene.update(dt, physics);
}
else
{
    // Sin física corriendo, pero los transforms padre→hijo se siguen
    // propagando: gizmo/Properties deben seguir funcionando en Edit Mode.
    scene.getRoot().updateWorldTransforms();
}
```
Cero cambios a la clase `Scene` para este gate — `updateWorldTransforms()` ya es
público en `GameObject`, accesible vía `scene.getRoot()`.

**Por qué no basta con saltar solo `physics.stepSimulation`**: `Scene::update`
también sincroniza cada frame la pose de PhysX hacia el `GameObject` en la rama de
collider dinámico (`useGravity=true`), incluso si PhysX no avanzó ese frame (la
pose congelada se seguiría reaplicando sobre el `GameObject`, pisando cualquier
edición manual). Por eso `scene.update(...)` se salta entero en Edit Mode, no solo
`stepSimulation`.

### 5. Manejo de errores

`fromJson` sobre el propio snapshot que `toJson()` acaba de generar segundos antes
no debería fallar en la práctica (mismo contenido, misma sesión). Si aun así
devolviera `false` (p.ej. un asset referenciado se borró/movió del disco durante
la sesión de Play), se muestra `m_sceneIOError` igual que en "Load Scene" — mismo
comportamiento ya aceptado y revisado en `Scene::fromJson`/`load`.

### 6. Testing

Manual, sin framework — igual que el resto del motor:
- Play con el cubo dinámico: cae, colisiona. Stop: vuelve exactamente a su
  posición inicial.
- En Edit Mode, arrastrar el cubo dinámico con el gizmo: ya no pelea con la
  física (bug motivador de esta feature, resuelto).
- Añadir un GameObject durante Play (Basic Shapes), Stop: desaparece.
- Borrar un GameObject durante Play, Stop: reaparece.
- Guardar/Cargar una escena mientras Play Mode está activo, luego Stop: no
  crashea (el restore machaca el Load intermedio con el snapshot de antes de
  Play — comportamiento esperado, edición durante Play se descarta igual).
- Botón cambia de label Play↔Stop con feedback visual (mismo estilo que
  Wireframe activo).

## Riesgos / notas de implementación

- `m_playSnapshot` es un miembro no trivial (`nlohmann::json`) en `EditorUI` — su
  ciclo de vida es simple (se sobreescribe en cada Play, se lee una vez en Stop),
  no hace falta invalidarlo explícitamente entre sesiones.
- Confirmar en implementación que `<nlohmann/json_fwd.hpp>` es suficiente para
  declarar `nlohmann::json toJson() const` en `Scene.h` sin arrastrar el header
  completo (es el patrón documentado de la librería para este caso).
- El botón Play/Stop debe quedar deshabilitado (`ImGui::BeginDisabled`) si
  `m_scene`/`m_physics`/`m_audio`/`m_renderer` es `nullptr`, mismo guard que ya
  usan Save/Load Scene.
