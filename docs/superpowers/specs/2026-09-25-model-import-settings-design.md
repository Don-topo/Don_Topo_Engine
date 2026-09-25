# Ajustes de importación por modelo (FBX) — Diseño

Fecha: 2026-09-25. Cierra `docs/assets-editor-audit.md` U8 (import settings por asset).
Texturas (`2026-09-24-texture-import-settings-design.md`) y audio
(`2026-09-24-audio-import-settings-design.md`) ya lo tienen, con el mismo sidecar; esta
spec añade los modelos.

## Objetivo

Que un modelo `.fbx` pueda llevar cinco ajustes **de fichero**, persistentes, editables
desde el Content Browser y respetados por el editor y por el juego exportado:

- **Escala**: factor uniforme, [0.001, 1000], 1 por defecto. Se hornea en la geometría al
  importar (FBX en cm frente a m).
- **Normales**: `file` (defecto: las del fichero, planas si faltan — lo de hoy), `smooth`
  (regenera siempre, suaves) o `flat` (regenera siempre, planas).
- **Recalcular tangentes**: sí por defecto (lo de hoy).
- **Voltear UVs**: sí por defecto (lo de hoy).
- **Importar animaciones**: sí por defecto (lo de hoy).

Semántica decidida con el usuario: son propiedades del **fichero**, no del objeto. El
`Transform.scale` del GameObject sigue siendo independiente y se multiplica encima; los
colliders no se re-dimensionan solos. Al pulsar **Aplicar** se **recargan en vivo todos
los objetos de la escena que usan ese FBX**, igual que Aplicar en texturas y en `.mat`
refresca a sus usuarios.

Fuera de alcance: undo del Aplicar (ver Riesgos), unir mallas / `JoinIdenticalVertices`,
`LimitBoneWeights`, ejes / conversión de handedness, ajustes por submalla, importar
más de la primera malla en estáticos (`load()` sigue leyendo solo `mMeshes[0]`), y
cambiar el umbral de ángulo de las normales suaves.

## Restricciones que salen del código

- **El formato del sidecar ya es extensible por tipo** (`Core/ImportSettings.cpp`): un
  `type` desconocido para el lector que pregunta da el defecto con aviso. Modelo añade
  `type: "model"`.
- **Lo genérico por ruta ya vale para modelos sin tocarlo**: `importSidecarPath`,
  `isImportSidecar` (el grid no lista `.import.json`), `moveImportSidecar` /
  `copyImportSidecar` / `removeImportSidecar` / `importSidecarConflict` y su uso en
  `moveAsset`, `renameAssetFile`, `removeAssetPath` e `importExternalAsset`.
- **Cuatro caminos llaman a `ModelLoader`** y todos deben ver los ajustes:
  `Scene.cpp` síncrono (`loadSkinned` `:2182`, `load` `:2287`), `AsyncAssetLoader.cpp:193`
  (`loadAuto`), `SkinnedMeshAnimations.cpp:56` (`loadAnimationClips`, fuentes externas)
  y el juego exportado (que precarga con `loadAuto` en paralelo). Si cada llamante tuviera
  que leer el sidecar y pasarlo, uno nuevo lo olvidaría en silencio.
- **Flags de Assimp hoy fijos** (`ModelLoader.cpp:85` y `:175`): `Triangulate | FlipUVs |
  GenNormals | CalcTangentSpace`. `loadAnimationClips` lee con flags `0` (`:443`): no
  construye geometría.
- **`aiProcess_GlobalScale` + `AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY`** (`ScaleProcess.cpp`,
  verificado en el código de Assimp del repo) escala los vértices, las claves de posición
  de toda animación, el `mOffsetMatrix` de cada hueso y la traslación de cada nodo. Vale
  por tanto para estático (que lee los vértices crudos) y para skinned (bind pose y
  animación consistentes) sin escalar a mano.
- **`load()` lee `ai->mNormals[i]` sin guarda** (`:107`): la rama `file` mantiene
  `aiProcess_GenNormals` (genera solo si faltan) para que nunca sean nulas. Las tangentes
  ya tienen guarda (`:108`, `ai->mTangents ? … : fallback`).
- **`loadSkinned` registra siempre una fuente de animación builtin** (`:360-369`), incluso
  sin clips; la UI la necesita como fila. `importAnimations = false` deja esa fuente con
  0 clips en vez de quitarla.
- **`rebuildStaticMesh` NO soporta cambiar vértices** (aviso en `EditorRenderer.h:111-115`):
  para geometría nueva hay que quitar y volver a registrar el objeto. La receta ya existe
  en `MeshComponentCommand::remove/put` (`Command.cpp:863-912`): `removeMeshComponent` →
  `setMesh` → `applyMaterialOverrides` → `addStaticMesh` / `addSkinnedMesh` →
  `flushUploadsAndWait`.
- **La configuración de fuentes de animación de un skinned vive inline en
  `Scene::fromJson`** (`Scene.cpp:2184-2250`): la fuente builtin solo recupera nombres de
  clip (`applyClipNamesPositionally`) y las externas se reañaden con `addAnimationSource`.
  Un reimport tiene que hacer exactamente lo mismo, y dos copias divergirían.
- **`Animator::rebindClips`** (`bindClips` sin `reset()`) existe para reengancharse tras
  cambiar las fuentes sin perder parámetros ni playhead.
- **Las mallas estáticas se comparten** entre clones y quien las edita las copia
  (`editMesh`); las skinned se copian por objeto. Un reimport carga **una vez por FBX** y
  reparte: compartida en estáticos, copia en skinned.

## Diseño

### 1. Modelo y sidecar (Core)

Se amplía `Core/ImportSettings.h/.cpp`:

```cpp
enum class NormalsMode : uint8_t { File, Smooth, Flat };
struct ModelImportSettings {
    float       scale            = 1.0f;          // [kModelScaleMin, kModelScaleMax]
    NormalsMode normals          = NormalsMode::File;
    bool        calcTangents     = true;
    bool        flipUVs          = true;
    bool        importAnimations = true;
};
inline constexpr float kModelScaleMin = 0.001f, kModelScaleMax = 1000.0f;
bool  operator==(const ModelImportSettings&, const ModelImportSettings&);
bool  isDefault(const ModelImportSettings&);
float clampModelScale(float);                      // NaN / <=0 -> 1
ModelImportSettings loadModelImportSettings(const std::filesystem::path& asset, std::string* warning = nullptr);
bool saveModelImportSettings(const std::filesystem::path& asset, const ModelImportSettings&, std::string* error = nullptr);
```

Fichero: `{ "version": 1, "type": "model", "scale": 1.0, "normals": "file",
"calcTangents": true, "flipUVs": true, "importAnimations": true }`.

- **Solo existe si difiere del defecto** (guardar el defecto lo borra), escritura por
  temporal + `rename`, igual que texturas y audio.
- **Lectura tolerante, nunca lanza**: ausente (sin aviso), JSON roto, tope de 64 KiB,
  `version` ≠ 1, `type` ≠ `model`, campo de tipo equivocado → su defecto y aviso. Un campo
  suelto malo deja los demás leídos. `scale` no finito, ≤ 0 o fuera de rango se **acota**
  (NaN y ≤ 0 → 1; el resto a [0.001, 1000]) con aviso: una escala 0 colapsaría la malla y
  una enorme reventaría el bounding box y el near/far. Un `normals` desconocido → `file`.
- **Cruce de tipos**: un sidecar `texture` o `audio` leído como modelo da el defecto con
  aviso, y al revés; el parseo tolerante y el tope se comparten con los otros lectores.

### 2. Consumo en `ModelLoader`

`load`, `loadSkinned` y `loadAnimationClips` llaman a `loadModelImportSettings(path)`
**ellos mismos**, así que los cuatro callers y el juego exportado lo heredan sin cambios.
Se lee una vez por llamada (un fichero de ~100 bytes); el aviso, si lo hay, sale por
`stderr` con el prefijo `[ModelImport]`.

- **Flags** = `Triangulate` siempre, más: `FlipUVs` si `flipUVs`; `CalcTangentSpace` si
  `calcTangents`; según `normals`: `File` → `GenNormals`; `Smooth` → `RemoveComponent`
  (con `aiComponent_NORMALS`) + `GenSmoothNormals`; `Flat` → `RemoveComponent` +
  `GenNormals`; y `GlobalScale` (con la propiedad `AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY` =
  `scale`) solo si `scale` ≠ 1. Sin sidecar las flags son **exactamente** las de hoy.
- **`loadAnimationClips`** aplica `GlobalScale` con **el sidecar del propio fichero de
  animación** si su `scale` ≠ 1 (sus claves de traslación tienen que estar en las
  unidades del esqueleto al que se mapean). El resto de sus ajustes no le afectan.
- **`importAnimations = false`** en `loadSkinned`: no se construyen clips; la fuente
  builtin se registra igual con 0 clips. Los estados de Animator que los usaran quedan
  huérfanos con el aviso de `bindClips` de siempre. No afecta a `load()` (no importa
  animaciones) ni a las fuentes externas (las añade el usuario a mano).
- `hasBones` y `loadAuto` no cambian: la decisión estático/skinned no depende de los
  ajustes. La textura derivada del FBX sigue siendo `dirname(fbx)/basename`.

### 3. Extracción: configurar fuentes de animación de un skinned

La lógica de `Scene.cpp:2184-2250` se extrae a una función compartida, sin cambio de
comportamiento:

```cpp
struct AnimationSourceConfig { std::string path; bool builtin = false; std::vector<std::string> clipNames; };
// Reaplica sobre una malla recién cargada las fuentes que tenía antes: la builtin recupera
// los nombres (posicionalmente), las externas se reañaden. Devuelve avisos, nunca lanza.
void applyAnimationSourceConfig(SkinnedMesh&, const std::vector<AnimationSourceConfig>&,
                                std::vector<std::string>& warnings);
```

`Scene::fromJson` parsea su JSON (con su validación y avisos actuales) a esa lista y
llama a la función; el reimport la construye leyendo `animationSources` de la malla vieja.
Los tests de escena y de Animator existentes cubren la no-regresión.

### 4. Aplicar en vivo

`applyModelImportSettings(sceneRoot, fbx, settings, reimport)` (función pura, análoga a
`applyMaterialAssetSettings`): guarda el sidecar (el defecto lo borra) y, **solo si se
pudo**, recorre los objetos con `hasMesh()` cuyo `sourcePath` sea ese FBX (`samePath`) y
llama a `reimport(go)`. Un fallo de escritura no reconstruye nada y se muestra en el
modal, que no se cierra. Sin `sceneRoot` o sin `reimport` solo escribe.

`reimport` en el panel (`ctx.renderer` puede ser `nullptr` en tests):

1. Si `pendingMeshJob != 0` (carga asíncrona en vuelo): se salta ese objeto y se avisa en
   el Log; cargará con los ajustes nuevos porque el loader los lee él mismo.
2. Captura de la malla vieja: `animationSources` (→ `AnimationSourceConfig`) si es
   skinned.
3. Carga **una vez por FBX** con `ModelLoader::loadAuto` y reparte (compartida en
   estáticos, `make_shared<SkinnedMesh>(copia)` en skinned); en skinned aplica
   `applyAnimationSourceConfig`. Una excepción de Assimp deja ese objeto **intacto** y da
   un aviso: nunca queda un objeto sin malla por un reimport fallido.
4. Receta de `MeshComponentCommand`: `removeMeshComponent` → `setMesh(nueva)` →
   `applyMaterialOverrides` → `addStaticMesh`/`addSkinnedMesh` → al final un único
   `flushUploadsAndWait`.
5. Si tiene Animator, `rebindClips(mesh, &avisos)`; los avisos van al Log.

### 5. UI (Content Browser)

`importSettingsKindFor` gana `ImportSettingsKind::Model` (`AssetKind::Model3D`, **una**
sola selección; hoy da `None`). El modal "Import Settings" muestra, para un modelo:
**Scale** (`DragFloat` con rango y formato `%.4f`), **Normals** (combo: Del fichero /
Suaves / Planas), y las casillas **Recalcular tangentes**, **Voltear UVs** e **Importar
animaciones**. Texto de ayuda: "Se aplica a todos los objetos que usan este modelo. Los
colliders no se re-dimensionan." Aplicar / Cancelar como en texturas y audio.

### 6. Ciclo de vida y export

Mover, renombrar, borrar, importar y listado: sin cambios (genérico por ruta).
**Exportador**: `collectSceneAssets` ya añade el FBX (y sus fuentes de animación) con
`add(...)`; pasan a `addWithSidecar(...)`. El runtime lee el sidecar por la misma ruta
dentro del paquete. Las fuentes de animación externas llevan **su propio** sidecar.

## Verificación

Tests planos con `CHECK`, desde la raíz del repo:

- **`import_settings_tests`**: ida y vuelta; defecto = sin fichero; JSON roto, versión,
  tipo, cruce texture/audio/model; `NaN`, `inf`, `0`, `-1`, `1e30` en `scale` acotados sin
  lanzar; `normals` desconocido → `file`; campo de tipo equivocado deja los demás;
  Unicode; escribir en carpeta inexistente da error; sidecar hostil (5 MB).
- **`model_loader_tests`** (nuevo): con `modelAnimation.fbx` (skinned) y un FBX estático
  del repo — `scale = 2` duplica exactamente el bounding box; `scale = 2` duplica las
  claves de traslación de un clip y el `invBind` de un hueso; sin sidecar el resultado es
  **idéntico byte a byte** al de antes (mismos vértices e índices); `normals = flat` da
  normales de cara (todas las de un triángulo iguales) y `smooth` no; `calcTangents =
  false` deja tangentes del fallback; `flipUVs = false` invierte `v` respecto a `true`;
  `importAnimations = false` deja 0 clips y la fuente builtin presente; un `.fbx`
  inexistente sigue lanzando con el mensaje de siempre.
- **`scene_tests` / `animator_tests`** (no regresión): la extracción a
  `applyAnimationSourceConfig` no cambia ningún resultado existente.
- **`content_browser_tests`**: `applyModelImportSettings` escribe y llama a `reimport`
  solo para los objetos de ese FBX (no para otros ni para procedurales); con fallo de
  escritura no llama; sin `reimport` solo escribe; `importSettingsKindFor(".fbx")` = Model.
- **`exporter_tests`**: el sidecar de un FBX y el de una fuente de animación viajan con su
  fichero; un FBX sin sidecar no añade nada.

Verificación manual del usuario (visual, Vulkan **y** D3D12): un FBX con Scale 0.01 se ve
100 veces más pequeño y un personaje skinned sigue animándose sin deformarse; Aplicar con
el modelo ya en escena lo reemplaza sin perder transform, material ni Animator; Flat vs
Suaves cambia el sombreado; mover y renombrar conservan los ajustes; el juego exportado
los respeta; cierre limpio y syncval de Vulkan con control positivo.

## Riesgos

- **`GlobalScale` frente a las unidades propias del importador FBX**: verificado en la
  fuente de Assimp que `ScaleProcess` escala vértices, claves y huesos, pero el importador
  FBX aplica su propio factor de unidades por defecto. Los tests de escala comprueban la
  **razón** con y sin ajuste sobre los FBX reales, no un valor absoluto.
- **Aplicar no es deshacible** (como texturas, audio y `.mat`), y el historial de undo
  puede guardar mallas de antes del reimport: un undo de un Delete restauraría el objeto
  con su geometría anterior. Limitación conocida; no se vacía el historial.
- **Mixamo**: personaje y animaciones son FBX distintos y cada uno usa su sidecar. Si se
  escala el personaje hay que poner el mismo factor en sus FBX de animaciones, o las
  traslaciones de raíz no casarán. Se documenta en el README.
- **Coste**: reimportar un skinned pesado (varias decenas de MB) bloquea el hilo del
  editor durante la carga, una vez por FBX y no por objeto.
- **Colliders y transform**: no se re-dimensionan con la escala del fichero; el usuario
  los ajusta a mano.
- **Ampliación futura**: `JoinIdenticalVertices`, `LimitBoneWeights`, ejes y más de una
  malla en estáticos quedan fuera; con este cierre el audit U8 no tiene más tipos de
  asset abiertos.
