# Auditoría del módulo de Animación — Fase 1: diagnóstico y backlog

Fecha: 2026-09-11. Solo lectura: no se ha tocado código ni se ha ejecutado
build, test ni harness. **Ninguna cifra de tiempo de esta auditoría está
medida**; donde aparece un número es un cálculo desde `sizeof`/layout y se
dice. Todo lo marcado **[sin verificar]** es sospecha sin comprobar contra el
repo.

Formato heredado de `docs/core-audit.md`. Rutas relativas a `engine/` salvo
`shaders/`, `runtime/`, `sandbox/` y `README.md`.

## 0. Correcciones al punto de partida

El encargo daba por existentes varias cosas. Contra el código:

| Afirmación del encargo | Qué hay de verdad | Evidencia |
|---|---|---|
| "parámetros float" | Hay **cuatro** tipos: Bool, Trigger, Int, Float, y condiciones con `Greater/Less/Equals/NotEquals`. | `include/DonTopo/Core/AnimatorComponent.h:33-39` |
| "root motion" | **No hay root motion.** `lockRootMotion` DESCARTA la traslación de la raíz (la devuelve a bind pose); no la pasa al GameObject. El propio header lo dice: "Esto NO es root motion". | `AnimatorComponent.h:109-119`, `shaders/bone_eval.comp:117-122` |
| "reloj doble" | Existe, y es el del **cross-fade** (`m_animTime` + `m_prevAnimTime`). No es un reloj doble Animator/Renderer: `Renderer::updateAnimation` solo corre en la rama SIN Animator. | `src/Core/AnimatorComponent.cpp:480-501`; `runtime/main.cpp:679-696`; `sandbox/src/main.cpp:1099-1125` |
| "concatenación multi-clip en GPU" | Existe: `BoneInfos` en layout `[clip][hueso]`, el clip se elige con `clipBase = activeClip * boneCount` en el push constant. | `src/Renderer/SkinnedMeshPacking.cpp:21`, `src/Renderer/Passes/SkinningPass.cpp:190-192` |
| "skinning por compute con SSBO por modelo" | Tres dispatches por personaje (bone_eval → bone_hierarchy → skinning). Que el SSBO sea **por modelo** (compartido entre instancias) y no por instancia es **[sin verificar]**: `packSkinnedClips` se llama al registrar cada malla (`src/Renderer/Renderer.cpp:3739`, `src/Renderer/D3D12/D3D12Renderer.cpp:3328`) y no se comprobó si hay dedup. | `SkinningPass.cpp:199-229` |

Y una corrección al mapa del subagente: afirmó que en runtime el reloj lo
lleva el Renderer "sin llamar a `AnimatorComponent::update`". Es falso: los
tres hosts llaman a `anim->update` y usan `updateAnimation` solo en el `else`.

## 1. Mapa del módulo

| Pieza | fichero:línea | Dueño / quién la usa |
|---|---|---|
| `AnimatorComponent` (grafo + reloj + parámetros) | `include/DonTopo/Core/AnimatorComponent.h:27-309` | `GameObject`; sin Vulkan ni GameObject dentro (`:14-16`) |
| `State`, `Transition`, `Condition`, `Parameter` | `AnimatorComponent.h:71-120`, `:53-69`, `:41-51`, `:122-126` | — |
| `update` (reloj, cross-fade, transiciones) | `src/Core/AnimatorComponent.cpp:472-541` | `runtime/main.cpp:681`, `sandbox/src/main.cpp:520` (D3D12), `:1106` (Vulkan) |
| Pose que sale a la GPU (`poseClipA/B`, `poseTimeA/B`, `poseWeight`, `poseLockRootMotion`) | `AnimatorComponent.cpp:254-299` | los mismos tres hosts → `setAnimationBlend` |
| `bindClips` / `rebindClips` | `AnimatorComponent.cpp:330-378` | `src/Core/Scene.cpp:2726` (carga), `src/Editor/AnimatorPanel.cpp:280`, `Command.cpp` (AnimationSourceCommand) |
| `AnimationClip`, `BoneChannel`, `Skeleton`, `AnimationSource` | `include/DonTopo/Renderer/SkinnedMesh.h:23-68` | `SkinnedMesh : Mesh` (`:70-82`) |
| `GpuBoneInfo` (160 B, espejo std430) | `SkinnedMesh.h:101-124` | `bone_eval.comp:9-16`, `bone_hierarchy.comp` |
| Fuentes de animación (añadir/quitar/renombrar clips) | `src/Renderer/SkinnedMeshAnimations.cpp:52-224` | AnimatorPanel vía `AnimationSourceCommand`, `ClipRenameCommand` |
| Interfaz de backend | `include/DonTopo/Renderer/EditorRenderer.h:179-189` | `Renderer.cpp:3953-4000`, `D3D12Renderer.cpp:9897-9992` |
| Pase de skinning Vulkan | `src/Renderer/Passes/SkinningPass.cpp:171-239` | 3 dispatches + 3 barreras por personaje |
| Pase de skinning D3D12 | `D3D12Renderer.cpp:3595` (dispatch de bone_eval) | resto de líneas **[sin verificar]** |
| Shaders | `shaders/bone_eval.comp` (182 l.), `shaders/bone_hierarchy.comp`, `shaders/skinning.comp` (59 l.) | — |
| Serialización | `Scene.cpp:518-583` (`animatorToJson`), `:589-707` (`animatorFromJson`) | no guarda estado de runtime (tiempo, valores) |
| Lua | `src/Scripting/ScriptBindings.cpp:1493-1518` (12 métodos) | `LuaApiReference.cpp:187-191`, `:674-686`; `README.md:676-704` |
| Panel | `src/Editor/AnimatorPanel.cpp` (990 l.): parámetros `:115-189`, grafo `:193-491`, condiciones `:493-654`, fuentes `:657-852` | — |
| Tests | `tests/animator_tests.cpp` | cargador, packing, grafo, serialización, triggers, `animation finished` |

## 2. Tablas de hallazgos

### A. Arquitectura: acoplamientos, dueños, obligaciones del llamante, estados imposibles

| ID | Severidad · Estado | fichero:línea | Evidencia | Arreglo | Talla |
|---|---|---|---|---|---|
| A1 | Media · ABIERTO | `runtime/main.cpp:679-696`, `sandbox/src/main.cpp:518-536` y `:1099-1125` | El pegamento Animator→Renderer está **copiado en tres hosts**: `anim->update(...)`, `setAnimationBlend` con **7 argumentos** sacados uno a uno de `pose*()`, y el `else` con `updateAnimation`. Es una obligación del llamante triplicada: cualquier campo nuevo de la pose (capas, root motion, velocidad) exige tocar los tres sitios, `EditorRenderer.h:189` y los dos backends. Uno olvidado compila y falla en silencio en un backend. | Un `struct AnimationPose` devuelto por el Animator y **un único** helper (`applyAnimator(go, renderer, dt, evaluate)`) que llamen los tres hosts. | S-M |
| A2 | **Alta** · **CERRADO** (2026-09-12, `2e06fc0`..`f14cf35`) | `src/Editor/AnimatorPanel.cpp:171, 185, 302, 305, 380, 422, 435, 480, 506, 549` | Las ediciones del grafo **no pasan por el undo**: quitar o añadir parámetros, `blendMin/Max`, crear o borrar transiciones, borrar estados, Set as Entry, duración del cross-fade y umbral de condición mutan el componente directamente. `grep Command` en el fichero solo encuentra `:649` y `:800` (`AnimationSourceCommand`) y `:747` (`ClipRenameCommand`). Incumple la regla del repo "los cambios del editor pasan por el undo". | `AnimatorGraphCommand` con snapshot antes/después del grafo (el formato ya existe en `animatorToJson`, `Scene.cpp:518`), y el drag agrupado en un solo comando al soltar. **Cerrado, pero no envolviendo los 22 sitios uno a uno**: el panel abre y cierra una sesión en su propio `draw` (`AnimatorGraphUndoTracker`) y compara el grafo con la clave del `.scene` (`animatorGraphKey` = lo que emite `animatorToJson` menos `pos`). Así, cualquier campo que se guarde entra en el undo venga del sitio que venga, incluidos los que se añadan después, y no queda ninguna obligación por sitio. El comando (`AnimatorGraphCommand`) aplica solo lo autorado con `applyGraph`, que conserva valores de parámetros y playhead. | M |
| A3 | Media · ABIERTO | `src/Core/AnimatorComponent.cpp:256` y `:267`; límite escrito en `AnimatorComponent.h:216-218` | Durante un cross-fade cada lado aporta **su clip primario** (`poseClipA → previousClipIndex`), así que salir de un estado con blend (p. ej. walk/run a peso 1 = run) hace que la pose **salte al primario (walk)** en el primer frame del fade. Entrar a un estado con blend da el salto contrario al acabar el fade. La causa es que en el push constant solo caben dos clips. El salto visual es **[sin verificar]** en pantalla; la lógica está verificada en el código. | Buffer de pose (resultado de la evaluación guardado entre pases) o 4 clips en la evaluación. Es el mismo prerrequisito que las capas y el 2D (C3, C4). | L |
| A4 | Baja · ABIERTO | `AnimatorComponent.cpp:515-525` | Una transición que dispara con un cross-fade ya en vuelo **descarta** la mezcla anterior: la pose salta del estado mezclado al estado origen puro. Está documentado como decisión. | El mismo buffer de pose que A3: congelar la pose actual como fuente A. | L (con A3) |
| A5 | Media · ABIERTO | `AnimatorComponent.cpp:401`, `:413-415` y `:446-448` | Un estado **con loop** no puede salir por tiempo: `advanceClock` hace `fmod` y **nunca** pone `finished`, así que `AnimationFinished` no dispara jamás. Una transición sin condiciones tampoco dispara (`:401`). No hay exit time. | `exitTime` normalizado en `Transition` (campo al final del struct, desactivado por defecto), evaluado contra la fase del ciclo actual. | S-M |
| A6 | Baja · ABIERTO | `AnimatorComponent.h:109-119`, `shaders/bone_eval.comp:117-122` | No existe root motion: `lockRootMotion` descarta el desplazamiento. Un "correr" exportado con traslación solo se puede reproducir en el sitio, y el GameObject hay que moverlo desde script a una velocidad que no sale del clip. | Ver C12. | M |
| A7 | Media · ABIERTO (evaluación del Apéndice B) | `src/Core/Scene.cpp:3085-3091` → `:1759` | `cloneGameObject` siembra `mallas` con el `Mesh` vivo y `nodeFromJson` hace `std::make_shared<SkinnedMesh>(*sk)`: una **copia profunda** del SkinnedMesh por clon, con `skinnedVertices`, `materials` y **todos los `animationClips` con sus keyframes** (`SkinnedMesh.h:72-82`). Es el Apéndice B visto desde la animación, y más ancho que allí: los clips también son asset compartible y también se copian. **No medido.** | **No se reabre todavía**: el criterio del Apéndice B pide un problema medido, y aquí no lo hay. Se mide primero (backlog #11); si el Instantiate de un personaje sale caro o la RAM de clips duplicados aparece en un perfil, el arreglo es el mismo: geometría y clips en `shared_ptr` inmutable, y `Material` por objeto. | S (medir) / L (arreglar) |
| A8 | Baja · ABIERTO | `Scene.cpp:3446-3447` → `:1762` | El undo de un Delete (`insertFromJson`) pasa `preloaded=nullptr`, así que un skinned borrado se reconstruye con `ModelLoader::loadSkinned` **síncrono desde disco**. H14 cerró ese camino en el clon; el undo se quedó fuera. Solo afecta al editor. Coste **no medido**. | Sembrar la caché desde el loader asíncrono o desde una caché de mallas del editor. | S |
| A9 | Baja · ABIERTO [sin verificar] | `shaders/skinning.comp:47-52` y `:57-58` | Un vértice con los 4 pesos a 0 da `skin = mat4(0)` y **colapsa al origen**. No se ha comprobado si `ModelLoader` garantiza que los pesos sumen más de 0. Las normales se transforman con `skin` y no con su inversa transpuesta, lo que es incorrecto si un hueso tiene escala no uniforme. | Comprobar los pesos en el cargador y usar identidad si suman 0; el problema de las normales solo si hay rigs con escala. | S |
| A10 | Baja · ABIERTO | `AnimatorComponent.h:144-145` | `statesMutable()` y `transitionsMutable()` exponen los vectores enteros a la UI: se puede escribir un `toState` fuera de rango. Mitigado en lectura (`AnimatorComponent.cpp:510`) y en carga (test `test_animator_out_of_range_transition_is_dropped`, `tests/animator_tests.cpp:781`). | **Corrección (2026-09-12)**: **NO se cerró con A2**. El undo del grafo no va por un comando por sitio, sino por diff de snapshots, así que `statesMutable()` y `transitionsMutable()` siguen expuestos igual; lo que cambia es que todo lo que escriben queda registrado en el undo. | S |
| A11 | Media · ABIERTO (2026-09-12) | `src/Editor/EditorUI.cpp:1175-1176` | `handleUndoRedoShortcut` sale de inmediato si `m_isPlaying`, y ese atajo es el **único** llamante de `UndoManager::undo()`/`redo()` (no hay entrada de menú): **en Play no se puede deshacer nada**. | Salió al cerrar A2. La spec del undo del grafo justificaba conservar valores de parámetros y playhead "porque corre en Play", y esa garantía hoy es inalcanzable; donde sí vale es en Edit, porque el preview avanza el reloj y los valores de parámetros del panel están vivos. `applyGraph` la conserva de todos modos, así que levantar el gate no obligaría a tocar el Animator. Decidir si el gate se queda (y documentarlo) o se levanta. | S |

### B. Eficiencia: coste por frame en CPU y GPU, asignaciones, copias

**Nada de esta tabla está medido.** Cada fila da el mecanismo con su línea; la
columna "Plan de medición" dice cómo se cierra (harness headless + ablación,
ver memoria `runtime_headless_perf_harness`).

| ID | Severidad · Estado | fichero:línea | Evidencia | Plan de medición / arreglo | Talla |
|---|---|---|---|---|---|
| B1 | Media · SIN MEDIR | `shaders/bone_hierarchy.comp:2`, `:38-47`; `SkinningPass.cpp:216` | `local_size_x = 1` y `vkCmdDispatch(cmd, 1, 1, 1)`: la jerarquía se recorre en **un solo hilo de GPU**, en serie sobre `boneCount`, con dos bucles. Es latencia pura por personaje, y con N personajes se suma porque cada uno va detrás de su barrera (B2). | Medirlo con el harness y N = 1, 10 y 50 personajes, ablacionando este pase (poniendo `finalBones` a identidad). Si pesa: evaluar por niveles de profundidad, o un workgroup con memoria compartida. | M |
| B2 | Media · SIN MEDIR | `SkinningPass.cpp:171-239` | Por personaje visible: 3 `vkCmdDispatch`, 3 `vkCmdPushConstants`, 3 binds de pipeline y **3 barreras** dentro del bucle. Las barreras serializan a todos los personajes: el bone_eval del personaje 2 espera al skinning del 1 sin necesidad. En D3D12 hay el mismo patrón por objeto (`D3D12Renderer.cpp:3595`); el resto de líneas **[sin verificar]**. | Reordenar en tres fases: todos los bone_eval, **una** barrera, todas las jerarquías, una barrera, todos los skinning. Medir con el harness y 50 personajes. Hay que mantener la paridad en D3D12. | M |
| B3 | Baja · SIN MEDIR | `shaders/bone_eval.comp:86`, `:96`, `:106` | La búsqueda de keyframe es **lineal** desde la primera key, por hueso, por canal y por frame: O(keys) con un clip largo, y **doble** durante un cross-fade (`:176-177`). | Búsqueda binaria, o remuestreo a paso fijo al empaquetar (índice = `T / paso`). Medir con un clip largo (≥ 1.000 keys por canal) contra uno corto. | S-M |
| B4 | Baja · SIN MEDIR (cálculo) | `SkinnedMeshPacking.cpp:21`, `SkinnedMesh.h:124`, comentario de `bone_eval.comp:28-31` | `GpuBoneInfo` son 160 B en layout `[clip][hueso]`, pero `inverseBindPose`, `bindLocal` y `parentIndex` (132 de los 160 B) son **idénticos en todos los clips**, y el propio shader lo admite. Cálculo: 20 clips × 65 huesos × 160 B = 208 KB, de los que ~170 KB están repetidos. Es poca memoria, y más ancho de banda en el fetch de `BoneInfo`. | Separar un buffer de esqueleto (una entrada por hueso) del de offsets por clip. Prioridad baja. | S-M |
| B5 | — · DESCARTADO POR LECTURA | `AnimatorComponent.cpp:472-541`, `:396-437`, `:158-198` | CPU por frame: `update` no asigna nada. `getBool`, `getInt` y `getFloat` hacen `find` en `unordered_map<std::string>` (hash del nombre por condición), y `consumeTriggers` usa `operator[]` sobre claves que ya existen. Son unas pocas decenas de hashes por personaje. `currentStateName()` devuelve `std::string` por valor, pero solo lo llama Lua. | No se propone nada: misma vara que H12 y H13 del Core. Solo se reabriría si un perfil lo muestra. | — |
| B6 | Media · SIN MEDIR | ver A7 | Copia profunda del SkinnedMesh con todos sus clips por cada `Scene.Instantiate` de un personaje; en el lado GPU, `packSkinnedClips` se ejecuta por malla registrada (**[sin verificar]** si deduplica). | Backlog #11. | S (medir) |

### C. Funcionalidades frente a Unity Mecanim

| ID | Funcionalidad | Estado | Evidencia | Qué falta / nota de diseño | Coste |
|---|---|---|---|---|---|
| C1 | Parámetros bool / int / trigger | **EXISTE** | `AnimatorComponent.h:34`, setters en `:173-182`, `consumeTriggers` en `AnimatorComponent.cpp:430-437` | Falta `ResetTrigger` en Lua (ver D). | — |
| C2 | Blend tree 1D | **PARCIAL** | `State::blendClipName/blendParam/blendMin/blendMax` (`AnimatorComponent.h:87-99`); fase normalizada en `AnimatorComponent.cpp:272-284` | Solo **2 clips** por estado. Un 1D de N clips con umbrales **cabe en la GPU actual**: en cada instante solo se mezclan los dos vecinos, así que no hace falta tocar el push constant. | S-M |
| C3 | Blend tree 2D | **FALTA** | — | Necesita 3 o 4 clips simultáneos, que no caben en el push constant (`bone_eval.comp:24-43`). Depende del buffer de pose (A3). | L |
| C4 | Capas con máscara de avatar y peso | **FALTA** | — | Evaluación por capa, máscara por hueso y mezcla override/additive. Mismo prerrequisito que C3. Añade campos a la pose, así que A1 va antes. | L |
| C5 | Any State | **FALTA** | `AnimatorComponent.cpp:509` exige `t.fromState == m_currentState` | `fromState = -2` (centinela), con un flag que decida si puede transicionar a sí mismo. Afecta a la serialización y al canvas (nodo especial). | S |
| C6 | Exit time | **PARCIAL** | `AnimationFinished` (`AnimatorComponent.cpp:413-415`) | Solo al final de un clip sin loop; en un estado con loop no existe (A5). | S-M |
| C7 | Crossfade por código | **FALTA** | Lua solo expone parámetros y consultas (`ScriptBindings.cpp:1493-1518`) | `crossFade(stateName, seconds)` y `play(stateName)` en el core, reutilizando el bloque de `AnimatorComponent.cpp:515-538`; más binding, referencia y README. | S |
| C8 | Animation events | **FALTA** | `AnimationClip` no tiene eventos (`SkinnedMesh.h:33-39`) | Eventos por clip, **del usuario** (no del FBX: se reconstruye en cada carga, igual que `loop`, ver `AnimatorComponent.h:83-85`). Hay que dispararlos al cruzar el tiempo, contando el wrap del loop y los dt que saltan más de un ciclo, y entregarlos a Lua. | M |
| C9 | Curvas de clip | **FALTA** | Solo hay canales de hueso pos/rot/scale (`SkinnedMesh.h:26-31`) | Curvas float con nombre que escriban un parámetro. Con C8 y C13 comparten el modelo de "pista de clip no-hueso". | M |
| C10 | Sub-máquinas de estados | **FALTA** | `m_states` es un vector plano (`AnimatorComponent.h:284`) | Anidamiento y entradas/salidas de la sub-máquina; toca el canvas y la serialización. | L |
| C11 | Velocidad por estado | **FALTA** | `advanceClock` usa solo `ticksPerSecond` (`AnimatorComponent.cpp:443`) | `State::speed` y `speedParam` opcional (multiplicador). Con velocidad negativa hay que definir qué es `finished`. | S |
| C12 | Root motion real | **FALTA** | A6 | Los keyframes están en CPU (`SkinnedMesh::animationClips`), así que el Animator puede muestrear el canal de la raíz y dar un delta por frame al GameObject. La GPU sigue usando el bloqueo. Hay que resolver el wrap del loop, el cross-fade (mezclar deltas) y la interacción con Rigidbody. | M |
| C13 | IK (look-at, dos huesos) | **FALTA** | La jerarquía se calcula en GPU (`bone_hierarchy.comp:38-47`) | La IK necesita posiciones de mundo **antes** de componer las matrices finales: o un pase de compute entre la jerarquía y el skinning, o evaluar la jerarquía en CPU para las cadenas afectadas. Riesgo alto de paridad entre Vulkan y D3D12. | L |
| C14 | Clips de propiedades (Transform/material sin esqueleto) | **FALTA** | El Animator se ofrece gris en objetos no skinned (`README.md:680-681`); `bindClips` exige `SkinnedMesh` (`AnimatorComponent.h:152`) | Es otro tipo de clip, evaluado en CPU y escrito en el componente destino; el Animator deja de depender de `SkinnedMesh`. Toca serialización y UI de tracks. | L |

### D. UI de ImGui (AnimatorPanel) y API de Lua frente al core

| ID | Estado | fichero:línea | Hueco |
|---|---|---|---|
| D1 | **CERRADO** (2026-09-12, `2e06fc0`..`f14cf35`) | ver A2 | **Undo**: ninguna edición del grafo lo usaba (sí las fuentes y el rename de clip). Ahora todas, con un comando por gesto: un drag es un solo paso de Ctrl+Z. Mover nodos por el canvas se deja fuera a propósito. |
| D2 | OK (no es hueco) | `AnimatorPanel.cpp:302`, `:305`, `:506`, `:549` | `blendMin/Max`, la duración del cross-fade y el umbral usan `DragFloat` sobre el dato vivo y surten efecto en vivo: la regla de `DeferredSliderFloat` (solo para sliders sin efecto en vivo) no aplica. Lo que les falta es el undo (D1), agrupando el drag en un comando al soltar. |
| D3 | OK | `AnimatorPanel.cpp:128`, `:231`, `:520`, `:573`, `:683-684`, `:706` | PushID por parámetro, estado, condición y fuente. |
| D4 | ABIERTO | `ScriptBindings.cpp:1493-1518` | Lua tiene 12 métodos (Set/Get de Bool, Int y Float, SetTrigger, GetState, GetPreviousState, IsBlending, GetBlendWeight, GetPoseWeight). **Faltan, aunque el core ya los soporta**: `ResetTrigger` (el core consume triggers, pero no hay forma de desarmarlos desde código) y el tiempo o fase del estado (`animTime()` y `finished()` son públicos en `AnimatorComponent.h:190-191` y no están expuestos). **Faltan porque el core tampoco los tiene**: `Play`, `CrossFade`, `SetSpeed` (C7, C11). |
| D5 | ABIERTO | `README.md:35` | La lista de features dice "`bool`/`trigger`/`animation finished` conditions … **instant-cut transitions (no blending)**". Es falso desde el cross-fade y los parámetros Int/Float; contradice a `README.md:704` del mismo fichero. |
| D6 | [sin verificar] | `LuaApiReference.cpp:187-191`, `:674-686` | El subagente dice que están documentados "los 11 bindings", pero el binding tiene 12. No se ha contado a mano: hay que comprobar si falta uno en el autocompletado. |
| D7 | [sin verificar] | `AnimatorPanel.cpp:493-654` | Según el mapa, el nombre del parámetro de una condición se escribe en un `InputText` libre. Si no hay combo con los parámetros declarados, una errata deja la condición falsa para siempre sin avisar. Comprobar leyendo el rango. |
| D8 | [sin verificar] | `AnimatorPanel.cpp` | No se ha comprobado si el panel muestra en Play el estado actual y su progreso (tiempo normalizado, mezcla en curso). Unity lo pinta en el nodo. |

## 3. Backlog priorizado

Orden: primero lo que incumple una regla del repo o es prerrequisito de otras
filas, después las features baratas y, al final, lo que exige el buffer de
pose. Toda fila con binding Lua incluye `ScriptBindings.cpp` +
`LuaApiReference.cpp` + `## Lua Scripting` del README; toda fila de GPU,
Vulkan **y** D3D12; toda fila de UI, `Command` y `PushID`.

| # | Qué | Filas | Impacto | Coste | Riesgo de regresión | Test que lo demuestra |
|---|---|---|---|---|---|---|
| 1 | ✅ **HECHO** (2026-09-12, `2e06fc0`..`f14cf35`) — Undo del grafo del Animator (`AnimatorGraphCommand`, drag agrupado al soltar) | A2, D1, A10 | **Alto**: incumple una regla del repo y hoy un borrado de estado no se deshace | M | Medio: `editorId` e identidad de nodos en imgui-node-editor (`AnimatorComponent.h:102-108`); no debe pasar por `reset()` en Play (H4) | Aplicar y deshacer cada una de las 10 mutaciones de `:171…:549` deja `animatorToJson` idéntico al de antes; en Play, el undo conserva los valores de los parámetros. Sabotaje: quitar el push del comando y ver que cae el test de **esa** mutación. |
| 2 | Pegamento único Animator→Renderer (`AnimationPose` + helper) | A1 | Medio. Prerrequisito de 5, 8, 12, 13 y 14 | S-M | Bajo si el helper reproduce las 3 copias; el orden `setSkinnedMeshVisible` → animación importa (`runtime/main.cpp:674-677`) | Test con un `EditorRenderer` falso que graba llamadas: con y sin Animator, y con el mesh oculto. Criterio binario: `grep setAnimationBlend` en `runtime/` y `sandbox/` = 0. |
| 3 | Exit time + Any State | A5, C5, C6 | Alto: sin ellos no se sale de un estado con loop sin script | S-M | Bajo: campos al final de los structs, desactivados por defecto; hay que respetar la serialización por nombre | Un estado con loop y `exitTime=0.9` transiciona en el primer ciclo y no antes; Any State dispara desde cualquier estado y no hacia sí mismo sin el flag; una escena vieja carga sin cambios (`test_animator_valid_graph_loads_untouched`). |
| 4 | Velocidad por estado (+ parámetro multiplicador) + UI | C11 | Medio | S | Bajo | `advanceClock` con speed 2 → el doble de `animTime`; speed 0 → nunca `finished`; comportamiento definido y probado con speed negativa. |
| 5 | `Play` / `CrossFade` por código + `ResetTrigger` + tiempo normalizado en Lua | C7, D4 | Alto para gameplay | S | Bajo; hay que actualizar autocompletado y README (regla) | Core: `crossFade("Run",0.2)` → `blending()` y `previousStateName()` correctos, y un nombre inexistente no hace nada y avisa. Lua: script de test que llama a cada binding nuevo. |
| 6 | README desfasado + recuento de LuaApiReference | D5, D6 | Bajo (documentación) | S | Nulo | Revisión: `README.md:35` coherente con `:704`; 12 entradas de Animator en `LuaApiReference.cpp`. |
| 7 | Blend 1D de N clips con umbrales (sustituye al par `blendClip`) | C2 | Medio-alto (locomoción) | S-M | Medio: migrar escenas con `blendClipName` a un 1D de 2 entradas | Un parámetro entre los umbrales 2 y 3 da `poseClipA/B` = clips 2 y 3 con peso lineal; fuera de rango se clampa; carga de una escena vieja con `blendClip` → mismo `poseWeight`. |
| 8 | Animation events (con entrega a Lua) | C8 | Alto (pasos, golpes, sonidos) | M | Medio: el wrap del loop y los dt grandes | Un evento en t=0,5 dispara **exactamente una vez** por ciclo con dt=0,016, con un dt que cruza el wrap y con un dt que salta 2 ciclos (dispara 2). En el cross-fade, definido si dispara el estado que se apaga. |
| 9 | SkinningPass en 3 fases con 2 barreras en total (Vulkan + D3D12) | B2 | **Sin medir**: medir primero | M | Medio: barreras y sincronización (validación) | Captura del back buffer (memoria `backbuffer_readback_as_screenshot`) byte a byte igual antes y después con 10 personajes en los dos backends; y medición con 50 personajes y ablación. |
| 10 | Jerarquía de huesos en paralelo | B1 | **Sin medir**: medir primero | M | Medio | Readback de `finalBones` contra una referencia en CPU del mismo rig con tolerancia 1e-5; medición con el harness antes y después. |
| 11 | Medir el Instantiate de un skinned (ms y RAM) y el undo de un Delete de skinned | A7, A8, B6 | Decide si se reabre el Apéndice B | S | Nulo (solo medir) | Harness Release: 20 `Scene.Instantiate` de un personaje Mixamo → ms por clon y bytes de `animationClips` duplicados. **Criterio de reapertura** del Apéndice B: más de 1 ms por clon, o clips duplicados en un perfil. |
| 12 | Root motion real (delta de la raíz al GameObject) | A6, C12 | Alto (locomoción sin patinar) | M | Medio: el Rigidbody y el cross-fade; va después de #2 | Un clip con traslación X lineal: el GameObject avanza el delta del clip por ciclo, la raíz en GPU queda bloqueada y el wrap del loop no teletransporta; durante un cross-fade el delta es la mezcla ponderada. |
| 13 | Buffer de pose (evaluar la pose a un buffer, y mezclar buffers) | A3, A4 | Alto: prerrequisito de 14, C3 y C4 | L | **Alto**: rehace bone_eval en los dos backends | Continuidad: el máximo desplazamiento articular entre el último frame antes de una transición desde un estado con blend y el primero del fade es menor que ε (hoy falla por A3); lo mismo al interrumpir un fade (A4). |
| 14 | Capas con máscara de avatar y peso (+ blend 2D encima de #13) | C3, C4 | Alto | L | Alto | Capa 1 con máscara de brazo y peso 1: los huesos fuera de la máscara son idénticos a la capa 0 (readback) y los de dentro son iguales a la capa 1; peso 0 = solo la capa 0 exacta. |
| 15 | Diferidas hasta tener #13: IK, clips de propiedades, curvas, sub-máquinas | C9, C10, C13, C14 | Medio-alto | L cada una | Alto | Se definen al diseñarlas; la IK necesita resolver antes dónde vive la jerarquía (GPU hoy). |
