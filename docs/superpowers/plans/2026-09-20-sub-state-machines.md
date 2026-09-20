# Sub-máquinas de estados (fila 15 / C10) — plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** que un estado pueda contener a otros, con entrada propia y transiciones escritas contra la caja, sin tocar la máquina de estados.

**Architecture:** el vector de estados de la capa sigue plano; `State` gana `parent`, `isSubMachine` y `subEntry`. Todo lo nuevo vive en la resolución de índices: entrar resuelve la caja a una hoja bajando por `subEntry`, y salir acepta como origen a cualquier ancestro del estado actual. El estado activo sigue siendo siempre una hoja, así que pose, cross-fade, root motion, curvas e IK no cambian.

**Tech Stack:** C++17, imgui-node-editor (canvas), nlohmann::json (escena), arnés propio de tests (`engine/tests/animator_tests.cpp`, macro `CHECK`).

**Spec:** `docs/superpowers/specs/2026-09-20-sub-state-machines-design.md`

## Global Constraints

- Ficheros en **CRLF**: editar con Edit o con Python usando `newline=''`; nunca `sed -i`.
- Build con la herramienta **PowerShell** (`.\build.bat`) y comprobar el exit code antes de ejecutar nada. **Nunca `cmd /c` desde el Bash tool**: abre un prompt interactivo y cuelga la llamada.
- Tests desde la **raíz del repo**: `build-ninja\engine\tests\dt_animator_tests.exe`.
- Sabotajes **uno a uno**, una llamada por sabotaje, sin bucles; revertir y releer el fichero antes de commitear.
- Any State se sigue evaluando **primero**: eso no se toca.
- Un `.scene` de hoy debe cargar igual y, sin cajas, guardarse igual (ninguna clave nueva).
- Ningún cambio en `update`, la pose, el cross-fade, el root motion, las curvas ni la IK.

---

### Task 1: Modelo y helpers de jerarquía

**Files:**
- Modify: `engine/include/DonTopo/Core/AnimatorComponent.h` (struct `State`; declarar los helpers)
- Modify: `engine/src/Core/AnimatorComponent.cpp` (implementar los helpers; guarda de `enterState`)
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Produces: `State::parent`, `State::isSubMachine`, `State::subEntry`;
  `bool AnimatorComponent::isDescendantOf(int state, int maybeAncestor, int layer) const`;
  `int AnimatorComponent::resolveEntryLeaf(int state, int layer) const` (hoja en la que entrar, o −1).

- [ ] **Step 1: Escribir el test que falla**

```cpp
// Base -> caja "Ataques" { Golpe, caja "Combo" { Uno } }
static AnimatorComponent makeCajas()
{
    AnimatorComponent a;
    AnimatorComponent::State s;
    s.name = "Base";      a.addState(s);            // 0
    s = {}; s.name = "Ataques"; s.isSubMachine = true; a.addState(s);   // 1
    s = {}; s.name = "Golpe";   s.parent = 1;          a.addState(s);   // 2
    s = {}; s.name = "Combo";   s.parent = 1; s.isSubMachine = true; a.addState(s);  // 3
    s = {}; s.name = "Uno";     s.parent = 3;          a.addState(s);   // 4
    a.statesMutable()[1].subEntry = 3;   // Ataques entra por Combo
    a.statesMutable()[3].subEntry = 4;   // Combo entra por Uno
    a.setEntryState(0);
    return a;
}

static void test_submachine_hierarchy_helpers()
{
    AnimatorComponent a = makeCajas();
    CHECK(a.isDescendantOf(4, 1, 0));    // Uno está dentro de Ataques (dos niveles)
    CHECK(a.isDescendantOf(2, 1, 0));
    CHECK(!a.isDescendantOf(0, 1, 0));   // Base no
    CHECK(!a.isDescendantOf(1, 1, 0));   // uno mismo no es su descendiente
    // Entrar en una caja baja hasta la hoja.
    CHECK(a.resolveEntryLeaf(1, 0) == 4);
    CHECK(a.resolveEntryLeaf(2, 0) == 2);   // una hoja se resuelve a sí misma
    // Caja vacía: no hay hoja, y eso NO es entrar a medias.
    a.statesMutable()[3].subEntry = -1;
    CHECK(a.resolveEntryLeaf(1, 0) == -1);
    // Un ciclo de subEntry no puede colgar el motor.
    a.statesMutable()[3].subEntry = 1;
    CHECK(a.resolveEntryLeaf(1, 0) == -1);
    // enterState nunca deja una caja como estado actual.
    AnimatorComponent b = makeCajas();
    b.enterState(1, 0);
    CHECK(b.currentState() == 4);
    b.enterState(0, 0);
    b.statesMutable()[3].subEntry = -1;
    b.enterState(1, 0);
    CHECK(b.currentState() == 0);    // caja rota: no se mueve
}
```

Registrarlo en `main` junto a los demás tests del grafo.

- [ ] **Step 2: Compilar y ver que falla**

PowerShell: `.\build.bat` → error de compilación (`isDescendantOf` no existe).

- [ ] **Step 3: Implementar**

En `AnimatorComponent.h`, dentro de `struct State`, tras `loop`:

```cpp
                // --- Sub-máquinas (C10) ---
                // El vector de la capa sigue plano: esto es contención, no un
                // grafo anidado. Una caja NO se reproduce y nunca es el estado
                // actual; sirve para agrupar en el canvas y para escribir una
                // transición contra el bloque entero.
                int  parent       = -1;      // quién lo contiene; -1 = raíz de la capa
                bool isSubMachine = false;   // es una caja
                int  subEntry     = -1;      // hijo por el que se entra (solo si isSubMachine)
```

En la sección pública, junto a `stateIndexByEditorId`:

```cpp
            // ¿`state` está dentro de `maybeAncestor`, a cualquier profundidad?
            // Uno mismo NO es descendiente de sí mismo.
            bool isDescendantOf(int state, int maybeAncestor, int layer) const;
            // Hoja en la que hay que entrar al ir a `state`: él mismo si no es
            // caja, o el final de la cadena de subEntry. -1 si la cadena se
            // rompe (caja vacía, índice malo o ciclo): entrar a medias en un
            // estado que no existe es peor que no moverse.
            int  resolveEntryLeaf(int state, int layer) const;
```

En `AnimatorComponent.cpp`, junto a `stateIndexByName`:

```cpp
    bool AnimatorComponent::isDescendantOf(int state, int maybeAncestor, int layer) const
    {
        const Layer& L = lay(layer);
        if (maybeAncestor < 0 || state < 0 || state >= (int)L.states.size()) return false;
        int actual = L.states[(size_t)state].parent;
        // El tope es el número de estados: un fichero con un ciclo de parent no
        // puede colgar el motor.
        for (int pasos = 0; actual >= 0 && actual < (int)L.states.size() && pasos <= (int)L.states.size(); pasos++)
        {
            if (actual == maybeAncestor) return true;
            actual = L.states[(size_t)actual].parent;
        }
        return false;
    }

    int AnimatorComponent::resolveEntryLeaf(int state, int layer) const
    {
        const Layer& L = lay(layer);
        int actual = state;
        for (int pasos = 0; pasos <= (int)L.states.size(); pasos++)
        {
            if (actual < 0 || actual >= (int)L.states.size()) return -1;
            const State& st = L.states[(size_t)actual];
            if (!st.isSubMachine) return actual;
            actual = st.subEntry;
        }
        return -1;   // ciclo de subEntry
    }
```

Y `enterState` pasa a resolver antes de mover nada:

```cpp
    void AnimatorComponent::enterState(int idx, int layer)
    {
        Layer& L = lay(layer);
        // Una caja no se reproduce: se entra en su hoja. Si la cadena está rota
        // no se mueve nada, que es la garantía de que currentState nunca es una
        // caja venga de donde venga (transición, Play de Lua o el editor).
        const int hoja = resolveEntryLeaf(idx, layer);
        if (idx >= 0 && hoja < 0) return;
        L.currentState = hoja;
        L.animTime     = 0.0f;
        L.finished     = false;
        L.stateTicks   = 0.0;
    }
```

(El `idx >= 0` conserva el uso actual de `enterState(-1)` para dejar la capa sin estado, que hace `removeState` cuando la capa se queda vacía: comprobar los llamantes con `grep -n "enterState(" engine/src` antes de dar el paso por bueno.)

- [ ] **Step 4: Compilar y ejecutar** — `.\build.bat` (exit 0) y `dt_animator_tests.exe`: todo PASS, incluidos los tests del grafo que ya existían.

- [ ] **Step 5: Sabotajes (uno por llamada)**

1. En `resolveEntryLeaf`, devolver `actual` sin mirar `isSubMachine` → cae `test_submachine_hierarchy_helpers`. Revertir.
2. Quitar el tope de pasos de `resolveEntryLeaf` y comprobar que el test del ciclo **cuelga** (matar el proceso): eso demuestra para qué está el tope. Revertir.
3. En `enterState`, quitar `if (idx >= 0 && hoja < 0) return;` → cae la parte de "caja rota: no se mueve". Revertir.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Core/AnimatorComponent.h engine/src/Core/AnimatorComponent.cpp engine/tests/animator_tests.cpp
git commit -m "feat(animation): un estado puede contener a otros (parent, isSubMachine, subEntry)"
```

---

### Task 2: Runtime — entrar, salir y orden

**Files:**
- Modify: `engine/src/Core/AnimatorComponent.cpp` (bloque de selección de transición de `updateLayer`, ~1307-1336; `play`, ~1389; `crossFade`, ~1397)
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `isDescendantOf`, `resolveEntryLeaf`, los tres campos de `State` (Task 1).

- [ ] **Step 1: Escribir los tests que fallan**

```cpp
// Añade a makeCajas una transición Base -> Ataques y otra Ataques -> Base,
// ambas por trigger.
static AnimatorComponent makeCajasConTransiciones()
{
    AnimatorComponent a = makeCajas();
    a.addParameter("entra", AnimatorComponent::ParamType::Trigger);
    a.addParameter("sale",  AnimatorComponent::ParamType::Trigger);
    auto liga = [&](int from, int to, const char* trigger) {
        AnimatorComponent::Transition t;
        t.fromState = from; t.toState = to; t.duration = 0.0f;
        AnimatorComponent::Condition c;
        c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = trigger;
        t.conditions.push_back(c);
        a.addTransition(t);
    };
    liga(0, 1, "entra");    // hacia la caja
    liga(1, 0, "sale");     // desde la caja
    return a;
}

static void test_submachine_transition_enters_the_leaf()
{
    AnimatorComponent a = makeCajasConTransiciones();
    a.reset();
    CHECK(a.currentState() == 0);
    a.setTrigger("entra");
    a.update(0.016f, true);
    // Ataques -> Combo -> Uno: se entra en la hoja, no en la caja.
    CHECK(a.currentState() == 4);
}

static void test_submachine_broken_entry_does_not_fire()
{
    AnimatorComponent a = makeCajasConTransiciones();
    a.statesMutable()[3].subEntry = -1;    // Combo vacía
    a.reset();
    a.setTrigger("entra");
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);          // no se entra a medias
}

static void test_submachine_exit_fires_from_any_leaf()
{
    AnimatorComponent a = makeCajasConTransiciones();
    a.reset();
    a.setTrigger("entra");
    a.update(0.016f, true);
    CHECK(a.currentState() == 4);          // dentro, a dos niveles
    a.setTrigger("sale");
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);          // la transición DESDE la caja vale
    // Y no dispara desde fuera: estando en Base, "sale" no hace nada.
    a.setTrigger("sale");
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);
}

static void test_submachine_leaf_transition_wins_over_ancestor()
{
    AnimatorComponent a = makeCajasConTransiciones();
    AnimatorComponent::State s;
    s.name = "Otro";
    const int otro = a.addState(s);        // 5, en la raíz
    a.addParameter("ya", AnimatorComponent::ParamType::Bool);
    auto liga = [&](int from, int to) {
        AnimatorComponent::Transition t;
        t.fromState = from; t.toState = to; t.duration = 0.0f;
        AnimatorComponent::Condition c;
        c.type = AnimatorComponent::ConditionType::Bool; c.paramName = "ya"; c.expected = true;
        t.conditions.push_back(c);
        a.addTransition(t);
    };
    // La del ANCESTRO se declara antes que la de la hoja, a propósito: lo que
    // decide es el nivel, no el orden del vector.
    liga(1, 0);            // desde la caja -> Base
    liga(4, otro);         // desde la hoja -> Otro
    a.reset();
    a.setTrigger("entra");
    a.update(0.016f, true);
    CHECK(a.currentState() == 4);
    a.setBool("ya", true);
    a.update(0.016f, true);
    CHECK(a.currentState() == otro);       // gana la de la hoja
}

static void test_play_resolves_a_submachine()
{
    AnimatorComponent a = makeCajasConTransiciones();
    a.reset();
    CHECK(a.play("Ataques", 0));
    CHECK(a.currentState() == 4);
    // Caja vacía: no hay hoja a la que ir.
    AnimatorComponent b = makeCajasConTransiciones();
    b.statesMutable()[3].subEntry = -1;
    b.reset();
    CHECK(!b.play("Ataques", 0));
    CHECK(b.currentState() == 0);
}
```

Registrar los cinco en `main`. Antes de implementar, comprobar con `grep` las
firmas reales de `setBool`, `play(nombre, capa)`, `reset()` y `Condition::expected`.

- [ ] **Step 2: Compilar y ver que fallan** — los cinco FAIL (hoy `toState` a una caja deja el estado actual en la caja, y la transición desde la caja no dispara).

- [ ] **Step 3: Implementar**

En `updateLayer`, el grupo de transiciones del estado actual (hoy exige
`t.fromState != L.currentState`) pasa a recorrerse por niveles:

```cpp
        if (!elegida)
        {
            // Por NIVELES: primero las que salen de la hoja, luego las de su
            // caja, luego las de la caja de arriba. Así una salida general de un
            // bloque no le gana a una salida concreta de un estado solo porque
            // se declarara antes. Any State ya se ha mirado arriba y sigue
            // teniendo prioridad sobre todo esto.
            int nivel = L.currentState;
            for (int pasos = 0; nivel >= 0 && nivel < (int)L.states.size() && !elegida &&
                                pasos <= (int)L.states.size(); pasos++)
            {
                for (const auto& t : L.transitions)
                {
                    if (t.fromState != nivel) continue;
                    if (t.toState < 0 || t.toState >= (int)L.states.size()) continue;
                    // Entrar en una caja rota no dispara: mejor quedarse donde
                    // se está que en un estado que no existe.
                    if (resolveEntryLeaf(t.toState, li) < 0) continue;
                    if (transitionReady(t, n0, n1, conDuracion, li)) { elegida = &t; break; }
                }
                nivel = L.states[(size_t)nivel].parent;
            }
        }
```

El bucle de Any State gana la misma guarda, justo después de su comprobación de
rango:

```cpp
            if (resolveEntryLeaf(t.toState, li) < 0) continue;
```

`startTransitionTo` no cambia: `enterState` ya resuelve la caja a su hoja (Task
1), y por él pasan la transición elegida, `play` y `crossFade`.

`play` y `crossFade` pasan a rechazar una caja rota, que hoy devolverían true sin
moverse:

```cpp
    bool AnimatorComponent::play(const std::string& stateName, int layer)
    {
        const int idx = stateIndexByName(stateName, layer);
        if (idx < 0 || resolveEntryLeaf(idx, layer) < 0) return false;
        startTransitionTo(idx, 0.0f, layer);
        return true;
    }
```

y lo mismo en `crossFade`, antes de calcular `hayActual`.

- [ ] **Step 4: Compilar y ejecutar** — todo PASS, **incluidos** los tests de capas, Any State, exit time y eventos: este bloque es el que evalúa todas las transiciones del motor.

- [ ] **Step 5: Sabotajes (uno por llamada)**

1. Sustituir el recorrido por niveles por el bucle plano de antes (`t.fromState != L.currentState` y nada más) → cae `test_submachine_exit_fires_from_any_leaf`. Revertir.
2. Recorrer los niveles de la raíz hacia la hoja (empezar por el ancestro) → cae `test_submachine_leaf_transition_wins_over_ancestor`. Revertir.
3. Quitar `if (resolveEntryLeaf(t.toState, li) < 0) continue;` del grupo por niveles → cae `test_submachine_broken_entry_does_not_fire`. Revertir.
4. Quitar la guarda nueva de `play` → cae `test_play_resolves_a_submachine`. Revertir.

- [ ] **Step 6: Commit**

```bash
git add engine/src/Core/AnimatorComponent.cpp engine/tests/animator_tests.cpp
git commit -m "feat(animation): entrar en una caja resuelve su hoja y salir vale desde cualquier hoja"
```

---

### Task 3: Borrado y reindexado

**Files:**
- Modify: `engine/src/Core/AnimatorComponent.cpp` (`removeState`, ~44-92)
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `isDescendantOf`, los campos de `State` (Task 1).

- [ ] **Step 1: Escribir los tests que fallan**

```cpp
static void test_removing_a_submachine_removes_its_children()
{
    AnimatorComponent a = makeCajasConTransiciones();
    AnimatorComponent::State s;
    s.name = "Otro";
    a.addState(s);                       // 5, en la raíz, para ver que sobrevive
    a.removeState(1, 0);                 // borra Ataques
    // Quedan Base y Otro: Golpe, Combo y Uno se van con la caja.
    CHECK(a.states().size() == 2u);
    CHECK(a.states()[0].name == "Base");
    CHECK(a.states()[1].name == "Otro");
    // Y no queda ninguna transición apuntando a lo borrado.
    for (const auto& t : a.transitions())
    {
        CHECK(t.fromState >= -2 && t.fromState < (int)a.states().size());
        CHECK(t.toState   >= 0  && t.toState   < (int)a.states().size());
    }
}

static void test_removing_a_state_reindexes_parent_and_entry()
{
    AnimatorComponent a = makeCajas();
    a.removeState(0, 0);                 // borra Base, que va ANTES de las cajas
    // Ataques pasa a 0, Golpe a 1, Combo a 2, Uno a 3.
    CHECK(a.states().size() == 4u);
    CHECK(a.states()[0].name == "Ataques" && a.states()[0].subEntry == 2);
    CHECK(a.states()[1].name == "Golpe"   && a.states()[1].parent   == 0);
    CHECK(a.states()[2].name == "Combo"   && a.states()[2].parent   == 0);
    CHECK(a.states()[2].subEntry == 3);
    CHECK(a.states()[3].name == "Uno"     && a.states()[3].parent   == 2);
    // Y la jerarquía sigue significando lo mismo.
    CHECK(a.resolveEntryLeaf(0, 0) == 3);
}
```

Registrarlos en `main`.

- [ ] **Step 2: Compilar y ver que fallan** — el primero deja 5 estados (los hijos se quedan huérfanos) y el segundo deja `parent`/`subEntry` apuntando a otros estados.

- [ ] **Step 3: Implementar**

Al principio de `removeState`, antes de borrar nada:

```cpp
        // Los descendientes se van con la caja. Dejarlos sueltos en la raíz es
        // crear huérfanos que nadie ha pedido, y el undo es un snapshot del
        // grafo entero, así que deshacer los devuelve todos.
        //
        // La caja se relocaliza por editorId y NO por índice: borrar un
        // descendiente que iba antes que ella la desplaza, y seguir con el
        // índice viejo borraría a otro estado. El editorId es estable dentro de
        // la sesión (ver el comentario de addState).
        if (L.states[(size_t)idx].isSubMachine)
        {
            const int idCaja = L.states[(size_t)idx].editorId;
            for (;;)
            {
                const int caja = stateIndexByEditorId(idCaja, layer);
                if (caja < 0) return;               // ya no está: nada que borrar
                int hijo = -1;
                for (int i = 0; i < (int)L.states.size(); i++)
                    if (isDescendantOf(i, caja, layer)) { hijo = i; break; }
                if (hijo < 0) { idx = caja; break; }   // no quedan descendientes
                removeState(hijo, layer);              // recursivo: una caja hija se lleva los suyos
            }
        }
```

(`idx` es el parámetro de la función, así que reasignarlo aquí es lo que deja el
resto de `removeState` —el `erase`, el reindexado de transiciones y el playhead—
trabajando sobre la posición correcta de la caja.)

Y junto al reindexado de transiciones que ya existe:

```cpp
        for (auto& st : L.states)
        {
            if (st.parent   == idx) st.parent   = -1;
            else if (st.parent   > idx) st.parent--;
            if (st.subEntry == idx) st.subEntry = -1;
            else if (st.subEntry > idx) st.subEntry--;
        }
```

- [ ] **Step 4: Compilar y ejecutar** — todo PASS, incluidos los tests de borrado que ya existían.

- [ ] **Step 5: Sabotajes (uno por llamada)**

1. Quitar el bloque de borrado de descendientes → cae `test_removing_a_submachine_removes_its_children`. Revertir.
2. Quitar el reindexado de `parent`/`subEntry` → cae `test_removing_a_state_reindexes_parent_and_entry`. Revertir.
3. Cambiar `st.parent > idx` por `>=` → cae el mismo test de reindexado, por el otro lado. Revertir.

- [ ] **Step 6: Commit**

```bash
git add engine/src/Core/AnimatorComponent.cpp engine/tests/animator_tests.cpp
git commit -m "fix(animation): borrar una caja se lleva a sus descendientes y reindexa la jerarquia"
```

---

### Task 4: Serialización y validación al cargar

**Files:**
- Modify: `engine/src/Core/Scene.cpp` (`animatorToJson`, bloque de estados; `animatorFromJson`, lectura de estados)
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: los campos de `State` (Task 1), `isDescendantOf` (Task 1).

- [ ] **Step 1: Escribir el test que falla**

```cpp
static void test_submachine_serialization(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Bicho");
    const uint64_t id = go->id;
    go->setAnimator(std::make_shared<AnimatorComponent>(makeCajas()));

    const nlohmann::json ja = animatorToJson(*go->getAnimator());
    CHECK(ja["states"][1].contains("subMachine"));
    CHECK(ja["states"][2].contains("parent"));
    // Un estado normal no escribe ninguna clave nueva.
    CHECK(!ja["states"][0].contains("parent"));
    CHECK(!ja["states"][0].contains("subMachine"));
    CHECK(!ja["states"][0].contains("subEntry"));

    nlohmann::json j = scene.toJson();
    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    const auto& anim = *found->getAnimator();
    CHECK(anim.states().size() == 5u);
    if (anim.states().size() != 5u) return;
    CHECK(anim.states()[1].isSubMachine);
    CHECK(anim.states()[2].parent == 1);
    CHECK(anim.states()[1].subEntry == 3);
    CHECK(anim.resolveEntryLeaf(1, 0) == 4);

    // Un Animator sin cajas no escribe ninguna clave nueva.
    AnimatorComponent plano;
    AnimatorComponent::State s;
    s.name = "Solo";
    plano.addState(s);
    const std::string texto = animatorToJson(plano).dump();
    CHECK(texto.find("subMachine") == std::string::npos);
    CHECK(texto.find("parent") == std::string::npos);
}

static void test_submachine_bad_file_warns(PhysicsManager& pm, AudioManager& am)
{
    // parent fuera de rango, parent que no es caja, y un ciclo de contención.
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Bicho");
    go->setAnimator(std::make_shared<AnimatorComponent>(makeCajas()));
    nlohmann::json j = animatorToJson(*go->getAnimator());
    j["states"][2]["parent"] = 99;     // fuera de rango
    j["states"][4]["parent"] = 0;      // Base no es caja
    AnimatorComponent leido;
    std::vector<std::string> avisos;
    animatorFromJson(j, leido, &avisos);
    CHECK(leido.states()[2].parent == -1);
    CHECK(leido.states()[4].parent == -1);
    CHECK(avisos.size() >= 2u);

    // Ciclo: Ataques dentro de Combo y Combo dentro de Ataques.
    nlohmann::json c = animatorToJson(*go->getAnimator());
    c["states"][1]["parent"] = 3;
    AnimatorComponent ciclo;
    avisos.clear();
    animatorFromJson(c, ciclo, &avisos);
    CHECK(ciclo.resolveEntryLeaf(1, 0) >= -1);   // no cuelga
    CHECK(!avisos.empty());
    // Los implicados quedan en la raíz.
    CHECK(ciclo.states()[1].parent == -1 || ciclo.states()[3].parent == -1);
}
```

Registrar ambos en `main` junto a los otros tests que reciben `pm, am`. Si
`animatorToJson`/`animatorFromJson` no son visibles desde el test, hacer el
round-trip por `scene.toJson()` / `loaded.fromJson(...)` como el resto de tests
de escena, y comprobar las claves sobre el JSON del nodo.

- [ ] **Step 2: Compilar y ver que falla** — el JSON no trae `subMachine` ni `parent`.

- [ ] **Step 3: Implementar**

Escritura, en el objeto de cada estado, solo cuando no son el default:

```cpp
            if (st.parent >= 0)   js["parent"]     = st.parent;
            if (st.isSubMachine)  js["subMachine"] = true;
            if (st.subEntry >= 0) js["subEntry"]   = st.subEntry;
```

Lectura, tras leer el resto de campos del estado:

```cpp
            st.parent       = sj.value("parent", -1);
            st.isSubMachine = sj.value("subMachine", false);
            st.subEntry     = sj.value("subEntry", -1);
```

Y una pasada de validación **después** de cargar todos los estados de la capa,
que es cuando se puede comprobar a quién apuntan:

```cpp
        // La jerarquía se valida con todos los estados ya cargados: un parent
        // que no existe, que no es una caja, o un ciclo de contención, dejan al
        // estado en la raíz. Un ciclo colgaría los recorridos de jerarquía.
        for (int i = 0; i < (int)estados.size(); i++)
        {
            auto& st = estados[(size_t)i];
            if (st.parent >= (int)estados.size() || st.parent < -1)
            {
                if (warnings) warnings->push_back("Animator: el estado '" + st.name +
                                                  "' tiene un padre fuera de rango; se deja en la raiz");
                st.parent = -1;
            }
            else if (st.parent >= 0 && !estados[(size_t)st.parent].isSubMachine)
            {
                if (warnings) warnings->push_back("Animator: el padre del estado '" + st.name +
                                                  "' no es una sub-maquina; se deja en la raiz");
                st.parent = -1;
            }
            if (st.subEntry >= (int)estados.size() || st.subEntry < -1) st.subEntry = -1;
        }
        // Ciclos: subir desde cada estado con un tope; si se pasa, ese estado a
        // la raíz.
        for (int i = 0; i < (int)estados.size(); i++)
        {
            int p = estados[(size_t)i].parent;
            int pasos = 0;
            while (p >= 0 && pasos <= (int)estados.size()) { p = estados[(size_t)p].parent; pasos++; }
            if (pasos > (int)estados.size())
            {
                if (warnings) warnings->push_back("Animator: ciclo de sub-maquinas en '" +
                                                  estados[(size_t)i].name + "'; se deja en la raiz");
                estados[(size_t)i].parent = -1;
            }
        }
```

(adaptar `estados` al nombre real del vector local del lector, y hacerlo por capa:
el lector ya distingue la capa base de `"layers"`.)

- [ ] **Step 4: Compilar y ejecutar** — todo PASS, incluidos los tests de escena de las filas anteriores.

- [ ] **Step 5: Sabotajes (uno por llamada)**

1. Escribir siempre `js["parent"]` (aunque sea −1) → cae la parte de "un Animator sin cajas no escribe ninguna clave nueva". Revertir.
2. Quitar la comprobación de `parent` fuera de rango → cae `test_submachine_bad_file_warns`. Revertir.
3. Quitar la pasada de ciclos → cae la parte del ciclo del mismo test. Revertir.

- [ ] **Step 6: Commit**

```bash
git add engine/src/Core/Scene.cpp engine/tests/animator_tests.cpp
git commit -m "feat(animation): las sub-maquinas se guardan, se cargan y se validan"
```

---

### Task 5: Canvas, propiedades del estado y documentación

**Files:**
- Modify: `engine/src/Editor/AnimatorPanel.cpp` (dibujo de nodos y links del grafo; propiedades del estado seleccionado; barra de la capa)
- Modify: `engine/include/DonTopo/Editor/AnimatorPanel.h` (nivel actual)
- Modify: `README.md` (sección del Animator)
- Modify: `docs/animation-audit.md` (C10 y fila 15)

**Interfaces:**
- Consumes: `isDescendantOf`, `resolveEntryLeaf`, los campos de `State` (Task 1).
- Produces: `int m_nivel = -1;` en el panel (estado que se está mirando por dentro; −1 = raíz de la capa).

- [ ] **Step 1: Nivel actual y filtrado de nodos**

En `AnimatorPanel.h`, junto a los demás miembros de estado del panel:

```cpp
    // Sub-máquina que se está mirando por dentro; -1 es la raíz de la capa.
    // Se resetea al cambiar de capa o de objeto: su índice no significa nada
    // en otro grafo.
    int m_nivel = -1;
```

En el bucle que dibuja los nodos del grafo, saltar los que no son de este nivel:

```cpp
            // Solo lo de este nivel: los hijos de una caja se ven al entrar en
            // ella.
            if (st.parent != m_nivel) continue;
```

Y al cambiar de capa o de GameObject, `m_nivel = -1` (buscar dónde se resetea hoy
el estado del panel al cambiar de objeto y añadirlo ahí).

- [ ] **Step 2: Breadcrumb y navegación**

Encima del canvas, en la misma barra que la capa:

```cpp
        // Breadcrumb: Base / Ataques / Combo. Cada trozo sube a ese nivel.
        if (ImGui::SmallButton("Base###nivelRaiz")) m_nivel = -1;
        {
            std::vector<int> cadena;
            for (int n = m_nivel; n >= 0 && n < (int)anim->states().size();
                 n = anim->states()[(size_t)n].parent)
                cadena.push_back(n);
            for (int k = (int)cadena.size() - 1; k >= 0; k--)
            {
                ImGui::SameLine();
                ImGui::TextUnformatted("/");
                ImGui::SameLine();
                ImGui::PushID(cadena[(size_t)k]);
                if (ImGui::SmallButton(anim->states()[(size_t)cadena[(size_t)k]].name.c_str()))
                    m_nivel = cadena[(size_t)k];
                ImGui::PopID();
            }
        }
```

Entrar en una caja: en el manejo de doble clic del canvas (`ed::GetDoubleClickedNode`),
si el nodo es una caja, `m_nivel = índice`.

- [ ] **Step 3: Links contra el ancestro visible**

Al dibujar cada transición, resolver los dos extremos al nodo visible en este
nivel:

```cpp
        // Un extremo que vive dentro de otra caja se dibuja contra la caja: si
        // no, la transición desaparecería del grafo y parecería que no existe.
        auto visibleEnEsteNivel = [&](int estado) {
            int s = estado;
            while (s >= 0 && s < (int)anim->states().size() && anim->states()[(size_t)s].parent != m_nivel)
                s = anim->states()[(size_t)s].parent;
            return s;
        };
```

y saltar el link cuando los dos extremos resuelven al mismo nodo, o cuando alguno
resuelve a −1 (está fuera de esta rama). Any State (`fromState == -2`) se dibuja
como hoy.

- [ ] **Step 4: Crear cajas y elegir padre/entrada**

Junto al botón de añadir estado:

```cpp
        if (ImGui::Button("Add Sub-State Machine"))
        {
            AnimatorComponent::State caja;
            caja.name         = "Sub-Machine";
            caja.isSubMachine = true;
            caja.parent       = m_nivel;   // se crea en el nivel que se está viendo
            anim->addState(caja, capa);
        }
```

En las propiedades del estado seleccionado, un combo **Padre** con la raíz y las
cajas que no son descendientes suyas (ni él mismo):

```cpp
            const char* etiquetaPadre = st.parent >= 0 ? anim->states()[(size_t)st.parent].name.c_str()
                                                       : "(raiz)";
            if (ImGui::BeginCombo("Padre###padre", etiquetaPadre))
            {
                if (ImGui::Selectable("(raiz)", st.parent < 0)) st.parent = -1;
                for (int i = 0; i < (int)anim->states().size(); i++)
                {
                    if (!anim->states()[(size_t)i].isSubMachine) continue;
                    if (i == sel || anim->isDescendantOf(i, sel, capa)) continue;   // no dentro de sí mismo
                    if (ImGui::Selectable(anim->states()[(size_t)i].name.c_str(), st.parent == i))
                        st.parent = i;
                }
                ImGui::EndCombo();
            }
```

Y, solo si el estado es una caja, un combo **Entrada** con sus hijos directos, que
escribe `subEntry`. Una caja no muestra clip, blend, velocidad ni clip de
propiedades: esas secciones se saltan con `if (st.isSubMachine) ...`.

- [ ] **Step 5: Compilar y probar en el editor**

`.\build.bat` (exit 0) y abrir el Sandbox **desde la GUI**. Comprobar: crear una
caja, meterle dos estados con el combo, marcar su entrada, entrar con doble clic,
volver con el breadcrumb, y que una transición contra un estado de dentro se
dibuja contra la caja.

- [ ] **Step 6: Documentar**

En `README.md`, sección del Animator:

```markdown
A state can also be a **sub-state machine**: a box that holds other states. It
never plays — entering it enters its **entry** state, following the chain down to
a leaf — and a transition drawn *from* the box fires from any state inside it, at
any depth. Transitions are evaluated Any State first, then the current state's
own, then its box's, then the box above it: a general exit never beats a specific
one. Deleting a box deletes what it holds (one undo step). Double-click a box to
go in, and the breadcrumb above the canvas to come back out. It is a way to
organise a graph, not Unity's hierarchical state machine: there is no active
compound state.
```

En `docs/animation-audit.md`: C10 a `✅ **EXISTE** (2026-09-20, <rango>, fila 15)`
con `State::parent/isSubMachine/subEntry` y `resolveEntryLeaf` como evidencia, y
la fila 15 cerrada (ya no queda nada pendiente en ella).

- [ ] **Step 7: Commit**

```bash
git add engine/src/Editor/AnimatorPanel.cpp engine/include/DonTopo/Editor/AnimatorPanel.h README.md docs/animation-audit.md
git commit -m "feat(editor): navegacion por sub-maquinas en el canvas del Animator"
```

- [ ] **Step 8: Verificación manual (usuario)**

En Vulkan **y** D3D12, con el proyecto abierto desde la GUI: crear una caja,
meterle dos estados, marcar la entrada, transicionar desde fuera hacia la caja y
desde la caja hacia fuera, navegar con el breadcrumb, borrar la caja y ver que el
grafo queda coherente, guardar y recargar la escena. Solo tras el visto bueno:
`git merge --no-ff` a main, push y borrar la rama.
