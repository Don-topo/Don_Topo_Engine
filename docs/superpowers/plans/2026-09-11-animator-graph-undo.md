# Undo del grafo del Animator — plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Que toda edición del grafo hecha en el `AnimatorPanel` (estados, transiciones, condiciones, parámetros, blend, entrada) se pueda deshacer con Ctrl+Z, un comando por gesto.

**Architecture:** Diff de snapshots por gesto. `AnimatorComponent` gana un `Graph` (lo autorado) y `applyGraph`, que lo sustituye conservando el runtime. Un `AnimatorGraphUndoTracker` sin ImGui compara el grafo al principio y al final del draw del panel (con la clave `animatorGraphKey` = el JSON que se guarda, sin posiciones) y, cuando no hay ningún widget activo, devuelve un `AnimatorGraphCommand(before, after)`. `UndoManager::revision()` le dice al tracker cuándo otro ha tocado el historial.

**Tech Stack:** C++20, MSVC + Ninja, nlohmann::json, Dear ImGui + imgui-node-editor. Tests: ejecutables `dt_*.exe` con `CHECK` propio, sin framework.

**Spec:** `docs/superpowers/specs/2026-09-11-animator-graph-undo-design.md` (commit `7139927`). Léela antes de empezar: este plan argumenta desde ella.

## Global Constraints

- **Build**: SIEMPRE con la herramienta **PowerShell**, desde la raíz del repo: `& .\build.bat` (Debug, sale en `build-ninja/`). Nunca `cmake`/`ninja` a pelo desde Bash: sin `vcvarsall` no hay compilador. Añadir un `.cpp` a `engine/CMakeLists.txt` lo recoge el propio `build.bat` (reconfigura solo).
- **Tests**: con la herramienta **Bash**, desde la **raíz del repo** (`cd /c/Users/ruben/Documents/Don_Topo_Engine`). Desde otro directorio `dt_animator_tests` revienta con exit 139 **sin imprimir nada**; no es un crash tuyo.
  - `./build-ninja/engine/tests/dt_animator_tests.exe`
  - `./build-ninja/engine/tests/dt_render_settings_undo_tests.exe`
- **Estado de partida**: 28/28 ejecutables de test en verde (Debug y Release).
- **Edición de ficheros**: solo con las herramientas Edit/Write. Nada de `sed -i`, `python -c` con código, ni `Get-Content`/`Set-Content`: convierten CRLF a LF o destrozan los acentos UTF-8.
- **Finales de línea**: el repo va en CRLF. Antes de cada commit, `git diff --stat` y `git diff --ignore-cr-at-eol --stat` tienen que dar **las mismas cifras**. Si difieren, has convertido un fichero entero: arréglalo antes de hacer commit.
- **Commits**: mensaje escrito con Write en un fichero del scratchpad y `git commit -F <fichero>` desde Bash (las comillas dentro de `-m` en PowerShell parten los argumentos). Terminar el mensaje con la línea `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`. No usar `--no-verify`.
- **Sabotaje de uno en uno**: cada test nuevo se demuestra rompiendo SU guarda, compilando, viendo caer ESE test y restaurando. Nunca varios sabotajes a la vez.
- **Estilo**: comentarios en español y con la densidad del fichero que tocas; los ficheros de `Core/` indentan dentro de `namespace DonTopo`, los de `Editor/` no.
- **Alcance**: no tocar renderer, física, audio, UI de juego ni la serialización de otros componentes. No hay código de backend (Vulkan/D3D12) en este plan: la paridad se verifica a mano en la tarea 7.
- `imgui.ini` va siempre en el commit si sale modificado tras abrir el editor; nunca se descarta.

## Mapa de ficheros

| Fichero | Qué cambia |
|---|---|
| `engine/include/DonTopo/Editor/UndoManager.h`, `engine/src/Editor/UndoManager.cpp` | `revision()` |
| `engine/include/DonTopo/Core/AnimatorComponent.h`, `engine/src/Core/AnimatorComponent.cpp` | `Graph`, `graph()`, `applyGraph()` |
| `engine/include/DonTopo/Core/AnimatorSerialization.h` (nuevo) | declara `animatorToJson` y `animatorGraphKey` |
| `engine/src/Core/Scene.cpp` | `animatorToJson` sale del namespace anónimo; define `animatorGraphKey` |
| `engine/include/DonTopo/Editor/Command.h`, `engine/src/Editor/Command.cpp` | `AnimatorGraphCommand` |
| `engine/include/DonTopo/Editor/AnimatorGraphUndo.h`, `engine/src/Editor/AnimatorGraphUndo.cpp` (nuevos) | `AnimatorGraphUndoTracker` |
| `engine/CMakeLists.txt` | alta de `src/Editor/AnimatorGraphUndo.cpp` |
| `engine/include/DonTopo/Editor/AnimatorPanel.h`, `engine/src/Editor/AnimatorPanel.cpp` | integración |
| `engine/tests/render_settings_undo_tests.cpp` | tests de `revision()` |
| `engine/tests/animator_tests.cpp` | tests de `applyGraph`, clave, comando y tracker |
| `README.md` | una frase en `## Animator` |

Dependencias entre tareas: 1, 2 y 3 son independientes entre sí (la 3 usa `applyGraph` de la 2 en dos mutaciones de test, así que va después de la 2); la 4 necesita la 2 y la 3; la 5 necesita la 1, la 3 y la 4; la 6 necesita la 5.

---

### Task 1: `UndoManager::revision()`

**Files:**
- Modify: `engine/include/DonTopo/Editor/UndoManager.h` (públicos tras `canRedo()`, `:46`; privados tras `m_sceneDirty`, `:56`)
- Modify: `engine/src/Editor/UndoManager.cpp:5-42`
- Test: `engine/tests/render_settings_undo_tests.cpp` (función nueva antes de `int main()` en `:278`, y su llamada dentro de `main`)

**Interfaces:**
- Produces: `uint64_t UndoManager::revision() const` — cambia en cada `push`, en `undo`/`redo` que hacen algo, y en `clear`. No cambia en `undo`/`redo` con el stack vacío.

- [ ] **Step 1: Write the failing test**

En `engine/tests/render_settings_undo_tests.cpp`, antes de `int main()`. `makeSceneCommand(int& target, int before, int after)` ya existe en ese fichero (`:30`).

```cpp
// revision() es la señal con la que AnimatorGraphUndoTracker sabe que alguien
// ha tocado el historial en mitad de un gesto. Tiene que cambiar con TODO lo
// que mueve los stacks y con nada más: si no cambiara en un undo, el tracker
// metería lo deshecho dentro del comando del gesto; si cambiara en un undo
// vacío, descartaría gestos del usuario sin motivo.
static void test_revision_cambia_con_cada_movimiento_del_historial()
{
    UndoManager undo;
    int target = 0;

    const uint64_t r0 = undo.revision();
    undo.undo();   // stacks vacíos: no hacen nada
    undo.redo();
    CHECK(undo.revision() == r0);

    undo.push(makeSceneCommand(target, 0, 1));
    const uint64_t r1 = undo.revision();
    CHECK(r1 != r0);

    undo.undo();
    const uint64_t r2 = undo.revision();
    CHECK(r2 != r1);

    undo.redo();
    const uint64_t r3 = undo.revision();
    CHECK(r3 != r2);

    undo.clear();
    CHECK(undo.revision() != r3);
}
```

Y en `main()`, junto a las demás llamadas:

```cpp
    test_revision_cambia_con_cada_movimiento_del_historial();
```

- [ ] **Step 2: Run it to verify it fails**

PowerShell: `& .\build.bat`
Expected: error de compilación `'revision': is not a member of 'DonTopo::UndoManager'`.

- [ ] **Step 3: Implement**

`UndoManager.h`, después de `bool canRedo() const { ... }`:

```cpp
    // Cambia cada vez que se mueve el historial: push, undo y redo que hacen
    // algo, y clear. AnimatorGraphUndoTracker la compara entre el principio y
    // el final de un gesto para saber si la diferencia que ve en el grafo es
    // solo del usuario o también de un comando ajeno (un Ctrl+Z en mitad de un
    // drag, un ClipRenameCommand del propio panel, el clear() de Play).
    uint64_t revision() const { return m_revision; }
```

y en la parte privada, después de `bool m_sceneDirty = false;`:

```cpp
    uint64_t m_revision = 0;
```

`UndoManager.h` necesita `<cstdint>`: si no lo incluye ya, añadir `#include <cstdint>` a sus includes.

`UndoManager.cpp`:
- `push`: `++m_revision;` como última línea.
- `undo`: `++m_revision;` como última línea (después del `return` de stack vacío, así que solo cuenta si hizo algo).
- `redo`: igual que `undo`.
- `clear`: `++m_revision;` como última línea.

- [ ] **Step 4: Run tests to verify they pass**

PowerShell: `& .\build.bat`
Bash: `./build-ninja/engine/tests/dt_render_settings_undo_tests.exe`
Expected: sin líneas `FAIL`, exit 0.

- [ ] **Step 5: Sabotage, one at a time**

Para cada uno: aplicar, `& .\build.bat`, ejecutar, ver caer el `CHECK` indicado, restaurar.
1. Quitar `++m_revision;` de `undo` → cae `CHECK(r2 != r1)`.
2. Mover el `++m_revision;` de `undo` antes del `return` de stack vacío → cae `CHECK(undo.revision() == r0)`.
3. Quitar `++m_revision;` de `clear` → cae el último `CHECK`.

Tras restaurar, volver a ejecutar: sin fallos.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Editor/UndoManager.h engine/src/Editor/UndoManager.cpp engine/tests/render_settings_undo_tests.cpp
git commit -F <scratchpad>/msg.txt
```
Mensaje: `feat(undo): UndoManager::revision() para detectar historial movido`, con una línea de cuerpo diciendo para qué la usará el tracker del Animator, y la línea Co-Authored-By.

---

### Task 2: `AnimatorComponent::Graph` y `applyGraph`

**Files:**
- Modify: `engine/include/DonTopo/Core/AnimatorComponent.h` (tras `struct Parameter`, `:122-126`)
- Modify: `engine/src/Core/AnimatorComponent.cpp` (funciones nuevas tras `removeParameter`, `:143`)
- Test: `engine/tests/animator_tests.cpp` (funciones nuevas antes de `int main()`, `:4183`; llamadas al final de la lista de `main`, antes de `am.shutdown();`)

**Interfaces:**
- Produces:
  - `struct AnimatorComponent::Graph { std::vector<State> states; std::vector<Transition> transitions; std::vector<Parameter> parameters; int entryState = -1; };`
  - `AnimatorComponent::Graph AnimatorComponent::graph() const;`
  - `void AnimatorComponent::applyGraph(const Graph& g);`

- [ ] **Step 1: Write the failing tests**

En `animator_tests.cpp`, antes de `int main()`:

```cpp
// ---- applyGraph: el undo del grafo sustituye lo autorado sin tocar el runtime ----

// Estado mínimo con clip de 100 ticks a 10 ticks/s: 1 s de update = 10 ticks.
static AnimatorComponent::State makeTimedState(const char* name)
{
    AnimatorComponent::State s;
    s.name = name; s.clipName = name;
    s.duration = 100.0f; s.ticksPerSecond = 10.0f;
    return s;
}

// Los valores que el script venía escribiendo sobreviven a un undo si el
// parámetro sigue siendo el mismo (nombre Y tipo). Uno que cambió de tipo
// vuelve a su valor por defecto: el valor viejo no significa nada en el tipo
// nuevo.
static void test_apply_graph_keeps_param_values_of_same_name_and_type()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    a.addParameter("speed", AnimatorComponent::ParamType::Float);
    a.addParameter("jump",  AnimatorComponent::ParamType::Bool);
    a.setFloat("speed", 3.5f);
    a.setBool("jump", true);

    AnimatorComponent::Graph g = a.graph();
    for (auto& p : g.parameters)
        if (p.name == "jump") p.type = AnimatorComponent::ParamType::Int;
    a.applyGraph(g);

    CHECK(nearlyEqual(a.getFloat("speed"), 3.5f));
    CHECK(a.getInt("jump") == 0);
    CHECK(!a.getBool("jump"));
}

// Un undo que reinserta un estado DELANTE del actual cambia el índice del
// actual. El playhead se casa por editorId: sigue en el mismo estado, con su
// tiempo.
static void test_apply_graph_playhead_follows_editor_id()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    a.addState(makeTimedState("B"));
    a.setEntryState(1);
    a.update(1.0f, false);   // B, 10 ticks
    CHECK(a.currentStateName() == "B");

    AnimatorComponent::Graph g = a.graph();
    AnimatorComponent::State c = makeTimedState("C");
    c.editorId = 99;
    g.states.insert(g.states.begin(), c);
    g.entryState = 2;
    a.applyGraph(g);

    CHECK(a.currentState() == 2);
    CHECK(a.currentStateName() == "B");
    CHECK(nearlyEqual(a.animTime(), 10.0f));
}

// Si el grafo aplicado ya no tiene el estado actual, no hay nada que casar:
// cae a la entrada con tiempo 0.
static void test_apply_graph_missing_current_state_falls_to_entry()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    a.addState(makeTimedState("B"));
    a.setEntryState(1);
    a.update(1.0f, false);

    AnimatorComponent::Graph g = a.graph();
    g.states.erase(g.states.begin() + 1);
    g.entryState = 0;
    a.applyGraph(g);

    CHECK(a.currentState() == 0);
    CHECK(nearlyEqual(a.animTime(), 0.0f));
}

// Deshacer el alta de B deja el grafo con solo A, pero el id de B no se puede
// volver a repartir: un redo lo traerá de vuelta y dos nodos con el mismo id
// comparten slot visual en imgui-node-editor.
static void test_apply_graph_never_lowers_next_editor_id()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    const AnimatorComponent::Graph soloA = a.graph();
    const int idB = a.states()[a.addState(makeTimedState("B"))].editorId;

    a.applyGraph(soloA);
    const int idC = a.states()[a.addState(makeTimedState("C"))].editorId;

    CHECK(idC != idB);
    CHECK(idC != a.states()[0].editorId);
}

// Mover nodos no entra en el undo: al deshacer otra cosa, un nodo vivo se queda
// donde está AHORA. Solo el que vuelve de un borrado toma la posición del
// snapshot.
static void test_apply_graph_live_states_keep_position()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    AnimatorComponent::State b = makeTimedState("B");
    b.editorPos = glm::vec2(7.0f, 8.0f);
    a.addState(b);
    const AnimatorComponent::Graph snapshot = a.graph();

    a.statesMutable()[0].editorPos = glm::vec2(50.0f, 60.0f);   // el usuario mueve A
    a.removeState(1);                                            // y borra B
    a.applyGraph(snapshot);                                      // undo del borrado

    CHECK(a.states().size() == 2);
    CHECK(a.states()[0].editorPos == glm::vec2(50.0f, 60.0f));
    CHECK(a.states()[1].editorPos == glm::vec2(7.0f, 8.0f));
}

// A -> B por trigger con cross-fade de 1 s, ya disparada: deja una mezcla en
// vuelo con A apagándose.
static void makeCrossfadeInFlight(AnimatorComponent& a)
{
    a.addState(makeTimedState("A"));
    a.addState(makeTimedState("B"));
    a.addParameter("t", AnimatorComponent::ParamType::Trigger);
    a.addParameter("v", AnimatorComponent::ParamType::Float);
    AnimatorComponent::Transition tr;
    tr.fromState = 0; tr.toState = 1; tr.duration = 1.0f;
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = "t";
    tr.conditions.push_back(c);
    a.addTransition(tr);

    a.setFloat("v", 2.0f);
    a.update(0.1f, true);
    a.setTrigger("t");
    a.update(0.1f, true);
}

// Aplicar el propio grafo no puede cambiar nada que se vea: ni parámetros, ni
// playhead, ni la mezcla en vuelo. Es el caso de deshacer, en Play, una
// edición que no toca a los estados que se están mezclando.
static void test_apply_own_graph_changes_nothing()
{
    AnimatorComponent a;
    makeCrossfadeInFlight(a);
    CHECK(a.blending());

    const int   cur = a.currentState(),  prev = a.previousState();
    const float t   = a.animTime(),      pt   = a.previousAnimTime();
    const float w   = a.blendWeight();

    a.applyGraph(a.graph());

    CHECK(a.blending());
    CHECK(a.currentState() == cur);
    CHECK(a.previousState() == prev);
    CHECK(nearlyEqual(a.animTime(), t));
    CHECK(nearlyEqual(a.previousAnimTime(), pt));
    CHECK(nearlyEqual(a.blendWeight(), w));
    CHECK(nearlyEqual(a.getFloat("v"), 2.0f));
}

// Sin el estado que se apaga no hay contra qué mezclar: la mezcla se corta.
static void test_apply_graph_without_fading_state_cuts_crossfade()
{
    AnimatorComponent a;
    makeCrossfadeInFlight(a);
    CHECK(a.blending());

    AnimatorComponent::Graph g = a.graph();
    g.states.erase(g.states.begin());   // A, el que se apagaba
    g.transitions.clear();              // apuntaban a A
    g.entryState = 0;
    a.applyGraph(g);

    CHECK(!a.blending());
    CHECK(a.currentStateName() == "B");
}
```

Y en `main()`, antes de `am.shutdown();`:

```cpp
    test_apply_graph_keeps_param_values_of_same_name_and_type();
    test_apply_graph_playhead_follows_editor_id();
    test_apply_graph_missing_current_state_falls_to_entry();
    test_apply_graph_never_lowers_next_editor_id();
    test_apply_graph_live_states_keep_position();
    test_apply_own_graph_changes_nothing();
    test_apply_graph_without_fading_state_cuts_crossfade();
```

- [ ] **Step 2: Run to verify it fails**

PowerShell: `& .\build.bat`
Expected: error de compilación, `'Graph': is not a member of 'DonTopo::AnimatorComponent'`.

- [ ] **Step 3: Implement**

`AnimatorComponent.h`, justo después de `struct Parameter { ... };`:

```cpp
            // Lo AUTORADO del grafo, sin nada de runtime: lo que guarda y
            // restaura el undo del editor (AnimatorGraphCommand). Los estados
            // van enteros —editorId y editorPos incluidos— porque applyGraph
            // necesita el editorId para casar los estados vivos con los del
            // snapshot, y la posición para colocar un nodo que vuelve de un
            // borrado.
            struct Graph
            {
                std::vector<State>      states;
                std::vector<Transition> transitions;
                std::vector<Parameter>  parameters;
                int                     entryState = -1;
            };
```

y en la sección `// --- Diseño (editor / carga de escena) ---`, después de `void removeParameter(...)`:

```cpp
            Graph graph() const;
            // Sustituye estados, transiciones, parámetros y entrada por los de
            // g SIN pasar por reset(): corre en Play (undo a mitad de partida).
            //  - El playhead se casa por editorId, no por índice: si el estado
            //    actual sigue en g conserva su tiempo aunque cambie de índice;
            //    si no, cae a la entrada con tiempo 0. El que se apaga en un
            //    cross-fade se casa igual, y la mezcla solo se corta si falta
            //    alguno de los dos.
            //  - Un parámetro conserva su valor si ya existía con el mismo
            //    nombre Y el mismo tipo; si no, arranca a su valor por defecto.
            //  - Los estados vivos (mismo editorId) conservan su editorPos
            //    actual: mover nodos no entra en el undo.
            //  - m_nextEditorId nunca baja.
            // NO resuelve clips: el caché de clipIndex lo rehace el llamante
            // con rebindClips.
            void applyGraph(const Graph& g);
```

`AnimatorComponent.cpp`, después de `removeParameter` (tras la llave de cierre de `:143`):

```cpp
    AnimatorComponent::Graph AnimatorComponent::graph() const
    {
        return Graph{ m_states, m_transitions, m_parameters, m_entryState };
    }

    void AnimatorComponent::applyGraph(const Graph& g)
    {
        // Identidades vivas ANTES de sustituir nada: tras copiar g ya no se
        // sabría qué editorId era el actual, cuál el que se apagaba, ni dónde
        // estaba cada nodo en el canvas.
        auto editorIdAt = [this](int idx) {
            return (idx >= 0 && idx < (int)m_states.size()) ? m_states[idx].editorId : -1;
        };
        const int curId  = editorIdAt(m_currentState);
        const int prevId = editorIdAt(m_prevState);
        std::unordered_map<int, glm::vec2> livePos;
        for (const auto& s : m_states) livePos[s.editorId] = s.editorPos;

        const std::vector<Parameter> oldParams = m_parameters;
        auto oldBools    = m_bools;
        auto oldTriggers = m_triggers;
        auto oldInts     = m_ints;
        auto oldFloats   = m_floats;

        m_states      = g.states;
        m_transitions = g.transitions;
        m_parameters  = g.parameters;
        m_entryState  = g.entryState;

        // Dos pasadas: primero adelantar el contador con los ids que ya
        // traen los estados, después repartir a los que llegan sin id (-1).
        // Al revés, un estado sin id podría recibir uno que otro trae ya.
        for (auto& s : m_states)
        {
            if (s.editorId < 0) continue;
            m_nextEditorId = std::max(m_nextEditorId, s.editorId + 1);
            auto it = livePos.find(s.editorId);
            if (it != livePos.end()) s.editorPos = it->second;
        }
        for (auto& s : m_states)
            if (s.editorId < 0) s.editorId = m_nextEditorId++;

        m_bools.clear();
        m_triggers.clear();
        m_ints.clear();
        m_floats.clear();
        for (const auto& p : m_parameters)
        {
            bool mismoTipo = false;
            for (const auto& o : oldParams)
                if (o.name == p.name) { mismoTipo = (o.type == p.type); break; }
            switch (p.type)
            {
                case ParamType::Bool:    m_bools[p.name]    = mismoTipo ? oldBools[p.name]    : false; break;
                case ParamType::Trigger: m_triggers[p.name] = mismoTipo ? oldTriggers[p.name] : false; break;
                case ParamType::Int:     m_ints[p.name]     = mismoTipo ? oldInts[p.name]     : 0;     break;
                case ParamType::Float:   m_floats[p.name]   = mismoTipo ? oldFloats[p.name]   : 0.0f;  break;
            }
        }

        auto indexOf = [this](int editorId) {
            if (editorId < 0) return -1;
            for (int i = 0; i < (int)m_states.size(); i++)
                if (m_states[i].editorId == editorId) return i;
            return -1;
        };
        const int cur  = indexOf(curId);
        const int prev = indexOf(prevId);

        if (curId < 0)
        {
            // No había playhead (grafo sin arrancar): update() lo pondrá en la
            // entrada, igual que antes de aplicar nada.
            m_currentState = -1;
        }
        else if (cur >= 0)
        {
            m_currentState = cur;
        }
        else
        {
            m_currentState = m_entryState;
            m_animTime     = 0.0f;
            m_finished     = false;
        }

        if (m_prevState >= 0 && cur >= 0 && prev >= 0)
        {
            m_prevState = prev;
        }
        else
        {
            m_prevState     = -1;
            m_prevAnimTime  = 0.0f;
            m_blendElapsed  = 0.0f;
            m_blendDuration = 0.0f;
        }
    }
```

`AnimatorComponent.cpp` ya incluye `<algorithm>` (`std::max`); `<unordered_map>` y glm llegan por el header.

- [ ] **Step 4: Run tests to verify they pass**

PowerShell: `& .\build.bat`
Bash: `./build-ninja/engine/tests/dt_animator_tests.exe`
Expected: `dt_animator_tests: OK`.

- [ ] **Step 5: Sabotage, one at a time**

Aplicar, compilar, ejecutar, ver caer SOLO el test indicado, restaurar:
1. Cambiar `mismoTipo ? oldFloats[p.name] : 0.0f` por `0.0f` → cae `test_apply_graph_keeps_param_values_of_same_name_and_type`.
2. Cambiar `m_currentState = cur;` por `m_currentState = std::min(m_currentState, (int)m_states.size() - 1);` (casar por índice) → cae `test_apply_graph_playhead_follows_editor_id`.
3. Cambiar `m_nextEditorId = std::max(m_nextEditorId, s.editorId + 1);` por `m_nextEditorId = s.editorId + 1;` → cae `test_apply_graph_never_lowers_next_editor_id`.
4. Quitar la línea `if (it != livePos.end()) s.editorPos = it->second;` → cae `test_apply_graph_live_states_keep_position`.
5. Cambiar la condición `if (m_prevState >= 0 && cur >= 0 && prev >= 0)` por `if (false)` → cae `test_apply_own_graph_changes_nothing`.
6. Cambiar esa misma condición por `if (m_prevState >= 0)` → cae `test_apply_graph_without_fading_state_cuts_crossfade`.

Tras restaurar: `dt_animator_tests: OK`.

- [ ] **Step 6: Commit**

Mensaje: `feat(animator): Graph y applyGraph sin tocar el runtime`, con una línea de cuerpo: "Base del undo del grafo del AnimatorPanel: sustituye lo autorado casando playhead y posiciones por editorId." + Co-Authored-By.

---

### Task 3: clave de comparación `animatorGraphKey`

**Files:**
- Create: `engine/include/DonTopo/Core/AnimatorSerialization.h`
- Modify: `engine/src/Core/Scene.cpp` (include; cerrar y reabrir el namespace anónimo alrededor de `animatorToJson`, `:518-583`)
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `AnimatorComponent::graph()` / `applyGraph()` (Task 2), solo en dos mutaciones de test.
- Produces:
  - `nlohmann::json DonTopo::animatorToJson(const AnimatorComponent& a);` (el mismo formato de siempre del bloque `"animator"` del `.scene`)
  - `nlohmann::json DonTopo::animatorGraphKey(const AnimatorComponent& a);` (lo mismo sin `"pos"` en cada estado)
  - En los tests: `static void makeBaseGraph(AnimatorComponent&)` y `static std::vector<GraphMutation> graphMutations()`, que reutiliza la Task 4.

**Por qué así y no de otra forma:** la función anónima no se puede dejar donde está con una pública del mismo nombre al lado. La llamada de `Scene.cpp:1284` pasa un `DonTopo::AnimatorComponent`, así que ADL encontraría las dos y sería ambigua. Hay que **sacarla** del namespace anónimo. En `Scene.cpp` el namespace anónimo está en el nivel superior (`:49`) y solo trae declaraciones `using DonTopo::X;` (`:51-52`), así que cerrarlo antes de la función y reabrirlo después es seguro. Los helpers `paramTypeToStr`, `condTypeToStr` y `compareToStr` se quedan dentro y siguen siendo visibles desde `namespace DonTopo`, porque la búsqueda sin calificar llega al ámbito global.

- [ ] **Step 1: Write the failing tests**

En `animator_tests.cpp`: añadir a los includes

```cpp
#include "DonTopo/Core/AnimatorSerialization.h"
#include <functional>
#include <utility>
#include <vector>
```

y antes de `int main()`:

```cpp
// ---- Clave del undo del grafo: lo que se guarda, menos la posición de los nodos ----

// Grafo base de los tests de undo: dos estados (A mezcla con otro clip, así
// animatorToJson emite también los campos de blend), un parámetro Bool, uno
// Float y uno Int, y una transición A->B con una condición Bool y otra Float
// (así se emiten expected, compare y threshold).
static void makeBaseGraph(AnimatorComponent& a)
{
    AnimatorComponent::State sa;
    sa.name = "A"; sa.clipName = "walk";
    sa.blendClipName = "run"; sa.blendParam = "speed";
    sa.blendMin = 0.0f; sa.blendMax = 1.0f;
    sa.duration = 40.0f; sa.ticksPerSecond = 20.0f;
    AnimatorComponent::State sb;
    sb.name = "B"; sb.clipName = "run";
    sb.duration = 100.0f; sb.ticksPerSecond = 50.0f;
    a.addState(sa);
    a.addState(sb);
    a.addParameter("go",    AnimatorComponent::ParamType::Bool);
    a.addParameter("speed", AnimatorComponent::ParamType::Float);
    a.addParameter("hits",  AnimatorComponent::ParamType::Int);

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1; t.duration = 0.25f;
    AnimatorComponent::Condition cb;
    cb.type = AnimatorComponent::ConditionType::Bool;
    cb.paramName = "go"; cb.expected = true;
    AnimatorComponent::Condition cf;
    cf.type = AnimatorComponent::ConditionType::Float;
    cf.paramName = "speed";
    cf.compare = AnimatorComponent::Compare::Greater; cf.threshold = 0.5f;
    t.conditions = { cb, cf };
    a.addTransition(t);
}

// Una mutación por cada cosa que el AnimatorPanel puede cambiar y el .scene
// guarda, hecha por la misma API de Core que usa el panel. Sobre makeBaseGraph.
using GraphMutation = std::pair<const char*, std::function<void(AnimatorComponent&)>>;
static std::vector<GraphMutation> graphMutations()
{
    using A = AnimatorComponent;
    return {
        { "state.name",           [](A& a) { a.statesMutable()[0].name = "X"; } },
        { "state.clip",           [](A& a) { a.statesMutable()[0].clipName = "idle"; } },
        { "state.loop",           [](A& a) { a.statesMutable()[0].loop = !a.states()[0].loop; } },
        { "state.blendClip",      [](A& a) { a.statesMutable()[0].blendClipName = "idle"; } },
        { "state.blendParam",     [](A& a) { a.statesMutable()[0].blendParam = "hits"; } },
        { "state.blendMin",       [](A& a) { a.statesMutable()[0].blendMin = 0.25f; } },
        { "state.blendMax",       [](A& a) { a.statesMutable()[0].blendMax = 2.0f; } },
        { "state.lockRootMotion", [](A& a) { a.statesMutable()[0].lockRootMotion = true; } },
        { "addState",             [](A& a) { A::State s; s.name = "C"; s.clipName = "idle"; a.addState(s); } },
        { "removeState",          [](A& a) { a.removeState(1); } },
        { "entryState",           [](A& a) { a.setEntryState(1); } },
        { "addParameter",         [](A& a) { a.addParameter("nuevo", A::ParamType::Trigger); } },
        { "removeParameter",      [](A& a) { a.removeParameter("hits"); } },
        { "parameter.name",       [](A& a) { A::Graph g = a.graph(); g.parameters[2].name = "golpes"; a.applyGraph(g); } },
        { "parameter.type",       [](A& a) { A::Graph g = a.graph(); g.parameters[2].type = A::ParamType::Float; a.applyGraph(g); } },
        { "addTransition",        [](A& a) { A::Transition t; t.fromState = 1; t.toState = 0; a.addTransition(t); } },
        { "removeTransition",     [](A& a) { a.removeTransition(0); } },
        { "transition.from",      [](A& a) { a.transitionsMutable()[0].fromState = 1; } },
        { "transition.to",        [](A& a) { a.transitionsMutable()[0].toState = 0; } },
        { "transition.duration",  [](A& a) { a.transitionsMutable()[0].duration = 1.5f; } },
        { "condition.add",        [](A& a) { A::Condition c; c.type = A::ConditionType::AnimationFinished;
                                              a.transitionsMutable()[0].conditions.push_back(c); } },
        { "condition.remove",     [](A& a) { a.transitionsMutable()[0].conditions.pop_back(); } },
        { "condition.type",       [](A& a) { a.transitionsMutable()[0].conditions[0].type = A::ConditionType::Trigger; } },
        { "condition.param",      [](A& a) { a.transitionsMutable()[0].conditions[0].paramName = "otro"; } },
        { "condition.expected",   [](A& a) { a.transitionsMutable()[0].conditions[0].expected = false; } },
        { "condition.compare",    [](A& a) { a.transitionsMutable()[0].conditions[1].compare = A::Compare::Less; } },
        { "condition.threshold",  [](A& a) { a.transitionsMutable()[0].conditions[1].threshold = 9.0f; } },
    };
}

// Mover un nodo no es una edición para el undo (decisión de diseño). El
// formato del .scene SÍ guarda la posición: se comprueba también, para que el
// test no pase solo porque animatorToJson dejara de emitirla.
static void test_graph_key_ignores_node_position()
{
    AnimatorComponent a;
    makeBaseGraph(a);
    const nlohmann::json k0 = animatorGraphKey(a);

    a.statesMutable()[0].editorPos = glm::vec2(123.0f, 456.0f);

    CHECK(animatorGraphKey(a) == k0);
    CHECK(animatorToJson(a)["states"][0].contains("pos"));
}

// Todo lo que se guarda tiene que verse en la clave: un campo que no se viera
// sería una edición que no deja entrada en el undo, y sin avisar.
static void test_graph_key_sees_every_saved_field()
{
    AnimatorComponent base;
    makeBaseGraph(base);
    const nlohmann::json k0 = animatorGraphKey(base);

    for (const auto& [name, mutate] : graphMutations())
    {
        AnimatorComponent a = base;
        mutate(a);
        if (animatorGraphKey(a) == k0)
        {
            std::printf("FAIL: la clave del grafo no ve '%s'\n", name);
            ++g_failures;
        }
    }
}
```

Y en `main()`, antes de `am.shutdown();`:

```cpp
    test_graph_key_ignores_node_position();
    test_graph_key_sees_every_saved_field();
```

- [ ] **Step 2: Run to verify it fails**

PowerShell: `& .\build.bat`
Expected: `Cannot open include file: 'DonTopo/Core/AnimatorSerialization.h'`.

- [ ] **Step 3: Implement**

Crear `engine/include/DonTopo/Core/AnimatorSerialization.h`:

```cpp
#pragma once
#include <nlohmann/json_fwd.hpp>

namespace DonTopo
{
    class AnimatorComponent;

    // El bloque "animator" del .scene. La definición vive en Scene.cpp, porque el
    // formato es de la escena. Se declara aquí para que el undo del editor
    // compare grafos con la MISMA vara con la que se guardan.
    nlohmann::json animatorToJson(const AnimatorComponent& a);

    // animatorToJson sin la posición de los nodos ("pos" de cada estado): lo
    // que el undo del AnimatorPanel considera una edición. Mover un nodo no
    // lo es (ver docs/superpowers/specs/2026-09-11-animator-graph-undo-design.md).
    // Cualquier campo que se añada al .scene entra aquí sin tocar nada más.
    nlohmann::json animatorGraphKey(const AnimatorComponent& a);
}
```

En `engine/src/Core/Scene.cpp`:

1. Añadir `#include "DonTopo/Core/AnimatorSerialization.h"` justo después de `#include "DonTopo/Core/AnimatorComponent.h"` (`:7`).
2. Justo **antes** de la línea `    nlohmann::json animatorToJson(const AnimatorComponent& a)` (`:518`), insertar:

```cpp
}   // namespace (anónimo)

// Fuera del namespace anónimo porque el undo del editor la usa (ver
// AnimatorSerialization.h). Sigue en este fichero porque el formato es de la
// escena, y sus helpers (paramTypeToStr, condTypeToStr, compareToStr) se quedan
// dentro del anónimo: desde aquí se ven igual.
namespace DonTopo
{
```

3. Justo **después** de la llave que cierra `animatorToJson` (`:583`, la línea `    }` que sigue a `                 {"transitions", transitions} };`), insertar:

```cpp

    nlohmann::json animatorGraphKey(const AnimatorComponent& a)
    {
        nlohmann::json key = animatorToJson(a);
        for (auto& s : key["states"]) s.erase("pos");
        return key;
    }
}   // namespace DonTopo

namespace
{
```

El cuerpo de `animatorToJson` no se toca. Comprobarlo antes de hacer commit: `git diff engine/src/Core/Scene.cpp` solo puede mostrar el include y los dos bloques insertados. Ninguna línea de `:518-583` puede aparecer como cambiada.

- [ ] **Step 4: Run tests to verify they pass**

PowerShell: `& .\build.bat`
Bash: `./build-ninja/engine/tests/dt_animator_tests.exe`
Expected: `dt_animator_tests: OK`. Los tests de ida y vuelta de escena que ya existían (`test_graph_survives_scene_round_trip` y compañía) siguen en verde: prueban que el formato no ha cambiado.

- [ ] **Step 5: Sabotage, one at a time**

1. En `animatorGraphKey`, quitar el bucle del `erase` → cae `test_graph_key_ignores_node_position`.
2. En `animatorToJson`, comentar la línea `sj["blendMax"] = s.blendMax;` → cae `test_graph_key_sees_every_saved_field` con `la clave del grafo no ve 'state.blendMax'`, y nada más (los tests de ida y vuelta de blend también pueden caer, porque es el formato: vale, es la misma guarda). Restaurar.

- [ ] **Step 6: Commit**

Mensaje: `feat(animator): animatorGraphKey, la clave del undo del grafo`, con cuerpo: "animatorToJson sale del namespace anónimo de Scene.cpp sin cambiar su cuerpo; la clave es ese JSON sin 'pos'." + Co-Authored-By.

---

### Task 4: `AnimatorGraphCommand`

**Files:**
- Modify: `engine/include/DonTopo/Editor/Command.h` (clase nueva justo después de `AnimatorComponentCommand`, que termina en `:644`)
- Modify: `engine/src/Editor/Command.cpp` (después de `AnimatorComponentCommand::apply`, que termina en `:475`)
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `AnimatorComponent::Graph`, `applyGraph` (Task 2); `animatorGraphKey`, `makeBaseGraph`, `graphMutations` (Task 3).
- Produces: `AnimatorGraphCommand(Scene& scene, std::string label, uint64_t id, AnimatorComponent::Graph before, AnimatorComponent::Graph after)`: `execute()` aplica `after` y `undo()` aplica `before`.

- [ ] **Step 1: Write the failing tests**

En `animator_tests.cpp`, antes de `int main()`:

```cpp
// ---- AnimatorGraphCommand ----

// Por cada cosa que el panel puede editar: undo deja el grafo como estaba y
// redo lo vuelve a dejar editado.
static void test_graph_command_round_trip_for_each_mutation()
{
    for (const auto& [name, mutate] : graphMutations())
    {
        Scene scene("Test");
        GameObject* go = scene.addGameObject("Personaje");
        auto a = std::make_shared<AnimatorComponent>();
        makeBaseGraph(*a);
        go->setAnimator(a);

        const AnimatorComponent::Graph before    = a->graph();
        const nlohmann::json           keyBefore = animatorGraphKey(*a);
        mutate(*a);
        const nlohmann::json           keyAfter  = animatorGraphKey(*a);

        AnimatorGraphCommand cmd(scene, name, go->id, before, a->graph());

        cmd.undo();
        if (animatorGraphKey(*a) != keyBefore)
        {
            std::printf("FAIL: el undo de '%s' no restaura el grafo\n", name);
            ++g_failures;
        }
        cmd.execute();
        if (animatorGraphKey(*a) != keyAfter)
        {
            std::printf("FAIL: el redo de '%s' no vuelve a aplicar el grafo\n", name);
            ++g_failures;
        }
    }
}

// El Animator puede haber desaparecido entre el gesto y el undo (un
// AnimatorComponentCommand posterior lo quitó), o el objeto entero.
static void test_graph_command_noop_without_animator()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto a = std::make_shared<AnimatorComponent>();
    makeBaseGraph(*a);
    go->setAnimator(a);
    const uint64_t id = go->id;

    AnimatorGraphCommand cmd(scene, "Editar Animator", id, a->graph(), a->graph());

    go->setAnimator(nullptr);
    cmd.undo();
    cmd.execute();
    CHECK(!go->hasAnimator());

    scene.removeGameObject(go);
    cmd.undo();      // findById devuelve nullptr y sale sin tocar nada
    cmd.execute();
}

// El snapshot trae clipIndex de cuando se tomó; entretanto una fuente de
// animación puede haber cambiado la lista de clips. Tras aplicar, los índices
// salen resueltos contra el mesh ACTUAL.
static void test_graph_command_rebinds_clips()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto mesh = std::make_shared<SkinnedMesh>();
    AnimationClip walk; walk.name = "walk"; walk.duration = 40.0f;  walk.ticksPerSecond = 20.0f;
    AnimationClip run;  run.name  = "run";  run.duration  = 100.0f; run.ticksPerSecond  = 50.0f;
    mesh->animationClips = { walk, run };
    go->setMesh(mesh);

    auto a = std::make_shared<AnimatorComponent>();
    makeBaseGraph(*a);   // A usa "walk" (+ "run" de blend), B usa "run"
    go->setAnimator(a);

    AnimatorComponent::Graph stale = a->graph();
    for (auto& s : stale.states) { s.clipIndex = -1; s.blendClipIndex = -1; }

    AnimatorGraphCommand cmd(scene, "Editar Animator", go->id, stale, stale);
    cmd.undo();

    CHECK(a->states()[0].clipIndex == 0);
    CHECK(a->states()[0].blendClipIndex == 1);
    CHECK(a->states()[1].clipIndex == 1);
}
```

Y en `main()`, antes de `am.shutdown();`:

```cpp
    test_graph_command_round_trip_for_each_mutation();
    test_graph_command_noop_without_animator();
    test_graph_command_rebinds_clips();
```

- [ ] **Step 2: Run to verify it fails**

PowerShell: `& .\build.bat`
Expected: `'AnimatorGraphCommand': undeclared identifier`.

- [ ] **Step 3: Implement**

`Command.h`, justo después del `};` de `AnimatorComponentCommand`:

```cpp

// Edición del GRAFO de un Animator que ya existe (estados, transiciones,
// condiciones, parámetros, entrada), por el stack de undo. La crea
// AnimatorGraphUndoTracker al terminar un gesto en el AnimatorPanel, con el
// cambio YA aplicado: se empuja sin execute().
//
// A diferencia de AnimatorComponentCommand, que pone o quita el componente
// entero, esto aplica solo lo autorado vía applyGraph: los valores de los
// parámetros y el playhead sobreviven, así que un undo en Play no se lleva por
// delante lo que el script venía escribiendo (H4).
//
// Resuelve el GameObject por id en cada aplicación, nunca por puntero. Si el
// objeto o su Animator ya no existen, no hace nada.
class AnimatorGraphCommand : public ICommand {
public:
    AnimatorGraphCommand(Scene& scene, std::string label, uint64_t id,
                          AnimatorComponent::Graph before, AnimatorComponent::Graph after);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(const AnimatorComponent::Graph& g);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    AnimatorComponent::Graph m_before;
    AnimatorComponent::Graph m_after;
};
```

`Command.cpp`, después de `AnimatorComponentCommand::apply`:

```cpp

AnimatorGraphCommand::AnimatorGraphCommand(Scene& scene, std::string label, uint64_t id,
                                           AnimatorComponent::Graph before,
                                           AnimatorComponent::Graph after)
    : m_scene(scene), m_label(std::move(label)), m_id(id),
      m_before(std::move(before)), m_after(std::move(after)) {}

void AnimatorGraphCommand::execute() { apply(m_after); }
void AnimatorGraphCommand::undo()    { apply(m_before); }

void AnimatorGraphCommand::apply(const AnimatorComponent::Graph& g)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go || !go->getAnimator()) return;
    go->getAnimator()->applyGraph(g);
    // El snapshot trae los clipIndex de cuando se tomó, y una fuente de
    // animación añadida o quitada entretanto cambia la lista de clips: se
    // vuelven a resolver por nombre. rebindClips y no bindClips: esto corre
    // en Play y bindClips haría reset().
    if (SkinnedMesh* mesh = go->getSkinnedMesh())
        go->getAnimator()->rebindClips(*mesh, nullptr);
}
```

(`Command.cpp` ya llama a `rebindClips` en `AnimationSourceCommand`, así que tiene los includes de `SkinnedMesh` y de `AnimatorComponent`.)

- [ ] **Step 4: Run tests to verify they pass**

PowerShell: `& .\build.bat`
Bash: `./build-ninja/engine/tests/dt_animator_tests.exe`
Expected: `dt_animator_tests: OK`.

- [ ] **Step 5: Sabotage, one at a time**

1. En `undo()`, cambiar `apply(m_before)` por `apply(m_after)` → caen las líneas `el undo de '...' no restaura el grafo` de `test_graph_command_round_trip_for_each_mutation`.
2. Quitar la comprobación `|| !go->getAnimator()` → `test_graph_command_noop_without_animator` revienta (desreferencia de nulo). Vale como "cae"; restaurar.
3. Quitar las dos líneas del `rebindClips` → cae `test_graph_command_rebinds_clips`.

- [ ] **Step 6: Commit**

Mensaje: `feat(editor): AnimatorGraphCommand, undo del grafo del Animator` + Co-Authored-By.

---

### Task 5: `AnimatorGraphUndoTracker`

**Files:**
- Create: `engine/include/DonTopo/Editor/AnimatorGraphUndo.h`
- Create: `engine/src/Editor/AnimatorGraphUndo.cpp`
- Modify: `engine/CMakeLists.txt:149` (añadir `src/Editor/AnimatorGraphUndo.cpp` en la línea siguiente a `src/Editor/AnimatorPanel.cpp`, con la misma indentación)
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `animatorGraphKey` (Task 3); `AnimatorGraphCommand` (Task 4); el valor de `UndoManager::revision()` (Task 1), que llega como `uint64_t`, así que el tracker no depende de `UndoManager`.
- Produces:
  ```cpp
  class AnimatorGraphUndoTracker {
  public:
      void beginFrame(uint64_t goId, const AnimatorComponent* anim, uint64_t undoRevision);
      std::unique_ptr<ICommand> endFrame(Scene& scene, const AnimatorComponent* anim,
                                         bool anyItemActive, uint64_t undoRevision);
      void setLabel(std::string label);
      void discard();
      bool sessionOpen() const;
  };
  ```

- [ ] **Step 1: Write the failing tests**

En `animator_tests.cpp`, añadir `#include "DonTopo/Editor/AnimatorGraphUndo.h"` a los includes, y antes de `int main()`:

```cpp
// ---- AnimatorGraphUndoTracker: un comando por gesto ----

// Un drag son varios frames con el widget activo y un frame de soltar. Todo el
// drag tiene que ser UN comando, y su 'before' es el grafo de antes del primer
// frame.
static void test_tracker_drag_yields_one_command()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto a = std::make_shared<AnimatorComponent>();
    makeBaseGraph(*a);
    go->setAnimator(a);
    const nlohmann::json keyBefore = animatorGraphKey(*a);

    AnimatorGraphUndoTracker tr;
    const uint64_t rev = 7;
    int durante = 0;
    for (int f = 0; f < 3; f++)
    {
        tr.beginFrame(go->id, a.get(), rev);
        a->transitionsMutable()[0].duration += 0.1f;
        if (tr.endFrame(scene, a.get(), /*anyItemActive=*/true, rev)) durante++;
    }
    tr.beginFrame(go->id, a.get(), rev);
    std::unique_ptr<ICommand> cmd = tr.endFrame(scene, a.get(), /*anyItemActive=*/false, rev);

    CHECK(durante == 0);
    CHECK(cmd != nullptr);
    if (cmd)
    {
        cmd->undo();
        CHECK(animatorGraphKey(*a) == keyBefore);
    }
}

// Un drag que acaba donde empezó no es una edición.
static void test_tracker_drag_back_to_start_yields_nothing()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto a = std::make_shared<AnimatorComponent>();
    makeBaseGraph(*a);
    go->setAnimator(a);
    const float original = a->transitions()[0].duration;

    AnimatorGraphUndoTracker tr;
    tr.beginFrame(go->id, a.get(), 1);
    a->transitionsMutable()[0].duration = 3.0f;
    CHECK(tr.endFrame(scene, a.get(), true, 1) == nullptr);
    tr.beginFrame(go->id, a.get(), 1);
    a->transitionsMutable()[0].duration = original;
    CHECK(tr.endFrame(scene, a.get(), false, 1) == nullptr);
}

// Si el historial se movió durante el gesto (un comando propio del panel, un
// Ctrl+Z, el clear() de Play), la diferencia ya no es solo del usuario: no se
// emite nada. Y la nueva línea base es buena: el frame siguiente, sin cambios,
// tampoco emite nada.
static void test_tracker_revision_change_rebases()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto a = std::make_shared<AnimatorComponent>();
    makeBaseGraph(*a);
    go->setAnimator(a);

    AnimatorGraphUndoTracker tr;
    tr.beginFrame(go->id, a.get(), 1);
    a->statesMutable()[0].clipName = "idle";   // p. ej. un ClipRenameCommand
    CHECK(tr.endFrame(scene, a.get(), false, /*undoRevision=*/2) == nullptr);

    tr.beginFrame(go->id, a.get(), 2);
    CHECK(tr.endFrame(scene, a.get(), false, 2) == nullptr);
}

// Cambiar de GameObject cierra la sesión del anterior: su 'before' no puede
// acabar en un comando contra el objeto nuevo.
static void test_tracker_selection_change_discards_session()
{
    Scene scene("Test");
    GameObject* goA = scene.addGameObject("A");
    GameObject* goB = scene.addGameObject("B");
    auto a = std::make_shared<AnimatorComponent>();
    auto b = std::make_shared<AnimatorComponent>();
    makeBaseGraph(*a);
    makeBaseGraph(*b);
    // B distinto de A: si la sesión de A sobreviviera al cambio de selección,
    // su 'before' (el grafo base) no casaría con B y saldría un comando.
    b->statesMutable()[0].name = "OtroGrafo";
    goA->setAnimator(a);
    goB->setAnimator(b);

    AnimatorGraphUndoTracker tr;
    tr.beginFrame(goA->id, a.get(), 1);
    a->transitionsMutable()[0].duration = 3.0f;
    CHECK(tr.endFrame(scene, a.get(), true, 1) == nullptr);   // drag en curso sobre A

    tr.beginFrame(goB->id, b.get(), 1);                       // la selección pasa a B
    CHECK(tr.endFrame(scene, b.get(), false, 1) == nullptr);  // B no ha cambiado
}

// Mover nodos no entra en el undo.
static void test_tracker_node_move_yields_nothing()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto a = std::make_shared<AnimatorComponent>();
    makeBaseGraph(*a);
    go->setAnimator(a);

    AnimatorGraphUndoTracker tr;
    tr.beginFrame(go->id, a.get(), 1);
    a->statesMutable()[0].editorPos = glm::vec2(99.0f, 99.0f);
    CHECK(tr.endFrame(scene, a.get(), false, 1) == nullptr);
}

// El label que pone un sitio del panel vale para ESE gesto; el siguiente
// vuelve al genérico.
static void test_tracker_label_is_per_gesture()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto a = std::make_shared<AnimatorComponent>();
    makeBaseGraph(*a);
    go->setAnimator(a);

    AnimatorGraphUndoTracker tr;
    tr.beginFrame(go->id, a.get(), 1);
    tr.setLabel("Borrar estado");
    a->removeState(1);
    std::unique_ptr<ICommand> c1 = tr.endFrame(scene, a.get(), false, 1);

    tr.beginFrame(go->id, a.get(), 1);
    a->statesMutable()[0].loop = !a->states()[0].loop;
    std::unique_ptr<ICommand> c2 = tr.endFrame(scene, a.get(), false, 1);

    CHECK(c1 && c1->label() == "Borrar estado");
    CHECK(c2 && c2->label() == "Editar Animator");
}
```

Y en `main()`, antes de `am.shutdown();`:

```cpp
    test_tracker_drag_yields_one_command();
    test_tracker_drag_back_to_start_yields_nothing();
    test_tracker_revision_change_rebases();
    test_tracker_selection_change_discards_session();
    test_tracker_node_move_yields_nothing();
    test_tracker_label_is_per_gesture();
```

- [ ] **Step 2: Run to verify it fails**

PowerShell: `& .\build.bat`
Expected: `Cannot open include file: 'DonTopo/Editor/AnimatorGraphUndo.h'`.

- [ ] **Step 3: Implement**

Crear `engine/include/DonTopo/Editor/AnimatorGraphUndo.h`:

```cpp
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>
#include "DonTopo/Core/AnimatorComponent.h"

namespace DonTopo {

class ICommand;
class Scene;

// Convierte en comandos de undo, uno por gesto, las ediciones que el
// AnimatorPanel hace EN VIVO sobre el grafo.
//
// El panel muta el componente en ~22 sitios. Envolver cada uno con su
// antes/después serían 22 obligaciones, y el sitio 23 se la saltaría sin
// avisar. En su lugar, el panel llama a beginFrame al empezar a dibujar y a
// endFrame al terminar, y esto compara el grafo entre las dos llamadas con la
// clave del .scene (animatorGraphKey). Así, cualquier cosa que se guarde y
// cambie entra en el undo, venga del sitio que venga.
//
// Sin ImGui a propósito: quién está activo y en qué revisión va el historial
// llegan como parámetros, así que se prueba sin GUI.
class AnimatorGraphUndoTracker {
public:
    // Abre una sesión si no hay una abierta para este mismo GameObject: un drag
    // que sigue de un frame anterior conserva su 'before'. anim nulo descarta.
    void beginFrame(uint64_t goId, const AnimatorComponent* anim, uint64_t undoRevision);

    // Devuelve el comando del gesto cuando el grafo ha cambiado y ya no queda
    // ningún widget activo, con el cambio YA aplicado: el llamante lo empuja
    // sin execute(). Si el historial se movió durante el gesto (undoRevision
    // distinta), no emite nada y toma una nueva línea base: la diferencia
    // incluiría lo que hizo otro comando.
    std::unique_ptr<ICommand> endFrame(Scene& scene, const AnimatorComponent* anim,
                                       bool anyItemActive, uint64_t undoRevision);

    // Opcional: nombre del gesto en curso para la Log Console ("Borrar
    // estado"). Si ningún sitio lo llama, el comando se llama "Editar Animator".
    void setLabel(std::string label) { m_label = std::move(label); }

    // El panel ha dejado de dibujar el grafo (cerrado, colapsado, sin
    // Animator): una sesión no puede sobrevivir a eso.
    void discard();

    bool sessionOpen() const { return m_open; }

private:
    static constexpr const char* kEtiquetaPorDefecto = "Editar Animator";

    void open(uint64_t goId, const AnimatorComponent& anim, uint64_t undoRevision);
    void close();

    bool                     m_open     = false;
    uint64_t                 m_id       = 0;
    uint64_t                 m_revision = 0;
    AnimatorComponent::Graph m_before;
    nlohmann::json           m_beforeKey;
    std::string              m_label    = kEtiquetaPorDefecto;
};

} // namespace DonTopo
```

Crear `engine/src/Editor/AnimatorGraphUndo.cpp`:

```cpp
#include "DonTopo/Editor/AnimatorGraphUndo.h"
#include "DonTopo/Core/AnimatorSerialization.h"
#include "DonTopo/Editor/Command.h"

namespace DonTopo {

void AnimatorGraphUndoTracker::open(uint64_t goId, const AnimatorComponent& anim,
                                    uint64_t undoRevision)
{
    m_open      = true;
    m_id        = goId;
    m_revision  = undoRevision;
    m_before    = anim.graph();
    m_beforeKey = animatorGraphKey(anim);
}

void AnimatorGraphUndoTracker::close()
{
    m_open  = false;
    m_label = kEtiquetaPorDefecto;
}

void AnimatorGraphUndoTracker::discard()
{
    close();
}

void AnimatorGraphUndoTracker::beginFrame(uint64_t goId, const AnimatorComponent* anim,
                                          uint64_t undoRevision)
{
    if (!anim) { close(); return; }
    if (m_open && m_id == goId) return;   // el gesto sigue desde un frame anterior
    open(goId, *anim, undoRevision);
}

std::unique_ptr<ICommand> AnimatorGraphUndoTracker::endFrame(Scene& scene,
                                                             const AnimatorComponent* anim,
                                                             bool anyItemActive,
                                                             uint64_t undoRevision)
{
    if (!m_open) return nullptr;
    if (!anim) { close(); return nullptr; }

    if (undoRevision != m_revision)
    {
        // Alguien movió el historial en mitad del gesto. Nueva línea base: si el
        // gesto sigue (un drag), lo que quede de él se medirá desde aquí.
        open(m_id, *anim, undoRevision);
        m_label = kEtiquetaPorDefecto;
        if (!anyItemActive) close();
        return nullptr;
    }

    if (animatorGraphKey(*anim) == m_beforeKey) { close(); return nullptr; }
    if (anyItemActive) return nullptr;   // el drag sigue: un solo comando al soltar

    auto cmd = std::make_unique<AnimatorGraphCommand>(scene, m_label, m_id,
                                                      std::move(m_before), anim->graph());
    close();
    return cmd;
}

} // namespace DonTopo
```

`engine/CMakeLists.txt`: en la línea siguiente a `    src/Editor/AnimatorPanel.cpp` (`:149`), añadir `    src/Editor/AnimatorGraphUndo.cpp`.

- [ ] **Step 4: Run tests to verify they pass**

PowerShell: `& .\build.bat`
Bash: `./build-ninja/engine/tests/dt_animator_tests.exe`
Expected: `dt_animator_tests: OK`.

- [ ] **Step 5: Sabotage, one at a time**

1. En `beginFrame`, quitar la línea `if (m_open && m_id == goId) return;` → cae `CHECK(cmd != nullptr)` de `test_tracker_drag_yields_one_command`: cada frame reabre la sesión con el grafo del momento, así que el frame de soltar ya no ve diferencia.
2. Quitar `if (anyItemActive) return nullptr;` → cae `test_tracker_drag_yields_one_command` (`durante != 0`).
3. Quitar el bloque entero `if (undoRevision != m_revision) { ... }` → cae `test_tracker_revision_change_rebases`.
4. En `beginFrame`, cambiar `m_id == goId` por `true` → cae `test_tracker_selection_change_discards_session`.
5. En `close()`, quitar `m_label = kEtiquetaPorDefecto;` → cae `test_tracker_label_is_per_gesture`.

- [ ] **Step 6: Commit**

`git add` los dos ficheros nuevos, `engine/CMakeLists.txt` y `engine/tests/animator_tests.cpp`. Mensaje: `feat(editor): AnimatorGraphUndoTracker, un comando por gesto` + Co-Authored-By.

---

### Task 6: integración en el `AnimatorPanel`

**Files:**
- Modify: `engine/include/DonTopo/Editor/AnimatorPanel.h`
- Modify: `engine/src/Editor/AnimatorPanel.cpp`: `draw` (`:888-988`), `drawParameterList` (`:171`, `:185`) y `drawGraph` (`:380`, `:432-435`)
- Modify: `README.md`, sección `## Animator` (`:676`)

**Interfaces:**
- Consumes: `AnimatorGraphUndoTracker` (Task 5), `UndoManager::revision()` (Task 1). En el `EditorContext` (`EditorContext.h:35-39`) hay `Scene* scene`, `UndoManager* undo` y `std::function<void(const std::string&)> pushLog`.

Esta tarea no tiene test automático: el panel es ImGui y toda la lógica está probada en las tareas 2-5. Su verificación es la build, la suite entera y la tarea 7.

- [ ] **Step 1: Header**

`AnimatorPanel.h`: añadir `#include "DonTopo/Editor/AnimatorGraphUndo.h"` a los includes, y en la parte privada, después de `int m_nodeCtxTarget = -1;`:

```cpp

    // Undo del grafo: convierte las ediciones en vivo de este panel en un
    // comando por gesto (ver AnimatorGraphUndo.h).
    AnimatorGraphUndoTracker m_graphUndo;
    // Revisión del historial en el frame anterior. Si cambia (undo, redo o un
    // push), los nodos que un undo haya reinsertado se recolocan desde el
    // componente: el canvas no los conocía.
    uint64_t m_lastUndoRevision = 0;
```

- [ ] **Step 2: `draw`**

En `AnimatorPanel::draw` (`AnimatorPanel.cpp:888`):

1. Primera línea del cuerpo, antes de `if (m_open)`:

```cpp
    // Solo hay sesión de undo mientras se dibuja un grafo: panel cerrado,
    // colapsado o sin Animator la descartan (ver el final de la función).
    bool grafoDibujado = false;
```

2. En la rama `else` (`:912`), justo después de `if (selectionChanged) m_renamingClip.clear();` (`:920`):

```cpp

                // Undo del grafo: el bracket envuelve TODO lo que puede mutar el
                // componente en este frame, desde drawAnimationSources hasta el
                // popup de condiciones que se dibuja dentro de drawGraph.
                m_graphUndo.beginFrame(go->id, go->getAnimator().get(), ctx.undo->revision());
                grafoDibujado = true;
                const bool historialMovido = ctx.undo->revision() != m_lastUndoRevision;
```

3. Cambiar la condición `if (selectionChanged)` que va justo antes de `syncPositionsFromComponent(go);` (`:959`) por:

```cpp
                if (selectionChanged || historialMovido)
```

y dentro de ese bloque, dejar `m_boundTo = go;` como está: asignarlo también cuando solo se movió el historial no cambia nada, porque `go` ya es `m_boundTo`.

4. Después de `ed::SetCurrentEditor(nullptr);` (`:977`, el que sigue a `syncPositionsToComponent(go);`) y antes de `ImGui::EndChild();`:

```cpp

                // Fin del bracket del undo. IsAnyItemActive: mientras un drag
                // siga activo, el gesto no ha terminado y no se apila nada.
                if (auto cmd = m_graphUndo.endFrame(*ctx.scene, go->getAnimator().get(),
                                                    ImGui::IsAnyItemActive(), ctx.undo->revision()))
                {
                    ctx.pushLog("Animator: " + cmd->label());
                    // Sin execute(): el cambio ya está aplicado (contrato de push).
                    ctx.undo->push(std::move(cmd));
                }
                m_lastUndoRevision = ctx.undo->revision();
```

5. Justo antes de `drawAnimationSourceDialog(ctx);` (`:987`):

```cpp
    if (!grafoDibujado) m_graphUndo.discard();
```

- [ ] **Step 3: Labels**

- `drawParameterList`, en el `if (!toRemove.empty())` (`:169-173`), antes de `anim->removeParameter(toRemove);`:
  ```cpp
        m_graphUndo.setLabel("Quitar parámetro");
  ```
- `drawParameterList`, en el `if (ImGui::Button("Add Parameter") ...)` (`:183-188`), antes de `anim->addParameter(...)`:
  ```cpp
        m_graphUndo.setLabel("Añadir parámetro");
  ```
- `drawGraph`, antes de `anim->addTransition(tr);` (`:380`):
  ```cpp
                    m_graphUndo.setLabel("Crear transición");
  ```
- `drawGraph`, antes de `std::sort(statesToRemove.rbegin(), statesToRemove.rend());` (`:432`):
  ```cpp
        if (!statesToRemove.empty()) m_graphUndo.setLabel("Borrar estado");
  ```

- [ ] **Step 4: README**

En `README.md`, sección `## Animator` (`:676`), al final del párrafo que empieza por "Open the graph with **View → Animator**", añadir la frase:

```
Every edit to the graph (states, transitions, conditions, parameters, blend, entry state) is undoable with Ctrl+Z, one step per gesture — dragging a value is a single step. Moving nodes on the canvas is not recorded.
```

- [ ] **Step 5: Build and full suite**

PowerShell: `& .\build.bat`
Expected: build limpia, sin avisos nuevos en `AnimatorPanel.cpp`, `AnimatorGraphUndo.cpp` ni `Command.cpp`.

Bash, desde la raíz del repo:

```bash
cd /c/Users/ruben/Documents/Don_Topo_Engine
for exe in build-ninja/engine/tests/dt_*.exe; do "$exe" >/dev/null 2>&1 || echo "FALLA $exe"; done
```

Expected: ninguna línea `FALLA` (28/28). `dt_audio_tests` tiene un fallo intermitente conocido de antes (1 de cada ~29 ejecuciones). Si aparece, captura su salida ANTES de repetirlo y dilo en el informe; no lo des por arreglado repitiendo.

- [ ] **Step 6: Commit**

`git add engine/include/DonTopo/Editor/AnimatorPanel.h engine/src/Editor/AnimatorPanel.cpp README.md`. Mensaje: `feat(editor): el AnimatorPanel pasa sus ediciones por el undo`, con cuerpo: "Cierra A2/D1 de docs/animation-audit.md. Un comando por gesto vía AnimatorGraphUndoTracker; mover nodos no se registra." + Co-Authored-By.

---

### Task 7: verificación final (la hace el controlador, no un subagente)

- [ ] **Step 1: Release**

PowerShell: `& .\configure-release.bat` (solo si `build-ninja-release` no existe) y `& .\build-release.bat`. Bash desde la raíz: el mismo bucle sobre `build-ninja-release/engine/tests/dt_*.exe`. Expected: 28/28.

- [ ] **Step 2: Verificación manual en la GUI (usuario), Vulkan y D3D12**

El backend del editor sale del `project.json` del último proyecto abierto: hay que cambiarlo en el proyecto para probar el otro. Para cada backend:
1. Borrar un estado con transiciones → Ctrl+Z: el nodo vuelve a su sitio con sus transiciones. Ctrl+Y: vuelve a borrarse.
2. Arrastrar el umbral de una condición → Ctrl+Z: vuelve al valor de antes del drag en **un** solo paso.
3. En Play: un script cambia un parámetro, borrar una transición, Ctrl+Z: el parámetro conserva su valor y el estado actual no cambia.
4. Mover un nodo → Ctrl+Z: no hace nada con el nodo (deshace la edición anterior, si la hay).

Si `imgui.ini` sale modificado, va al commit.

- [ ] **Step 3: Auditoría**

En `docs/animation-audit.md`: A2 y D1 pasan a **CERRADO** con el hash del commit de la Task 6. A10 sigue ABIERTO, con una nota: todo lo que escribe el acceso mutable queda ahora en el undo, pero el acceso sigue ahí. La fila #1 del backlog se marca como hecha.
