# Animator (grafo de estados de animación) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Un modelo carga N animaciones de su FBX y una máquina de estados editable
visualmente (nodos + links) decide qué clip se reproduce, con condiciones `bool`, `trigger` y
`animation finished`.

**Architecture:** Los keyframes de TODOS los clips se concatenan en los 3 SSBOs que ya
existen; `BoneInfos` pasa a layout `[clip][hueso]` y un `clipBase` en el push constant indexa
el clip activo. El grafo es lógica de CPU en un `AnimatorComponent` (Core, sin Vulkan) que
avanza el tiempo y decide; lo único que cruza a GPU es `(clip activo, animTime)`.

**Tech Stack:** C++20, Vulkan (compute shaders), Assimp, Dear ImGui + imgui-node-editor
(thedmd), nlohmann/json, sol2/Lua, CMake + Ninja + MSVC.

**Spec:** `docs/superpowers/specs/2026-07-16-animator-graph-design.md`

## Global Constraints

- **Build:** SOLO `configure.bat` y `build.bat` desde PowerShell (hacen vcvarsall + Ninja).
  Nunca invocar `cmake` crudo desde Bash.
- **Build stale de Ninja:** este plan toca `SkinnedMesh.h` y `Renderer.h`, headers muy
  incluidos. Si tras editarlos aparece un crash con puntero basura, es un `.obj` stale:
  borrar los `.obj` afectados y reconstruir ANTES de tocar el código nuevo.
- **ABI de `ComputePush`:** queda en 16 bytes. `pad` se renombra a `clipBase` (offset 12).
  Prohibido añadir campos o cambiar el tamaño.
- **Bindings de los 3 compute shaders:** NO se tocan.
- **Los shaders SÍ los compila el build.** `sandbox/CMakeLists.txt:11-56` define un target
  `Shaders ALL` que corre `glslc` sobre `shaders/*.{vert,frag,comp}` y copia los `.spv` al
  lado del ejecutable y de vuelta a `shaders/` (que está en `.gitignore`). `build.bat` basta;
  no hace falta invocar `glslc` a mano. Ojo: el `file(GLOB ...)` corre en configure, así que
  un shader NUEVO exige `configure.bat` — este plan no añade ninguno.
- **`shaders/bone_hierarchy.comp` y `shaders/skinning.comp`: NO se tocan.** Siguen declarando
  el 4º campo del push como `uint pad`; no lo leen, así que el nombre distinto respecto a
  `bone_eval.comp` es inocuo y mantiene el diff mínimo.
- **Dependencias:** solo se añade imgui-node-editor. Ninguna otra.
- **Ficheros prohibidos:** `build-ninja/`, `assets/`, `Scripts/`, `.claude/`.
- **Sin blending.** Transición = corte instantáneo. No añadir blending "porque es fácil".
- **Solo 3 condiciones:** `bool`, `trigger`, `animation finished`. Nada de int/float/exit time.
- **Idioma:** comentarios de código y mensajes de log en español, como el resto del repo.
  Mensajes de commit en inglés.
- **Tests:** plain `main()` + macro `CHECK`, sin framework (patrón de `camera_tests.cpp`).
- **`AnimationClip` NO lleva flag `loop`.** El loop vive en `AnimatorComponent::State`, porque
  el `SkinnedMesh` se reconstruye desde el FBX en cada carga y no se serializa.

## Cómo correr los tests

```powershell
cd c:\Users\ruben\Documents\Don_Topo_Engine
.\build.bat
.\build-ninja\engine\tests\dt_animator_tests.exe
```

Salida esperada al pasar: `dt_animator_tests: OK` y exit code 0. Cada `CHECK` fallido imprime
`FAIL: <expr> (line N)`.

## File Structure

| Fichero | Responsabilidad |
| --- | --- |
| `engine/include/DonTopo/Renderer/SkinnedMesh.h` | Datos de malla/esqueleto/clips. Gana `animationClips` (vector). |
| `engine/include/DonTopo/Renderer/SkinnedMeshPacking.h` | **Nuevo.** Contrato de `packSkinnedClips`. |
| `engine/src/Renderer/SkinnedMeshPacking.cpp` | **Nuevo.** Flatten puro de N clips a formato GPU. Sin Vulkan. |
| `engine/src/Renderer/ModelLoader.cpp` | Carga N animaciones de Assimp. |
| `engine/include/DonTopo/Renderer/Renderer.h` | `clipBase`, `activeClip`, `clipCount`, `setAnimationState`. |
| `engine/src/Renderer/Renderer.cpp` | Sube lo empaquetado; carga `clipBase` en el push. |
| `shaders/bone_eval.comp` | Indexa `BoneInfos` con `clipBase`. |
| `engine/include/DonTopo/Core/AnimatorComponent.h` | **Nuevo.** Grafo + runtime. Sin Vulkan, sin `GameObject`. |
| `engine/src/Core/AnimatorComponent.cpp` | **Nuevo.** `update`, condiciones, triggers, `bindClips`. |
| `engine/include/DonTopo/Core/GameObject.h` | Slot `m_animator`. |
| `engine/src/Core/Scene.cpp` | Bloque JSON `"animator"` + `bindClips` al cargar. |
| `engine/include/DonTopo/Editor/Command.h` + `.cpp` | `AnimatorComponentCommand` (Add/Remove con undo). |
| `engine/include/DonTopo/Editor/AnimatorPanel.h` + `.cpp` | **Nuevo.** Canvas de nodos. |
| `engine/src/Editor/PropertiesPanel.cpp` | Bloque Animator tras el Add-gate. |
| `engine/src/Editor/EditorUI.cpp` + `.h` | Registro del panel + toggle en View. |
| `engine/src/Scripting/ScriptBindings.cpp` | `GetComponent("Animator")` + métodos. |
| `engine/src/Scripting/LuaApiReference.cpp` | Autocompletado del Script Editor. |
| `sandbox/src/main.cpp` | Rama Animator vs `updateAnimation`. |
| `engine/tests/animator_tests.cpp` | **Nuevo.** Los 9 tests. |

## Orden y dependencias

```
Task 1 (N clips) ──► Task 2 (packing) ──► Task 3 (clipBase GPU) ─┐
                          │                                       │
Task 4 (Animator core) ───┼──► Task 5 (bindClips + slot) ─────────┼──► Task 7 (main.cpp)
                          │            │                          │
                          │            └──► Task 6 (JSON) ──► Task 8 (Command) ─┐
                          │                                                     │
Task 9 (CMake node editor) ──► Task 10 (AnimatorPanel) ──────────────────────► Task 11 (Properties)
                                                                                │
Task 5 ──► Task 12 (Lua) ─────────────────────────────────────────────────────► Task 13 (README)
                                                                                     │
                                                                              Task 14 (GUI manual)
```

Tasks 1, 4 y 9 no dependen de nada: pueden ir en paralelo.

---

### Task 1: N clips desde el FBX

**Files:**
- Modify: `engine/include/DonTopo/Renderer/SkinnedMesh.h:58`
- Modify: `engine/src/Renderer/ModelLoader.cpp:274-313`
- Modify: `engine/src/Renderer/Renderer.cpp:1834`
- Create: `engine/tests/animator_tests.cpp`
- Modify: `engine/tests/CMakeLists.txt`

**Interfaces:**
- Produces: `SkinnedMesh::animationClips` (`std::vector<AnimationClip>`) — Tasks 2 y 5 lo consumen.
- Produces: el ejecutable de test `dt_animator_tests` — Tasks 2, 4, 5, 6 le añaden tests.

- [ ] **Step 1: Cambiar el campo de `SkinnedMesh`**

En `engine/include/DonTopo/Renderer/SkinnedMesh.h`, sustituye la línea
`AnimationClip                animationClip;` (línea 58) por:

```cpp
        // Todas las animaciones del fichero de origen, en el orden de
        // scene->mAnimations. El Animator las referencia por nombre (no por
        // índice): reexportar el modelo puede reordenarlas.
        std::vector<AnimationClip>   animationClips;
```

- [ ] **Step 2: Cargar todas las animaciones en `ModelLoader`**

En `engine/src/Renderer/ModelLoader.cpp`, sustituye el bloque completo
`// --- Animación (clip 0) ---` (líneas 274-313, desde el comentario hasta la llave que
cierra el `if (scene->mNumAnimations > 0)`) por:

```cpp
        // --- Animaciones: todas las del fichero ---
        for (uint32_t a = 0; a < scene->mNumAnimations; a++)
        {
            aiAnimation* anim = scene->mAnimations[a];
            AnimationClip clip;

            // Nombres únicos y no vacíos: Mixamo exporta cada take como
            // "mixamo.com", y los FBX de Blender a veces sin nombre. El
            // Animator resuelve los clips por nombre, así que dos clips
            // homónimos harían que el segundo fuera inalcanzable.
            std::string base = anim->mName.C_Str();
            if (base.empty()) base = "Animation " + std::to_string(a);
            std::string unique = base;
            int suffix = 1;
            auto taken = [&](const std::string& n) {
                for (const auto& c : smesh.animationClips)
                    if (c.name == n) return true;
                return false;
            };
            while (taken(unique)) unique = base + " (" + std::to_string(suffix++) + ")";

            clip.name            = unique;
            clip.duration        = (float)anim->mDuration;
            clip.ticksPerSecond  = (anim->mTicksPerSecond > 0.0) ? (float)anim->mTicksPerSecond : 24.0f;

            for (uint32_t c = 0; c < anim->mNumChannels; c++)
            {
                aiNodeAnim* ch = anim->mChannels[c];
                std::string boneName = ch->mNodeName.C_Str();
                if (!skel.boneMap.count(boneName)) continue;

                BoneChannel bc;
                bc.boneIndex = skel.boneMap[boneName];

                for (uint32_t k = 0; k < ch->mNumPositionKeys; k++)
                {
                    auto& key = ch->mPositionKeys[k];
                    bc.posKeys.push_back({ (float)key.mTime,
                        { key.mValue.x, key.mValue.y, key.mValue.z } });
                }
                for (uint32_t k = 0; k < ch->mNumRotationKeys; k++)
                {
                    auto& key = ch->mRotationKeys[k];
                    // glm::quat constructor: (w, x, y, z)
                    bc.rotKeys.push_back({ (float)key.mTime,
                        glm::quat(key.mValue.w, key.mValue.x, key.mValue.y, key.mValue.z) });
                }
                for (uint32_t k = 0; k < ch->mNumScalingKeys; k++)
                {
                    auto& key = ch->mScalingKeys[k];
                    bc.scaleKeys.push_back({ (float)key.mTime,
                        { key.mValue.x, key.mValue.y, key.mValue.z } });
                }
                clip.channels.push_back(std::move(bc));
            }
            smesh.animationClips.push_back(std::move(clip));
        }
```

- [ ] **Step 3: Arreglar el único consumidor roto**

En `engine/src/Renderer/Renderer.cpp`, línea 1834, sustituye:

```cpp
        const AnimationClip& clip = mesh.animationClip;
```

por:

```cpp
        // Clip 0 pa duration/ticksPerSecond del objeto: son lo que consume
        // updateAnimation(), que solo corre en el caso SIN Animator (Task 3
        // añade el camino con Animator). Malla sin animaciones -> clip vacío.
        static const AnimationClip kEmptyClip{};
        const AnimationClip& clip = mesh.animationClips.empty() ? kEmptyClip : mesh.animationClips[0];
```

Y en el bucle de flatten de ese mismo método (líneas ~1858-1860), `clip.channels` sigue
funcionando sin cambios. No toques nada más de `addSkinnedMesh` en esta task.

- [ ] **Step 4: Escribir el test que falla**

Crea `engine/tests/animator_tests.cpp`:

```cpp
// Tests headless del Animator: carga de N clips, empaquetado GPU, máquina de
// estados y serialización. Plain main + asserts, sin framework — coherente con
// camera_tests.cpp y physics_tests.cpp.
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/SkinnedMesh.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool nearlyEqual(float a, float b, float eps = 0.001f) { return std::fabs(a - b) < eps; }

// Criterio 1: todo clip cargado del FBX tiene nombre no vacío y único, duration
// y ticksPerSecond válidos. Ejercita el bucle sobre mAnimations aunque el asset
// del repo traiga una sola animación.
static void test_loader_reads_all_clips()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    CHECK(!m.skeleton.names.empty());
    CHECK(!m.animationClips.empty());

    for (size_t i = 0; i < m.animationClips.size(); i++)
    {
        const AnimationClip& c = m.animationClips[i];
        CHECK(!c.name.empty());
        CHECK(c.duration > 0.0f);
        CHECK(c.ticksPerSecond > 0.0f);
        CHECK(!c.channels.empty());
        // Nombres únicos entre sí: el Animator resuelve por nombre.
        for (size_t j = i + 1; j < m.animationClips.size(); j++)
            CHECK(m.animationClips[i].name != m.animationClips[j].name);
    }
}

int main()
{
    test_loader_reads_all_clips();

    if (g_failures) { std::printf("dt_animator_tests: %d FAILURES\n", g_failures); return 1; }
    std::printf("dt_animator_tests: OK\n");
    return 0;
}
```

- [ ] **Step 5: Registrar el test en CMake**

En `engine/tests/CMakeLists.txt`, tras el bloque de `dt_camera_tests` (línea 12), añade:

```cmake
add_executable(dt_animator_tests animator_tests.cpp)
target_link_libraries(dt_animator_tests PRIVATE DonTopoEngine)
target_compile_features(dt_animator_tests PRIVATE cxx_std_20)
```

Y en el `foreach` de fmod.dll (línea 21), añade el target nuevo a la lista:

```cmake
        foreach(_dt_test_target dt_physics_tests dt_content_browser_tests dt_camera_tests dt_animator_tests)
```

- [ ] **Step 6: Compilar y correr el test**

```powershell
.\configure.bat
.\build.bat
.\build-ninja\engine\tests\dt_animator_tests.exe
```

`configure.bat` hace falta porque el fichero de test es nuevo. El test carga
`assets/modelAnimation.fbx` con ruta relativa, así que se ejecuta desde la raíz del repo.
Esperado: `dt_animator_tests: OK`, exit code 0.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Renderer/SkinnedMesh.h engine/src/Renderer/ModelLoader.cpp engine/src/Renderer/Renderer.cpp engine/tests/animator_tests.cpp engine/tests/CMakeLists.txt
git commit -m "feat(anim): load every animation in the FBX, not just the first

ModelLoader read scene->mAnimations[0] and dropped the rest, so a
multi-take export lost everything past the first clip. SkinnedMesh holds
a vector now.

Clip names are made unique and non-empty: Mixamo exports every take as
\"mixamo.com\", and the Animator resolves clips by name, so two homonyms
would leave the second unreachable.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: `packSkinnedClips` — empaquetado `[clip][hueso]`

**Files:**
- Create: `engine/include/DonTopo/Renderer/SkinnedMeshPacking.h`
- Create: `engine/src/Renderer/SkinnedMeshPacking.cpp`
- Modify: `engine/src/Renderer/Renderer.cpp:1845-1907`
- Modify: `engine/CMakeLists.txt:14`
- Modify: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `SkinnedMesh::animationClips` (Task 1).
- Produces: `DonTopo::PackedClips { std::vector<GpuPosKey> pos, scale; std::vector<GpuRotKey> rot; std::vector<GpuBoneInfo> boneInfos; }`
  y `DonTopo::PackedClips packSkinnedClips(const SkinnedMesh& mesh)` — Task 3 lo consume.
- Contrato clave: `boneInfos[c * boneCount + b]` es la entrada del hueso `b` en el clip `c`.
  `c * boneCount` es exactamente el `clipBase` del push constant.

- [ ] **Step 1: Escribir el test que falla**

En `engine/tests/animator_tests.cpp`, añade el include arriba:

```cpp
#include "DonTopo/Renderer/SkinnedMeshPacking.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
```

y esta función antes de `main()`:

```cpp
// Los keyframes de los N clips van concatenados en los mismos vectores y
// boneInfos queda en layout [clip][hueso]. parentIndex/inverseBindPose son del
// esqueleto, no del clip: idénticos en todos los bloques — eso es lo que deja a
// bone_hierarchy.comp sin cambios.
static void test_pack_concatenates_clips()
{
    SkinnedMesh m;
    m.skeleton.names           = { "root", "child" };
    m.skeleton.parentIndex     = { -1, 0 };
    m.skeleton.inverseBindPose = { glm::mat4(2.0f), glm::mat4(3.0f) };

    // 3 clips; solo el hueso 0 tiene canal, con 1, 2 y 3 keys respectivamente.
    // El valor de posición lleva el índice del clip como marcador.
    for (int c = 0; c < 3; c++)
    {
        AnimationClip clip;
        clip.name           = "clip" + std::to_string(c);
        clip.duration       = 10.0f * (float)(c + 1);
        clip.ticksPerSecond = 24.0f;

        BoneChannel ch;
        ch.boneIndex = 0;
        for (int k = 0; k <= c; k++)
        {
            ch.posKeys.push_back({ (float)k, glm::vec3((float)c) });
            ch.rotKeys.push_back({ (float)k, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
            ch.scaleKeys.push_back({ (float)k, glm::vec3(1.0f) });
        }
        clip.channels.push_back(ch);
        m.animationClips.push_back(clip);
    }

    PackedClips p = packSkinnedClips(m);

    // C*B entradas
    CHECK(p.boneInfos.size() == 3u * 2u);
    // 1+2+3 keys, concatenadas sin solapar
    CHECK(p.pos.size() == 6u);
    CHECK(p.rot.size() == 6u);
    CHECK(p.scale.size() == 6u);

    // Layout [clip][hueso]: el hueso 0 del clip c está en c*2 + 0
    CHECK(p.boneInfos[0 * 2 + 0].posCount == 1);
    CHECK(p.boneInfos[1 * 2 + 0].posCount == 2);
    CHECK(p.boneInfos[2 * 2 + 0].posCount == 3);
    // El hueso 1 no tiene canal en ningún clip
    CHECK(p.boneInfos[0 * 2 + 1].posCount == 0);
    CHECK(p.boneInfos[2 * 2 + 1].posCount == 0);

    // Offsets crecientes y sin solape
    CHECK(p.boneInfos[0 * 2 + 0].posOffset == 0);
    CHECK(p.boneInfos[1 * 2 + 0].posOffset == 1);
    CHECK(p.boneInfos[2 * 2 + 0].posOffset == 3);

    // Datos de esqueleto replicados idénticos en cada bloque de clip
    for (int c = 0; c < 3; c++)
    {
        CHECK(p.boneInfos[c * 2 + 0].parentIndex == -1);
        CHECK(p.boneInfos[c * 2 + 1].parentIndex == 0);
        CHECK(p.boneInfos[c * 2 + 0].inverseBindPose == glm::mat4(2.0f));
        CHECK(p.boneInfos[c * 2 + 1].inverseBindPose == glm::mat4(3.0f));
    }

    // El bloque del clip 2 apunta a SUS keys, no a las del clip 0
    CHECK(nearlyEqual(p.pos[p.boneInfos[2 * 2 + 0].posOffset].value.x, 2.0f));
    CHECK(nearlyEqual(p.pos[p.boneInfos[0 * 2 + 0].posOffset].value.x, 0.0f));
}

// Malla sin animaciones: un bloque de clip con todos los counts a 0, y los
// buffers nunca vacíos (Vulkan no acepta buffers de tamaño 0).
static void test_pack_mesh_without_clips()
{
    SkinnedMesh m;
    m.skeleton.names           = { "root" };
    m.skeleton.parentIndex     = { -1 };
    m.skeleton.inverseBindPose = { glm::mat4(1.0f) };

    PackedClips p = packSkinnedClips(m);

    CHECK(p.boneInfos.size() == 1u);
    CHECK(p.boneInfos[0].posCount == 0);
    CHECK(p.boneInfos[0].parentIndex == -1);
    CHECK(p.pos.size() == 1u);     // dummy
    CHECK(p.rot.size() == 1u);
    CHECK(p.scale.size() == 1u);
}
```

Y en `main()`, añade las llamadas tras `test_loader_reads_all_clips();`:

```cpp
    test_pack_concatenates_clips();
    test_pack_mesh_without_clips();
```

- [ ] **Step 2: Correr el test para verificar que falla**

```powershell
.\build.bat
```

Esperado: FALLO DE COMPILACIÓN — `cannot open include file: 'DonTopo/Renderer/SkinnedMeshPacking.h'`.

- [ ] **Step 3: Crear el header**

Crea `engine/include/DonTopo/Renderer/SkinnedMeshPacking.h`:

```cpp
#pragma once
#include <vector>
#include "DonTopo/Renderer/SkinnedMesh.h"

namespace DonTopo
{
    // Keyframes de TODOS los clips concatenados en los mismos vectores, listos
    // pa subir a los 3 SSBOs de una sola vez al construir el objeto. Cambiar de
    // clip en runtime no vuelve a tocar VRAM: solo cambia el clipBase del push
    // constant.
    //
    // boneInfos va en layout [clip][hueso]: la entrada del hueso b en el clip c
    // está en boneInfos[c * boneCount + b], y c * boneCount es exactamente el
    // clipBase que consume bone_eval.comp.
    //
    // parentIndex e inverseBindPose son del ESQUELETO, no del clip, así que se
    // replican idénticos en cada bloque. Eso cuesta 96 B por hueso y clip (2,4 %
    // sobre los keyframes de un personaje típico) y a cambio deja el bloque del
    // clip 0 sirviendo de jerarquía válida pa cualquier clip — por eso
    // bone_hierarchy.comp no necesita saber nada de clips.
    struct PackedClips
    {
        std::vector<GpuPosKey>   pos;
        std::vector<GpuRotKey>   rot;
        std::vector<GpuPosKey>   scale;
        std::vector<GpuBoneInfo> boneInfos;
    };

    // Función libre y pura (sin Vulkan) a propósito: dentro de
    // Renderer::addSkinnedMesh este empaquetado solo se podría probar con un
    // VkDevice vivo, es decir, no se podría probar.
    PackedClips packSkinnedClips(const SkinnedMesh& mesh);
}
```

- [ ] **Step 4: Implementar el packing**

Crea `engine/src/Renderer/SkinnedMeshPacking.cpp`:

```cpp
#include "DonTopo/Renderer/SkinnedMeshPacking.h"

namespace DonTopo
{
    PackedClips packSkinnedClips(const SkinnedMesh& mesh)
    {
        const Skeleton& skel      = mesh.skeleton;
        const int       boneCount = (int)skel.names.size();
        // Malla sin animaciones: un bloque igualmente, con todos los counts a 0.
        // bone_eval devuelve entonces la identidad, que es lo que hacía el
        // código de un solo clip cuando el FBX no traía animación.
        const size_t clipCount = mesh.animationClips.empty() ? 1u : mesh.animationClips.size();

        PackedClips out;
        out.boneInfos.resize(clipCount * (size_t)boneCount);

        for (size_t c = 0; c < clipCount; c++)
        {
            const AnimationClip* clip = mesh.animationClips.empty() ? nullptr : &mesh.animationClips[c];

            for (int b = 0; b < boneCount; b++)
            {
                GpuBoneInfo& bi    = out.boneInfos[c * (size_t)boneCount + (size_t)b];
                bi.parentIndex     = skel.parentIndex[b];
                bi.inverseBindPose = skel.inverseBindPose[b];
                bi.pad             = 0;

                const BoneChannel* ch = nullptr;
                if (clip)
                    for (auto& cc : clip->channels)
                        if (cc.boneIndex == b) { ch = &cc; break; }

                bi.posOffset = (int)out.pos.size();
                bi.posCount  = ch ? (int)ch->posKeys.size() : 0;
                for (int k = 0; k < bi.posCount; k++)
                {
                    GpuPosKey pk{};
                    pk.timePad = { ch->posKeys[k].time, 0, 0, 0 };
                    pk.value   = { ch->posKeys[k].value.x, ch->posKeys[k].value.y, ch->posKeys[k].value.z, 0 };
                    out.pos.push_back(pk);
                }

                bi.rotOffset = (int)out.rot.size();
                bi.rotCount  = ch ? (int)ch->rotKeys.size() : 0;
                for (int k = 0; k < bi.rotCount; k++)
                {
                    const glm::quat& q = ch->rotKeys[k].value;
                    GpuRotKey rk{};
                    rk.timePad = { ch->rotKeys[k].time, 0, 0, 0 };
                    rk.value   = { q.x, q.y, q.z, q.w };
                    out.rot.push_back(rk);
                }

                bi.scaleOffset = (int)out.scale.size();
                bi.scaleCount  = ch ? (int)ch->scaleKeys.size() : 0;
                for (int k = 0; k < bi.scaleCount; k++)
                {
                    GpuPosKey sk{};
                    sk.timePad = { ch->scaleKeys[k].time, 0, 0, 0 };
                    sk.value   = { ch->scaleKeys[k].value.x, ch->scaleKeys[k].value.y, ch->scaleKeys[k].value.z, 0 };
                    out.scale.push_back(sk);
                }
            }
        }

        // Vulkan no acepta buffers de tamaño 0
        if (out.pos.empty())   out.pos.push_back({});
        if (out.rot.empty())   out.rot.push_back({});
        if (out.scale.empty()) out.scale.push_back({});

        return out;
    }
}
```

- [ ] **Step 5: Registrar la fuente nueva**

En `engine/CMakeLists.txt`, tras `src/Renderer/ModelLoader.cpp` (línea 14), añade:

```cmake
    src/Renderer/SkinnedMeshPacking.cpp
```

- [ ] **Step 6: Hacer que `addSkinnedMesh` use la función extraída**

En `engine/src/Renderer/Renderer.cpp`, añade el include junto a los demás de Renderer:

```cpp
#include "DonTopo/Renderer/SkinnedMeshPacking.h"
```

Sustituye TODO el bloque desde `// --- Flatten keyframes a GPU format ---` (línea 1845)
hasta el cierre del `if (allScale.empty()) allScale.push_back({});` (línea 1897) por:

```cpp
        // --- Flatten keyframes de TODOS los clips a formato GPU ---
        // (packSkinnedClips vive fuera pa poder probarse sin un VkDevice)
        const PackedClips packed = packSkinnedClips(mesh);
        obj.clipCount = (uint32_t)(mesh.animationClips.empty() ? 1u : mesh.animationClips.size());
```

Y en los 4 `uploadBuffer` siguientes (líneas ~1900-1907), sustituye los vectores locales por
los campos de `packed`:

```cpp
        // --- Upload SSBOs estáticos ---
        m_res.uploadBuffer(packed.pos.data(),   packed.pos.size()   * sizeof(GpuPosKey),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, obj.keyframePosBuffer,   obj.keyframePosMemory);
        m_res.uploadBuffer(packed.rot.data(),   packed.rot.size()   * sizeof(GpuRotKey),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, obj.keyframeRotBuffer,   obj.keyframeRotMemory);
        m_res.uploadBuffer(packed.scale.data(), packed.scale.size() * sizeof(GpuPosKey),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, obj.keyframeScaleBuffer, obj.keyframeScaleMemory);
        m_res.uploadBuffer(packed.boneInfos.data(), packed.boneInfos.size() * sizeof(GpuBoneInfo),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, obj.boneInfoBuffer,      obj.boneInfoMemory);
```

`obj.clipCount` todavía no existe: lo añade el Step 7.

- [ ] **Step 7: Añadir `clipCount` a `SkinnedRenderObject`**

En `engine/include/DonTopo/Renderer/Renderer.h`, en `struct SkinnedRenderObject`, tras
`uint32_t boneCount = 0;` (línea 205), añade:

```cpp
                // Nº de clips concatenados en los SSBOs de keyframes. Solo se usa
                // pa clampar en setAnimationState (Task 3): un clipIndex fuera de
                // rango haría que clipBase apuntara fuera del SSBO de BoneInfos y
                // el compute leyera basura sin que nada avisara.
                uint32_t       clipCount            = 1;
```

- [ ] **Step 8: Compilar y correr los tests**

```powershell
.\configure.bat
.\build.bat
.\build-ninja\engine\tests\dt_animator_tests.exe
```

Esperado: `dt_animator_tests: OK`, exit code 0.

- [ ] **Step 9: Verificar que no hay regresión visual**

```powershell
.\build-ninja\sandbox\Sandbox.exe
```

El personaje animado debe verse EXACTAMENTE igual que antes (un solo clip, `clipBase` aún no
existe, así que `boneInfos` se lee desde el índice 0). Si se deforma, el packing está mal.
Cierra la ventana.

- [ ] **Step 10: Commit**

```bash
git add engine/include/DonTopo/Renderer/SkinnedMeshPacking.h engine/src/Renderer/SkinnedMeshPacking.cpp engine/include/DonTopo/Renderer/Renderer.h engine/src/Renderer/Renderer.cpp engine/CMakeLists.txt engine/tests/animator_tests.cpp
git commit -m "refactor(anim): extract keyframe packing out of addSkinnedMesh

Concatenates every clip into the same three SSBOs, with BoneInfos laid
out as [clip][bone]. parentIndex and inverseBindPose belong to the
skeleton rather than the clip, so replicating them per clip block costs
2.4% over the keyframes and lets clip 0's block serve as valid hierarchy
for any clip -- which is why bone_hierarchy.comp needs no change.

Extracted because inside addSkinnedMesh this could only be tested with a
live VkDevice, i.e. not at all. Behaviour is unchanged: clipBase does not
exist yet, so reads still start at index 0.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: `clipBase` — conmutar de clip en GPU

**Files:**
- Modify: `engine/include/DonTopo/Renderer/Renderer.h:163-169,179-216,105`
- Modify: `engine/src/Renderer/Renderer.cpp:2248-2252,2078-2086`
- Modify: `shaders/bone_eval.comp:14-25,59`

**Interfaces:**
- Consumes: `PackedClips` / `obj.clipCount` (Task 2).
- Produces: `void Renderer::setAnimationState(int index, uint32_t clipIndex, float animTime)` — Task 7 lo consume.

- [ ] **Step 1: Renombrar `pad` → `clipBase` en `ComputePush`**

En `engine/include/DonTopo/Renderer/Renderer.h`, sustituye `struct ComputePush` (líneas 163-169) por:

```cpp
            // ABI compartida por los 3 compute shaders. 16 bytes, fijos: el 4º
            // campo era un pad sin usar y ahora lleva el clipBase, así que
            // ningún offset se ha movido.
            struct ComputePush
            {
                float animTime;
                uint32_t boneCount;
                uint32_t vertexCount;
                // activeClip * boneCount: índice base del bloque del clip activo
                // dentro del SSBO de BoneInfos, que va en layout [clip][hueso].
                // Solo lo lee bone_eval.comp; bone_hierarchy y skinning declaran
                // este slot como "pad" y no lo tocan.
                uint32_t clipBase;
            };
            static_assert(sizeof(ComputePush) == 16, "ComputePush debe seguir en 16 bytes: los 3 .comp declaran este layout");
```

- [ ] **Step 2: Añadir `activeClip` y declarar `setAnimationState`**

En `engine/include/DonTopo/Renderer/Renderer.h`, en `struct SkinnedRenderObject`, en el
bloque `// Estado de animación` (línea 211), tras `float animTime = 0.0f;` añade:

```cpp
                // Índice del clip que se evalúa este frame. Sin blending solo se
                // evalúa uno: los demás residen en el SSBO y no se leen.
                uint32_t  activeClip     = 0;
```

Y en la sección pública, tras `void updateAnimation(int index, float deltaTime);` (línea 105), añade:

```cpp
            // Sink puro: fija el clip y el tiempo que el Animator ya ha
            // calculado en CPU. No avanza el tiempo — a diferencia de
            // updateAnimation, que sigue siendo el camino de los objetos SIN
            // AnimatorComponent. Los dos no se pisan: quien tiene Animator nunca
            // pasa por updateAnimation.
            void setAnimationState(int index, uint32_t clipIndex, float animTime);```
```

- [ ] **Step 3: Indexar `BoneInfos` con `clipBase` en el shader**

En `shaders/bone_eval.comp`, sustituye el bloque `layout(push_constant)` (líneas 20-25) por:

```glsl
layout(push_constant) uniform PC {
    float animTime;
    uint  boneCount;
    uint  vertexCount;
    // activeClip * boneCount. BoneInfos va en layout [clip][hueso], así que
    // esto salta al bloque del clip activo. bone_hierarchy y skinning declaran
    // este mismo slot como "pad" y no lo leen: parentIndex e inverseBindPose
    // son iguales en todos los bloques, así que el del clip 0 les vale.
    uint  clipBase;
} push;
```

Y la línea 59, `BoneInfo info = boneInfos.data[bi];`, por:

```glsl
    BoneInfo info = boneInfos.data[push.clipBase + bi];
```

No toques nada más del fichero. NO toques `bone_hierarchy.comp` ni `skinning.comp`.

- [ ] **Step 4: Recompilar el SPIR-V (lo hace el build)**

`build.bat` recompila el shader solo: `sandbox/CMakeLists.txt:11-56` corre `glslc` sobre
`shaders/*.comp` vía el target `Shaders ALL` y copia los `.spv` al lado del ejecutable y de
vuelta a `shaders/`. `Renderer::createComputePipelines` carga esos `.spv`
(`Renderer.cpp:1673`), que están en `.gitignore`.

No hace falta ningún paso manual: basta con el `.\build.bat` del Step 7. Un error de sintaxis
GLSL sale en la salida del build, en la línea `Compiling shader bone_eval.comp`.

- [ ] **Step 5: Cargar `clipBase` en el push constant**

En `engine/src/Renderer/Renderer.cpp`, en `recordComputePass`, sustituye la línea 2252
(`push.pad = 0;`) por:

```cpp
            push.clipBase    = obj.activeClip * obj.boneCount;
```

- [ ] **Step 6: Implementar `setAnimationState`**

En `engine/src/Renderer/Renderer.cpp`, justo después del cierre de `Renderer::updateAnimation`
(línea 2086), añade:

```cpp
    void Renderer::setAnimationState(int index, uint32_t clipIndex, float animTime)
    {
        if (index < 0 || index >= (int)m_skinnedObjects.size()) return;
        auto& obj = m_skinnedObjects[index];
        // Clamp y no assert: un clipIndex fuera de rango (escena con un grafo que
        // referencia un clip que el FBX ya no trae) haría que clipBase apuntara
        // fuera del SSBO de BoneInfos, y el compute leería basura en silencio.
        obj.activeClip = (clipIndex < obj.clipCount) ? clipIndex : 0;
        obj.animTime   = animTime;
    }
```

- [ ] **Step 7: Compilar y verificar que no hay regresión**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_animator_tests.exe
.\build-ninja\sandbox\Sandbox.exe
```

Los tests deben seguir en `OK`. En el sandbox, el personaje animado debe verse EXACTAMENTE
igual que antes: `activeClip` es 0, luego `clipBase` es 0, luego la ruta es byte a byte la de
siempre. Si se deforma, sospecha `.spv` stale (¿corriste el Step 4?) o build stale de Ninja.
Cierra la ventana.

- [ ] **Step 8: Commit**

```bash
git add engine/include/DonTopo/Renderer/Renderer.h engine/src/Renderer/Renderer.cpp shaders/bone_eval.comp
git commit -m "feat(anim): index bone keyframes by active clip"
```

Cuerpo del mensaje (pégalo con `-m` adicional o un editor):

```
ComputePush.pad was declared by all three compute shaders and always
written as zero. It now carries clipBase (activeClip * boneCount), which
bone_eval uses to jump to the active clip's block in BoneInfos. Offsets
and size are unchanged, so the push constant ABI stays at 16 bytes and
the descriptor bindings are untouched.

bone_hierarchy.comp and skinning.comp need no change: they only read
parentIndex and inverseBindPose, which are per-skeleton and therefore
identical in every clip block, so clip 0's block serves them.

With activeClip = 0 the render path is byte-for-byte the previous one.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
```

---

### Task 4: `AnimatorComponent` — grafo y runtime

**Files:**
- Create: `engine/include/DonTopo/Core/AnimatorComponent.h`
- Create: `engine/src/Core/AnimatorComponent.cpp`
- Modify: `engine/CMakeLists.txt:20`
- Modify: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: nada. Es independiente de las Tasks 1-3.
- Produces: la clase `DonTopo::AnimatorComponent` con los tipos anidados `ConditionType`,
  `ParamType`, `Condition`, `Transition`, `State`, `Parameter` y los métodos listados abajo.
  Tasks 5, 6, 8, 10, 11 y 12 los consumen. Las firmas exactas de esta task son el contrato.

- [ ] **Step 1: Escribir los tests que fallan**

En `engine/tests/animator_tests.cpp`, añade el include:

```cpp
#include "DonTopo/Core/AnimatorComponent.h"
```

y estas funciones antes de `main()`:

```cpp
// Helper: grafo Idle(loop) -> Run(loop) -> Jump(no loop) -> Idle.
// Idle->Run   por bool "running" == true
// Run->Jump   por trigger "jump"
// Jump->Idle  por animation finished
static AnimatorComponent makeGraph()
{
    AnimatorComponent a;

    AnimatorComponent::State idle;
    idle.name = "Idle"; idle.clipName = "Idle";
    idle.clipIndex = 0; idle.duration = 30.0f; idle.ticksPerSecond = 30.0f; idle.loop = true;

    AnimatorComponent::State run;
    run.name = "Run"; run.clipName = "Run";
    run.clipIndex = 1; run.duration = 20.0f; run.ticksPerSecond = 20.0f; run.loop = true;

    AnimatorComponent::State jump;
    jump.name = "Jump"; jump.clipName = "Jump";
    jump.clipIndex = 2; jump.duration = 10.0f; jump.ticksPerSecond = 10.0f; jump.loop = false;

    a.addState(idle);
    a.addState(run);
    a.addState(jump);
    a.setEntryState(0);

    a.addParameter("running", AnimatorComponent::ParamType::Bool);
    a.addParameter("jump",    AnimatorComponent::ParamType::Trigger);

    AnimatorComponent::Transition t0;
    t0.fromState = 0; t0.toState = 1;
    t0.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "running", true });
    a.addTransition(t0);

    AnimatorComponent::Transition t1;
    t1.fromState = 1; t1.toState = 2;
    t1.conditions.push_back({ AnimatorComponent::ConditionType::Trigger, "jump", true });
    a.addTransition(t1);

    AnimatorComponent::Transition t2;
    t2.fromState = 2; t2.toState = 0;
    t2.conditions.push_back({ AnimatorComponent::ConditionType::AnimationFinished, "", true });
    a.addTransition(t2);

    a.reset();
    return a;
}

// Criterio 2, parte 1: un trigger cambia el estado activo; sin trigger, no.
static void test_trigger_switches_state()
{
    AnimatorComponent a = makeGraph();
    CHECK(a.currentState() == 0);           // entry

    // Sin parámetros, nada dispara
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);

    // bool running -> Run
    a.setBool("running", true);
    a.update(0.016f, true);
    CHECK(a.currentState() == 1);
    CHECK(a.currentClipIndex() == 1);

    // trigger jump -> Jump
    a.setTrigger("jump");
    a.update(0.016f, true);
    CHECK(a.currentState() == 2);
    CHECK(a.currentClipIndex() == 2);
    // Al entrar en un estado, el tiempo arranca de cero
    CHECK(nearlyEqual(a.animTime(), 0.0f));
}

// Criterio 2, parte 2: "animation finished" NO dispara antes de duration y SÍ
// después. Jump: duration 10 ticks a 10 tps = 1 segundo real.
static void test_animation_finished_timing()
{
    AnimatorComponent a = makeGraph();
    a.setBool("running", true);
    a.update(0.016f, true);          // -> Run
    a.setTrigger("jump");
    a.update(0.016f, true);          // -> Jump
    CHECK(a.currentState() == 2);

    // 0.9 s < 1.0 s de duración: no ha terminado, no transiciona
    a.update(0.9f, true);
    CHECK(!a.finished());
    CHECK(a.currentState() == 2);

    // Pasado el final: finished y transición a Idle
    a.update(0.2f, true);
    CHECK(a.currentState() == 0);
}

// Criterio 2, parte 3: loop=false se clava en el último frame; loop=true reinicia.
static void test_loop_flag()
{
    // loop = false: animTime se clampa a duration y se queda ahí
    AnimatorComponent noLoop;
    AnimatorComponent::State s;
    s.name = "Once"; s.clipName = "Once";
    s.clipIndex = 0; s.duration = 10.0f; s.ticksPerSecond = 10.0f; s.loop = false;
    noLoop.addState(s);
    noLoop.setEntryState(0);
    noLoop.reset();

    noLoop.update(2.0f, true);       // 20 ticks > 10 de duración
    CHECK(nearlyEqual(noLoop.animTime(), 10.0f));
    CHECK(noLoop.finished());
    noLoop.update(2.0f, true);       // sigue clavado, no vuelve al principio
    CHECK(nearlyEqual(noLoop.animTime(), 10.0f));
    CHECK(noLoop.finished());

    // loop = true: reinicia y nunca se marca finished
    AnimatorComponent looping;
    AnimatorComponent::State l;
    l.name = "Cycle"; l.clipName = "Cycle";
    l.clipIndex = 0; l.duration = 10.0f; l.ticksPerSecond = 10.0f; l.loop = true;
    looping.addState(l);
    looping.setEntryState(0);
    looping.reset();

    looping.update(1.2f, true);      // 12 ticks -> fmod -> 2
    CHECK(nearlyEqual(looping.animTime(), 2.0f));
    CHECK(!looping.finished());
}

// Condición bool: dispara con expected cumplido, no con el contrario.
static void test_bool_condition_expected()
{
    AnimatorComponent a = makeGraph();
    a.setBool("running", false);
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);    // expected == true, running == false

    a.setBool("running", true);
    a.update(0.016f, true);
    CHECK(a.currentState() == 1);
}

// El trigger que consume la transición ganadora se apaga. Uno no consumido sigue
// encendido esperando (mismo comportamiento que Unity).
static void test_trigger_consumption()
{
    AnimatorComponent a = makeGraph();

    // "jump" seteado en Idle: ninguna transición que salga de Idle lo consume,
    // así que sigue armado cuando lleguemos a Run.
    a.setTrigger("jump");
    a.setBool("running", true);
    a.update(0.016f, true);          // Idle -> Run (por el bool)
    CHECK(a.currentState() == 1);

    a.update(0.016f, true);          // Run -> Jump: el trigger seguía armado
    CHECK(a.currentState() == 2);

    // Ya consumido: al volver a Idle y luego a Run, no vuelve a saltar
    a.update(2.0f, true);            // Jump termina -> Idle
    CHECK(a.currentState() == 0);
    a.update(0.016f, true);          // -> Run (running sigue true)
    CHECK(a.currentState() == 1);
    a.update(0.016f, true);          // se queda: el trigger ya se gastó
    CHECK(a.currentState() == 1);
}

// evaluateTransitions == false (Edit Mode): el tiempo avanza pero el grafo no se
// mueve. Sin esto, las condiciones "animation finished" pasearían el grafo solo
// en el editor.
static void test_edit_mode_does_not_transition()
{
    AnimatorComponent a = makeGraph();
    a.setBool("running", true);
    a.setTrigger("jump");

    a.update(0.5f, false);
    CHECK(a.currentState() == 0);            // no se ha movido
    CHECK(a.animTime() > 0.0f);              // pero el tiempo corre

    a.update(0.016f, true);
    CHECK(a.currentState() == 1);            // en Play sí
}

// Borrar un estado reindexa las transiciones que apuntaban por encima y tira las
// que lo tocaban. Sin esto, borrar un nodo dejaría links apuntando a otro estado.
static void test_remove_state_reindexes()
{
    AnimatorComponent a = makeGraph();
    a.removeState(1);                        // borra "Run"

    CHECK(a.states().size() == 2u);
    CHECK(a.states()[0].name == "Idle");
    CHECK(a.states()[1].name == "Jump");
    // Idle->Run y Run->Jump se van; Jump->Idle sobrevive reindexada (2 -> 1)
    CHECK(a.transitions().size() == 1u);
    CHECK(a.transitions()[0].fromState == 1);
    CHECK(a.transitions()[0].toState == 0);
    CHECK(a.entryState() == 0);
}
```

Y en `main()`, tras las llamadas de la Task 2:

```cpp
    test_trigger_switches_state();
    test_animation_finished_timing();
    test_loop_flag();
    test_bool_condition_expected();
    test_trigger_consumption();
    test_edit_mode_does_not_transition();
    test_remove_state_reindexes();
```

- [ ] **Step 2: Correr para verificar que falla**

```powershell
.\build.bat
```

Esperado: FALLO DE COMPILACIÓN — `cannot open include file: 'DonTopo/Core/AnimatorComponent.h'`.

- [ ] **Step 3: Escribir el header**

Crea `engine/include/DonTopo/Core/AnimatorComponent.h`:

```cpp
#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

namespace DonTopo
{
    struct SkinnedMesh;

    // Máquina de estados de animación (equivalente al Animator de Unity). Cada
    // estado contiene un clip; los links son transiciones dirigidas.
    //
    // Data + lógica pura: sin Vulkan y sin conocer GameObject, misma regla que
    // CameraComponent y Rigidbody (la dependencia va Core -> resto, nunca al
    // revés). Eso es lo que deja probarlo entero sin GPU ni ventana.
    //
    // Es el ÚNICO dueño de animTime: el Renderer solo recibe (clip, tiempo) ya
    // calculados vía Renderer::setAnimationState. Partir el tiempo entre los dos
    // daría dos fuentes de verdad.
    //
    // Sin blending: una transición es un corte instantáneo, y solo se evalúa un
    // clip por frame.
    class AnimatorComponent
    {
        public:
            enum class ConditionType { Bool, Trigger, AnimationFinished };
            enum class ParamType     { Bool, Trigger };

            struct Condition
            {
                ConditionType type     = ConditionType::Bool;
                std::string   paramName;          // vacío si AnimationFinished
                bool          expected = true;    // solo Bool
            };

            struct Transition
            {
                int fromState = -1;
                int toState   = -1;
                // AND de todas: la transición dispara cuando se cumplen todas.
                std::vector<Condition> conditions;
            };

            struct State
            {
                std::string name;
                // El clip se referencia por NOMBRE, no por índice: el índice
                // depende del orden de mAnimations en el FBX, y reexportar el
                // modelo lo baraja. bindClips resuelve nombre -> clipIndex.
                std::string clipName;
                int         clipIndex      = -1;
                // Cacheados por bindClips pa que el componente sea auto-contenido
                // (y probable sin FBX ni Vulkan).
                float       duration       = 0.0f;    // ticks
                float       ticksPerSecond = 24.0f;
                // Autoría del usuario (checkbox del nodo), NO cacheado del clip:
                // el SkinnedMesh se reconstruye desde el FBX en cada carga y no
                // se serializa, así que un loop guardado ahí se perdería.
                bool        loop           = true;
                // Posición del nodo en el canvas del AnimatorPanel.
                glm::vec2   editorPos{0.0f};
            };

            struct Parameter
            {
                std::string name;
                ParamType   type = ParamType::Bool;
            };

            // --- Diseño (editor / carga de escena) ---
            int  addState(State s);                 // devuelve el índice del nuevo estado
            void addTransition(Transition t);
            void removeState(int idx);              // reindexa las transiciones
            void removeTransition(int idx);
            void setEntryState(int idx);
            void addParameter(std::string name, ParamType type);
            void removeParameter(const std::string& name);

            const std::vector<State>&      states()      const { return m_states; }
            const std::vector<Transition>& transitions() const { return m_transitions; }
            const std::vector<Parameter>&  parameters()  const { return m_parameters; }
            int                            entryState()  const { return m_entryState; }

            // Acceso mutable pa la UI (editar nombre/loop/editorPos in situ sin
            // reconstruir el estado entero).
            std::vector<State>&      statesMutable()      { return m_states; }
            std::vector<Transition>& transitionsMutable() { return m_transitions; }

            // Resuelve clipName -> clipIndex y cachea duration/ticksPerSecond de
            // cada estado. Un clipName que no exista en la malla deja clipIndex a
            // -1 y empuja un aviso (falla ruidoso, no silencioso). NO toca loop.
            void bindClips(const SkinnedMesh& mesh, std::vector<std::string>* warnings = nullptr);

            // --- Runtime ---
            void setBool(const std::string& n, bool v);
            bool getBool(const std::string& n) const;
            void setTrigger(const std::string& n);

            // evaluateTransitions == false (Edit Mode): avanza el tiempo del
            // estado actual pero no mueve el grafo.
            void update(float dt, bool evaluateTransitions);

            int   currentState()     const { return m_currentState; }
            int   currentClipIndex() const;
            float animTime()         const { return m_animTime; }   // ticks
            bool  finished()         const { return m_finished; }
            // Nombre del estado actual, "" si el grafo está vacío. Lo consume Lua.
            std::string currentStateName() const;

            // Vuelve al estado de entrada, tiempo a 0, parámetros y triggers a
            // false. El Stop de Play no necesita llamarlo (reconstruye la escena
            // desde JSON), pero el editor sí al reeditar el grafo.
            void reset();

        private:
            bool conditionsMet(const Transition& t) const;
            void consumeTriggers(const Transition& t);
            bool isTriggerSet(const std::string& n) const;
            bool hasParam(const std::string& n, ParamType type) const;

            std::vector<State>      m_states;
            std::vector<Transition> m_transitions;
            std::vector<Parameter>  m_parameters;
            int                     m_entryState   = -1;

            int                     m_currentState = -1;
            float                   m_animTime     = 0.0f;
            bool                    m_finished     = false;
            std::unordered_map<std::string, bool> m_bools;
            std::unordered_map<std::string, bool> m_triggers;
    };
}
```

- [ ] **Step 4: Implementar**

Crea `engine/src/Core/AnimatorComponent.cpp`:

```cpp
#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include <algorithm>
#include <cmath>

namespace DonTopo
{
    int AnimatorComponent::addState(State s)
    {
        m_states.push_back(std::move(s));
        // Primer estado añadido: entrada por defecto. Un grafo sin entrada no
        // arranca, y obligar a marcarla a mano sería un pie en el que tropezar.
        if (m_entryState < 0) m_entryState = 0;
        return (int)m_states.size() - 1;
    }

    void AnimatorComponent::addTransition(Transition t) { m_transitions.push_back(std::move(t)); }

    void AnimatorComponent::removeState(int idx)
    {
        if (idx < 0 || idx >= (int)m_states.size()) return;
        m_states.erase(m_states.begin() + idx);

        // Las transiciones guardan índices: borrar un estado invalida las que lo
        // tocan y desplaza las que apuntan por encima. Sin esto, borrar un nodo
        // dejaría links apuntando a un estado distinto del que el usuario ve.
        m_transitions.erase(
            std::remove_if(m_transitions.begin(), m_transitions.end(),
                [idx](const Transition& t) { return t.fromState == idx || t.toState == idx; }),
            m_transitions.end());
        for (auto& t : m_transitions)
        {
            if (t.fromState > idx) t.fromState--;
            if (t.toState   > idx) t.toState--;
        }

        if (m_entryState == idx)      m_entryState = m_states.empty() ? -1 : 0;
        else if (m_entryState > idx)  m_entryState--;

        reset();
    }

    void AnimatorComponent::removeTransition(int idx)
    {
        if (idx < 0 || idx >= (int)m_transitions.size()) return;
        m_transitions.erase(m_transitions.begin() + idx);
    }

    void AnimatorComponent::setEntryState(int idx)
    {
        if (idx < 0 || idx >= (int)m_states.size()) return;
        m_entryState = idx;
        reset();
    }

    void AnimatorComponent::addParameter(std::string name, ParamType type)
    {
        if (name.empty()) return;
        for (const auto& p : m_parameters)
            if (p.name == name) return;      // nombres únicos: se consultan por nombre
        m_parameters.push_back({ std::move(name), type });
        const std::string& n = m_parameters.back().name;
        if (type == ParamType::Bool) m_bools[n] = false;
        else                         m_triggers[n] = false;
    }

    void AnimatorComponent::removeParameter(const std::string& name)
    {
        m_parameters.erase(
            std::remove_if(m_parameters.begin(), m_parameters.end(),
                [&name](const Parameter& p) { return p.name == name; }),
            m_parameters.end());
        m_bools.erase(name);
        m_triggers.erase(name);

        // Las condiciones que lo usaban quedarían colgadas y no dispararían
        // nunca: se van con él.
        for (auto& t : m_transitions)
            t.conditions.erase(
                std::remove_if(t.conditions.begin(), t.conditions.end(),
                    [&name](const Condition& c) { return c.paramName == name; }),
                t.conditions.end());
    }

    bool AnimatorComponent::hasParam(const std::string& n, ParamType type) const
    {
        for (const auto& p : m_parameters)
            if (p.name == n) return p.type == type;
        return false;
    }

    void AnimatorComponent::setBool(const std::string& n, bool v)
    {
        if (!hasParam(n, ParamType::Bool)) return;
        m_bools[n] = v;
    }

    bool AnimatorComponent::getBool(const std::string& n) const
    {
        auto it = m_bools.find(n);
        return it != m_bools.end() && it->second;
    }

    void AnimatorComponent::setTrigger(const std::string& n)
    {
        if (!hasParam(n, ParamType::Trigger)) return;
        m_triggers[n] = true;
    }

    bool AnimatorComponent::isTriggerSet(const std::string& n) const
    {
        auto it = m_triggers.find(n);
        return it != m_triggers.end() && it->second;
    }

    int AnimatorComponent::currentClipIndex() const
    {
        if (m_currentState < 0 || m_currentState >= (int)m_states.size()) return 0;
        const int ci = m_states[m_currentState].clipIndex;
        return ci >= 0 ? ci : 0;
    }

    std::string AnimatorComponent::currentStateName() const
    {
        if (m_currentState < 0 || m_currentState >= (int)m_states.size()) return "";
        return m_states[m_currentState].name;
    }

    void AnimatorComponent::reset()
    {
        m_currentState = m_entryState;
        m_animTime     = 0.0f;
        m_finished     = false;
        for (auto& b : m_bools)    b.second = false;
        for (auto& t : m_triggers) t.second = false;
    }

    void AnimatorComponent::bindClips(const SkinnedMesh& mesh, std::vector<std::string>* warnings)
    {
        for (auto& st : m_states)
        {
            int found = -1;
            for (size_t i = 0; i < mesh.animationClips.size(); i++)
                if (mesh.animationClips[i].name == st.clipName) { found = (int)i; break; }

            if (found < 0)
            {
                st.clipIndex = -1;
                if (warnings)
                    warnings->push_back("Animator: el estado '" + st.name + "' referencia el clip '" +
                                        st.clipName + "', que no existe en el modelo");
                continue;
            }
            st.clipIndex      = found;
            st.duration       = mesh.animationClips[found].duration;
            st.ticksPerSecond = mesh.animationClips[found].ticksPerSecond;
            // st.loop NO se toca: es autoría del usuario, no un dato del FBX.
        }
        reset();
    }

    bool AnimatorComponent::conditionsMet(const Transition& t) const
    {
        // Una transición sin condiciones dispararía el frame en que se crea y
        // haría el grafo inusable. Unity cubre ese caso con exit time, que está
        // fuera de alcance.
        if (t.conditions.empty()) return false;

        for (const auto& c : t.conditions)
        {
            switch (c.type)
            {
                case ConditionType::Bool:
                    if (getBool(c.paramName) != c.expected) return false;
                    break;
                case ConditionType::Trigger:
                    if (!isTriggerSet(c.paramName)) return false;
                    break;
                case ConditionType::AnimationFinished:
                    if (!m_finished) return false;
                    break;
            }
        }
        return true;
    }

    void AnimatorComponent::consumeTriggers(const Transition& t)
    {
        // Solo los de la transición que gana: un trigger que nadie consume sigue
        // armado esperando (mismo comportamiento que Unity).
        for (const auto& c : t.conditions)
            if (c.type == ConditionType::Trigger)
                m_triggers[c.paramName] = false;
    }

    void AnimatorComponent::update(float dt, bool evaluateTransitions)
    {
        if (m_currentState < 0 || m_currentState >= (int)m_states.size())
        {
            m_currentState = m_entryState;
            if (m_currentState < 0 || m_currentState >= (int)m_states.size()) return;
        }

        const State& st = m_states[m_currentState];
        if (st.duration > 0.0f && st.ticksPerSecond > 0.0f)
        {
            m_animTime += dt * st.ticksPerSecond;
            if (m_animTime >= st.duration)
            {
                if (st.loop)
                {
                    m_animTime = std::fmod(m_animTime, st.duration);
                }
                else
                {
                    // Clavado en el último frame, y así se queda en los updates
                    // siguientes.
                    m_animTime = st.duration;
                    m_finished = true;
                }
            }
        }

        if (!evaluateTransitions) return;

        // Orden de declaración: la primera cuyo AND se cumple, gana. Determinista
        // y sin prioridades explícitas que mantener.
        for (const auto& t : m_transitions)
        {
            if (t.fromState != m_currentState) continue;
            if (t.toState < 0 || t.toState >= (int)m_states.size()) continue;
            if (!conditionsMet(t)) continue;

            consumeTriggers(t);
            m_currentState = t.toState;
            m_animTime     = 0.0f;
            m_finished     = false;
            return;                  // una transición por update
        }
    }
}
```

- [ ] **Step 5: Registrar la fuente nueva**

En `engine/CMakeLists.txt`, tras `src/Core/CameraComponent.cpp` (línea 20), añade:

```cmake
    src/Core/AnimatorComponent.cpp
```

- [ ] **Step 6: Compilar y correr los tests**

```powershell
.\configure.bat
.\build.bat
.\build-ninja\engine\tests\dt_animator_tests.exe
```

Esperado: `dt_animator_tests: OK`, exit code 0.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Core/AnimatorComponent.h engine/src/Core/AnimatorComponent.cpp engine/CMakeLists.txt engine/tests/animator_tests.cpp
git commit -m "feat(anim): add AnimatorComponent state machine"
```

Cuerpo del mensaje:

```
Data and logic only -- no Vulkan, no GameObject -- so the whole graph is
testable headless. It owns animTime outright: the renderer receives an
already-resolved (clip, time) pair, because splitting the clock between
the two would give it two sources of truth.

Transitions fire on the first rule whose conditions all hold, in
declaration order. Triggers stay armed until a transition consumes them,
and only the winning transition's triggers are consumed. A transition
with no conditions never fires: it would otherwise trip the frame it was
drawn, and Unity's answer to that case (exit time) is out of scope.

States cache duration and ticksPerSecond from the clip but never loop --
that flag is the user's, and the mesh it would come from is rebuilt from
the FBX on every scene load.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
```

---

### Task 5: Slot en `GameObject` + `bindClips`

**Files:**
- Modify: `engine/include/DonTopo/Core/GameObject.h:15,98,136`
- Modify: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `AnimatorComponent` (Task 4), `SkinnedMesh::animationClips` (Task 1).
- Produces: `GameObject::setAnimator(std::shared_ptr<AnimatorComponent>)`,
  `GameObject::getAnimator() const -> const std::shared_ptr<AnimatorComponent>&`,
  `GameObject::hasAnimator() const -> bool`. Tasks 6, 7, 8, 10, 11 y 12 los consumen.

- [ ] **Step 1: Escribir el test que falla**

En `engine/tests/animator_tests.cpp` añade el include:

```cpp
#include "DonTopo/Core/GameObject.h"
```

y esta función antes de `main()`:

```cpp
// bindClips resuelve el clip por NOMBRE y cachea duration/ticksPerSecond. Un
// nombre que no exista deja clipIndex a -1 y avisa: falla ruidoso, no silencioso.
// loop NO se toca: es del usuario, no del FBX.
static void test_bind_clips_resolves_by_name()
{
    SkinnedMesh mesh;
    mesh.skeleton.names           = { "root" };
    mesh.skeleton.parentIndex     = { -1 };
    mesh.skeleton.inverseBindPose = { glm::mat4(1.0f) };

    AnimationClip idle;  idle.name = "Idle"; idle.duration = 30.0f; idle.ticksPerSecond = 30.0f;
    AnimationClip run;   run.name  = "Run";  run.duration  = 20.0f; run.ticksPerSecond  = 20.0f;
    mesh.animationClips = { idle, run };

    AnimatorComponent a;
    AnimatorComponent::State s0; s0.name = "A"; s0.clipName = "Run";     s0.loop = false;
    AnimatorComponent::State s1; s1.name = "B"; s1.clipName = "NoExiste"; s1.loop = true;
    a.addState(s0);
    a.addState(s1);

    std::vector<std::string> warnings;
    a.bindClips(mesh, &warnings);

    // Resuelto por nombre, no por orden de declaración en el grafo
    CHECK(a.states()[0].clipIndex == 1);
    CHECK(nearlyEqual(a.states()[0].duration, 20.0f));
    CHECK(nearlyEqual(a.states()[0].ticksPerSecond, 20.0f));
    // loop lo puso el usuario: bindClips no lo pisa
    CHECK(a.states()[0].loop == false);

    // Clip inexistente: -1 y aviso
    CHECK(a.states()[1].clipIndex == -1);
    CHECK(warnings.size() == 1u);

    // clipIndex -1 no puede reventar el Renderer: currentClipIndex cae a 0
    a.setEntryState(1);
    CHECK(a.currentClipIndex() == 0);
}

// El slot sigue el patrón de CameraComponent, pero sin invariante de unicidad:
// cada GameObject skinned puede tener el suyo.
static void test_gameobject_animator_slot()
{
    GameObject go("personaje");
    CHECK(!go.hasAnimator());
    CHECK(go.getAnimator() == nullptr);

    go.setAnimator(std::make_shared<AnimatorComponent>());
    CHECK(go.hasAnimator());

    go.setAnimator(nullptr);
    CHECK(!go.hasAnimator());
}
```

Añade también `#include <memory>` si no está ya, y en `main()`:

```cpp
    test_bind_clips_resolves_by_name();
    test_gameobject_animator_slot();
```

- [ ] **Step 2: Correr para verificar que falla**

```powershell
.\build.bat
```

Esperado: FALLO DE COMPILACIÓN — `'setAnimator': is not a member of 'DonTopo::GameObject'`.

- [ ] **Step 3: Añadir el slot**

En `engine/include/DonTopo/Core/GameObject.h`, tras el include de `CameraComponent.h` (línea 15), añade:

```cpp
#include "DonTopo/Core/AnimatorComponent.h"
```

Tras el bloque de `hasCameraComponent()` (línea 98), añade:

```cpp
            // Animator: máquina de estados que decide qué clip del SkinnedMesh
            // se reproduce. A diferencia de la cámara, no hay invariante de
            // unicidad por escena: cada GameObject skinned lleva el suyo.
            void setAnimator(std::shared_ptr<AnimatorComponent> a) { m_animator = std::move(a); }
            const std::shared_ptr<AnimatorComponent>& getAnimator() const { return m_animator; }
            bool hasAnimator() const { return m_animator != nullptr; }
```

Y en la sección privada, tras `std::shared_ptr<CameraComponent> m_cameraComponent;` (línea 136):

```cpp
            std::shared_ptr<AnimatorComponent> m_animator;
```

- [ ] **Step 4: Correr los tests**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_animator_tests.exe
```

Esperado: `dt_animator_tests: OK`, exit code 0.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/Core/GameObject.h engine/tests/animator_tests.cpp
git commit -m "feat(anim): give GameObject an Animator slot"
```

Cuerpo:

```
States reference clips by name rather than index: the index depends on
the order of mAnimations in the FBX, so re-exporting the model would
silently repoint a state at the wrong animation. bindClips resolves the
name and leaves clipIndex at -1 with a warning when it cannot, and
currentClipIndex falls back to 0 so an unresolved state cannot push an
out-of-range clipBase at the compute shader.

Unlike the camera, there is no one-per-scene invariant: every skinned
GameObject can carry its own.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
```

---

### Task 6: Serialización del grafo en el JSON de escena

**Files:**
- Modify: `engine/src/Core/Scene.cpp:5,124-135,223-224,366-381,423,460,537,661`
- Modify: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `AnimatorComponent` (Task 4), `GameObject::setAnimator/getAnimator` (Task 5).
- Produces: el bloque JSON `"animator"` y las funciones del anon namespace de `Scene.cpp`
  `animatorToJson(const AnimatorComponent&) -> nlohmann::json` y
  `animatorFromJson(const nlohmann::json&) -> std::shared_ptr<AnimatorComponent>`.
  Ojo: viven en un anon namespace, así que NO son accesibles desde otras TUs — Task 8 no las
  usa, guarda una copia del componente.

- [ ] **Step 1: Escribir el test que falla**

En `engine/tests/animator_tests.cpp`, añade los includes:

```cpp
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include <nlohmann/json.hpp>
```

y esta función antes de `main()`:

```cpp
// Criterio 3: el grafo entero (nodos, posiciones, links, condiciones,
// parámetros, loop por nodo y estado de entrada) sobrevive guardar -> cargar.
static void test_graph_survives_scene_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    // nodeFromJson reutiliza el "id" del JSON cuando existe, así que el mismo id
    // resuelve el nodo en la escena cargada. Scene no tiene findByName.
    const uint64_t id = go->id;

    auto a = std::make_shared<AnimatorComponent>();

    AnimatorComponent::State idle;
    idle.name = "Idle"; idle.clipName = "ClipIdle";
    idle.loop = true;  idle.editorPos = glm::vec2(10.0f, 20.0f);
    AnimatorComponent::State jump;
    jump.name = "Jump"; jump.clipName = "ClipJump";
    jump.loop = false; jump.editorPos = glm::vec2(300.0f, 40.0f);
    a->addState(idle);
    a->addState(jump);
    a->setEntryState(1);                    // entrada NO por defecto, a propósito

    a->addParameter("running", AnimatorComponent::ParamType::Bool);
    a->addParameter("jump",    AnimatorComponent::ParamType::Trigger);

    AnimatorComponent::Transition t0;
    t0.fromState = 0; t0.toState = 1;
    t0.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "running", false });
    t0.conditions.push_back({ AnimatorComponent::ConditionType::Trigger, "jump", true });
    a->addTransition(t0);

    AnimatorComponent::Transition t1;
    t1.fromState = 1; t1.toState = 0;
    t1.conditions.push_back({ AnimatorComponent::ConditionType::AnimationFinished, "", true });
    a->addTransition(t1);

    go->setAnimator(a);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));

    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->name == "Personaje");
    CHECK(found->hasAnimator());
    if (!found->hasAnimator()) return;
    const AnimatorComponent& r = *found->getAnimator();

    // Estados: nombre, clip, loop, posición del nodo
    CHECK(r.states().size() == 2u);
    CHECK(r.states()[0].name == "Idle");
    CHECK(r.states()[0].clipName == "ClipIdle");
    CHECK(r.states()[0].loop == true);
    CHECK(nearlyEqual(r.states()[0].editorPos.x, 10.0f));
    CHECK(nearlyEqual(r.states()[0].editorPos.y, 20.0f));
    CHECK(r.states()[1].name == "Jump");
    CHECK(r.states()[1].clipName == "ClipJump");
    CHECK(r.states()[1].loop == false);
    CHECK(nearlyEqual(r.states()[1].editorPos.x, 300.0f));

    // Estado de entrada
    CHECK(r.entryState() == 1);

    // Parámetros con su tipo
    CHECK(r.parameters().size() == 2u);
    CHECK(r.parameters()[0].name == "running");
    CHECK(r.parameters()[0].type == AnimatorComponent::ParamType::Bool);
    CHECK(r.parameters()[1].name == "jump");
    CHECK(r.parameters()[1].type == AnimatorComponent::ParamType::Trigger);

    // Links y condiciones (incluido expected == false, que un default a true
    // se comería sin que nada fallara)
    CHECK(r.transitions().size() == 2u);
    CHECK(r.transitions()[0].fromState == 0);
    CHECK(r.transitions()[0].toState == 1);
    CHECK(r.transitions()[0].conditions.size() == 2u);
    CHECK(r.transitions()[0].conditions[0].type == AnimatorComponent::ConditionType::Bool);
    CHECK(r.transitions()[0].conditions[0].paramName == "running");
    CHECK(r.transitions()[0].conditions[0].expected == false);
    CHECK(r.transitions()[0].conditions[1].type == AnimatorComponent::ConditionType::Trigger);
    CHECK(r.transitions()[1].conditions[0].type == AnimatorComponent::ConditionType::AnimationFinished);
}

// Bloque aditivo: una escena guardada antes de que existiera "animator" carga
// igual, sin animator y sin avisos.
static void test_scene_without_animator_block_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    const uint64_t id = scene.addGameObject("Vacio")->id;
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(!found->hasAnimator());
}
```

Sustituye `main()` entero por:

```cpp
int main()
{
    // Una sola PxFoundation por proceso: un único PhysicsManager compartido por
    // todos los tests, nunca uno por test. physics/audio solo hacen falta porque
    // Scene::fromJson los exige en su firma pa recrear colliders y clips — estos
    // tests no simulan nada.
    PhysicsManager pm;
    pm.init();
    AudioManager am;
    am.init();

    test_loader_reads_all_clips();
    test_pack_concatenates_clips();
    test_pack_mesh_without_clips();
    test_trigger_switches_state();
    test_animation_finished_timing();
    test_loop_flag();
    test_bool_condition_expected();
    test_trigger_consumption();
    test_edit_mode_does_not_transition();
    test_remove_state_reindexes();
    test_bind_clips_resolves_by_name();
    test_gameobject_animator_slot();
    test_graph_survives_scene_round_trip(pm, am);
    test_scene_without_animator_block_loads(pm, am);

    am.shutdown();
    pm.shutdown();
    if (g_failures) { std::printf("dt_animator_tests: %d FAILURES\n", g_failures); std::fflush(stdout); return 1; }
    std::printf("dt_animator_tests: OK\n");
    std::fflush(stdout);
    return 0;
}
```

- [ ] **Step 2: Correr para verificar que falla**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_animator_tests.exe
```

Esperado: compila, pero `FAIL: found->hasAnimator()` — el bloque JSON aún no existe.

- [ ] **Step 3: Añadir los helpers de JSON**

En `engine/src/Core/Scene.cpp`, añade el include tras el de `CameraComponent.h` (línea 5):

```cpp
#include "DonTopo/Core/AnimatorComponent.h"
```

En el anon namespace, tras `using DonTopo::CameraComponent;` (línea 31), añade:

```cpp
    using DonTopo::AnimatorComponent;
```

Y tras la función `vec3ToJson` (línea 45), añade:

```cpp
    // Los enums van como string y no como int: legible en un .scene editado a
    // mano y estable si el enum crece por el medio. Mismo criterio que el "mode"
    // de la cámara.
    const char* paramTypeToStr(AnimatorComponent::ParamType t)
    {
        return t == AnimatorComponent::ParamType::Trigger ? "trigger" : "bool";
    }

    AnimatorComponent::ParamType paramTypeFromStr(const std::string& s)
    {
        return s == "trigger" ? AnimatorComponent::ParamType::Trigger
                              : AnimatorComponent::ParamType::Bool;
    }

    const char* condTypeToStr(AnimatorComponent::ConditionType t)
    {
        switch (t)
        {
            case AnimatorComponent::ConditionType::Trigger:           return "trigger";
            case AnimatorComponent::ConditionType::AnimationFinished: return "animationFinished";
            default:                                                  return "bool";
        }
    }

    AnimatorComponent::ConditionType condTypeFromStr(const std::string& s)
    {
        if (s == "trigger")           return AnimatorComponent::ConditionType::Trigger;
        if (s == "animationFinished") return AnimatorComponent::ConditionType::AnimationFinished;
        return AnimatorComponent::ConditionType::Bool;
    }

    nlohmann::json animatorToJson(const AnimatorComponent& a)
    {
        auto states = nlohmann::json::array();
        for (const auto& s : a.states())
        {
            // El clip va por NOMBRE: el índice depende del orden de mAnimations
            // en el FBX, y reexportar el modelo lo baraja en silencio.
            states.push_back({ {"name", s.name},
                               {"clip", s.clipName},
                               {"loop", s.loop},
                               {"pos", nlohmann::json::array({ s.editorPos.x, s.editorPos.y })} });
        }

        auto params = nlohmann::json::array();
        for (const auto& p : a.parameters())
            params.push_back({ {"name", p.name}, {"type", paramTypeToStr(p.type)} });

        auto transitions = nlohmann::json::array();
        for (const auto& t : a.transitions())
        {
            auto conds = nlohmann::json::array();
            for (const auto& c : t.conditions)
            {
                nlohmann::json cj = { {"type", condTypeToStr(c.type)} };
                if (c.type != AnimatorComponent::ConditionType::AnimationFinished)
                    cj["param"] = c.paramName;
                if (c.type == AnimatorComponent::ConditionType::Bool)
                    cj["expected"] = c.expected;
                conds.push_back(cj);
            }
            // from/to son índices al array "states" de ESTE mismo JSON:
            // self-contained, sin depender de ningún asset externo.
            transitions.push_back({ {"from", t.fromState}, {"to", t.toState}, {"conditions", conds} });
        }

        return { {"entryState", a.entryState()},
                 {"parameters", params},
                 {"states", states},
                 {"transitions", transitions} };
    }

    // No deserializa estado runtime (estado actual, animTime, valores de
    // parámetros, triggers pendientes) porque no se serializa: el Stop de Play
    // reconstruye la escena desde JSON, así que el reset al estado de entrada
    // sale gratis, y guardar en mitad de Play no hornea estado transitorio.
    std::shared_ptr<AnimatorComponent> animatorFromJson(const nlohmann::json& j)
    {
        auto a = std::make_shared<AnimatorComponent>();

        // Parámetros primero: addParameter es quien crea las entradas de bools/
        // triggers que las condiciones consultarán.
        if (j.contains("parameters"))
            for (const auto& p : j["parameters"])
                a->addParameter(p.value("name", std::string()),
                                paramTypeFromStr(p.value("type", std::string("bool"))));

        if (j.contains("states"))
        {
            for (const auto& s : j["states"])
            {
                AnimatorComponent::State st;
                st.name     = s.value("name", std::string());
                st.clipName = s.value("clip", std::string());
                st.loop     = s.value("loop", true);
                if (s.contains("pos") && s["pos"].is_array() && s["pos"].size() == 2)
                    st.editorPos = glm::vec2(s["pos"][0].get<float>(), s["pos"][1].get<float>());
                // duration/ticksPerSecond/clipIndex los rellena bindClips contra
                // el SkinnedMesh: son del FBX, no del fichero de escena.
                a->addState(st);
            }
        }

        if (j.contains("transitions"))
        {
            for (const auto& t : j["transitions"])
            {
                AnimatorComponent::Transition tr;
                tr.fromState = t.value("from", -1);
                tr.toState   = t.value("to", -1);
                if (t.contains("conditions"))
                {
                    for (const auto& c : t["conditions"])
                    {
                        AnimatorComponent::Condition cond;
                        cond.type      = condTypeFromStr(c.value("type", std::string("bool")));
                        cond.paramName = c.value("param", std::string());
                        cond.expected  = c.value("expected", true);
                        tr.conditions.push_back(cond);
                    }
                }
                a->addTransition(tr);
            }
        }

        // Después de addState: setEntryState valida contra m_states.size().
        a->setEntryState(j.value("entryState", 0));
        return a;
    }
```

- [ ] **Step 4: Serializar en `nodeToJson`**

En `engine/src/Core/Scene.cpp`, tras el bloque `if (node.hasCameraComponent())` (que termina
en la línea 135), añade:

```cpp
        if (node.hasAnimator())
            j["animator"] = animatorToJson(*node.getAnimator());
```

- [ ] **Step 5: Deserializar en `nodeFromJson` y enhebrar los avisos**

`nodeFromJson` es una función libre del anon namespace y no ve `Scene::m_warnings`, así que
gana un parámetro. Cambia su firma (líneas 223-224) a:

```cpp
    void nodeFromJson(const nlohmann::json& j, GameObject* node, const glm::mat4& parentWorld,
                       DonTopo::PhysicsManager& physics, DonTopo::AudioManager& audio,
                       std::vector<std::string>* warnings)
```

Tras el bloque `if (j.contains("camera"))` (que termina en la línea 381), añade:

```cpp
        // Bloque aditivo: las escenas guardadas antes de este campo no lo traen
        // y cargan igual (version sigue en 1).
        if (j.contains("animator"))
        {
            auto anim = animatorFromJson(j["animator"]);
            // El bloque "mesh" se parsea ANTES que este, así que el SkinnedMesh
            // ya está montado y bindClips puede resolver los nombres de clip
            // aquí mismo. Sin malla skinned (grafo huérfano) los clipIndex se
            // quedan a -1 y currentClipIndex cae a 0.
            if (auto* sm = node->getSkinnedMesh())
                anim->bindClips(*sm, warnings);
            node->setAnimator(std::move(anim));
        }
```

Y actualiza las 4 llamadas a `nodeFromJson`:

- Línea 423 (recursiva, dentro de la propia `nodeFromJson`):
  ```cpp
            nodeFromJson(childJson, child, node->worldTransform, physics, audio, warnings);
  ```
- Línea 460 (en `Scene::cloneGameObject`):
  ```cpp
            nodeFromJson(j, clone, target->worldTransform, physics, audio, &m_warnings);
  ```
- Línea 537 (en `Scene::insertFromJson`):
  ```cpp
            nodeFromJson(j, node, target->worldTransform, physics, audio, &m_warnings);
  ```
- Línea 661 (en `Scene::fromJson`):
  ```cpp
            nodeFromJson(rootJson, &newRoot, glm::mat4(1.0f), physics, audio, &m_warnings);
  ```

Ojo con la línea 460: `Scene::cloneGameObject` limpia `m_warnings` DESPUÉS de llamar a
`nodeFromJson` (línea 468, `m_warnings.clear();`). Mueve ese `clear()` a ANTES de la llamada
a `nodeFromJson`, o los avisos de `bindClips` se perderían.

- [ ] **Step 6: Correr los tests**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_animator_tests.exe
```

Esperado: `dt_animator_tests: OK`, exit code 0.

- [ ] **Step 7: Commit**

```bash
git add engine/src/Core/Scene.cpp engine/tests/animator_tests.cpp
git commit -m "feat(anim): serialize the animator graph into the scene JSON"
```

Cuerpo:

```
Additive block, so scenes saved before it load unchanged and the format
version stays at 1.

States name their clip instead of indexing it: the index depends on the
order of mAnimations in the FBX, so re-exporting a model would silently
repoint states at the wrong animation, whereas a name that no longer
resolves fails loudly through bindClips and Scene::m_warnings. Transition
endpoints stay indices into this same JSON's states array, which keeps
the block self-contained.

Runtime state is deliberately absent. Stopping Play rebuilds the scene
from JSON, so leaving it out is what makes the reset to the entry state
free, and saving mid-Play does not bake transient state into the file.

nodeFromJson takes a warnings vector now: it is a free function and could
not otherwise reach Scene::m_warnings.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
```

---

### Task 7: Conducir el tiempo desde `main.cpp`

**Files:**
- Modify: `sandbox/src/main.cpp:301-305`

**Interfaces:**
- Consumes: `Renderer::setAnimationState` (Task 3), `GameObject::getAnimator` (Task 5),
  `AnimatorComponent::update/currentClipIndex/animTime` (Task 4), `Renderer::isPlaying()` (ya existe).
- Produces: nada consumible por otras tasks. Es el cableado.

- [ ] **Step 1: Ramificar el traverse**

En `sandbox/src/main.cpp`, sustituye el bloque (líneas 301-305):

```cpp
                if (go->skinnedRenderIndex >= 0)
                {
                    renderer.updateAnimation(go->skinnedRenderIndex, dt);
                    renderer.setSkinnedTransform(go->skinnedRenderIndex, go->worldTransform);
                }
```

por:

```cpp
                if (go->skinnedRenderIndex >= 0)
                {
                    if (const auto& anim = go->getAnimator())
                    {
                        // El Animator es el único dueño de animTime: calcula en
                        // CPU y el Renderer solo recibe el resultado. En Edit el
                        // grafo no evalúa transiciones (solo avanza el tiempo del
                        // estado de entrada); si no, las condiciones "animation
                        // finished" pasearían el grafo solo en el editor.
                        anim->update(dt, renderer.isPlaying());
                        renderer.setAnimationState(go->skinnedRenderIndex,
                                                    (uint32_t)anim->currentClipIndex(),
                                                    anim->animTime());
                    }
                    else
                    {
                        // Sin Animator: clip 0 en bucle, exactamente como antes
                        // de que existiera el componente. Los dos caminos no se
                        // pisan.
                        renderer.updateAnimation(go->skinnedRenderIndex, dt);
                    }
                    renderer.setSkinnedTransform(go->skinnedRenderIndex, go->worldTransform);
                }
```

- [ ] **Step 2: Compilar y verificar que no hay regresión**

```powershell
.\build.bat
.\build-ninja\sandbox\Sandbox.exe
```

El personaje del sandbox no tiene `AnimatorComponent`, así que cae por la rama `else` y debe
animarse EXACTAMENTE igual que siempre. Cierra la ventana.

- [ ] **Step 3: Commit**

```bash
git add sandbox/src/main.cpp
git commit -m "feat(anim): drive skinned animation through the Animator when present"
```

Cuerpo:

```
Objects carrying an AnimatorComponent get their clip and time from it and
never reach updateAnimation; everything else keeps looping clip 0 exactly
as before. The two paths do not overlap, so the component stays opt-in.

Edit mode advances the entry state's clock but does not evaluate
transitions: triggers only arrive from Lua, which only runs in Play, so
leaving evaluation on would let "animation finished" walk the graph on
its own inside the editor.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
```

---

### Task 8: `AnimatorComponentCommand` (undo/redo del Add/Remove)

**Files:**
- Modify: `engine/include/DonTopo/Editor/Command.h:173`
- Modify: `engine/src/Editor/Command.cpp:113`
- Modify: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `AnimatorComponent` (Task 4), `GameObject::setAnimator` (Task 5), `Scene::findById` (ya existe).
- Produces: `AnimatorComponentCommand(Scene& scene, std::string label, uint64_t id, bool add, AnimatorComponent state)`.
  Task 11 lo consume.

- [ ] **Step 1: Escribir el test que falla**

En `engine/tests/animator_tests.cpp` añade el include:

```cpp
#include "DonTopo/Editor/Command.h"
```

y esta función antes de `main()`:

```cpp
// Add/Remove del componente pasan por el stack de undo. El comando guarda una
// COPIA del grafo pa que un Add-undo-redo no lo devuelva a un componente vacío.
static void test_animator_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    AnimatorComponent st;
    AnimatorComponent::State s; s.name = "Idle"; s.clipName = "ClipIdle"; s.loop = false;
    st.addState(s);
    st.addParameter("running", AnimatorComponent::ParamType::Bool);

    AnimatorComponentCommand cmd(scene, "Añadir Animator", id, /*add=*/true, st);

    cmd.execute();
    CHECK(go->hasAnimator());
    CHECK(go->getAnimator()->states().size() == 1u);

    cmd.undo();
    CHECK(!go->hasAnimator());

    // Redo: el grafo vuelve entero, no un componente vacío
    cmd.execute();
    CHECK(go->hasAnimator());
    CHECK(go->getAnimator()->states().size() == 1u);
    CHECK(go->getAnimator()->states()[0].name == "Idle");
    CHECK(go->getAnimator()->states()[0].loop == false);
    CHECK(go->getAnimator()->parameters().size() == 1u);
}

// El Remove es el mismo comando con add=false: execute quita, undo devuelve.
static void test_animator_command_remove()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    AnimatorComponent st;
    AnimatorComponent::State s; s.name = "Idle"; s.clipName = "ClipIdle";
    st.addState(s);
    go->setAnimator(std::make_shared<AnimatorComponent>(st));

    AnimatorComponentCommand cmd(scene, "Quitar Animator", id, /*add=*/false, st);
    cmd.execute();
    CHECK(!go->hasAnimator());
    cmd.undo();
    CHECK(go->hasAnimator());
    CHECK(go->getAnimator()->states().size() == 1u);
}

// El objeto puede haber desaparecido entre el execute y el undo: se resuelve por
// id en cada uno, nunca por puntero crudo, así que no debe crashear.
static void test_animator_command_survives_missing_target()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    AnimatorComponentCommand cmd(scene, "Añadir Animator", id, /*add=*/true, AnimatorComponent{});
    scene.removeGameObject(go);
    cmd.execute();   // findById devuelve nullptr y sale sin tocar nada
    cmd.undo();
}
```

Y en `main()`, antes de `am.shutdown();`:

```cpp
    test_animator_command_add_undo_redo();
    test_animator_command_remove();
    test_animator_command_survives_missing_target();
```

- [ ] **Step 2: Correr para verificar que falla**

```powershell
.\build.bat
```

Esperado: FALLO DE COMPILACIÓN — `'AnimatorComponentCommand': undeclared identifier`.

- [ ] **Step 3: Declarar el comando**

En `engine/include/DonTopo/Editor/Command.h`, añade el include arriba junto a los demás:

```cpp
#include "DonTopo/Core/AnimatorComponent.h"
```

Y tras el cierre de `class CameraComponentCommand` (línea 173), antes del `} // namespace DonTopo`, añade:

```cpp
// Add/Remove del AnimatorComponent, mismo contrato que CameraComponentCommand:
// resuelve el GameObject por id en cada execute()/undo() (nunca puntero crudo),
// y m_state conserva el grafo pa que un Add-undo-redo no lo devuelva vacío.
//
// El estado es una COPIA del componente entero, no un POD de campos como
// CameraState: el "estado" de un Animator es el grafo completo, y
// AnimatorComponent es copiable (solo vectores, mapas y PODs). Serializarlo a
// JSON pa esto no compraría nada — las funciones de JSON viven en el anon
// namespace de Scene.cpp y no son accesibles desde aquí.
class AnimatorComponentCommand : public ICommand {
public:
    AnimatorComponentCommand(Scene& scene, std::string label, uint64_t id,
                              bool add, AnimatorComponent state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    AnimatorComponent m_state;
};
```

- [ ] **Step 4: Implementar el comando**

En `engine/src/Editor/Command.cpp`, tras el cierre de `CameraComponentCommand::apply`
(línea 113), antes del `} // namespace DonTopo`, añade:

```cpp
AnimatorComponentCommand::AnimatorComponentCommand(Scene& scene, std::string label, uint64_t id,
                                                    bool add, AnimatorComponent state)
    : m_scene(scene), m_label(std::move(label)), m_id(id), m_add(add), m_state(std::move(state)) {}

void AnimatorComponentCommand::execute() { apply(m_add); }
void AnimatorComponentCommand::undo()    { apply(!m_add); }

void AnimatorComponentCommand::apply(bool add)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    if (!add)
    {
        go->setAnimator(nullptr);
        return;
    }
    go->setAnimator(std::make_shared<AnimatorComponent>(m_state));
}
```

- [ ] **Step 5: Correr los tests**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_animator_tests.exe
```

Esperado: `dt_animator_tests: OK`, exit code 0.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Editor/Command.h engine/src/Editor/Command.cpp engine/tests/animator_tests.cpp
git commit -m "feat(anim): add AnimatorComponentCommand for undoable add/remove"
```

Cuerpo:

```
Same contract as CameraComponentCommand: the target is resolved by id on
every execute and undo rather than held as a raw pointer, and the stored
state survives an add-undo-redo instead of coming back empty.

The state is a copy of the whole component rather than a flat POD, because
an animator's state is its entire graph. AnimatorComponent is copyable, and
the JSON helpers live in Scene.cpp's anonymous namespace and are not
reachable from here anyway.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
```

---

### Task 9: Añadir imgui-node-editor al build

**Files:**
- Modify: `CMakeLists.txt:164`
- Modify: `engine/CMakeLists.txt:60`

**Interfaces:**
- Produces: el target CMake `imgui_node_editor` y el include `<imgui_node_editor.h>`
  (namespace `ax::NodeEditor`). Task 10 lo consume.

- [ ] **Step 1: Declarar la dependencia**

En `CMakeLists.txt` (raíz), tras el bloque de ImGuiColorTextEdit (que termina en la línea 164
con `target_compile_features(imgui_texteditor PUBLIC cxx_std_17)`), añade:

```cmake
# imgui-node-editor — canvas de nodos del panel Animator
# Igual que ImGuiFileDialog/ImGuizmo/ImGuiColorTextEdit: Populate en vez de
# MakeAvailable, su CMakeLists busca imgui por find_package y falla.
FetchContent_Declare(
    ImGuiNodeEditor
    GIT_REPOSITORY https://github.com/thedmd/imgui-node-editor.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(ImGuiNodeEditor)
if(NOT imguinodeeditor_POPULATED)
    FetchContent_Populate(ImGuiNodeEditor)
endif()

add_library(imgui_node_editor STATIC
    ${imguinodeeditor_SOURCE_DIR}/imgui_node_editor.cpp
    ${imguinodeeditor_SOURCE_DIR}/imgui_node_editor_api.cpp
    ${imguinodeeditor_SOURCE_DIR}/imgui_canvas.cpp
    ${imguinodeeditor_SOURCE_DIR}/crude_json.cpp
)
target_include_directories(imgui_node_editor PUBLIC
    ${imguinodeeditor_SOURCE_DIR}
)
target_link_libraries(imgui_node_editor PUBLIC imgui_backend)
target_compile_features(imgui_node_editor PUBLIC cxx_std_17)
```

- [ ] **Step 2: Enlazarlo desde el engine**

En `engine/CMakeLists.txt`, en el bloque `target_link_libraries(DonTopoEngine PUBLIC ...)`,
tras `imgui_texteditor` (línea 60), añade:

```cmake
        imgui_node_editor
```

- [ ] **Step 3: Verificar que descarga y compila**

```powershell
.\configure.bat
.\build.bat
```

Esperado: el configure clona el repo (tarda) y el build compila `imgui_node_editor.lib` sin
errores. Aún no lo usa nadie, así que el resto del build no cambia.

Si el configure falla con `imguinodeeditor_POPULATED` sin definir, comprueba que el nombre en
minúsculas del `FetchContent_GetProperties` coincide: CMake baja el nombre declarado
(`ImGuiNodeEditor`) a `imguinodeeditor`.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt engine/CMakeLists.txt
git commit -m "build: vendor imgui-node-editor for the Animator canvas"
```

Cuerpo:

```
Populate rather than MakeAvailable, matching ImGuiFileDialog, ImGuizmo and
ImGuiColorTextEdit: the upstream CMakeLists looks for imgui via
find_package and fails.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
```

---

### Task 10: `AnimatorPanel` — el canvas de nodos

**Files:**
- Create: `engine/include/DonTopo/Editor/AnimatorPanel.h`
- Create: `engine/src/Editor/AnimatorPanel.cpp`
- Modify: `engine/CMakeLists.txt:10`
- Modify: `engine/include/DonTopo/Editor/EditorContext.h:43`
- Modify: `engine/include/DonTopo/Editor/EditorUI.h:16,159`
- Modify: `engine/src/Editor/EditorUI.cpp:52,66,115`

**Interfaces:**
- Consumes: `AnimatorComponent` (Task 4), `GameObject::getAnimator` (Task 5),
  target CMake `imgui_node_editor` (Task 9), `EditorContext` (ya existe).
- Produces: `class AnimatorPanel { void draw(EditorContext& ctx); bool* GetOpenPtr(); }`
  y el callback `EditorContext::openAnimator` (`std::function<void()>`). Task 11 usa el callback.

- [ ] **Step 1: Añadir el callback al contexto**

En `engine/include/DonTopo/Editor/EditorContext.h`, tras `std::function<void(const std::filesystem::path&)> openScript;`
(línea 43), añade:

```cpp
    // Abre el panel Animator (EditorUI::m_animatorPanel, fuera del alcance de
    // PropertiesPanel — mismo caso y mismo patrón que openScript). Vacío por
    // defecto: solo lo rellena EditorUI::draw().
    std::function<void()> openAnimator;
```

- [ ] **Step 2: Escribir el header del panel**

Crea `engine/include/DonTopo/Editor/AnimatorPanel.h`:

```cpp
#pragma once
#include <cstdint>
#include <string>

namespace ax::NodeEditor { struct EditorContext; }

namespace DonTopo {

struct EditorContext;
class GameObject;

// Ventana "Animator" — canvas de nodos del grafo de estados del GameObject
// seleccionado (nodo = estado con su clip y su loop, link = transición).
//
// Panel propio y no un bloque de Properties porque el canvas de
// imgui-node-editor necesita zoom y pan propios: en la columna de Properties
// sería inservible.
class AnimatorPanel {
public:
    AnimatorPanel();
    ~AnimatorPanel();
    AnimatorPanel(const AnimatorPanel&)            = delete;
    AnimatorPanel& operator=(const AnimatorPanel&) = delete;

    void draw(EditorContext& ctx);
    bool* GetOpenPtr() { return &m_open; }
    void open() { m_open = true; }

private:
    // Vuelca AnimatorComponent::State::editorPos al canvas. Solo al cambiar de
    // objeto: hacerlo cada frame pelearía con el ratón del usuario y los nodos
    // no se podrían arrastrar.
    void syncPositionsFromComponent(GameObject* go);
    // Camino inverso, cada frame: el canvas es la fuente de verdad de las
    // posiciones mientras el panel está abierto, y editorPos es lo que se
    // serializa.
    void syncPositionsToComponent(GameObject* go);
    void drawParameterList(EditorContext& ctx, GameObject* go);
    void drawGraph(EditorContext& ctx, GameObject* go);
    void drawConditionsPopup(EditorContext& ctx, GameObject* go);

    ax::NodeEditor::EditorContext* m_ctx = nullptr;
    bool m_open = false;   // arranca cerrado: es un panel especializado

    // Último GameObject cuyas posiciones se volcaron al canvas. Al cambiar la
    // selección hay que re-volcarlas.
    GameObject* m_boundTo = nullptr;

    // Índice de la transición cuyo popup de condiciones está abierto, -1 si
    // ninguno. Diferido al final del frame: abrir un popup en mitad del canvas
    // rompe el layout del node editor.
    int m_conditionsFor = -1;

    char m_newParamName[64] = {};
    int  m_newParamType     = 0;   // 0 = bool, 1 = trigger
};

} // namespace DonTopo
```

- [ ] **Step 3: Implementar el panel**

Crea `engine/src/Editor/AnimatorPanel.cpp`:

```cpp
#include "DonTopo/Editor/AnimatorPanel.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include <imgui.h>
#include <imgui_node_editor.h>
#include <string>
#include <vector>

namespace ed = ax::NodeEditor;

namespace DonTopo {

namespace {
    // IDs del node editor: tienen que ser != 0 y no colisionar entre nodos,
    // pines y links. Tres slots por estado + un rango aparte pa los links.
    int nodeId(int stateIdx)      { return stateIdx * 3 + 1; }
    int inputPinId(int stateIdx)  { return stateIdx * 3 + 2; }
    int outputPinId(int stateIdx) { return stateIdx * 3 + 3; }
    int linkId(int transIdx)      { return 100000 + transIdx; }

    // Inverso de outputPinId/inputPinId.
    int stateFromPin(int pin) { return (pin - 1) / 3; }
    bool isOutputPin(int pin) { return (pin - 1) % 3 == 2; }

    const char* condLabel(AnimatorComponent::ConditionType t)
    {
        switch (t)
        {
            case AnimatorComponent::ConditionType::Trigger:           return "trigger";
            case AnimatorComponent::ConditionType::AnimationFinished: return "animation finished";
            default:                                                  return "bool";
        }
    }
}

AnimatorPanel::AnimatorPanel()
{
    ed::Config config;
    // Sin fichero de settings: las posiciones de los nodos viven en el JSON de
    // escena (AnimatorComponent::State::editorPos). Si lo dejáramos por defecto,
    // el node editor escribiría un NodeEditor.json paralelo y habría dos fuentes
    // de verdad peleándose.
    config.SettingsFile = nullptr;
    m_ctx = ed::CreateEditor(&config);
}

AnimatorPanel::~AnimatorPanel()
{
    if (m_ctx) ed::DestroyEditor(m_ctx);
}

void AnimatorPanel::syncPositionsFromComponent(GameObject* go)
{
    const auto& states = go->getAnimator()->states();
    for (size_t i = 0; i < states.size(); i++)
        ed::SetNodePosition(nodeId((int)i), ImVec2(states[i].editorPos.x, states[i].editorPos.y));
}

void AnimatorPanel::syncPositionsToComponent(GameObject* go)
{
    auto& states = go->getAnimator()->statesMutable();
    for (size_t i = 0; i < states.size(); i++)
    {
        const ImVec2 p = ed::GetNodePosition(nodeId((int)i));
        states[i].editorPos = glm::vec2(p.x, p.y);
    }
}

void AnimatorPanel::drawParameterList(EditorContext& ctx, GameObject* go)
{
    auto anim = go->getAnimator();

    ImGui::BeginChild("params", ImVec2(200, 0), true);
    ImGui::TextUnformatted("Parameters");
    ImGui::Separator();

    std::string toRemove;
    for (const auto& p : anim->parameters())
    {
        ImGui::PushID(p.name.c_str());
        ImGui::TextUnformatted(p.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", p.type == AnimatorComponent::ParamType::Trigger ? "(trigger)" : "(bool)");
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) toRemove = p.name;
        ImGui::PopID();
    }
    // Diferido: borrar dentro del for-range invalidaría el iterador.
    if (!toRemove.empty())
    {
        anim->removeParameter(toRemove);
        ctx.pushLog("Animator: parámetro '" + toRemove + "' eliminado");
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(110);
    ImGui::InputText("##newparam", m_newParamName, sizeof(m_newParamName));
    const char* types[] = { "bool", "trigger" };
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("##newparamtype", &m_newParamType, types, 2);
    if (ImGui::Button("Add Parameter") && m_newParamName[0] != '\0')
    {
        anim->addParameter(m_newParamName,
            m_newParamType == 1 ? AnimatorComponent::ParamType::Trigger
                                : AnimatorComponent::ParamType::Bool);
        ctx.pushLog(std::string("Animator: parámetro '") + m_newParamName + "' añadido");
        m_newParamName[0] = '\0';
    }

    ImGui::EndChild();
}

void AnimatorPanel::drawGraph(EditorContext& ctx, GameObject* go)
{
    auto anim = go->getAnimator();

    ed::SetCurrentEditor(m_ctx);
    ed::Begin("AnimatorCanvas");

    // --- Nodos ---
    const auto& states = anim->states();
    for (size_t i = 0; i < states.size(); i++)
    {
        ed::BeginNode(nodeId((int)i));

        const bool isEntry = ((int)i == anim->entryState());
        if (isEntry)
        {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", states[i].name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(entry)");
        }
        else
        {
            ImGui::TextUnformatted(states[i].name.c_str());
        }

        // clipIndex < 0: el clip del grafo no existe en el modelo (bindClips ya
        // avisó al cargar). Se marca aquí también o el nodo mentiría.
        if (states[i].clipIndex < 0)
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "clip: %s (no existe)", states[i].clipName.c_str());
        else
            ImGui::TextDisabled("clip: %s", states[i].clipName.c_str());

        ImGui::PushID((int)i);
        bool loop = states[i].loop;
        if (ImGui::Checkbox("loop", &loop))
            anim->statesMutable()[i].loop = loop;
        ImGui::PopID();

        ed::BeginPin(inputPinId((int)i), ed::PinKind::Input);
        ImGui::TextUnformatted("-> in");
        ed::EndPin();
        ImGui::SameLine();
        ed::BeginPin(outputPinId((int)i), ed::PinKind::Output);
        ImGui::TextUnformatted("out ->");
        ed::EndPin();

        ed::EndNode();
    }

    // --- Links ---
    const auto& transitions = anim->transitions();
    for (size_t t = 0; t < transitions.size(); t++)
    {
        if (transitions[t].fromState < 0 || transitions[t].toState < 0) continue;
        ed::Link(linkId((int)t),
                 outputPinId(transitions[t].fromState),
                 inputPinId(transitions[t].toState));
    }

    // --- Crear links arrastrando de pin a pin ---
    if (ed::BeginCreate())
    {
        ed::PinId a, b;
        if (ed::QueryNewLink(&a, &b) && a && b)
        {
            const int pa = (int)a.Get();
            const int pb = (int)b.Get();
            // El usuario puede arrastrar en cualquier dirección: se normaliza a
            // (salida -> entrada).
            const int outPin = isOutputPin(pa) ? pa : pb;
            const int inPin  = isOutputPin(pa) ? pb : pa;

            if (isOutputPin(outPin) && !isOutputPin(inPin) && ed::AcceptNewItem())
            {
                AnimatorComponent::Transition tr;
                tr.fromState = stateFromPin(outPin);
                tr.toState   = stateFromPin(inPin);
                // Sin condiciones no dispara nunca (por diseño): el usuario las
                // añade con doble clic en el link.
                anim->addTransition(tr);
                ctx.pushLog("Animator: transición creada (sin condiciones todavía)");
            }
        }
    }
    ed::EndCreate();

    // --- Borrar nodos y links ---
    if (ed::BeginDelete())
    {
        ed::LinkId dl;
        while (ed::QueryDeletedLink(&dl))
            if (ed::AcceptDeletedItem())
                anim->removeTransition((int)dl.Get() - 100000);

        ed::NodeId dn;
        while (ed::QueryDeletedNode(&dn))
            if (ed::AcceptDeletedItem())
                // removeState reindexa las transiciones supervivientes.
                anim->removeState(((int)dn.Get() - 1) / 3);
    }
    ed::EndDelete();

    // --- Menús contextuales ---
    ed::Suspend();
    ed::NodeId ctxNode;
    ed::LinkId ctxLink;
    if (ed::ShowNodeContextMenu(&ctxNode))
    {
        ImGui::OpenPopup("node_ctx");
        m_conditionsFor = -1;
        ImGui::GetStateStorage()->SetInt(ImGui::GetID("ctx_node"), ((int)ctxNode.Get() - 1) / 3);
    }
    else if (ed::ShowLinkContextMenu(&ctxLink))
    {
        m_conditionsFor = (int)ctxLink.Get() - 100000;
        ImGui::OpenPopup("link_ctx");
    }

    if (ImGui::BeginPopup("node_ctx"))
    {
        const int idx = ImGui::GetStateStorage()->GetInt(ImGui::GetID("ctx_node"), -1);
        if (idx >= 0 && idx < (int)anim->states().size())
        {
            if (ImGui::MenuItem("Set as Entry"))
            {
                anim->setEntryState(idx);
                ctx.pushLog("Animator: '" + anim->states()[idx].name + "' es ahora el estado de entrada");
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("link_ctx"))
    {
        if (ImGui::MenuItem("Edit Conditions...")) ImGui::OpenPopup("conditions");
        ImGui::EndPopup();
    }
    drawConditionsPopup(ctx, go);
    ed::Resume();

    ed::End();
    ed::SetCurrentEditor(nullptr);
}

void AnimatorPanel::drawConditionsPopup(EditorContext& ctx, GameObject* go)
{
    auto anim = go->getAnimator();
    if (m_conditionsFor < 0 || m_conditionsFor >= (int)anim->transitions().size()) return;

    if (!ImGui::BeginPopup("conditions")) return;

    auto& tr = anim->transitionsMutable()[m_conditionsFor];
    ImGui::TextUnformatted("Conditions (AND)");
    ImGui::Separator();

    int toRemove = -1;
    for (size_t c = 0; c < tr.conditions.size(); c++)
    {
        ImGui::PushID((int)c);
        auto& cond = tr.conditions[c];
        ImGui::Text("%s", condLabel(cond.type));
        if (cond.type != AnimatorComponent::ConditionType::AnimationFinished)
        {
            ImGui::SameLine();
            ImGui::TextUnformatted(cond.paramName.c_str());
        }
        if (cond.type == AnimatorComponent::ConditionType::Bool)
        {
            ImGui::SameLine();
            ImGui::Checkbox("expected", &cond.expected);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) toRemove = (int)c;
        ImGui::PopID();
    }
    if (toRemove >= 0) tr.conditions.erase(tr.conditions.begin() + toRemove);

    ImGui::Separator();
    // Solo se ofrecen parámetros ya declarados: una condición sobre un parámetro
    // inexistente no dispararía nunca y no habría forma de saber por qué.
    for (const auto& p : anim->parameters())
    {
        ImGui::PushID(p.name.c_str());
        if (ImGui::MenuItem(("Add: " + p.name).c_str()))
        {
            AnimatorComponent::Condition cond;
            cond.type = (p.type == AnimatorComponent::ParamType::Trigger)
                          ? AnimatorComponent::ConditionType::Trigger
                          : AnimatorComponent::ConditionType::Bool;
            cond.paramName = p.name;
            cond.expected  = true;
            tr.conditions.push_back(cond);
        }
        ImGui::PopID();
    }
    if (ImGui::MenuItem("Add: animation finished"))
    {
        AnimatorComponent::Condition cond;
        cond.type = AnimatorComponent::ConditionType::AnimationFinished;
        tr.conditions.push_back(cond);
    }

    ImGui::EndPopup();
}

void AnimatorPanel::draw(EditorContext& ctx)
{
    if (!m_open) return;
    if (!ImGui::Begin("Animator", &m_open)) { ImGui::End(); return; }

    GameObject* go = ctx.selected;
    if (!go || !go->hasAnimator())
    {
        ImGui::TextDisabled("Selecciona un GameObject con componente Animator.");
        ImGui::TextDisabled("Properties > Add > Animator");
        m_boundTo = nullptr;
        ImGui::End();
        return;
    }

    // --- Añadir estado desde los clips del modelo ---
    SkinnedMesh* mesh = go->getSkinnedMesh();
    if (!mesh || mesh->animationClips.empty())
    {
        ImGui::TextDisabled("El GameObject no tiene un mesh skinned con animaciones.");
    }
    else if (ImGui::BeginCombo("##addstate", "Add State from Clip"))
    {
        for (size_t i = 0; i < mesh->animationClips.size(); i++)
        {
            if (!ImGui::Selectable(mesh->animationClips[i].name.c_str())) continue;
            AnimatorComponent::State st;
            st.name           = mesh->animationClips[i].name;
            st.clipName       = mesh->animationClips[i].name;
            st.clipIndex      = (int)i;
            st.duration       = mesh->animationClips[i].duration;
            st.ticksPerSecond = mesh->animationClips[i].ticksPerSecond;
            st.editorPos      = glm::vec2(40.0f + 40.0f * (float)go->getAnimator()->states().size(),
                                           40.0f + 30.0f * (float)go->getAnimator()->states().size());
            const int idx = go->getAnimator()->addState(st);
            // El nodo es nuevo: hay que colocarlo en el canvas a mano, el sync
            // general solo corre al cambiar de objeto.
            ed::SetCurrentEditor(m_ctx);
            ed::SetNodePosition(nodeId(idx), ImVec2(st.editorPos.x, st.editorPos.y));
            ed::SetCurrentEditor(nullptr);
            ctx.pushLog("Animator: estado '" + st.name + "' añadido");
        }
        ImGui::EndCombo();
    }

    drawParameterList(ctx, go);
    ImGui::SameLine();

    ImGui::BeginChild("canvas", ImVec2(0, 0), false);
    if (m_boundTo != go)
    {
        // Cambio de selección: el canvas todavía tiene las posiciones del objeto
        // anterior. Se vuelca una vez, no cada frame — si no, el usuario no
        // podría arrastrar los nodos.
        ed::SetCurrentEditor(m_ctx);
        syncPositionsFromComponent(go);
        ed::SetCurrentEditor(nullptr);
        m_boundTo = go;
    }
    drawGraph(ctx, go);
    ed::SetCurrentEditor(m_ctx);
    syncPositionsToComponent(go);
    ed::SetCurrentEditor(nullptr);
    ImGui::EndChild();

    ImGui::End();
}

} // namespace DonTopo
```

- [ ] **Step 4: Registrar la fuente**

En `engine/CMakeLists.txt`, tras `src/Editor/ContentBrowserPanel.cpp` (línea 10), añade:

```cmake
    src/Editor/AnimatorPanel.cpp
```

- [ ] **Step 5: Montar el panel en `EditorUI`**

En `engine/include/DonTopo/Editor/EditorUI.h`, añade el include tras el de `ContentBrowserPanel.h` (línea 16):

```cpp
#include "DonTopo/Editor/AnimatorPanel.h"
```

Y como miembro, junto a los demás paneles (cerca de la línea 159, donde está `m_scriptEditor`):

```cpp
    AnimatorPanel m_animatorPanel;
```

En `engine/src/Editor/EditorUI.cpp`:

1. En el `EditorContext ctx{...}` de `draw()`, tras la línea del `openScript` (línea 52), añade:

```cpp
        [this]() { m_animatorPanel.open(); },
```

2. Tras `m_scriptEditor->draw();` (línea 66), añade:

```cpp
    m_animatorPanel.draw(ctx);
```

3. En `drawMenuBar()`, tras el `MenuItem` de "Script Editor" (línea 115), añade:

```cpp
            ImGui::MenuItem("Animator", nullptr, m_animatorPanel.GetOpenPtr());
```

- [ ] **Step 6: Compilar**

```powershell
.\configure.bat
.\build.bat
.\build-ninja\engine\tests\dt_animator_tests.exe
```

Esperado: compila limpio y los tests siguen en `OK`. El panel se puede abrir desde View >
Animator y debe decir "Selecciona un GameObject con componente Animator." (todavía no hay
forma de añadirlo — la trae la Task 11).

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Editor/AnimatorPanel.h engine/src/Editor/AnimatorPanel.cpp engine/include/DonTopo/Editor/EditorContext.h engine/include/DonTopo/Editor/EditorUI.h engine/src/Editor/EditorUI.cpp engine/CMakeLists.txt
git commit -m "feat(anim): add the Animator node graph panel"
```

Cuerpo:

```
Its own window rather than a Properties block: the node canvas needs its
own zoom and pan, which the Properties column cannot give it.

Node positions live in the scene JSON via State::editorPos, so the editor
is configured with SettingsFile = nullptr -- letting it write its own
NodeEditor.json would leave two sources of truth fighting over the layout.
Positions are pushed into the canvas only when the selection changes;
doing it every frame would fight the user's mouse and make nodes
undraggable.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
```

---

### Task 11: Bloque Animator en Properties (Add-gate)

**Files:**
- Modify: `engine/include/DonTopo/Editor/PropertiesPanel.h:46`
- Modify: `engine/src/Editor/PropertiesPanel.cpp:18,332,999,1576`

**Interfaces:**
- Consumes: `AnimatorComponentCommand` (Task 8), `EditorContext::openAnimator` (Task 10),
  `GameObject::hasAnimator/getAnimator` (Task 5).
- Produces: nada consumible. Es UI terminal.

- [ ] **Step 1: Declarar la sección**

En `engine/include/DonTopo/Editor/PropertiesPanel.h`, tras `void drawCameraSection(EditorContext& ctx);`
(línea 46), añade:

```cpp
    void drawAnimatorSection(EditorContext& ctx);
```

- [ ] **Step 2: Implementar la sección**

En `engine/src/Editor/PropertiesPanel.cpp`, añade el include tras el de `CameraComponent.h` (línea 18):

```cpp
#include "DonTopo/Core/AnimatorComponent.h"
```

Tras el cierre de `PropertiesPanel::drawCameraSection` (busca el final del método que empieza
en la línea 999), añade:

```cpp
// El grafo NO se edita aquí: eso es del panel Animator (el canvas necesita su
// propio zoom/pan). Esta sección solo resume y da la puerta de entrada.
void PropertiesPanel::drawAnimatorSection(EditorContext& ctx)
{
    if (!ctx.selected || !ctx.selected->hasAnimator()) return;
    auto anim = ctx.selected->getAnimator();

    if (!ImGui::TreeNodeEx("Animator", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::Text("Estados: %d", (int)anim->states().size());
    ImGui::Text("Transiciones: %d", (int)anim->transitions().size());
    const int entry = anim->entryState();
    if (entry >= 0 && entry < (int)anim->states().size())
        ImGui::Text("Entrada: %s", anim->states()[entry].name.c_str());
    else
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Sin estado de entrada");

    if (!anim->parameters().empty())
    {
        ImGui::Separator();
        ImGui::TextUnformatted("Parameters");
        for (const auto& p : anim->parameters())
            ImGui::BulletText("%s (%s)", p.name.c_str(),
                p.type == AnimatorComponent::ParamType::Trigger ? "trigger" : "bool");
    }

    if (ImGui::Button("Open Animator") && ctx.openAnimator)
        ctx.openAnimator();

    ImGui::SameLine();
    if (ImGui::Button("Remove Animator"))
    {
        // Pasa por el stack igual que el Add (ver AnimatorComponentCommand): el
        // grafo se conserva en el comando pa que el Undo lo devuelva entero.
        const uint64_t id = ctx.selected->id;
        AnimatorComponent st = *anim;
        ctx.pushLog("Componente Animator quitado de '" + ctx.selected->name + "'");
        if (ctx.scene && ctx.undo)
        {
            auto cmd = std::make_unique<AnimatorComponentCommand>(
                *ctx.scene, "Quitar Animator de '" + ctx.selected->name + "'", id, /*add=*/false, st);
            cmd->execute();
            ctx.undo->push(std::move(cmd));
        }
        else
        {
            ctx.selected->setAnimator(nullptr);
        }
        ImGui::TreePop();
        return;
    }

    ImGui::TreePop();
}
```

- [ ] **Step 3: Llamarla desde `draw`**

En `engine/src/Editor/PropertiesPanel.cpp`, tras `drawCameraSection(ctx);` (línea 332), añade:

```cpp
            drawAnimatorSection(ctx);
```

- [ ] **Step 4: Añadir la entrada al popup "Add"**

En `engine/src/Editor/PropertiesPanel.cpp`, en el popup de Add, tras el bloque de la cámara
(que termina con el `SetTooltip` de la línea 1578), añade:

```cpp
        // Animator: solo tiene sentido sobre un mesh skinned (es quien trae los
        // clips). El gate pregunta al GameObject, no a un flag propio.
        const bool canAnimate     = ctx.selected->isSkinned();
        const bool alreadyHasAnim = ctx.selected->hasAnimator();
        ImGui::BeginDisabled(!canAnimate || alreadyHasAnim);
        if (ImGui::Selectable("Animator") && canAnimate && !alreadyHasAnim && ctx.scene && ctx.undo)
        {
            auto cmd = std::make_unique<AnimatorComponentCommand>(
                *ctx.scene, "Añadir Animator a '" + ctx.selected->name + "'",
                ctx.selected->id, /*add=*/true, AnimatorComponent{});
            cmd->execute();
            ctx.undo->push(std::move(cmd));
            ctx.pushLog("Componente Animator añadido a '" + ctx.selected->name + "'");
        }
        ImGui::EndDisabled();
        if (!canAnimate && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("El Animator necesita un mesh skinned (los clips vienen del FBX)");
```

- [ ] **Step 5: Compilar y probar el ciclo entero a mano**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_animator_tests.exe
.\build-ninja\sandbox\Sandbox.exe
```

En la ventana: selecciona el GameObject skinned → Properties → Add → Animator. El bloque
Animator debe aparecer con "Estados: 0". Pulsa "Open Animator": se abre el panel. Cierra.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Editor/PropertiesPanel.h engine/src/Editor/PropertiesPanel.cpp
git commit -m "feat(anim): add the Animator component behind the Add gate"
```

Cuerpo:

```
Hidden until Add, like the colliders. The gate asks the GameObject whether
it is skinned rather than tracking a flag: an animator on a static mesh
would have no clips to name.

The block summarises the graph and opens the panel; it deliberately does
not edit the graph, which is the node canvas's job.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
```

---

### Task 12: Bindings de Lua

**Files:**
- Modify: `engine/src/Scripting/ScriptBindings.cpp:45,354,390`
- Modify: `engine/src/Scripting/LuaApiReference.cpp:76`
- Modify: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `AnimatorComponent` (Task 4), `GameObject::getAnimator` (Task 5).
- Produces: el usertype Lua `Animator` con `SetBool`/`GetBool`/`SetTrigger`/`GetState`,
  accesible por `GetComponent("Animator")`.

- [ ] **Step 1: Declarar el wrapper**

En `engine/src/Scripting/ScriptBindings.cpp`, tras `struct LuaRigidbody { LuaEntity e; };` (línea 45), añade:

```cpp
        struct LuaAnimator { LuaEntity e; };
```

Y añade el include arriba, junto a los demás:

```cpp
#include "DonTopo/Core/AnimatorComponent.h"
```

- [ ] **Step 2: Registrar el usertype**

En `engine/src/Scripting/ScriptBindings.cpp`, tras el cierre del `new_usertype<LuaRigidbody>`
(línea 354), añade:

```cpp
            // Animator: máquina de estados de animación. Se obtiene con
            // GetComponent("Animator"). Sin propiedades: los parámetros se
            // declaran en el grafo y se consultan por nombre, no son campos.
            auto animOf = [](const LuaAnimator& c) -> AnimatorComponent* {
                GameObject* go = deref(c.e);
                if (!go->hasAnimator()) throw std::runtime_error("El GameObject ya no tiene Animator");
                return go->getAnimator().get();
            };
            lua.new_usertype<LuaAnimator>("Animator",
                sol::no_constructor,
                "SetBool",    [animOf](const LuaAnimator& c, const std::string& n, bool v) { animOf(c)->setBool(n, v); },
                "GetBool",    [animOf](const LuaAnimator& c, const std::string& n) { return animOf(c)->getBool(n); },
                "SetTrigger", [animOf](const LuaAnimator& c, const std::string& n) { animOf(c)->setTrigger(n); },
                "GetState",   [animOf](const LuaAnimator& c) { return animOf(c)->currentStateName(); });
```

- [ ] **Step 3: Exponerlo en `GetComponent`**

En `engine/src/Scripting/ScriptBindings.cpp`, en el `GetComponent`, tras la línea del
`Rigidbody` (línea 390), añade:

```cpp
                    if (name == "Animator"        && go->hasAnimator())        return sol::make_object(lua, LuaAnimator{e});
```

- [ ] **Step 4: Añadir el autocompletado**

En `engine/src/Scripting/LuaApiReference.cpp`, tras el bloque de Rigidbody (línea 76), añade:

```cpp
        // Animator (máquina de estados; GetComponent("Animator"))
        "Animator:SetBool", "Animator:GetBool", "Animator:SetTrigger", "Animator:GetState",
```

- [ ] **Step 5: Escribir el test**

En `engine/tests/animator_tests.cpp`, añade antes de `main()`:

```cpp
// La API que consume Lua: los parámetros se consultan por nombre y solo si
// están declarados en el grafo. Un nombre no declarado se ignora en vez de
// crear un parámetro fantasma que ninguna condición miraría.
static void test_parameter_api_ignores_undeclared()
{
    AnimatorComponent a = makeGraph();

    a.setBool("running", true);
    CHECK(a.getBool("running"));

    // No declarado: ni se guarda ni revienta
    a.setBool("noExiste", true);
    CHECK(!a.getBool("noExiste"));

    // Tipo equivocado: "jump" es trigger, no bool
    a.setBool("jump", true);
    CHECK(!a.getBool("jump"));

    CHECK(a.currentStateName() == "Idle");
    a.update(0.016f, true);
    CHECK(a.currentStateName() == "Run");
}
```

Y en `main()`, antes de `am.shutdown();`:

```cpp
    test_parameter_api_ignores_undeclared();
```

- [ ] **Step 6: Compilar y correr**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_animator_tests.exe
```

Esperado: `dt_animator_tests: OK`, exit code 0.

Si MSVC falla con C1128 en `ScriptBindings.cpp`, ya tiene `/bigobj` puesto
(`engine/CMakeLists.txt:81`); si aun así se queja, no toques nada más y avisa.

- [ ] **Step 7: Commit**

```bash
git add engine/src/Scripting/ScriptBindings.cpp engine/src/Scripting/LuaApiReference.cpp engine/tests/animator_tests.cpp
git commit -m "feat(anim): expose the Animator to Lua via GetComponent"
```

Cuerpo:

```
Follows the Rigidbody binding exactly -- a wrapper struct plus a usertype
reached through GetComponent("Animator") -- rather than hanging methods off
Entity, which is not how this engine exposes any component.

Parameters are looked up by name and only if the graph declares them: an
undeclared name is ignored rather than creating a phantom parameter no
condition would ever read.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
```

---

### Task 13: Documentar en el README

**Files:**
- Modify: `README.md`

**Interfaces:**
- Consumes: todo lo anterior. No produce nada.

- [ ] **Step 1: Leer la estructura actual**

Abre `README.md` y localiza la sección donde se documenta el componente Camera (el commit
`fc4774f` la añadió) y la sección de estructura del proyecto (`9b44c2b`).

- [ ] **Step 2: Documentar el componente**

Junto a la documentación del componente Camera, con el mismo formato y nivel de detalle,
añade una sección Animator que cubra:

- Qué es: máquina de estados de animación, nodo = clip, link = transición, sin blending.
- Cómo se añade: Properties > Add > Animator (solo sobre meshes skinned).
- Cómo se edita: View > Animator; "Add State from Clip", arrastrar de pin a pin para crear
  transiciones, menú contextual del nodo para "Set as Entry", menú contextual del link para
  las condiciones, checkbox `loop` en el nodo.
- Las 3 condiciones: `bool`, `trigger`, `animation finished`.
- La API de Lua, con este ejemplo:

```lua
local anim = self.entity:GetComponent("Animator")
anim:SetBool("running", true)
anim:SetTrigger("jump")
if anim:GetState() == "Jump" then
    -- ...
end
```

- Que el grafo evalúa solo en Play; en Edit se previsualiza el estado de entrada.

- [ ] **Step 3: Actualizar la estructura del proyecto**

En el árbol de estructura del README, añade las entradas nuevas:
`engine/src/Core/AnimatorComponent.cpp`, `engine/src/Editor/AnimatorPanel.cpp`,
`engine/src/Renderer/SkinnedMeshPacking.cpp`, `engine/tests/animator_tests.cpp`.

Si el README lista las dependencias de terceros, añade imgui-node-editor.

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "docs(readme): document the Animator component and panel"
```

---

### Task 14: Verificación visual (la hace el usuario)

**Files:** ninguno.

**Interfaces:**
- Consumes: todo. Es el criterio de aceptación 5.

- [ ] **Step 1: Comprobar que todo está verde**

```powershell
.\configure.bat
.\build.bat
.\build-ninja\engine\tests\dt_animator_tests.exe
.\build-ninja\engine\tests\dt_camera_tests.exe
.\build-ninja\engine\tests\dt_physics_tests.exe
.\build-ninja\engine\tests\dt_content_browser_tests.exe
```

Los 4 ejecutables deben salir con exit code 0. Criterio de aceptación 4.

- [ ] **Step 2: Pasarle al usuario el guion exacto**

NO declares la feature terminada. Pídele al usuario que haga esto y espera su respuesta —
ningún subagente tiene GUI, así que esta parte solo la puede verificar él:

> Abre `.\build-ninja\sandbox\Sandbox.exe` y comprueba:
>
> 1. **Add-gate.** Selecciona el GameObject del personaje animado (el que tiene mesh
>    skinned) → panel **Properties** → botón **Add**. Debe aparecer **Animator**, habilitado.
>    Sobre un cubo o cualquier objeto no skinned, ese mismo item debe salir **gris**, con el
>    tooltip "El Animator necesita un mesh skinned".
> 2. **El bloque aparece.** Pulsa **Add > Animator**. En Properties debe salir un bloque
>    **Animator** con "Estados: 0", "Transiciones: 0" y "Sin estado de entrada" en rojo.
> 3. **El panel abre.** Pulsa **Open Animator** (o **View > Animator**). Se abre una ventana
>    "Animator" con la lista de Parameters a la izquierda y un canvas vacío a la derecha.
> 4. **Nodos.** En el desplegable **"Add State from Clip"** deben aparecer los clips del FBX.
>    Añade dos. Deben salir dos nodos en el canvas; el primero con "(entry)" en verde.
>    Arrástralos: deben moverse.
> 5. **Links.** Arrastra del pin **"out ->"** de un nodo al pin **"-> in"** del otro. Debe
>    dibujarse una flecha entre ellos.
> 6. **Parámetros y condiciones.** En el panel izquierdo escribe `jump`, elige **trigger**,
>    pulsa **Add Parameter**. Luego clic derecho sobre la flecha → **Edit Conditions...** →
>    **Add: jump**. La condición debe listarse.
> 7. **Loop.** Marca/desmarca el checkbox **loop** de un nodo.
> 8. **Estado de entrada.** Clic derecho sobre el otro nodo → **Set as Entry**. El "(entry)"
>    verde debe saltar a ese nodo.
> 9. **Criterio 3 — persistencia.** Guarda la escena, ciérrala y vuélvela a cargar. El grafo
>    tiene que volver **igual**: los dos nodos **en la misma posición**, la flecha, la
>    condición del trigger, el parámetro, el loop y el estado de entrada.
> 10. **Sin regresión.** Sobre un personaje **sin** Animator, la animación debe seguir
>     reproduciéndose en bucle como siempre.
>
> Dime qué falla, si algo falla.

- [ ] **Step 3: Solo tras el OK del usuario, invocar `superpowers:finishing-a-development-branch`**

---

## Self-Review

**Cobertura del spec:**

| Requisito / sección del spec | Task |
| --- | --- |
| N clips desde el FBX | 1 |
| Concatenar keyframes, layout `[clip][hueso]` | 2 |
| `clipBase`, ABI de 16 B, 1 línea de shader | 3 |
| Grafo, 3 condiciones, entry state, parámetros | 4 |
| `loop` por nodo | 4 (en `State`), 6 (JSON), 10 (checkbox) |
| `AnimatorComponent` en Core | 4 |
| Slot en `GameObject`, `bindClips` por nombre | 5 |
| Serialización JSON del grafo | 6 |
| Quién avanza el tiempo / Play vs Edit | 4 (`evaluateTransitions`), 7 (cableado) |
| Sin Animator = clip 0 en loop | 7 (rama `else`) |
| Undo/redo | 6 (grafo, vía snapshot JSON), 8 (Add/Remove) |
| imgui-node-editor por FetchContent | 9 |
| Panel de nodos | 10 |
| Add-gate en Properties | 11 |
| Lua | 12 |
| README | 13 |
| Criterio 1 (N clips) | Tests 8 y 9 (Tasks 1 y 2) |
| Criterio 2 (trigger, finished, loop) | Tests 1-5 (Task 4) |
| Criterio 3 (round-trip) | Test 6 (Task 6) + Task 14 paso 9 |
| Criterio 4 (build + tests) | Task 14 paso 1 |
| Criterio 5 (visual) | Task 14 paso 2 |

**Consistencia de tipos:** `packSkinnedClips` devuelve `PackedClips` con campos
`pos`/`rot`/`scale`/`boneInfos` — usados con esos nombres en Task 2 Step 6.
`AnimatorComponent::update(float, bool)`, `currentClipIndex()`, `animTime()`,
`currentStateName()`, `statesMutable()`, `transitionsMutable()`, `removeState(int)`,
`removeTransition(int)`, `removeParameter(const std::string&)`, `bindClips(const SkinnedMesh&, std::vector<std::string>*)`
declarados en Task 4 Step 3 y consumidos con esas mismas firmas en Tasks 5, 6, 7, 8, 10, 11 y 12.
`Renderer::setAnimationState(int, uint32_t, float)` declarado en Task 3 y llamado así en Task 7.
`AnimatorComponentCommand(Scene&, std::string, uint64_t, bool, AnimatorComponent)` declarado en
Task 8 y construido así en Tasks 8 y 11.

**Riesgos abiertos:**

- El API de imgui-node-editor se usa a ciegas (`ed::QueryDeletedNode`, `ed::ShowNodeContextMenu`,
  `ed::PinKind`, `config.SettingsFile`). Si alguna firma no casa con el `master` que baje
  FetchContent, ajústala contra la cabecera real en
  `build-ninja/_deps/imguinodeeditor-src/imgui_node_editor.h` — NO cambies el diseño por ello.
- `ImGui::GetStateStorage()` para pasar el índice del nodo al popup contextual es apaño; si el
  popup sale con el nodo equivocado, guárdalo en un miembro `int m_ctxNode` del panel.
