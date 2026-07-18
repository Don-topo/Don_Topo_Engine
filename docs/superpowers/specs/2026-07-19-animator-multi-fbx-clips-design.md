# Animator: clips de animación desde múltiples ficheros FBX

Fecha: 2026-07-19

## Problema

Hoy un `SkinnedMesh` se construye desde un único FBX (`ModelLoader::loadSkinned`) y
sus clips son exactamente los que ese fichero trae en `scene->mAnimations`. El
Animator solo puede referenciar esos clips.

Los pipelines habituales (Mixamo, Blender, packs de animación comerciales) exportan
cada animación en su propio FBX: un fichero con la malla y el esqueleto, y N ficheros
adicionales con una animación cada uno. Con el flujo actual esas animaciones son
inalcanzables.

## Objetivo

Poder añadir clips al Animator desde ficheros FBX distintos del que aportó la malla,
sin cambiar nada del comportamiento actual del Animator (máquina de estados,
parámetros bool/int/float, transiciones, evaluación GPU, bindings de Lua).

## No objetivos

- Blending entre clips (sigue habiendo un único clip activo por frame).
- Retargeting real entre esqueletos distintos (solo mapeo por nombre de hueso).
- Formato de animación propio precocinado (`.anim`).
- Importar animaciones desde formatos que no sean FBX.

## Contexto del código actual

- `SkinnedMesh::animationClips` — lista plana de clips; el Animator los resuelve
  **por nombre**, no por índice (`AnimatorComponent::bindClips`,
  `engine/src/Core/AnimatorComponent.cpp:197`).
- `Renderer::addSkinnedMesh` (`engine/src/Renderer/Renderer.cpp:1829`) empaqueta
  **todos** los clips con `packSkinnedClips` y sube los SSBOs una sola vez. El
  compute shader indexa con `clipBase = activeClip * boneCount`, layout
  `[clip][hueso]`.
- `ModelLoader::loadSkinned` (`engine/src/Renderer/ModelLoader.cpp:114`) ya ignora
  los canales cuyo hueso no está en `skel.boneMap` (línea 303) y ya deduplica
  nombres de clip con sufijo ` (N)` (líneas 280-293).
- La escena serializa el mesh con `sourcePath` + `skinned`, y lo reconstruye desde
  el FBX en cada carga (`engine/src/Core/Scene.cpp:426`). Play Mode usa ese mismo
  round-trip JSON.

## Diseño

### 1. Datos: fuentes de animación en el `SkinnedMesh`

En `engine/include/DonTopo/Renderer/SkinnedMesh.h`:

```cpp
struct AnimationSource
{
    std::string              path;      // .fbx de origen
    bool                     builtin;   // true = el FBX del propio modelo, no removible
    std::vector<std::string> clipNames; // nombres finales de los clips que aportó, en orden
};
```

`SkinnedMesh` gana `std::vector<AnimationSource> animationSources;`.

`animationClips` sigue siendo la única lista plana de clips. Las fuentes solo
registran **qué clip vino de dónde**, para poder listarlos agrupados en la UI y
poder quitar un fichero entero. Consecuencia: `packSkinnedClips`, el compute
shader, `clipBase`, `clipCount` y `bindClips` no cambian.

`loadSkinned` rellena `animationSources` con una única entrada `builtin = true`
cuyo `path` es el del propio FBX y cuyos `clipNames` son los clips que cargó.

### 2. Loader de solo animaciones

En `ModelLoader`:

```cpp
struct LoadedClips
{
    std::vector<AnimationClip> clips;
    std::vector<std::string>   warnings;
};

static LoadedClips loadAnimationClips(const std::string& path, const Skeleton& skel);
```

- Importa con flags mínimos: sin `aiProcess_Triangulate`, sin tangentes, sin
  procesado de materiales. No construye geometría.
- Recorre `scene->mAnimations` y, por cada `aiNodeAnim`, resuelve el hueso por
  nombre contra `skel.boneMap` — misma regla que ya aplica `loadSkinned`.
- El bucle de extracción de keyframes de `ModelLoader.cpp:299-329` se extrae a un
  helper estático de fichero que comparten `loadSkinned` y `loadAnimationClips`. La
  carga actual queda funcionalmente idéntica; solo se factoriza.
- Los nombres de los clips que devuelve son los internos del FBX; el renombrado a
  basename lo hace la capa de merge (§3), que es quien conoce los clips ya
  presentes en el mesh.

### 3. Merge: `Renderer/SkinnedMeshAnimations.{h,cpp}`

Archivo nuevo, sin dependencias de Vulkan, testeable en el binario de tests igual
que `SkinnedMeshPacking`:

```cpp
bool addAnimationSource   (SkinnedMesh& mesh, const std::string& path,
                           std::vector<std::string>& warnings);
void removeAnimationSource(SkinnedMesh& mesh, size_t sourceIndex);
bool renameClip           (SkinnedMesh& mesh, size_t clipIndex,
                           const std::string& newName);
```

**`addAnimationSource`**
1. Llama a `loadAnimationClips(path, mesh.skeleton)`.
2. Nombra cada clip con el basename del fichero (`walk.fbx` → `"walk"`). Si el
   fichero trae varios clips, el segundo y siguientes usan el sufijo ` (N)`. La
   deduplicación se hace contra **todos** los nombres ya presentes en
   `mesh.animationClips`, reusando la regla actual del loader.
3. Añade los clips a `animationClips` y una entrada nueva a `animationSources`.
4. Devuelve `false` sin tocar el mesh si el fichero se rechaza (§5).

**`removeAnimationSource`** borra de `animationClips` los clips listados en esa
fuente y quita la entrada. No hace nada si `sourceIndex` apunta a la fuente
`builtin`. Los índices de los clips restantes se recolocan solos: como los estados
referencian por nombre, el siguiente `bindClips` los vuelve a resolver.

**`renameClip`** rechaza nombre vacío o ya en uso. Actualiza el nombre en
`animationClips` y en el `clipNames` de la fuente correspondiente. Quien llama es
responsable de propagar el nombre nuevo a los estados del Animator (§4).

### 4. UI: Animator Panel

Sección plegable *Animation Sources* encima del grafo, en
`engine/src/Editor/AnimatorPanel.cpp`:

- Una fila por fuente: nombre del fichero, número de clips, botón `X`. El botón
  está deshabilitado en la fuente `builtin`.
- Botón `Add Animation FBX…` que abre un `IGFD::FileDialog` **propio del panel**
  (id `"AddAnimSrcDlg"`, filtro `.fbx`), siguiendo la pauta de
  `m_meshFileDialog` / `m_audioFileDialog` en `PropertiesPanel` — instancias
  separadas para que redimensionar un popup no toque el estado del otro. El
  diálogo se drena cada frame, independientemente de la selección actual, para que
  cambiar de GameObject con el diálogo abierto no deje el flag atascado.
- La sección es además drop target de `.fbx`, reusando el payload que ya emite el
  Content Browser.
- Cada fuente despliega su lista de clips. Doble clic sobre el nombre lo hace
  editable; al confirmar se llama a `renameClip` y, si tiene éxito, se reescribe
  `st.clipName` en todo estado del `AnimatorComponent` del GameObject que
  referenciara el nombre anterior.
- Errores y warnings se emiten al Log Console con `ctx.pushLog`; el último error se
  muestra además en rojo bajo la lista, como hace `m_meshLoadError`.
- El desplegable de selección de clip al crear un estado (`AnimatorPanel.cpp:542`)
  no cambia: lee `animationClips`, que ya incluye los importados.

Añadir fuente, quitar fuente y renombrar clip pasan por el sistema de comandos
undo/redo existente (snapshot JSON). El rename en particular lo necesita: sin él,
deshacer dejaría estados apuntando a un nombre que ya no existe.

### 5. Validación de esqueleto y errores

`addAnimationSource` cuenta los canales cuyo hueso existe en `skeleton.boneMap`
frente al total del fichero:

| Caso | Resultado |
|---|---|
| 0 canales casan | **Rechazo.** `"walk.fbx: ningún hueso coincide con el esqueleto de 'soldier' (0/65)"` |
| Casan algunos | **Importa con warning.** `"walk.fbx: 58/65 canales mapeados, 7 huesos desconocidos ignorados"` + hasta 5 nombres de hueso ignorados |
| Casan todos | Importa, sin warnings |
| Fichero sin animaciones | **Rechazo.** `"walk.fbx: no contiene animaciones"` |
| Fichero ilegible / Assimp falla | **Rechazo** con el mensaje de Assimp |
| Un clip concreto sin canales válidos | Se descarta ese clip; el resto del fichero se importa |
| `path` ya presente como fuente | Se permite; los nombres se deduplican con ` (N)`. El fichero puede haber cambiado en disco. |

Rechazo significa que el mesh queda **exactamente** como estaba: ni clips a medias
ni fuente registrada.

### 6. Refresco GPU

Los SSBOs de keyframes se suben una sola vez en `addSkinnedMesh`, así que cambiar
la lista de clips obliga a rehacerlos.

- El cuerpo actual de `addSkinnedMesh` se extrae a
  `initSkinnedRenderObject(SkinnedRenderObject&, const SkinnedMesh&)`.
- `addSkinnedMesh` pasa a ser `emplace_back` + `initSkinnedRenderObject`:
  comportamiento idéntico al de hoy.
- Se añade `void Renderer::rebuildSkinnedMesh(int index, const SkinnedMesh& mesh)`:
  `vkDeviceWaitIdle` → `destroySkinnedRenderObject` → `initSkinnedRenderObject`
  **en el mismo slot**. El `skinnedRenderIndex` del GameObject no cambia. Se
  conservan `transform`, `animTime` y `activeClip` del objeto anterior.

La UI llama a `rebuildSkinnedMesh` tras cada add/remove de fuente que haya
modificado `animationClips`. Un rename no toca los buffers.

### 7. Serialización

El bloque `"mesh"` de la escena gana un array; `sourcePath` y `skinned` no cambian:

```json
"mesh": {
  "sourcePath": "assets/soldier.fbx",
  "skinned": true,
  "animationSources": [
    { "path": "assets/soldier.fbx", "builtin": true,  "clips": ["Idle"] },
    { "path": "assets/walk.fbx",    "builtin": false, "clips": ["walk"] }
  ]
}
```

Al cargar (`Scene.cpp:426`):
1. `loadSkinned(sourcePath)` como ahora, que deja la fuente `builtin`.
2. Por cada fuente no-`builtin`, en orden, `addAnimationSource(path)`.
3. Los nombres guardados en `clips` se aplican en orden a los clips que cada fuente
   aportó, hasta `min(guardados, cargados)`. Así un rename sobrevive al round-trip
   y un FBX reexportado con más clips no rompe la carga.
4. Fuente cuyo fichero falta o es rechazado: error al Log y se continúa. Los
   estados que usaran sus clips quedan huérfanos, que es el comportamiento
   acordado (`bindClips` ya avisa y cae a `clipIndex 0`).

Escenas guardadas antes de este cambio no tienen `animationSources`: se cargan
igual que hoy y la fuente `builtin` se sintetiza desde `sourcePath`.

Play Mode se apoya en el mismo round-trip JSON, así que no necesita nada aparte.

## Tests

En `engine/tests/animator_tests.cpp`, sin Vulkan, como los actuales. El único
asset necesario es `assets/modelAnimation.fbx`, que ya usan; el caso multi-fichero
se cubre importándolo dos veces.

1. `loadAnimationClips(modelAnimation.fbx, skel_del_mismo_fbx)` devuelve los mismos
   clips que `loadSkinned`, con los mismos `boneIndex`, y 0 warnings.
2. El mismo fichero contra un `Skeleton` vacío: `addAnimationSource` devuelve
   `false`, emite warning y deja `animationClips` / `animationSources` intactos.
3. Contra un esqueleto al que se le han quitado huesos: importa, warning con el
   recuento correcto, canales sobrantes ignorados.
4. Importar el mismo fichero dos veces: nombres únicos
   (`"modelAnimation"`, `"modelAnimation (1)"`, …) y `animationSources.size() == 3`
   contando la `builtin`.
5. `removeAnimationSource`: desaparecen exactamente los clips de esa fuente, los
   demás conservan su orden relativo, y `packSkinnedClips` da `clipCount` y offsets
   coherentes con la lista nueva.
6. `renameClip`: actualiza el `clipName` de los estados que lo usaban; rechaza
   nombre vacío y nombre duplicado sin modificar nada.
7. Round-trip de escena: guardar con 2 fuentes y un clip renombrado, cargar, y
   comprobar nombres, orden y que `bindClips` resuelve todos los estados sin
   warnings.

## Lo que no cambia

Compute shader `bone_eval.comp`, `packSkinnedClips`, el layout `[clip][hueso]` de
los SSBOs, `AnimatorComponent::bindClips`, la máquina de estados, los parámetros
bool/int/float, las transiciones y los bindings de Lua.
