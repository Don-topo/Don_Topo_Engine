# Material como asset independiente (`.mat`) — Diseño

Fecha: 2026-09-24. Cierra el último punto abierto del audit del Content Browser
(`docs/assets-editor-audit.md`, "Asset de Material independiente": un `.mat`
reutilizable con su propio "Create > Material"; hoy el Material vive embebido en
`Mesh` / `GameObject`).

## Objetivo

Que un material sea un fichero del proyecto que varios objetos comparten:

- Se **crea** desde el Content Browser (clic derecho en el vacío → Create → Material)
  y se **edita** en un panel propio (doble clic sobre el `.mat`).
- Cada **slot de material** de un objeto (estático o skinned) puede **apuntar** a un
  `.mat`. Editar el `.mat` actualiza a **todos** los objetos que lo usan.
- Las overrides actuales del objeto (texturas, metallic, roughness, y la animación
  de metallic/roughness que las escribe) siguen mandando **encima**, y "Clear" de un
  slot vuelve al valor del `.mat`.
- Funciona en Vulkan y en D3D12, en el editor y en el juego exportado, y una escena
  sin ningún `.mat` se carga y se dibuja exactamente igual que hoy.

Decidido con el usuario: **referencia viva por slot** (no copia de valores) y
**edición en un panel propio con doble clic** (no a través del slot en Properties).

Fuera de alcance: color base, emisivo, tiling y alfa (`Material` no los tiene; serían
una feature de render aparte), presets, vista previa mientras se arrastra un slider,
bindings de Lua (no existen hoy), `.mat` fuera del proyecto, undo de la edición del
propio `.mat` (es un fichero, como los ajustes de importación) y **reescribir los
`.mat` cuando se renombra o mueve una textura** (ver Riesgos).

## Restricciones que salen del código

- **`Material` no tiene identidad y solo cinco campos llegan a la GPU**
  (`Renderer/Material.h:8-18`): `texturePath`, `normalMapPath`,
  `metallicRoughnessPath`, `metallic`, `roughness` (más tres vectores de bytes
  embebidos que este diseño no toca). Es un valor dentro de un `shared_ptr<const
  Mesh>` con copia al escribir (`GameObject::editMesh`, `GameObject.cpp:116`).
- **Las overrides ya son una capa** (`MaterialOverride`, `GameObject.h:83-100`) y
  `applyMaterialOverrides` (`GameObject.cpp:156-257`) las hornea sobre una copia de los
  materiales del mesh, con un mecanismo por campo `aplica(override, base, baseTomado,
  destino)`: override vacío → vuelve al baseline capturado (el valor del modelo);
  override presente → el baseline se captura una vez y se escribe el override.
  Solo escribe en la malla (y copia la compartida) si algo cambia. `-1.0` es el
  centinela de "sin override" en metallic/roughness.
- **Solo `mesh.materials` de la escena guarda las overrides** (`Scene.cpp:1179-1259`,
  lectura `:2379-2424`), como entradas dispersas por `index`, con rutas por
  `toStoredPath` / `fromStoredPath` (relativas a la raíz de assets, `Scene.cpp:1107`).
  Una entrada sin nada activo se omite. Los `base*` solo viajan en la ruta en memoria
  (clonar, undo).
- **`PropertyTracks` anima metallic/roughness escribiendo `materialOverrides[0]`**
  (`PropertyTracks.cpp:176-268`): la capa de override no puede desaparecer.
- **`setMesh` reinicia los `base*Taken`** (`GameObject.h:169-186`): la carga de escena
  depende de que las overrides se apliquen después de `setMesh`.
- **La carga asíncrona (Vulkan) pre-decodifica las texturas del FBX** en el worker
  (`AsyncAssetLoader.cpp:26`, `decodeSlot`) y `createSharedGpuMesh` **prefiere** una
  imagen ya decodificada sobre la ruta del material. `discardOverriddenDecodedImages`
  (`AsyncAssetLoader.cpp:40-57`) descarta los slots pisados por una override activa;
  un `.mat` que aporte una textura tiene que descartarlos igual, o se subiría a GPU la
  textura del FBX y no la del `.mat`.
- **Cambiar una textura de un objeto exige reconstruirlo**: `rebuildStaticMesh`
  (Vulkan `Renderer.cpp:4499`, D3D12 `:10641`) recalcula la clave, sube texturas nuevas
  y espera a la GPU; el skinned solo tiene `rebuildSkinnedMesh`, que reconstruye el
  personaje entero. Compartir un material hace ese coste proporcional al número de
  usuarios.
- **Ciclo de vida de referencias por ruta** (`ContentBrowserPanel.cpp`):
  `updateSceneReferencesForRename` (`:504`), `countSceneReferences` (`:639`) y
  `detachSceneReferencesForDelete` (`:667`) recorren la escena buscando rutas de
  malla, textura y audio. Un tipo de asset nuevo tiene que engancharse a las tres.
- **El exportador** recorre `materialsOf(go)` (materiales ya horneados) y añade las
  tres texturas con su sidecar (`GameExporter.cpp:184-280`); no lee
  `materialOverrides`. Reescribe las rutas de la escena exportada
  (`rewrite…`, con el test `test_rewrite_materials_override_outside_root`), y el
  runtime carga la escena con el mismo `Scene` de Core.
- **El Content Browser solo ofrece "Create Folder"** (`ContentBrowserPanel.cpp:1427`),
  y `.json` es `AssetKind::Scene` (`classifyAsset`, `:226`): `.mat` cae hoy en `Other`.
  El precedente más cercano de "fichero asociado a un asset" es el sidecar de
  importación (`Core/ImportSettings`), con su ciclo de vida por ruta.

## Diseño

### 1. Formato y módulo (Core)

`Core/MaterialAsset.h/.cpp`, sin GPU ni ImGui:

```cpp
struct MaterialAsset {
    std::string albedo, normal, orm;      // rutas ABSOLUTAS en memoria; "" = heredar del modelo
    float metallic  = -1.0f;              // -1 = heredar del modelo; si no, [0, 1]
    float roughness = -1.0f;
};
MaterialAsset loadMaterialAsset(const std::filesystem::path& mat, std::string* warning = nullptr);
bool saveMaterialAsset(const std::filesystem::path& mat, const MaterialAsset&, std::string* error = nullptr);
```

Fichero `.mat`:
`{ "version": 1, "type": "material", "albedo": "", "normal": "", "orm": "", "metallic": 0.5, "roughness": 0.5 }`.

- **Un campo ausente o vacío significa "heredar del modelo"** (`metallic` y `roughness`
  ausentes = `-1`): un `.mat` con solo `roughness` deja la textura del FBX.
- **Rutas de textura relativas a la carpeta del propio `.mat`** (`generic_string`, con
  `..` si hace falta): así el `.mat` se resuelve con solo su ruta, sin conocer la raíz
  del proyecto, y sigue funcionando en el paquete exportado, donde se conserva la
  jerarquía. Una textura que no se pueda expresar relativa (otra unidad) se guarda
  absoluta; en memoria las rutas son siempre absolutas.
- **Lectura tolerante, nunca lanza**: ausente, vacío, JSON roto, > 64 KiB, `version` ≠ 1,
  `type` ≠ `material`, campo de tipo equivocado → ese campo se hereda, con aviso.
  `metallic` y `roughness` se acotan a [0, 1]; no finitos → heredar.
- **Guardar SIEMPRE escribe el fichero**, aunque todo sea "heredar" (a diferencia del
  sidecar de importación, que borra el fichero en el defecto): un `.mat` es un asset
  con nombre, no un ajuste opcional. Escritura por fichero temporal + `rename`.
- Un `.mat` no es un `.import.json` ni lleva sidecar de importación propio; las
  texturas que nombra conservan los suyos.

### 2. Referencia y capas

`MaterialOverride` gana `std::string matAsset` (ruta absoluta en memoria, `""` = sin
material asset). En `applyMaterialOverrides`, por slot, se resuelve el `.mat`
(`loadMaterialAsset(ov.matAsset)`, una lectura por slot y aplicación) y la override
**efectiva** de cada campo es:

```
efectivo(campo) = override del objeto no vacío  ? override
                : valor del .mat no vacío       ? valor del .mat
                :                                  (vacío → baseline del modelo)
```

y se alimenta al mismo `aplica` / `aplicaFactor` de siempre. Por construcción:
override > `.mat` > modelo; **Clear** de un slot (override vacío) cae al valor del
`.mat`; desvincular (`matAsset = ""`) devuelve el baseline del modelo, porque el
baseline ya se capturó al pisar el slot por primera vez. Sin `matAsset` (y con
overrides o sin ellas) el resultado es **idéntico** al de hoy.

- **Un `.mat` inexistente o ilegible** cuenta como vacío (heredar todo) y da un aviso
  único por ruta: el objeto se dibuja con su modelo, nunca falla la carga.
- **Serialización** (`Scene.cpp`): `entry["matAsset"] = toStoredPath(ov.matAsset,
  assetRoot)` y lectura con `fromStoredPath`; una entrada con solo `matAsset` **cuenta
  como activa** (hoy una entrada sin texturas ni factores se omite). Ausente en escenas
  viejas. Viaja también en la ruta en memoria (clonar, undo).
- **`discardOverriddenDecodedImages`** descarta también los slots (índice 0) para los
  que el `.mat` aporta una textura, con el mismo criterio que la override.
- **`PropertyTracks`** no cambia: sigue escribiendo `materialOverrides[0].metallic/roughness`,
  que manda sobre el `.mat`.

### 3. Cambiar el `.mat` en caliente

`applyMaterialAssetSettings(sceneRoot, matPath, asset, rebuild)`, función pura análoga
a `applyTextureImportSettings`: escribe el `.mat`; solo si se pudo, recorre la escena y,
para cada objeto con algún slot cuya `matAsset` sea esa ruta (`sameAssetPath`),
llama a `applyMaterialOverrides(go)` y luego a `rebuild(go)` (el mismo par que
`MaterialTextureCommand::apply`: `rebuildSkinnedMesh` / `rebuildStaticMesh`). Un fallo de
escritura no reconstruye nada. **No hay vista previa mientras se arrastra**: el panel
edita una copia y **Aplicar** escribe y refresca (cada rebuild espera a la GPU, y un
personaje se reconstruye entero: el coste conocido de compartir).

### 4. UI

- **Crear**: clic derecho en el vacío del grid → **Create → Material** crea
  `Nuevo material.mat` (nombre libre, `uniqueFolderName` generalizado) con todo en
  "heredar" en la carpeta actual y abre el renombrado, como Create Folder.
- **`AssetKind::Material`**: `classifyAsset(".mat")`, icono/etiqueta `MAT`, entrada en el
  filtro por tipo. No ofrece "Import Settings…" ni miniatura.
- **Editar**: doble clic sobre un `.mat` abre el modal **Material** con los cinco campos:
  por textura, la ruta (solo lectura), **Browse…** y **Clear**, y admite soltar un asset
  del grid (mismo filtro y mismo veto que Properties; una textura de fuera del proyecto
  se importa con el flujo que ya existe); dos sliders 0–1 con casilla "Heredar" cada uno.
  **Aplicar** llama a `applyMaterialAssetSettings`; un error de escritura se muestra y no
  cierra el modal.
- **Asignar**: en cada material de Properties, una fila **Material asset** (arrastrar un
  `.mat`, Browse, Abrir y Clear/desvincular). Asignar y desvincular van por
  `MaterialAssetCommand` (antes/después de la ruta, con undo y redo, como
  `MaterialTextureCommand`): aplica `applyMaterialOverrides` y reconstruye el objeto. Las
  filas de textura y los sliders del slot siguen igual y siguen siendo overrides.

### 5. Ciclo de vida

- **Renombrar / mover un `.mat`** (`updateSceneReferencesForRename`): reescribe
  `matAsset` en las overrides de la escena, con el mismo criterio que las rutas de
  textura (fichero o carpeta contenedora).
- **Borrar un `.mat`**: `countSceneReferences` lo cuenta en el diálogo;
  `detachSceneReferencesForDelete` vacía `matAsset`, llama a `applyMaterialOverrides` y
  reconstruye los objetos afectados, que vuelven a su modelo.
- **El sidecar de importación** de las texturas del `.mat` sigue el ciclo de vida de
  cada textura, sin cambios.
- **Exportador**: `collectSceneAssets` añade cada `matAsset` (con `add`, sin sidecar) de
  las overrides de la escena; las texturas del `.mat` ya están en los materiales
  horneados, así que ya se copian. La reescritura de rutas de la escena exportada trata
  `matAsset` igual que `albedo`/`normal`/`orm`. Como el `.mat` guarda sus texturas
  relativas a su carpeta y se conserva la jerarquía del proyecto, el runtime lo
  resuelve sin cambios. El runtime lee el `.mat` con el mismo `applyMaterialOverrides` de
  Core.

## Verificación

Tests planos con `CHECK`, desde la raíz del repo:

- **`MaterialAsset`**: ida y vuelta; guardar el "todo heredar" escribe el fichero;
  ausente / vacío / roto / versión / tipo / campos con tipo equivocado → heredar con
  aviso; `metallic` y `roughness` fuera de rango o no finitos; sidecar hostil (5 MB,
  anidado a 200 000 niveles); rutas relativas a la carpeta del `.mat` y ruta absoluta
  cuando no se puede; ruta Unicode.
- **Capas (`applyMaterialOverrides`)**: modelo / `.mat` / override en cada campo;
  Clear cae al `.mat`; desvincular vuelve al modelo; `.mat` con solo roughness deja la
  textura del FBX; `.mat` inexistente hereda todo sin lanzar; estático y skinned (índice
  ≠ 0); sin `matAsset` el material es idéntico al de hoy y la malla compartida no se
  copia; la override de `PropertyTracks` manda sobre el `.mat`.
- **Escena**: ida y vuelta de `matAsset`; una entrada con solo `matAsset` se guarda;
  escena sin el campo carga igual; ruta relativa a la raíz de assets.
- **`discardOverriddenDecodedImages`** con un `.mat` que aporta albedo.
- **Refresco**: `applyMaterialAssetSettings` escribe y reconstruye solo a los usuarios
  de ese `.mat` (otro `.mat` no); con fallo de escritura no reconstruye; sin renderer
  solo escribe.
- **Ciclo de vida**: renombrar / mover / borrar un `.mat` actualiza o vacía `matAsset`,
  y el diálogo de borrado cuenta sus usuarios; `classifyAsset(".mat")` y el filtro.
- **Exportador**: el `.mat` viaja con la jerarquía y su ruta se reescribe en la escena
  exportada, incluidas las texturas que nombra.
- **`MaterialAssetCommand`**: undo y redo restauran la ruta y el material efectivo.

Verificación manual del usuario, **en GUI y en Vulkan y D3D12**: crear un material,
asignarlo a varios objetos (estáticos y un personaje) y cambiar su textura y sus
sliders con Aplicar refresca a todos; una override del objeto manda sobre el `.mat` y
Clear vuelve a él; desvincular devuelve el modelo; renombrar y mover el `.mat` no
rompe la escena; borrar el `.mat` deja los objetos con su modelo; el juego exportado
respeta el material; cierre limpio en D3D12 y syncval de Vulkan con control positivo.

## Riesgos

- **Rebuild en cadena**: Aplicar reconstruye a todos los usuarios, cada uno con espera
  a la GPU, y un personaje entero. Con decenas de usuarios la edición se nota; por eso
  no hay vista previa mientras se arrastra. Optimizar (por ejemplo, solo
  `setObjectMaterialFactors` cuando cambian únicamente los factores en estáticos) es
  trabajo posterior.
- **Renombrar o mover una TEXTURA no reescribe los `.mat` que la nombran** (sí reescribe
  la escena, como hasta ahora). El material muestra el damero de "textura ausente", igual
  que una ruta rota de hoy; el usuario la reasigna en el panel. Reescribirlos exige
  escanear todos los `.mat` del proyecto en cada renombrado o movimiento: siguiente paso.
- **Lectura del `.mat` por slot y aplicación**: una escena con mil objetos que comparten
  un `.mat` lee ese fichero (~100 bytes) mil veces al cargar. No hay caché en la v1;
  se mide en la verificación y se añade una si pesa.
- **Baseline y `setMesh`**: el baseline se captura al pisar el slot por primera vez;
  un `.mat` que se vincula antes de que el mesh esté cargado (carga asíncrona) depende
  del mismo orden `setMesh` → `applyMaterialOverrides` que ya usan las overrides.
- **Ampliación**: color base, emisivo y tiling exigirán tocar `Material`, los dos
  backends y `makeSharedMeshKey`; el formato con `version` y `type` está pensado para
  añadirlos sin migrar.
