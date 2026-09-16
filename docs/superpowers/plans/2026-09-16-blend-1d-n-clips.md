# Blend 1D de N clips — plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (inline, elegido por el usuario). Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** que un estado del Animator mezcle N clips por umbrales de un parámetro float (blend 1D de Unity), sustituyendo al par `blendClip`.

**Architecture:** la CPU elige los dos clips vecinos del parámetro y los publica por la API de pose que ya existe (`poseClipA/B`, `poseTimeA/B`, `poseWeight`). No hay cambios en la GPU ni en los backends. Las escenas viejas se migran al cargar.

**Tech Stack:** C++20, nlohmann::json, ImGui + imgui-node-editor.

**Spec:** `docs/superpowers/specs/2026-09-16-blend-1d-n-clips-design.md` (`aebbe87`)

## Global Constraints

- Rama `feat/blend-1d`. Build con `.\build.bat` desde PowerShell; tests desde la raíz del repo (`build-ninja\engine\tests\dt_*.exe`).
- No se tocan shaders, `Renderer`, `D3D12Renderer` ni Lua.
- Edición de ficheros solo con Edit (el repo es CRLF). Commits con `git commit -F -` y el trailer `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`.
- Cada test nuevo lleva su sabotaje, **de uno en uno**, con script PowerShell: restaurar con `git checkout --` sobre un árbol commiteado, compilar con `cmd /c ".\build.bat > nul 2>&1"` y sin `$ErrorActionPreference="Stop"`.
- En los popups de un nodo: abrir entre `ed::Suspend()`/`ed::Resume()`. `PushID` por fila.

---

### Task 1: modelo, evaluación, serialización y panel (una sola unidad de compilación)

El cambio del `State` rompe a la vez `AnimatorComponent.cpp`, `Scene.cpp`, `AnimatorPanel.cpp` y los tests, así que va en un único commit verde.

**Files:**
- Modify: `engine/include/DonTopo/Core/AnimatorComponent.h` (`State` ~:106-118; privados ~:370-374)
- Modify: `engine/src/Core/AnimatorComponent.cpp` (`stateBlends`/`stateBlendWeight`/`pose*` :338-400; `rebindClips` :445-463; `renameClipReferences` :497)
- Modify: `engine/src/Core/Scene.cpp` (escritura :544-554, lectura :655-662)
- Modify: `engine/src/Editor/AnimatorPanel.cpp` (nodo :288-329, popup :577-620) y `engine/include/DonTopo/Editor/AnimatorPanel.h:84-86`
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Produces: `AnimatorComponent::BlendEntry { std::string clipName; int clipIndex = -1; float duration = 0.0f; float threshold = 0.0f; }`; en `State`: `std::string blendParam; float clipThreshold = 0.0f; std::vector<BlendEntry> blendEntries;`. Privado: `struct BlendPair { int clipA; float timeA; int clipB; float timeB; float weight; }; BlendPair stateBlendPair(int stateIdx) const;`. Salen `blendClipName`, `blendClipIndex`, `blendDuration`, `blendMin`, `blendMax` y `stateBlendWeight`.

- [ ] **Step 1: rama**

```bash
git checkout -b feat/blend-1d
```

- [ ] **Step 2: tests nuevos (antes del código)**

Añadir en `animator_tests.cpp`, justo después de `test_blend_state_pose_interpolates_between_clips`:

```cpp
// ── Blend 1D de N clips ──────────────────────────────────────────────────────
//
// Principal "Walk" (clip 0, umbral 1) EN MEDIO, y las entradas desordenadas a
// propósito: "Run" (clip 2, umbral 4) antes que "Idle" (clip 1, umbral 0). Los
// umbrales no están equiespaciados: un peso calculado con la posición en la
// lista en vez del umbral no pasaría.
static AnimatorComponent::BlendEntry entrada(const char* clip, int index, float duration, float threshold)
{
    AnimatorComponent::BlendEntry e;
    e.clipName = clip; e.clipIndex = index; e.duration = duration; e.threshold = threshold;
    return e;
}

static AnimatorComponent makeBlend1DGraph(float runThreshold = 4.0f, float idleThreshold = 0.0f)
{
    AnimatorComponent a;
    AnimatorComponent::State s;
    s.name = "Loco"; s.clipName = "Walk"; s.clipIndex = 0; s.duration = 40.0f;
    s.ticksPerSecond = 20.0f; s.loop = true;
    s.blendParam = "speed"; s.clipThreshold = 1.0f;
    s.blendEntries = { entrada("Run", 2, 100.0f, runThreshold), entrada("Idle", 1, 20.0f, idleThreshold) };
    a.addState(s);
    a.setEntryState(0);
    a.addParameter("speed", AnimatorComponent::ParamType::Float);
    a.reset();
    return a;
}

static void poner(AnimatorComponent& a, float speed)
{
    a.setFloat("speed", speed);
    a.update(0.0f, true);
}

// Entre dos umbrales: la pareja vecina con peso lineal, esté donde esté el
// principal en la lista.
static void test_blend1d_picks_neighbours_by_threshold()
{
    AnimatorComponent a = makeBlend1DGraph();
    poner(a, 2.5f);                              // entre Walk(1) y Run(4)
    CHECK(a.poseClipA() == 0);
    CHECK(a.poseClipB() == 2);
    CHECK(nearlyEqual(a.poseWeight(), 0.5f));    // (2.5-1)/(4-1)
    poner(a, 0.25f);                             // entre Idle(0) y Walk(1)
    CHECK(a.poseClipA() == 1);
    CHECK(a.poseClipB() == 0);
    CHECK(nearlyEqual(a.poseWeight(), 0.25f));
}

// Fuera de rango: solo el extremo, sin extrapolar.
static void test_blend1d_clamps_to_extremes()
{
    AnimatorComponent a = makeBlend1DGraph();
    poner(a, -5.0f);
    CHECK(a.poseClipA() == 1 && a.poseClipB() == 1);
    CHECK(nearlyEqual(a.poseWeight(), 1.0f));
    poner(a, 9.0f);
    CHECK(a.poseClipA() == 2 && a.poseClipB() == 2);
    CHECK(nearlyEqual(a.poseWeight(), 1.0f));
    poner(a, 4.0f);                              // justo en el último umbral
    CHECK(a.poseClipA() == 2 && a.poseClipB() == 2);
}

// Umbrales iguales: solo cuenta el primero (principal antes que entradas, y
// entre entradas, la anterior), por encima y por debajo.
static void test_blend1d_equal_thresholds_first_wins()
{
    AnimatorComponent a = makeBlend1DGraph(/*run=*/1.0f, /*idle=*/0.0f);   // Run empata con Walk
    poner(a, 3.0f);
    CHECK(a.poseClipA() == 0 && a.poseClipB() == 0);
    poner(a, 0.5f);
    CHECK(a.poseClipA() == 1 && a.poseClipB() == 0);
    CHECK(nearlyEqual(a.poseWeight(), 0.5f));

    AnimatorComponent b = makeBlend1DGraph(/*run=*/4.0f, /*idle=*/4.0f);   // Idle empata con Run
    poner(b, 5.0f);
    CHECK(b.poseClipA() == 2 && b.poseClipB() == 2);
    poner(b, 2.5f);
    CHECK(b.poseClipB() == 2);
}

// Una entrada sin clip resuelto no participa; sin ninguna válida, el estado es
// de un solo clip.
static void test_blend1d_ignores_unresolved_entries()
{
    AnimatorComponent a = makeBlend1DGraph();
    a.statesMutable()[0].blendEntries[1].clipIndex = -1;   // Idle sin resolver
    poner(a, 0.25f);
    CHECK(a.poseClipA() == 0 && a.poseClipB() == 0);
    CHECK(nearlyEqual(a.poseWeight(), 1.0f));

    a.statesMutable()[0].blendEntries[0].clipIndex = -1;   // y Run tampoco
    poner(a, 2.5f);
    CHECK(a.poseClipA() == 0 && a.poseClipB() == 0);
    CHECK(nearlyEqual(a.poseTimeB(), a.animTime()));
}

// Cada clip se muestrea en la fase del principal, escalada a SU duración.
static void test_blend1d_samples_every_clip_at_principal_phase()
{
    AnimatorComponent a = makeBlend1DGraph();
    a.setFloat("speed", 2.5f);
    a.update(0.5f, true);                        // 10 ticks de 40 = fase 0.25
    CHECK(nearlyEqual(a.poseTimeA(), 10.0f));    // Walk
    CHECK(nearlyEqual(a.poseTimeB(), 25.0f));    // Run: 0.25 * 100
    poner(a, 0.25f);
    CHECK(nearlyEqual(a.poseTimeA(), 5.0f));     // Idle: 0.25 * 20
    CHECK(nearlyEqual(a.poseTimeB(), 10.0f));    // Walk
}

// Renombrar toca las entradas; rebindClips sin el clip deja la entrada a -1 y
// NO la borra (el nombre se conserva para cuando vuelva el FBX).
static void test_blend1d_rename_and_rebind_entries()
{
    AnimatorComponent a = makeBlend1DGraph();
    CHECK(a.renameClipReferences("Run", "Sprint") == 1);
    CHECK(a.states()[0].blendEntries[0].clipName == "Sprint");

    SkinnedMesh mesh;
    AnimationClip walk; walk.name = "Walk"; walk.duration = 40.0f;
    AnimationClip idle; idle.name = "Idle"; idle.duration = 20.0f;
    mesh.animationClips = { walk, idle };
    std::vector<std::string> avisos;
    a.rebindClips(mesh, &avisos);
    CHECK(a.states()[0].blendEntries.size() == 2u);
    CHECK(a.states()[0].blendEntries[0].clipIndex == -1);
    CHECK(a.states()[0].blendEntries[1].clipIndex == 1);
    CHECK(nearlyEqual(a.states()[0].blendEntries[1].duration, 20.0f));
    CHECK(avisos.size() == 1u);
}
```

Y los de escena, junto a `test_blend_state_survives_scene_round_trip`:

```cpp
// Peso de cada clip en la pose (A == B cuenta como peso 1 a ese clip).
static float pesoDeClip(const AnimatorComponent& a, int clip)
{
    if (a.poseClipA() == a.poseClipB()) return a.poseClipA() == clip ? 1.0f : 0.0f;
    float w = 0.0f;
    if (a.poseClipA() == clip) w += 1.0f - a.poseWeight();
    if (a.poseClipB() == clip) w += a.poseWeight();
    return w;
}

// Una escena guardada con el par viejo (blendClip/blendMin/blendMax) carga con
// la MISMA pose: mismo peso por clip, con span positivo, negativo y 0. A/B no
// se comparan: con span negativo salen intercambiados.
static void test_blend1d_migrates_old_pair(PhysicsManager& pm, AudioManager& am)
{
    const float spans[][2] = { {1.5f, 6.5f}, {6.5f, 1.5f}, {2.0f, 2.0f} };
    for (const auto& mm : spans)
    {
        Scene scene("Test");
        GameObject* go = scene.addGameObject("Personaje");
        const uint64_t id = go->id;
        auto a = std::make_shared<AnimatorComponent>();
        AnimatorComponent::State s;
        s.name = "Loco"; s.clipName = "Walk"; s.blendParam = "speed";
        s.blendEntries = { entrada("Run", -1, 0.0f, 0.0f) };
        a->addState(s);
        a->addParameter("speed", AnimatorComponent::ParamType::Float);
        go->setAnimator(a);

        nlohmann::json j = scene.toJson();
        bool convertido = false;
        for (auto& node : j["root"]["children"])
        {
            if (!node.contains("animator")) continue;
            auto& js = node["animator"]["states"][0];
            js.erase("blendEntries");
            js.erase("clipThreshold");
            js["blendClip"] = "Run";
            js["blendMin"]  = mm[0];
            js["blendMax"]  = mm[1];
            convertido = true;
        }
        CHECK(convertido);

        Scene loaded("Loaded");
        CHECK(loaded.fromJson(j, pm, am));
        GameObject* found = loaded.findById(id);
        CHECK(found && found->hasAnimator());
        if (!found || !found->hasAnimator()) return;
        AnimatorComponent& b = *found->getAnimator();
        CHECK(b.states()[0].blendEntries.size() == 1u);
        if (b.states()[0].blendEntries.size() != 1u) return;
        // Sin malla: se resuelven a mano los índices.
        b.statesMutable()[0].clipIndex = 0;
        b.statesMutable()[0].duration  = 40.0f;
        b.statesMutable()[0].blendEntries[0].clipIndex = 1;
        b.statesMutable()[0].blendEntries[0].duration  = 40.0f;

        for (float p : { -1.0f, 1.5f, 2.0f, 3.0f, 5.0f, 6.5f, 9.0f })
        {
            const float span = mm[1] - mm[0];
            float wViejo = std::fabs(span) < 1e-6f ? 0.0f : (p - mm[0]) / span;
            wViejo = wViejo < 0.0f ? 0.0f : (wViejo > 1.0f ? 1.0f : wViejo);
            b.setFloat("speed", p);
            b.update(0.0f, true);
            CHECK(nearlyEqual(pesoDeClip(b, 0), 1.0f - wViejo));
            CHECK(nearlyEqual(pesoDeClip(b, 1), wViejo));
        }
    }
}

// Ida y vuelta del formato nuevo; al guardar no sale el formato viejo.
static void test_blend1d_scene_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>(makeBlend1DGraph());
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();
    const std::string texto = j.dump();
    CHECK(texto.find("\"blendClip\"") == std::string::npos);
    CHECK(texto.find("\"blendMin\"") == std::string::npos);

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    const auto& st = found->getAnimator()->states()[0];
    CHECK(st.blendParam == "speed");
    CHECK(nearlyEqual(st.clipThreshold, 1.0f));
    CHECK(st.blendEntries.size() == 2u);
    if (st.blendEntries.size() != 2u) return;
    CHECK(st.blendEntries[0].clipName == "Run" && nearlyEqual(st.blendEntries[0].threshold, 4.0f));
    CHECK(st.blendEntries[1].clipName == "Idle" && nearlyEqual(st.blendEntries[1].threshold, 0.0f));
}
```

Enganchar en `main` los ocho nuevos, tras las llamadas de blend existentes.

- [ ] **Step 3: migrar los tests existentes (regla mecánica)**

Donde un test construya el par viejo:
`X.blendClipName = N; X.blendClipIndex = I; X.blendDuration = D; X.blendMin = m; X.blendMax = M;`
pasa a
`X.clipThreshold = m; X.blendEntries = { entrada(N, I, D, M) };`
(los campos ausentes toman el default de `BlendEntry`). `entrada` tiene que declararse antes del primer uso: se sube junto a `makeBlendStateGraph`. Sitios: `makeBlendStateGraph` (:2458-2463), :2539-2540, :2581-2583, :3732-3735, :3774-3775, :4407-4408. Lecturas: `blendClipIndex` → `blendEntries[0].clipIndex` (:2590, :2592, :2614, :4616, y :4603 recorre `blendEntries`); `blendClipName` → `blendEntries[0].clipName`; `blendMin`/`blendMax` → `clipThreshold`/`blendEntries[0].threshold` (:3753-3759).

Tres cambios **de expectativa**, no mecánicos, porque cambia A/B pero no la pose:
- `test_blend_state_weight_from_float_param`: fuera de rango por abajo, `poseClipA()==0 && poseClipB()==0` y peso 1; por arriba, `poseClipA()==1 && poseClipB()==1` y peso 1. Las dos comprobaciones finales de A/B pasan a hacerse con `speed = 3.0`.
- `test_state_without_blend_fields_loads`: borra `blendEntries`, `clipThreshold` y `blendParam` en lugar de los cuatro campos viejos, y comprueba `st.blendEntries.empty()`.
- `graphMutations` (:4450-4453): se sustituyen las cuatro líneas por
```cpp
        { "state.blendEntry.clip",      [](A& a) { a.statesMutable()[0].blendEntries[0].clipName = "idle"; } },
        { "state.blendParam",           [](A& a) { a.statesMutable()[0].blendParam = "hits"; } },
        { "state.clipThreshold",        [](A& a) { a.statesMutable()[0].clipThreshold = 0.25f; } },
        { "state.blendEntry.threshold", [](A& a) { a.statesMutable()[0].blendEntries[0].threshold = 2.0f; } },
        { "state.addBlendEntry",        [](A& a) { a.statesMutable()[0].blendEntries.push_back(entrada("idle", -1, 0.0f, 3.0f)); } },
```
(requiere que `makeBaseGraph` tenga una entrada, por la regla mecánica de :4407).

- [ ] **Step 4: header**

En `AnimatorComponent.h`, antes de `struct State`:

```cpp
            // Un clip extra de un blend 1D. El nombre es la autoría; índice y
            // duración los resuelve rebindClips desde la malla y no se guardan.
            struct BlendEntry
            {
                std::string clipName;
                int         clipIndex = -1;
                float       duration  = 0.0f;   // ticks
                float       threshold = 0.0f;
            };
```

En `State`, sustituir el bloque `// --- Blend por parámetro ...` (desde `blendClipName` hasta `blendMax`) por:

```cpp
                // --- Blend 1D por parámetro ---
                // El clip principal (clipName) es una entrada más, con
                // clipThreshold; blendEntries son los extra. Sonarán los dos
                // vecinos del valor de blendParam (ver stateBlendPair). Sin
                // entradas, o con un parámetro no declarado, un solo clip.
                std::string             blendParam;
                float                   clipThreshold = 0.0f;
                std::vector<BlendEntry> blendEntries;
```

Privados: sustituir `stateBlends`/`stateBlendWeight` y sus comentarios por:

```cpp
            // true si hay al menos una entrada con clip RESUELTO y blendParam
            // es un Float declarado: solo entonces hay mezcla que hacer.
            bool stateBlends(int stateIdx) const;
            // Los dos clips vecinos del parámetro, sus tiempos y el peso.
            struct BlendPair { int clipA; float timeA; int clipB; float timeB; float weight; };
            BlendPair stateBlendPair(int stateIdx) const;
```

Actualizar el comentario de `poseClipA` (":301") a: `si no, estado con blend: los dos clips vecinos del parámetro entre sus umbrales.`

- [ ] **Step 5: evaluación (`AnimatorComponent.cpp:338-400`)**

```cpp
    bool AnimatorComponent::stateBlends(int stateIdx) const
    {
        if (stateIdx < 0 || stateIdx >= (int)m_states.size()) return false;
        const State& st = m_states[stateIdx];
        // Una entrada a -1 = el clip no existe en la malla (rebindClips ya
        // avisó): mezclar contra ella leería otro clip o fuera del SSBO.
        bool alguna = false;
        for (const auto& e : st.blendEntries)
            if (e.clipIndex >= 0) { alguna = true; break; }
        if (!alguna) return false;
        // Un parámetro no declarado devolvería 0.0f en getFloat y clavaría el
        // peso en un extremo sin decir por qué; mejor no mezclar.
        return hasParam(st.blendParam, ParamType::Float);
    }

    AnimatorComponent::BlendPair AnimatorComponent::stateBlendPair(int stateIdx) const
    {
        const int clip = currentClipIndex();
        BlendPair out{ clip, m_animTime, clip, m_animTime, 1.0f };
        if (!stateBlends(stateIdx)) return out;

        const State& st    = m_states[stateIdx];
        const float  p     = getFloat(st.blendParam);
        const float  phase = st.duration > 0.0f ? m_animTime / st.duration : 0.0f;

        // Vecino de abajo (mayor umbral <= p) y de arriba (menor umbral > p),
        // sin ordenar ni asignar memoria. -1 es el principal, que se mira
        // primero: la comparación ESTRICTA hace que con umbrales iguales se
        // quede el primero visto, que es la regla de la spec.
        const int kNinguno = -2;
        int   lo = kNinguno, hi = kNinguno;
        float loT = 0.0f,    hiT = 0.0f;
        auto considerar = [&](int k, float t) {
            if (t <= p) { if (lo == kNinguno || t > loT) { lo = k; loT = t; } }
            else        { if (hi == kNinguno || t < hiT) { hi = k; hiT = t; } }
        };
        considerar(-1, st.clipThreshold);
        for (int k = 0; k < (int)st.blendEntries.size(); k++)
            if (st.blendEntries[k].clipIndex >= 0)
                considerar(k, st.blendEntries[k].threshold);

        // Por debajo del primer umbral o por encima del último: solo el extremo.
        if (lo == kNinguno) lo = hi;
        if (hi == kNinguno) hi = lo;

        // Todos en la MISMA fase normalizada del principal: un walk de 40
        // ticks y un run de 100 mezclados por tiempo absoluto se desincronizan
        // y las piernas patinan.
        auto clipDe  = [&](int k) { return k < 0 ? st.clipIndex : st.blendEntries[k].clipIndex; };
        auto tiempoDe = [&](int k) { return k < 0 ? m_animTime : phase * st.blendEntries[k].duration; };
        out.clipA  = clipDe(lo);  out.timeA = tiempoDe(lo);
        out.clipB  = clipDe(hi);  out.timeB = tiempoDe(hi);
        out.weight = (lo == hi) ? 1.0f : (p - loT) / (hiT - loT);
        return out;
    }

    int AnimatorComponent::poseClipA() const
    {
        return blending() ? previousClipIndex() : stateBlendPair(m_currentState).clipA;
    }

    float AnimatorComponent::poseTimeA() const
    {
        return blending() ? m_prevAnimTime : stateBlendPair(m_currentState).timeA;
    }

    int AnimatorComponent::poseClipB() const
    {
        // Cross-fade: el destino aporta su clip PRIMARIO (solo caben dos clips).
        return blending() ? currentClipIndex() : stateBlendPair(m_currentState).clipB;
    }

    float AnimatorComponent::poseTimeB() const
    {
        return blending() ? m_animTime : stateBlendPair(m_currentState).timeB;
    }

    float AnimatorComponent::poseWeight() const
    {
        return blending() ? blendWeight() : stateBlendPair(m_currentState).weight;
    }
```

Nota: `p` justo en el primer umbral da la pareja (primero, segundo) con peso 0, que es la misma pose que "solo el primero".

- [ ] **Step 6: clips (`rebindClips` :445-463, `renameClipReferences` :497)**

Sustituir el `if (st.blendClipName.empty()) {...} else {...}` por:

```cpp
            // Las entradas del blend se resuelven SIEMPRE, aunque el primario
            // falle: los avisos son independientes y ver solo uno mandaría a
            // buscar al sitio equivocado. Una entrada recién añadida en el
            // editor todavía no tiene clip: no es un error, no avisa.
            for (auto& e : st.blendEntries)
            {
                const int b = e.clipName.empty() ? -1 : findClip(e.clipName);
                e.clipIndex = b;
                e.duration  = (b >= 0) ? mesh.animationClips[b].duration : 0.0f;
                if (b < 0 && !e.clipName.empty() && warnings)
                    warnings->push_back("Animator: el estado '" + st.name + "' mezcla con el clip '" +
                                        e.clipName + "', que no existe en el modelo");
            }
```

Y en el rename, sustituir la línea de `blendClipName` por:

```cpp
            for (auto& e : st.blendEntries)
                if (e.clipName == oldName) { e.clipName = newName; touched = true; }
```

- [ ] **Step 7: `Scene.cpp`**

Escritura (:544-554), sustituir el `if (!s.blendClipName.empty()) {...}` por:

```cpp
            if (!s.blendEntries.empty())
            {
                sj["blendParam"]    = s.blendParam;
                sj["clipThreshold"] = s.clipThreshold;
                nlohmann::json entradas = nlohmann::json::array();
                for (const auto& e : s.blendEntries)
                    entradas.push_back({ {"clip", e.clipName}, {"threshold", e.threshold} });
                sj["blendEntries"] = std::move(entradas);
            }
```

(ajustar el comentario de encima: `clipIndex/duration de cada entrada NO se guardan`).

Lectura (:655-662), sustituir las cinco asignaciones por:

```cpp
                const std::string ctxBlend = "animator.state." + st.name;
                st.blendParam = s.value("blendParam", std::string());
                if (s.contains("blendEntries") && s["blendEntries"].is_array())
                {
                    st.clipThreshold = readFloat(s, "clipThreshold", 0.0f, warnings, ctxBlend);
                    for (const auto& ej : s["blendEntries"])
                    {
                        if (!ej.is_object()) continue;
                        AnimatorComponent::BlendEntry e;
                        e.clipName  = ej.value("clip", std::string());
                        e.threshold = readFloat(ej, "threshold", 0.0f, warnings, ctxBlend + ".blendEntries");
                        st.blendEntries.push_back(std::move(e));
                    }
                }
                else if (!s.value("blendClip", std::string()).empty())
                {
                    // Formato anterior a N clips: el par (clip, blendClip) con
                    // [blendMin, blendMax]. Como umbrales dan la MISMA pose,
                    // incluidos span negativo (A/B salen intercambiados con el
                    // peso complementario) y span 0 (el empate descarta la
                    // entrada, como el peso 0 de antes).
                    st.clipThreshold = readFloat(s, "blendMin", 0.0f, warnings, ctxBlend);
                    AnimatorComponent::BlendEntry e;
                    e.clipName  = s.value("blendClip", std::string());
                    e.threshold = readFloat(s, "blendMax", 1.0f, warnings, ctxBlend);
                    st.blendEntries.push_back(std::move(e));
                }
```

- [ ] **Step 8: panel**

`AnimatorPanel.h:84-86`: añadir `int m_blendPickEntry = -1;   // entrada de blendEntries para kind 0; -1 = añadir una nueva` y actualizar el comentario de `m_blendPickKind` (`0 clip de una entrada`).

Nodo (:288-329), sustituir el bloque del blend por:

```cpp
        // --- Blend 1D: clips extra con su umbral ---
        // Cada fila abre la lista de TODOS los clips de la malla (ver
        // drawBlendPickPopup). Una entrada sin clip resuelto sale en rojo.
        auto& stMut = anim->statesMutable()[i];
        if (const SkinnedMesh* mesh = go->getSkinnedMesh())
        {
            (void)mesh;
            if (!stMut.blendEntries.empty())
            {
                // Solo parámetros float: son los únicos que dan un peso
                // continuo. Que la lista salga vacía es la pista de que hay
                // que declarar uno abajo, en Parameters.
                const std::string paramLabel = stMut.blendParam.empty()
                                               ? std::string("(sin parametro)") : stMut.blendParam;
                if (ImGui::Button(("by: " + paramLabel + "##by").c_str(), ImVec2(140.0f, 0.0f)))
                {
                    m_blendPickRequested = true;
                    m_blendPickEditorId  = eid;
                    m_blendPickKind      = 1;
                }
                ImGui::SetNextItemWidth(60.0f);
                ImGui::DragFloat("umbral principal##clipThr", &stMut.clipThreshold, 0.01f);
            }

            int quitar = -1;
            for (int k = 0; k < (int)stMut.blendEntries.size(); k++)
            {
                auto& e = stMut.blendEntries[k];
                ImGui::PushID(k);
                const bool roto = e.clipIndex < 0;
                if (roto) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                const std::string lbl = e.clipName.empty() ? std::string("(elige clip)") : e.clipName;
                if (ImGui::Button((lbl + "##clip").c_str(), ImVec2(90.0f, 0.0f)))
                {
                    m_blendPickRequested = true;
                    m_blendPickEditorId  = eid;
                    m_blendPickKind      = 0;
                    m_blendPickEntry     = k;
                }
                if (roto) ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(50.0f);
                ImGui::DragFloat("##thr", &e.threshold, 0.01f);
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) quitar = k;
                ImGui::PopID();
            }
            if (quitar >= 0)
                stMut.blendEntries.erase(stMut.blendEntries.begin() + quitar);

            if (ImGui::Button("+ blend clip##addBlend", ImVec2(140.0f, 0.0f)))
            {
                // Umbral por encima de todos: la entrada nueva no roba el peso
                // a las que ya estaban hasta que el usuario lo mueva.
                float maxT = stMut.clipThreshold;
                for (const auto& e : stMut.blendEntries) maxT = std::max(maxT, e.threshold);
                AnimatorComponent::BlendEntry nueva;
                nueva.threshold = maxT + 1.0f;
                stMut.blendEntries.push_back(nueva);
            }
        }
```

(Si `mesh` no se usa en el bloque, quitar el `if` con la variable y dejar `if (go->getSkinnedMesh())`.)

Popup, rama `m_blendPickKind == 0` (:577-604), sustituir por:

```cpp
    else if (m_blendPickKind == 0)
    {
        // Lista TODOS los clips de la malla, no solo los que ya usa el grafo:
        // el motor admite cualquiera de ellos. La entrada puede haber
        // desaparecido (se quitó con la "x" con el popup abierto).
        if (m_blendPickEntry < 0 || m_blendPickEntry >= (int)st.blendEntries.size())
        {
            ImGui::CloseCurrentPopup();
        }
        else
        {
            auto& e = st.blendEntries[m_blendPickEntry];
            bool cambio = false;
            for (const auto& c : mesh->animationClips)
            {
                if (ImGui::Selectable(c.name.c_str(), c.name == e.clipName))
                {
                    e.clipName = c.name;
                    cambio = true;
                }
            }
            // El índice y la duración los resuelve rebindClips por nombre; sin
            // esto la entrada quedaría a -1 hasta recargar la escena.
            // rebindClips y no bindClips: puede estar corriendo Play Mode y
            // bindClips reiniciaría el grafo y los parámetros del usuario.
            if (cambio)
                anim->rebindClips(*mesh, nullptr);
        }
    }
```

Se deja de filtrar el clip principal: en un 1D, el mismo clip con otro umbral es legal (hace de meseta). Si `#include <algorithm>` falta para `std::max`, añadirlo.

- [ ] **Step 9: compilar y pasar toda la suite**

```powershell
.\build.bat 2>&1 | Select-String "error C" | Select-Object -First 10; $LASTEXITCODE
Get-ChildItem build-ninja\engine\tests\dt_*.exe | ForEach-Object { & $_.FullName *> $null; if ($LASTEXITCODE -ne 0) { "FALLA $($_.Name)" } }
```

Esperado: build 0, sin FALLA, 28/28. `grep -rn "blendClipName\|blendMin\|blendMax\|stateBlendWeight" engine sandbox` no debe devolver nada salvo el comentario de la migración en `Scene.cpp`.

- [ ] **Step 10: comprobar CRLF y hacer commit**

```bash
[ "$(git diff --stat | tail -1)" = "$(git diff --ignore-cr-at-eol --stat | tail -1)" ] && echo "CRLF OK"
git add -u && git commit -F - <<'EOF'
feat(animator): blend 1D de N clips por umbrales

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
```

- [ ] **Step 11: sabotajes (uno a uno, script PowerShell)**

| # | Sabotaje en | Cambio | Debe caer |
|---|---|---|---|
| S1 | `stateBlendPair` | `t > loT` → `t >= loT` | `equal_thresholds_first_wins` |
| S2 | `stateBlendPair` | `t < hiT` → `t <= hiT` | `equal_thresholds_first_wins` |
| S3 | `stateBlendPair` | `if (st.blendEntries[k].clipIndex >= 0)` → `if (true)` | `ignores_unresolved_entries` |
| S4 | `stateBlendPair` | `phase * st.blendEntries[k].duration` → `m_animTime` | `samples_every_clip_at_principal_phase` |
| S5 | `stateBlendPair` | `if (lo == kNinguno) lo = hi;` → borrar | `clamps_to_extremes` (o crash: vale como caída) |
| S6 | `stateBlendPair` | `(p - loT) / (hiT - loT)` → `(p - loT)` | `picks_neighbours_by_threshold` |
| S7 | `rebindClips` | `e.clipIndex = b;` → `e.clipIndex = 0;` | `rename_and_rebind_entries` |
| S8 | `renameClipReferences` | borrar el bucle de entradas | `rename_and_rebind_entries` |
| S9 | `Scene.cpp` lectura | `readFloat(s, "blendMin", ...)` → `readFloat(s, "blendMax", ...)` | `migrates_old_pair` |
| S10 | `Scene.cpp` escritura | `{"threshold", e.threshold}` → `{"threshold", 0.0f}` | `scene_round_trip` |

Reportar cada uno con sus FAIL; los que no caigan se investigan antes de seguir.

---

### Task 2: documentación, audit y verificación manual

**Files:**
- Modify: `README.md` (:35 y la sección del Animator, ~:681 y ~:710)
- Modify: `docs/animation-audit.md` (fila 7 del backlog y C2)

- [ ] **Step 1: README**

Donde dice que un estado puede "blend two of its clips by a float parameter" (~:681), sustituir por: `a state can also be a **1D blend** of any number of its clips: each clip has a threshold on a float parameter, and the two clips around the parameter's value are mixed linearly (below the first threshold or above the last, only that clip plays). Scenes saved with the old two-clip blend load unchanged.` Y en :35, si menciona "two clips", cambiarlo a `1D blend of N clips`.

- [ ] **Step 2: audit**

Fila 7 del backlog: `✅ **HECHO** (2026-09-16, <hash>)` con los tests que lo cubren. C2 pasa de PARTIAL a **EXISTS (1D)**, y el 2D sigue en la fila 14.

- [ ] **Step 3: commit**

```bash
git add -u && git commit -F - <<'EOF'
docs: blend 1D de N clips en README y audit

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
```

- [ ] **Step 4: verificación manual (usuario), Vulkan y D3D12**

1. Estado con Walk de principal (umbral 1), y entradas Idle (0) y Run (4); parámetro float `speed`.
2. En Play, cambiar `speed` de 0 a 5 desde Parameters: idle → walk → run sin saltos.
3. "x" sobre una entrada con su popup abierto: no crashea.
4. Ctrl+Z (fuera de Play) de añadir entrada, cambiar clip, mover umbral y quitar entrada.
5. Cargar una escena guardada antes de esta rama con el blend viejo: se ve igual.
