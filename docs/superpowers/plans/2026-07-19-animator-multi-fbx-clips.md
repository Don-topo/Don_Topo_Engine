# Animator: clips desde múltiples ficheros FBX — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Poder añadir clips de animación al Animator desde ficheros FBX distintos del que aportó la malla, sin cambiar el comportamiento actual del Animator.

**Architecture:** `SkinnedMesh` gana una lista `animationSources` que registra qué clip vino de qué fichero; `animationClips` sigue siendo la única lista plana que consumen el packing GPU, el compute shader y `bindClips`. Un loader nuevo (`ModelLoader::loadAnimationClips`) importa solo animaciones de un FBX y las mapea al esqueleto existente por nombre de hueso. Una capa de merge sin Vulkan (`SkinnedMeshAnimations`) añade, quita y renombra clips; el Renderer rehace los SSBOs del objeto skinned en su mismo slot; el Animator Panel expone todo con undo/redo.

**Tech Stack:** C++20, Assimp, Vulkan, ImGui + imgui-node-editor + ImGuiFileDialog, nlohmann/json, CMake + Ninja + MSVC.

## Global Constraints

- Spec de referencia: `docs/superpowers/specs/2026-07-19-animator-multi-fbx-clips-design.md`.
- **No cambia**: `shaders/bone_eval.comp`, `packSkinnedClips`, el layout `[clip][hueso]` de los SSBOs, `AnimatorComponent::bindClips`, la máquina de estados, los parámetros bool/int/float, las transiciones ni los bindings de Lua.
- Los tests son *plain main + asserts*, sin framework, con la macro `CHECK` ya existente en `engine/tests/animator_tests.cpp`. Toda función de test nueva se registra en `main()`.
- Una sola `PhysicsManager`/`AudioManager` por proceso: los tests que necesitan `Scene::fromJson` reciben `pm`/`am` por parámetro desde `main()`. Nunca crear una por test.
- Build: `.\build.bat` desde la raíz del repo, en PowerShell. Ejecutar tests: `.\build-ninja\engine\tests\dt_animator_tests.exe` desde la raíz (las rutas de asset son relativas a ella).
- Comentarios en castellano, explicando el *por qué*, como el resto del repo.
- Commits: Conventional Commits, en inglés, terminados en `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.
- Desviación consciente respecto al spec: `renameClip` se define **por nombre** (`oldName`/`newName`) en vez de por índice de clip. Los estados del Animator referencian por nombre, y los comandos de undo necesitan una clave estable frente a reordenaciones de `animationClips`; un índice no lo es.

## Estructura de ficheros

| Fichero | Responsabilidad | Tarea |
|---|---|---|
| `engine/include/DonTopo/Renderer/SkinnedMesh.h` | `struct AnimationSource` + campo `animationSources` | 1 |
| `engine/include/DonTopo/Renderer/SkinnedMeshAnimations.h` (nuevo) | API de merge sin Vulkan: nombres únicos, add/remove/rename | 2, 4 |
| `engine/src/Renderer/SkinnedMeshAnimations.cpp` (nuevo) | Implementación de lo anterior | 2, 4 |
| `engine/include/DonTopo/Renderer/ModelLoader.h` | `struct LoadedClips` + `loadAnimationClips` | 3 |
| `engine/src/Renderer/ModelLoader.cpp` | `clipFromAssimp` compartido; `loadSkinned` registra la fuente builtin | 1, 3 |
| `engine/include/DonTopo/Core/AnimatorComponent.h` + `.cpp` | `renameClipReferences` | 5 |
| `engine/src/Core/Scene.cpp` | Serialización de `animationSources` | 6 |
| `engine/include/DonTopo/Editor/Command.h` + `engine/src/Editor/Command.cpp` | `AnimationSourceCommand`, `ClipRenameCommand` | 7 |
| `engine/include/DonTopo/Renderer/Renderer.h` + `engine/src/Renderer/Renderer.cpp` | `initSkinnedRenderObject`, `rebuildSkinnedMesh`, pools liberables | 8 |
| `engine/include/DonTopo/Editor/AnimatorPanel.h` + `engine/src/Editor/AnimatorPanel.cpp` | Sección *Animation Sources* | 9 |
| `engine/CMakeLists.txt` | Alta de `SkinnedMeshAnimations.cpp` | 2 |
| `engine/tests/animator_tests.cpp` | Tests de todo lo anterior | 1-7 |

---

### Task 1: `AnimationSource` en `SkinnedMesh` y fuente builtin en `loadSkinned`

**Files:**
- Modify: `engine/include/DonTopo/Renderer/SkinnedMesh.h:54-64`
- Modify: `engine/src/Renderer/ModelLoader.cpp:274-330`
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: nada.
- Produces: `struct DonTopo::AnimationSource { std::string path; bool builtin = false; std::vector<std::string> clipNames; };` y el campo `std::vector<AnimationSource> animationSources;` dentro de `SkinnedMesh`. Tras `ModelLoader::loadSkinned(path)`, `animationSources` tiene exactamente 1 entrada con `builtin == true`, `path == path` y `clipNames` igual a los nombres de `animationClips` en el mismo orden.

- [ ] **Step 1: Escribir el test que falla**

En `engine/tests/animator_tests.cpp`, justo después de `test_loader_reads_all_clips()`:

```cpp
// El FBX del propio modelo se registra como fuente "builtin": es la entrada de
// la lista que la UI muestra sin botón de borrar, y la que la escena reconstruye
// vía mesh.sourcePath en vez de vía addAnimationSource.
static void test_loader_registers_builtin_source()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");

    CHECK(m.animationSources.size() == 1u);
    if (m.animationSources.empty()) return;

    const AnimationSource& src = m.animationSources[0];
    CHECK(src.builtin == true);
    CHECK(src.path == "assets/modelAnimation.fbx");
    // clipNames refleja exactamente los clips cargados, en el mismo orden
    CHECK(src.clipNames.size() == m.animationClips.size());
    for (size_t i = 0; i < src.clipNames.size() && i < m.animationClips.size(); i++)
        CHECK(src.clipNames[i] == m.animationClips[i].name);
}
```

Y registrarlo en `main()`, justo debajo de `test_loader_reads_all_clips();`:

```cpp
    test_loader_registers_builtin_source();
```

- [ ] **Step 2: Ejecutar y comprobar que falla**

Run: `.\build.bat`
Expected: FAIL al compilar, `error C2039: 'animationSources': is not a member of 'DonTopo::SkinnedMesh'`.

- [ ] **Step 3: Añadir la estructura**

En `engine/include/DonTopo/Renderer/SkinnedMesh.h`, justo antes de `struct SkinnedMesh`:

```cpp
    // Fichero del que salieron uno o más clips. La lista existe para poder
    // mostrar los clips agrupados por origen en el Animator Panel y para poder
    // quitar un fichero entero; la evaluación en GPU no la mira nunca, sigue
    // consumiendo animationClips plano.
    //
    // builtin marca el FBX que aportó la malla y el esqueleto: no se puede
    // quitar (quitarlo sería quitar el modelo) y la escena lo reconstruye vía
    // Mesh::sourcePath, no vía addAnimationSource.
    struct AnimationSource
    {
        std::string              path;
        bool                     builtin = false;
        std::vector<std::string> clipNames; // nombres finales, en el orden en que se añadieron
    };
```

Y dentro de `struct SkinnedMesh`, tras `animationClips`:

```cpp
        // Origen de cada clip. Invariante: la concatenación de los clipNames de
        // todas las fuentes es una permutación de los nombres de animationClips.
        std::vector<AnimationSource> animationSources;
```

- [ ] **Step 4: Rellenar la fuente builtin en `loadSkinned`**

En `engine/src/Renderer/ModelLoader.cpp`, sustituir la línea 329 (`smesh.animationClips.push_back(std::move(clip));`) y lo que la sigue hasta el cierre del bucle de animaciones por:

```cpp
            smesh.animationClips.push_back(std::move(clip));
        }

        // Fuente builtin: el propio FBX. Se registra siempre, incluso sin
        // animaciones — la UI necesita una fila que represente al modelo.
        {
            AnimationSource builtin;
            builtin.path    = path;
            builtin.builtin = true;
            for (const auto& c : smesh.animationClips)
                builtin.clipNames.push_back(c.name);
            smesh.animationSources.push_back(std::move(builtin));
        }
```

(El `}` que cierra el bucle `for (uint32_t a = 0; ...)` pasa a estar dentro del bloque insertado; comprobar que no queda un `}` duplicado tras el bloque de materiales.)

- [ ] **Step 5: Ejecutar y comprobar que pasa**

Run: `.\build.bat`; luego `.\build-ninja\engine\tests\dt_animator_tests.exe`
Expected: `dt_animator_tests: OK`

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Renderer/SkinnedMesh.h engine/src/Renderer/ModelLoader.cpp engine/tests/animator_tests.cpp
git commit -m "feat(anim): record the source file of every animation clip

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: `uniqueClipName` en `SkinnedMeshAnimations`

**Files:**
- Create: `engine/include/DonTopo/Renderer/SkinnedMeshAnimations.h`
- Create: `engine/src/Renderer/SkinnedMeshAnimations.cpp`
- Modify: `engine/CMakeLists.txt:16`
- Modify: `engine/src/Renderer/ModelLoader.cpp:280-293`
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `AnimationSource` (Task 1).
- Produces: `std::string DonTopo::uniqueClipName(const std::vector<AnimationClip>& existing, const std::string& base);` — devuelve `base` si no está tomado, y si no `base + " (N)"` con el primer `N >= 1` libre. `base` vacío se trata como `"Animation"`.

- [ ] **Step 1: Escribir el test que falla**

En `engine/tests/animator_tests.cpp`, tras `test_loader_registers_builtin_source()`:

```cpp
// La regla de nombres únicos es la misma que ya aplicaba el loader (Mixamo
// exporta todo como "mixamo.com"), pero ahora vive en un sitio compartido: la
// usan tanto loadSkinned como la importación de ficheros extra.
static void test_unique_clip_name()
{
    std::vector<AnimationClip> existing;
    AnimationClip a; a.name = "walk";       existing.push_back(a);
    AnimationClip b; b.name = "walk (1)";   existing.push_back(b);

    CHECK(uniqueClipName(existing, "run")  == "run");
    CHECK(uniqueClipName(existing, "walk") == "walk (2)");
    // Nombre vacío: no puede quedarse vacío, el Animator resuelve por nombre
    CHECK(uniqueClipName(existing, "")     == "Animation");
}
```

Añadir el include en la cabecera del fichero de tests, tras `#include "DonTopo/Renderer/SkinnedMeshPacking.h"`:

```cpp
#include "DonTopo/Renderer/SkinnedMeshAnimations.h"
```

Y registrar en `main()` tras `test_loader_registers_builtin_source();`:

```cpp
    test_unique_clip_name();
```

- [ ] **Step 2: Ejecutar y comprobar que falla**

Run: `.\build.bat`
Expected: FAIL, `Cannot open include file: 'DonTopo/Renderer/SkinnedMeshAnimations.h'`.

- [ ] **Step 3: Crear la cabecera**

`engine/include/DonTopo/Renderer/SkinnedMeshAnimations.h`:

```cpp
#pragma once
#include <string>
#include <vector>
#include "DonTopo/Renderer/SkinnedMesh.h"

namespace DonTopo
{
    // Capa de merge de clips: sin Vulkan a propósito, igual que
    // SkinnedMeshPacking. Dentro del Renderer solo se podría probar con un
    // VkDevice vivo, es decir, no se podría probar.

    // Devuelve base si ningún clip de existing lo usa; si no, base + " (N)" con
    // el primer N libre. base vacío -> "Animation": el Animator resuelve los
    // clips por nombre, así que un nombre vacío o repetido deja clips
    // inalcanzables.
    std::string uniqueClipName(const std::vector<AnimationClip>& existing,
                               const std::string& base);
}
```

- [ ] **Step 4: Implementar**

`engine/src/Renderer/SkinnedMeshAnimations.cpp`:

```cpp
#include "DonTopo/Renderer/SkinnedMeshAnimations.h"

namespace DonTopo
{
    std::string uniqueClipName(const std::vector<AnimationClip>& existing,
                               const std::string& base)
    {
        const std::string root = base.empty() ? std::string("Animation") : base;

        auto taken = [&](const std::string& n) {
            for (const auto& c : existing)
                if (c.name == n) return true;
            return false;
        };

        if (!taken(root)) return root;

        int suffix = 1;
        std::string candidate = root + " (" + std::to_string(suffix) + ")";
        while (taken(candidate))
            candidate = root + " (" + std::to_string(++suffix) + ")";
        return candidate;
    }
}
```

En `engine/CMakeLists.txt`, tras la línea `src/Renderer/SkinnedMeshPacking.cpp`:

```cmake
    src/Renderer/SkinnedMeshAnimations.cpp
```

- [ ] **Step 5: Ejecutar y comprobar que pasa**

Run: `.\build.bat`; luego `.\build-ninja\engine\tests\dt_animator_tests.exe`
Expected: `dt_animator_tests: OK`

- [ ] **Step 6: Hacer que `loadSkinned` use el helper**

En `engine/src/Renderer/ModelLoader.cpp`, sustituir el bloque de nombres únicos (líneas ~280-293, desde el comentario `// Nombres únicos y no vacíos:` hasta `while (taken(unique)) ...`) por:

```cpp
            // Nombres únicos y no vacíos: Mixamo exporta cada take como
            // "mixamo.com", y los FBX de Blender a veces sin nombre. La regla
            // vive en uniqueClipName porque la importación de ficheros extra
            // tiene que aplicar exactamente la misma.
            const std::string unique =
                uniqueClipName(smesh.animationClips, anim->mName.C_Str());
```

Y añadir el include al principio de `ModelLoader.cpp`, tras `#include "DonTopo/Renderer/ModelLoader.h"`:

```cpp
#include "DonTopo/Renderer/SkinnedMeshAnimations.h"
```

Nota: la variable local `base` y el lambda `taken` desaparecen; `clip.name = unique;` se mantiene.

- [ ] **Step 7: Ejecutar y comprobar que sigue pasando**

Run: `.\build.bat`; luego `.\build-ninja\engine\tests\dt_animator_tests.exe`
Expected: `dt_animator_tests: OK` — `test_loader_reads_all_clips` sigue verificando unicidad, así que cubre la refactorización.

- [ ] **Step 8: Commit**

```bash
git add engine/include/DonTopo/Renderer/SkinnedMeshAnimations.h engine/src/Renderer/SkinnedMeshAnimations.cpp engine/CMakeLists.txt engine/src/Renderer/ModelLoader.cpp engine/tests/animator_tests.cpp
git commit -m "refactor(anim): share the unique clip-name rule

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: `ModelLoader::loadAnimationClips`

**Files:**
- Modify: `engine/include/DonTopo/Renderer/ModelLoader.h`
- Modify: `engine/src/Renderer/ModelLoader.cpp:274-330`
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `uniqueClipName` (Task 2), `AnimationSource` (Task 1).
- Produces:
```cpp
struct DonTopo::LoadedClips {
    std::vector<AnimationClip> clips;
    std::vector<std::string>   warnings;
    int mappedChannels = 0;   // canales cuyo hueso existe en el esqueleto
    int totalChannels  = 0;   // canales del fichero
};
DonTopo::LoadedClips DonTopo::ModelLoader::loadAnimationClips(const std::string& path, const Skeleton& skel);
```
Los nombres de los clips devueltos son los internos del FBX, sin deduplicar: quien llama conoce los clips ya presentes y aplica `uniqueClipName`. Un fichero ilegible **no lanza**: devuelve `clips` vacío y un warning con el mensaje de Assimp.

- [ ] **Step 1: Escribir los tests que fallan**

En `engine/tests/animator_tests.cpp`, tras `test_unique_clip_name()`:

```cpp
// Importar animaciones contra el esqueleto del propio fichero tiene que dar
// exactamente lo mismo que loadSkinned: mismos clips, mismos boneIndex, sin
// avisos. Es el caso "el FBX de animación es del mismo rig que el modelo".
static void test_load_animation_clips_matches_full_load()
{
    SkinnedMesh base = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    LoadedClips lc = ModelLoader::loadAnimationClips("assets/modelAnimation.fbx", base.skeleton);

    CHECK(lc.warnings.empty());
    CHECK(lc.clips.size() == base.animationClips.size());
    CHECK(lc.mappedChannels == lc.totalChannels);
    CHECK(lc.totalChannels > 0);

    for (size_t i = 0; i < lc.clips.size() && i < base.animationClips.size(); i++)
    {
        const AnimationClip& a = lc.clips[i];
        const AnimationClip& b = base.animationClips[i];
        CHECK(nearlyEqual(a.duration, b.duration));
        CHECK(nearlyEqual(a.ticksPerSecond, b.ticksPerSecond));
        CHECK(a.channels.size() == b.channels.size());
        for (size_t c = 0; c < a.channels.size() && c < b.channels.size(); c++)
        {
            CHECK(a.channels[c].boneIndex == b.channels[c].boneIndex);
            CHECK(a.channels[c].posKeys.size()   == b.channels[c].posKeys.size());
            CHECK(a.channels[c].rotKeys.size()   == b.channels[c].rotKeys.size());
            CHECK(a.channels[c].scaleKeys.size() == b.channels[c].scaleKeys.size());
        }
    }
}

// Esqueleto ajeno: ningún hueso casa, así que no hay nada que importar. Se avisa
// y se devuelven 0 clips — el caller lo convierte en rechazo.
static void test_load_animation_clips_against_foreign_skeleton()
{
    Skeleton foreign;
    foreign.names           = { "hueso_que_no_existe" };
    foreign.parentIndex     = { -1 };
    foreign.inverseBindPose = { glm::mat4(1.0f) };
    foreign.boneMap         = { { "hueso_que_no_existe", 0 } };

    LoadedClips lc = ModelLoader::loadAnimationClips("assets/modelAnimation.fbx", foreign);

    CHECK(lc.clips.empty());
    CHECK(!lc.warnings.empty());
    CHECK(lc.mappedChannels == 0);
    CHECK(lc.totalChannels > 0);
}

// Esqueleto parcial: se importa lo que casa y se avisa de lo que no. Es el caso
// real de un rig al que le faltan huesos de dedos respecto al FBX de animación.
static void test_load_animation_clips_partial_skeleton()
{
    SkinnedMesh base = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    CHECK(base.skeleton.names.size() >= 2u);
    if (base.skeleton.names.size() < 2u) return;

    // Esqueleto recortado: solo el primer hueso del original
    Skeleton partial;
    partial.names           = { base.skeleton.names[0] };
    partial.parentIndex     = { -1 };
    partial.inverseBindPose = { base.skeleton.inverseBindPose[0] };
    partial.boneMap         = { { base.skeleton.names[0], 0 } };

    LoadedClips lc = ModelLoader::loadAnimationClips("assets/modelAnimation.fbx", partial);

    CHECK(!lc.warnings.empty());                 // avisa de los huesos ignorados
    CHECK(lc.mappedChannels < lc.totalChannels);
    // Los canales que sobreviven apuntan al único hueso del esqueleto recortado
    for (const auto& c : lc.clips)
        for (const auto& ch : c.channels)
            CHECK(ch.boneIndex == 0);
}

// Fichero inexistente: no lanza, avisa. La UI lo convierte en un mensaje rojo,
// y Scene::fromJson en un log — ninguno de los dos quiere una excepción.
static void test_load_animation_clips_missing_file()
{
    Skeleton skel;
    skel.names = { "root" }; skel.parentIndex = { -1 };
    skel.inverseBindPose = { glm::mat4(1.0f) };
    skel.boneMap = { { "root", 0 } };

    LoadedClips lc = ModelLoader::loadAnimationClips("assets/no_existe_este_fichero.fbx", skel);

    CHECK(lc.clips.empty());
    CHECK(!lc.warnings.empty());
}
```

Registrar en `main()` tras `test_unique_clip_name();`:

```cpp
    test_load_animation_clips_matches_full_load();
    test_load_animation_clips_against_foreign_skeleton();
    test_load_animation_clips_partial_skeleton();
    test_load_animation_clips_missing_file();
```

- [ ] **Step 2: Ejecutar y comprobar que falla**

Run: `.\build.bat`
Expected: FAIL, `error C2065: 'LoadedClips': undeclared identifier`.

- [ ] **Step 3: Declarar la API**

En `engine/include/DonTopo/Renderer/ModelLoader.h`, dentro de `namespace DonTopo` y antes de `class ModelLoader`:

```cpp
    // Resultado de importar SOLO las animaciones de un fichero. warnings lleva
    // los mensajes ya formateados para el Log Console; mapped/totalChannels
    // dejan al caller decidir si eso es un fichero válido o un rig equivocado.
    struct LoadedClips
    {
        std::vector<AnimationClip> clips;
        std::vector<std::string>   warnings;
        int mappedChannels = 0;
        int totalChannels  = 0;
    };
```

Y dentro de `class ModelLoader`, tras `loadSkinned`:

```cpp
            // Importa las animaciones de path mapeando cada canal al esqueleto
            // skel POR NOMBRE de hueso. No construye geometría ni materiales:
            // un FBX de Mixamo trae la malla entera y aquí sobra.
            //
            // No lanza: un fichero ilegible devuelve clips vacío y un warning.
            static LoadedClips loadAnimationClips(const std::string& path, const Skeleton& skel);
```

- [ ] **Step 4: Extraer el helper de keyframes**

En `engine/src/Renderer/ModelLoader.cpp`, tras `aiToGlm` (línea 22), añadir:

```cpp
    // Convierte una aiAnimation a AnimationClip resolviendo cada canal contra
    // skel POR NOMBRE. Compartido por loadSkinned y loadAnimationClips: son el
    // mismo trabajo, y duplicarlo garantizaba que las dos rutas divergieran.
    //
    // clip.name queda con el nombre CRUDO del FBX: la unicidad la aplica quien
    // llama, que es quien sabe qué clips hay ya en el mesh destino.
    static AnimationClip clipFromAssimp(const aiAnimation* anim, const Skeleton& skel,
                                        int& mappedChannels, int& totalChannels,
                                        std::vector<std::string>* unknownBones)
    {
        AnimationClip clip;
        clip.name           = anim->mName.C_Str();
        clip.duration       = (float)anim->mDuration;
        clip.ticksPerSecond = (anim->mTicksPerSecond > 0.0) ? (float)anim->mTicksPerSecond : 24.0f;

        for (uint32_t c = 0; c < anim->mNumChannels; c++)
        {
            aiNodeAnim* ch = anim->mChannels[c];
            std::string boneName = ch->mNodeName.C_Str();
            totalChannels++;

            auto it = skel.boneMap.find(boneName);
            if (it == skel.boneMap.end())
            {
                if (unknownBones) unknownBones->push_back(boneName);
                continue;
            }
            mappedChannels++;

            BoneChannel bc;
            bc.boneIndex = it->second;

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
        return clip;
    }
```

- [ ] **Step 5: Reescribir el bucle de animaciones de `loadSkinned` con el helper**

Sustituir el bloque `// --- Animaciones: todas las del fichero ---` completo (desde `for (uint32_t a = 0; a < scene->mNumAnimations; a++)` hasta el cierre del bloque de la fuente builtin añadido en Task 1) por:

```cpp
        // --- Animaciones: todas las del fichero ---
        for (uint32_t a = 0; a < scene->mNumAnimations; a++)
        {
            int mapped = 0, total = 0;
            AnimationClip clip = clipFromAssimp(scene->mAnimations[a], skel, mapped, total, nullptr);
            // Nombres únicos y no vacíos: Mixamo exporta cada take como
            // "mixamo.com", y los FBX de Blender a veces sin nombre. El
            // Animator resuelve los clips por nombre, así que dos clips
            // homónimos harían que el segundo fuera inalcanzable.
            clip.name = uniqueClipName(smesh.animationClips, clip.name);
            smesh.animationClips.push_back(std::move(clip));
        }

        // Fuente builtin: el propio FBX. Se registra siempre, incluso sin
        // animaciones — la UI necesita una fila que represente al modelo.
        {
            AnimationSource builtin;
            builtin.path    = path;
            builtin.builtin = true;
            for (const auto& c : smesh.animationClips)
                builtin.clipNames.push_back(c.name);
            smesh.animationSources.push_back(std::move(builtin));
        }
```

- [ ] **Step 6: Implementar `loadAnimationClips`**

Al final de `engine/src/Renderer/ModelLoader.cpp`, dentro de `namespace DonTopo`:

```cpp
    LoadedClips ModelLoader::loadAnimationClips(const std::string& path, const Skeleton& skel)
    {
        LoadedClips out;

        Assimp::Importer importer;
        // Flags mínimos: aquí no se construye geometría, así que triangulate,
        // normales y tangentes serían trabajo tirado. Assimp lee las
        // animaciones igual.
        const aiScene* scene = importer.ReadFile(path, 0);

        const std::string file = std::filesystem::path(path).filename().string();

        if (!scene || !scene->mRootNode)
        {
            out.warnings.push_back(file + ": " + std::string(importer.GetErrorString()));
            return out;
        }
        if (scene->mNumAnimations == 0)
        {
            out.warnings.push_back(file + ": no contiene animaciones");
            return out;
        }

        std::vector<std::string> unknownBones;
        for (uint32_t a = 0; a < scene->mNumAnimations; a++)
        {
            AnimationClip clip = clipFromAssimp(scene->mAnimations[a], skel,
                                                out.mappedChannels, out.totalChannels,
                                                &unknownBones);
            // Un clip sin un solo canal válido no aporta nada: se descarta
            // individualmente en vez de tumbar el fichero entero.
            if (clip.channels.empty()) continue;
            out.clips.push_back(std::move(clip));
        }

        if (out.mappedChannels == 0)
        {
            out.warnings.push_back(file + ": ningún hueso coincide con el esqueleto (0/"
                                    + std::to_string(out.totalChannels) + " canales)");
            out.clips.clear();
            return out;
        }

        if (!unknownBones.empty())
        {
            std::string msg = file + ": " + std::to_string(out.mappedChannels) + "/"
                            + std::to_string(out.totalChannels) + " canales mapeados, "
                            + std::to_string(unknownBones.size()) + " huesos desconocidos ignorados (";
            // Solo los 5 primeros: la lista completa de un rig ajeno llenaría
            // el Log Console sin decir nada más de lo que dicen 5 ejemplos.
            const size_t shown = unknownBones.size() < 5 ? unknownBones.size() : 5;
            for (size_t i = 0; i < shown; i++)
                msg += (i ? ", " : "") + unknownBones[i];
            if (unknownBones.size() > shown) msg += ", ...";
            msg += ")";
            out.warnings.push_back(std::move(msg));
        }

        return out;
    }
```

- [ ] **Step 7: Ejecutar y comprobar que pasa**

Run: `.\build.bat`; luego `.\build-ninja\engine\tests\dt_animator_tests.exe`
Expected: `dt_animator_tests: OK`

- [ ] **Step 8: Commit**

```bash
git add engine/include/DonTopo/Renderer/ModelLoader.h engine/src/Renderer/ModelLoader.cpp engine/tests/animator_tests.cpp
git commit -m "feat(anim): load animation clips from a standalone FBX

Maps every channel onto an existing skeleton by bone name and reports how
many matched, so a wrong rig is diagnosable instead of silently broken.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: add / remove / rename en `SkinnedMeshAnimations`

**Files:**
- Modify: `engine/include/DonTopo/Renderer/SkinnedMeshAnimations.h`
- Modify: `engine/src/Renderer/SkinnedMeshAnimations.cpp`
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `LoadedClips` + `ModelLoader::loadAnimationClips` (Task 3), `uniqueClipName` (Task 2).
- Produces:
```cpp
bool DonTopo::addAnimationSource(SkinnedMesh& mesh, const std::string& path,
                                 std::vector<std::string>& warnings,
                                 const std::vector<std::string>* forcedNames = nullptr);
bool DonTopo::removeAnimationSource(SkinnedMesh& mesh, size_t sourceIndex);
bool DonTopo::renameClip(SkinnedMesh& mesh, const std::string& oldName,
                         const std::string& newName);
```
`forcedNames` se aplica en orden a los clips importados hasta `min(forcedNames->size(), clips.size())`; el resto se nombra con `uniqueClipName`. Lo usan la carga de escena y el undo de un remove, para que un rename sobreviva.

- [ ] **Step 1: Escribir los tests que fallan**

En `engine/tests/animator_tests.cpp`, tras `test_load_animation_clips_missing_file()`:

```cpp
// El caso central de la feature: los clips de un segundo fichero se suman a los
// del modelo. Se importa el MISMO fbx dos veces a propósito — ejercita la
// deduplicación de nombres sin necesitar un asset extra en el repo.
static void test_add_animation_source_appends_clips()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    const size_t before = m.animationClips.size();

    std::vector<std::string> warnings;
    CHECK(addAnimationSource(m, "assets/modelAnimation.fbx", warnings));

    CHECK(m.animationClips.size() == before * 2);
    CHECK(m.animationSources.size() == 2u);
    CHECK(m.animationSources[1].builtin == false);
    CHECK(m.animationSources[1].path == "assets/modelAnimation.fbx");
    CHECK(m.animationSources[1].clipNames.size() == before);

    // Nombres únicos entre TODOS los clips: el Animator resuelve por nombre
    for (size_t i = 0; i < m.animationClips.size(); i++)
        for (size_t j = i + 1; j < m.animationClips.size(); j++)
            CHECK(m.animationClips[i].name != m.animationClips[j].name);

    // Los clips nuevos se nombran por el fichero, no por el nombre interno
    CHECK(m.animationSources[1].clipNames[0].rfind("modelAnimation", 0) == 0);
}

// Rechazo: un rig que no casa deja el mesh EXACTAMENTE como estaba. Nada de
// clips a medias ni de una fuente registrada que no aportó nada.
static void test_add_animation_source_rejects_foreign_rig()
{
    SkinnedMesh m;
    m.skeleton.names           = { "hueso_que_no_existe" };
    m.skeleton.parentIndex     = { -1 };
    m.skeleton.inverseBindPose = { glm::mat4(1.0f) };
    m.skeleton.boneMap         = { { "hueso_que_no_existe", 0 } };

    std::vector<std::string> warnings;
    CHECK(!addAnimationSource(m, "assets/modelAnimation.fbx", warnings));

    CHECK(m.animationClips.empty());
    CHECK(m.animationSources.empty());
    CHECK(!warnings.empty());
}

// Quitar una fuente se lleva sus clips y solo los suyos, y el packing GPU queda
// coherente con la lista nueva (clipCount y offsets).
static void test_remove_animation_source()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    const size_t builtinCount = m.animationClips.size();
    std::vector<std::string> builtinNames;
    for (const auto& c : m.animationClips) builtinNames.push_back(c.name);

    std::vector<std::string> warnings;
    CHECK(addAnimationSource(m, "assets/modelAnimation.fbx", warnings));
    CHECK(removeAnimationSource(m, 1));

    CHECK(m.animationSources.size() == 1u);
    CHECK(m.animationClips.size() == builtinCount);
    for (size_t i = 0; i < builtinNames.size() && i < m.animationClips.size(); i++)
        CHECK(m.animationClips[i].name == builtinNames[i]);   // orden intacto

    // El packing refleja la lista nueva: un boneInfos de más significaría que la
    // GPU seguiría teniendo el bloque del clip borrado.
    const PackedClips packed = packSkinnedClips(m);
    CHECK(packed.boneInfos.size() == m.animationClips.size() * m.skeleton.names.size());
}

// La fuente builtin es el modelo: quitarla dejaría un mesh sin el FBX que lo
// creó. Se rechaza, sin tocar nada.
static void test_remove_builtin_source_is_rejected()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    const size_t before = m.animationClips.size();

    CHECK(!removeAnimationSource(m, 0));
    CHECK(m.animationSources.size() == 1u);
    CHECK(m.animationClips.size() == before);

    // Índice fuera de rango: mismo trato, false y sin efectos
    CHECK(!removeAnimationSource(m, 99));
}

// Rename: cambia el clip y el clipNames de su fuente. Rechaza vacío y duplicado
// — los dos dejarían clips inalcanzables por nombre.
static void test_rename_clip()
{
    SkinnedMesh m;
    m.skeleton.names = { "root" };
    AnimationClip a; a.name = "walk"; m.animationClips.push_back(a);
    AnimationClip b; b.name = "run";  m.animationClips.push_back(b);
    AnimationSource src; src.path = "x.fbx"; src.builtin = true;
    src.clipNames = { "walk", "run" };
    m.animationSources.push_back(src);

    CHECK(renameClip(m, "walk", "andar"));
    CHECK(m.animationClips[0].name == "andar");
    CHECK(m.animationSources[0].clipNames[0] == "andar");

    CHECK(!renameClip(m, "andar", ""));       // vacío
    CHECK(!renameClip(m, "andar", "run"));    // ya existe
    CHECK(!renameClip(m, "no_existe", "x"));  // origen inexistente
    CHECK(m.animationClips[0].name == "andar");
}

// forcedNames es lo que hace que un rename sobreviva a guardar/cargar y a un
// undo: los clips se importan con los nombres que ya tenían, no con los del
// fichero.
static void test_add_animation_source_with_forced_names()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    const size_t builtinCount = m.animationClips.size();

    std::vector<std::string> forced;
    for (size_t i = 0; i < builtinCount; i++)
        forced.push_back("MiClip" + std::to_string(i));

    std::vector<std::string> warnings;
    CHECK(addAnimationSource(m, "assets/modelAnimation.fbx", warnings, &forced));

    CHECK(m.animationSources.size() == 2u);
    CHECK(m.animationSources[1].clipNames == forced);
    for (size_t i = 0; i < forced.size(); i++)
        CHECK(m.animationClips[builtinCount + i].name == forced[i]);
}
```

Registrar en `main()` tras `test_load_animation_clips_missing_file();`:

```cpp
    test_add_animation_source_appends_clips();
    test_add_animation_source_rejects_foreign_rig();
    test_remove_animation_source();
    test_remove_builtin_source_is_rejected();
    test_rename_clip();
    test_add_animation_source_with_forced_names();
```

- [ ] **Step 2: Ejecutar y comprobar que falla**

Run: `.\build.bat`
Expected: FAIL, `error C3861: 'addAnimationSource': identifier not found`.

- [ ] **Step 3: Declarar la API**

En `engine/include/DonTopo/Renderer/SkinnedMeshAnimations.h`, dentro de `namespace DonTopo`, tras `uniqueClipName`:

```cpp
    // Importa las animaciones de path y las añade a mesh.animationClips,
    // registrando la fuente en mesh.animationSources.
    //
    // Los clips se nombran por el basename del fichero (walk.fbx -> "walk",
    // "walk (1)"...): el nombre interno de un FBX de Mixamo es "mixamo.com"
    // para todos, y con eso la lista de clips no se puede leer.
    //
    // forcedNames, si no es nullptr, pisa esos nombres en orden hasta agotarse.
    // Lo usan la carga de escena y el undo de un remove: sin él, un clip
    // renombrado volvería con el nombre del fichero y los estados del grafo que
    // lo referencian quedarían huérfanos.
    //
    // Devuelve false y deja mesh INTACTO si el fichero no aporta nada (ilegible,
    // sin animaciones, o ningún hueso en común con mesh.skeleton).
    bool addAnimationSource(SkinnedMesh& mesh, const std::string& path,
                            std::vector<std::string>& warnings,
                            const std::vector<std::string>* forcedNames = nullptr);

    // Quita la fuente y los clips que aportó. false si el índice está fuera de
    // rango o apunta a la fuente builtin (esa es el modelo, no se puede quitar).
    //
    // Los índices de los clips supervivientes se recolocan; no hace falta
    // arreglar nada en el grafo porque los estados referencian por nombre y
    // AnimatorComponent::bindClips los vuelve a resolver.
    bool removeAnimationSource(SkinnedMesh& mesh, size_t sourceIndex);

    // Renombra un clip y actualiza el clipNames de su fuente. false si oldName
    // no existe, newName está vacío o newName ya está en uso.
    //
    // NO toca los estados del Animator: eso lo hace
    // AnimatorComponent::renameClipReferences, que vive en Core (este módulo es
    // Renderer y no debe depender de Core).
    bool renameClip(SkinnedMesh& mesh, const std::string& oldName,
                    const std::string& newName);
```

- [ ] **Step 4: Implementar**

En `engine/src/Renderer/SkinnedMeshAnimations.cpp`, añadir los includes al principio:

```cpp
#include "DonTopo/Renderer/ModelLoader.h"
#include <filesystem>
```

Y tras `uniqueClipName`:

```cpp
    bool addAnimationSource(SkinnedMesh& mesh, const std::string& path,
                            std::vector<std::string>& warnings,
                            const std::vector<std::string>* forcedNames)
    {
        LoadedClips loaded = ModelLoader::loadAnimationClips(path, mesh.skeleton);
        for (auto& w : loaded.warnings) warnings.push_back(w);

        if (loaded.clips.empty()) return false;   // el warning ya lo puso el loader

        const std::string base = std::filesystem::path(path).stem().string();

        AnimationSource src;
        src.path    = path;
        src.builtin = false;

        for (size_t i = 0; i < loaded.clips.size(); i++)
        {
            AnimationClip clip = std::move(loaded.clips[i]);
            // forcedNames manda; si se agota (el FBX trae más clips que la
            // última vez), el resto cae en la regla normal de basename.
            const bool forced = forcedNames && i < forcedNames->size();
            clip.name = forced ? (*forcedNames)[i]
                                : uniqueClipName(mesh.animationClips, base);
            src.clipNames.push_back(clip.name);
            mesh.animationClips.push_back(std::move(clip));
        }

        mesh.animationSources.push_back(std::move(src));
        return true;
    }

    bool removeAnimationSource(SkinnedMesh& mesh, size_t sourceIndex)
    {
        if (sourceIndex >= mesh.animationSources.size())  return false;
        if (mesh.animationSources[sourceIndex].builtin)   return false;

        const std::vector<std::string>& names = mesh.animationSources[sourceIndex].clipNames;

        for (const std::string& n : names)
        {
            for (size_t i = 0; i < mesh.animationClips.size(); i++)
            {
                if (mesh.animationClips[i].name != n) continue;
                mesh.animationClips.erase(mesh.animationClips.begin() + (long)i);
                break;   // los nombres son únicos: uno y solo uno por nombre
            }
        }

        mesh.animationSources.erase(mesh.animationSources.begin() + (long)sourceIndex);
        return true;
    }

    bool renameClip(SkinnedMesh& mesh, const std::string& oldName,
                    const std::string& newName)
    {
        if (newName.empty() || oldName == newName) return false;

        for (const auto& c : mesh.animationClips)
            if (c.name == newName) return false;      // duplicado

        AnimationClip* target = nullptr;
        for (auto& c : mesh.animationClips)
            if (c.name == oldName) { target = &c; break; }
        if (!target) return false;

        target->name = newName;

        for (auto& src : mesh.animationSources)
            for (auto& n : src.clipNames)
                if (n == oldName) n = newName;

        return true;
    }
```

- [ ] **Step 5: Ejecutar y comprobar que pasa**

Run: `.\build.bat`; luego `.\build-ninja\engine\tests\dt_animator_tests.exe`
Expected: `dt_animator_tests: OK`

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Renderer/SkinnedMeshAnimations.h engine/src/Renderer/SkinnedMeshAnimations.cpp engine/tests/animator_tests.cpp
git commit -m "feat(anim): add, remove and rename clips from extra FBX sources

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: `AnimatorComponent::renameClipReferences`

**Files:**
- Modify: `engine/include/DonTopo/Core/AnimatorComponent.h`
- Modify: `engine/src/Core/AnimatorComponent.cpp`
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: nada de tareas anteriores.
- Produces: `int AnimatorComponent::renameClipReferences(const std::string& oldName, const std::string& newName);` — reescribe `clipName` en todos los estados que usaban `oldName` y devuelve cuántos cambió.

- [ ] **Step 1: Escribir el test que falla**

En `engine/tests/animator_tests.cpp`, tras `test_add_animation_source_with_forced_names()`:

```cpp
// Renombrar un clip tiene que arrastrar a los estados que lo usan: si no, el
// rename dejaría el grafo apuntando a un nombre que ya no existe y bindClips
// los marcaría como huérfanos.
static void test_rename_clip_references_in_animator()
{
    AnimatorComponent a;
    AnimatorComponent::State s0; s0.name = "A"; s0.clipName = "walk";
    AnimatorComponent::State s1; s1.name = "B"; s1.clipName = "run";
    AnimatorComponent::State s2; s2.name = "C"; s2.clipName = "walk";
    a.addState(s0); a.addState(s1); a.addState(s2);

    CHECK(a.renameClipReferences("walk", "andar") == 2);
    CHECK(a.states()[0].clipName == "andar");
    CHECK(a.states()[1].clipName == "run");
    CHECK(a.states()[2].clipName == "andar");

    // Nombre no usado por ningún estado: 0 y sin efectos
    CHECK(a.renameClipReferences("no_existe", "x") == 0);
}
```

Registrar en `main()` tras `test_add_animation_source_with_forced_names();`:

```cpp
    test_rename_clip_references_in_animator();
```

- [ ] **Step 2: Ejecutar y comprobar que falla**

Run: `.\build.bat`
Expected: FAIL, `error C2039: 'renameClipReferences': is not a member of 'DonTopo::AnimatorComponent'`.

- [ ] **Step 3: Declarar**

En `engine/include/DonTopo/Core/AnimatorComponent.h`, junto a `bindClips` (línea ~113):

```cpp
            // Reescribe clipName en los estados que usaban oldName. Devuelve
            // cuántos cambió. Lo llama el Animator Panel tras renombrar un clip
            // del mesh: el grafo referencia por nombre, así que sin esto el
            // rename dejaría los estados huérfanos.
            int renameClipReferences(const std::string& oldName, const std::string& newName);
```

- [ ] **Step 4: Implementar**

En `engine/src/Core/AnimatorComponent.cpp`, tras `bindClips`:

```cpp
    int AnimatorComponent::renameClipReferences(const std::string& oldName,
                                                 const std::string& newName)
    {
        int changed = 0;
        for (auto& st : m_states)
        {
            if (st.clipName != oldName) continue;
            st.clipName = newName;
            changed++;
        }
        return changed;
    }
```

(Si el vector de estados no se llama `m_states`, usar el nombre real del miembro que recorre `bindClips`.)

- [ ] **Step 5: Ejecutar y comprobar que pasa**

Run: `.\build.bat`; luego `.\build-ninja\engine\tests\dt_animator_tests.exe`
Expected: `dt_animator_tests: OK`

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Core/AnimatorComponent.h engine/src/Core/AnimatorComponent.cpp engine/tests/animator_tests.cpp
git commit -m "feat(anim): retarget graph states when a clip is renamed

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: Serialización de `animationSources` en la escena

**Files:**
- Modify: `engine/src/Core/Scene.cpp:237-259` (guardado), `engine/src/Core/Scene.cpp:414-433` (carga)
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `addAnimationSource` con `forcedNames` (Task 4), `AnimationSource` (Task 1).
- Produces: el bloque `"mesh"` gana `"animationSources": [ { "path": str, "builtin": bool, "clips": [str] } ]`. Escenas sin ese campo cargan igual que hoy.

- [ ] **Step 1: Escribir los tests que fallan**

En `engine/tests/animator_tests.cpp`, tras `test_rename_clip_references_in_animator()`:

```cpp
// Las fuentes extra y los renames viven en la escena: el SkinnedMesh se
// reconstruye desde los FBX en cada carga, así que sin esto un proyecto
// guardado perdería todas las animaciones importadas.
static void test_animation_sources_survive_scene_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    std::vector<std::string> warnings;
    CHECK(addAnimationSource(*mesh, "assets/modelAnimation.fbx", warnings));
    const std::string importedName = mesh->animationSources[1].clipNames[0];
    CHECK(renameClip(*mesh, importedName, "SaltoRenombrado"));
    const std::string builtinName = mesh->animationSources[0].clipNames[0];
    go->setMesh(mesh);

    // Un estado que usa el clip importado y renombrado: tras cargar tiene que
    // seguir resolviendo
    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State st;
    st.name = "Salto"; st.clipName = "SaltoRenombrado";
    a->addState(st);
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;

    SkinnedMesh* lm = found->getSkinnedMesh();
    CHECK(lm != nullptr);
    if (!lm) return;

    CHECK(lm->animationSources.size() == 2u);
    CHECK(lm->animationSources[0].builtin == true);
    CHECK(lm->animationSources[0].clipNames[0] == builtinName);
    CHECK(lm->animationSources[1].builtin == false);
    CHECK(lm->animationSources[1].path == "assets/modelAnimation.fbx");
    CHECK(lm->animationSources[1].clipNames[0] == "SaltoRenombrado");

    // El clip renombrado existe con ese nombre en la lista plana
    bool encontrado = false;
    for (const auto& c : lm->animationClips)
        if (c.name == "SaltoRenombrado") encontrado = true;
    CHECK(encontrado);

    // Y el estado lo resuelve sin avisos
    std::vector<std::string> bindWarnings;
    found->getAnimator()->bindClips(*lm, &bindWarnings);
    CHECK(bindWarnings.empty());
    CHECK(found->getAnimator()->states()[0].clipIndex >= 0);
}

// Una fuente cuyo fichero ya no está no puede tumbar la carga de la escena: se
// avisa y se sigue, dejando los estados que la usaran huérfanos (bindClips ya
// los marca).
static void test_missing_animation_source_does_not_break_load(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    go->setMesh(mesh);

    nlohmann::json j = scene.toJson();
    // Se inyecta a mano una fuente que apunta a un fichero inexistente: simula
    // un proyecto cuyo .fbx se borró o se movió después de guardar.
    j["root"]["children"][0]["mesh"]["animationSources"].push_back(
        { {"path", "assets/no_existe.fbx"}, {"builtin", false}, {"clips", {"Fantasma"}} });

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;
    SkinnedMesh* lm = found->getSkinnedMesh();
    CHECK(lm != nullptr);
    if (!lm) return;
    // La fuente fantasma no se registra; el modelo sigue entero
    CHECK(lm->animationSources.size() == 1u);
    CHECK(!lm->animationClips.empty());
}

// Escenas guardadas antes de esta feature no tienen "animationSources": cargan
// con la fuente builtin sintetizada desde sourcePath, sin avisos.
static void test_scene_without_animation_sources_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    go->setMesh(std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx")));

    nlohmann::json j = scene.toJson();
    j["root"]["children"][0]["mesh"].erase("animationSources");

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;
    SkinnedMesh* lm = found->getSkinnedMesh();
    CHECK(lm != nullptr);
    if (!lm) return;
    CHECK(lm->animationSources.size() == 1u);
    CHECK(lm->animationSources[0].builtin == true);
}
```

Registrar en `main()`, junto a los demás tests de round-trip (tras `test_condition_without_compare_fields_loads(pm, am);`):

```cpp
    test_animation_sources_survive_scene_round_trip(pm, am);
    test_missing_animation_source_does_not_break_load(pm, am);
    test_scene_without_animation_sources_loads(pm, am);
```

Nota para quien implemente: los tests acceden al nodo por `j["root"]["children"][0]`. Comprobar con `Scene::toJson` cuál es la forma real del JSON (imprimir `j.dump(2)` una vez) y ajustar la ruta si difiere, sin cambiar lo que el test verifica.

- [ ] **Step 2: Ejecutar y comprobar que falla**

Run: `.\build.bat`; luego `.\build-ninja\engine\tests\dt_animator_tests.exe`
Expected: FAIL — `CHECK(lm->animationSources.size() == 2u)` falla: la fuente extra no se guarda.

- [ ] **Step 3: Guardar las fuentes**

En `engine/src/Core/Scene.cpp`, en el bloque `if (node.hasMesh())` (línea ~237), tras construir `meshJson` y antes de `j["mesh"] = std::move(meshJson);`:

```cpp
            // Fuentes de animación: el SkinnedMesh se reconstruye desde los FBX
            // en cada carga, así que sin esto los clips importados de ficheros
            // extra (y los renames) se perderían al guardar.
            if (const SkinnedMesh* sm = node.getSkinnedMesh())
            {
                nlohmann::json sources = nlohmann::json::array();
                for (const auto& src : sm->animationSources)
                    sources.push_back({ {"path", src.path},
                                        {"builtin", src.builtin},
                                        {"clips", src.clipNames} });
                meshJson["animationSources"] = std::move(sources);
            }
```

- [ ] **Step 4: Cargar las fuentes**

En `engine/src/Core/Scene.cpp`, en el bloque de carga (línea ~424), sustituir:

```cpp
                if (skinned && !sourcePath.empty())
                {
                    auto mesh = std::make_shared<DonTopo::SkinnedMesh>(DonTopo::ModelLoader::loadSkinned(sourcePath));
                    node->setMesh(std::move(mesh));
                }
```

por:

```cpp
                if (skinned && !sourcePath.empty())
                {
                    auto mesh = std::make_shared<DonTopo::SkinnedMesh>(DonTopo::ModelLoader::loadSkinned(sourcePath));

                    // Fuentes de animación. La builtin ya la creó loadSkinned:
                    // de ella solo se recuperan los NOMBRES (un rename), y se
                    // aplican en orden hasta el menor de los dos tamaños — un
                    // FBX reexportado con más o menos clips no debe romper la
                    // carga.
                    if (j["mesh"].contains("animationSources"))
                    {
                        for (const auto& sj : j["mesh"]["animationSources"])
                        {
                            const std::string path = sj.value("path", std::string());
                            const bool builtin     = sj.value("builtin", false);
                            std::vector<std::string> names;
                            if (sj.contains("clips"))
                                names = sj["clips"].get<std::vector<std::string>>();

                            if (builtin)
                            {
                                if (mesh->animationSources.empty()) continue;
                                auto& b = mesh->animationSources[0];
                                const size_t n = names.size() < b.clipNames.size()
                                                ? names.size() : b.clipNames.size();
                                for (size_t i = 0; i < n; i++)
                                    DonTopo::renameClip(*mesh, b.clipNames[i], names[i]);
                                continue;
                            }

                            std::vector<std::string> warnings;
                            if (!DonTopo::addAnimationSource(*mesh, path, warnings, &names))
                            {
                                // Fichero movido, borrado o de otro rig: se
                                // avisa y se sigue. Los estados que usaran sus
                                // clips quedan huérfanos, y bindClips ya lo
                                // reporta — perder la escena entera por esto
                                // sería mucho peor.
                                for (const auto& w : warnings)
                                    std::printf("Scene: %s\n", w.c_str());
                            }
                        }
                    }

                    node->setMesh(std::move(mesh));
                }
```

Añadir el include en `engine/src/Core/Scene.cpp`, junto a los demás de Renderer:

```cpp
#include "DonTopo/Renderer/SkinnedMeshAnimations.h"
```

Nota: `renameClip` sobre un nombre que no cambió (`oldName == newName`) devuelve `false` sin efectos, que es justo lo que se quiere cuando no hubo rename.

- [ ] **Step 5: Ejecutar y comprobar que pasa**

Run: `.\build.bat`; luego `.\build-ninja\engine\tests\dt_animator_tests.exe`
Expected: `dt_animator_tests: OK`

- [ ] **Step 6: Commit**

```bash
git add engine/src/Core/Scene.cpp engine/tests/animator_tests.cpp
git commit -m "feat(anim): persist animation sources and clip renames in scenes

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 7: Comandos de undo/redo

**Files:**
- Modify: `engine/include/DonTopo/Editor/Command.h`
- Modify: `engine/src/Editor/Command.cpp`
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `addAnimationSource`/`removeAnimationSource`/`renameClip` (Task 4), `renameClipReferences` (Task 5), `Renderer::rebuildSkinnedMesh` **no** (Task 8 la añade; aquí el renderer se pasa como puntero y se ignora si es `nullptr`, que es como corren los tests).
- Produces:
```cpp
class DonTopo::AnimationSourceCommand : public ICommand {
public:
    AnimationSourceCommand(Scene& scene, Renderer* renderer, std::string label,
                           uint64_t id, bool add, std::string path,
                           std::vector<std::string> clipNames);
};
class DonTopo::ClipRenameCommand : public ICommand {
public:
    ClipRenameCommand(Scene& scene, std::string label, uint64_t id,
                      std::string oldName, std::string newName);
};
```

- [ ] **Step 1: Escribir los tests que fallan**

En `engine/tests/animator_tests.cpp`, tras `test_animator_command_survives_missing_target()`:

```cpp
// Añadir una fuente pasa por el stack: sin esto, un Ctrl+Z tras importar por
// error un FBX de 60 clips no tendría vuelta atrás.
static void test_animation_source_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    const size_t before = mesh->animationClips.size();
    go->setMesh(mesh);

    AnimationSourceCommand cmd(scene, /*renderer=*/nullptr, "Añadir animaciones",
                                id, /*add=*/true, "assets/modelAnimation.fbx",
                                /*clipNames=*/{});

    cmd.execute();
    CHECK(mesh->animationSources.size() == 2u);
    CHECK(mesh->animationClips.size() == before * 2);

    cmd.undo();
    CHECK(mesh->animationSources.size() == 1u);
    CHECK(mesh->animationClips.size() == before);

    // Redo: vuelven los mismos clips con los mismos nombres que la primera vez
    cmd.execute();
    CHECK(mesh->animationSources.size() == 2u);
    CHECK(mesh->animationClips.size() == before * 2);
}

// El Remove es el mismo comando con add=false, y su undo tiene que devolver los
// clips con los nombres EXACTOS que tenían (por eso el comando guarda
// clipNames): si volvieran con el nombre del fichero, los estados del grafo
// quedarían huérfanos tras un Ctrl+Z.
static void test_animation_source_command_remove_restores_names()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    go->setMesh(mesh);

    std::vector<std::string> warnings;
    CHECK(addAnimationSource(*mesh, "assets/modelAnimation.fbx", warnings));
    const std::string imported = mesh->animationSources[1].clipNames[0];
    CHECK(renameClip(*mesh, imported, "MiSalto"));
    const std::vector<std::string> names = mesh->animationSources[1].clipNames;

    AnimationSourceCommand cmd(scene, nullptr, "Quitar animaciones",
                                id, /*add=*/false, "assets/modelAnimation.fbx", names);

    cmd.execute();
    CHECK(mesh->animationSources.size() == 1u);

    cmd.undo();
    CHECK(mesh->animationSources.size() == 2u);
    CHECK(mesh->animationSources[1].clipNames == names);
    bool encontrado = false;
    for (const auto& c : mesh->animationClips)
        if (c.name == "MiSalto") encontrado = true;
    CHECK(encontrado);
}

// El rename es undoable y arrastra a los estados del grafo en ambos sentidos.
static void test_clip_rename_command()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    auto mesh = std::make_shared<SkinnedMesh>();
    AnimationClip c; c.name = "walk"; mesh->animationClips.push_back(c);
    AnimationSource src; src.path = "x.fbx"; src.builtin = true; src.clipNames = { "walk" };
    mesh->animationSources.push_back(src);
    go->setMesh(mesh);

    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State st; st.name = "Andar"; st.clipName = "walk";
    a->addState(st);
    go->setAnimator(a);

    ClipRenameCommand cmd(scene, "Renombrar clip", id, "walk", "andar");

    cmd.execute();
    CHECK(mesh->animationClips[0].name == "andar");
    CHECK(a->states()[0].clipName == "andar");

    cmd.undo();
    CHECK(mesh->animationClips[0].name == "walk");
    CHECK(a->states()[0].clipName == "walk");
}

// Igual que el resto de comandos: el objeto puede haber desaparecido entre
// execute y undo. Se resuelve por id cada vez, nunca por puntero crudo.
static void test_animation_source_command_survives_missing_target()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    AnimationSourceCommand cmd(scene, nullptr, "Añadir animaciones",
                                id, true, "assets/modelAnimation.fbx", {});
    scene.removeGameObject(go);
    cmd.execute();   // findById devuelve nullptr y sale sin tocar nada
    cmd.undo();
}
```

Registrar en `main()` tras `test_animator_command_survives_missing_target();`:

```cpp
    test_animation_source_command_add_undo_redo();
    test_animation_source_command_remove_restores_names();
    test_clip_rename_command();
    test_animation_source_command_survives_missing_target();
```

- [ ] **Step 2: Ejecutar y comprobar que falla**

Run: `.\build.bat`
Expected: FAIL, `error C2065: 'AnimationSourceCommand': undeclared identifier`.

- [ ] **Step 3: Declarar los comandos**

En `engine/include/DonTopo/Editor/Command.h`, tras `AnimatorComponentCommand`:

```cpp
// Añade (add=true) o quita (add=false) una fuente de animación del SkinnedMesh
// del GameObject id; undo() hace lo contrario. Mismo contrato que el resto:
// resuelve el GameObject por id en cada execute()/undo().
//
// m_clipNames guarda los nombres que la fuente aportó. Sin él, deshacer un
// Remove reimportaría el fichero con los nombres del FBX y se perdería
// cualquier rename — dejando huérfanos los estados del grafo que los usaban.
//
// renderer puede ser nullptr (tests headless). Cuando no lo es, los SSBOs del
// objeto skinned se rehacen: la lista de clips ha cambiado y la GPU tiene la
// vieja.
class AnimationSourceCommand : public ICommand {
public:
    AnimationSourceCommand(Scene& scene, Renderer* renderer, std::string label,
                            uint64_t id, bool add, std::string path,
                            std::vector<std::string> clipNames);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void applyAdd();
    void applyRemove();

    Scene& m_scene;
    Renderer* m_renderer;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    std::string m_path;
    std::vector<std::string> m_clipNames;
};

// Renombra un clip del mesh y arrastra los estados del Animator que lo usaban.
// No toca la GPU: los buffers van por índice de clip, y renombrar no reordena
// nada.
class ClipRenameCommand : public ICommand {
public:
    ClipRenameCommand(Scene& scene, std::string label, uint64_t id,
                       std::string oldName, std::string newName);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(const std::string& from, const std::string& to);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    std::string m_oldName;
    std::string m_newName;
};
```

Añadir al principio de `Command.h`, junto a los demás includes:

```cpp
#include <vector>
```

- [ ] **Step 4: Implementar**

En `engine/src/Editor/Command.cpp`, añadir los includes que falten al principio:

```cpp
#include "DonTopo/Renderer/SkinnedMeshAnimations.h"
#include "DonTopo/Renderer/Renderer.h"
```

Y al final, dentro de `namespace DonTopo`:

```cpp
    AnimationSourceCommand::AnimationSourceCommand(Scene& scene, Renderer* renderer,
                                                    std::string label, uint64_t id, bool add,
                                                    std::string path,
                                                    std::vector<std::string> clipNames)
        : m_scene(scene), m_renderer(renderer), m_label(std::move(label)), m_id(id),
          m_add(add), m_path(std::move(path)), m_clipNames(std::move(clipNames)) {}

    void AnimationSourceCommand::execute() { m_add ? applyAdd() : applyRemove(); }
    void AnimationSourceCommand::undo()    { m_add ? applyRemove() : applyAdd(); }

    void AnimationSourceCommand::applyAdd()
    {
        GameObject* go = m_scene.findById(m_id);
        if (!go) return;
        SkinnedMesh* mesh = go->getSkinnedMesh();
        if (!mesh) return;

        std::vector<std::string> warnings;
        // m_clipNames vacío = primera vez (el usuario acaba de elegir el
        // fichero): los nombres los decide addAnimationSource y se guardan aquí
        // para que un redo posterior reproduzca exactamente los mismos.
        const std::vector<std::string>* forced = m_clipNames.empty() ? nullptr : &m_clipNames;
        if (!addAnimationSource(*mesh, m_path, warnings, forced)) return;

        m_clipNames = mesh->animationSources.back().clipNames;

        if (m_renderer && go->skinnedRenderIndex >= 0)
            m_renderer->rebuildSkinnedMesh(go->skinnedRenderIndex, *mesh);
    }

    void AnimationSourceCommand::applyRemove()
    {
        GameObject* go = m_scene.findById(m_id);
        if (!go) return;
        SkinnedMesh* mesh = go->getSkinnedMesh();
        if (!mesh) return;

        // Se localiza por path Y por no-builtin: dos fuentes pueden compartir
        // path (reimportar el mismo fichero), así que se quita la última que
        // coincida, que es la que este comando añadió.
        for (size_t i = mesh->animationSources.size(); i-- > 0; )
        {
            const auto& src = mesh->animationSources[i];
            if (src.builtin || src.path != m_path) continue;
            m_clipNames = src.clipNames;
            removeAnimationSource(*mesh, i);
            break;
        }

        if (m_renderer && go->skinnedRenderIndex >= 0)
            m_renderer->rebuildSkinnedMesh(go->skinnedRenderIndex, *mesh);
    }

    ClipRenameCommand::ClipRenameCommand(Scene& scene, std::string label, uint64_t id,
                                          std::string oldName, std::string newName)
        : m_scene(scene), m_label(std::move(label)), m_id(id),
          m_oldName(std::move(oldName)), m_newName(std::move(newName)) {}

    void ClipRenameCommand::execute() { apply(m_oldName, m_newName); }
    void ClipRenameCommand::undo()    { apply(m_newName, m_oldName); }

    void ClipRenameCommand::apply(const std::string& from, const std::string& to)
    {
        GameObject* go = m_scene.findById(m_id);
        if (!go) return;
        SkinnedMesh* mesh = go->getSkinnedMesh();
        if (!mesh) return;
        if (!renameClip(*mesh, from, to)) return;
        if (go->hasAnimator())
            go->getAnimator()->renameClipReferences(from, to);
    }
```

Nota: `rebuildSkinnedMesh` la añade la Task 8. Si esta tarea se implementa antes, declararla ya en `Renderer.h` con un cuerpo mínimo (`{ (void)index; (void)mesh; }`) o implementar la Task 8 primero — no dejar la llamada sin declarar.

- [ ] **Step 5: Ejecutar y comprobar que pasa**

Run: `.\build.bat`; luego `.\build-ninja\engine\tests\dt_animator_tests.exe`
Expected: `dt_animator_tests: OK`

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Editor/Command.h engine/src/Editor/Command.cpp engine/tests/animator_tests.cpp
git commit -m "feat(anim): undoable animation source add/remove and clip rename

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 8: `Renderer::rebuildSkinnedMesh`

**Files:**
- Modify: `engine/include/DonTopo/Renderer/Renderer.h:101`
- Modify: `engine/src/Renderer/Renderer.cpp:1134-1150` (pool gráfico), `1640-1652` (pool de compute), `1793-1827` (destroy), `1829+` (add)
- Test: sin test unitario — requiere un `VkDevice` vivo. Se valida con build limpio y verificación manual en la Task 9.

**Interfaces:**
- Consumes: `packSkinnedClips` (existente).
- Produces: `void Renderer::rebuildSkinnedMesh(int index, const SkinnedMesh& mesh);` — rehace todos los recursos GPU del objeto `index` conservando `transform`, `animTime` y `activeClip`. El `skinnedRenderIndex` del GameObject no cambia.

- [ ] **Step 1: Hacer los descriptor pools liberables**

Sin esto, cada rebuild consume sets de un pool que nunca los devuelve: el de compute tiene `maxSets = 16`, así que la importación número 16 lanzaría `failed to allocate compute descriptor set!`.

En `engine/src/Renderer/Renderer.cpp`, en `createDescriptorPool()` (línea ~1143), tras `poolInfo.maxSets = n;`:

```cpp
        // FREE_DESCRIPTOR_SET: rebuildSkinnedMesh destruye y recrea el objeto en
        // su sitio, y sin poder devolver los sets al pool cada reimportación
        // consumiría slots hasta agotarlo.
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
```

Y en la creación del pool de compute (línea ~1648), tras `poolInfo.maxSets = 16;`:

```cpp
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
```

- [ ] **Step 2: Devolver los sets al destruir el objeto skinned**

En `engine/src/Renderer/Renderer.cpp`, en `destroySkinnedRenderObject`, justo antes de `obj.matGfx.clear();`:

```cpp
        // Los sets vuelven al pool (creado con FREE_DESCRIPTOR_SET_BIT): sin
        // esto, reconstruir el objeto lo agotaría.
        if (obj.computeDescSet != VK_NULL_HANDLE)
        {
            vkFreeDescriptorSets(m_gpu.device(), m_computeDescPool, 1, &obj.computeDescSet);
            obj.computeDescSet = VK_NULL_HANDLE;
        }
        for (auto& mgfx : obj.matGfx)
        {
            if (mgfx.descSets[0] != VK_NULL_HANDLE)
                vkFreeDescriptorSets(m_gpu.device(), m_descriptorPool, MAX_FRAMES, mgfx.descSets);
            mgfx.descSets[0] = VK_NULL_HANDLE;
            mgfx.descSets[1] = VK_NULL_HANDLE;
        }
```

- [ ] **Step 3: Extraer `initSkinnedRenderObject`**

En `engine/include/DonTopo/Renderer/Renderer.h`, en la sección pública junto a `addSkinnedMesh` (línea 101):

```cpp
            int addSkinnedMesh(const SkinnedMesh& mesh);
            // Rehace TODOS los recursos GPU del objeto skinned `index` a partir
            // de `mesh`, en el mismo slot (el skinnedRenderIndex del GameObject
            // no cambia). Necesario tras añadir o quitar clips: los keyframes
            // viven en SSBOs subidos una sola vez, y la GPU tendría la lista
            // vieja. Conserva transform, animTime y activeClip.
            void rebuildSkinnedMesh(int index, const SkinnedMesh& mesh);
```

Y en la sección privada, junto a las demás helpers de objetos:

```cpp
            // Cuerpo compartido por addSkinnedMesh y rebuildSkinnedMesh: crea
            // buffers, sube SSBOs, aloja descriptor sets y carga texturas sobre
            // un SkinnedRenderObject ya vacío.
            void initSkinnedRenderObject(SkinnedRenderObject& obj, const SkinnedMesh& mesh);
```

En `engine/src/Renderer/Renderer.cpp`, renombrar la definición actual:

```cpp
    int Renderer::addSkinnedMesh(const SkinnedMesh& mesh)
    {
        m_skinnedObjects.emplace_back();
        SkinnedRenderObject& obj = m_skinnedObjects.back();
        ...
```

a:

```cpp
    void Renderer::initSkinnedRenderObject(SkinnedRenderObject& obj, const SkinnedMesh& mesh)
    {
        ...
```

manteniendo el cuerpo entero **sin cambios** salvo: quitar las dos primeras líneas (`m_skinnedObjects.emplace_back();` y la que obtiene `obj`) y quitar el `return` final del índice. Después, añadir justo encima:

```cpp
    int Renderer::addSkinnedMesh(const SkinnedMesh& mesh)
    {
        m_skinnedObjects.emplace_back();
        initSkinnedRenderObject(m_skinnedObjects.back(), mesh);
        return (int)m_skinnedObjects.size() - 1;
    }
```

(Comprobar cuál es exactamente la sentencia `return` original de `addSkinnedMesh` al final de su cuerpo y replicar ese valor.)

- [ ] **Step 4: Implementar `rebuildSkinnedMesh`**

En `engine/src/Renderer/Renderer.cpp`, tras `addSkinnedMesh`:

```cpp
    void Renderer::rebuildSkinnedMesh(int index, const SkinnedMesh& mesh)
    {
        if (index < 0 || index >= (int)m_skinnedObjects.size()) return;

        // Espera a que la GPU termine: un command buffer en vuelo (double
        // buffering) puede estar leyendo los buffers que vamos a destruir.
        // Mismo motivo que en removeGameObject.
        vkDeviceWaitIdle(m_gpu.device());

        SkinnedRenderObject& obj = m_skinnedObjects[index];

        // El estado de animación es del Animator, no de los buffers: perderlo
        // haría que el personaje diera un salto visible al importar un fichero.
        const glm::mat4 transform  = obj.transform;
        const float     animTime   = obj.animTime;
        const uint32_t  activeClip = obj.activeClip;

        destroySkinnedRenderObject(obj);
        obj = SkinnedRenderObject{};
        initSkinnedRenderObject(obj, mesh);

        obj.transform = transform;
        obj.animTime  = animTime;
        // Clamp: la lista de clips puede haber encogido y activeClip apuntaría
        // fuera del SSBO de BoneInfos, con el compute leyendo basura en
        // silencio. Mismo criterio que setAnimationState.
        obj.activeClip = (activeClip < obj.clipCount) ? activeClip : 0;
    }
```

- [ ] **Step 5: Compilar y comprobar que no hay regresiones**

Run: `.\build.bat`
Expected: build sin errores ni warnings nuevos.

Run: `.\build-ninja\engine\tests\dt_animator_tests.exe`
Expected: `dt_animator_tests: OK`

Run: `.\build-ninja\sandbox\DonTopoSandbox.exe` (o el ejecutable que produzca `sandbox/`), y comprobar que el personaje animado del sandbox sigue renderizándose y animándose como antes. Cerrar la ventana.
Expected: sin cambios visibles respecto a antes de esta tarea, sin errores de validación de Vulkan en consola.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Renderer/Renderer.h engine/src/Renderer/Renderer.cpp
git commit -m "feat(render): rebuild a skinned object in place after clip changes

Keyframe SSBOs are uploaded once at creation, so changing the clip list
needs a full rebuild. Descriptor pools now allow freeing sets, otherwise
repeated rebuilds would exhaust them.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 9: Sección *Animation Sources* en el Animator Panel

**Files:**
- Modify: `engine/include/DonTopo/Editor/AnimatorPanel.h`
- Modify: `engine/src/Editor/AnimatorPanel.cpp:519-563`
- Test: verificación manual (la UI de ImGui no es testeable headless en este repo)

**Interfaces:**
- Consumes: `addAnimationSource`/`removeAnimationSource` (Task 4), `AnimationSourceCommand`/`ClipRenameCommand` (Task 7), `Renderer::rebuildSkinnedMesh` (Task 8).
- Produces: nada que consuman tareas posteriores.

- [ ] **Step 1: Ampliar la cabecera del panel**

En `engine/include/DonTopo/Editor/AnimatorPanel.h`, añadir los includes:

```cpp
#include <memory>
#include <vector>
```

Y la forward declaration del diálogo, junto a la de `ax::NodeEditor`:

```cpp
namespace IGFD { class FileDialog; }
```

En la sección privada de la clase, tras `drawConditionsPopup`:

```cpp
    // Lista de ficheros FBX que aportan clips, con Add/Remove y rename inline.
    void drawAnimationSources(EditorContext& ctx, GameObject* go);
    // Drena el diálogo de fichero cada frame, incondicionalmente: si solo se
    // drenara con el GameObject correcto seleccionado, cambiar de selección con
    // el diálogo abierto dejaría m_animSrcDlgOpen atascado en true para siempre.
    // Mismo patrón que PropertiesPanel::drawMeshDialog.
    void drawAnimationSourceDialog(EditorContext& ctx);
    // Importa path como fuente del GameObject seleccionado, vía comando (undo).
    void importAnimationSource(EditorContext& ctx, GameObject* go, const std::string& path);

    // Instancia propia y no compartida con los diálogos de PropertiesPanel:
    // IGFD guarda estado por instancia, y compartirla haría que redimensionar
    // un popup tocara el otro.
    std::unique_ptr<IGFD::FileDialog> m_animSrcDialog;
    bool m_animSrcDlgOpen = false;
    std::string m_animSrcError;      // último error, en rojo bajo la lista
    // Clip cuyo nombre se está editando, "" si ninguno.
    std::string m_renamingClip;
    char m_renameBuf[64] = {};
```

- [ ] **Step 2: Construir y destruir el diálogo**

En `engine/src/Editor/AnimatorPanel.cpp`, añadir includes:

```cpp
#include "DonTopo/Editor/UndoManager.h"
#include "DonTopo/Editor/Command.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Renderer/Renderer.h"
#include "DonTopo/Renderer/SkinnedMeshAnimations.h"
#include <ImGuiFileDialog.h>
#include <filesystem>
#include <memory>
```

Y en el constructor, tras `m_ctx = ed::CreateEditor(&config);`:

```cpp
    m_animSrcDialog = std::make_unique<IGFD::FileDialog>();
```

- [ ] **Step 3: Implementar la sección**

En `engine/src/Editor/AnimatorPanel.cpp`, antes de `AnimatorPanel::draw`:

```cpp
void AnimatorPanel::importAnimationSource(EditorContext& ctx, GameObject* go, const std::string& path)
{
    SkinnedMesh* mesh = go->getSkinnedMesh();
    if (!mesh) return;

    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".fbx")
    {
        m_animSrcError = "Formato no soportado: " + ext;
        return;
    }

    // Ensayo en seco sobre una copia: el comando no puede decir "no" a medias,
    // y así el error del loader (rig equivocado, fichero sin animaciones) llega
    // al usuario antes de meter nada en el stack de undo.
    SkinnedMesh probe = *mesh;
    std::vector<std::string> warnings;
    const bool ok = addAnimationSource(probe, path, warnings);
    for (const auto& w : warnings) ctx.pushLog("Animator: " + w);

    if (!ok)
    {
        m_animSrcError = warnings.empty() ? ("No se pudieron importar animaciones de " + path)
                                           : warnings.back();
        return;
    }
    m_animSrcError.clear();

    auto cmd = std::make_unique<AnimationSourceCommand>(
        *ctx.scene, ctx.renderer, "Añadir animaciones", go->id,
        /*add=*/true, path, std::vector<std::string>{});
    cmd->execute();
    ctx.undo->push(std::move(cmd));
    ctx.pushLog("Animator: animaciones de '" + path + "' importadas");
}

void AnimatorPanel::drawAnimationSources(EditorContext& ctx, GameObject* go)
{
    SkinnedMesh* mesh = go->getSkinnedMesh();
    if (!mesh) return;

    if (!ImGui::CollapsingHeader("Animation Sources", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    int sourceToRemove = -1;
    for (size_t s = 0; s < mesh->animationSources.size(); s++)
    {
        const AnimationSource& src = mesh->animationSources[s];
        ImGui::PushID((int)s);

        const std::string file = std::filesystem::path(src.path).filename().string();
        const std::string label = file + "  (" + std::to_string(src.clipNames.size()) + " clips)"
                                + (src.builtin ? "  [modelo]" : "");

        const bool open = ImGui::TreeNodeEx("##src", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", label.c_str());

        // La fuente builtin es el FBX del modelo: quitarla dejaría la malla sin
        // el fichero que la creó, así que el botón existe pero deshabilitado
        // (mostrarlo y explicarlo enseña la regla; ocultarlo la esconde).
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20.0f);
        ImGui::BeginDisabled(src.builtin);
        if (ImGui::SmallButton("X")) sourceToRemove = (int)s;
        ImGui::EndDisabled();
        if (src.builtin && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Es el FBX del modelo: sus animaciones no se pueden quitar por separado.");

        if (open)
        {
            for (const std::string& clipName : src.clipNames)
            {
                ImGui::PushID(clipName.c_str());
                if (m_renamingClip == clipName)
                {
                    ImGui::SetNextItemWidth(180.0f);
                    if (ImGui::InputText("##rename", m_renameBuf, sizeof(m_renameBuf),
                                          ImGuiInputTextFlags_EnterReturnsTrue))
                    {
                        const std::string nuevo = m_renameBuf;
                        auto cmd = std::make_unique<ClipRenameCommand>(
                            *ctx.scene, "Renombrar clip", go->id, clipName, nuevo);
                        cmd->execute();
                        // El comando no informa de si el rename se aplicó, así
                        // que se comprueba en el mesh: un duplicado o un vacío
                        // no debe entrar en el stack de undo.
                        bool aplicado = false;
                        for (const auto& c : mesh->animationClips)
                            if (c.name == nuevo) aplicado = true;
                        if (aplicado)
                        {
                            ctx.undo->push(std::move(cmd));
                            ctx.pushLog("Animator: clip '" + clipName + "' renombrado a '" + nuevo + "'");
                        }
                        else
                        {
                            m_animSrcError = "Nombre inválido o ya en uso: " + nuevo;
                        }
                        m_renamingClip.clear();
                    }
                    if (ImGui::IsItemDeactivated()) m_renamingClip.clear();
                }
                else
                {
                    ImGui::BulletText("%s", clipName.c_str());
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                    {
                        m_renamingClip = clipName;
                        std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", clipName.c_str());
                    }
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    if (sourceToRemove >= 0)
    {
        const AnimationSource& src = mesh->animationSources[(size_t)sourceToRemove];
        auto cmd = std::make_unique<AnimationSourceCommand>(
            *ctx.scene, ctx.renderer, "Quitar animaciones", go->id,
            /*add=*/false, src.path, src.clipNames);
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        // Los estados que usaran esos clips quedan huérfanos a propósito: el
        // grafo es trabajo del usuario y borrarlo por él sería peor que dejarlo
        // avisado. bindClips los reporta en la siguiente vinculación.
        ctx.pushLog("Animator: fuente de animación quitada; los estados que la usaran quedan sin clip");
    }

    if (ImGui::Button("Add Animation FBX..."))
    {
        IGFD::FileDialogConfig cfg;
        cfg.path = ".";
        cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                    ImGuiFileDialogFlags_HideColumnDate |
                    ImGuiFileDialogFlags_DisableThumbnailMode |
                    ImGuiFileDialogFlags_DisablePlaceMode;
        m_animSrcDialog->OpenDialog("AddAnimSrcDlg", "Choose Animation FBX", ".fbx", cfg);
        m_animSrcDlgOpen = true;
    }

    // Drop target: el Content Browser ya emite el payload con la ruta del .fbx
    ImGui::SameLine();
    ImGui::TextDisabled("(o arrastra un .fbx aquí)");
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
            importAnimationSource(ctx, go, std::string(static_cast<const char*>(payload->Data)));
        ImGui::EndDragDropTarget();
    }

    if (!m_animSrcError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_animSrcError.c_str());

    ImGui::Separator();
}

void AnimatorPanel::drawAnimationSourceDialog(EditorContext& ctx)
{
    if (!m_animSrcDlgOpen) return;
    if (!m_animSrcDialog->Display("AddAnimSrcDlg")) return;

    if (m_animSrcDialog->IsOk() && ctx.selected && ctx.selected->hasAnimator())
        importAnimationSource(ctx, ctx.selected, m_animSrcDialog->GetFilePathName());

    m_animSrcDialog->Close();
    m_animSrcDlgOpen = false;
}
```

Nota: comprobar en `engine/src/Editor/ContentBrowserPanel.cpp` cuál es el identificador real del payload de drag&drop y usar ese en lugar de `"CONTENT_BROWSER_ITEM"` si difiere.

- [ ] **Step 4: Enganchar en `draw`**

En `AnimatorPanel::draw`, sustituir el bloque que empieza en `// --- Añadir estado desde los clips del modelo ---` por:

```cpp
    drawAnimationSources(ctx, go);

    // --- Añadir estado desde los clips del modelo ---
    SkinnedMesh* mesh = go->getSkinnedMesh();
```

(el resto del bloque `if (!mesh || mesh->animationClips.empty()) ... else if (ImGui::BeginCombo(...))` se mantiene igual).

Y justo antes del `ImGui::End();` final de `draw`:

```cpp
    // Fuera del early-return de "sin Animator": el diálogo tiene que drenarse
    // aunque la selección cambie mientras está abierto.
    drawAnimationSourceDialog(ctx);
```

Además, en el early-return de "GameObject sin Animator" (el bloque `if (!go || !go->hasAnimator())`), llamar a `drawAnimationSourceDialog(ctx);` antes de `ImGui::End(); return;` por el mismo motivo.

- [ ] **Step 5: Compilar**

Run: `.\build.bat`
Expected: build sin errores.

- [ ] **Step 6: Verificación manual**

Arrancar el sandbox y comprobar, punto por punto:

1. Seleccionar el GameObject animado → panel Animator → la sección *Animation Sources* muestra una fila con el FBX del modelo, marcada `[modelo]`, con la `X` deshabilitada y tooltip al pasar por encima.
2. `Add Animation FBX...` → elegir `assets/modelAnimation.fbx` otra vez → aparece una segunda fila con sus clips, nombrados `modelAnimation`, `modelAnimation (1)`… y el Log muestra el mensaje de importación.
3. El desplegable `Add State from Clip` ahora lista también los clips nuevos; crear un estado con uno de ellos y comprobar en Play Mode que el personaje lo reproduce.
4. Doble clic sobre el nombre de un clip → editarlo → Enter → el nombre cambia, y el estado que lo usaba sigue apuntando a él (no se pone huérfano).
5. Ctrl+Z tras el rename → vuelve el nombre anterior en el clip y en el estado.
6. `X` sobre la fuente extra → sus clips desaparecen de la lista y del desplegable; el personaje sigue animándose con los clips del modelo.
7. Ctrl+Z tras el borrado → vuelven los clips con sus nombres (incluido el renombrado).
8. Guardar la escena, cerrar el sandbox, reabrir y cargar → las fuentes, los nombres y el grafo siguen ahí.
9. Repetir la importación 20 veces seguidas → no aparece `failed to allocate compute descriptor set!` (valida el `FREE_DESCRIPTOR_SET_BIT` de la Task 8).

Anotar cualquier desviación y corregirla antes de commitear.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Editor/AnimatorPanel.h engine/src/Editor/AnimatorPanel.cpp
git commit -m "feat(editor): manage animation source files from the Animator panel

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Cobertura del spec

| Sección del spec | Tarea |
|---|---|
| §1 Datos: `AnimationSource` | 1 |
| §2 Loader de solo animaciones | 3 (helper compartido: 2 y 3) |
| §3 Merge (`add`/`remove`/`rename`) | 2, 4 |
| §4 UI Animator Panel (lista, diálogo, drag&drop, rename, undo) | 7, 9 |
| §4 Propagación del rename a los estados | 5 |
| §5 Validación de esqueleto y errores | 3 (diagnóstico), 4 (rechazo), 9 (mensajes) |
| §6 Refresco GPU | 8 |
| §7 Serialización | 6 |
| Tests 1-7 del spec | 3, 4, 5, 6, 7 |
