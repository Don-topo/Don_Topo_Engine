# Miniaturas de texturas en el Content Browser — Diseño

Fecha: 2026-09-23. Deriva de `docs/assets-editor-audit.md`, hallazgo U3 ("los
iconos son un botón de color con tres letras: no hay preview real").

## Objetivo

Que el grid del Content Browser muestre una vista reducida real de cada
`.png/.jpg/.jpeg/.tga/.bmp` de la carpeta actual, en lugar del recuadro de color
con `IMG` de hoy, sin tirones y sin agotar memoria de GPU, en Vulkan y en D3D12
con la misma interfaz y **sin dependencias nuevas** (`stb_image` ya está).

Fuera de alcance: miniaturas de modelos, materiales y fuentes; un atlas que crezca
cuando se llena (se desaloja por LRU); regenerar miniaturas de imágenes que el
editor no muestra.

## Restricciones que salen del código (por eso el diseño es este)

- **Hay muy pocos descriptores de ImGui.** Vulkan: pool de `maxSets = 48` y 16
  imágenes (`EditorUI.cpp:296-301`), compartido con el viewport y el Sprite Editor.
  D3D12: `kImGuiReserved = 16` (`D3D12Renderer.cpp:304`). Una textura por
  miniatura no escala: una carpeta de 30 imágenes ya no cabe.
- **`loadUiAtlas` no sirve para miniaturas.** Carga la imagen a resolución completa
  y la cachea por ruta hasta cerrar el editor (`Renderer.cpp:763`,
  `D3D12Renderer.cpp:11132`): 64 MB por una textura de 4096².
- **Decodificar es caro** (decenas o cientos de ms por PNG grande): no puede pasar
  en el hilo de render. El repo ya tiene `JobSystem` (`Core/JobSystem.h`), y
  `AsyncAssetLoader` es específico de mallas.
- **No existe subida parcial de textura.** Los dos backends solo suben la imagen
  entera (`GpuResources::uploadPixelsToImage`, `D3D12Renderer::uploadTexture`).

## Decisiones ya tomadas

- **Un atlas compartido de miniaturas**: una sola textura de 2048×2048 con casillas
  de 64×64 (1024 miniaturas), **un único descriptor** de ImGui, cada miniatura es
  un sub-rect UV. Coste fijo ≈ 16 MB. Descartada una textura pequeña por miniatura:
  consume un descriptor cada una y obligaría a limitar el grid a unas decenas.
- **Solo texturas**, y solo las que están a la vista.
- **Sin dependencias nuevas.** Los tests escriben un BMP a mano (no hay
  `stb_image_write`).

## Arquitectura

### `Renderer/ThumbnailAtlas.h` (Core, header-only)

Lo que comparten el editor y los backends, y nada más:

```cpp
namespace DonTopo {
constexpr uint32_t kThumbCell       = 64;                 // lado de una casilla, en píxeles
constexpr uint32_t kThumbAtlasCells = 32;                 // casillas por lado
constexpr uint32_t kThumbAtlasSize  = kThumbCell * kThumbAtlasCells;   // 2048
constexpr uint32_t kThumbSlotCount  = kThumbAtlasCells * kThumbAtlasCells;

struct UvRect { float u0, v0, u1, v1; };
// UV de una casilla, con la mitad de un texel de margen por cada lado para que el
// filtrado lineal no sangre el color de la casilla vecina.
UvRect thumbnailUv(uint32_t slot);
}
```

### `EditorRenderer`: dos virtuales nuevos, por defecto "no soportado"

Mismo patrón que `loadUiAtlas` ("nullptr = no se pudo, y quien pide sigue con su
alternativa"):

```cpp
// Descriptor de ImGui del atlas compartido de miniaturas (lo que ImGui::AddImage
// entiende por textura), o 0 si el backend no lo soporta. Se crea y registra en
// la primera llamada; después devuelve siempre el mismo valor.
virtual uint64_t uiThumbnailAtlasId() { return 0; }
// Copia una casilla kThumbCell×kThumbCell RGBA8 (sRGB) a `slot`. false = no se
// pudo (sin backend, slot fuera de rango, fallo de GPU).
virtual bool uploadUiThumbnail(uint32_t slot, const uint8_t* rgba) { (void)slot; (void)rgba; return false; }
```

**Vulkan** (`Renderer`): imagen `R8G8B8A8_SRGB` de `kThumbAtlasSize`², uso
`SAMPLED | TRANSFER_DST`, creada de forma perezosa, inicializada a transparente y
en `SHADER_READ_ONLY_OPTIMAL`. Registrada una vez con
`m_ui->registerUiTexture(m_uiBatch.sampler(), view)`. Subida por región con
staging + `vkCmdCopyBufferToImage`, con las transiciones de layout necesarias
alrededor y **la misma sincronización que el subidor de imagen completa** (submit
inmediato + espera): las subidas de un frame se agrupan en un solo submit. Nueva
función de apoyo `GpuResources::uploadPixelsToImageRegion`, con
`uploadPixelsToImage` como plantilla. Se destruye (y se desregistra el
descriptor) en el apagado, antes de `m_gpu.shutdown()`, junto a los demás atlas.

**D3D12** (`D3D12Renderer`): textura en heap por defecto, `R8G8B8A8_UNORM_SRGB`,
estado `PIXEL_SHADER_RESOURCE`, con **un** SRV tomado del mismo heap y de la misma
forma que `registerUiAtlas`. Subida por región con upload heap y
`CopyTextureRegion` entre transiciones `PIXEL_SHADER_RESOURCE ↔ COPY_DEST`. Nueva
función de apoyo `uploadTextureRegion`, con `uploadTexture` como plantilla.

Los dos backends deben acabar con la **misma** semántica visible: mismas
coordenadas UV, mismo orden de filas (fila 0 arriba), mismo espacio de color.

### `Editor/Thumbnail.h/.cpp` (Editor)

Tres piezas con una responsabilidad cada una, todas probables sin GPU:

**`makeThumbnail(path) -> ThumbnailResult`** — CPU pura.
`stbi_info` primero: dimensiones inválidas o más de **100 megapíxeles** →
`TooLarge`/`Unreadable` sin decodificar (un PNG de 16k² serían 1 GB transitorios
en un worker). Si pasa, `stbi_load` a RGBA, reducción a una casilla
`kThumbCell`² **conservando la proporción** (filtro de caja, sin ampliar imágenes
más pequeñas que la casilla), centrada y con relleno transparente. Devuelve
`{ status, rgba[kThumbCell*kThumbCell*4] }`.

**`ThumbnailSlots`** — reparto de las `kThumbSlotCount` casillas con LRU:

```cpp
class ThumbnailSlots {
public:
    static constexpr uint32_t kNone = 0xFFFFFFFFu;
    explicit ThumbnailSlots(uint32_t capacity = kThumbSlotCount);
    void     beginFrame();                 // avanza el contador de frame
    uint32_t assign(uint64_t key);         // casilla de key (la que ya tenía, marcada
                                           // como usada este frame) o una nueva; si
                                           // no queda libre desaloja la menos usada
                                           // recientemente que NO se usó este frame;
                                           // kNone si todas se usaron este frame
    uint32_t find(uint64_t key);           // como assign pero sin reservar; kNone si no está
    void     release(uint64_t key);
};
```

**`ThumbnailCache`** — orquesta pedidos, decodificación asíncrona y subida:

```cpp
class ThumbnailCache {
public:
    using Runner   = std::function<void(std::function<void()>)>;        // JobSystem::submit
    using Uploader = std::function<bool(uint32_t slot, const uint8_t* rgba)>;
    ThumbnailCache(Runner run, Uploader upload, uint32_t maxInFlight = 4);

    void beginFrame();
    // Miniatura de path si ya está en el atlas; nullopt = todavía no (pendiente,
    // fallida o sin soporte) y quien pide sigue con el icono de color. Un nullopt
    // por primera vez ENCOLA la decodificación.
    std::optional<UvRect> request(const std::filesystem::path& path);
    // Sube a GPU lo ya decodificado, como mucho `maxUploads` casillas.
    void pump(int maxUploads = 8);
    // Vuelve a leer el mtime de lo que se pidió este frame y regenera lo que
    // cambió. Lo llama el polling del panel (cada 0.5 s), no cada frame.
    void refreshStamps();
    // Descarta lo pendiente y lo que no se pidió (cambio de carpeta).
    void newGeneration();
};
```

- **Clave** = ruta + `last_write_time`. Cambiar el contenido con el mismo nombre
  cambia el `mtime`, `refreshStamps` lo detecta y se regenera.
- **Como máximo 4 decodificaciones en vuelo**: una carpeta de 500 texturas no debe
  ocupar todos los workers y retrasar una carga de malla que comparte el
  `JobSystem`. Al terminar una, se lanza la siguiente pedida.
- **Estados por entrada:** `Pending` (encolada o decodificada sin subir), `Ready`
  (en el atlas), `Failed` (decodificación fallida o imagen demasiado grande).
  `Failed` se cachea y **no se reintenta** hasta que cambie el `mtime`.
- **Hilos:** el worker solo ejecuta `makeThumbnail` y deja el resultado en una cola
  con mutex; **toda** la subida a GPU y todo el estado de las casillas viven en el
  hilo principal (`pump`). El estado compartido con los workers va en un
  `shared_ptr`, de modo que un resultado que llegue tarde (cambio de carpeta,
  cierre) se descarta sin tocar memoria liberada. Un contador de generación
  descarta también los resultados de la carpeta anterior.
- **Sin renderer o sin `JobSystem`:** el cache no se crea y el grid queda como hoy.

### Integración en `ContentBrowserPanel`

- `EditorContext` gana `JobSystem* jobs = nullptr`; `EditorUI::setJobSystem`, y
  `sandbox/src/main.cpp` lo cablea en las **dos** ramas (Vulkan y D3D12) junto a
  `setAssetLoader`, con el mismo patrón que `setDroppedFilesProvider`.
- El panel crea el cache de forma perezosa cuando hay `ctx.renderer` **y**
  `ctx.jobs` **y** `ctx.renderer->uiThumbnailAtlasId() != 0`.
- En el bucle del grid, para cada ítem de tipo `Image` **visible**
  (`ImGui::IsItemVisible()` tras dibujar el botón, para no pedir miles de
  decodificaciones en una carpeta enorme): `request(path)`. Con `UvRect` se dibuja
  con `AddImage(atlasId, ...)` sobre el botón —conservando el borde de selección—
  y sin etiqueta `IMG`; sin él, el recuadro de color de siempre.
- `pump()` una vez por frame; `beginFrame()` al principio; `refreshStamps()` dentro
  del bloque de polling que ya existe; `newGeneration()` cuando cambia
  `m_currentDir`.

## Errores y casos límite

| Caso | Comportamiento |
|---|---|
| Imagen ilegible o con extensión falsa | `Failed`, icono de color, sin reintento hasta que cambie el `mtime` |
| Imagen de más de 100 MP | `Failed` sin decodificar |
| Imagen más pequeña que la casilla | Se centra sin ampliar |
| Imagen con alfa | El alfa se conserva; se dibuja sobre el fondo del panel |
| Atlas lleno (más de 1024 pedidos vivos) | Desaloja por LRU lo que no se usó este frame; si todo se usó este frame, `assign` devuelve `kNone` y ese ítem sigue con el icono |
| Backend sin soporte (`uiThumbnailAtlasId() == 0`) | El cache no se crea; comportamiento actual |
| Falla `uploadUiThumbnail` | La entrada queda `Failed`; se reintenta al cambiar el `mtime` |
| Cambio de carpeta con decodificaciones en vuelo | Sus resultados se descartan por generación |
| Apagado del editor con jobs en vuelo | El `JobSystem` se apaga primero (ya ocurre); los resultados tardíos caen en el estado compartido, que sobrevive por `shared_ptr` |

## Testing

**Headless** (`dt_thumbnail_tests`, enlaza `DonTopoEditor`):
- `makeThumbnail`, con BMPs escritos a mano: imagen apaisada y vertical (proporción
  y centrado), más pequeña que la casilla (sin ampliar), alfa conservado, fichero
  inexistente o corrupto, y límite de megapíxeles (cabecera BMP con dimensiones
  enormes, sin cuerpo).
- `ThumbnailSlots`: reparto, reutilización de la misma clave, desalojo LRU, no
  desalojar lo usado este frame (`kNone`), `release`.
- `thumbnailUv`: casillas de las esquinas y margen de medio texel.
- `ThumbnailCache` con `Runner` manual y `Uploader` falso, deterministas:
  pendiente → listo tras `pump`; tope de 4 en vuelo; fallo cacheado sin
  reintentar; regeneración por cambio de `mtime`; descarte por `newGeneration`;
  tope de subidas por frame; resultado tardío tras destruir el cache sin tocar
  memoria liberada.

**Manual** (la subida real y el dibujado no se pueden probar sin GPU):
- Vulkan y D3D12: una carpeta con imágenes de distintos tamaños y proporciones
  muestra miniaturas correctas y con los mismos colores en los dos backends
  (comparar contra la imagen abierta en el Sprite Editor).
- Carpeta con cientos de imágenes: sin tirones al entrar ni al hacer scroll.
- Re-exportar una textura con el mismo nombre: la miniatura se actualiza en menos
  de un segundo.
- Vulkan con validation layers y syncval (ver la nota del repo: `vk_layer_settings.txt`
  junto al exe): sin errores durante subidas repetidas.
- Cambiar de carpeta a mitad de carga: sin miniaturas de la carpeta anterior.

## Riesgos

1. **La subida por región es código nuevo en dos backends**, sin equivalente hoy.
   Es lo más delicado del cambio y por eso cada backend sale con su verificación
   manual propia.
2. **Sincronización en Vulkan**: hay que pasar el atlas de `SHADER_READ_ONLY` a
   `TRANSFER_DST` y volver mientras la GPU puede estar leyéndolo en un frame en
   vuelo. El diseño obliga a la misma espera que el subidor existente; syncval es
   el árbitro.
3. **Descriptor en D3D12**: el SRV del atlas sale del mismo heap de 16 reservados
   para ImGui que ya comparten el viewport y el Sprite Editor. Es uno solo, pero
   hay que confirmar que cabe y que no lo recicla ImGui.
4. **Espacio de color**: el atlas es sRGB igual que `UiTextureAtlas`; si un backend
   lo tratara como lineal, las miniaturas saldrían más claras o más oscuras que la
   imagen real. La comprobación manual entre backends existe para esto.
