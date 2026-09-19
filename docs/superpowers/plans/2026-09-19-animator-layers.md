# Capas del Animator (fila 14b) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Capas del Animator estilo Unity: una máquina de estados por capa, con peso, modo override/additive y máscara de huesos, evaluadas y combinadas en `bone_eval`.

**Architecture:** Primero un refactor sin cambios visibles (el estado del grafo pasa a `m_layers[0]`). Después la CPU (update por capa, `pose()` por capa, máscara resuelta), la serialización y el undo, la GPU (bloque de pose por personaje en un SSBO con copia por frame en vuelo, `bone_eval` por capas), el editor y Lua. El layout del bloque lo escribe un helper compartido (`PoseBlock.h`) que prueban los tests de CPU.

**Tech Stack:** C++17, glm, GLSL → SPIR-V / HLSL por spirv-cross, ImGui + imgui-node-editor, sol2.

**Spec:** `docs/superpowers/specs/2026-09-19-animator-layers-design.md`

## Global Constraints

- `kMaxLayers = 8`, `kMaxPoseSamplesPerLayer = 6`; capa 0 siempre Override, peso 1, sin máscara.
- Escena de una capa: se guarda idéntica a hoy, byte a byte.
- Push de los tres `.comp` = `boneCount, vertexCount, rootMotionMode, poseBlockOffset` (16 bytes), espejado en `SkinningPass::Push` y `ComputePush`.
- Bloque de pose (uints): `[0] layerCount [1] sampleCount [2..3] pad`, capas en `4 + 4L` (`weight, mode, frozenWeight, hasMask`), muestras en `36 + 4k` (`clipBase, time, weight, layer`, k < 48), máscaras en `228 + L*boneCount + bone`.
- Build: `configure.bat` si hay ficheros nuevos, `build.bat` desde PowerShell; tests desde la raíz del repo. Ficheros CRLF (salvo los que ya sean LF): Edit o Python `newline=''`.
- Commit antes de cada sabotaje; sabotajes de uno en uno.
- Binding Lua nuevo ⇒ `ScriptBindings.cpp` + `LuaApiReference.cpp` + README `## Lua Scripting`.
- Trailer: `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`.

---

### Task 1: Refactor: el grafo pasa a `m_layers[0]` (sin cambios visibles)

**Files:** `engine/include/DonTopo/Core/AnimatorComponent.h`, `engine/src/Core/AnimatorComponent.cpp`

**Interfaces — Produces:**

```cpp
enum class LayerMode { Override, Additive };
static constexpr int kMaxLayers = 8;
struct Layer
{
    std::string              name = "Base Layer";
    std::vector<State>       states;
    std::vector<Transition>  transitions;
    int                      entryState = -1;
    float                    weight     = 1.0f;
    LayerMode                mode       = LayerMode::Override;
    std::vector<std::string> maskBones;
    glm::vec2                anyStatePos{ -220.0f, 40.0f };
    // --- ejecución (no se serializa) ---
    int      currentState   = -1;
    float    animTime       = 0.0f;
    bool     finished       = false;
    double   stateTicks     = 0.0;
    double   prevStateTicks = 0.0;
    int      prevState      = -1;
    float    prevAnimTime   = 0.0f;
    float    blendElapsed   = 0.0f;
    float    blendDuration  = 0.0f;
    bool     frozenFade     = false;
    bool     freezePending  = false;
    std::vector<uint8_t> maskResolved;   // uno por hueso; vacío = todo el cuerpo
};
```

- [ ] **Step 1:** en el header, declarar `LayerMode`, `kMaxLayers` y `Layer` (dentro de la clase, tras `Transition`), sustituir los miembros privados `m_states, m_transitions, m_entryState, m_currentState, m_animTime, m_finished, m_stateTicks, m_prevStateTicks, m_frozenFade, m_freezePending, m_prevState, m_prevAnimTime, m_blendElapsed, m_blendDuration, m_anyStateEditorPos` por `std::vector<Layer> m_layers = std::vector<Layer>(1);`, y añadir los accesos privados:

```cpp
            Layer&       lay(int i)       { return m_layers[(size_t)std::clamp(i, 0, (int)m_layers.size() - 1)]; }
            const Layer& lay(int i) const { return m_layers[(size_t)std::clamp(i, 0, (int)m_layers.size() - 1)]; }
```

- [ ] **Step 2:** script Python sobre header y `.cpp`: renombrar con `\b` y en orden de más largo a más corto `m_prevStateTicks→L.prevStateTicks, m_prevAnimTime→L.prevAnimTime, m_prevState→L.prevState, m_stateTicks→L.stateTicks, m_states→L.states, m_transitions→L.transitions, m_entryState→L.entryState, m_currentState→L.currentState, m_animTime→L.animTime, m_finished→L.finished, m_frozenFade→L.frozenFade, m_freezePending→L.freezePending, m_blendElapsed→L.blendElapsed, m_blendDuration→L.blendDuration, m_anyStateEditorPos→L.anyStatePos`.

- [ ] **Step 3:** a cada función que ahora usa `L.` se le añade `int layer = 0` como ÚLTIMO parámetro (en la declaración; la definición sin default) y en su primera línea `Layer& L = lay(layer);` (o `const Layer& L` si es `const`). Las inline del header pasan a `{ return lay(layer).campo; }`. Lista: `addState, addTransition, removeState, removeTransition, setEntryState, states, transitions, entryState, statesMutable, transitionsMutable, anyStateEditorPos, setAnyStateEditorPos, play, crossFade, normalizedTime, currentState, currentClipIndex, animTime, finished, previousState, previousClipIndex, previousAnimTime, blendWeight, blending, fading, currentStateName, previousStateName, stateBlends2D, stateBlendSamples` y las privadas `startTransitionTo, stateBlends, stateBlendPair, stateBlendPair1D, collectRootMotion`. `update`, `pose`, `poseClip*/poseTime*/poseWeight/poseRootMotionMode`, `reset`, `bindClips/rebindClips`, `renameClipReferences`, `applyGraph`, `clearFreezeRequest` y `removeParameter` usan `Layer& L = m_layers[0];` en este task (el bucle por capas llega en el Task 2). Las llamadas internas entre funciones de capa pasan `layer`.

- [ ] **Step 4:** build y la suite entera (29) en verde, sin tocar ningún test. `git diff --stat` solo en los dos ficheros del Animator (+ ninguno más: la API sin argumento es la de antes).

- [ ] **Step 5:** commit `refactor(animator): el grafo y su estado de ejecucion pasan a la capa 0`.

---

### Task 2: Capas en CPU (gestión, update, eventos, máscara, pose)

**Files:** `AnimatorComponent.h/.cpp`, `engine/include/DonTopo/Core/AnimationPose.h`, `engine/src/Renderer/Renderer.cpp` y `D3D12Renderer.cpp` (solo adaptar a la nueva forma de la pose), `engine/src/Renderer/Passes/SkinningPass.cpp` (ídem), `engine/tests/animator_tests.cpp`

**Interfaces — Produces:**

```cpp
// AnimationPose.h
static constexpr int kMaxLayersPose        = 8;   // == AnimatorComponent::kMaxLayers (static_assert en el .cpp)
static constexpr int kMaxPoseSamplesPerLayer = 6;
struct PoseSample { int clip = 0; float time = 0.0f; float weight = 0.0f; int layer = 0; };
struct PoseLayer
{
    float    weight = 1.0f;
    uint32_t mode   = 0;            // 0 override, 1 additive
    float    frozenWeight = 0.0f;
    bool     freezeNow    = false;
    const std::vector<uint8_t>* mask = nullptr;   // null = todo el cuerpo
};
struct AnimationPose
{
    PoseSample samples[kMaxLayersPose * kMaxPoseSamplesPerLayer] = {};
    int        count = 0;
    PoseLayer  layers[kMaxLayersPose] = {};
    int        layerCount = 1;
    uint32_t   rootMotionMode = 0;
};
// AnimatorComponent (públicos)
int   layerCount() const;
const Layer& layer(int i) const;
Layer&       layerMutable(int i);
int   addLayer(const std::string& name);          // -1 si ya hay kMaxLayers
void  removeLayer(int i);                          // no la 0
void  moveLayer(int from, int to);                 // ni desde ni hacia la 0
void  setLayerWeight(int i, float w);              // [0,1]; ignora la 0
float layerWeight(int i) const;                    // la 0 siempre 1
void  setLayerMode(int i, LayerMode m);            // ignora la 0
```

- [ ] **Step 1: tests (fallan: no existen)** — añadir antes de `int main()` y registrarlos:

```cpp
// Dos capas: base con "Idle"(clip 0) -> "Run"(clip 1) por trigger "go"; capa
// 1 "Brazos" con "Low"(clip 2) -> "Aim"(clip 3) por el MISMO trigger.
static AnimatorComponent makeTwoLayers()
{
    AnimatorComponent a;
    auto st = [](const char* n, int clip) {
        AnimatorComponent::State s;
        s.name = n; s.clipName = n; s.clipIndex = clip; s.duration = 40.0f;
        s.ticksPerSecond = 20.0f; s.loop = true;
        return s;
    };
    a.addParameter("go", AnimatorComponent::ParamType::Trigger);
    auto trans = [&](int layer) {
        AnimatorComponent::Transition t;
        t.fromState = 0; t.toState = 1; t.duration = 0.5f;
        AnimatorComponent::Condition c;
        c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = "go";
        t.conditions.push_back(c);
        a.addTransition(t, layer);
    };
    a.addState(st("Idle", 0)); a.addState(st("Run", 1)); a.setEntryState(0); trans(0);
    const int l1 = a.addLayer("Brazos");
    a.addState(st("Low", 2), l1); a.addState(st("Aim", 3), l1); a.setEntryState(0, l1); trans(l1);
    a.reset();
    return a;
}

static void test_layers_management()
{
    AnimatorComponent a = makeTwoLayers();
    CHECK(a.layerCount() == 2);
    CHECK(a.layer(1).name == "Brazos");
    a.setLayerWeight(0, 0.3f);  CHECK(nearlyEqual(a.layerWeight(0), 1.0f));   // la base no cambia
    a.setLayerWeight(1, 1.7f);  CHECK(nearlyEqual(a.layerWeight(1), 1.0f));
    a.setLayerWeight(1, 0.25f); CHECK(nearlyEqual(a.layerWeight(1), 0.25f));
    for (int i = 2; i < AnimatorComponent::kMaxLayers; i++) CHECK(a.addLayer("x") == i);
    CHECK(a.addLayer("sobra") == -1);
    a.removeLayer(0);            CHECK(a.layerCount() == AnimatorComponent::kMaxLayers);
    a.moveLayer(1, 0);           CHECK(a.layer(1).name == "Brazos");
    a.moveLayer(1, 2);           CHECK(a.layer(2).name == "Brazos");
    a.removeLayer(2);            CHECK(a.layerCount() == AnimatorComponent::kMaxLayers - 1);
}

static void test_layers_trigger_fires_in_both_layers()
{
    AnimatorComponent a = makeTwoLayers();
    a.update(0.1f, true);
    a.setTrigger("go");
    a.update(0.1f, true);
    CHECK(a.currentState(0) == 1);
    CHECK(a.currentState(1) == 1);
    CHECK(a.fading(0) && a.fading(1));
}

static void test_layers_clocks_independent()
{
    AnimatorComponent a = makeTwoLayers();
    a.update(0.5f, true);
    a.play("Aim", 1);                     // solo la capa 1 reinicia su reloj
    a.update(0.25f, true);
    CHECK(nearlyEqual(a.animTime(0), 15.0f));
    CHECK(nearlyEqual(a.animTime(1), 5.0f));
}

static void test_layers_pose_tags_and_weights()
{
    AnimatorComponent a = makeTwoLayers();
    a.setLayerWeight(1, 0.4f);
    a.setLayerMode(1, AnimatorComponent::LayerMode::Additive);
    a.update(0.1f, true);
    AnimationPose p = a.pose();
    CHECK(p.layerCount == 2);
    CHECK(p.count == 2);
    CHECK(p.samples[0].layer == 0 && p.samples[0].clip == 0);
    CHECK(p.samples[1].layer == 1 && p.samples[1].clip == 2);
    CHECK(nearlyEqual(p.layers[1].weight, 0.4f));
    CHECK(p.layers[1].mode == 1u);
    a.setLayerWeight(1, 0.0f);
    p = a.pose();
    CHECK(p.layerCount == 2 && p.count == 1);            // la capa sigue, sin muestras
    CHECK(nearlyEqual(p.layers[1].weight, 0.0f));
}

static void test_layers_events_and_root_motion()
{
    AnimatorComponent a = makeTwoLayers();
    a.statesMutable(1)[0].events = { { "golpe", 0.1f } };
    a.statesMutable(1)[0].rootMotion = AnimatorComponent::RootMotion::Apply;
    a.update(0.3f, true);                                // 6 ticks: fase 0.15
    CHECK(std::find(a.firedEvents().begin(), a.firedEvents().end(), "golpe") != a.firedEvents().end());
    CHECK(a.rootMotionSamples().empty());                // root motion solo de la base
    a.reset();
    a.setLayerWeight(1, 0.0f);
    a.update(0.3f, true);
    CHECK(a.firedEvents().empty());                      // capa a peso 0 no dispara
}

static void test_layers_mask_resolution()
{
    SkinnedMesh m;
    m.skeleton.names       = { "hips", "spine", "arm", "hand", "leg" };
    m.skeleton.parentIndex = { -1, 0, 1, 2, 0 };
    m.skeleton.inverseBindPose.assign(5, glm::mat4(1.0f));
    AnimatorComponent a = makeTwoLayers();
    a.layerMutable(1).maskBones = { "arm", "hand", "noExiste" };
    std::vector<std::string> avisos;
    a.bindClips(m, &avisos);
    const auto& r = a.layer(1).maskResolved;
    CHECK(r.size() == 5u);
    if (r.size() == 5u) CHECK(r[0] == 0 && r[1] == 0 && r[2] == 1 && r[3] == 1 && r[4] == 0);
    bool avisado = false;
    for (const auto& w : avisos) if (w.find("noExiste") != std::string::npos) avisado = true;
    CHECK(avisado);
    a.layerMutable(1).maskBones.clear();
    a.rebindClips(m, nullptr);
    CHECK(a.layer(1).maskResolved.empty());
    const AnimationPose p = a.pose();
    CHECK(p.layers[1].mask == nullptr);
}
```

(`events` es `std::vector<AnimationEvent>` con `{name, time}`, time normalizado.) Los tests de la fila 13 que leen `p.frozenWeight` / `p.freezeNow` pasan a `p.layers[0].frozenWeight` / `p.layers[0].freezeNow` (búsqueda y sustitución en `animator_tests.cpp`), y el doble `SkinnedRendererDoble` no cambia.

- [ ] **Step 2: build → falla.**

- [ ] **Step 3: implementar.**
  - `AnimationPose.h` con la forma de arriba.
  - Gestión: `addLayer` hace `push_back(Layer{})` con `name` y devuelve el índice (−1 si `size() == kMaxLayers`); `removeLayer`/`moveLayer` rechazan el índice 0 y fuera de rango (`moveLayer` también `to == 0`); `setLayerWeight` acota con `std::clamp(w, 0.0f, 1.0f)`; `setLayerMode`.
  - `update(dt, evaluate)`: vacía eventos y root motion, y recorre `for (int li = 0; li < layerCount(); li++) updateLayer(li, dt, evaluate, consumir);` donde `updateLayer` es el cuerpo actual con `Layer& L = m_layers[li]`, que en vez de llamar a `consumeTriggers(*elegida)` hace `consumir.push_back(elegida)`, que dispara `collectEvents` solo si `li == 0 || L.weight > 0`, y `collectRootMotion` solo si `li == 0`. Tras el bucle: `for (auto* t : consumir) consumeTriggers(*t);`.
  - `reset()`: para cada capa, el reset de hoy.
  - `bindClips`/`rebindClips`/`renameClipReferences`: el cuerpo actual en un bucle por capas. Además, al final de `bindClips` y `rebindClips`, resolver máscaras:

```cpp
        for (auto& L : m_layers)
        {
            L.maskResolved.clear();
            if (L.maskBones.empty()) continue;
            L.maskResolved.assign(mesh.skeleton.names.size(), 0);
            for (const auto& nombre : L.maskBones)
            {
                auto it = std::find(mesh.skeleton.names.begin(), mesh.skeleton.names.end(), nombre);
                if (it == mesh.skeleton.names.end())
                {
                    if (warnings) warnings->push_back("Animator: la máscara de la capa '" + L.name +
                                                      "' nombra el hueso '" + nombre + "', que el modelo no tiene");
                    continue;
                }
                L.maskResolved[(size_t)(it - mesh.skeleton.names.begin())] = 1;
            }
        }
```

  - `pose()`: `out.layerCount = layerCount()`; para cada capa `li`, rellena `out.layers[li] = { li == 0 ? 1.0f : L.weight, li == 0 ? 0u : (uint32_t)L.mode, 0.0f, L.freezePending, L.maskResolved.empty() ? nullptr : &L.maskResolved }`; si `li > 0 && L.weight <= 0` no emite muestras; si no, el cuerpo actual (frozen / blending / estado solo) con `add` que pone `layer = li` y cuenta el tope por capa (`kMaxPoseSamplesPerLayer`) y `frozenWeight` en `out.layers[li]`. `clearFreezeRequest()` apaga `freezePending` de todas.
  - Backends (compilar sin cambiar el comportamiento aún): en `SkinningPass.cpp` y `D3D12Renderer.cpp` (`pushDe` y la congelación), `pose.freezeNow` → `pose.layers[0].freezeNow`, `pose.frozenWeight` → `pose.layers[0].frozenWeight`, y solo se copian al push las muestras con `layer == 0` (hasta 6). `setAnimationPose` en los dos backends no cambia (copia el struct).

- [ ] **Step 4:** build; suite entera en verde.
- [ ] **Step 5:** commit `feat(animator): capas con su propia maquina de estados, peso, modo y mascara`.
- [ ] **Step 6: sabotajes:** S1 consumir el trigger dentro de la capa (como antes) → cae `trigger_fires_in_both_layers`; S2 root motion de todas las capas → cae `events_and_root_motion`; S3 eventos sin mirar el peso → cae `events_and_root_motion`; S4 capa de peso 0 emite muestras → cae `pose_tags_and_weights`; S5 máscara sin resolver en `rebindClips` → cae `mask_resolution`; S6 `setLayerWeight` sin ignorar la 0 → cae `management`.

---

### Task 3: Serialización, grafo y undo

**Files:** `engine/src/Core/Scene.cpp:527-800` (`animatorToJson`, `animatorGraphKey`, `animatorFromJson`), `AnimatorComponent.h/.cpp` (`Graph`, `applyGraph`), `engine/tests/animator_tests.cpp`

**Interfaces — Produces:** `Graph` gana `std::vector<Layer> extraLayers;` (solo campos de diseño; `applyGraph` conserva la ejecución de las capas con el mismo índice, con la regla de hoy, y resetea las nuevas).

- [ ] **Step 1: tests**

```cpp
static void test_layers_serialization(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>(makeTwoLayers());
    const int l2 = a->addLayer("Respirar");
    a->setLayerWeight(1, 0.6f);
    a->setLayerMode(l2, AnimatorComponent::LayerMode::Additive);
    a->layerMutable(1).maskBones = { "arm", "hand" };
    go->setAnimator(a);
    nlohmann::json j = scene.toJson();
    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    const auto& b = *found->getAnimator();
    CHECK(b.layerCount() == 3);
    if (b.layerCount() != 3) return;
    CHECK(b.layer(1).name == "Brazos" && nearlyEqual(b.layer(1).weight, 0.6f));
    CHECK(b.layer(1).maskBones.size() == 2u);
    CHECK(b.layer(1).states.size() == 2u && b.layer(1).transitions.size() == 1u);
    CHECK(b.layer(2).mode == AnimatorComponent::LayerMode::Additive);
    CHECK(b.layer(2).maskBones.empty());
}

// Una capa: ninguna clave nueva en el JSON.
static void test_layers_single_layer_json_unchanged()
{
    AnimatorComponent a = makeTwoBlendStates();
    const nlohmann::json j = animatorToJson(a);
    CHECK(!j.contains("layers"));
    AnimatorComponent b = makeTwoLayers();
    CHECK(animatorToJson(b).contains("layers"));
}

static void test_layers_too_many_warns(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto a = std::make_shared<AnimatorComponent>(makeTwoLayers());
    go->setAnimator(a);
    nlohmann::json j = scene.toJson();
    for (auto& node : j["root"]["children"])
        if (node.contains("animator"))
            while (node["animator"]["layers"].size() < 9) node["animator"]["layers"].push_back(node["animator"]["layers"][0]);
    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    bool avisado = false;
    for (const auto& w : loaded.lastWarnings()) if (w.find("capas") != std::string::npos) avisado = true;
    CHECK(avisado);
}

// El undo de la capa: la clave del grafo cambia al tocar peso, modo o máscara.
static void test_layers_graph_key_sees_layer_edits()
{
    AnimatorComponent a = makeTwoLayers();
    const auto k0 = animatorGraphKey(a);
    a.setLayerWeight(1, 0.5f);
    const auto k1 = animatorGraphKey(a);
    CHECK(k0 != k1);
    a.layerMutable(1).maskBones = { "arm" };
    CHECK(animatorGraphKey(a) != k1);
}
```

(`animatorToJson`/`animatorGraphKey` están declarados en `AnimatorSerialization.h`; los avisos de carga se leen con `Scene::lastWarnings()`.)

- [ ] **Step 2:** build → falla.
- [ ] **Step 3: implementar.**
  - Extraer de `animatorToJson` dos helpers: `statesToJson(const std::vector<State>&)` y `transitionsToJson(const std::vector<Transition>&)` (el código de hoy, sin cambios). `animatorToJson` los usa para la capa 0 (claves de hoy) y, solo si `layerCount() > 1`, añade `"layers"` con, por capa 1..N: `{"name", "weight", "mode": "override"|"additive", "mask" (si no vacía), "entryState", "states", "transitions", "anyStatePos"}`.
  - `animatorGraphKey`: también borra `pos` de los estados y `anyStatePos` de cada capa de `"layers"`.
  - `animatorFromJson`: extraer la lectura de estados y transiciones a helpers que reciben la capa destino; tras la capa 0, si hay `"layers"` (array), por cada elemento hasta `kMaxLayers - 1`: `addLayer(name)`, peso, modo, máscara, estados, transiciones, entrada. Si hay más: aviso `"Animator: el fichero trae N capas; se cargan las " + kMaxLayers`.
  - `Graph::extraLayers` y `applyGraph`: la capa 0 como hoy; las demás, se sustituyen los campos de diseño de las capas con el mismo índice (conservando su ejecución si su estado actual sigue siendo válido, con la misma regla que la capa 0), se añaden las nuevas y se quitan las sobrantes. El sitio que construye `Graph` desde la clave (el comando del undo) rellena `extraLayers` desde el JSON con el mismo helper de lectura.
- [ ] **Step 4:** build; suite en verde; una escena de una capa guardada da el mismo JSON que en `main` (`test_layers_single_layer_json_unchanged` + la suite de escena).
- [ ] **Step 5:** commit `feat(animator): serializacion y undo de las capas`.
- [ ] **Step 6: sabotajes:** S7 escribir `"layers"` siempre → cae `single_layer_json_unchanged`; S8 no leer la máscara → cae `serialization`; S9 la clave borra `"layers"` → cae `graph_key_sees_layer_edits`; S10 sin tope al cargar → cae `too_many_warns`.

---

### Task 4: GPU por capas (bloque de pose, `bone_eval`)

**Files:**
- Create: `engine/include/DonTopo/Renderer/PoseBlock.h`
- Modify: `shaders/bone_eval.comp`, `shaders/bone_hierarchy.comp`, `shaders/skinning.comp`, `SkinningPass.h/.cpp`, `RenderObjects.h`, `Renderer.cpp`, `D3D12Renderer.cpp`
- Test: `engine/tests/animator_tests.cpp`

**Interfaces — Produces:**

```cpp
// PoseBlock.h — layout del bloque de pose que lee bone_eval (binding 10).
namespace DonTopo {
    constexpr uint32_t kPoseBlockLayers  = 4;     // offset de las capas
    constexpr uint32_t kPoseBlockSamples = 36;    // offset de las muestras
    constexpr uint32_t kPoseBlockMasks   = 228;   // offset de las máscaras
    inline uint32_t poseBlockUints(uint32_t boneCount) { return kPoseBlockMasks + kMaxLayersPose * boneCount; }
    // Escribe la pose en dst (poseBlockUints(boneCount) uints). clipBase = clip * boneCount.
    void writePoseBlock(const AnimationPose& pose, uint32_t boneCount, uint32_t* dst);
}
```

(implementación inline en el header, con `std::memcpy` para los floats).

- [ ] **Step 1: tests del layout (fallan: no existe el header) + referencia en CPU de la combinación**

```cpp
static void test_pose_block_layout()
{
    AnimationPose p;
    p.layerCount = 2;
    p.count = 2;
    p.samples[0] = { 3, 12.5f, 1.0f, 0 };
    p.samples[1] = { 1, 4.0f, 1.0f, 1 };
    std::vector<uint8_t> mask = { 0, 1, 1 };
    p.layers[1] = { 0.75f, 1u, 0.25f, false, &mask };
    std::vector<uint32_t> b(poseBlockUints(3), 0xDEADBEEFu);
    writePoseBlock(p, 3, b.data());
    auto f = [&](uint32_t i) { float v; std::memcpy(&v, &b[i], 4); return v; };
    CHECK(b[0] == 2u && b[1] == 2u);
    CHECK(nearlyEqual(f(4 + 4), 0.75f) && b[4 + 5] == 1u && nearlyEqual(f(4 + 6), 0.25f) && b[4 + 7] == 1u);
    CHECK(b[4 + 3] == 0u);                                        // capa 0 sin máscara
    CHECK(b[36] == 9u && nearlyEqual(f(37), 12.5f) && b[39] == 0u);
    CHECK(b[40] == 3u && b[43] == 1u);
    CHECK(b[228 + 3 + 0] == 0u && b[228 + 3 + 1] == 1u && b[228 + 3 + 2] == 1u);
}
```

  Referencia de la combinación, junto a `evalPoseTrs` (fila 13):

```cpp
// Lo que calcula bone_eval por capas para el hueso i: la capa 0 con
// evalPoseTrs; cada capa L >= 1 sobre ella, override (mix/nlerp) o additive
// (delta respecto al clip en t = 0), con m = peso * máscara.
static Trs evalLayeredTrs(const PackedClips& p, size_t boneCount, size_t i, const AnimationPose& pose)
{
    auto capa = [&](int L, bool additive) {
        Trs acc; acc.p = glm::vec3(0.0f); acc.s = glm::vec3(0.0f);
        glm::vec4 q(0.0f), ref(0.0f, 0.0f, 0.0f, 1.0f);
        bool hayRef = false, alguna = false;
        float total = 0.0f;
        for (int k = 0; k < pose.count; k++)
        {
            if (pose.samples[k].layer != L) continue;
            Trs t, t0;
            const size_t base = (size_t)pose.samples[k].clip * boneCount;
            const bool tiene = sampleTrs(p, base, i, pose.samples[k].time, t);
            if (additive)
            {
                sampleTrs(p, base, i, 0.0f, t0);
                const glm::quat qt(t.q.w, t.q.x, t.q.y, t.q.z), q0(t0.q.w, t0.q.x, t0.q.y, t0.q.z);
                const glm::quat dq = qt * glm::inverse(q0);
                t.p = t.p - t0.p;
                t.q = glm::vec4(dq.x, dq.y, dq.z, dq.w);
                t.s = glm::vec3(t0.s.x != 0.0f ? t.s.x / t0.s.x : 1.0f,
                                t0.s.y != 0.0f ? t.s.y / t0.s.y : 1.0f,
                                t0.s.z != 0.0f ? t.s.z / t0.s.z : 1.0f);
                if (!tiene) t = Trs{}, t.p = glm::vec3(0.0f);
            }
            if (tiene) alguna = true;
            glm::vec4 qk = t.q;
            if (!hayRef) { ref = additive ? glm::vec4(0, 0, 0, 1) : qk; hayRef = true; }
            if (glm::dot(qk, ref) < 0.0f) qk = -qk;
            const float w = pose.samples[k].weight;
            acc.p += w * t.p; acc.s += w * t.s; q += w * qk; total += w;
        }
        if (total > 0.0f && std::fabs(total - 1.0f) > 1e-4f) { acc.p /= total; acc.s /= total; q /= total; }
        const float len = glm::length(q);
        acc.q = len > 1e-6f ? q / len : glm::vec4(0, 0, 0, 1);
        acc.bind = !alguna && !additive;
        return acc;
    };
    Trs r = capa(0, false);
    if (r.bind)
    {
        // La base no anima el hueso: parte de su bindLocal descompuesto.
        const glm::mat4& bl = p.boneInfos[(size_t)pose.samples[0].clip * boneCount + i].bindLocal;
        glm::vec3 sk, tr; glm::quat rot; glm::vec3 skew; glm::vec4 persp;
        glm::decompose(bl, sk, rot, tr, skew, persp);
        r.p = tr; r.s = sk; r.q = glm::vec4(rot.x, rot.y, rot.z, rot.w); r.bind = false;
    }
    for (int L = 1; L < pose.layerCount; L++)
    {
        const PoseLayer& pl = pose.layers[L];
        const float m = pl.weight * ((pl.mask && i < pl.mask->size()) ? (float)(*pl.mask)[i] : (pl.mask ? 0.0f : 1.0f));
        if (m <= 0.0f) continue;
        const bool add = pl.mode == 1u;
        Trs c = capa(L, add);
        if (!add)
        {
            if (c.bind) continue;
            glm::vec4 qc = c.q;
            if (glm::dot(qc, r.q) < 0.0f) qc = -qc;
            r.p = glm::mix(r.p, c.p, m); r.s = glm::mix(r.s, c.s, m);
            r.q = glm::normalize(glm::mix(r.q, qc, m));
        }
        else
        {
            glm::vec4 dq = glm::normalize(glm::mix(glm::vec4(0, 0, 0, 1), c.q, m));
            const glm::quat d(dq.w, dq.x, dq.y, dq.z), b(r.q.w, r.q.x, r.q.y, r.q.z);
            const glm::quat out = glm::normalize(d * b);
            r.p += m * c.p;
            r.s *= glm::mix(glm::vec3(1.0f), c.s, m);
            r.q = glm::vec4(out.x, out.y, out.z, out.w);
        }
    }
    return r;
}

// Criterio de la fila 14: capa 1 override con máscara de brazo.
static void test_layers_override_mask_criterion()
{
    const SkinnedMesh m = makeFourConstantClips();           // de la fila 13: 4 clips constantes
    const PackedClips p = packSkinnedClips(m);
    const size_t B = m.skeleton.names.size();
    std::vector<uint8_t> mask(B, 0);
    mask[B - 1] = 1;                                          // el último hueso = "brazo"
    AnimationPose base; base.count = 1; base.samples[0] = { 0, 0.0f, 1.0f, 0 };
    AnimationPose capa1 = base; capa1.samples[0] = { 1, 0.0f, 1.0f, 0 };
    AnimationPose pose = base;
    pose.layerCount = 2; pose.count = 2;
    pose.samples[1] = { 1, 0.0f, 1.0f, 1 };
    pose.layers[1] = { 1.0f, 0u, 0.0f, false, &mask };
    for (size_t i = 0; i < B; i++)
    {
        const Trs r = evalLayeredTrs(p, B, i, pose);
        const Trs e = evalLayeredTrs(p, B, i, mask[i] ? capa1 : base);
        CHECK(glm::length(r.p - e.p) < 1e-5f && glm::length(r.q - e.q) < 1e-5f);
    }
    pose.layers[1].weight = 0.0f;
    for (size_t i = 0; i < B; i++)
    {
        const Trs r = evalLayeredTrs(p, B, i, pose), e = evalLayeredTrs(p, B, i, base);
        CHECK(glm::length(r.p - e.p) < 1e-6f && glm::length(r.q - e.q) < 1e-6f);
    }
}

// Additive en t = 0: la base exacta.
static void test_layers_additive_zero_at_start()
{
    const SkinnedMesh m = makeFourConstantClips();
    const PackedClips p = packSkinnedClips(m);
    const size_t B = m.skeleton.names.size();
    AnimationPose base; base.count = 1; base.samples[0] = { 0, 0.0f, 1.0f, 0 };
    AnimationPose pose = base;
    pose.layerCount = 2; pose.count = 2;
    pose.samples[1] = { 2, 0.0f, 1.0f, 1 };
    pose.layers[1] = { 1.0f, 1u, 0.0f, false, nullptr };
    for (size_t i = 0; i < B; i++)
    {
        const Trs r = evalLayeredTrs(p, B, i, pose), e = evalLayeredTrs(p, B, i, base);
        CHECK(glm::length(r.p - e.p) < 1e-5f && std::fabs(std::fabs(glm::dot(r.q, e.q)) - 1.0f) < 1e-5f);
    }
}
```

(Si `makeFourConstantClips` da clips constantes en el tiempo, el test de additive en t > 0 no ve nada; se añade `test_layers_additive_adds_delta` con un clip de dos claves construido en el propio test: posición (0,0,0) en t=0 y (0,1,0) en t=10 en el hueso 0, y se comprueba que a t=5 la capa additive suma (0,0.5,0) a la posición de la base.)

- [ ] **Step 2:** build → falla (no existe `PoseBlock.h`).
- [ ] **Step 3: implementar.**
  - `PoseBlock.h` con `writePoseBlock` según el layout de las Global Constraints (`hasMask = mask != nullptr`, las máscaras de las capas sin máscara no se escriben).
  - Shaders: push de 16 bytes en los tres (`uint boneCount; uint vertexCount; uint rootMotionMode; uint poseBlockOffset;`). `bone_eval.comp`: binding 10 `PoseBlock`, `poseTrs`/`frozenTrs` indexados `(L * boneCount + bi) * 3 + c`; función `evalCapa(L, additive, out pos, out rot, out scl, out bool alguna)` que replica la `capa` de la referencia (muestras con `layer == L`, delta additive con `sampleBone(info, 0.0)`, congelada de la capa L si `frozenWeight > 0` y `fs.w != 0`), escribe `poseTrs` de la capa; `main` = la combinación de `evalLayeredTrs`, con el bindLocal de la capa 0 descompuesto (el shader ya tiene `trs()`; se añade `decomposeTrs(mat4)` con escala = longitud de las columnas y rotación por `quatFromMat3` de las columnas normalizadas), y `applyRootLock` al final.
  - `SkinningPass::Push` y `ComputePush`: los 4 campos, `static_assert(... == 16)`. `pushDe` rellena `poseBlockOffset = frameIndex * poseBlockUints(boneCount)`.
  - Vulkan (`RenderObjects.h`, `Renderer.cpp`): `SkinnedRenderObject` gana `VkBuffer poseBlockBuffer; VkDeviceMemory poseBlockMemory; void* poseBlockMapped;` (HOST_VISIBLE|HOST_COHERENT, `kFramesInFlight × poseBlockUints × 4` bytes, mapeado persistente); `poseTrs`/`frozenTrs` a `kMaxLayers × boneCount × 3 × vec4`; descriptores 10 → 11 (binding 10) y pool `11 * kSetsPerPool`; destroy. `setAnimationPose` guarda la pose y **copia las máscaras** a un `std::vector<uint8_t> masks[kMaxLayers]` del objeto (y apunta `pose.layers[L].mask` a su copia). La escritura del bloque (`writePoseBlock` en la copia del frame) va en `record`, antes de la fase 1, para cada personaje activo; sin pose (`hasPose == false`) se escribe la pose de una muestra (`activeClip`, `animTime`). Congelación: por cada capa con `freezeNow`, `vkCmdCopyBuffer` de la región `L` de `poseTrs` a la de `frozenTrs`, con las barreras de hoy.
  - D3D12: lo mismo con un buffer UPLOAD mapeado (`framesInFlight() × poseBlockUints × 4`), root SRV `{SRV, 10}` en `bone_eval` (parámetro raíz 8), `SetComputeRootShaderResourceView(8, gpu(poseBlock))`, y la copia por capa con `CopyBufferRegion` y las transiciones de hoy.
- [ ] **Step 4:** build; `build-ninja/hlsl/bone_eval.comp.hlsl` tiene `ByteAddressBuffer poseBlock : register(t10)` y el cbuffer con 4 escalares; suite en verde.
- [ ] **Step 5:** commit `feat(skinning): bone_eval combina capas override y additive con mascara`.
- [ ] **Step 6:** sabotajes en la referencia de CPU y el layout: S11 máscara ignorada en `evalLayeredTrs` → cae `override_mask_criterion`; S12 additive sin restar t=0 → cae `additive_zero_at_start`; S13 `writePoseBlock` con las muestras en 32 → cae `pose_block_layout`. Runtime Debug `rr10.scene` en los dos backends: Vulkan sin validación, D3D12 solo `id=1328`.

---

### Task 5: Editor (capas, propiedades, máscara)

**Files:** `engine/src/Editor/AnimatorPanel.cpp`, `engine/include/DonTopo/Editor/AnimatorPanel.h`

- [ ] **Step 1:** `int m_layer = 0;` en el panel (acotado cada frame a `[0, layerCount-1]`). Todo lo que hoy lee `anim->states()/statesMutable()/transitions()/transitionsMutable()/entryState()/setEntryState/addState/addTransition/removeState/removeTransition/anyStateEditorPos/currentState/previousState` en el panel pasa `m_layer`.
- [ ] **Step 2:** barra de capas (`PushID("capas")`) encima del lienzo del grafo: `Selectable` por capa (doble clic → `InputText` para renombrar), botones `+` (`addLayer("Layer N")`, desactivado en 8), `-` (no en la 0), `^`/`v` (`moveLayer`, sin tocar la 0). Con `m_layer > 0`: `SliderFloat("Weight##lw", ...)` → `setLayerWeight`, `Combo("Mode##lm", {"Override","Additive"})` → `setLayerMode`, botón `Mask (N)##lmask` que abre el popup `"layerMask"`.
- [ ] **Step 3:** popup de máscara: botón `Todo el cuerpo` (vacía `maskBones`) y árbol del esqueleto (`SkinnedMesh::skeleton.names/parentIndex`), cada hueso con `Checkbox` dentro de `TreeNodeEx` (`DefaultOpen`, hojas con `Leaf`), `PushID(hueso)`. Clic: si Ctrl está pulsado, alterna solo ese hueso; si no, pone la rama entera (el hueso y todos sus descendientes) al nuevo valor. Tras cambiar, `anim->rebindClips(*mesh, nullptr)` para resolver la máscara.
- [ ] **Step 4:** build; suite en verde.
- [ ] **Step 5:** commit `feat(editor): capas del Animator con peso, modo y mascara de huesos`.

---

### Task 6: Lua, documentación y verificación

**Files:** `engine/src/Scripting/ScriptBindings.cpp:1495-1545`, `LuaApiReference.cpp`, `README.md`, `docs/animation-audit.md`, tests de Lua del Animator (donde estén hoy los de `CrossFade`)

- [ ] **Step 1: tests Lua** (junto a los de `CrossFade`): un script que hace `anim:SetLayerWeight(1, 0.5)`, lee `anim:GetLayerWeight(1)` = 0.5, `anim:GetLayerCount()` = 2, `anim:Play("Aim", 1)` y `anim:GetState(1)` = "Aim" con `anim:GetState()` todavía "Idle"; `anim:GetState(9)` = "" y `anim:SetLayerWeight(9, 1)` no rompe.
- [ ] **Step 2:** bindings: los tres nuevos; `Play`, `CrossFade`, `GetState`, `IsBlending`, `GetNormalizedTime` con `sol::optional<int> layer` (fuera de rango: no hace nada / devuelve lo de sin estado). `LuaApiReference.cpp` con las firmas nuevas y el argumento opcional; README `## Lua Scripting` y la sección Animator (capas: grafo por capa, peso, override/additive, máscara por rama, root motion solo de la base, eventos de las capas con peso).
- [ ] **Step 3:** build; suite en verde; commit `feat(lua): capas del Animator desde Lua`.
- [ ] **Step 4:** `docs/animation-audit.md`: C4 EXISTE, fila 14 HECHA (rango de commits). Commit `docs(animation): capas del Animator (fila 14b)`.
- [ ] **Step 5:** verificación manual del usuario en Vulkan y D3D12 (la de la spec).
