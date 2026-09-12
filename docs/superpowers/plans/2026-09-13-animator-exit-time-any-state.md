# Exit time y Any State en el Animator — plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Que una transición del Animator pueda dispararse por tiempo (exit time, como Unity) y que existan transiciones desde Any State, con serialización, undo y UI.

**Architecture:** Tres campos nuevos al final de `Transition` (`hasExitTime`, `exitTime`, `canTransitionToSelf`) y un centinela `kAnyState = -2` en `fromState`. `update` avanza un reloj normalizado acumulado que se reinicia en un único `enterState`, evalúa primero las transiciones Any State y después las del estado actual. La serialización y el undo pasan por el `animatorToJson` que ya existe; el panel dibuja un nodo Any State con ids reservados.

**Tech Stack:** C++20, MSVC + Ninja, nlohmann::json, Dear ImGui + imgui-node-editor. Tests: ejecutables `dt_*.exe` con `CHECK` propio, sin framework.

**Spec:** `docs/superpowers/specs/2026-09-13-animator-exit-time-any-state-design.md` (commit `b4c152f`). Léela antes de empezar: este plan argumenta desde ella, y las fórmulas del exit time están ahí.

## Global Constraints

- **Build**: SIEMPRE con la herramienta **PowerShell**, desde la raíz del repo: `& .\build.bat`. Nunca `cmake`/`ninja` a pelo desde Bash.
- **Tests**: con la herramienta **Bash**, desde la **raíz del repo** (`cd /c/Users/ruben/Documents/Don_Topo_Engine`): `./build-ninja/engine/tests/dt_animator_tests.exe`. Desde otro directorio revienta con exit 139 **sin imprimir nada**. Éxito: `dt_animator_tests: OK`.
- **Suite completa** antes de cada commit, desde la raíz: `for exe in build-ninja/engine/tests/dt_*.exe; do "$exe" >/dev/null 2>&1 || echo "FALLA $exe"; done`. Estado de partida: 28/28. `dt_audio_tests` tiene un fallo intermitente conocido (~1/29): si sale, capturar su salida ANTES de repetir y decirlo.
- **Ficheros grandes**: `AnimatorComponent.cpp` (~650 l.), `Scene.cpp` (~3800), `AnimatorPanel.cpp` (~1020), `animator_tests.cpp` (~5100). Grep + lecturas de rango, nunca enteros. Los anclajes de este plan son de `main` en `b4c152f`: confírmalos con un Read estrecho antes de editar.
- **Edición**: solo Edit/Write. Nunca `sed -i`, `python -c` con código, ni `Get-Content`/`Set-Content` de PowerShell: el repo va en CRLF y tiene comentarios con acentos.
- **Finales de línea**: antes de cada commit, `git diff --stat` y `git diff --ignore-cr-at-eol --stat` idénticos.
- **Commits**: mensaje escrito con Write en un fichero del scratchpad y `git commit -F <fichero>` desde Bash. Termina EXACTAMENTE con: `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>` (nunca tu propio nombre de modelo). Nunca `--no-verify`.
- **Sabotajes**: de uno en uno — aplicar, compilar (mirando que el BUILD dé exit 0), ejecutar, citar las líneas `FAIL`, restaurar, recompilar, confirmar OK. **Commitea antes de sabotear** para poder restaurar con `git checkout -- <fichero>`. Si un sabotaje no tumba ninguno de los tests que dice, o tumba uno inesperado, PARA y dilo en el informe: no inventes otro.
- **Scripts de sabotaje en PowerShell**: nada de `$ErrorActionPreference = "Stop"` (el aviso de `vswhere` de `build.bat` por stderr lo mata a mitad y deja el sabotaje puesto); llamar al build con `cmd /c ".\build.bat > nul 2>&1"` y mirar `$LASTEXITCODE`. Pares de reemplazo como tablas hash, no `@(@("a","b"))` (se aplana).
- **Estilo**: comentarios en español con la densidad del fichero; `Core/` indenta dentro de `namespace DonTopo`, `Editor/` no. README en inglés.
- **Undo del editor**: no hay que tocarlo. El tracker compara con `animatorToJson`, así que cualquier campo que se serialice entra solo. Lo que sí hay que mantener es `graphMutations()` en los tests.
- **Paridad Vulkan/D3D12**: no hay código de backend en este plan. Se verifica a mano en la tarea 5.

## Mapa de ficheros

| Fichero | Tareas |
|---|---|
| `engine/include/DonTopo/Core/AnimatorComponent.h`, `engine/src/Core/AnimatorComponent.cpp` | 1, 2, 3 |
| `engine/src/Core/Scene.cpp` (`animatorToJson`, `animatorFromJson`, `animatorGraphKey`) | 3 |
| `engine/src/Editor/AnimatorPanel.cpp` | 4 |
| `engine/tests/animator_tests.cpp` | 1, 2, 3 |
| `README.md` (`## Animator`) | 4 |

Orden: 1 → 2 → 3 → 4. Cada tarea deja la suite en verde.

---

### Task 1: exit time en Core

**Files:**
- Modify: `engine/include/DonTopo/Core/AnimatorComponent.h` (`struct Transition`, `:53-69`; parte privada, `:315-345`)
- Modify: `engine/src/Core/AnimatorComponent.cpp` (`removeState` `:31-78`, `applyGraph` `:150-243`, `resetPlayback` `:406-419`, `conditionsMet` `:495-528`, `update` `:571-641`)
- Test: `engine/tests/animator_tests.cpp` (antes de `int main()`, `:4925`; llamadas antes de `am.shutdown();`, `:5088`)

**Interfaces:**
- Produces:
  - `bool Transition::hasExitTime = false;` `float Transition::exitTime = 1.0f;` `bool Transition::canTransitionToSelf = false;` (los tres al final del struct)
  - `static constexpr int AnimatorComponent::kAnyState = -2;` (público)
  - privados: `void enterState(int idx);`, `bool transitionReady(const Transition& t, double n0, double n1, bool hasDuration) const;`, `static bool exitTimeCrossed(double n0, double n1, float exitTime);`, `double m_stateTicks = 0.0;`

**Nota:** `canTransitionToSelf` y `kAnyState` se declaran en esta tarea pero no se usan hasta la 2; así el struct cambia una sola vez.

- [ ] **Step 1: Write the failing tests**

En `animator_tests.cpp`, antes de `int main()` (`makeTimedState` ya existe: 100 ticks a 10 ticks/s, así que un segundo de `update` sube el tiempo normalizado 0.1 y una vuelta son 10 s):

```cpp
// ---- Exit time ----

// A -> B con exit time, y opcionalmente una condición bool "ok". Playhead en
// la entrada SIN evaluar transiciones (update(0, false)): con exitTime 0 un
// update(0, true) ya dispararía.
static AnimatorComponent makeExitTimeGraph(float exitTime, bool conCondicion)
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    a.addState(makeTimedState("B"));
    a.addParameter("ok", AnimatorComponent::ParamType::Bool);
    AnimatorComponent::Transition t;
    t.fromState   = 0;
    t.toState     = 1;
    t.hasExitTime = true;
    t.exitTime    = exitTime;
    if (conCondicion)
    {
        AnimatorComponent::Condition c;
        c.type = AnimatorComponent::ConditionType::Bool; c.paramName = "ok"; c.expected = true;
        t.conditions.push_back(c);
    }
    a.addTransition(t);
    a.update(0.0f, false);
    return a;
}

// Exit time < 1 sin condiciones: dispara en el frame que CRUZA el 0.9, no antes.
static void test_exit_time_below_one_fires_when_crossed()
{
    AnimatorComponent a = makeExitTimeGraph(0.9f, false);
    a.update(8.5f, true);                 // N = 0.85
    CHECK(a.currentStateName() == "A");
    a.update(1.0f, true);                 // N = 0.95: cruza 0.9
    CHECK(a.currentStateName() == "B");
}

// Con condición, se mira SOLO en el frame del cruce: si no se cumple ahí,
// espera a la vuelta siguiente aunque se cumpla justo después.
static void test_exit_time_below_one_checks_conditions_only_at_the_crossing()
{
    AnimatorComponent a = makeExitTimeGraph(0.9f, true);
    a.update(9.5f, true);                 // N = 0.95: cruza con ok == false
    CHECK(a.currentStateName() == "A");
    a.setBool("ok", true);
    a.update(0.1f, true);                 // N = 0.96: el 0.9 de esta vuelta ya pasó
    CHECK(a.currentStateName() == "A");
    a.update(8.5f, true);                 // N = 1.81: todavía no llega a 1.9
    CHECK(a.currentStateName() == "A");
    a.update(1.0f, true);                 // N = 1.91: cruza el 0.9 de la segunda vuelta
    CHECK(a.currentStateName() == "B");
}

// Un dt grande que da la vuelta pasando por el 0.9 también es un cruce. Una
// regla que mirara solo la fase de la vuelta (0.95 -> 0.92) no lo vería.
static void test_exit_time_below_one_fires_when_a_big_dt_wraps_past_it()
{
    AnimatorComponent a = makeExitTimeGraph(0.9f, true);
    a.update(9.5f, true);                 // N = 0.95, cruza con ok == false
    CHECK(a.currentStateName() == "A");
    a.setBool("ok", true);
    a.update(9.7f, true);                 // N = 1.92 de golpe: pasa por 1.9
    CHECK(a.currentStateName() == "B");
}

// Exit time >= 1 cuenta vueltas: 2.5 no dispara con 2 vueltas.
static void test_exit_time_above_one_counts_loops()
{
    AnimatorComponent a = makeExitTimeGraph(2.5f, false);
    a.update(20.0f, true);                // N = 2.0
    CHECK(a.currentStateName() == "A");
    a.update(5.0f, true);                 // N = 2.5
    CHECK(a.currentStateName() == "B");
}

// Sin condiciones y sin exit time no dispara nunca (lo de siempre); con exit
// time, sí.
static void test_transition_without_conditions_needs_exit_time()
{
    AnimatorComponent sinExit;
    sinExit.addState(makeTimedState("A"));
    sinExit.addState(makeTimedState("B"));
    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    sinExit.addTransition(t);
    sinExit.update(0.0f, false);
    sinExit.update(50.0f, true);
    CHECK(sinExit.currentStateName() == "A");

    AnimatorComponent conExit = makeExitTimeGraph(0.5f, false);
    conExit.update(6.0f, true);
    CHECK(conExit.currentStateName() == "B");
}

// Un clip de duración 0 (o sin resolver) no tiene tiempo normalizado: el exit
// time cuenta como alcanzado en el primer update.
static void test_exit_time_on_zero_duration_state_is_reached_at_once()
{
    AnimatorComponent a;
    AnimatorComponent::State cero = makeTimedState("A");
    cero.duration = 0.0f;
    a.addState(cero);
    a.addState(makeTimedState("B"));
    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1; t.hasExitTime = true; t.exitTime = 0.9f;
    a.addTransition(t);
    a.update(0.0f, false);
    a.update(0.016f, true);
    CHECK(a.currentStateName() == "B");
}

// El reloj acumulado se reinicia al entrar en un estado por una transición:
// 1.5 vueltas cuentan desde que se entró en B, no desde que arrancó el grafo.
static void test_exit_time_clock_restarts_when_entering_a_state()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    a.addState(makeTimedState("B"));
    a.addState(makeTimedState("C"));
    a.addParameter("go", AnimatorComponent::ParamType::Trigger);
    AnimatorComponent::Transition ab;
    ab.fromState = 0; ab.toState = 1;
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = "go";
    ab.conditions.push_back(c);
    a.addTransition(ab);
    AnimatorComponent::Transition bc;
    bc.fromState = 1; bc.toState = 2; bc.hasExitTime = true; bc.exitTime = 1.5f;
    a.addTransition(bc);
    a.update(0.0f, false);

    a.update(12.0f, true);                // 1.2 vueltas en A
    a.setTrigger("go");
    a.update(0.0f, true);                 // entra en B
    CHECK(a.currentStateName() == "B");
    a.update(14.0f, true);                // 1.4 en B (2.6 si el reloj no se reiniciara)
    CHECK(a.currentStateName() == "B");
    a.update(1.0f, true);                 // 1.5
    CHECK(a.currentStateName() == "C");
}

// Lo mismo por el otro camino que reinicia el playhead: setEntryState pasa
// por resetPlayback.
static void test_exit_time_clock_restarts_on_reset_playback()
{
    AnimatorComponent a = makeExitTimeGraph(1.5f, false);
    a.update(12.0f, true);                // 1.2 vueltas en A
    CHECK(a.currentStateName() == "A");
    a.setEntryState(0);                   // resetPlayback
    a.update(5.0f, true);                 // 0.5 (1.7 si no se reiniciara)
    CHECK(a.currentStateName() == "A");
}
```

Y en `main()`, antes de `am.shutdown();`:

```cpp
    test_exit_time_below_one_fires_when_crossed();
    test_exit_time_below_one_checks_conditions_only_at_the_crossing();
    test_exit_time_below_one_fires_when_a_big_dt_wraps_past_it();
    test_exit_time_above_one_counts_loops();
    test_transition_without_conditions_needs_exit_time();
    test_exit_time_on_zero_duration_state_is_reached_at_once();
    test_exit_time_clock_restarts_when_entering_a_state();
    test_exit_time_clock_restarts_on_reset_playback();
```

- [ ] **Step 2: Run to verify it fails**

PowerShell: `& .\build.bat`
Expected: error de compilación, `'hasExitTime': is not a member of 'DonTopo::AnimatorComponent::Transition'`.

- [ ] **Step 3: Implement — header**

En `struct Transition`, después de `float duration = 0.0f;`:

```cpp
                // --- Exit time (como Unity) ---
                // Con hasExitTime la transición espera a que el estado de
                // origen llegue a exitTime, en tiempo NORMALIZADO (1 = fin del
                // clip). Por debajo de 1 se comprueba en cada vuelta; a partir
                // de 1 cuenta vueltas acumuladas (2.5 = dos vueltas y media).
                // Sin condiciones basta el tiempo; con condiciones hacen falta
                // las dos cosas. Al final del struct y apagado por defecto: es
                // lo que traen las escenas guardadas sin estos campos.
                bool  hasExitTime = false;
                float exitTime    = 1.0f;
                // Solo se lee en transiciones que salen de Any State: si puede
                // volver al estado en el que ya se está. Apagado por defecto:
                // encendido y con un bool, reiniciaría el estado cada frame.
                bool  canTransitionToSelf = false;
```

En la parte pública, justo después de `enum class Compare { ... };`:

```cpp
            // fromState de una transición que sale de "Any State": vale desde
            // cualquier estado. Negativo a propósito: removeState solo
            // reindexa índices >= 0, así que el centinela sobrevive intacto.
            static constexpr int kAnyState = -2;
```

En la parte privada, junto a `bool conditionsMet(const Transition& t) const;`:

```cpp
            // Único punto de entrada a un estado: fija el actual y pone a 0 su
            // reloj, su finished y el reloj normalizado acumulado. Todo camino
            // que reinicie el playhead pasa por aquí, para que el reloj del
            // exit time no quede colgado en el que se olvide.
            void enterState(int idx);
            // Si la transición puede disparar este frame. n0/n1: tiempo
            // normalizado acumulado del estado actual antes y después de
            // avanzar el reloj. hasDuration false = clip de duración 0 o sin
            // resolver, donde el exit time cuenta como alcanzado.
            bool transitionReady(const Transition& t, double n0, double n1, bool hasDuration) const;
            // La regla del exit time, aislada para que se lea en un sitio.
            static bool exitTimeCrossed(double n0, double n1, float exitTime);
```

Y junto a los miembros de runtime, después de `bool m_finished = false;`:

```cpp
            // Ticks avanzados desde que se entró en el estado actual, SIN fmod:
            // en un loop sigue creciendo, que es lo que permite contar vueltas
            // para el exit time. double y no float: en una sesión larga un
            // float pierde resolución para decidir un cruce. No se serializa.
            double                  m_stateTicks   = 0.0;
```

- [ ] **Step 4: Implement — .cpp**

1. Añadir, justo antes de `void AnimatorComponent::removeState(int idx)`:

```cpp
    void AnimatorComponent::enterState(int idx)
    {
        m_currentState = idx;
        m_animTime     = 0.0f;
        m_finished     = false;
        m_stateTicks   = 0.0;
    }

```

2. En `removeState`, sustituir el bloque

```cpp
        if (m_currentState == idx)
        {
            m_currentState = m_entryState;
            m_animTime     = 0.0f;
            m_finished     = false;
        }
```

por

```cpp
        if (m_currentState == idx)
        {
            enterState(m_entryState);
        }
```

3. En `applyGraph`, en la rama que cae a la entrada, sustituir

```cpp
        else
        {
            m_currentState = m_entryState;
            m_animTime     = 0.0f;
            m_finished     = false;
        }
```

por

```cpp
        else
        {
            enterState(m_entryState);
        }
```

4. En `resetPlayback`, sustituir las tres líneas

```cpp
        m_currentState  = m_entryState;
        m_animTime      = 0.0f;
        m_finished      = false;
```

por

```cpp
        enterState(m_entryState);
```

5. Justo después de la función `conditionsMet` completa, añadir:

```cpp
    bool AnimatorComponent::exitTimeCrossed(double n0, double n1, float exitTime)
    {
        const double e = exitTime;
        // A partir de 1 cuenta vueltas acumuladas: listo en cualquier frame
        // que ya las haya dado.
        if (e >= 1.0) return n1 >= e;
        // Primer update tras entrar con exitTime 0: el inicio de la vuelta
        // cero es un cruce, o no dispararía hasta la segunda.
        if (e == 0.0 && n0 == 0.0) return true;
        // Por debajo de 1, en cada vuelta: ¿hay un entero k con
        // n0 < k + e <= n1? Cubre también el dt que da la vuelta cruzando e.
        return std::floor(n1 - e) > std::floor(n0 - e);
    }

    bool AnimatorComponent::transitionReady(const Transition& t, double n0, double n1,
                                            bool hasDuration) const
    {
        if (t.hasExitTime)
        {
            if (hasDuration && !exitTimeCrossed(n0, n1, t.exitTime)) return false;
            // Solo por tiempo: no hace falta ninguna condición.
            if (t.conditions.empty()) return true;
        }
        // Sin exit time y sin condiciones, conditionsMet devuelve false: una
        // transición así no dispara nunca, como siempre.
        return conditionsMet(t);
    }
```

6. Sustituir la función `update` COMPLETA (de `void AnimatorComponent::update(float dt, bool evaluateTransitions)` a su llave de cierre) por:

```cpp
    void AnimatorComponent::update(float dt, bool evaluateTransitions)
    {
        if (m_currentState < 0 || m_currentState >= (int)m_states.size())
        {
            m_currentState = m_entryState;
            if (m_currentState < 0 || m_currentState >= (int)m_states.size()) return;
        }

        const State& actual = m_states[m_currentState];
        const bool   conDuracion = actual.duration > 0.0f && actual.ticksPerSecond > 0.0f;
        const double ticks0 = m_stateTicks;
        if (conDuracion)
            m_stateTicks += (double)dt * actual.ticksPerSecond;
        advanceClock(actual, m_animTime, &m_finished, dt);

        // Cross-fade en curso: el estado que se apaga sigue animándose con SU
        // ritmo y SU loop mientras dura la mezcla. Congelarlo daría un salto
        // visible justo al empezar la transición, que es lo contrario de lo que
        // el cross-fade viene a resolver.
        if (m_prevState >= 0)
        {
            if (m_prevState < (int)m_states.size())
                advanceClock(m_states[m_prevState], m_prevAnimTime, nullptr, dt);

            m_blendElapsed += dt;
            if (m_blendDuration <= 0.0f || m_blendElapsed >= m_blendDuration)
            {
                // Mezcla terminada: el destino se queda solo. A partir de aquí
                // blendWeight() vuelve a valer 1 por el camino de siempre.
                m_prevState     = -1;
                m_prevAnimTime  = 0.0f;
                m_blendElapsed  = 0.0f;
                m_blendDuration = 0.0f;
            }
        }

        if (!evaluateTransitions) return;

        const double n0 = conDuracion ? ticks0 / actual.duration : 0.0;
        const double n1 = conDuracion ? m_stateTicks / actual.duration : 0.0;

        // Orden de declaración: la primera lista, gana. Determinista y sin
        // prioridades explícitas que mantener.
        for (const auto& t : m_transitions)
        {
            if (t.fromState != m_currentState) continue;
            if (t.toState < 0 || t.toState >= (int)m_states.size()) continue;
            if (!transitionReady(t, n0, n1, conDuracion)) continue;

            consumeTriggers(t);

            if (t.duration > 0.0f)
            {
                // El estado que dejamos pasa a ser el que se apaga, con el
                // tiempo que llevara. Si YA había una mezcla en vuelo se
                // descarta: mezclar tres clips necesitaría un tercer bloque en
                // el SSBO y en el push constant, así que la mezcla anterior se
                // corta aquí (mismo criterio que Unity con su capa base).
                m_prevState     = m_currentState;
                m_prevAnimTime  = m_animTime;
                m_blendElapsed  = 0.0f;
                m_blendDuration = t.duration;
            }
            else
            {
                // Corte seco: ni estado previo ni mezcla, el camino de siempre.
                m_prevState     = -1;
                m_prevAnimTime  = 0.0f;
                m_blendElapsed  = 0.0f;
                m_blendDuration = 0.0f;
            }

            enterState(t.toState);
            return;                  // una transición por update
        }
    }
```

(`<cmath>` ya está incluido para `std::floor`.)

- [ ] **Step 5: Run the tests to verify they pass**

PowerShell: `& .\build.bat` · Bash: `./build-ninja/engine/tests/dt_animator_tests.exe` → `dt_animator_tests: OK`.

- [ ] **Step 6: Full suite and commit**

Suite completa (28/28). Commit: `feat(animator): exit time en las transiciones` + Co-Authored-By.

- [ ] **Step 7: Sabotage, one at a time** (el commit anterior es la red: restaurar con `git checkout -- engine/src/Core/AnimatorComponent.cpp`)

| # | Sabotaje | Valor observable que cambia | Debe caer |
|---|---|---|---|
| 1 | En `exitTimeCrossed`, `std::floor(n1 - e)` → `std::floor(n0 - e)` | nunca hay cruce por debajo de 1 | `test_exit_time_below_one_fires_when_crossed` (entre otros de exit time < 1) |
| 2 | En `exitTimeCrossed`, cambiar la última línea por `return n1 >= e;` | dispara con N = 0.96 aunque el 0.9 ya pasara | `test_exit_time_below_one_checks_conditions_only_at_the_crossing` |
| 3 | Cambiar la última línea por `{ const double f0 = n0 - std::floor(n0), f1 = n1 - std::floor(n1); return f0 < e && f1 >= e; }` | un dt que da la vuelta (0.95 → 1.92) no se ve | `test_exit_time_below_one_fires_when_a_big_dt_wraps_past_it` y **solo** ese |
| 4 | Quitar `if (e >= 1.0) return n1 >= e;` | con 2.5 dispara a las 2 vueltas | `test_exit_time_above_one_counts_loops` |
| 5 | En `transitionReady`, primera línea `if (t.conditions.empty()) return true;` | la transición sin nada dispara | `test_transition_without_conditions_needs_exit_time` |
| 6 | En `transitionReady`, `if (hasDuration && !exitTimeCrossed(...))` → `if (!exitTimeCrossed(...))` | con duración 0 nunca cruza | `test_exit_time_on_zero_duration_state_is_reached_at_once` |
| 7 | Quitar `m_stateTicks = 0.0;` de `enterState` | B hereda las vueltas de A | `test_exit_time_clock_restarts_when_entering_a_state` (y el de resetPlayback) |
| 8 | En `resetPlayback`, volver a las tres líneas viejas (`m_currentState = m_entryState; m_animTime = 0.0f; m_finished = false;`) en vez de `enterState` | resetPlayback no reinicia el reloj | `test_exit_time_clock_restarts_on_reset_playback` y **solo** ese |

---

### Task 2: Any State en `update`

**Files:**
- Modify: `engine/src/Core/AnimatorComponent.cpp` (`update`, la reescrita en la tarea 1)
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `AnimatorComponent::kAnyState`, `Transition::canTransitionToSelf`, `transitionReady`, `enterState` (tarea 1).

- [ ] **Step 1: Write the failing tests**

```cpp
// ---- Any State ----

static AnimatorComponent::Condition triggerCond(const char* nombre)
{
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::Trigger;
    c.paramName = nombre;
    return c;
}

// Una transición Any State dispara desde cualquier estado.
static void test_any_state_fires_from_every_state()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    a.addState(makeTimedState("B"));
    a.addState(makeTimedState("C"));
    a.addParameter("hit", AnimatorComponent::ParamType::Trigger);
    AnimatorComponent::Transition t;
    t.fromState = AnimatorComponent::kAnyState; t.toState = 2;
    t.conditions.push_back(triggerCond("hit"));
    a.addTransition(t);

    a.update(0.0f, false);                // en A
    a.setTrigger("hit");
    a.update(0.016f, true);
    CHECK(a.currentStateName() == "C");

    a.setEntryState(1);                   // ahora en B
    a.setTrigger("hit");
    a.update(0.016f, true);
    CHECK(a.currentStateName() == "C");
}

// Any State va antes que las transiciones del estado actual, aunque la del
// estado esté declarada antes.
static void test_any_state_wins_over_the_current_state_transition()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    a.addState(makeTimedState("B"));
    a.addState(makeTimedState("C"));
    a.addParameter("go", AnimatorComponent::ParamType::Trigger);
    AnimatorComponent::Transition propia;
    propia.fromState = 0; propia.toState = 1;
    propia.conditions.push_back(triggerCond("go"));
    a.addTransition(propia);
    AnimatorComponent::Transition any;
    any.fromState = AnimatorComponent::kAnyState; any.toState = 2;
    any.conditions.push_back(triggerCond("go"));
    a.addTransition(any);

    a.update(0.0f, false);
    a.setTrigger("go");
    a.update(0.016f, true);
    CHECK(a.currentStateName() == "C");
}

// Con canTransitionToSelf apagado, Any State no reentra al estado actual: si
// lo hiciera con un bool, el estado se reiniciaría cada frame.
static void test_any_state_does_not_reenter_the_current_state_by_default()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    a.addState(makeTimedState("B"));
    a.addParameter("b", AnimatorComponent::ParamType::Bool);
    AnimatorComponent::Transition t;
    t.fromState = AnimatorComponent::kAnyState; t.toState = 0;
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::Bool; c.paramName = "b"; c.expected = true;
    t.conditions.push_back(c);
    a.addTransition(t);

    a.update(0.0f, false);
    a.setBool("b", true);
    a.update(1.0f, true);                 // si reentrara, animTime volvería a 0
    CHECK(a.currentStateName() == "A");
    CHECK(nearlyEqual(a.animTime(), 10.0f));
}

// Con el flag encendido, un trigger reentra al mismo estado desde el principio.
static void test_any_state_reenters_with_can_transition_to_self()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    a.addState(makeTimedState("B"));
    a.addParameter("again", AnimatorComponent::ParamType::Trigger);
    AnimatorComponent::Transition t;
    t.fromState = AnimatorComponent::kAnyState; t.toState = 0;
    t.canTransitionToSelf = true;
    t.conditions.push_back(triggerCond("again"));
    a.addTransition(t);

    a.update(0.0f, false);
    a.update(1.0f, true);
    CHECK(nearlyEqual(a.animTime(), 10.0f));
    a.setTrigger("again");
    a.update(0.5f, true);                 // reentra: el reloj vuelve a 0
    CHECK(a.currentStateName() == "A");
    CHECK(nearlyEqual(a.animTime(), 0.0f));
}

// removeState con Any State: la que apuntaba al borrado se va, la otra se
// reindexa. El centinela no se toca.
static void test_remove_state_drops_and_reindexes_any_state_transitions()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    a.addState(makeTimedState("B"));
    a.addState(makeTimedState("C"));
    AnimatorComponent::Transition haciaB;
    haciaB.fromState = AnimatorComponent::kAnyState; haciaB.toState = 1;
    a.addTransition(haciaB);
    AnimatorComponent::Transition haciaC;
    haciaC.fromState = AnimatorComponent::kAnyState; haciaC.toState = 2;
    a.addTransition(haciaC);

    a.removeState(1);

    CHECK(a.transitions().size() == 1u);
    if (a.transitions().size() == 1u)
    {
        CHECK(a.transitions()[0].fromState == AnimatorComponent::kAnyState);
        CHECK(a.transitions()[0].toState == 1);
    }
}
```

Llamadas en `main()`, antes de `am.shutdown();`:

```cpp
    test_any_state_fires_from_every_state();
    test_any_state_wins_over_the_current_state_transition();
    test_any_state_does_not_reenter_the_current_state_by_default();
    test_any_state_reenters_with_can_transition_to_self();
    test_remove_state_drops_and_reindexes_any_state_transitions();
```

- [ ] **Step 2: Run to verify they fail**

Build y ejecutar. Expected: compila (los campos existen desde la tarea 1) y caen al menos `test_any_state_fires_from_every_state` y `test_any_state_wins_over_the_current_state_transition` (Any State todavía no se evalúa). `test_remove_state_drops_and_reindexes_any_state_transitions` puede pasar ya: el código de `removeState` lo hace bien hoy; su demostración es el sabotaje 5.

- [ ] **Step 3: Implement**

En `update`, justo antes del bucle `for (const auto& t : m_transitions)` de las transiciones del estado actual, añadir un bucle Any State que comparta el cuerpo del disparo. Para no duplicar el bloque del cross-fade, extraerlo a una lambda local. El tramo desde `// Orden de declaración: la primera lista, gana.` hasta el final de la función queda así:

```cpp
        // Dispara t: consume sus triggers, arma o corta el cross-fade y entra
        // en el destino.
        auto dispara = [this](const Transition& t)
        {
            consumeTriggers(t);

            if (t.duration > 0.0f)
            {
                // El estado que dejamos pasa a ser el que se apaga, con el
                // tiempo que llevara. Si YA había una mezcla en vuelo se
                // descarta: mezclar tres clips necesitaría un tercer bloque en
                // el SSBO y en el push constant, así que la mezcla anterior se
                // corta aquí (mismo criterio que Unity con su capa base).
                m_prevState     = m_currentState;
                m_prevAnimTime  = m_animTime;
                m_blendElapsed  = 0.0f;
                m_blendDuration = t.duration;
            }
            else
            {
                // Corte seco: ni estado previo ni mezcla, el camino de siempre.
                m_prevState     = -1;
                m_prevAnimTime  = 0.0f;
                m_blendElapsed  = 0.0f;
                m_blendDuration = 0.0f;
            }

            enterState(t.toState);
        };

        // Primero Any State y después las del estado actual, cada grupo por
        // orden de declaración: la primera lista, gana. Es la prioridad de
        // Unity, y lo que espera quien viene de allí.
        for (const auto& t : m_transitions)
        {
            if (t.fromState != kAnyState) continue;
            if (t.toState < 0 || t.toState >= (int)m_states.size()) continue;
            // Hacia el estado actual solo con el flag: con un bool, reentrar
            // reiniciaría el estado cada frame.
            if (t.toState == m_currentState && !t.canTransitionToSelf) continue;
            if (!transitionReady(t, n0, n1, conDuracion)) continue;
            dispara(t);
            return;                  // una transición por update
        }

        for (const auto& t : m_transitions)
        {
            if (t.fromState != m_currentState) continue;
            if (t.toState < 0 || t.toState >= (int)m_states.size()) continue;
            if (!transitionReady(t, n0, n1, conDuracion)) continue;
            dispara(t);
            return;                  // una transición por update
        }
    }
```

- [ ] **Step 4: Run the tests to verify they pass** — `dt_animator_tests: OK`.

- [ ] **Step 5: Full suite and commit** — `feat(animator): transiciones desde Any State` + Co-Authored-By.

- [ ] **Step 6: Sabotage, one at a time**

| # | Sabotaje | Valor observable | Debe caer |
|---|---|---|---|
| 1 | Borrar el bucle Any State entero | nunca se llega a C | `test_any_state_fires_from_every_state` |
| 2 | Poner el bucle del estado actual ANTES del de Any State | gana A→B | `test_any_state_wins_over_the_current_state_transition` |
| 3 | Quitar la línea `if (t.toState == m_currentState && !t.canTransitionToSelf) continue;` | reentra cada frame, animTime 0 | `test_any_state_does_not_reenter_the_current_state_by_default` |
| 4 | Cambiar esa línea por `if (t.toState == m_currentState) continue;` | nunca reentra, animTime 15 | `test_any_state_reenters_with_can_transition_to_self` |
| 5 | En `removeState`, en el predicado del `remove_if`, quitar `\|\| t.toState == idx` | sobrevive la que iba a B | `test_remove_state_drops_and_reindexes_any_state_transitions` |

---

### Task 3: serialización, clave del undo y posición del nodo

**Files:**
- Modify: `engine/include/DonTopo/Core/AnimatorComponent.h` (getter/setter y miembro)
- Modify: `engine/src/Core/Scene.cpp` (`animatorToJson` `:533-598`, `animatorGraphKey` `:600-605`, `animatorFromJson` transiciones `:653-712`)
- Test: `engine/tests/animator_tests.cpp` (`makeBaseGraph`, `graphMutations` `:4423`, `test_graph_key_ignores_node_position` `:4461`)

**Interfaces:**
- Consumes: `kAnyState`, los tres campos de `Transition` (tarea 1).
- Produces: `glm::vec2 AnimatorComponent::anyStateEditorPos() const;` `void AnimatorComponent::setAnyStateEditorPos(glm::vec2 p);` — la tarea 4 los usa.

- [ ] **Step 1: Write the failing tests**

1. **Dentro de `makeBaseGraph`** (la línea `a.addTransition(t);` aparece muchas veces en el fichero: localiza la de esa función con un Read del rango de `static void makeBaseGraph`), justo antes de su `a.addTransition(t);` añadir exit time a esa transición, y justo después añadir una Any State:

```cpp
    t.hasExitTime = true;       // así el .scene emite exitTime y las mutaciones lo ven
    t.exitTime    = 0.75f;
```

```cpp
    // Una transición Any State, para que canTransitionToSelf se pueda mutar.
    AnimatorComponent::Transition any;
    any.fromState = AnimatorComponent::kAnyState;
    any.toState   = 1;
    a.addTransition(any);
```

2. En `graphMutations()`, después de la entrada `"condition.threshold"`:

```cpp
        { "transition.hasExitTime",       [](A& a) { a.transitionsMutable()[0].hasExitTime = false; } },
        { "transition.exitTime",          [](A& a) { a.transitionsMutable()[0].exitTime = 0.25f; } },
        { "anyState.canTransitionToSelf", [](A& a) { a.transitionsMutable()[1].canTransitionToSelf = true; } },
        { "anyState.addTransition",       [](A& a) { A::Transition t; t.fromState = A::kAnyState; t.toState = 0;
                                                     a.addTransition(t); } },
```

3. En `test_graph_key_ignores_node_position`, antes de su llave de cierre:

```cpp
    // Y mover el nodo Any State tampoco es una edición.
    const nlohmann::json k1 = animatorGraphKey(a);
    a.setAnyStateEditorPos(glm::vec2(9.0f, 9.0f));
    CHECK(animatorGraphKey(a) == k1);
    CHECK(animatorToJson(a).contains("anyStatePos"));
```

4. Tests nuevos, antes de `int main()`:

```cpp
// ---- Exit time y Any State en el .scene ----

static void test_exit_time_and_any_state_survive_scene_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State s0; s0.name = "Idle"; s0.clipName = "ClipIdle";
    AnimatorComponent::State s1; s1.name = "Hit";  s1.clipName = "ClipHit";
    a->addState(s0);
    a->addState(s1);
    a->addParameter("hit", AnimatorComponent::ParamType::Trigger);

    AnimatorComponent::Transition vuelta;
    vuelta.fromState = 1; vuelta.toState = 0;
    vuelta.hasExitTime = true; vuelta.exitTime = 0.75f;
    a->addTransition(vuelta);

    AnimatorComponent::Transition any;
    any.fromState = AnimatorComponent::kAnyState; any.toState = 1;
    any.canTransitionToSelf = true;
    any.conditions.push_back({ AnimatorComponent::ConditionType::Trigger, "hit" });
    a->addTransition(any);
    a->setAnyStateEditorPos(glm::vec2(-300.0f, 55.0f));

    go->setAnimator(a);
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;

    const AnimatorComponent& b = *found->getAnimator();
    CHECK(b.transitions().size() == 2u);
    if (b.transitions().size() != 2u) return;
    CHECK(b.transitions()[0].hasExitTime);
    CHECK(nearlyEqual(b.transitions()[0].exitTime, 0.75f));
    CHECK(b.transitions()[1].fromState == AnimatorComponent::kAnyState);
    CHECK(b.transitions()[1].toState == 1);
    CHECK(b.transitions()[1].canTransitionToSelf);
    CHECK(b.anyStateEditorPos() == glm::vec2(-300.0f, 55.0f));
}

// Retrocompatibilidad: una escena sin los campos nuevos carga con sus
// defaults. Se escribe a mano un JSON con valores DISTINTOS de los que un
// default dejaría, y luego se le quitan, para que un lector roto se note.
static void test_scene_without_exit_time_fields_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State s0; s0.name = "Idle";
    AnimatorComponent::State s1; s1.name = "Run";
    a->addState(s0);
    a->addState(s1);
    AnimatorComponent::Transition t; t.fromState = 0; t.toState = 1;
    a->addTransition(t);
    a->setAnyStateEditorPos(glm::vec2(123.0f, 456.0f));
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();
    auto& anim = j["root"]["children"][0]["animator"];
    anim.erase("anyStatePos");
    for (auto& tr : anim["transitions"])
    {
        tr.erase("hasExitTime");
        tr.erase("exitTime");
        tr.erase("canTransitionToSelf");
    }

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    const AnimatorComponent& b = *found->getAnimator();
    CHECK(b.transitions().size() == 1u);
    if (b.transitions().size() != 1u) return;
    CHECK(!b.transitions()[0].hasExitTime);
    CHECK(nearlyEqual(b.transitions()[0].exitTime, 1.0f));
    CHECK(!b.transitions()[0].canTransitionToSelf);
    CHECK(b.anyStateEditorPos() != glm::vec2(123.0f, 456.0f));   // no se inventa la guardada
    CHECK(b.anyStateEditorPos() == AnimatorComponent().anyStateEditorPos());
}

// Any State con destino fuera de rango se descarta con aviso, igual que una
// normal; una Any State buena sobrevive; from = -1 sigue siendo inválido.
static void test_any_state_transition_with_bad_target_is_dropped(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State s0; s0.name = "Idle";
    AnimatorComponent::State s1; s1.name = "Run";
    a->addState(s0);
    a->addState(s1);
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();
    auto& trs = j["root"]["children"][0]["animator"]["transitions"];
    trs.push_back({ {"from", -2}, {"to", 1}, {"duration", 0.0f} });   // buena
    trs.push_back({ {"from", -2}, {"to", 7}, {"duration", 0.0f} });   // destino que no existe
    trs.push_back({ {"from", -1}, {"to", 1}, {"duration", 0.0f} });   // origen sin poner

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;

    const auto& cargadas = found->getAnimator()->transitions();
    CHECK(cargadas.size() == 1u);
    if (cargadas.size() == 1u)
    {
        CHECK(cargadas[0].fromState == AnimatorComponent::kAnyState);
        CHECK(cargadas[0].toState   == 1);
    }
    int avisos = 0;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("transition") != std::string::npos) ++avisos;
    CHECK(avisos == 2);
}

// Un exitTime negativo en el .scene se acota a 0 y se avisa.
static void test_negative_exit_time_is_clamped_with_warning(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State s0; s0.name = "Idle";
    AnimatorComponent::State s1; s1.name = "Run";
    a->addState(s0);
    a->addState(s1);
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();
    j["root"]["children"][0]["animator"]["transitions"].push_back(
        { {"from", 0}, {"to", 1}, {"duration", 0.0f}, {"hasExitTime", true}, {"exitTime", -3.0f} });

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    const auto& cargadas = found->getAnimator()->transitions();
    CHECK(cargadas.size() == 1u);
    if (cargadas.size() == 1u) CHECK(nearlyEqual(cargadas[0].exitTime, 0.0f));

    bool avisado = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("exitTime") != std::string::npos) avisado = true;
    CHECK(avisado);
}
```

Llamadas en `main()`, antes de `am.shutdown();`:

```cpp
    test_exit_time_and_any_state_survive_scene_round_trip(pm, am);
    test_scene_without_exit_time_fields_loads(pm, am);
    test_any_state_transition_with_bad_target_is_dropped(pm, am);
    test_negative_exit_time_is_clamped_with_warning(pm, am);
```

- [ ] **Step 2: Run to verify it fails**

Build. Expected: error de compilación, `'setAnyStateEditorPos': is not a member of 'DonTopo::AnimatorComponent'`.

- [ ] **Step 3: Implement — header**

Parte pública, en `// --- Diseño (editor / carga de escena) ---`, después de `void removeParameter(...)`:

```cpp
            // Posición del nodo Any State en el canvas del AnimatorPanel. Va
            // fuera de Graph a propósito: como la de los estados, mover un nodo
            // no entra en el undo.
            glm::vec2 anyStateEditorPos() const        { return m_anyStateEditorPos; }
            void      setAnyStateEditorPos(glm::vec2 p) { m_anyStateEditorPos = p; }
```

Parte privada, después de `int m_nextEditorId = 0;`:

```cpp
            // A la izquierda del primer estado que crea el panel (40, 40).
            glm::vec2               m_anyStateEditorPos{ -220.0f, 40.0f };
```

- [ ] **Step 4: Implement — `Scene.cpp`**

1. En `animatorToJson`, sustituir

```cpp
            transitions.push_back({ {"from", t.fromState}, {"to", t.toState},
                                    {"duration", t.duration}, {"conditions", conds} });
```

por

```cpp
            nlohmann::json tj = { {"from", t.fromState}, {"to", t.toState},
                                  {"duration", t.duration}, {"conditions", conds} };
            // Exit time y Any State: solo si se usan, igual que los campos de
            // blend. Any State se guarda como "from": -2 (kAnyState), que sigue
            // siendo un entero: el lector de "from" no cambia.
            if (t.hasExitTime)
            {
                tj["hasExitTime"] = true;
                tj["exitTime"]    = t.exitTime;
            }
            if (t.fromState == AnimatorComponent::kAnyState && t.canTransitionToSelf)
                tj["canTransitionToSelf"] = true;
            transitions.push_back(tj);
```

2. En el `return` de `animatorToJson`, añadir la posición del nodo:

```cpp
        return { {"entryState", a.entryState()},
                 {"parameters", params},
                 {"states", states},
                 {"transitions", transitions},
                 {"anyStatePos", nlohmann::json::array({ a.anyStateEditorPos().x,
                                                          a.anyStateEditorPos().y })} };
```

3. En `animatorGraphKey`, después del bucle que borra `"pos"`:

```cpp
        // Mover el nodo Any State tampoco es una edición.
        key.erase("anyStatePos");
```

4. En `animatorFromJson`, justo antes de `if (j.contains("transitions"))`:

```cpp
        // Ausente en escenas anteriores a Any State: se queda la posición por
        // defecto del componente.
        if (j.contains("anyStatePos") && j["anyStatePos"].is_array() && j["anyStatePos"].size() == 2)
            a->setAnyStateEditorPos(glm::vec2(readArrayFloat(j["anyStatePos"], 0, -220.0f, warnings, "animator.anyStatePos"),
                                              readArrayFloat(j["anyStatePos"], 1, 40.0f, warnings, "animator.anyStatePos")));
```

5. En el mismo bucle de transiciones, justo después de la lectura de `tr.duration`:

```cpp
                // Ausentes en escenas anteriores al exit time y a Any State:
                // caen en los defaults del struct.
                tr.hasExitTime = t.value("hasExitTime", false);
                tr.exitTime    = readFloat(t, "exitTime", 1.0f, warnings,
                                            "animator.transition[" + std::to_string(tr.fromState) +
                                            "->" + std::to_string(tr.toState) + "].exitTime");
                if (tr.exitTime < 0.0f)
                {
                    if (warnings)
                        warnings->push_back("animator.transition[" + std::to_string(tr.fromState) +
                                             "->" + std::to_string(tr.toState) +
                                             "].exitTime negativo (" + std::to_string(tr.exitTime) +
                                             "), se acota a 0");
                    tr.exitTime = 0.0f;
                }
                tr.canTransitionToSelf = t.value("canTransitionToSelf", false);
```

6. Sustituir la condición de la validación de índices

```cpp
                if (tr.fromState < 0 || tr.fromState >= nEstados ||
                    tr.toState   < 0 || tr.toState   >= nEstados)
```

por

```cpp
                // Any State (kAnyState) es un origen válido; -1 u otro negativo
                // sigue sin serlo. El destino se exige siempre en rango.
                const bool origenValido = tr.fromState == AnimatorComponent::kAnyState ||
                                          (tr.fromState >= 0 && tr.fromState < nEstados);
                if (!origenValido || tr.toState < 0 || tr.toState >= nEstados)
```

- [ ] **Step 5: Run the tests to verify they pass** — `dt_animator_tests: OK`. Los tests de undo y de ida y vuelta que ya existían (`test_graph_key_sees_every_saved_field`, `test_graph_command_round_trip_for_each_mutation`, `test_animator_out_of_range_transition_is_dropped`, `test_graph_survives_scene_round_trip`…) siguen en verde.

- [ ] **Step 6: Full suite and commit** — `feat(animator): exit time y Any State en el .scene y en el undo` + Co-Authored-By.

- [ ] **Step 7: Sabotage, one at a time** (restaurar con `git checkout -- engine/src/Core/Scene.cpp`)

| # | Sabotaje | Valor observable | Debe caer |
|---|---|---|---|
| 1 | Quitar el bloque `if (t.hasExitTime) { ... }` de `animatorToJson` | exit time no llega al .scene | `test_exit_time_and_any_state_survive_scene_round_trip`; `test_graph_key_sees_every_saved_field` con "no ve 'transition.hasExitTime'" |
| 2 | Quitar `if (... && t.canTransitionToSelf) tj["canTransitionToSelf"] = true;` | el flag se pierde | round trip; clave "no ve 'anyState.canTransitionToSelf'" |
| 3 | Dejar la validación vieja (`tr.fromState < 0 \|\| ...`) | Any State se descarta al cargar | round trip y `test_any_state_transition_with_bad_target_is_dropped` |
| 4 | En la validación nueva, quitar `\|\| tr.toState >= nEstados` | Any State hacia 7 sobrevive | `test_any_state_transition_with_bad_target_is_dropped` |
| 5 | Quitar el bloque del acotado de `exitTime` | exitTime −3 y sin aviso | `test_negative_exit_time_is_clamped_with_warning` |
| 6 | Quitar `key.erase("anyStatePos");` | mover Any State cambia la clave | `test_graph_key_ignores_node_position` |
| 7 | Cambiar `t.value("hasExitTime", false)` por `t.value("hasExitTime", true)` | una escena vieja carga con exit time | `test_scene_without_exit_time_fields_loads` |
| 8 | Borrar el bloque `if (j.contains("anyStatePos") ...)` entero | la posición guardada no se lee | `test_exit_time_and_any_state_survive_scene_round_trip` (el `CHECK` de `anyStateEditorPos`) |

---

### Task 4: UI del AnimatorPanel y README

**Files:**
- Modify: `engine/src/Editor/AnimatorPanel.cpp` (ids `:35-45`, sync `:98-113`, nodos `:204-340`, links `:342-355`, creación `:357-390`, borrado `:410-420`, popup `:504-520` y `:602-617`)
- Modify: `README.md` (`## Animator`, `:676`)

**Interfaces:**
- Consumes: `AnimatorComponent::kAnyState`, `Transition::hasExitTime/exitTime/canTransitionToSelf` (tarea 1), `anyStateEditorPos()/setAnyStateEditorPos()` (tarea 3).

Sin test automático (ImGui). Su verificación es la build limpia, la suite completa y la comprobación manual de la tarea 5.

- [ ] **Step 1: Ids reservados**

Justo después de `bool isOutputPin(int pin) { return (pin - 1) % 3 == 2; }`:

```cpp

    // Nodo Any State: ids FUERA del esquema de los estados (eid*3+1..3) y de
    // los links (100000+idx). Se comprueban SIEMPRE antes de decodificar con
    // editorIdFromRawId: pasados por esa fórmula casarían con un editorId
    // (300000) que ningún grafo alcanza, pero isOutputPin los clasificaría
    // mal — por eso esPinDeSalida.
    const int kAnyStateNodeId   = 900001;
    const int kAnyStateOutPinId = 900002;

    bool esPinDeSalida(int pin) { return pin == kAnyStateOutPinId || (pin != kAnyStateNodeId && isOutputPin(pin)); }
```

- [ ] **Step 2: Posiciones**

`syncPositionsFromComponent`, al final del cuerpo:

```cpp
    // El nodo Any State existe mientras haya al menos un estado.
    if (!states.empty())
    {
        const glm::vec2 p = go->getAnimator()->anyStateEditorPos();
        ed::SetNodePosition(kAnyStateNodeId, ImVec2(p.x, p.y));
    }
```

`syncPositionsToComponent`, al final del cuerpo:

```cpp
    if (!states.empty())
    {
        const ImVec2 p = ed::GetNodePosition(kAnyStateNodeId);
        go->getAnimator()->setAnyStateEditorPos(glm::vec2(p.x, p.y));
    }
```

- [ ] **Step 3: El nodo**

En `drawGraph`, justo antes de `    // --- Links ---`:

```cpp
    // --- Nodo Any State ---
    // Solo si hay estados: sin ellos no hay adónde ir. Solo tiene salida.
    if (!states.empty())
    {
        ed::PushStyleColor(ed::StyleColor_NodeBg, ImVec4(0.30f, 0.20f, 0.45f, 0.90f));
        ed::BeginNode(kAnyStateNodeId);
        ImGui::TextUnformatted("Any State");
        ed::BeginPin(kAnyStateOutPinId, ed::PinKind::Output);
        ImGui::TextUnformatted("out ->");
        ed::EndPin();
        ed::EndNode();
        ed::PopStyleColor();
    }

```

- [ ] **Step 4: Links**

En el bucle de `// --- Links ---`, sustituir

```cpp
        if (from < 0 || from >= (int)states.size() || to < 0 || to >= (int)states.size()) continue;
        ed::Link(linkId((int)t),
                 outputPinId(states[from].editorId),
                 inputPinId(states[to].editorId));
```

por

```cpp
        const bool desdeAny = from == AnimatorComponent::kAnyState;
        if ((!desdeAny && (from < 0 || from >= (int)states.size())) ||
            to < 0 || to >= (int)states.size()) continue;
        ed::Link(linkId((int)t),
                 desdeAny ? kAnyStateOutPinId : outputPinId(states[from].editorId),
                 inputPinId(states[to].editorId));
```

- [ ] **Step 5: Crear links desde Any State**

En `// --- Crear links arrastrando de pin a pin ---`, sustituir desde `const int outPin = isOutputPin(pa) ? pa : pb;` hasta `const int toIdx   = stateIndexFromPin(*anim, inPin);` (inclusive) por:

```cpp
            const int outPin = esPinDeSalida(pa) ? pa : pb;
            const int inPin  = esPinDeSalida(pa) ? pb : pa;

            if (esPinDeSalida(outPin) && !esPinDeSalida(inPin) && inPin != kAnyStateNodeId &&
                ed::AcceptNewItem())
            {
                // stateFromPin (índice) en vez de directamente el editorId: las
                // transiciones guardan índices del vector, no editorIds. El pin
                // de Any State no se decodifica: es el centinela.
                const int fromIdx = (outPin == kAnyStateOutPinId)
                                    ? AnimatorComponent::kAnyState
                                    : stateIndexFromPin(*anim, outPin);
                const int toIdx   = stateIndexFromPin(*anim, inPin);
```

y en la condición siguiente, `if (fromIdx >= 0 && toIdx >= 0)` por `if ((fromIdx >= 0 || fromIdx == AnimatorComponent::kAnyState) && toIdx >= 0)`.

- [ ] **Step 6: El nodo Any State no se borra**

Sustituir

```cpp
        while (ed::QueryDeletedNode(&dn))
            if (ed::AcceptDeletedItem())
            {
                const int idx = stateIndexFromPin(*anim, (int)dn.Get());
                if (idx >= 0) statesToRemove.push_back(idx);
            }
```

por

```cpp
        while (ed::QueryDeletedNode(&dn))
        {
            // El nodo Any State no se borra: existe mientras haya estados.
            if ((int)dn.Get() == kAnyStateNodeId) { ed::RejectDeletedItem(); continue; }
            if (ed::AcceptDeletedItem())
            {
                const int idx = stateIndexFromPin(*anim, (int)dn.Get());
                if (idx >= 0) statesToRemove.push_back(idx);
            }
        }
```

(El menú contextual no necesita cambios: `stateIndexFromPin` devuelve −1 para el nodo Any State y "Set as Entry" ya no se ofrece con −1.)

- [ ] **Step 7: Popup de la transición**

1. Justo después de `if (tr.duration < 0.0f) tr.duration = 0.0f;`:

```cpp

    // Exit time: la transición espera a que el estado de origen llegue a
    // exitTime (normalizado; 1 = fin del clip, >1 cuenta vueltas). Sin
    // condiciones dispara solo por tiempo.
    ImGui::PushID("exit_time");
    ImGui::Checkbox("Has Exit Time", &tr.hasExitTime);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Espera a que el estado de origen llegue a 'exit time'. Sin condiciones, dispara solo por tiempo.");
    if (tr.hasExitTime)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::DragFloat("exit time", &tr.exitTime, 0.01f, 0.0f, 100.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Tiempo normalizado: 0.9 = al 90%% del clip, 2.5 = tras dos vueltas y media.");
        if (tr.exitTime < 0.0f) tr.exitTime = 0.0f;
    }
    if (tr.fromState == AnimatorComponent::kAnyState)
    {
        ImGui::Checkbox("Can Transition To Self", &tr.canTransitionToSelf);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Si puede volver al estado en el que ya se esta. Encendido con un bool, lo reiniciaria cada frame.");
    }
    ImGui::PopID();
```

2. Sustituir el texto del tooltip de "animation finished" en loop:

```cpp
        ImGui::SetTooltip("El estado de origen está en loop: 'animation finished' nunca dispara aquí (un clip en loop nunca termina).");
```

por

```cpp
        ImGui::SetTooltip("El estado de origen está en loop: 'animation finished' nunca dispara aquí. Para salir por tiempo, usa 'Has Exit Time'.");
```

- [ ] **Step 8: README**

En `README.md`, sección `## Animator`, justo antes del párrafo que empieza por "Every edit to the graph", añadir (envolviendo a ~90 columnas como el resto de la sección):

```
A transition can also wait for time. **Has Exit Time** makes it fire when the source state
reaches **exit time**, in normalized time: `0.9` is 90% of the clip, and values above `1`
count loops (`2.5` waits two and a half loops). On a looping state an exit time below `1` is
checked once per loop. With no conditions it fires on time alone; with conditions it needs
both.

The **Any State** node holds transitions that apply from whichever state is current. They are
evaluated before the current state's own transitions. By default an Any State transition
never re-enters the state that is already playing; turn on **Can Transition To Self** on that
transition to allow it (for example, a hit reaction that restarts on every trigger).
```

- [ ] **Step 9: Build, warnings, full suite and commit**

PowerShell: `& .\build.bat`; sin avisos nuevos en `AnimatorPanel.cpp`. Suite completa 28/28. Commit: `feat(editor): exit time y nodo Any State en el AnimatorPanel` + Co-Authored-By.

---

### Task 5: verificación final (la hace el controlador)

- [ ] **Step 1: Release** — `& .\build-release.bat` y la suite desde `build-ninja-release/engine/tests` (28/28).
- [ ] **Step 2: Manual (usuario), Vulkan y D3D12**
  1. Crear un link desde Any State a un estado; guardar la escena, cargarla, y el link sigue ahí.
  2. Un estado en loop con una transición `Has Exit Time` a 0.9 sin condiciones: sale al 90 % de la primera vuelta.
  3. Ctrl+Z sobre `Has Exit Time`, `exit time` (un drag = un paso) y `Can Transition To Self`.
  4. Seleccionar el nodo Any State y pulsar Supr: no se borra.
- [ ] **Step 3: Auditoría** — en `docs/animation-audit.md`: A5 CERRADO; C5 EXISTE; C6 de PARCIAL a EXISTE; fila 3 del backlog hecha.
