# Modelos glTF y OBJ en el editor — Diseño

Fecha: 2026-09-25. Sale del repaso de pendientes tras las miniaturas de modelos
(`2026-09-25-model-material-thumbnails-design.md`).

## El fallo

El editor y el core no están de acuerdo sobre qué es un modelo:

| Formato | Core (Assimp) | Content Browser | Add Mesh / soltar en Properties | Importar desde fuera | Animator (fuentes) |
|---|---|---|---|---|---|
| `.fbx` | carga | modelo | sí | sí | sí |
| `.obj` | **carga** | modelo | **no** («Formato no soportado») | **no** | no |
| `.gltf`/`.glb` | **no** (importador no compilado) | **modelo**, con Import Settings de modelo | no | no | no |

- `classifyAsset` (`ContentBrowserPanel.cpp:236`) promete `.gltf`/`.glb` como
  modelo (icono `3D`, filtro «3D», Import Settings de modelo), pero
  `CMakeLists.txt:110-112` solo compila los importadores FBX y OBJ.
- `.obj` es lo contrario: el core lo carga (hoy ya tiene miniatura), pero
  `PropertiesPanel.cpp:390` lo rechaza, el diálogo de Add Mesh solo ofrece
  `.fbx` (`:8018`) y `AssetImport.cpp:23` no lo importa. Esto contradice la
  regla del repo de no capar en la UI lo que el core soporta.
- Las listas de extensiones están repartidas en cinco sitios que ya han
  divergido.

## Objetivo

`.fbx`, `.obj`, `.gltf` y `.glb` se tratan igual en todo el editor: se ven como
modelo, se añaden a un objeto (Add Mesh, soltar, arrastrar al viewport), se
importan desde fuera, tienen miniatura, sirven como fuente de animación, se
cargan en el juego exportado con sus ficheros asociados, y en Vulkan y D3D12.

Fuera de alcance:
- **Draco** (mallas comprimidas en glTF): Assimp 5.3.1 lo deja apagado. Un
  `.glb` con Draco falla al cargar con el error de Assimp, como cualquier
  fichero ilegible.
- Mover o renombrar un `.gltf`/`.obj` en el Content Browser **no** arrastra su
  `.bin`/`.mtl`/texturas. Es el comportamiento que ya tiene un FBX con sus
  texturas externas.
- Exportar a glTF.

## Decisiones

### 1. Una sola lista de formatos, en Core

```cpp
// ModelLoader.h
// Formatos de modelo que Assimp tiene compilados. La UNICA lista: el editor
// entero pregunta aqui (clasificar, Add Mesh, importar, miniaturas, Animator).
static bool isSupportedModelExtension(const std::string& ext);   // sin distinguir mayusculas
// Para los filtros de los dialogos de fichero (ImGuiFileDialog): ".fbx,.obj,.gltf,.glb".
static const char* supportedModelFilter();
```

La usan:
- `classifyAsset` (`AssetKind::Model3D`).
- `PropertiesPanel::loadMeshForSelected` y el filtro y el texto del diálogo de
  Add Mesh («Drop a model here»).
- `isImportableExtension` e `importedAssetDestDir` (los cuatro a
  `assets/Imported/Meshes`).
- `isModelThumbnailPath`.
- `AnimatorPanel`: la comprobación y el filtro de las fuentes de animación
  (`loadAnimationClips` es Assimp genérico).

### 2. Build

`set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)` junto a los de FBX y OBJ.
Es parte del mismo Assimp ya descargado: sin dependencias nuevas. Lo compila
también el CI de Linux.

### 3. Texturas externas en subcarpeta

Hoy `load`, `loadSkinned` y `loadPreview` buscan una textura externa como
`dirname(modelo) / filename(ruta del fichero)`: descartan la subcarpeta. Un
`.gltf` exportado como «separado» deja las texturas en `textures/`.

Una función compartida:

```cpp
// Ruta de una textura externa referenciada por el modelo: primero la ruta
// relativa TAL CUAL respecto a la carpeta del modelo (textures/x.png); si no
// existe, el nombre suelto junto al modelo, que es lo de siempre. Una ruta
// absoluta o que salga de la carpeta (..) solo prueba el nombre suelto.
static std::filesystem::path resolveModelTexture(const std::filesystem::path& modelDir,
                                                 const std::string& raw);
```

Para FBX y OBJ que hoy funcionan el resultado es el mismo: la textura ya está
junto al modelo, y ese caso se sigue probando.

### 4. Ficheros asociados

```cpp
// Ficheros que el modelo lee ademas de si mismo, en rutas RELATIVAS a su
// carpeta, sin duplicados: los mtllib de un .obj, y los buffers[].uri e
// images[].uri externos de un .gltf (los "data:" van dentro y se ignoran; un
// .glb no tiene). FBX: ninguno (sus texturas ya viajan como texturas). Nunca
// lanza: un fichero ilegible devuelve vacio.
static std::vector<std::string> modelCompanionFiles(const std::string& path);
```

- Las URIs de glTF se decodifican del percent-encoding (`%20` → espacio).
- Una ruta absoluta o con `..` que salga de la carpeta del modelo se ignora
  (no se empaqueta ni se vigila).

**Exportador** (`collectSceneAssets`):
- Tras el modelo y su sidecar, añade cada fichero asociado **colocado respecto
  a la carpeta del modelo en el paquete**: `dirname(packagePath del modelo) /
  relativa`.
- Dentro del proyecto esto coincide con la jerarquía de siempre. Fuera del
  proyecto evita que `D:/ext/textures/x.png` acabe en otra
  `assets/_external/N` distinta de la del `.gltf` y rompa la ruta relativa.
- Los asociados se añaden **antes** que las texturas del material, para que la
  deduplicación por ruta conserve esa colocación.
- Esto se aplica también a las fuentes de animación.

**Miniaturas**: `loadPreview` declara los asociados como dependencias,
sellados antes de leerlos. Sustituye al escaneo de `mtllib` de
`stampObjMaterialLibraries`, que desaparece. `kThumbDiskVersion` pasa a 3,
porque las entradas de la 2 no tienen los `.bin`.

## Errores y casos límite

| Caso | Comportamiento |
|---|---|
| `.glb` con Draco o glTF roto | Falla al cargar con el mensaje de Assimp, como hoy un FBX roto; miniatura `Unreadable` |
| `.gltf` cuyo `.bin` no existe | Falla al cargar; el `.bin` es dependencia, así que la miniatura se regenera cuando aparece |
| URI con `..` o absoluta | No se empaqueta ni se vigila; la carga hace lo que haga Assimp |
| `.obj` sin `.mtl` | Carga la geometría sin textura (como hoy en el core) |
| Textura en subcarpeta y además una homónima junto al modelo | Gana la de la subcarpeta, que es la que el fichero nombra |
| Mismo `.bin` compartido por dos `.gltf` | Se empaqueta una vez (dedup por ruta) |

## Testing

**Headless**:
- `model_import_tests`:
  - un `.gltf` con el buffer en base64 (`data:`) carga con N triángulos y UV;
  - un `.gltf` con `.bin` externo y textura en `textures/` carga con la
    textura resuelta a la subcarpeta;
  - un `.glb` montado en bytes dentro del test (cabecera + chunk JSON + chunk
    BIN) carga;
  - `modelCompanionFiles` para `.obj` (varios `mtllib`), `.gltf` (buffers e
    imágenes, ignora `data:`, decodifica `%20`, ignora `..` y absolutas),
    `.glb` y `.fbx` (vacío);
  - `resolveModelTexture`: subcarpeta primero, nombre suelto de respaldo,
    `..` y absoluta solo el nombre suelto;
  - un FBX existente (`assets/modelTexture.fbx`) resuelve la textura igual que
    antes.
- `content_browser_tests`: `classifyAsset` y `importSettingsKindFor` con los
  cuatro formatos.
- `asset_import_tests`: `isImportableExtension` e `importedAssetDestDir` con
  los cuatro.
- `thumbnail_tests`: `isModelThumbnailPath` acepta los cuatro, y un `.gltf`
  declara su `.bin` como dependencia.
- `exporter_tests`:
  - un `.gltf` con `.bin` y textura en `textures/`, dentro del proyecto, se
    empaqueta con la jerarquía;
  - lo mismo fuera del proyecto: el `.bin` y la textura quedan en la
    `assets/_external/N` del `.gltf`, con su subcarpeta;
  - un `.obj` lleva su `.mtl`.

**Manual** (Release, Vulkan y D3D12):
- Un `.glb` real con textura y un `.obj` con `.mtl`, arrastrados desde el
  Explorador:
  - se importan a `Imported/Meshes`;
  - tienen miniatura;
  - se añaden a un objeto y se ven con su textura;
  - sus Import Settings (escala) se aplican.
- Exportar esa escena: el juego carga los dos modelos con textura.
- Un `.gltf` con animación como fuente del Animator de un personaje con el
  mismo esqueleto.

## Riesgos

1. **El importador glTF de Assimp alarga el build** (incluye Open3DGC). Es una
   vez, en Windows y en el CI de Linux.
2. **Materiales PBR de glTF**: el loader lee la difusa (`aiTextureType_DIFFUSE`,
   que el importador glTF2 rellena con el `baseColorTexture`), la normal y el ORM
   como `aiTextureType_UNKNOWN`. Si el importador los mapeara distinto, un `.glb`
   se vería sin textura; lo destapan el test del `.gltf` con textura y la
   verificación manual.
3. **Ejes y unidades**: glTF es Y-arriba y en metros; FBX de Mixamo va en cm.
   Un `.glb` puede verse a otra escala que un FBX equivalente. Se corrige con la
   escala de Import Settings, que ya existe; no se normaliza nada.
