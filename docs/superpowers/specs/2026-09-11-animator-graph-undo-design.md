# Undo del grafo del Animator — diseño

Fecha: 2026-09-11. Origen: fila #1 del backlog de `docs/animation-audit.md`
(hallazgos A2 y D1).

## Problema

`AnimatorPanel` muta el `AnimatorComponent` en ~22 sitios y **ninguno pasa
por el undo**; solo lo hacen las fuentes de animación
(`AnimationSourceCommand`, `AnimatorPanel.cpp:653`, `:804`) y el rename de
clip (`ClipRenameCommand`, `:750`). Borrar un estado, una transición o un
parámetro no se puede deshacer. Incumple la regla del repo "los cambios del
editor pasan por el undo".

Sitios que mutan el grafo (`AnimatorPanel.cpp`): parámetros `:171`, `:185`;
loop `:234`; lock root motion `:240`; blend clip `:258`, `:268` (+ rebind
`:280`); blend param `:296`; blend min/max `:302`, `:305`; crear transición
`:380`; borrar transiciones `:422`; borrar estados `:435`; entrada `:480`;
cross-fade `:506`, `:511`; condiciones `:531`, `:544`, `:549`, `:559`,
`:566`, `:594`, `:609`; añadir estado `:930-948`.

## Decisiones

| Decisión | Elegido | Por qué |
|---|---|---|
| Qué se puede deshacer | Exactamente lo que se **guarda**: lo que emite `animatorToJson` (`Scene.cpp:518-583`), menos `pos` | Un campo nuevo que se añada al `.scene` entra en el undo sin tocar nada más; lo cacheado (`clipIndex`, `duration`, `ticksPerSecond`, `blendClipIndex`, `blendDuration`) y lo de runtime no se guardan y no se deshacen |
| Mover nodos | **Fuera** del undo (decisión del usuario) | `editorPos` se sigue guardando, pero no genera comandos |
| Valores de los parámetros en runtime (`:142-159`, botón Set) | Fuera del undo | No se serializan; no son escena |
| Mecanismo | Diff de snapshots por gesto, dentro del panel | Cubre los 22 sitios y los futuros sin obligaciones por sitio; descartados: captura explícita por sitio (22 obligaciones olvidables) y un comando por operación (reindexado de índices bajo undo, ver `Command.h:653-657`) |
| Formato del snapshot | `AnimatorComponent::Graph` (structs), no JSON | `animatorFromJson` regenera `editorId` (no se serializa, `AnimatorComponent.h:102-108`) y rompería la identidad del canvas y el casado del playhead |

## Diseño

### 1. Core: `AnimatorComponent::Graph` y `applyGraph`

En `include/DonTopo/Core/AnimatorComponent.h`:

```cpp
struct Graph {
    std::vector<State>      states;       // copia tal cual: editorId y editorPos incluidos
    std::vector<Transition> transitions;
    std::vector<Parameter>  parameters;
    int                     entryState = -1;
};
Graph graph() const;
void  applyGraph(const Graph& g);
```

`applyGraph(g)` sustituye estados, transiciones, parámetros y entrada por los
de `g`, respetando los invariantes que ya mantienen `addState`,
`removeState`, `addParameter` y `removeParameter`
(`AnimatorComponent.cpp:8-143`):

- **Playhead**: se casa por `editorId`, no por índice. Si el estado actual
  (por su `editorId`) existe en `g`, `m_currentState` pasa a su índice nuevo y
  conserva `m_animTime` y `m_finished`. Si no existe, cae a la entrada con
  tiempo 0 y `m_finished = false`. El estado que se apaga en un cross-fade se
  casa igual (`m_prevState` por su `editorId`). El fade se conserva si los dos
  estados siguen existiendo y se corta si falta cualquiera de los dos (el
  criterio de `removeState:71-77`, que corta porque no puede casar). Así,
  deshacer un umbral en Play no corta una mezcla en vuelo.
- **Parámetros**: cada parámetro de `g` conserva su valor de runtime si ya
  existía con el **mismo nombre y el mismo tipo**; si no, arranca a su valor por
  defecto (false / 0 / 0.0f, igual que `addParameter`). Las entradas de los
  mapas de los parámetros que desaparecen se borran. **Nunca** pasa por
  `reset()` (cierre de H4: corre en Play).
- **Posiciones**: los estados de `g` cuyo `editorId` ya vive en el componente
  conservan su `editorPos` **actual**; solo los reinsertados toman la de `g`.
  Así, deshacer un "loop" no devuelve a su sitio un nodo que se movió
  después.
- **`m_nextEditorId`** = `max(actual, maxEditorId(g) + 1)`: nunca baja (mismo
  criterio que `addState:19-20`).
- **No resuelve clips.** La caché la rehace el llamante con `rebindClips`.

### 2. Clave de comparación

- `animatorToJson` sale del namespace anónimo de `Scene.cpp` al namespace
  `DonTopo` y se declara en un header nuevo `include/DonTopo/Core/AnimatorSerialization.h`
  (con `nlohmann/json_fwd.hpp`). La definición no se mueve de sitio; sus
  helpers (`paramTypeToStr`, `condTypeToStr`, `compareToStr`) siguen en el
  namespace anónimo.
- En el mismo header: `nlohmann::json animatorGraphKey(const AnimatorComponent&)`,
  que es `animatorToJson` con la clave `pos` borrada de cada estado. Dos grafos
  son iguales para el undo si su clave es igual.

### 3. `AnimatorGraphCommand`

En `include/DonTopo/Editor/Command.h`, junto a `AnimatorComponentCommand`:

```cpp
AnimatorGraphCommand(Scene& scene, std::string label, uint64_t id,
                     AnimatorComponent::Graph before, AnimatorComponent::Graph after);
```

- `execute()` aplica `after` y `undo()` aplica `before`. Cada aplicación busca
  el GameObject por id (`m_scene.findById`, nunca un puntero crudo), llama a
  `getAnimator()->applyGraph(...)` y, si `getSkinnedMesh()` no es nulo, a
  `rebindClips(*mesh, nullptr)`.
- Si no hay GameObject o no tiene Animator, no hace nada (un
  `AnimatorComponentCommand` posterior puede haber quitado el componente).
- No recibe renderer: los hosts leen `pose*()` en cada frame.

### 4. `UndoManager::revision()`

`uint64_t revision() const`, que se incrementa en `push`, en `undo` y `redo`
**cuando hacen algo** y en `clear`. Es la señal de "alguien ha tocado el
historial" y permite al tracker quitarse de en medio sin que nadie tenga que
avisarle.

### 5. `AnimatorGraphUndoTracker`

Header y cpp nuevos en `include/DonTopo/Editor/` y `src/Editor/`, **sin
ImGui**: se prueba sin GUI.

```cpp
void beginFrame(uint64_t goId, const AnimatorComponent* anim, uint64_t undoRevision);
std::unique_ptr<ICommand> endFrame(Scene& scene, const AnimatorComponent* anim,
                                   bool anyItemActive, uint64_t undoRevision);
void setLabel(std::string label);   // opcional; por defecto "Editar Animator"
void discard();
```

- `beginFrame`: si no hay una sesión abierta con ese `goId`, abre una y guarda
  `before = anim->graph()` y su clave. Si hay una abierta con ese mismo id (un
  drag que sigue de un frame anterior), la mantiene. Si `anim` es nulo,
  descarta.
- `endFrame`:
  - Si `anim` es nulo o el id no coincide con el de la sesión: descarta y
    devuelve nulo.
  - Si la revisión es distinta de la de la sesión: toma una **nueva línea
    base** (`before` = el grafo actual, con la revisión nueva) y devuelve nulo.
    Cubre los comandos que el propio panel apila en mitad del draw
    (`ClipRenameCommand` cambia `clipName`), un Ctrl+Z durante un drag y
    `clear()` al entrar o salir de Play.
  - Si la clave actual es igual a la de `before`: cierra la sesión y devuelve
    nulo (esto incluye el drag que acaba donde empezó).
  - Si difiere y `anyItemActive`: mantiene la sesión abierta (el drag sigue) y
    devuelve nulo.
  - Si difiere y no hay nada activo: devuelve
    `AnimatorGraphCommand(label, id, before, graph())`, cierra la sesión y
    restaura el label por defecto.
- **Coste aceptado**: si en el mismo gesto coinciden una edición del grafo y
  un comando ajeno, la edición se queda aplicada pero sin entrada en el undo.
  En la práctica son dos clics en frames distintos.

### 6. Integración en `AnimatorPanel::draw`

Dentro de la rama con GameObject y Animator (`AnimatorPanel.cpp:912-979`):

- `m_graphUndo.beginFrame(go->id, go->getAnimator().get(), ctx.undo->revision())`
  justo después de calcular `selectionChanged` (`:919`), antes de
  `drawAnimationSources`.
- Tras `syncPositionsToComponent` (`:976`):
  `if (auto cmd = m_graphUndo.endFrame(*ctx.scene, anim, ImGui::IsAnyItemActive(), ctx.undo->revision()))`
  → `ctx.pushLog(cmd->label())` y `ctx.undo->push(std::move(cmd))`, **sin
  `execute()`**: el cambio ya está aplicado (contrato de `UndoManager.h:14`).
- En las ramas de panel cerrado, colapsado o sin Animator:
  `m_graphUndo.discard()`.
- **Posiciones tras un undo o redo**: el panel guarda `m_lastUndoRevision`. Si
  `ctx.undo->revision()` ha cambiado desde el frame anterior, llama una vez a
  `syncPositionsFromComponent(go)` (con `ed::SetCurrentEditor`) para colocar
  los nodos reinsertados. Los nodos vivos no saltan, porque
  `syncPositionsToComponent` ya copia su posición al componente en cada frame.
- `setLabel` en borrar estado (`:435`, "Borrar estado"), crear transición
  (`:380`, "Crear transición"), añadir parámetro (`:185`, "Añadir parámetro") y
  quitar parámetro (`:171`, "Quitar parámetro").
- No se tocan: los valores de runtime de los parámetros (`:142-159`) ni los
  tres comandos que ya existen (`:653`, `:750`, `:804`).

## Play Mode

`UndoManager::clear()` al entrar y al salir de Play ya vacía el stack (y ahora
sube la revisión). Dentro de Play, las ediciones se apilan igual; un undo
pasa por `applyGraph`, que conserva los valores de los parámetros y el
playhead (casado por `editorId`).

## Tests

En `engine/tests/animator_tests.cpp`, que ya enlaza el editor. Cada test se
comprueba con **su** sabotaje, de uno en uno (memoria
`sabotage_one_at_a_time`).

1. **`applyGraph`**
   - Conserva el valor de un parámetro con el mismo nombre y tipo, y resetea
     el de uno que cambió de tipo.
   - El playhead sigue al `editorId` cuando el grafo aplicado reinserta un
     estado delante del actual (índice distinto, mismo estado, mismo
     `animTime`).
   - Con el estado actual ausente del grafo, cae a la entrada con tiempo 0.
   - `m_nextEditorId` no baja: un `addState` posterior no repite ningún id.
   - Los estados vivos conservan su `editorPos`; un estado reinsertado toma la
     del snapshot.
   - Aplicar `graph()` sobre sí mismo no cambia nada observable: ni
     parámetros, ni playhead, ni un cross-fade en vuelo.
   - Con un cross-fade en vuelo, un grafo sin el estado que se apaga corta
     la mezcla (`blending()` pasa a false).
2. **Clave**
   - Mover un nodo (`editorPos`) no cambia la clave.
   - Cada campo que emite `animatorToJson` la cambia, por separado: nombre,
     clip, loop, blendClip, blendParam, blendMin, blendMax, lockRootMotion,
     entrada, parámetro (nombre y tipo), transición (from, to, duration) y
     condición (type, param, expected, compare, threshold).
3. **Tracker**
   - Un drag de 3 frames con `anyItemActive` más el frame de soltar da
     **exactamente 1** comando, cuyo `before` es el grafo de antes del primer
     frame.
   - Un drag que acaba en su valor inicial da 0 comandos.
   - Si la revisión cambia entre `begin` y `end`, no hay comando, y el frame
     siguiente sin cambios tampoco da ninguno (la nueva línea base es buena).
   - Un cambio de id entre frames descarta la sesión.
   - Solo mover un nodo da 0 comandos.
4. **Comando**
   - Una ronda `execute`/`undo` deja `animatorGraphKey` idéntico al de antes,
     para cada tipo de mutación del panel (hechas vía API de Core, que es lo
     que ejecuta el panel).
   - Con el Animator quitado del GameObject, `execute` y `undo` no hacen nada
     y no crashean.
   - Con un SkinnedMesh, tras `undo` el `clipIndex` de los estados está
     resuelto (no -1).
5. **`UndoManager::revision`**: sube en `push`, en `undo`/`redo` que hacen algo
   y en `clear`; no sube en un `undo` o `redo` con el stack vacío.

**Verificación manual en la GUI**, en Vulkan **y** en D3D12:

- Borrar un estado con transiciones, Ctrl+Z: vuelve el nodo a su sitio con sus
  transiciones; Ctrl+Y lo vuelve a borrar.
- Arrastrar el umbral de una condición, Ctrl+Z: vuelve al valor de antes del
  drag en **un** solo paso.
- En Play: cambiar un parámetro desde Lua, borrar una transición, Ctrl+Z: el
  parámetro conserva su valor y el estado actual no cambia.

## Fuera de alcance

- Mover nodos en el undo.
- El hallazgo A10 (acceso mutable de la UI a los vectores de estados y
  transiciones): no se cierra con esto; lo que cambia es que todo lo que ese
  acceso escribe queda registrado en el undo. Se corregirá su fila en
  `docs/animation-audit.md`.
- Labels precisos en todos los sitios.
