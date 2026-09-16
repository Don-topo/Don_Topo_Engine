# Root motion — plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (inline, elegido por el usuario). Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** que el desplazamiento horizontal de la raíz de un clip mueva al GameObject (por transform, o por velocidad si tiene un Rigidbody dinámico) y la GPU deje de avanzar la raíz en X y Z.

**Architecture:** el Animator publica, por update y solo en Play, qué tramos de reloj avanzó cada clip y con qué peso (`rootMotionSamples`). Una función pura de Renderer convierte eso en un delta con los keyframes de la malla, y otra de Core lo aplica al GameObject. `applySkinnedFrame` las encadena. El shader gana un tercer modo de bloqueo.

**Tech Stack:** C++20, GLSL (`shaders/bone_eval.comp`, compartido por Vulkan y D3D12), PhysX vía `Rigidbody`.

**Spec:** `docs/superpowers/specs/2026-09-16-root-motion-design.md` (`0b037df`)

## Global Constraints

- Rama `feat/root-motion`. Build `.\build.bat` desde PowerShell; tests desde la raíz.
- Editar con Edit o Python con `newline=''` (CRLF). Commits con `git commit -F -` y el trailer `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`.
- Sabotajes de uno en uno por script PowerShell (`git checkout --` sobre árbol commiteado, `cmd /c ".\build.bat > nul 2>&1"`, sin `Stop`).
- Paridad Vulkan/D3D12: el shader es uno; en los backends solo cambia el tipo del modo.
- **Resuelto al planificar** (el `[sin verificar]` de la spec): `Scene.cpp:3739-3741` ya empuja la pose del GameObject al actor kinematic con `setKinematicTarget`, así que mover el transform basta.
- `ModelLoader.cpp:56` asigna `BoneChannel::boneIndex` con `skel.boneMap` final: los índices de canal son los del esqueleto ya ordenado.

---

### Task 1: modo de raíz (enum, GPU, serialización y panel)

Rompe a la vez Core, los dos backends, el panel y los tests: un único commit verde.

**Files:**
- Modify: `engine/include/DonTopo/Core/AnimatorComponent.h:154` (campo), `:338` (`poseLockRootMotion`)
- Modify: `engine/src/Core/AnimatorComponent.cpp:423-429`
- Modify: `engine/src/Core/Scene.cpp:559-560`, `:694`
- Modify: `engine/src/Editor/AnimatorPanel.cpp:262-268`
- Modify: `engine/include/DonTopo/Renderer/EditorRenderer.h:185-191`, `Renderer.h:475-479`, `D3D12/D3D12Renderer.h:181`, `RenderObjects.h:154`, `SkinnedFrameSync.h:55`
- Modify: `engine/src/Renderer/Renderer.cpp:3980-3995`, `D3D12/D3D12Renderer.cpp:975, 3575, 9987-10005`, `Passes/SkinningPass.cpp:194`
- Modify: `shaders/bone_eval.comp:114-122`
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Produces: `enum class AnimatorComponent::RootMotion { Off, Lock, Apply };`, `State::rootMotion`, `uint32_t AnimatorComponent::poseRootMotionMode() const` (0/1/2). `setAnimationBlend(..., float weight, uint32_t rootMotionMode = 0)`. `RenderObjects`/D3D12 `uint32_t rootMotionMode`.

- [ ] **Step 1: rama** — `git checkout -b feat/root-motion`

- [ ] **Step 2: Core.** En el header, antes de `struct State`:

```cpp
            // Qué hace la traslación de la raíz del clip. Off: la pose la
            // mueve (lo de siempre). Lock: se clava a su bind y el clip se ve
            // en el sitio. Apply: la GPU clava solo X y Z (el vaivén vertical
            // se ve) y el desplazamiento horizontal mueve al GameObject.
            enum class RootMotion { Off, Lock, Apply };
```

`bool lockRootMotion = false;` (con su comentario) pasa a `RootMotion rootMotion = RootMotion::Off;`, y el comentario queda: `Off es lo que traen todas las escenas guardadas sin el campo.`

`bool poseLockRootMotion() const;` → `uint32_t poseRootMotionMode() const;` y en el .cpp:

```cpp
    uint32_t AnimatorComponent::poseRootMotionMode() const
    {
        // Durante un cross-fade manda el estado DESTINO, que ES m_currentState
        // (el que aporta poseClipB): no hay caso especial que escribir.
        if (m_currentState < 0 || m_currentState >= (int)m_states.size()) return 0u;
        return (uint32_t)m_states[m_currentState].rootMotion;
    }
```

- [ ] **Step 3: shader** (`bone_eval.comp`, `applyRootLock`):

```glsl
// Único punto de override de la raíz, compartido por el camino rápido y el de
// mezcla. Modo 1: toda la traslación a la de bind (el clip en el sitio). Modo
// 2 (root motion): solo X y Z a bind; la Y se queda para que el vaivén del
// paso se vea, y el avance lo pone el GameObject. Otros huesos, tal cual.
vec3 applyRootLock(BoneInfo info, vec3 pos)
{
    if (info.parentIndex >= 0) return pos;
    if (push.lockRootMotion == 1u) return info.bindLocal[3].xyz;
    if (push.lockRootMotion == 2u) return vec3(info.bindLocal[3].x, pos.y, info.bindLocal[3].z);
    return pos;
}
```

(el nombre `lockRootMotion` del push constant se conserva: el layout no cambia).

- [ ] **Step 4: backends (tipo).** `bool lockRootMotion` → `uint32_t rootMotionMode` en `EditorRenderer.h`, `Renderer.h`, `D3D12Renderer.h` (default `= 0`), `RenderObjects.h:154` y el struct de `D3D12Renderer.cpp:975`. Asignaciones: `obj.rootMotionMode = rootMotionMode;` (`Renderer.cpp:3995`), `obj.rootMotionMode = 0;` (`:3980`), lo mismo en `D3D12Renderer.cpp:9987/10005`. Push: `push.lockRootMotion = obj.rootMotionMode;` (`SkinningPass.cpp:194`) y `push.lockRootMotion = object.rootMotionMode;` (`D3D12Renderer.cpp:3575`). Los comentarios de `EditorRenderer.h:185` y `Renderer.h:475` pasan a: `rootMotionMode: 0 pose libre, 1 raíz clavada a bind, 2 solo X y Z clavadas (root motion)`. `SkinnedFrameSync.h:55`: `anim->poseRootMotionMode()`.

- [ ] **Step 5: `Scene.cpp`.** Escritura:

```cpp
            // Modo de raíz: solo si no es Off.
            if (s.rootMotion == AnimatorComponent::RootMotion::Lock)  sj["rootMotion"] = "lock";
            if (s.rootMotion == AnimatorComponent::RootMotion::Apply) sj["rootMotion"] = "apply";
```

Lectura, sustituyendo `st.lockRootMotion = ...`:

```cpp
                // "rootMotion" desde el root motion real; antes, un bool
                // lockRootMotion que equivale a Lock. Ausentes: Off.
                const std::string rm = s.value("rootMotion", std::string());
                if (rm == "lock")                       st.rootMotion = AnimatorComponent::RootMotion::Lock;
                else if (rm == "apply")                 st.rootMotion = AnimatorComponent::RootMotion::Apply;
                else if (!rm.empty())
                {
                    if (warnings) warnings->push_back("animator.state." + st.name + ": rootMotion '" + rm + "' desconocido, se usa normal");
                }
                else if (s.value("lockRootMotion", false)) st.rootMotion = AnimatorComponent::RootMotion::Lock;
```

- [ ] **Step 6: panel** (sustituye el checkbox de `:262-268`):

```cpp
        // Modo de la raíz. RadioButton y no combo: una lista dentro del nodo
        // se abre en espacio de canvas (ver drawBlendPickPopup).
        {
            using RM = AnimatorComponent::RootMotion;
            int modo = (int)states[i].rootMotion;
            ImGui::TextUnformatted("raiz:");
            ImGui::SameLine(); if (ImGui::RadioButton("normal##rm", modo == 0)) modo = 0;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("La pose mueve la raiz: el clip se desplaza con su animacion.");
            ImGui::SameLine(); if (ImGui::RadioButton("bloq.##rm", modo == 1)) modo = 1;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clava la traslacion de la raiz a su bind pose: el clip se reproduce en el sitio.");
            ImGui::SameLine(); if (ImGui::RadioButton("root motion##rm", modo == 2)) modo = 2;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("El avance horizontal de la raiz mueve al GameObject (con Rigidbody dinamico, como velocidad). La Y se queda en la pose.");
            if (modo != (int)states[i].rootMotion)
                anim->statesMutable()[i].rootMotion = (RM)modo;
        }
```

- [ ] **Step 7: tests existentes (reglas).**
- `X.lockRootMotion = true` → `X.rootMotion = AnimatorComponent::RootMotion::Lock`; `= false` → `RootMotion::Off`; `= lockFrom` → `lockFrom ? RootMotion::Lock : RootMotion::Off` (`:4179`, `:4184`, `:4253`, `:4255`, `:4295`).
- `CHECK(a.poseLockRootMotion())` → `CHECK(a.poseRootMotionMode() == 1u)`; `!...` → `== 0u` (`:4211-4241`).
- `:4223`: `a.poseLockRootMotion()` → `a.poseRootMotionMode() == 1u` (esa función de referencia sigue siendo bool).
- `:4267-4268`: `contains("lockRootMotion")` → `contains("rootMotion")`; `:4281-4282`: `st[0].rootMotion == RootMotion::Lock` / `st[1].rootMotion == RootMotion::Off`; `:4305`: `erase("rootMotion")`; `:4319`: `st.rootMotion == RootMotion::Off`.
- `graphMutations` `:4689`: `{ "state.rootMotion", [](A& a) { a.statesMutable()[0].rootMotion = A::RootMotion::Apply; } },`
- `SkinnedRendererDoble`: `bool lockRoot` → `uint32_t rootMode = 0xFFFFFFFFu;`, firma `uint32_t mode`, y `:5114` `CHECK(r.rootMode == a->poseRootMotionMode());`.

Nuevo, junto a los tests de bloqueo:

```cpp
// El bool viejo lockRootMotion carga como Lock; "apply" va y vuelve.
static void test_root_motion_mode_migration(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State s;
    s.name = "Viejo"; s.clipName = "Run"; a->addState(s);
    s.name = "Nuevo"; s.clipName = "Run"; s.rootMotion = AnimatorComponent::RootMotion::Apply; a->addState(s);
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();
    bool tocado = false;
    for (auto& node : j["root"]["children"])
    {
        if (!node.contains("animator")) continue;
        CHECK(node["animator"]["states"][1]["rootMotion"] == "apply");
        node["animator"]["states"][0]["lockRootMotion"] = true;
        tocado = true;
    }
    CHECK(tocado);

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    const auto& st = found->getAnimator()->states();
    CHECK(st[0].rootMotion == AnimatorComponent::RootMotion::Lock);
    CHECK(st[1].rootMotion == AnimatorComponent::RootMotion::Apply);
}
```

- [ ] **Step 8: build (compila shaders), suite, CRLF, commit** `feat(animator): modo de raiz Off/Lock/Apply con X y Z clavadas en GPU para root motion`.

- [ ] **Step 9: sabotajes**

| # | Cambio | Debe caer |
|---|---|---|
| S1 | `Scene.cpp`: `else if (s.value("lockRootMotion", false)) st.rootMotion = ...Lock;` → borrar | `root_motion_mode_migration` |
| S2 | `Scene.cpp`: `sj["rootMotion"] = "apply";` → `= "lock";` | `root_motion_mode_migration` |
| S3 | `SkinnedFrameSync.h`: `anim->poseRootMotionMode()` → `0u` | `apply_skinned_frame_passes_pose_b_then_a` |

---

### Task 2: muestras del Animator y delta en CPU

**Files:**
- Create: `engine/include/DonTopo/Renderer/RootMotion.h`, `engine/src/Renderer/RootMotion.cpp`
- Modify: `engine/CMakeLists.txt` (tras `src/Renderer/SkinnedMeshPacking.cpp`)
- Modify: `AnimatorComponent.h` (`BlendPair`, `m_stateTicks`, público), `AnimatorComponent.cpp` (`stateBlendPair`, `update`, `startTransitionTo`)
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Produces:
  - `struct AnimatorComponent::RootMotionSample { int clip; double ticks0; double ticks1; float duration; bool loop; float weight; };`
  - `const std::vector<RootMotionSample>& rootMotionSamples() const;`
  - `glm::vec3 sampleRootPosition(const SkinnedMesh&, int clipIndex, double t);`
  - `glm::vec3 rootDisplacement(const SkinnedMesh&, int clipIndex, double T, double duration, bool loop);`
  - `glm::vec3 rootMotionDelta(const SkinnedMesh&, const AnimatorComponent&);`

- [ ] **Step 1: tests**

```cpp
// ── Root motion ──────────────────────────────────────────────────────────────
//
// Malla sintética: esqueleto de un hueso raíz y clips cuya raíz avanza en X
// linealmente `avance` unidades por ciclo de `dur` ticks (y a 1 de altura, con
// Y creciente en el clip 2 para comprobar que se descarta).
static SkinnedMesh makeRootMotionMesh()
{
    SkinnedMesh m;
    m.skeleton.names = { "Hips" };
    m.skeleton.parentIndex = { -1 };
    m.skeleton.inverseBindPose = { glm::mat4(1.0f) };
    m.skeleton.boneMap["Hips"] = 0;
    auto clip = [](const char* n, float dur, glm::vec3 fin) {
        AnimationClip c; c.name = n; c.duration = dur; c.ticksPerSecond = 20.0f;
        BoneChannel ch; ch.boneIndex = 0;
        ch.posKeys = { { 0.0f, glm::vec3(0.0f, 1.0f, 0.0f) }, { dur, fin } };
        c.channels.push_back(ch);
        return c;
    };
    m.animationClips = { clip("Walk", 40.0f, glm::vec3(10.0f, 1.0f, 0.0f)),
                         clip("Run",  80.0f, glm::vec3(30.0f, 1.0f, 0.0f)),
                         clip("Sube", 40.0f, glm::vec3(10.0f, 5.0f, 0.0f)) };
    return m;
}

static void test_root_displacement_counts_cycles()
{
    const SkinnedMesh m = makeRootMotionMesh();
    CHECK(nearlyEqual(rootDisplacement(m, 0, 40.0, 40.0, true).x, 10.0f));
    CHECK(nearlyEqual(rootDisplacement(m, 0, 100.0, 40.0, true).x, 25.0f));
    CHECK(nearlyEqual(rootDisplacement(m, 0, 100.0, 40.0, false).x, 10.0f));
    CHECK(nearlyEqual(rootDisplacement(m, 0, 0.0, 40.0, true).x, 0.0f));
}

static AnimatorComponent makeRootMotionGraph(int clip = 0, float dur = 40.0f)
{
    AnimatorComponent a;
    AnimatorComponent::State s;
    s.name = "Walk"; s.clipName = "Walk"; s.clipIndex = clip; s.duration = dur;
    s.ticksPerSecond = 20.0f; s.loop = true; s.rootMotion = AnimatorComponent::RootMotion::Apply;
    a.addState(s);
    a.setEntryState(0);
    a.reset();
    return a;
}

// Un update que cruza el wrap da el avance real, sin salto negativo.
static void test_root_motion_delta_across_wrap()
{
    const SkinnedMesh m = makeRootMotionMesh();
    AnimatorComponent a = makeRootMotionGraph();
    a.update(1.9f, true);                          // 38 ticks
    a.update(0.2f, true);                          // 38 -> 42
    CHECK(nearlyEqual(rootMotionDelta(m, a).x, 1.0f));
}

// Solo X y Z: la Y de la raíz se queda en la pose.
static void test_root_motion_delta_drops_y()
{
    const SkinnedMesh m = makeRootMotionMesh();
    AnimatorComponent a = makeRootMotionGraph(2);
    a.update(1.0f, true);                          // 20 ticks: Y sube 2
    const glm::vec3 d = rootMotionDelta(m, a);
    CHECK(nearlyEqual(d.x, 5.0f));
    CHECK(nearlyEqual(d.y, 0.0f));
}

// Blend 1D: cada clip en su fase y el delta ponderado.
static void test_root_motion_delta_blend_weighted()
{
    const SkinnedMesh m = makeRootMotionMesh();
    AnimatorComponent a = makeRootMotionGraph();
    auto& s = a.statesMutable()[0];
    s.blendParam = "speed"; s.clipThreshold = 0.0f;
    s.blendEntries = { entrada("Run", 1, 80.0f, 1.0f) };
    a.addParameter("speed", AnimatorComponent::ParamType::Float);
    a.setFloat("speed", 0.5f);
    a.update(1.0f, true);                          // fase 0,5: Walk 5, Run 15
    CHECK(nearlyEqual(rootMotionDelta(m, a).x, 10.0f));
}

// Cross-fade entre dos Apply: 1 − w del que sale, w del que entra.
static void test_root_motion_delta_crossfade_weighted()
{
    const SkinnedMesh m = makeRootMotionMesh();
    AnimatorComponent a = makeRootMotionGraph();
    AnimatorComponent::State b;
    b.name = "Run"; b.clipName = "Run"; b.clipIndex = 1; b.duration = 40.0f;   // 30 por cada 40 ticks
    b.ticksPerSecond = 20.0f; b.loop = true; b.rootMotion = AnimatorComponent::RootMotion::Apply;
    a.addState(b);
    a.addParameter("go", AnimatorComponent::ParamType::Trigger);
    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1; t.duration = 2.0f;
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = "go";
    t.conditions.push_back(c);
    a.addTransition(t);

    a.setTrigger("go");
    a.update(0.016f, true);                        // sale a Run con fade de 2 s
    a.update(0.5f, true);                          // w = 0,25; 10 ticks cada uno
    // Walk: 10 ticks = 2,5. Run (clip de 80 ticks estirado a 40): 10 ticks de
    // la malla = 3,75. Delta = 0,75·2,5 + 0,25·3,75.
    CHECK(a.blending());
    CHECK(nearlyEqual(rootMotionDelta(m, a).x, 0.75f * 2.5f + 0.25f * 3.75f));
}

// Sin Apply, o en Edit, no hay muestras.
static void test_root_motion_samples_only_in_play_with_apply()
{
    AnimatorComponent a = makeRootMotionGraph();
    a.update(0.5f, false);
    CHECK(a.rootMotionSamples().empty());
    a.statesMutable()[0].rootMotion = AnimatorComponent::RootMotion::Lock;
    a.update(0.5f, true);
    CHECK(a.rootMotionSamples().empty());
}

// Espacio: en el FBX real la Y de la raíz es su mayor componente (altura de
// cadera). Si falla, las claves de la raíz no están en espacio de modelo con Y
// arriba y el diseño hay que revisarlo, no el test.
static void test_root_motion_space_is_model_y_up()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    CHECK(!m.animationClips.empty());
    if (m.animationClips.empty()) return;
    const glm::vec3 p = sampleRootPosition(m, 0, 0.0);
    CHECK(std::fabs(p.y) > std::fabs(p.x));
    CHECK(std::fabs(p.y) > std::fabs(p.z));
}
```

`#include "DonTopo/Renderer/RootMotion.h"` arriba; llamadas en `main` junto a las de eventos.

Nota: el clip Run de 80 ticks usado por un estado con `duration = 40` es a propósito: `rootDisplacement` recibe la duración de la **muestra** (la del estado para el principal), no la del clip. Así se ve que la fórmula de ciclos usa `P(duration)` interpolado. Con `duration = 40` sobre claves hasta 80, `P(40) − P(0) = 15` por ciclo y 10 ticks dan 3,75.

- [ ] **Step 2: Animator.**

Header, público junto a `firedEvents`:

```cpp
            // Lo que avanzó cada clip con root motion en el ÚLTIMO update (solo
            // Play y solo si el estado actual es Apply), con su peso en la pose.
            // Ticks acumulados, sin wrap. Lo convierte en delta rootMotionDelta.
            struct RootMotionSample { int clip; double ticks0; double ticks1; float duration; bool loop; float weight; };
            const std::vector<RootMotionSample>& rootMotionSamples() const { return m_rootMotionSamples; }
```

`BlendPair` gana `float durA = 0.0f; float durB = 0.0f;` al final. Privados junto a `m_firedEvents`: `std::vector<RootMotionSample> m_rootMotionSamples;` y `double m_prevStateTicks = 0.0;`.

`stateBlendPair`: `BlendPair out{ clip, m_animTime, clip, m_animTime, 1.0f, st_duration, st_duration };` con la duración del estado actual (si el índice es válido; si no, 0), y en la rama de mezcla `auto durDe = [&](int k) { return k < 0 ? st.duration : st.blendEntries[k].duration; }; out.durA = durDe(lo); out.durB = durDe(hi);`.

`update`: al principio, junto a `m_firedEvents.clear();`, `m_rootMotionSamples.clear();`. Antes de avanzar el reloj previo, `const double prevTicks0 = m_prevStateTicks;`, y dentro del `if (m_prevState >= 0)`, junto a su `advanceClock`: `m_prevStateTicks += (double)dt * stateRate(m_states[m_prevState]);`. Tras el bloque de eventos:

```cpp
        if (conDuracion && actual.rootMotion == RootMotion::Apply)
            collectRootMotion(ticks0, prevTicks0);
```

Nuevo privado `void collectRootMotion(double ticks0, double prevTicks0);`:

```cpp
    void AnimatorComponent::collectRootMotion(double ticks0, double prevTicks0)
    {
        const State& st = m_states[m_currentState];
        // En un fade la pose usa el clip PRIMARIO de cada lado (ver poseClipB):
        // el movimiento sale de lo mismo que se ve.
        if (blending())
        {
            const float w = blendWeight();
            m_rootMotionSamples.push_back({ st.clipIndex, ticks0, m_stateTicks, st.duration, st.loop, w });
            if (m_prevState >= 0 && m_prevState < (int)m_states.size())
            {
                const State& prev = m_states[m_prevState];
                if (prev.rootMotion == RootMotion::Apply && prev.duration > 0.0f)
                    m_rootMotionSamples.push_back({ prev.clipIndex, prevTicks0, m_prevStateTicks,
                                                    prev.duration, prev.loop, 1.0f - w });
            }
            return;
        }
        const BlendPair bp = stateBlendPair(m_currentState);
        // Cada clip del blend va en la fase del principal: sus ticks acumulados
        // son los del principal escalados a su duración.
        const double escA = st.duration > 0.0f ? bp.durA / st.duration : 0.0;
        const double escB = st.duration > 0.0f ? bp.durB / st.duration : 0.0;
        if (bp.clipA == bp.clipB)
        {
            m_rootMotionSamples.push_back({ bp.clipA, ticks0 * escA, m_stateTicks * escA, bp.durA, st.loop, 1.0f });
            return;
        }
        m_rootMotionSamples.push_back({ bp.clipA, ticks0 * escA, m_stateTicks * escA, bp.durA, st.loop, 1.0f - bp.weight });
        m_rootMotionSamples.push_back({ bp.clipB, ticks0 * escB, m_stateTicks * escB, bp.durB, st.loop, bp.weight });
    }
```

`startTransitionTo`, rama `duration > 0`: `m_prevStateTicks = m_stateTicks;` antes de `enterState`. Rama de corte seco: `m_prevStateTicks = 0.0;`.

- [ ] **Step 3: `RootMotion.h/.cpp`.**

```cpp
#pragma once
#include <glm/glm.hpp>

namespace DonTopo
{
    struct SkinnedMesh;
    class AnimatorComponent;

    // Root motion en CPU, sin GPU: posición y desplazamiento de la raíz de un
    // clip a partir de sus keyframes. La raíz es el primer hueso sin padre con
    // canal de posición en ese clip; sus claves están en espacio de modelo (la
    // jerarquía de la GPU no aplica nodos por encima de la raíz).

    // Posición de la raíz en t ticks, interpolada linealmente como bone_eval y
    // acotada a la primera y la última clave. Sin raíz con claves: (0,0,0).
    glm::vec3 sampleRootPosition(const SkinnedMesh& mesh, int clipIndex, double t);

    // Desplazamiento desde 0 hasta T ticks acumulados de un reloj de duración
    // `duration`. En loop suma un P(duration) − P(0) por cada ciclo completo.
    glm::vec3 rootDisplacement(const SkinnedMesh& mesh, int clipIndex, double T, double duration, bool loop);

    // Delta horizontal (y = 0) del último update del Animator, en espacio de
    // modelo, ponderando sus rootMotionSamples.
    glm::vec3 rootMotionDelta(const SkinnedMesh& mesh, const AnimatorComponent& anim);
}
```

```cpp
#include "DonTopo/Renderer/RootMotion.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Core/AnimatorComponent.h"
#include <cmath>

namespace DonTopo
{
    namespace
    {
        const BoneChannel* rootChannel(const SkinnedMesh& mesh, int clipIndex)
        {
            if (clipIndex < 0 || clipIndex >= (int)mesh.animationClips.size()) return nullptr;
            for (const auto& ch : mesh.animationClips[clipIndex].channels)
            {
                if (ch.posKeys.empty()) continue;
                if (ch.boneIndex < 0 || ch.boneIndex >= (int)mesh.skeleton.parentIndex.size()) continue;
                if (mesh.skeleton.parentIndex[ch.boneIndex] < 0) return &ch;
            }
            return nullptr;
        }
    }

    glm::vec3 sampleRootPosition(const SkinnedMesh& mesh, int clipIndex, double t)
    {
        const BoneChannel* ch = rootChannel(mesh, clipIndex);
        if (!ch) return glm::vec3(0.0f);
        const auto& k = ch->posKeys;
        if (t <= k.front().time) return k.front().value;
        if (t >= k.back().time)  return k.back().value;
        for (size_t i = 0; i + 1 < k.size(); i++)
        {
            if (k[i + 1].time > t)
            {
                const double span = k[i + 1].time - k[i].time;
                const float  f    = span > 0.0 ? (float)((t - k[i].time) / span) : 0.0f;
                return glm::mix(k[i].value, k[i + 1].value, f);
            }
        }
        return k.back().value;
    }

    glm::vec3 rootDisplacement(const SkinnedMesh& mesh, int clipIndex, double T, double duration, bool loop)
    {
        if (duration <= 0.0 || T <= 0.0) return glm::vec3(0.0f);
        const glm::vec3 p0 = sampleRootPosition(mesh, clipIndex, 0.0);
        if (!loop)
            return sampleRootPosition(mesh, clipIndex, std::min(T, duration)) - p0;
        const double ciclos = std::floor(T / duration);
        const double resto  = T - ciclos * duration;
        const glm::vec3 porCiclo = sampleRootPosition(mesh, clipIndex, duration) - p0;
        return (float)ciclos * porCiclo + (sampleRootPosition(mesh, clipIndex, resto) - p0);
    }

    glm::vec3 rootMotionDelta(const SkinnedMesh& mesh, const AnimatorComponent& anim)
    {
        glm::vec3 d(0.0f);
        for (const auto& s : anim.rootMotionSamples())
            d += s.weight * (rootDisplacement(mesh, s.clip, s.ticks1, s.duration, s.loop)
                           - rootDisplacement(mesh, s.clip, s.ticks0, s.duration, s.loop));
        d.y = 0.0f;
        return d;
    }
}
```

CMake: añadir `src/Renderer/RootMotion.cpp` tras `src/Renderer/SkinnedMeshPacking.cpp` y reconfigurar si hace falta (`configure.bat`).

- [ ] **Step 4: build, suite, CRLF, commit** `feat(animator): muestras de root motion y delta en CPU`.

- [ ] **Step 5: sabotajes**

| # | Cambio | Debe caer |
|---|---|---|
| S4 | `rootDisplacement`: `(float)ciclos * porCiclo +` → borrar | `root_displacement_counts_cycles`, `delta_across_wrap` |
| S5 | `rootDisplacement`: `std::min(T, duration)` → `T` y quitar `if (!loop)` completo | `root_displacement_counts_cycles` |
| S6 | `rootMotionDelta`: `d.y = 0.0f;` → borrar | `delta_drops_y` |
| S7 | `collectRootMotion`: `ticks0 * escB, m_stateTicks * escB` → `ticks0, m_stateTicks` | `delta_blend_weighted` |
| S8 | `collectRootMotion`: `1.0f - w` → `1.0f` | `delta_crossfade_weighted` |
| S9 | `update`: quitar `&& actual.rootMotion == RootMotion::Apply` | `samples_only_in_play_with_apply` |
| S10 | `startTransitionTo`: `m_prevStateTicks = m_stateTicks;` → `= 0.0;` | `delta_crossfade_weighted` |

---

### Task 3: aplicación al GameObject

**Files:**
- Create: `engine/include/DonTopo/Core/RootMotionApply.h`, `engine/src/Core/RootMotionApply.cpp`
- Modify: `engine/CMakeLists.txt` (tras `src/Core/GameObject.cpp`), `engine/include/DonTopo/Renderer/SkinnedFrameSync.h`
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `rootMotionDelta`, `rootMotionSamples`.
- Produces: `void applyRootMotion(GameObject& go, const glm::vec3& deltaModel, float dt);`

- [ ] **Step 1: tests**

```cpp
// Sin Rigidbody: el transform avanza el delta en mundo (con la escala del
// objeto) y, con padre rotado, la posición local lo compensa.
static void test_apply_root_motion_moves_transform()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    go->localTransform = glm::scale(glm::mat4(1.0f), glm::vec3(0.5f));
    scene.getRoot().updateWorldTransforms();
    applyRootMotion(*go, glm::vec3(4.0f, 9.0f, 0.0f), 0.016f);
    CHECK(nearlyEqual(go->worldTransform[3].x, 2.0f));
    CHECK(nearlyEqual(go->worldTransform[3].y, 0.0f));

    GameObject* padre = scene.addGameObject("Padre");
    padre->localTransform = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 1, 0));
    GameObject* hijo = scene.addGameObject("Hijo", padre);
    scene.getRoot().updateWorldTransforms();
    applyRootMotion(*hijo, glm::vec3(1.0f, 0.0f, 0.0f), 0.016f);
    // X del hijo en modelo = −Z en mundo (rotación de 90° en Y del padre).
    CHECK(nearlyEqual(hijo->worldTransform[3].z, -1.0f));
    CHECK(nearlyEqual(hijo->worldTransform[3].x, 0.0f));
}

// Con Rigidbody dinámico: velocidad X/Z = delta/dt y la Y se conserva; el
// transform no se toca.
static void test_apply_root_motion_sets_rigidbody_velocity(PhysicsManager& pm)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto rb  = std::make_shared<Rigidbody>();
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    go->setRigidbody(rb);
    rb->setVelocity(glm::vec3(0.0f, -3.0f, 0.0f));
    scene.getRoot().updateWorldTransforms();

    applyRootMotion(*go, glm::vec3(0.5f, 0.0f, 0.25f), 0.5f);
    const glm::vec3 v = rb->getVelocity();
    CHECK(nearlyEqual(v.x, 1.0f));
    CHECK(nearlyEqual(v.z, 0.5f));
    CHECK(nearlyEqual(v.y, -3.0f));
    CHECK(nearlyEqual(go->worldTransform[3].x, 0.0f));
}
```

Si `addGameObject` no acepta padre, usar la API de jerarquía que usen otros tests del fichero (`grep -n "setParent\|addChild" engine/tests/animator_tests.cpp`). Si la build no tiene PhysX (`DT_PHYSX_ENABLED`), `getVelocity` devuelve 0: envolver el test en `#ifdef DT_PHYSX_ENABLED` igual que hagan los tests de física. Llamadas en `main`.

- [ ] **Step 2: implementación**

```cpp
#pragma once
#include <glm/glm.hpp>

namespace DonTopo
{
    class GameObject;

    // Aplica un delta de root motion (espacio de modelo del objeto) en mundo,
    // solo en horizontal. Con Rigidbody dinámico como velocidad en X y Z
    // (conserva la Y: gravedad y saltos), para que la física colisione; sin
    // Rigidbody o kinematic, sobre el transform (Scene::update ya empuja la
    // pose de un kinematic a PhysX).
    void applyRootMotion(GameObject& go, const glm::vec3& deltaModel, float dt);
}
```

```cpp
#include "DonTopo/Core/RootMotionApply.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Physics/Rigidbody.h"

namespace DonTopo
{
    void applyRootMotion(GameObject& go, const glm::vec3& deltaModel, float dt)
    {
        if (dt <= 0.0f || deltaModel == glm::vec3(0.0f)) return;
        glm::vec3 mundo = glm::mat3(go.worldTransform) * deltaModel;
        mundo.y = 0.0f;

        if (const auto& rb = go.getRigidbody(); rb && !rb->getIsKinematic())
        {
            const glm::vec3 v = rb->getVelocity();
            rb->setVelocity(glm::vec3(mundo.x / dt, v.y, mundo.z / dt));
            return;
        }

        const glm::mat4 padre = go.parent ? go.parent->worldTransform : glm::mat4(1.0f);
        go.localTransform[3] += glm::vec4(glm::inverse(glm::mat3(padre)) * mundo, 0.0f);
        // Este mismo frame: setSkinnedTransform viene justo después.
        go.updateWorldTransforms(padre);
    }
}
```

`SkinnedFrameSync.h`, tras `anim->update(dt, evaluateTransitions);`:

```cpp
            // Root motion antes de mandar el transform: el avance de este
            // update ya sale en la posición que recibe el backend.
            if (!anim->rootMotionSamples().empty())
                if (const SkinnedMesh* sk = go.getSkinnedMesh())
                    applyRootMotion(go, rootMotionDelta(*sk, *anim), dt);
```

con `#include "DonTopo/Renderer/RootMotion.h"` y `#include "DonTopo/Core/RootMotionApply.h"`. (`dt` es el del frame, no el escalado por la velocidad del Animator: la velocidad del avance ya va en los ticks).

- [ ] **Step 3: build, suite, CRLF, commit** `feat(animator): el root motion mueve al GameObject o a su Rigidbody`.

- [ ] **Step 4: sabotajes**

| # | Cambio | Debe caer |
|---|---|---|
| S11 | `mundo.y = 0.0f;` → borrar | `moves_transform` |
| S12 | `glm::inverse(glm::mat3(padre)) *` → borrar | `moves_transform` (hijo) |
| S13 | `v.y` → `0.0f` | `sets_rigidbody_velocity` |
| S14 | `!rb->getIsKinematic()` → `false` | `sets_rigidbody_velocity` |

---

### Task 4: documentación, audit y verificación manual

- [ ] README, sección del Animator: sustituir la mención al bloqueo por los tres modos (`normal`, `locked`, `root motion`: horizontal root travel moves the GameObject, as a velocity when it has a dynamic Rigidbody, while the vertical bob stays in the pose; rotation isn't applied).
- [ ] Audit: A6 y C12 a CERRADO/EXISTE (solo traslación horizontal); fila 12 HECHO con hashes, tests y sabotajes.
- [ ] Commit `docs: root motion en README y audit`.
- [ ] **Verificación manual (usuario), Vulkan y D3D12:**
  1. Personaje con un andar que avanza, estado en "root motion": en Play avanza sin patinar y se ve el vaivén vertical.
  2. Con padre rotado o escala distinta de 1: avanza hacia donde mira y a su escala.
  3. Con Rigidbody dinámico y collider: choca con una pared y cae con la gravedad.
  4. "bloq." sigue igual que antes; "normal" deja que la pose desplace el modelo.
  5. Ctrl+Z del cambio de modo; guardar y recargar lo conserva.
