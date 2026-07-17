# Animator: grafo de estados de animación

**Fecha:** 2026-07-16
**Estado:** Diseño aprobado

## Objetivo

Sistema de animación tipo Animator de Unity: un modelo carga N clips desde su FBX y una
máquina de estados editable visualmente decide cuál se reproduce en cada momento.

Extiende el skinning GPU existente (`bone_eval.comp` → `bone_hierarchy.comp` →
`skinning.comp`); no lo sustituye.

## Requisitos

1. N animaciones por modelo, cargadas de todas las de `scene->mAnimations`, conmutables en runtime.
2. Grafo de estados editable visualmente: nodo = clip, link = transición dirigida, con estado de entrada.
3. Condiciones de transición: `bool`, `trigger`, `animation finished`. Nada más. Los parámetros
   (bools/triggers) se declaran en el Animator y se consultan/setean desde C++ y desde Lua.
4. Flag de loop por clip.
5. Sin blending. Transición = corte instantáneo.

## Fuera de alcance

Blending, blend trees, capas, máscaras de huesos, root motion, condiciones numéricas
(int/float), transiciones con duración o exit time, sub-state machines, Animator como
asset reutilizable entre GameObjects.

## Decisiones cerradas de entrada

- Librería de nodos: **imgui-node-editor (thedmd)**, vía `FetchContent`.
- Sin blending.
- Solo las 3 condiciones listadas.

---

## Decisión central: cómo almacenar N clips en GPU

Sin blending se evalúa **un solo clip por frame**. Lo único que cruza a GPU al cambiar de
estado es `(clip activo, animTime)`; el grafo es lógica de CPU.

### Opciones evaluadas

**(a) Re-subir los SSBOs al cambiar de clip.** Descartada: convierte cada transición en un
upload + stall, para un dato que ya estaba en VRAM. Sin argumento a favor.

**(b) Concatenar los keyframes de todos los clips en los mismos 3 SSBOs**, con un offset de
clip que indexa el bloque activo. Un upload al cargar el modelo; cambiar de clip = cambiar
un `uint`.

**(c) Un set de SSBOs + descriptor set por clip**, rebind al cambiar.

### Números (B=100 huesos, C=20 clips, ~30 keys/canal/hueso)

`GpuPosKey`/`GpuRotKey` = 32 B, `GpuBoneInfo` = 96 B (8 ints + mat4).

| Métrica | (b) concatenar | (c) por clip |
| --- | --- | --- |
| Keyframes VRAM | 5,6 MB | 5,6 MB (idénticos) |
| BoneInfo VRAM | C×B×96 = **192 KB** | B×96 = 9,6 KB |
| VkBuffer / objeto | **4** | 3×C+1 = **61** |
| VkDeviceMemory / objeto | **4** | **61** |
| Descriptor sets / objeto | **1** | **20** |
| Coste de cambiar de clip | escribir un `uint` en el push | rebind de descriptor set |

### Elección: (b)

**(c) no cabe en el pool actual.** `Renderer.cpp:1647` fija `poolInfo.maxSets = 16` y
`descriptorCount = 8*16`, creado en `createComputePipelines()` **antes de conocer el número
de clips de ningún modelo**. Con (c), un personaje de 20 clips agota el pool entero. Habría
que rehacer el pool a demanda o sobredimensionarlo a ciegas: complejidad nueva a cambio de
nada.

**(c) topa contra `maxMemoryAllocationCount`.** `GpuResources::uploadBuffer` crea un
`VkDeviceMemory` por buffer. 61 allocs/objeto contra el límite típico de 4096 → techo de
~67 personajes. Con (b), 4 allocs/objeto → ~1000.

**(b) no cuesta nada en runtime.** Solo `bone_eval.comp` lee keyframes, y evalúa un clip por
frame: los otros 19 clips residen en el buffer y nunca se leen — **cero ancho de banda**. El
coste de indexar es un `add`. Y (c) tampoco ahorra en el bind: `recordComputePass` ya emite
`vkCmdBindDescriptorSets` por objeto y por frame (`Renderer.cpp:2254`), así que su rebind no
es más barato que el push de (b).

**El precio de (b)** son 192 KB de `BoneInfo` con `parentIndex`/`inverseBindPose` duplicados
por clip: **2,4 % sobre los 5,6 MB de keyframes**. A cambio compra que los shaders casi no
cambien (ver abajo).

### Layout `[clip][hueso]` y su consecuencia en shaders

`parentIndex` e `inverseBindPose` son **por esqueleto, idénticos en todos los clips**.
Disponiendo `BoneInfos` como `[clip][hueso]`, el bloque del clip 0 contiene ya los valores
correctos de jerarquía para cualquier clip. Por tanto:

- **`bone_hierarchy.comp`: 0 cambios.** Sigue leyendo `boneInfos.data[i]` (= bloque del clip
  0) para `parentIndex`/`inverseBindPose`, iguales en todo clip, y escribiendo
  `finalBones.data[i]` con índice `0..boneCount`.
- **`skinning.comp`: 0 cambios.** Solo toca los bindings 5/6/7.
- **`bone_eval.comp`: 1 línea.** `boneInfos.data[bi]` → `boneInfos.data[push.clipBase + bi]`.

**Los bindings de los 3 shaders NO se tocan.**

### ABI de `ComputePush` (aprobada explícitamente)

`ComputePush.pad` (hoy declarado en los 3 shaders y escrito a 0 en `Renderer.cpp:2252`) pasa
a llamarse `clipBase` y lleva `activeClip * boneCount`. Mismo offset (12), mismo
`sizeof(ComputePush) == 16`. No hay campo nuevo ni cambio de `VkPushConstantRange`.

**Invariante de no-regresión:** con `activeClip = 0` y un solo clip, `clipBase = 0` y la ruta
es byte a byte la actual.

---

## Arquitectura

### Datos (`engine/include/DonTopo/Renderer/SkinnedMesh.h`)

- `SkinnedMesh::animationClip` (singular) → `std::vector<AnimationClip> animationClips`.
- `AnimationClip` **no** gana un flag `loop`.

**Dónde vive el loop (requisito 4).** En `AnimatorComponent::State`, no en `AnimationClip`.
El `SkinnedMesh` no se serializa: se reconstruye llamando a `ModelLoader::loadSkinned` sobre
el FBX en cada carga de escena (`Scene.cpp:249`). Un `loop` en `AnimationClip` se perdería en
cada ciclo guardar → cargar, y el criterio de aceptación 3 lo exige persistente. En el
`State` sí sobrevive, porque el grafo entero va al JSON de escena.

El requisito se cumple igual — un nodo del grafo contiene exactamente un clip — y además dos
nodos que usen el mismo clip pueden tener loop distinto.

### Carga (`engine/src/Renderer/ModelLoader.cpp:275-313`)

El bloque actual, que lee `scene->mAnimations[0]`, se envuelve en un bucle
`for (c = 0; c < scene->mNumAnimations; c++)` y empuja a `animationClips`. El cuerpo interno
(canales, keys, remap por `boneMap`) no cambia.

Clip con nombre vacío → `"Animation N"`. Los FBX de Mixamo dan nombres vacíos o
`"mixamo.com"` repetido; sin nombre único la UI del grafo es inusable y el `bindClips` por
nombre (ver serialización) no puede resolver.

### Empaquetado GPU

El flatten de keyframes que hoy vive dentro de `Renderer::addSkinnedMesh`
(`Renderer.cpp:1845-1892`) se extrae a una función libre en
`engine/include/DonTopo/Renderer/SkinnedMeshPacking.h` + `.cpp`:

```cpp
struct PackedClips {
    std::vector<GpuPosKey>   pos, scale;
    std::vector<GpuRotKey>   rot;
    std::vector<GpuBoneInfo> boneInfos;   // C*B entradas, layout [clip][hueso]
};
PackedClips packSkinnedClips(const SkinnedMesh& mesh);
```

**Por qué se extrae:** es el único cambio de este diseño que toca el layout de memoria de
GPU, y dentro de `addSkinnedMesh` solo se puede probar con un `VkDevice` vivo — es decir, no
se puede probar (los tests son headless). Fuera, es una función pura sobre `std::vector` que
el test 9 verifica sin Vulkan. `addSkinnedMesh` queda llamándola y subiendo el resultado, sin
cambiar su firma ni su contrato.

- `pos`/`rot`/`scale`: bucle externo sobre clips, interno sobre huesos. Los offsets ya son
  absolutos al vector, así que la concatenación es meter el bucle actual dentro de otro; no
  hay lógica nueva de offsets.
- `boneInfos`: `C*B` entradas en layout `[clip][hueso]`; `parentIndex`/`inverseBindPose`
  idénticos en cada bloque de clip.
- Los buffers de tamaño 0 se siguen rellenando con una entrada dummy (Vulkan no acepta
  buffers vacíos), igual que hoy en `Renderer.cpp:1895-1897`.
- `SkinnedRenderObject` gana **dos campos**: `uint32_t activeClip = 0` y `uint32_t clipCount = 1`.
  `clipCount` existe para que `setAnimationState` clampe un `clipIndex` fuera de rango a 0: sin
  el clamp, `clipBase` apuntaría fuera del SSBO de `BoneInfos` y el compute leería basura sin
  que nada avise. Sus escalares
  `duration`/`ticksPerSecond` se quedan como están (los del clip 0): su único consumidor es
  `updateAnimation`, que solo corre en el caso sin Animator. Cuando hay Animator, `animTime`
  llega ya calculado por `setAnimationState` y el Renderer no necesita saber la duración de
  ningún clip. El per-clip (`duration`/`ticksPerSecond`/`loop`) lo lee `bindClips` del
  `SkinnedMesh`, que es su fuente de verdad.

### API del Renderer

- **Nueva:** `void setAnimationState(int index, uint32_t clipIndex, float animTime)` — sink
  puro, no avanza tiempo.
- **Intacta:** `updateAnimation(int index, float dt)` — sigue avanzando `animTime` con loop
  incondicional sobre el clip 0, para objetos **sin** Animator.

Los dos caminos no se pisan: un objeto con Animator nunca pasa por `updateAnimation`.

### Slot en `GameObject`

`GameObject.h` gana el slot, calcado del de `CameraComponent` (`GameObject.h:96-98`):

```cpp
void setAnimator(std::shared_ptr<AnimatorComponent> a) { m_animator = std::move(a); }
const std::shared_ptr<AnimatorComponent>& getAnimator() const { return m_animator; }
bool hasAnimator() const { return m_animator != nullptr; }
```

Sin invariante de unicidad por escena (a diferencia de la cámara): cada GameObject skinned
puede tener el suyo. El clone de `Scene` lo copia como cualquier otro componente.

### `AnimatorComponent`

Vive en `engine/include/DonTopo/Core/AnimatorComponent.h` + `engine/src/Core/AnimatorComponent.cpp`.
Mismo sitio y misma regla que `CameraComponent`: data + lógica pura, sin Vulkan, sin conocer
`GameObject`. `GameObject.h` ya incluye `SkinnedMesh.h`, así que no arrastra include nuevo.
Módulo nuevo no se justifica para 2 ficheros.

```cpp
class AnimatorComponent
{
    public:
        enum class ConditionType { Bool, Trigger, AnimationFinished };
        enum class ParamType     { Bool, Trigger };

        struct Condition {
            ConditionType type;
            std::string   paramName;   // vacío si AnimationFinished
            bool          expected;    // solo Bool
        };
        struct Transition {
            int fromState, toState;
            std::vector<Condition> conditions;   // AND de todas
        };
        struct State {
            std::string name;
            std::string clipName;                // resuelto a clipIndex por bindClips
            int         clipIndex      = -1;     // índice en SkinnedMesh::animationClips
            float       duration       = 0.0f;   // ticks, cacheado de bindClips
            float       ticksPerSecond = 24.0f;  // cacheado de bindClips
            bool        loop           = true;   // autoría del usuario, NO cacheado
            glm::vec2   editorPos{0.0f};
        };

        // Diseño
        int  addState(State s);
        void addTransition(Transition t);
        void setEntryState(int idx);
        void addParameter(const std::string& name, ParamType type);
        void bindClips(const SkinnedMesh& mesh);

        // Runtime
        void  setBool(const std::string& n, bool v);
        bool  getBool(const std::string& n) const;
        void  setTrigger(const std::string& n);
        void  update(float dt, bool evaluateTransitions);
        int   currentState() const;
        int   currentClipIndex() const;
        float animTime() const;   // ticks
        bool  finished() const;
        void  reset();
};
```

**Por qué `State` cachea `duration`/`ticksPerSecond`** en vez de consultarlos en el
`SkinnedMesh`: deja el componente auto-contenido y permite que el test headless construya
estados a mano sin Vulkan ni FBX. Los rellena `bindClips(const SkinnedMesh&, warnings)`, que
resuelve `clipIndex` por `clipName` y copia esos dos campos; se llama al cargar la escena y al
añadir el componente.

`loop` **no** lo toca `bindClips`: es autoría del usuario (checkbox en el nodo), no un dato
del FBX. Si `bindClips` lo sobrescribiera, recargar la escena descartaría lo que el usuario
marcó en el grafo.

**`update(dt, evaluateTransitions)`** — orden estricto:

1. `m_animTime += dt * state.ticksPerSecond`.
2. Si `m_animTime >= duration`: si `loop`, `fmod`; si no, clamp a `duration` y `m_finished = true`.
3. Si `!evaluateTransitions`, return.
4. Recorre las transiciones que salen del estado actual en orden de declaración. **La primera
   cuyo AND de condiciones se cumple, gana.** Determinista, sin prioridades explícitas.
5. Al cambiar de estado: `animTime = 0`, `m_finished = false`, y **consume los triggers de la
   transición ganadora** (solo esos).

**Triggers:** `setTrigger` los enciende; se apagan solo al consumirlos una transición. Si
ninguna los consume, permanecen encendidos (igual que Unity).

### Quién avanza el tiempo

El `AnimatorComponent`, en CPU. `main.cpp`, dentro del traverse ya existente
(`sandbox/src/main.cpp:301`):

```cpp
if (go->skinnedRenderIndex >= 0)
{
    if (auto* a = go->getAnimator())
    {
        a->update(dt, renderer.isPlaying());
        renderer.setAnimationState(go->skinnedRenderIndex, a->currentClipIndex(), a->animTime());
    }
    else renderer.updateAnimation(go->skinnedRenderIndex, dt);   // igual que hoy
    renderer.setSkinnedTransform(go->skinnedRenderIndex, go->worldTransform);
}
```

"Animation finished" lo detecta el `AnimatorComponent`, no el Renderer: el Renderer no sabe
de grafos, y `animTime` debe tener **un solo dueño** o se calcularía en dos sitios.

### Play vs Edit

El grafo evalúa transiciones **solo en Play** (`evaluateTransitions = renderer.isPlaying()`).
En Edit solo avanza el tiempo del estado de entrada, respetando su `loop`: si es `loop=false`
se queda en el último frame — el mismo resultado que daría Play sin transición que dispare,
sin caso especial en el código.

Sin esta separación, las condiciones `animation finished` harían pasear el grafo solo en el
editor (los triggers solo llegan desde Lua, que solo corre en Play).

### Sin Animator

Un skinned mesh sin `AnimatorComponent` reproduce el **clip 0 en loop**, en Edit y en Play,
exactamente como hoy. El componente es opt-in tras el botón Add. Cero regresión en la demo
del sandbox ni en escenas ya guardadas.

### Lua (`engine/src/Scripting/ScriptBindings.cpp`)

El binding sigue el patrón exacto de `Rigidbody` (`ScriptBindings.cpp:324-354`): un wrapper
`struct LuaAnimator { LuaEntity e; }`, un `new_usertype<LuaAnimator>("Animator", ...)`, y
acceso vía `GetComponent("Animator")` — **no** métodos sueltos colgados de `Entity`, que no
es como este motor expone ningún componente.

```lua
local anim = self.entity:GetComponent("Animator")
anim:SetBool("running", true)
anim:SetTrigger("jump")
if anim:GetState() == "Jump" then ... end
```

Métodos: `SetBool(name, v)`, `GetBool(name)`, `SetTrigger(name)`, `GetState()` → nombre del
estado actual. Las entradas correspondientes se añaden a `LuaApiReference.cpp` para el
autocompletado del Script Editor.

---

## Serialización

Bloque `"animator"` en `nodeToJson`/`nodeFromJson` de `engine/src/Core/Scene.cpp`,
**aditivo**: las escenas guardadas antes de este campo cargan igual y `version` sigue en 1
(mismo criterio que el bloque `"camera"`, `Scene.cpp:363`).

```json
"animator": {
  "entryState": 0,
  "parameters": [ {"name": "running", "type": "bool"},
                  {"name": "jump",    "type": "trigger"} ],
  "states": [
    {"name": "Idle", "clip": "Idle", "loop": true,  "pos": [0, 0]},
    {"name": "Run",  "clip": "Run",  "loop": true,  "pos": [250, 0]},
    {"name": "Jump", "clip": "Jump", "loop": false, "pos": [500, 0]}
  ],
  "transitions": [
    {"from": 0, "to": 1, "conditions": [ {"type": "bool", "param": "running", "expected": true} ]},
    {"from": 1, "to": 2, "conditions": [ {"type": "trigger", "param": "jump"} ]},
    {"from": 2, "to": 0, "conditions": [ {"type": "animationFinished"} ]}
  ]
}
```

- **`"clip"` es el nombre, no el índice.** El índice depende del orden de `mAnimations` en el
  FBX; reexportar el modelo lo baraja y el grafo apuntaría a clips equivocados en silencio.
  El nombre falla ruidoso: `bindClips` deja `clipIndex = -1` y empuja a `Scene::m_warnings`
  (el vector que ya usan `pruneExtraCameras` y el clone); el estado conserva su clip anterior
  o el 0.
- **`"from"`/`"to"` son índices** al array `states` del mismo JSON: self-contained, sin
  dependencia de assets externos.
- **Enums como string** (`"bool"`, `"trigger"`, `"animationFinished"`, y el tipo de
  parámetro): legible en un `.scene` editado a mano y estable si el enum crece por el medio.
  Mismo razonamiento que el comentario de `"mode"` en la cámara (`Scene.cpp:127`).
- **`"pos"`** es la posición del nodo en el canvas. Vive en el JSON de escena porque el grafo
  vive ahí; sin él, reabrir la escena esparce los nodos.
- **No se serializa estado runtime** (`currentState`, `animTime`, valores de parámetros,
  triggers pendientes). Dos consecuencias, ambas deseadas: el Stop de Play reconstruye la
  escena desde JSON (`EditorUI.cpp:144`, `reloadSceneFromJson(m_playSnapshot)`) → **reset
  automático al estado de entrada, sin código**; y guardar en mitad de Play no hornea estado
  transitorio en el fichero.

### Undo/redo

`Command.cpp` ya opera sobre snapshots JSON de subárbol (`subtreeToJson`/`insertFromJson`),
así que el grafo entra en undo/redo **sin comando nuevo** en cuanto está en `nodeToJson`.

Añadir/quitar el componente necesita `AnimatorComponentCommand`, calcado de
`CameraComponentCommand` (`Command.h:157`): resuelve el GameObject por `id` en cada
`execute()`/`undo()` (nunca puntero crudo) y conserva el estado para que un Add-undo-redo no
lo devuelva a los defaults.

---

## UI

### CMake raíz

Bloque nuevo tras ImGuiColorTextEdit, patrón calcado de `CMakeLists.txt:146-164`:
`FetchContent_Declare` + `FetchContent_Populate` (no `MakeAvailable`),
`add_library(imgui_node_editor STATIC ...)` con los `.cpp` de thedmd
(`imgui_node_editor.cpp`, `imgui_node_editor_api.cpp`, `imgui_canvas.cpp`, `crude_json.cpp`),
`target_link_libraries(... imgui_backend)`, `target_compile_features(... cxx_std_17)`.

### `AnimatorPanel`

`engine/src/Editor/AnimatorPanel.cpp` + header, registrado en `engine/CMakeLists.txt`, con
toggle en el menú View junto a los 6 paneles existentes.

Panel propio y no embebido en Properties porque el canvas de imgui-node-editor necesita
espacio con zoom/pan propio; dentro de la columna de Properties es inservible.

- Dibuja el grafo del GameObject seleccionado si tiene `AnimatorComponent`; si no, un mensaje.
- Nodo = estado: título, nombre del clip, checkbox `loop`.
- Link = transición. Doble clic en el link → popup de condiciones.
- Panel lateral: parámetros (Add/Remove, nombre, tipo).
- Menú contextual del nodo: `Set as Entry`. El estado de entrada se marca con borde verde.

### `PropertiesPanel`

Bloque Animator oculto tras el botón Add (Add-gate, igual que los colliders), con la lista de
parámetros y un botón "Open Animator" que abre el panel. El Add/Remove va por
`AnimatorComponentCommand`.

---

## Tests

`engine/tests/animator_tests.cpp`, registrado en `engine/tests/CMakeLists.txt`. Todos
headless, sin Vulkan.

Sobre el criterio 1 (N clips desde FBX): `assets/` solo tiene `model.fbx`,
`modelAnimation.fbx` y `modelTexture.fbx`, y es read-only por scope-lock, así que no se añade
un FBX multi-animación. Lo cubren los tests 8 y 9: el 8 ejercita el bucle sobre `mAnimations`
contra el asset real, y el 9 ejercita el camino N>1 con datos construidos a mano.

1. Un trigger cambia el estado activo; sin trigger, no cambia.
2. `AnimationFinished` no dispara en `duration - ε` y sí dispara pasado `duration`.
3. `loop=false` clampa a `duration` y se mantiene ahí tras varios `update`; `loop=true`
   reinicia (`animTime < duration` tras pasarse).
4. Condición `bool`: dispara con `expected` cumplido, no con el contrario.
5. El trigger consumido por la transición ganadora se apaga; uno no consumido sigue encendido.
6. Round-trip JSON del grafo: serializar → deserializar → nodos, `pos`, links, condiciones,
   parámetros, `loop` y `entryState` idénticos.
7. `bindClips` resuelve por nombre; nombre inexistente → `clipIndex = -1` + warning.
8. `ModelLoader::loadSkinned("assets/modelAnimation.fbx")`: **todo** clip cargado tiene nombre
   no vacío, `duration > 0` y `ticksPerSecond > 0`; nombres únicos entre sí.
9. `packSkinnedClips` sobre un `SkinnedMesh` construido a mano con 3 clips de keys conocidas:
   los offsets de `allPos`/`allRot`/`allScale` quedan concatenados sin solaparse y en el orden
   `[clip][hueso]`; `boneInfos` tiene `C*B` entradas; `parentIndex` e `inverseBindPose` son
   idénticos en todos los bloques de clip.

## Criterios de aceptación

1. Cargar un FBX con N animaciones deja N `AnimationClip` en el `SkinnedMesh`, con nombre,
   `duration` y `ticksPerSecond` correctos por clip.
2. El test headless de `engine/tests/` cubre trigger, `animation finished` (antes/después de
   `duration`) y loop on/off.
3. El grafo (nodos, posiciones, links, condiciones, parámetros, loop por clip, estado de
   entrada) sobrevive un ciclo guardar → cargar escena.
4. `configure.bat` + `build.bat` compilan limpio y la suite de tests pasa entera.
5. Verificación visual manual del panel de nodos: abrir el ejecutable → seleccionar el
   GameObject skinned → Properties → Add → Animator → View → Animator → crear 2 nodos,
   linkarlos, poner un trigger → guardar → recargar escena → los nodos siguen en su sitio.

## Ficheros tocados

| Fichero | Cambio |
| --- | --- |
| `CMakeLists.txt` (raíz) | Bloque FetchContent + `add_library(imgui_node_editor)` |
| `engine/CMakeLists.txt` | Registra `AnimatorComponent.cpp`, `SkinnedMeshPacking.cpp`, `AnimatorPanel.cpp`; linka `imgui_node_editor` |
| `engine/include/DonTopo/Renderer/SkinnedMesh.h` | `animationClip` → `animationClips` |
| `engine/include/DonTopo/Renderer/SkinnedMeshPacking.h` | **Nuevo** — `PackedClips`, `packSkinnedClips` |
| `engine/src/Renderer/SkinnedMeshPacking.cpp` | **Nuevo** — flatten extraído de `addSkinnedMesh` |
| `engine/src/Renderer/ModelLoader.cpp` | Bucle sobre `mAnimations`; nombres por defecto |
| `engine/include/DonTopo/Renderer/Renderer.h` | `pad` → `clipBase`; `activeClip`; `setAnimationState` |
| `engine/src/Renderer/Renderer.cpp` | `addSkinnedMesh` llama a `packSkinnedClips`; `recordComputePass` carga `clipBase`; `setAnimationState` |
| `shaders/bone_eval.comp` | `boneInfos.data[push.clipBase + bi]` (1 línea) |
| `engine/include/DonTopo/Core/AnimatorComponent.h` | **Nuevo** |
| `engine/src/Core/AnimatorComponent.cpp` | **Nuevo** |
| `engine/include/DonTopo/Core/GameObject.h` | Slot `m_animator` |
| `engine/src/Core/Scene.cpp` | Bloque `"animator"` en `nodeToJson`/`nodeFromJson`; `bindClips` al cargar |
| `engine/include/DonTopo/Editor/Command.h` + `engine/src/Editor/Command.cpp` | `AnimatorComponentCommand` |
| `engine/include/DonTopo/Editor/AnimatorPanel.h` + `engine/src/Editor/AnimatorPanel.cpp` | **Nuevo** |
| `engine/src/Editor/PropertiesPanel.cpp` | Bloque Animator tras Add-gate |
| `engine/src/Editor/EditorUI.cpp` | Registro del panel + toggle en menú View |
| `engine/src/Scripting/ScriptBindings.cpp` | `SetBool`/`GetBool`/`SetTrigger`/`GetAnimatorState` |
| `sandbox/src/main.cpp` | Rama Animator vs `updateAnimation` en el traverse |
| `engine/tests/animator_tests.cpp` + `engine/tests/CMakeLists.txt` | **Nuevo** |
| `README.md` | Documentar el componente y el panel |

`shaders/bone_hierarchy.comp` y `shaders/skinning.comp` **no se tocan**.

## Riesgos

- **Build stale de Ninja.** Este cambio toca `SkinnedMesh.h` y `Renderer.h`, headers muy
  incluidos. Un crash con puntero basura tras editarlos = sospechar `.obj` stale, borrar y
  reconstruir antes de tocar el código nuevo.
- **imgui-node-editor con `GIT_TAG master`.** Mismo riesgo que ImGuizmo e ImGuiColorTextEdit
  ya asumen en este repo: un push upstream puede romper el build. Se acepta por consistencia
  con el patrón existente.
