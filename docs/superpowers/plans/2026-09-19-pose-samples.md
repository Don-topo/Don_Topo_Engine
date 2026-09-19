# Pose por muestras ponderadas — plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (inline). Steps use checkbox (`- [ ]`) syntax.

**Goal:** que `bone_eval` mezcle hasta 4 muestras ponderadas más una pose congelada, para que un fade desde un estado con blend (A3) y la interrupción de un fade (A4) no salten.

**Architecture:** el Animator produce un `AnimationPose` (CPU puro, testeable); una referencia en CPU del nuevo `bone_eval` demuestra la continuidad; después el shader y los dos backends consumen `setAnimationPose`.

**Spec:** `docs/superpowers/specs/2026-09-19-pose-samples-design.md` (`28c9e65`), con **una corrección**: `freezeNow` no se apaga en el siguiente `update` (un `CrossFade` de Lua lanzado antes de `applySkinnedFrame` se perdería), sino con `AnimatorComponent::clearFreezeRequest()`, que llama `applySkinnedFrame` justo después de mandar la pose. El backend además limpia su copia tras grabar la congelación.

## Global Constraints

- Rama `feat/pose-samples`. `.\build.bat` desde PowerShell; tests desde la raíz.
- CRLF: Edit, o Python con `newline=''` escrito con Write (el shell rompe `\n` en heredocs). `runtime/main.cpp` está en LF.
- **Commit antes de cada sabotaje.** Sabotajes de uno en uno (`sab_generic.ps1` del scratchpad).
- Paridad Vulkan/D3D12: `bone_eval.comp` es uno; el DXIL sale del mismo SPIR-V.

---

### Task 1: `AnimationPose` en CPU

**Files:** Create `engine/include/DonTopo/Core/AnimationPose.h`; Modify `AnimatorComponent.h/.cpp`, `engine/src/Scripting/ScriptBindings.cpp` (`IsBlending`); Test `engine/tests/animator_tests.cpp`.

**Produces:** `struct PoseSample { int clip = 0; float time = 0.0f; float weight = 0.0f; }`, `struct AnimationPose { PoseSample samples[4]; int count = 0; float frozenWeight = 0.0f; bool freezeNow = false; uint32_t rootMotionMode = 0; }` (en `DonTopo`), `AnimationPose AnimatorComponent::pose() const`, `void clearFreezeRequest()`, `bool fading() const`.

- [ ] **Step 1: tests** (delante de `int main`, llamadas tras las de root motion)

```cpp
// ── Pose por muestras ────────────────────────────────────────────────────────
//
// Dos estados con blend 1D: Loco (Walk clip 0 umbral 0, Run clip 2 umbral 1) y
// Otro (Idle clip 1 umbral 0, Jump clip 3 umbral 1), con speed = 0,5 (pareja a
// medias en los dos). go: Loco -> Otro, fade de 2 s. back: Otro -> Loco, 1 s.
static AnimatorComponent makeTwoBlendStates()
{
    AnimatorComponent a;
    auto estado = [](const char* n, const char* principal, int ci, const char* extra, int ce) {
        AnimatorComponent::State s;
        s.name = n; s.clipName = principal; s.clipIndex = ci; s.duration = 40.0f;
        s.ticksPerSecond = 20.0f; s.loop = true;
        s.blendParam = "speed"; s.clipThreshold = 0.0f;
        s.blendEntries = { entrada(extra, ce, 40.0f, 1.0f) };
        return s;
    };
    a.addState(estado("Loco", "Walk", 0, "Run", 2));
    a.addState(estado("Otro", "Idle", 1, "Jump", 3));
    a.setEntryState(0);
    a.addParameter("speed", AnimatorComponent::ParamType::Float);
    a.addParameter("go", AnimatorComponent::ParamType::Trigger);
    a.addParameter("back", AnimatorComponent::ParamType::Trigger);
    auto trans = [&](int from, int to, float dur, const char* trig) {
        AnimatorComponent::Transition t;
        t.fromState = from; t.toState = to; t.duration = dur;
        AnimatorComponent::Condition c;
        c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = trig;
        t.conditions.push_back(c);
        a.addTransition(t);
    };
    trans(0, 1, 2.0f, "go");
    trans(1, 0, 1.0f, "back");
    a.reset();
    a.setFloat("speed", 0.5f);
    return a;
}

static float pesoMuestra(const AnimationPose& p, int clip)
{
    float w = 0.0f;
    for (int i = 0; i < p.count; i++) if (p.samples[i].clip == clip) w += p.samples[i].weight;
    return w;
}

static float sumaPesos(const AnimationPose& p)
{
    float w = p.frozenWeight;
    for (int i = 0; i < p.count; i++) w += p.samples[i].weight;
    return w;
}

// Sin fade: la pareja del estado, con sus pesos.
static void test_pose_samples_without_fade()
{
    AnimatorComponent a = makeTwoBlendStates();
    a.update(0.5f, true);
    const AnimationPose p = a.pose();
    CHECK(p.count == 2);
    CHECK(nearlyEqual(pesoMuestra(p, 0), 0.5f));
    CHECK(nearlyEqual(pesoMuestra(p, 2), 0.5f));
    CHECK(nearlyEqual(p.frozenWeight, 0.0f));
    CHECK(!p.freezeNow);
    // Sin blend: una sola muestra de peso 1.
    a.setFloat("speed", 0.0f);
    a.update(0.0f, true);
    CHECK(a.pose().count == 1);
    CHECK(nearlyEqual(a.pose().samples[0].weight, 1.0f));
}

// Fade entre dos estados con blend: las 4 muestras, el que sale con SU reloj.
static void test_pose_samples_fade_between_blends()
{
    AnimatorComponent a = makeTwoBlendStates();
    a.update(0.5f, true);                          // Loco en 10 ticks
    a.setTrigger("go");
    a.update(0.016f, true);                        // sale a Otro
    a.update(0.5f, true);                          // w = 0,258 aprox.
    const float w = a.blendWeight();
    CHECK(w > 0.2f && w < 0.3f);
    const AnimationPose p = a.pose();
    CHECK(p.count == 4);
    CHECK(nearlyEqual(pesoMuestra(p, 0), (1.0f - w) * 0.5f));
    CHECK(nearlyEqual(pesoMuestra(p, 2), (1.0f - w) * 0.5f));
    CHECK(nearlyEqual(pesoMuestra(p, 1), w * 0.5f));
    CHECK(nearlyEqual(pesoMuestra(p, 3), w * 0.5f));
    for (int i = 0; i < p.count; i++)
        if (p.samples[i].clip == 0) CHECK(nearlyEqual(p.samples[i].time, a.previousAnimTime()));
}

// Interrumpir un fade congela la pose una vez y el estado previo deja de
// aportar muestras.
static void test_pose_freeze_on_interrupted_fade()
{
    AnimatorComponent a = makeTwoBlendStates();
    a.setTrigger("go");
    a.update(0.016f, true);
    a.update(0.5f, true);                          // fade Loco -> Otro en vuelo
    a.setTrigger("back");
    a.update(0.016f, true);                        // interrumpe: Otro -> Loco
    AnimationPose p = a.pose();
    CHECK(p.freezeNow);
    CHECK(a.fading());
    CHECK(!a.blending());
    CHECK(nearlyEqual(p.frozenWeight, 1.0f - a.blendWeight()));
    CHECK(nearlyEqual(pesoMuestra(p, 1) + pesoMuestra(p, 3), 0.0f));   // Otro ya no aporta
    a.clearFreezeRequest();
    CHECK(!a.pose().freezeNow);
    a.update(0.25f, true);
    p = a.pose();
    CHECK(!p.freezeNow);
    CHECK(nearlyEqual(p.frozenWeight, 1.0f - a.blendWeight()));
    CHECK(p.frozenWeight > 0.0f && p.frozenWeight < 1.0f);
    a.update(2.0f, true);                          // el fade acaba
    CHECK(!a.fading());
    CHECK(nearlyEqual(a.pose().frozenWeight, 0.0f));
}

// Los pesos suman 1 en todos los casos.
static void test_pose_weights_sum_to_one()
{
    AnimatorComponent a = makeTwoBlendStates();
    CHECK(nearlyEqual(sumaPesos(a.pose()), 1.0f));
    a.setTrigger("go");
    a.update(0.016f, true);
    for (int i = 0; i < 5; i++) { a.update(0.3f, true); CHECK(nearlyEqual(sumaPesos(a.pose()), 1.0f)); }
    a.setTrigger("back");
    a.update(0.016f, true);
    for (int i = 0; i < 5; i++) { a.update(0.3f, true); CHECK(nearlyEqual(sumaPesos(a.pose()), 1.0f)); }
}
```

(`makeTwoBlendStates` va después de `entrada`. Si `previousAnimTime()` no existe con ese nombre, usar el accesor público del reloj previo que tenga el header.)

- [ ] **Step 2: `AnimationPose.h`**

```cpp
#pragma once
#include <cstdint>

namespace DonTopo
{
    // Lo que el Animator pide a bone_eval cada frame: hasta 4 clips con su
    // tiempo (ticks) y su peso, más la pose congelada con el suyo. Los pesos
    // suman 1. Vive fuera de AnimatorComponent.h para que la interfaz del
    // backend no tenga que incluir el Animator.
    struct PoseSample { int clip = 0; float time = 0.0f; float weight = 0.0f; };
    struct AnimationPose
    {
        PoseSample samples[4] = {};
        int        count = 0;
        float      frozenWeight = 0.0f;   // 0 = no se usa la congelada
        // Este frame: copiar la pose actual a la congelada ANTES de evaluar.
        bool       freezeNow = false;
        uint32_t   rootMotionMode = 0;
    };
}
```

- [ ] **Step 3: Animator**
- Header: `#include "DonTopo/Core/AnimationPose.h"`; públicos `AnimationPose pose() const;`, `void clearFreezeRequest() { m_freezePending = false; }`, `bool fading() const { return m_prevState >= 0 || m_frozenFade; }` (comentario: fade en curso, vivo o desde una pose congelada; `blending()` sigue significando "hay un estado previo vivo"). Privados `bool m_frozenFade = false; bool m_freezePending = false;`. `stateBlendPair(int stateIdx)` → `stateBlendPair(int stateIdx, float animTime)`.
- `stateBlendPair`: `const int clip = (stateIdx >= 0 && stateIdx < (int)m_states.size() && m_states[stateIdx].clipIndex >= 0) ? m_states[stateIdx].clipIndex : 0;` en vez de `currentClipIndex()`, y todos sus `m_animTime` pasan a `animTime`. Las llamadas existentes pasan `m_animTime`.
- `blendWeight()`: la primera condición pasa a `if ((m_prevState < 0 && !m_frozenFade) || m_blendDuration <= 0.0f) return 1.0f;`.
- `update`: el bloque `if (m_prevState >= 0)` del fade pasa a `if (m_prevState >= 0 || m_frozenFade)` (el `advanceClock` del previo ya va guardado por `m_prevState < size`; añadir `m_prevState >= 0 &&` a esa guarda), y al terminar el fade, junto a `m_prevState = -1;`: `m_frozenFade = false;`.
- `startTransitionTo`, rama `duration > 0`:

```cpp
            if (fading())
            {
                // Interrupción: la mezcla en vuelo se CONGELA en vez de
                // descartarse (A4). El backend copia la pose de pantalla a la
                // congelada antes de evaluar (freezeNow) y el fade sale de ella.
                m_frozenFade    = true;
                m_freezePending = true;
                m_prevState     = -1;
                m_prevStateTicks = 0.0;
                m_prevAnimTime  = 0.0f;
            }
            else
            {
                m_prevState     = m_currentState;
                m_prevStateTicks = m_stateTicks;
                m_prevAnimTime  = m_animTime;
            }
            m_blendElapsed  = 0.0f;
            m_blendDuration = duration;
```

  Rama de corte seco y los demás `m_prevState = -1;` que cortan la mezcla (`removeState`, `applyGraph`, `resetPlayback`): añadir `m_frozenFade = false; m_freezePending = false;`.
- `pose()`:

```cpp
    AnimationPose AnimatorComponent::pose() const
    {
        AnimationPose out;
        out.rootMotionMode = poseRootMotionMode();
        out.freezeNow      = m_freezePending;
        auto add = [&](int clip, float time, float w) {
            if (w <= 0.0f || out.count >= 4) return;
            out.samples[out.count++] = { clip < 0 ? 0 : clip, time, w };
        };
        auto addPair = [&](int stateIdx, float time, float scale) {
            const BlendPair bp = stateBlendPair(stateIdx, time);
            if (bp.clipA == bp.clipB) { add(bp.clipB, bp.timeB, scale); return; }
            add(bp.clipA, bp.timeA, scale * (1.0f - bp.weight));
            add(bp.clipB, bp.timeB, scale * bp.weight);
        };
        if (m_currentState < 0 || m_currentState >= (int)m_states.size())
        {
            add(0, m_animTime, 1.0f);
            return out;
        }
        const float w = blendWeight();
        if (m_frozenFade)
        {
            out.frozenWeight = 1.0f - w;
            addPair(m_currentState, m_animTime, w);
        }
        else if (blending() && m_prevState < (int)m_states.size())
        {
            addPair(m_prevState, m_prevAnimTime, 1.0f - w);
            addPair(m_currentState, m_animTime, w);
        }
        else
            addPair(m_currentState, m_animTime, 1.0f);
        return out;
    }
```

  Comentario encima de `poseClipA`…`poseRootMotionMode` en el header: "La pareja PRINCIPAL (la vista de dos clips de siempre, para Lua y los tests). Lo que va a la GPU es pose()."
- `ScriptBindings.cpp` `IsBlending`: `animOf(c)->fading()`.

- [ ] **Step 4:** build, suite (29/29), CRLF, commit `feat(animator): pose por muestras ponderadas con congelacion al interrumpir`.
- [ ] **Step 5: sabotajes**

| # | Cambio | Debe caer |
|---|---|---|
| P1 | `addPair(m_prevState, m_prevAnimTime, 1.0f - w)` → `add(previousClipIndex(), m_prevAnimTime, 1.0f - w)` | `fade_between_blends` |
| P2 | en `startTransitionTo`, `if (fading())` → `if (false)` | `freeze_on_interrupted_fade` |
| P3 | en `blendWeight`, quitar `&& !m_frozenFade` | `freeze_on_interrupted_fade` o `weights_sum_to_one` |
| P4 | `out.frozenWeight = 1.0f - w;` → `= 1.0f;` | `weights_sum_to_one` |
| P5 | en `stateBlendPair`, `phase` con `m_animTime` en vez de `animTime` | `fade_between_blends` (tiempo del que sale) |

---

### Task 2: referencia en CPU y continuidad (criterio de la fila 13)

**Files:** Test `engine/tests/animator_tests.cpp`.

**Produces (test):** `struct Trs { glm::vec3 p{0}; glm::vec4 q{0,0,0,1}; glm::vec3 s{1}; bool bind = false; }`, `static bool sampleTrs(const PackedClips&, size_t clipBase, size_t i, float T, Trs&)` (false si el hueso no tiene canal en ese clip), `static Trs evalPoseTrs(const PackedClips&, size_t boneCount, size_t i, const AnimationPose&, const std::vector<Trs>* frozen)`, `static glm::mat4 trsToMat(const Trs&, const GpuBoneInfo&)` (con `bind` devuelve `bindLocal`).

- [ ] **Step 1: la referencia** (junto a `evalLocalXform`), la misma fórmula que tendrá el shader:
  1. Para cada muestra: `sampleTrs` del bloque `clip * boneCount`; si ninguna tiene canal y `frozenWeight == 0` (o el hueso congelado es `bind`): devolver `{ bind = true }`.
  2. Muestra sin canal en ese hueso: TRS identidad.
  3. `p += w·p_k`, `s += w·s_k`, `q += w·(dot(q_k, q_ref) < 0 ? -q_k : q_k)` con `q_ref` la primera muestra; la congelada entra igual con `frozenWeight` (si su hueso es `bind`, se ignora y se renormaliza dividiendo por `1 − frozenWeight`).
  4. `q = normalize(q)` (si la longitud es ~0, identidad). El bloqueo de raíz (`rootMotionMode` 1 o 2 sobre el hueso sin padre) se aplica después, como en `applyRootLock`.

  `evalLocalXformBlended` pasa a llamar a `evalPoseTrs` con dos muestras (`A` con `1−w`, `B` con `w`) y `trsToMat`: los tests que la usan comprueban desde ahora la fórmula nueva. Si alguno fija números de rotación calculados con `slerp`, se recalcula con la nueva fórmula y el commit lo explica.

- [ ] **Step 2: tests de continuidad** (fixture: `SkinnedMesh` de un hueso raíz y 4 clips de posición constante: clip 0 (0,0,0), 1 (0,20,0), 2 (10,0,0), 3 (0,0,30); dos claves iguales en t=0 y t=40; `packSkinnedClips`)

```cpp
static SkinnedMesh makeFourConstantClips()
{
    SkinnedMesh m;
    m.skeleton.names = { "Hips" };
    m.skeleton.parentIndex = { -1 };
    m.skeleton.inverseBindPose = { glm::mat4(1.0f) };
    m.skeleton.boneMap["Hips"] = 0;
    const glm::vec3 pos[4] = { {0,0,0}, {0,20,0}, {10,0,0}, {0,0,30} };
    const char* nombres[4] = { "Walk", "Idle", "Run", "Jump" };
    for (int c = 0; c < 4; c++)
    {
        AnimationClip clip; clip.name = nombres[c]; clip.duration = 40.0f; clip.ticksPerSecond = 20.0f;
        BoneChannel ch; ch.boneIndex = 0;
        ch.posKeys = { { 0.0f, pos[c] }, { 40.0f, pos[c] } };
        ch.rotKeys = { { 0.0f, glm::quat(1,0,0,0) }, { 40.0f, glm::quat(1,0,0,0) } };
        ch.scaleKeys = { { 0.0f, glm::vec3(1.0f) }, { 40.0f, glm::vec3(1.0f) } };
        clip.channels.push_back(ch);
        m.animationClips.push_back(clip);
    }
    return m;
}

// A3: un fade desde un estado con blend no salta en su primer frame.
static void test_pose_continuity_fade_from_blend()
{
    const SkinnedMesh mesh = makeFourConstantClips();
    const PackedClips p = packSkinnedClips(mesh);
    AnimatorComponent a = makeTwoBlendStates();
    a.update(0.5f, true);
    const glm::vec3 antes = evalPoseTrs(p, 1, 0, a.pose(), nullptr).p;   // (5,0,0)
    a.setTrigger("go");
    a.update(0.016f, true);                                              // primer frame del fade
    const glm::vec3 despues = evalPoseTrs(p, 1, 0, a.pose(), nullptr).p;
    CHECK(nearlyEqual(antes.x, 5.0f));
    CHECK(glm::length(despues - antes) < 0.5f);
}

// A4: interrumpir un fade no salta; la congelada es la salida del frame anterior.
static void test_pose_continuity_interrupted_fade()
{
    const SkinnedMesh mesh = makeFourConstantClips();
    const PackedClips p = packSkinnedClips(mesh);
    AnimatorComponent a = makeTwoBlendStates();
    a.setTrigger("go");
    a.update(0.016f, true);
    a.update(0.5f, true);
    std::vector<Trs> salida = { evalPoseTrs(p, 1, 0, a.pose(), nullptr) };
    const glm::vec3 antes = salida[0].p;
    a.setTrigger("back");
    a.update(0.016f, true);
    const AnimationPose tras = a.pose();
    CHECK(tras.freezeNow);
    const glm::vec3 despues = evalPoseTrs(p, 1, 0, tras, &salida).p;     // la GPU congela 'salida'
    CHECK(glm::length(despues - antes) < 0.5f);
}
```

- [ ] **Step 3:** build, suite, commit `test(animator): referencia en CPU de la pose por muestras y continuidad (A3, A4)`.
- [ ] **Step 4: sabotajes**

| # | Cambio | Debe caer |
|---|---|---|
| C1 | P1 de la tarea 1 (el previo solo aporta su principal) | `continuity_fade_from_blend` |
| C2 | en `startTransitionTo`, interrupción descarta en vez de congelar (`if (fading())` → `if (false)`) | `continuity_interrupted_fade` |

---

### Task 3: shader y backends

Un solo commit: la firma del backend cambia en todos a la vez.

**Files:** `shaders/bone_eval.comp`, `shaders/bone_hierarchy.comp`, `shaders/skinning.comp`; `EditorRenderer.h`, `Renderer.h/.cpp`, `RenderObjects.h`, `Passes/SkinningPass.h/.cpp`, `D3D12/D3D12Renderer.h/.cpp`; `SkinnedFrameSync.h`; test double y `test_apply_skinned_frame_passes_pose_b_then_a` en `animator_tests.cpp`.

- [ ] **Step 1: push constant común** (C++ y los tres `.comp`, mismo orden; 68 bytes). Escalares, **no arrays**: en HLSL un array de un cbuffer va a 16 bytes por elemento y spirv-cross no lo iguala.

```cpp
    struct Push   // y ComputePush en D3D12, idéntico
    {
        uint32_t boneCount, vertexCount, sampleCount, rootMotionMode;
        float    frozenWeight;
        uint32_t clipBase0, clipBase1, clipBase2, clipBase3;   // clip * boneCount
        float    time0, time1, time2, time3;
        float    weight0, weight1, weight2, weight3;
    };
    static_assert(sizeof(Push) == 68, "los 3 .comp declaran este bloque");
```

  En `bone_hierarchy.comp` y `skinning.comp` el bloque se declara igual (se leen `boneCount` y `vertexCount`; el resto son relleno con su nombre).

- [ ] **Step 2: `bone_eval.comp`**: bindings nuevos `layout(set=0, binding=8, std430) buffer PoseTrs { vec4 data[]; } poseTrs;` (3 vec4 por hueso: pos, rot, scale; `scale.w = 0` marca "bind local") y `layout(set=0, binding=9, std430) readonly buffer FrozenTrs { vec4 data[]; } frozenTrs;`. `main`: para `k < sampleCount`, `sampleBone` del bloque `clipBaseK + bi` si ese `BoneInfo` tiene canal; acumular como la referencia de la tarea 2; con `frozenWeight > 0` y `frozenTrs[bi*3+2].w != 0`, sumar la congelada (si no, renormalizar). Sin ninguna muestra con canal (ni congelada usable): `localXforms = bindLocal` y `poseTrs = (0, (0,0,0,1), (1,1,1,0))`. Si no: normalizar la rotación, `applyRootLock` sobre la posición (el `BoneInfo` del bloque de la muestra de más peso), escribir `localXforms` y `poseTrs` (con `scale.w = 1`).

- [ ] **Step 3: Vulkan**
  - `RenderObjects.h`: `SkinnedRenderObject` sustituye `prevClip`, `prevAnimTime`, `blendWeight` y `rootMotionMode` por `AnimationPose pose;` y gana `poseTrsBuffer/Memory` y `frozenTrsBuffer/Memory`. `activeClip`/`animTime` se quedan (camino sin Animator y límites): `setAnimationPose` los iguala a la muestra de más peso.
  - Creación (`initSkinnedRenderObject`, junto a `localTransformBuffer`): dos buffers de `boneCount * 3 * sizeof(glm::vec4)`, `STORAGE | TRANSFER_SRC | TRANSFER_DST`, device local; `frozenTrs` lleno a ceros con `vkCmdFillBuffer` en el batch no es necesario: sin `frozenWeight` no se lee. Destrucción junto a `localTransformBuffer`.
  - Descriptores: `SkinningPass` layout y writes de 8 a 10; pool `descriptorCount = 10 * kSetsPerPool`.
  - `setAnimationState` y `updateAnimation`: `obj.pose` = una muestra `{activeClip, animTime, 1}`, `rootMotionMode = 0`.
  - `setAnimationPose(int index, const AnimationPose& pose)`: copia acotando cada `clip` con `clampClipIndex(clip, clipCount)`, y fija `activeClip`/`animTime` a la de más peso.
  - `SkinningPass::record`: el push sale de `obj.pose` (`clipBaseK = clipK * boneCount`, muestras vacías con peso 0). Antes de la fase 1, si algún activo tiene `pose.freezeNow`: una barrera global `SHADER_WRITE(compute) → TRANSFER_READ|TRANSFER_WRITE(transfer)`, `vkCmdCopyBuffer(poseTrs → frozenTrs)` por cada uno, otra barrera `TRANSFER_WRITE → SHADER_READ(compute)`, y `obj.pose.freezeNow = false`.
- [ ] **Step 4: D3D12**
  - `SkinnedObject`: lo mismo (`AnimationPose pose;` y `poseTrs`/`frozenTrs` con `createStorageBuffer(..., UNORDERED_ACCESS)`), liberación junto a `localXforms`.
  - Root signature de `bone_eval`: añadir `{UAV, 8}` y `{SRV, 9}` (spirv-cross da `u8` al escribible y `t9` al de solo lectura).
  - `recordSkinning`: push desde `pose`; en la fase 1 `SetComputeRootUnorderedAccessView(6, poseTrs)` y `SetComputeRootShaderResourceView(7, frozenTrs)` (índices tras los cinco de hoy). Congelación antes de la fase 1: transiciones `poseTrs UAV→COPY_SOURCE`, `frozenTrs UAV→COPY_DEST`, `CopyBufferRegion`, y de vuelta a `UNORDERED_ACCESS` los dos; luego `pose.freezeNow = false`.
  - `setAnimationState`/`updateAnimation`/`setAnimationPose` como en Vulkan (con `clampClipIndex`).
- [ ] **Step 5: interfaz y host**
  - `EditorRenderer.h`: `setAnimationBlend(...)` → `virtual void setAnimationPose(int index, const AnimationPose& pose) = 0;` (`#include "DonTopo/Core/AnimationPose.h"`).
  - `SkinnedFrameSync.h`: `renderer.setAnimationPose(go.skinnedRenderIndex, anim->pose()); anim->clearFreezeRequest();` en lugar de `setAnimationBlend(...)`.
  - Test double: `void setAnimationPose(int, const AnimationPose& p) { orden.push_back("blend"); pose = p; }`; `test_apply_skinned_frame_passes_pose_b_then_a` pasa a comprobar que `r.pose` coincide con `a->pose()` (count, clips, pesos, `rootMotionMode`) y que tras `applySkinnedFrame` con un fade interrumpido el doble recibió `freezeNow == true` y el Animator ya no lo tiene pendiente.
- [ ] **Step 6:** `configure.bat` si hace falta, build (los `.spv` y `.dxil` se regeneran; comprobar en `build-ninja/hlsl/bone_eval.comp.hlsl` que el cbuffer tiene los 17 campos escalares en orden), suite, CRLF, commit `feat(skinning): bone_eval mezcla hasta 4 muestras y la pose congelada`.
- [ ] **Step 7:** runtime Debug con `rr10.scene` en los dos backends: Vulkan con la validación (el `vk_layer_settings.txt` sigue en `build-ninja/sandbox`) sin avisos nuevos; D3D12 sin mensajes nuevos de la capa distintos de 1328. Sabotaje del host: quitar `anim->clearFreezeRequest();` hace caer el test del doble.

---

### Task 4: audit y verificación manual

- [ ] `docs/animation-audit.md`: A3 y A4 CERRADOS; fila 13 HECHA con los tests de continuidad; C3/C4 (capas, 2D) siguen, ahora sobre esta base. README, sección del Animator: los fades desde estados con blend y las interrupciones son continuos.
- [ ] Verificación manual (usuario), en los dos backends: un fade desde un estado con blend y la interrupción de un fade no saltan; el resto del Animator (blend, eventos, root motion) se ve igual.
