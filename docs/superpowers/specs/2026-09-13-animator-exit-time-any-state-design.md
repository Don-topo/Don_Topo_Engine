# Exit time y Any State en el Animator — diseño

Fecha: 2026-09-13. Origen: fila 3 del backlog de `docs/animation-audit.md`
(hallazgos A5, C5, C6).

## Problema

- **No se sale de un estado con loop sin script.** `advanceClock` hace `fmod`
  en un estado con loop y nunca pone `finished`, así que `AnimationFinished`
  no dispara jamás ahí; y una transición sin condiciones no dispara nunca
  (`conditionsMet` devuelve false con la lista vacía). Un "idle → saludo cada
  vuelta" o un "ataque → idle al terminar el 90 %" exigen Lua.
- **No hay Any State.** `update` exige `t.fromState == m_currentState`: una
  reacción que vale desde cualquier estado (golpe, muerte) necesita una
  transición copiada en cada estado del grafo.

## Decisiones

| Decisión | Elegido | Por qué / coste descartado |
|---|---|---|
| Modelo de exit time | Como Unity: campos en la transición, tiempo normalizado, `exitTime > 1` cuenta vueltas | Como condición nueva era menos código pero no expresa `2.5` y en loop solo ve la fase de la vuelta |
| Any State hacia el estado actual | Flag por transición `canTransitionToSelf`, **apagado** por defecto | El default de Unity (encendido) reinicia el estado cada frame con un bool y congela al personaje |
| Representación de Any State | Centinela `fromState = kAnyState (-2)` en el mismo `m_transitions` | Un vector aparte duplicaba serialización, dibujo, borrado y reindexado |
| Prioridad | Primero las Any State, después las del estado actual; en cada grupo, orden de declaración; gana la primera | Es lo que hace Unity y lo que espera quien viene de allí |

## Diseño

### 1. Semántica en Core (`AnimatorComponent`)

**Campos nuevos de `Transition`**, al final del struct (la inicialización
agregada de los tests y las escenas viejas no cambian):

```cpp
bool  hasExitTime         = false;
float exitTime            = 1.0f;   // normalizado: 1 = fin del clip
bool  canTransitionToSelf = false;  // solo se lee si fromState == kAnyState
```

**Constante**: `static constexpr int kAnyState = -2;`

**Reloj normalizado acumulado** (runtime, no se serializa). `N` = ticks
avanzados desde que se entró al estado actual, divididos por su `duration`.
No hace `fmod`: en un loop sigue creciendo (1.0 = una vuelta, 2.5 = dos y
media), y en un estado sin loop también crece después del final aunque
`m_animTime` quede clavado en `duration`. Con `duration <= 0` (clip de duración
0 o sin resolver) no se calcula y el exit time cuenta como alcanzado.

**Entrar en un estado** pasa por una única función `enterState(idx)` que fija
`m_currentState` y pone a 0 `m_animTime`, `m_finished` y `N`. Todos los sitios
que hoy reinician el playhead a mano (la transición en `update`,
`resetPlayback`, `removeState` cuando borra el actual, `applyGraph` cuando el
actual ya no existe) llaman a esta función: el reloj nuevo no puede quedar
colgado en un sitio que se olvide de ponerlo a 0.

**Cuándo una transición está lista** (`N0` = valor antes de avanzar el reloj
en este `update`, `N1` = después):

- **Condiciones**:
  - `hasExitTime == false`: hacen falta condiciones y que se cumplan todas
    (comportamiento de hoy; sin condiciones, nunca).
  - `hasExitTime == true`: sin condiciones, basta el tiempo; con condiciones,
    hacen falta el tiempo **y** las condiciones.
- **Tiempo**, con `e = exitTime`:
  - `e < 1`: se comprueba en **cada vuelta**. Está listo si hay un entero
    `k >= 0` con `N0 < k + e <= N1`, es decir, si
    `floor(N1 - e) > floor(N0 - e)`. Cubre el dt que da la vuelta cruzando `e`.
    Caso especial: con `e == 0`, el primer `update` tras entrar en el estado
    (`N0 == 0`) cuenta como cruce. Las condiciones se evalúan en ESE frame; si
    no se cumplen, espera a la vuelta siguiente.
  - `e >= 1`: está listo en cualquier frame con `N1 >= e`.
  - `duration <= 0`: listo siempre.

**Orden de evaluación en `update`** (solo si `evaluateTransitions`):
1. Transiciones con `fromState == kAnyState`, por orden de declaración. Si su
   `toState` es el estado actual y `canTransitionToSelf` está apagado, se salta.
2. Transiciones con `fromState == m_currentState`, por orden de declaración.
La primera lista gana; se consumen sus triggers y se aplica su cross-fade como
hoy. Una transición que dispara en mitad de un cross-fade lo corta como hoy.

**Qué no cambia**: la condición `AnimationFinished`, el consumo de triggers, el
cross-fade y su corte, `evaluateTransitions == false` en Edit.

**`removeState(idx)`**: el código actual ya hace lo correcto con el centinela
(borra las transiciones con `toState == idx`; `fromState == idx` nunca casa con
-2; solo reindexa índices `> idx`). Se deja con test.

**Posición del nodo Any State**: `glm::vec2 anyStateEditorPos` con getter y
setter. Va **fuera** de `Graph`: como las posiciones de los estados, no entra
en el undo.

### 2. Serialización y undo

**`animatorToJson`**:
- Por transición: `"hasExitTime": true` y `"exitTime"` solo si `hasExitTime`;
  `"canTransitionToSelf": true` solo si `fromState == kAnyState` y el flag está
  activo. Any State se escribe como `"from": -2`.
- A nivel de animator: `"anyStatePos": [x, y]`.

**`animatorFromJson`**:
- Lee los tres campos con los defaults del struct (escenas viejas cargan
  igual). `exitTime` va por `readFloat` (no finito: aviso y default).
  `exitTime < 0`: se acota a 0 con aviso.
- La validación de índices (`Scene.cpp:697`) acepta `fromState == kAnyState`;
  `toState` se sigue exigiendo en rango.
- `"anyStatePos"` ausente: posición por defecto.

**Compatibilidad hacia atrás**: un motor anterior que cargue una escena nueva
descarta las transiciones Any State con su aviso de índice fuera de rango, e
ignora los campos de exit time. No se rompe nada; se pierden esas transiciones.

**Undo**: el tracker compara con `animatorToJson`, así que los tres campos
entran solos. `animatorGraphKey` quita también `"anyStatePos"` (mover el nodo
Any State no es una edición). `graphMutations()` en los tests gana cuatro
mutaciones: `hasExitTime`, `exitTime`, `canTransitionToSelf` y una transición
desde Any State.

### 3. UI (`AnimatorPanel`)

- **Ids reservados** `kAnyStateNodeId` y `kAnyStateOutPinId`, fuera del rango
  de estados (`eid*3+1..3`) y de links (`100000+idx`). Los helpers que
  decodifican ids (`stateIndexFromPin`, la creación y el borrado) los
  comprueban **antes** de aplicar `(id-1)/3`.
- **Nodo Any State**: se dibuja si el grafo tiene al menos un estado. Solo pin
  de salida y color de cabecera propio. Su posición se sincroniza con
  `anyStateEditorPos` en `syncPositionsFromComponent`/`ToComponent`; la
  primera vez, a la izquierda del estado de entrada.
- **No se puede borrar**: `QueryDeletedNode` con su id se rechaza. Sus links
  sí se borran.
- **Menú contextual**: sin "Set as Entry" para el nodo Any State.
- **Links**: `fromState == kAnyState` se dibuja del pin de Any State al de
  entrada del destino. Arrastrar desde ese pin crea la transición con
  `fromState = kAnyState` (mismo sitio de creación; label de undo "Crear
  transición").
- **Popup de la transición**, arriba: `[x] Has Exit Time` y, si está activo, un
  `DragFloat` "exit time" con mínimo 0 (efecto en vivo, así que `DragFloat` y no
  `DeferredSliderFloat`). `[x] Can Transition To Self` solo si la transición
  sale de Any State. `PushID` por sección.

### 4. Documentación

- README, sección `## Animator`: exit time, Any State y el flag.
- `docs/animation-audit.md`: A5, C5 y C6 cerradas; C6 pasa de PARCIAL a EXISTE.

## Tests

En `engine/tests/animator_tests.cpp`. Cada test con su sabotaje de uno en uno,
y para cada sabotaje se dice **qué valor observable cambia** (un sabotaje que
no cambia nada observable no demuestra el test).

1. **Exit time < 1 en loop**: con `e = 0.9`, no dispara con `N1 = 0.85` y sí
   en el frame que cruza 0.9; con la condición falsa en ese frame no dispara
   hasta la vuelta siguiente; un dt que pasa de 0.8 a 1.92 dispara.
2. **Exit time ≥ 1**: con `e = 2.5`, no dispara con `N = 2.0` y sí con 2.5.
3. **Sin condiciones**: con exit time dispara; sin exit time, nunca.
4. **Duración 0**: el exit time cuenta como alcanzado en el primer `update`.
5. **Reloj acumulado reiniciado**: tras entrar en un estado por una transición
   (y por `resetPlayback`), un `e = 1.5` espera vuelta y media desde la
   entrada, no desde que arrancó el grafo. Se usa un valor distinto del
   default para que un `N` no reiniciado se note.
6. **Any State**: dispara desde cualquier estado; gana a la transición propia
   del estado cuando las dos están listas; con el flag apagado no reentra al
   estado actual; con el flag encendido y un trigger reentra con
   `animTime == 0`.
7. **`removeState`**: borrar el destino de una Any State la elimina; borrar
   otro estado reindexa su `toState`.
8. **Serialización**: ida y vuelta de `hasExitTime`, `exitTime`,
   `canTransitionToSelf`, `"from": -2` y `anyStatePos`; una escena sin esos
   campos carga igual; Any State con `to` fuera de rango se descarta con aviso;
   `exitTime` negativo se acota con aviso.
9. **Clave del undo**: mover el nodo Any State no cambia la clave; las cuatro
   mutaciones nuevas sí.

**Verificación manual**, en Vulkan y D3D12: crear un link desde Any State,
guardar y cargar; que un exit time dispare donde se espera; Ctrl+Z sobre los
tres campos nuevos; el nodo Any State no se puede borrar.

## Fuera de alcance

- Transition offset, interrupción de transiciones y "ordered interruption" de
  Unity.
- Mostrar el exit time en el dibujo del link.
- Bindings Lua nuevos (no hacen falta: `GetState` ya existe).
