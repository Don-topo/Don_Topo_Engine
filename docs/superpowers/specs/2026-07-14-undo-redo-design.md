# Undo / Redo — Design Spec

## Problema

Hoy toda edición en el editor es directa e irreversible: `EditorUI` muta
`GameObject`/`Scene` en el sitio (DragFloat sobre `localTransform`, callbacks
de Add/Delete/Reparent) sin ningún registro de lo que cambió. Un error de
usuario (borrar el objeto equivocado, mover un collider por accidente,
reordenar mal la jerarquía) no tiene forma de deshacerse salvo recargar la
escena desde disco (perdiendo cualquier cambio no guardado).

Confirmado por grep sobre `engine/`: no existe ningún `Command`, `Undo`,
`Redo` ni `History` en el código (los únicos matches de "Command" son
`VkCommandBuffer`, ajeno a este dominio).

## Objetivo

`Ctrl+Z` / `Ctrl+Y` en el editor deshacen/rehacen hasta las últimas 50
acciones de:

1. Edición de Transform (Position/Rotation/Scale) desde el panel Properties.
2. Edición de propiedades de componente (campos de BoxCollider,
   SphereCollider, CapsuleCollider, PlaneCollider) desde el panel Properties.
3. Crear / borrar GameObject (Basic Shapes, Delete de Scene panel).
4. Reparent / reorder por drag&drop en el Scene panel.

**Fuera de scope de esta iteración:**

- **Add/Remove Component** (Box/Sphere/Capsule/Plane Collider, Mesh, Audio
  Clip, Script). Cada uno tiene su propio ciclo de vida de recursos externos
  disperso por tipo (`drawAddComponentButton`, `loadMeshForSelected`,
  `loadAudioClipForSelected` — actor PhysX, mesh GPU, clip FMOD, cada uno con
  su propia factory/liberación). Unificar eso en comandos reversibles es una
  tarea con superficie propia; se deja pa iteración futura.
- Edición de gizmo del Viewport (mover con ImGuizmo) — mismo mecanismo que
  Transform del panel Properties (`PropertyCommand<glm::mat4>`), pero el
  punto de enganche exacto (`ImGuizmo::IsUsing()`) se deja pa la fase de
  implementación; no cambia el diseño aquí descrito.
- Persistencia del historial entre sesiones o a través de Save/Load — el
  stack se resetea (ver sección 8).
- Undo/Redo durante Play Mode — deshabilitado mientras `m_isPlaying` es true.

## Diseño

### 1. IDs estables — `GameObject`

Los comandos deben sobrevivir a un ciclo undo→redo de Delete, donde el
`GameObject` original se destruye y se reconstruye desde JSON (mismo patrón
que `Scene::cloneGameObject`, vía `nodeFromJson`) — el puntero cambia. Ningún
comando puede guardar un `GameObject*` crudo como referencia de largo plazo.

Se añade un id estable:

```cpp
// GameObject.h
public:
    uint64_t id;   // asignado en el constructor, único pa toda la sesión

// GameObject.cpp
namespace { std::atomic<uint64_t> s_nextId{1}; }
GameObject::GameObject(std::string name) : name(std::move(name)), id(s_nextId++) {}
```

`Scene` gana un lookup:

```cpp
// Scene.h
GameObject* findById(uint64_t id);
```

`nodeToJson`/`nodeFromJson` (`Scene.cpp`) ganan el campo `"id"`: se escribe
siempre; al leer, si el campo existe se reusa (necesario pa que Undo de
Delete reconstruya el mismo id — así los comandos que le siguen en el stack
siguen resolviendo el objeto correcto), si no existe (escenas guardadas antes
de este cambio) se deja el id nuevo que ya asignó el constructor. Cambio
aditivo, no rompe compatibilidad de ficheros `.scene` existentes.

### 2. `ICommand` + `PropertyCommand<T>` — `engine/include/DonTopo/Command.h`

```cpp
class ICommand {
public:
    virtual ~ICommand() = default;
    virtual void execute() = 0;   // aplica "after" (redo)
    virtual void undo() = 0;      // aplica "before"
    virtual std::string label() const = 0;   // pa Log Console
};

// Comando genérico pa cualquier propiedad value-type de un GameObject o de
// uno de sus componentes. apply() resuelve el objeto en vivo cada vez que se
// llama (nunca captura un puntero crudo) — sobrevive a que el GameObject se
// haya reconstruido entretanto por un Undo de Delete.
template <typename T>
class PropertyCommand : public ICommand {
public:
    PropertyCommand(std::string label, T before, T after,
                     std::function<void(const T&)> apply)
        : m_label(std::move(label)), m_before(std::move(before)),
          m_after(std::move(after)), m_apply(std::move(apply)) {}

    void execute() override { m_apply(m_after); }
    void undo()    override { m_apply(m_before); }
    std::string label() const override { return m_label; }

private:
    std::string m_label;
    T m_before, m_after;
    std::function<void(const T&)> m_apply;
};
```

Un único template cubre Transform (`T = glm::mat4`) y cada tipo de collider
(`T` = un struct pequeño POD por tipo, ej. `BoxColliderState{center, size,
useGravity}`) sin necesitar una clase de comando por widget. `apply` es una
lambda que resuelve `scene->findById(id)` y escribe el campo correspondiente.

### 3. `ReparentCommand` / `CreateGameObjectCommand` / `DeleteGameObjectCommand` — mismo header

Ninguno de los tres es template — declarados en `Command.h`, implementados en
`Command.cpp` (necesitan `Scene`, `PhysicsManager`, `AudioManager`).

```cpp
class ReparentCommand : public ICommand {
public:
    ReparentCommand(Scene& scene, std::string label, uint64_t id,
                     uint64_t oldParentId, size_t oldIndex,
                     uint64_t newParentId, size_t newIndex);
    void execute() override;   // mueve a (newParentId, newIndex)
    void undo() override;      // mueve a (oldParentId, oldIndex)
    std::string label() const override { return m_label; }
private:
    void moveTo(uint64_t parentId, size_t index);
    Scene& m_scene;
    std::string m_label;
    uint64_t m_id, m_oldParentId, m_newParentId;
    size_t m_oldIndex, m_newIndex;
};
```

`CreateGameObjectCommand` y `DeleteGameObjectCommand` comparten la misma
mecánica invertida: guardan un snapshot JSON del subárbol completo (mismo
formato que `Scene::toJson`, reusando `nodeToJson`/`nodeFromJson`), más
`parentId` e `index` (posición dentro de `children` del padre) pa reinsertar
en el sitio exacto.

```cpp
class DeleteGameObjectCommand : public ICommand {
public:
    // snapshot = subárbol ya serializado ANTES de borrarlo
    DeleteGameObjectCommand(Scene& scene, PhysicsManager& physics, AudioManager& audio,
                             Renderer& renderer, std::string label,
                             uint64_t parentId, size_t index, nlohmann::json snapshot);
    void execute() override;   // borra (redo del delete)
    void undo() override;      // reinserta desde snapshot + re-registra GPU
    std::string label() const override { return m_label; }
private:
    Scene& m_scene; PhysicsManager& m_physics; AudioManager& m_audio; Renderer& m_renderer;
    std::string m_label;
    uint64_t m_parentId; size_t m_index;
    nlohmann::json m_snapshot;
};
// CreateGameObjectCommand: idéntica forma, execute()/undo() invertidos.
```

`Scene` gana dos wrappers públicos delgados (extracción de lo que ya hacen
`toJson`/`fromJson` internamente por nodo, sin cambiar su comportamiento):

```cpp
nlohmann::json subtreeToJson(const GameObject* node) const;
GameObject* insertFromJson(const nlohmann::json& j, GameObject* parent, size_t index,
                            PhysicsManager& physics, AudioManager& audio);
```

**GPU**: `Delete` ya notifica `m_onDelete` antes de desenganchar (libera
mesh/textura en GPU). El `undo()` de `DeleteGameObjectCommand` (o el
`execute()` de `CreateGameObjectCommand`) debe re-registrar GPU pa el
subárbol restaurado — se extrae a un helper el bucle de re-registro
(estático + skinned) que hoy usa `reloadSceneFromJson` pa la escena entera, y
se parametriza por nodo raíz en vez de por escena completa.

### 4. `UndoManager` — `engine/include/DonTopo/UndoManager.h`

```cpp
class UndoManager {
public:
    static constexpr size_t kMaxHistory = 50;

    // Registra cmd como ya-aplicado — el caller ejecuta la acción real
    // ANTES de llamar push(); push() nunca llama execute(). Vacía el redo stack.
    void push(std::unique_ptr<ICommand> cmd);
    void undo();   // no-op si vacío
    void redo();   // no-op si vacío
    void clear();  // Load Scene, entrar/salir de Play Mode
    bool canUndo() const { return !m_undoStack.empty(); }
    bool canRedo() const { return !m_redoStack.empty(); }
    // último label aplicado/deshecho, pa Log Console — ver sección 9
private:
    std::deque<std::unique_ptr<ICommand>> m_undoStack;
    std::deque<std::unique_ptr<ICommand>> m_redoStack;
};
```

`push` descarta el más antiguo (`pop_front`) si se supera `kMaxHistory`.

Vive como miembro de `EditorUI` (`UndoManager m_undoHistory;`), igual que
`m_playSnapshot` — es quien orquesta todas las interacciones de UI que
generan comandos, con acceso directo a `Scene*`/`PhysicsManager*`/
`AudioManager*`/`Renderer*` que los comandos necesitan.

### 5. Puntos de captura en `EditorUI`

**Transform** (`EditorUI.cpp:1028-1122`): hoy `m_transformDragActive` solo
cubre Position/Rotation (`posRotActive`), no Scale. Se extiende pa cubrir los
9 DragFloat. Se añade `glm::mat4 m_transformBeforeEdit`: en el flanco
inactivo→activo (inicio de cualquier drag del grupo Transform) se captura
`m_transformBeforeEdit = m_selected->localTransform`; en el flanco donde
`posCommitted || rotCommitted || scaleCommitted` es true, se hace push de
`PropertyCommand<glm::mat4>("Transform de '<name>'", m_transformBeforeEdit,
m_selected->localTransform, apply)` — `apply` resuelve `scene->findById(id)`
y reasigna `localTransform` + repite el `teleport()` del collider si aplica
(mismo bloque que ya existe en `changed`, `EditorUI.cpp:1104-1121`).

**Colliders** (`drawBoxColliderSection` y análogos): mismo patrón de cache
documentado en `EditorUI.h:281-310` (`m_colliderDragActive` etc.) — se
extiende igual: snapshot en el flanco activo, push en el flanco de commit,
con un struct POD pequeño por tipo de collider como `T` de
`PropertyCommand<T>`.

**Reparent** (resolución de `m_pendingMoveSource/Target`,
`EditorUI.cpp:502-506`): antes de llamar `moveGameObject(...)`, capturar
`oldParentId = dragged->parent->id` y `oldIndex` (posición de `dragged` en
`children` del padre actual). Tras el move, `newParentId`/`newIndex` son los
del padre destino. Push de `ReparentCommand` ya-aplicado.

**Create** (`createBasicShape`, `EditorUI.cpp:132`): tras crear el
GameObject y registrarlo en GPU, `snapshot = m_scene->subtreeToJson(node)`,
push de `CreateGameObjectCommand` ya-aplicado con `parentId`/`index` de
inserción.

**Delete** (resolución de `m_pendingDelete`, `EditorUI.cpp:457-460`): antes
de `Scene::removeGameObject`, capturar `parentId`, `index` y
`snapshot = m_scene->subtreeToJson(node)`. Tras el borrado, push de
`DeleteGameObjectCommand` ya-aplicado (el `m_onDelete` de liberación GPU ya
se dispara antes, sin cambios ahí).

### 6. Atajos de teclado

Dentro de `EditorUI::draw()`, no en el callback GLFW global de `main.cpp`
(donde vive el atajo `F` hoy) — así respeta el foco por panel, igual que
`ScriptEditorPanel` con su propio `Ctrl+S`, y no choca con el undo nativo de
un `ImGuiInputTextMultiline` (el Script Editor tiene su propio undo de texto):

```cpp
if (ImGui::GetIO().KeyCtrl && !ImGui::GetIO().WantTextInput && !m_isPlaying)
{
    if (ImGui::IsKeyPressed(ImGuiKey_Z)) {
        uint64_t prevSelId = m_selected ? m_selected->id : 0;
        m_undoHistory.undo();
        m_selected = prevSelId ? m_scene->findById(prevSelId) : nullptr;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Y)) {
        uint64_t prevSelId = m_selected ? m_selected->id : 0;
        m_undoHistory.redo();
        m_selected = prevSelId ? m_scene->findById(prevSelId) : nullptr;
    }
}
```

Re-resolver `m_selected` por id tras cada undo/redo es necesario solo pa el
caso donde el comando afectado sea estructural (Create/Delete/Reparent) y
haya reconstruido el objeto seleccionado; pa `PropertyCommand` no cambia
nada (mismo puntero), la operación es barata (`findById` sobre árboles
pequeños) y uniforme pa los 4 tipos de comando sin necesitar que cada uno
sepa si afecta o no a la selección.

### 7. Ciclo de vida del stack

`m_undoHistory.clear()` se llama en:

- `reloadSceneFromJson` tras éxito (cubre tanto Load Scene como el restore
  de Stop en Play Mode — ambos reconstruyen el árbol entero, cualquier
  comando viejo referenciaría ids que ya no existen de forma coherente).
- Al pulsar Play (antes de tomar `m_playSnapshot`) — evita que un Undo
  durante Play revierta un estado que Stop va a descartar igual, y evita
  interacción entre física/scripts en vivo y el stack.

Ctrl+Z/Ctrl+Y quedan además deshabilitados mientras `m_isPlaying` (sección
6) — no solo se resetea el stack, no se puede invocar mientras Play está
activo.

### 8. Log Console

Cada `undo()`/`redo()` exitoso llama `pushLog("Undo: " + label)` /
`pushLog("Redo: " + label)` (mismo mecanismo que ya usan
`posCommitted`/`rotCommitted`/etc., `EditorUI.cpp:1090-1095`) — feedback
visible sin infraestructura nueva.

## Testing

Manual, sin framework — igual que el resto del motor:

- Mover un cubo con Position del panel Properties, Ctrl+Z: vuelve a la
  posición anterior exacta. Ctrl+Y: vuelve a la posición nueva.
- Cambiar tamaño de un BoxCollider, Ctrl+Z: tamaño y gravedad vuelven al
  valor anterior (collider PhysX incluido, no solo el valor visual).
- Crear un Cube (Basic Shapes), Ctrl+Z: desaparece de la escena y del
  Viewport (GPU liberada). Ctrl+Y: reaparece en la misma posición de
  jerarquía.
- Borrar un GameObject con hijos y collider, Ctrl+Z: todo el subárbol
  reaparece con el mismo id, mismo padre, mismo índice; colliders vuelven a
  simular física con normalidad.
- Reordenar por drag&drop en Scene panel, Ctrl+Z: vuelve a la posición
  original en la jerarquía.
- Encadenar >50 acciones: las primeras se descartan del stack (no crashea al
  hacer Ctrl+Z de más, simplemente deja de tener efecto).
- Ctrl+Z dentro del Script Editor con foco en el textbox: no afecta al
  historial de escena (usa el undo nativo del widget de texto).
- Entrar en Play, hacer cambios, Ctrl+Z (no debe hacer nada), Stop: stack
  vacío, ningún Undo disponible de acciones previas a Play.

## Riesgos / notas de implementación

- `findById` es O(n) sobre el árbol — aceptable pa el tamaño de escena
  actual del motor; si se convierte en problema de rendimiento con escenas
  grandes, cachear un `unordered_map<uint64_t, GameObject*>` en `Scene` es
  la extensión natural, pero no hace falta pa esta iteración (YAGNI).
- Confirmar en implementación que extraer el bucle de re-registro GPU de
  `reloadSceneFromJson` a un helper parametrizado por nodo raíz no cambia su
  comportamiento pa el caso ya existente (escena completa == nodo raíz =
  `m_root`).
- `PropertyCommand<T>::apply` debe capturar `Scene*` (no referencia) si
  `EditorUI` ya maneja `Scene*` posiblemente nulo en otros sitios — revisar
  que el guard exista antes de construir el comando (mismo patrón que ya usan
  `drawBoxColliderSection` etc. al comprobar `m_scene` antes de tocarlo).
- Los 4 structs POD de estado de collider (`BoxColliderState`,
  `SphereColliderState`, `CapsuleColliderState`, `PlaneColliderState`) son
  triviales de definir a partir de los campos que cada `draw*ColliderSection`
  ya cachea (`EditorUI.h:281-310`) — no requieren diseño adicional.
