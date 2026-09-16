# Eventos de animación — plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (inline, elegido por el usuario). Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** que un estado del Animator dispare eventos con nombre en su ciclo y los scripts Lua del GameObject los reciban en `OnAnimationEvent(name)`.

**Architecture:** `AnimatorComponent::update` cuenta cruces sobre `m_stateTicks` (double y sin wrap) y deja los nombres en `firedEvents()`, que se vacía en cada update. `ScriptManager::update` los entrega antes de `Update`. Se serializan con el estado y se editan en el nodo.

**Tech Stack:** C++20, nlohmann::json, sol2/Lua 5.4, ImGui + imgui-node-editor.

**Spec:** `docs/superpowers/specs/2026-09-16-animation-events-design.md` (`fb68382`)

## Global Constraints

- Rama `feat/animation-events`. Build `.\build.bat` desde PowerShell; tests desde la raíz (`build-ninja\engine\tests\dt_*.exe`).
- Binding/callback Lua nuevo ⇒ `LuaApiReference.cpp` + README `## Lua Scripting`.
- Editar solo con Edit o Python con `newline=''` (CRLF). Commits con `git commit -F -` y el trailer `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`.
- Sabotajes de uno en uno por script PowerShell, restaurando con `git checkout --` sobre un árbol commiteado, compilando con `cmd /c ".\build.bat > nul 2>&1"` y sin `$ErrorActionPreference="Stop"`.
- No se tocan shaders ni backends.
- **Hecho comprobado al planificar**: el editor no tiene Pause (`grep -i pause sandbox/src/main.cpp engine/src/Editor/EditorUI.cpp` no devuelve nada). La nota `[sin verificar]` de la spec se resuelve así en la tarea 4.

---

### Task 1: núcleo (datos, disparo y serialización)

**Files:**
- Modify: `engine/include/DonTopo/Core/AnimatorComponent.h` (junto a `BlendEntry`; `State`; público cerca de `poseWeight`; privado cerca de `advanceClock` y `m_stateTicks`)
- Modify: `engine/src/Core/AnimatorComponent.cpp` (`update` :640-681)
- Modify: `engine/src/Core/Scene.cpp` (escritura junto a `lockRootMotion` :559, lectura junto a :686)
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Produces: `struct AnimatorComponent::AnimationEvent { std::string name; float time = 0.0f; };`, `State::events` (`std::vector<AnimationEvent>`), `const std::vector<std::string>& firedEvents() const`, y privado `void collectEvents(const State& st, double ticks0, double ticks1);` con `static constexpr int kMaxEventCyclesPerUpdate = 16;`.

- [ ] **Step 1: rama**

```bash
git checkout -b feat/animation-events
```

- [ ] **Step 2: tests**

Delante de `int main()` en `animator_tests.cpp`:

```cpp
// ── Eventos de animación ─────────────────────────────────────────────────────
//
// Walk: 40 ticks a 20 tps = 2 s por ciclo, en loop, con "paso" en la mitad.
static AnimatorComponent makeEventGraph(float eventTime = 0.5f, bool loop = true)
{
    AnimatorComponent a;
    AnimatorComponent::State s;
    s.name = "Walk"; s.clipName = "Walk"; s.clipIndex = 0;
    s.duration = 40.0f; s.ticksPerSecond = 20.0f; s.loop = loop;
    s.events.push_back({ "paso", eventTime });
    a.addState(s);
    a.setEntryState(0);
    a.reset();
    return a;
}

static int contar(const AnimatorComponent& a, const char* nombre)
{
    int n = 0;
    for (const auto& e : a.firedEvents()) if (e == nombre) n++;
    return n;
}

// Una vez por ciclo con dt de frame, y nunca dos en el mismo update.
static void test_event_fires_once_per_cycle()
{
    AnimatorComponent a = makeEventGraph();
    int total = 0;
    bool dobleEnUno = false;
    for (int i = 0; i < 250; i++)                  // 4 s = 2 ciclos
    {
        a.update(0.016f, true);
        const int n = contar(a, "paso");
        total += n;
        if (n > 1) dobleEnUno = true;
    }
    CHECK(total == 2);
    CHECK(!dobleEnUno);
}

// Un dt que cruza el wrap del loop dispara el evento del principio del ciclo.
static void test_event_fires_across_loop_wrap()
{
    AnimatorComponent a = makeEventGraph(0.02f);   // 0,8 ticks
    a.update(1.9f, true);                          // 38 ticks
    a.update(0.2f, true);                          // 38 -> 42: cruza 40,8
    CHECK(contar(a, "paso") == 1);
}

// Un dt que salta dos ciclos dispara dos veces.
static void test_event_fires_per_skipped_cycle()
{
    AnimatorComponent a = makeEventGraph();
    a.update(4.2f, true);                          // 84 ticks: cruza 20 y 60
    CHECK(contar(a, "paso") == 2);
}

// Sin loop, una sola vez aunque el reloj siga.
static void test_event_non_loop_fires_once()
{
    AnimatorComponent a = makeEventGraph(0.5f, /*loop=*/false);
    int total = 0;
    for (int i = 0; i < 10; i++) { a.update(1.0f, true); total += contar(a, "paso"); }
    CHECK(total == 1);
}

// Evento en 0: dispara en el primer update con dt > 0, no en uno de dt 0.
static void test_event_at_zero_fires_on_entry()
{
    AnimatorComponent a = makeEventGraph(0.0f);
    a.update(0.0f, true);
    CHECK(contar(a, "paso") == 0);
    a.update(0.1f, true);
    CHECK(contar(a, "paso") == 1);
}

// En Edit (sin evaluar transiciones) no dispara nada.
static void test_event_not_fired_in_edit_mode()
{
    AnimatorComponent a = makeEventGraph();
    a.update(4.2f, false);
    CHECK(a.firedEvents().empty());
}

// Durante un cross-fade dispara solo el destino; el que se apaga, no.
static void test_event_fading_out_state_is_silent()
{
    AnimatorComponent a = makeEventGraph(0.1f);    // 4 ticks
    AnimatorComponent::State b;
    b.name = "Idle"; b.clipName = "Idle"; b.clipIndex = 1;
    b.duration = 40.0f; b.ticksPerSecond = 20.0f; b.loop = true;
    a.addState(b);
    a.addParameter("go", AnimatorComponent::ParamType::Trigger);
    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1; t.duration = 2.0f;
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = "go";
    t.conditions.push_back(c);
    a.addTransition(t);

    a.setTrigger("go");
    a.update(0.016f, true);                        // sale a Idle con fade de 2 s
    CHECK(a.currentStateName() == "Idle");
    CHECK(a.blending());
    a.update(0.9f, true);                          // Walk sigue: 0,32 -> 18,32, cruza 4
    CHECK(a.blending());
    CHECK(a.firedEvents().empty());
}

// firedEvents solo guarda lo del último update.
static void test_fired_events_cleared_each_update()
{
    AnimatorComponent a = makeEventGraph();
    a.update(1.1f, true);                          // cruza 20
    CHECK(contar(a, "paso") == 1);
    a.update(0.01f, true);
    CHECK(a.firedEvents().empty());
}

// Ida y vuelta de events; un time fuera de rango se clampa con aviso.
static void test_events_scene_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>(makeEventGraph(0.25f));
    a->statesMutable()[0].events.push_back({ "golpe", 0.75f });
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();
    bool tocado = false;
    for (auto& node : j["root"]["children"])
    {
        if (!node.contains("animator")) continue;
        node["animator"]["states"][0]["events"][1]["time"] = 3.0f;
        tocado = true;
    }
    CHECK(tocado);

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    bool aviso = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("time fuera de [0,1]") != std::string::npos) aviso = true;
    CHECK(aviso);
    GameObject* found = loaded.findById(id);
    CHECK(found && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    const auto& ev = found->getAnimator()->states()[0].events;
    CHECK(ev.size() == 2u);
    if (ev.size() != 2u) return;
    CHECK(ev[0].name == "paso" && nearlyEqual(ev[0].time, 0.25f));
    CHECK(ev[1].name == "golpe" && nearlyEqual(ev[1].time, 1.0f));
}
```

En `graphMutations()`, añadir:

```cpp
        { "state.events",               [](A& a) { a.statesMutable()[0].events.push_back({ "paso", 0.5f }); } },
```

En `main`, tras las llamadas `test_blend1d_*`:

```cpp
    test_event_fires_once_per_cycle();
    test_event_fires_across_loop_wrap();
    test_event_fires_per_skipped_cycle();
    test_event_non_loop_fires_once();
    test_event_at_zero_fires_on_entry();
    test_event_not_fired_in_edit_mode();
    test_event_fading_out_state_is_silent();
    test_fired_events_cleared_each_update();
    test_events_scene_round_trip(pm, am);
```

- [ ] **Step 3: header**

Tras `struct BlendEntry {...};`:

```cpp
            // Evento con nombre en un instante del ciclo del estado. time es
            // fase normalizada [0, 1] sobre la duración del clip principal.
            struct AnimationEvent
            {
                std::string name;
                float       time = 0.0f;
            };
```

En `State`, tras `blendEntries`:

```cpp
                // Eventos del estado: disparan en Play (ver collectEvents) y
                // llegan a Lua como OnAnimationEvent(name).
                std::vector<AnimationEvent> events;
```

Público, junto a `poseWeight()`:

```cpp
            // Nombres disparados en el ÚLTIMO update, en orden. Se vacía al
            // principio de cada update: quien los lea una vez por frame los ve
            // una sola vez.
            const std::vector<std::string>& firedEvents() const { return m_firedEvents; }
```

Privado, junto a `advanceClock`:

```cpp
            // Empuja a m_firedEvents los eventos de st cuyo instante cae en
            // [ticks0, ticks1), una vez por ciclo cruzado (solo el primero
            // sin loop), con tope de kMaxEventCyclesPerUpdate por evento.
            void collectEvents(const State& st, double ticks0, double ticks1);
            static constexpr int kMaxEventCyclesPerUpdate = 16;
```

Junto a `m_stateTicks`: `std::vector<std::string> m_firedEvents;`

- [ ] **Step 4: `update` y `collectEvents`**

Primera línea de `update`, antes del `if` del estado inválido: `m_firedEvents.clear();`

Sustituir `if (!evaluateTransitions) return;` por:

```cpp
        if (!evaluateTransitions) return;

        // Eventos ANTES de las transiciones: si este update sale del estado,
        // su tramo final ya ha disparado. Solo el estado actual; el que se
        // apaga en un fade no, o las pisadas saldrían dobles.
        if (conDuracion)
            collectEvents(actual, ticks0, m_stateTicks);
```

Tras `advanceClock`:

```cpp
    void AnimatorComponent::collectEvents(const State& st, double ticks0, double ticks1)
    {
        if (st.events.empty() || st.duration <= 0.0f || ticks1 <= ticks0) return;
        const double dur = st.duration;
        for (const auto& ev : st.events)
        {
            // Instantes k·dur + c, k >= 0, dentro de [ticks0, ticks1).
            const double c    = (double)ev.time * dur;
            double       kMin = std::ceil((ticks0 - c) / dur);
            if (kMin < 0.0) kMin = 0.0;
            double       kMax = std::ceil((ticks1 - c) / dur) - 1.0;
            if (!st.loop) kMax = std::min(kMax, 0.0);
            if (kMax < kMin) continue;
            const double veces = std::min(kMax - kMin + 1.0, (double)kMaxEventCyclesPerUpdate);
            for (int i = 0; i < (int)veces; i++)
                m_firedEvents.push_back(ev.name);
        }
    }
```

- [ ] **Step 5: `Scene.cpp`**

Escritura, tras el bloque de `lockRootMotion`:

```cpp
            // Eventos: solo si hay alguno, como el blend.
            if (!s.events.empty())
            {
                nlohmann::json eventos = nlohmann::json::array();
                for (const auto& ev : s.events)
                    eventos.push_back({ {"name", ev.name}, {"time", ev.time} });
                sj["events"] = std::move(eventos);
            }
```

Lectura, tras `st.lockRootMotion = ...`:

```cpp
                // Ausentes en escenas anteriores a los eventos: ninguno.
                if (s.contains("events") && s["events"].is_array())
                {
                    const std::string ctxEv = "animator.state." + st.name + ".events";
                    for (const auto& ej : s["events"])
                    {
                        if (!ej.is_object()) continue;
                        AnimatorComponent::AnimationEvent ev;
                        ev.name = ej.value("name", std::string());
                        ev.time = readFloat(ej, "time", 0.0f, warnings, ctxEv);
                        if (ev.time < 0.0f || ev.time > 1.0f)
                        {
                            if (warnings) warnings->push_back(ctxEv + ": time fuera de [0,1], se ajusta");
                            ev.time = std::clamp(ev.time, 0.0f, 1.0f);
                        }
                        st.events.push_back(std::move(ev));
                    }
                }
```

(`<algorithm>` ya está si compila `std::clamp`; si no, añadirlo.)

- [ ] **Step 6: build y suite**

```powershell
.\build.bat 2>&1 | Select-String "error C" | Select-Object -First 8; $LASTEXITCODE
Get-ChildItem build-ninja\engine\tests\dt_*.exe | ForEach-Object { & $_.FullName *> $null; if ($LASTEXITCODE -ne 0) { "FALLA $($_.Name)" } }
```

Esperado: 0 y ninguna FALLA.

- [ ] **Step 7: comprobar CRLF y commit**

```bash
[ "$(git diff --stat | tail -1)" = "$(git diff --ignore-cr-at-eol --stat | tail -1)" ] && echo "CRLF OK"
git add -u && git commit -F - <<'EOF'
feat(animator): eventos de animación por estado

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
```

- [ ] **Step 8: sabotajes (uno a uno)**

| # | Dónde | Cambio | Debe caer |
|---|---|---|---|
| S1 | `collectEvents` | `std::ceil((ticks0 - c) / dur)` → `std::floor((ticks0 - c) / dur)` | `once_per_cycle` o `across_loop_wrap` |
| S2 | `collectEvents` | `std::ceil((ticks1 - c) / dur) - 1.0` → `std::ceil((ticks1 - c) / dur)` (incluye el ciclo siguiente) | `once_per_cycle` |
| S3 | `collectEvents` | `if (!st.loop) kMax = std::min(kMax, 0.0);` → borrar | `non_loop_fires_once` |
| S4 | `collectEvents` | `(double)kMaxEventCyclesPerUpdate` → `1.0` | `per_skipped_cycle` |
| S5 | `update` | `if (!evaluateTransitions) return;` se mueve DESPUÉS del bloque de eventos | `not_fired_in_edit_mode` |
| S6 | `update` | añadir tras el bloque de eventos: `if (m_prevState >= 0 && m_prevState < (int)m_states.size()) collectEvents(m_states[m_prevState], 0.0, 1e9);` | `fading_out_state_is_silent` |
| S7 | `update` | borrar `m_firedEvents.clear();` | `cleared_each_update` |
| S8 | `Scene.cpp` | borrar el `ev.time = std::clamp(...)` | `events_scene_round_trip` |
| S9 | `Scene.cpp` | `{"time", ev.time}` → `{"time", 0.0f}` | `events_scene_round_trip` y la clave del grafo |

---

### Task 2: entrega a Lua

**Files:**
- Modify: `engine/include/DonTopo/Scripting/ScriptManager.h` (junto a `callOptionalCallback` :147 y `drainTriggerQueue` :156)
- Modify: `engine/src/Scripting/ScriptManager.cpp` (junto a `callOptionalCallback` :279; `update` :498)
- Modify: `engine/src/Scripting/LuaApiReference.cpp:54`
- Modify: `README.md` (`## Lua Scripting` ~:873 y sección del Animator ~:681)
- Test: `engine/tests/scripting_tests.cpp`

**Interfaces:**
- Consumes: `AnimatorComponent::firedEvents()`, `State::events`, `AnimationEvent`.
- Produces: `void ScriptManager::callOptionalStringCallback(ScriptComponent&, const char* fn, const std::string& arg);`, `void ScriptManager::deliverAnimationEvents();`

- [ ] **Step 1: test** (delante de `int main` en `scripting_tests.cpp`, con su llamada tras `test_animator_lua_play_crossfade_reset_and_time(sm);`)

```cpp
// OnAnimationEvent: un evento que dispara el Animator llega a los scripts del
// mismo GameObject en el siguiente ScriptManager::update, una sola vez.
static void test_animation_event_reaches_lua(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Personaje");
    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State s;
    s.name = "Walk"; s.clipName = "Walk"; s.duration = 40.0f; s.ticksPerSecond = 20.0f;
    s.events.push_back({ "paso", 0.5f });
    a->addState(s);
    go->setAnimator(a);
    sm.rebuildAliveSet();
    sm.onPlayStart();

    auto r = sm.lua().safe_script(R"(
        recibidos = ""
        OyenteEventos = {}
        function OyenteEventos:OnAnimationEvent(nombre) recibidos = recibidos .. nombre .. ";" end
    )", sol::script_pass_on_error);
    CHECK(r.valid());
    if (!r.valid()) { sm.onPlayStop(); return; }
    // Instancia a mano y ya arrancada: no hay .lua en disco que cargar.
    auto comp = std::make_unique<ScriptComponent>("OyenteEventos", go);
    comp->instance = sm.lua()["OyenteEventos"];
    comp->started  = true;
    go->addScript(std::move(comp));

    a->update(1.1f, true);                 // cruza la mitad del ciclo de 2 s
    CHECK(a->firedEvents().size() == 1u);
    sm.update(0.016f);
    CHECK(sm.lua()["recibidos"].get<std::string>() == "paso;");

    a->update(0.1f, true);                 // no cruza nada
    sm.update(0.016f);
    CHECK(sm.lua()["recibidos"].get<std::string>() == "paso;");
    sm.onPlayStop();
}
```

Si hace falta, añadir `#include "DonTopo/Scripting/ScriptComponent.h"`.

- [ ] **Step 2: implementación**

`ScriptManager.h`, tras `callOptionalCallback`:

```cpp
    // Como callOptionalCallback, con un string de argumento (OnAnimationEvent).
    void callOptionalStringCallback(ScriptComponent& comp, const char* fn, const std::string& arg);
```

Tras `drainTriggerQueue`:

```cpp
    // Entrega los firedEvents() de cada Animator a OnAnimationEvent de los
    // scripts de su GameObject. Corre una vez por update, antes de Update.
    void deliverAnimationEvents();
```

`ScriptManager.cpp`, tras `callOptionalCallback`:

```cpp
    void ScriptManager::callOptionalStringCallback(ScriptComponent& comp, const char* fn, const std::string& arg)
    {
        if (comp.hasError || !comp.instance.valid()) return;
        sol::object entry = comp.instance[fn];
        if (entry.get_type() != sol::type::function) return; // el script no lo define
        sol::protected_function f = entry;
        auto r = f(comp.instance, arg);
        if (!r.valid())
        {
            sol::error err = r;
            log("Script '" + comp.scriptName + "' " + fn + ": " + std::string(err.what()));
            comp.hasError = true;
        }
    }

    void ScriptManager::deliverAnimationEvents()
    {
        // Primero se recogen y luego se llama: un callback puede instanciar o
        // destruir objetos, y eso no debe pasar con el traverse abierto.
        std::vector<GameObject*> conEventos;
        m_scene->traverse([&](GameObject* go) {
            const auto& anim = go->getAnimator();
            if (anim && !anim->firedEvents().empty() && !go->getScripts().empty())
                conEventos.push_back(go);
        });
        for (GameObject* go : conEventos)
        {
            if (!isAlive(go) || !go->getAnimator()) continue;
            // Copia: un callback puede tocar el Animator (Play, CrossFade).
            const std::vector<std::string> nombres = go->getAnimator()->firedEvents();
            std::vector<ScriptComponent*> scripts;
            for (auto& s : go->getScripts()) scripts.push_back(s.get());
            for (const std::string& n : nombres)
                for (ScriptComponent* s : scripts)
                    callOptionalStringCallback(*s, "OnAnimationEvent", n);
        }
    }
```

En `update`, tras `drainTriggerQueue();`:

```cpp
        // Eventos del Animator del último avance (applySkinnedFrame): uno por
        // frame de cada lado, así que cada tanda se entrega una vez.
        deliverAnimationEvents();
```

- [ ] **Step 3: referencia y README**

`LuaApiReference.cpp:54`: la línea pasa a `"OnCollisionEnter", "OnCollisionStay", "OnCollisionExit", "OnAnimationEvent",` y el comentario de encima gana: `OnAnimationEvent(name) cuando el Animator del objeto cruza un evento de su estado (solo en Play).` Si hay una tabla de firmas o descripciones por símbolo (buscar `"OnCollisionEnter"` en el resto del fichero), añadir la entrada equivalente.

README, `## Lua Scripting` (~:873): `plus the trigger and collision callbacks)` → `plus the trigger, collision and animation-event callbacks)`, y añadir tras el párrafo: `` `OnAnimationEvent(name)` fires on every script of a GameObject whose Animator crosses one of its current state's events (Play only, once per cycle, and during a cross-fade only the incoming state fires). ``

README, sección del Animator (~:681): tras la frase del blend, `Each state can also carry named **animation events** at normalized times of its cycle, delivered to Lua as ` + "`OnAnimationEvent`." + `

- [ ] **Step 4: build, suite, commit**

Igual que en la tarea 1. Commit `feat(scripting): OnAnimationEvent entrega los eventos del Animator a Lua`.

- [ ] **Step 5: sabotajes**

| # | Cambio | Debe caer |
|---|---|---|
| S10 | quitar la llamada `deliverAnimationEvents();` | `animation_event_reaches_lua` |
| S11 | `auto r = f(comp.instance, arg);` → `auto r = f(comp.instance, std::string("x"));` | `animation_event_reaches_lua` |

---

### Task 3: editor

**Files:**
- Modify: `engine/src/Editor/AnimatorPanel.cpp` (tras el botón "+ blend clip"; justo tras `ed::Begin("AnimatorCanvas");` :221)

- [ ] **Step 1: atajos del canvas mientras se escribe**

Justo después de `ed::Begin("AnimatorCanvas");`:

```cpp
    // Escribiendo en un campo de un nodo (nombre de evento), Supr borraría el
    // nodo seleccionado: imgui-node-editor mira la tecla, no el foco de texto.
    ed::EnableShortcuts(!ImGui::GetIO().WantTextInput);
```

- [ ] **Step 2: filas de eventos**

Tras el `if (ImGui::Button("+ blend clip##addBlend", ...)) {...}` y **fuera** del `if (go->getSkinnedMesh())`:

```cpp
        // --- Eventos del estado ---
        ImGui::PushID("eventos");
        int quitarEvento = -1;
        for (int k = 0; k < (int)stMut.events.size(); k++)
        {
            auto& ev = stMut.events[k];
            ImGui::PushID(k);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s", ev.name.c_str());
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::InputText("##evName", buf, sizeof(buf))) ev.name = buf;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(50.0f);
            ImGui::DragFloat("##evTime", &ev.time, 0.005f, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) quitarEvento = k;
            ImGui::PopID();
        }
        if (quitarEvento >= 0)
            stMut.events.erase(stMut.events.begin() + quitarEvento);
        if (ImGui::Button("+ evento##addEvent", ImVec2(140.0f, 0.0f)))
            stMut.events.push_back({ "", 0.5f });
        ImGui::PopID();
```

Si `stMut` está declarado dentro del `if`, subir su declaración delante del `if`.

- [ ] **Step 3: build, suite, CRLF y commit** `feat(editor): editar eventos de animación en el nodo del estado`

---

### Task 4: audit, spec y verificación manual

- [ ] **Step 1:** `docs/animation-audit.md`: fila 8 `✅ **HECHO** (2026-09-16, <hashes>)`; C8 pasa a **EXISTE** (por estado, solo nombre).
- [ ] **Step 2:** spec: la nota `[sin verificar]` de Pause se sustituye por `El editor no tiene Pause (comprobado al planificar): hoy no hay frames con Animator y sin scripts en Play.`
- [ ] **Step 3:** commit `docs: eventos de animación en audit y spec`.
- [ ] **Step 4: verificación manual (usuario), Vulkan y D3D12**
  1. En un estado, "+ evento", nombre `paso` y tiempo 0,5; Ctrl+Z y Ctrl+Y lo quitan y lo ponen.
  2. **Escribiendo el nombre, pulsar Supr: el nodo NO se borra.**
  3. Script en el personaje con `function Script:OnAnimationEvent(n) Log.Info(n) end`; en Play, un `paso` por ciclo en el Log.
  4. Guardar y recargar: el evento sigue.
