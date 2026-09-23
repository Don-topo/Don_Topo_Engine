# Ajustes de importación por textura — Diseño

Fecha: 2026-09-24. Deriva de `docs/assets-editor-audit.md`, hallazgo U8 ("import
settings por asset: depende de decidir dónde vive el metadato persistente en el
Core") y capacidad 6 de la comparación con Unity.

## Objetivo

Que cada textura de material pueda llevar dos ajustes persistentes, editables desde
el Content Browser y respetados por el editor y por el juego exportado, en Vulkan y
en D3D12:

- **Espacio de color**: `auto` (lo decide el slot del material, como hoy), `srgb` o
  `linear`.
- **Mipmaps**: encendidos o apagados. Hoy ninguna textura de fichero tiene mips.

Los ajustes viven en un **sidecar junto al asset** con un formato extensible por
tipo, para poder añadir modelos y audio después sin migrar.

Fuera de alcance de esta v1: modelos y audio (siguientes specs), texturas de UI,
skybox y splash, filtro/wrap/anisotropía, tamaño máximo, compresión, undo del
cambio de ajustes.

## Restricciones que salen del código

- **El formato lo decide el slot, no el fichero.** `TextureKind { BaseColor, Normal,
  Orm }` (`SharedTextureCache.h:11`) → sRGB solo para BaseColor. Vulkan:
  `createTextureImage` (sRGB fijo, `GpuResources.cpp:399`) y `createNormalMapImage`
  (UNORM fijo, `:425`); D3D12: bool `srgb` en `uploadMaterialTexture`
  (`D3D12Renderer.cpp:2610`), con el formato del SRV repetido a mano en
  `:3546` y `:10097-10113`.
- **Nunca se generan mips para un fichero.** `mipLevels = 1` en
  `GpuResources.cpp:130`, `Renderer.cpp:3257/5517`; D3D12 `uploadTexture` y SRV con
  `MipLevels = 1` (`:2484`, `:2591/2626`).
- **El sampler de material de Vulkan tiene `maxLod = 0`**
  (`GpuResources.cpp:445`, `createTextureSampler`): con mips seguiría leyendo solo el
  nivel 0. Es compartido (`sharedMaterialSampler`) y lo usa también
  `UiSpriteBatch.cpp:1737`. D3D12 ya usa `MaxLOD = FLOAT32_MAX` (`:2836`).
- **Hay un decodificador central de materiales**, `decodeMaterialTexture`
  (`MaterialTextureSource.cpp:20`), al que llegan estáticos, skinned, async y D3D12.
  El resto (UI, skybox, splash, miniaturas) decodifica por su cuenta y no entra.
- **La clave de caché ignora los ajustes**: `makeTextureKey` = `tipo:ruta`
  (`SharedTextureCache.h:18`). Dos materiales con el mismo fichero y ajustes
  distintos compartirían imagen.
- **El cargador asíncrono es solo Vulkan** (`AsyncAssetLoader.cpp:26`, `decodeSlot`):
  el worker produce `DecodedImage` en bruto sin formato; D3D12 lo ignora y
  redecodifica en el hilo principal.
- **El exportador copia por lista** (`GameExporter.cpp:183` `collectSceneAssets`,
  `copyOne :527`, más los bucles de skybox `:574` y splash `:603`): un sidecar no se
  copiaría hoy.
- **La selección del grid es privada de `ContentBrowserPanel`** (`AssetSelection`):
  no existe selección de asset visible para Properties.

## Diseño

### 1. Modelo y sidecar (Core)

Módulo `ImportSettings` (`Core/ImportSettings.h/.cpp`, sin dependencias de GPU ni de
ImGui, que se prueba sin device):

```cpp
enum class ColorSpaceOverride { Auto, Srgb, Linear };
struct TextureImportSettings { ColorSpaceOverride colorSpace = Auto; bool mipmaps = false; };

std::filesystem::path importSidecarPath(const std::filesystem::path& asset); // "<asset>.import.json"
TextureImportSettings loadTextureImportSettings(const std::filesystem::path& asset);
bool saveTextureImportSettings(const std::filesystem::path& asset, const TextureImportSettings&);
bool isDefault(const TextureImportSettings&);
```

Fichero: `{ "version": 1, "type": "texture", "colorSpace": "auto|srgb|linear",
"mipmaps": false }`.

- **Solo existe si difiere de los valores por defecto**: guardar un valor por defecto
  borra el sidecar. Un proyecto existente no gana ficheros ni cambia de aspecto.
- **Lectura tolerante**: fichero ausente, JSON roto, `version` desconocida, `type`
  distinto de `texture` o valor de enum desconocido → valores por defecto (por
  campo cuando sea posible) y un aviso en el log. Nunca lanza, nunca aborta la
  carga de la escena.
- `type` es el discriminador para futuros tipos; la v1 solo lee `texture`.

### 2. Consumidores

- **`decodeMaterialTexture`** lee los ajustes de la ruta (las embebidas del FBX no
  tienen sidecar: siempre valores por defecto) y devuelve, junto a los píxeles, el
  espacio de color resuelto y la cadena de mips.
- **Resolución del espacio de color**: `resolveSrgb(TextureKind, ColorSpaceOverride)`,
  función pura: `Srgb`→true, `Linear`→false, `Auto`→`kind == BaseColor`. Es lo
  único que decide, y se usa en los dos backends para dejar de repetir la regla.
- **Cadena de mips en CPU**: `buildMipChain(rgba, w, h)`, filtro de caja ponderado por
  alfa (mismo criterio que `makeThumbnail`), hasta 1×1, niveles con dimensión
  `max(1, d/2)`. Se genera en CPU para que Vulkan y D3D12 la suban con la misma
  entrada; el blit de GPU no tiene equivalente en D3D12 sin un compute.
- **Vulkan**: la imagen se crea con `mipLevels` = niveles de la cadena, la vista con
  `levelCount` igual, se sube nivel a nivel con el mecanismo de regiones ya
  existente (`uploadPixelsToImageRegions`), y `createTextureSampler` sube `maxLod`
  (a `VK_LOD_CLAMP_NONE`). El sampler compartido con `UiSpriteBatch` no cambia de
  comportamiento: sus imágenes tienen un solo nivel. El formato de imagen y el de
  vista deben coincidir (`MUTABLE_FORMAT` no hace falta).
- **D3D12**: `uploadTexture` acepta niveles (subresource por nivel, pitch de 256 B
  por nivel) y los SRV usan el `MipLevels` real; el formato del SRV se deriva del
  resuelto en un solo sitio.
- **Clave de caché**: `makeTextureKey` incorpora los ajustes (`srgb` resuelto y
  `mipmaps`) además de tipo y ruta, como sufijo que solo se añade cuando los ajustes no son los
  de siempre (así las claves de hoy no cambian). Se comprueba también
  `makeSharedMeshKey`, que lee las rutas.
- **Cargador asíncrono (Vulkan)**: `DecodedImage` lleva formato resuelto y niveles;
  `decodeSlot` los rellena con el mismo decodificador central.

### 3. UI (Content Browser)

Clic derecho sobre **una** textura seleccionada → "Import Settings…" abre un modal
(no toca Properties ni la selección global): combo de espacio de color, casilla
Mipmaps, botones Aplicar y Cancelar. Con varias seleccionadas o con otro tipo de
asset la entrada no aparece.

**Aplicar**: escribe el sidecar (`save…`; los valores por defecto lo borran) y
reconstruye los materiales que usan esa ruta con el camino de rebuild que ya existe
en ambos backends (`Renderer.cpp:4639-4666`, `D3D12Renderer.cpp:10792-10816`). Un
fallo de escritura se muestra en el modal y en el log; el modal no se cierra. No hay
undo: es configuración de fichero, como el resto de operaciones del Content Browser.

### 4. Ciclo de vida del sidecar

- **Mover, renombrar, borrar**: arrastran el sidecar junto al asset (`moveAsset`,
  rename y borrado del Content Browser; también cuando se mueve una carpeta entera).
  Si el destino ya tiene sidecar, la operación se rechaza como hoy se rechaza un
  conflicto de nombre.
- **Importar ficheros externos** (drop / Browse): copia también el sidecar si
  existe junto al origen; no lo crea.
- **Exportar**: el exportador copia `<asset>.import.json` con cada textura de
  material que lo tenga, respetando la ruta relativa (o `assets/_external/N/`). No se
  copia el de texturas que no son de material (skybox, splash).
- **Huérfano** (asset borrado fuera del editor): se ignora sin error; no se limpia
  solo.
- El grid **no muestra** los `.import.json` (filtro de listado), igual que se ocultan
  hoy los ficheros que el editor no reconoce como asset.

## Verificación

Tests con `CHECK` (planos, desde la raíz del repo):

- `ImportSettings`: ida y vuelta; defecto = sin fichero; JSON roto / versión / tipo /
  enum desconocidos → defecto sin lanzar; ruta con caracteres Unicode.
- `buildMipChain`: número y tamaño de niveles (potencia y no potencia de 2, 1×N),
  último nivel 1×1, alfa ponderado (un píxel transparente no oscurece), no modifica
  el nivel 0.
- `resolveSrgb`: tabla completa 3 slots × 3 overrides.
- Clave de caché: mismos ruta y slot con ajustes distintos → claves distintas;
  ajustes por defecto → estable.
- Ciclo de vida: mover/renombrar/borrar arrastran el sidecar; conflicto rechazado;
  importación externa lo copia; el exportador lo incluye; el listado no lo muestra.
- Política (grep): el formato de subida no se decide fuera de `resolveSrgb` en los
  caminos de material.

Verificación manual del usuario en GUI en **ambos backends** (no la puede hacer un
agente): mipmaps visibles a distancia sobre una textura de tablero; un normal map
marcado `linear` y una albedo marcada `srgb`/`linear` cambian de color como se
espera; Aplicar refresca sin reiniciar; mover/renombrar conserva los ajustes; juego
exportado respeta los ajustes; cierre limpio en D3D12 (D3D12MA); syncval de Vulkan con
control positivo, sin avisos de layout de imagen ni de nivel.

## Riesgos

- **Aspecto**: activar mips cambia la textura a distancia. Por eso el valor por
  defecto es apagado y nada existente cambia hasta que se active.
- **Mips sobre texturas con `srgb` mal resuelto**: promediar en espacio sRGB oscurece
  ligeramente. La v1 promedia el valor codificado (como las miniaturas); se anota
  como limitación conocida, no se corrige aquí.
- **Vulkan: transición de layout por nivel** en la subida es el punto donde más fácil
  se escapa un aviso de validación; por eso la verificación exige syncval.
- **Ampliación futura** (modelos y audio): el audio decide 2D/3D/loop/streaming por
  `AudioSource`, no por fichero; habrá que definir quién manda antes de esa spec.
