# Miniaturas de modelos y materiales en el Content Browser — Diseño

Fecha: 2026-09-25. Cierra lo que dejó abierto U3 de `docs/assets-editor-audit.md`
(«modelos y materiales siguen sin miniatura») y amplía las miniaturas de texturas
de `2026-09-23-content-browser-thumbnails-design.md` con una caché en disco.

## Objetivo

Que el grid del Content Browser muestre una vista reducida real de cada
`.fbx`/`.obj` y de cada `.mat` de la carpeta actual, en lugar del recuadro de color
con `3D`/`MAT`. El objetivo es **distinguir assets a simple vista**, no un render
fiel: a 64×64 basta con que la silueta y el color del modelo, y el color, la
textura y el brillo del material, sean los reales. Igual en Vulkan y en D3D12,
sin dependencias nuevas y sin tirones.

Además, todas las miniaturas (también las de texturas) se guardan en una **caché
en disco**, para que abrir una carpeta ya vista no vuelva a leer los ficheros.

Fuera de alcance:
- `.gltf`/`.glb`: el grid los clasifica como modelo, pero Assimp está compilado
  solo con los importadores OBJ y FBX (`Enabled importer formats: OBJ FBX` al
  configurar). Siguen con el icono. Hallazgo aparte, sin tocar aquí.
- Podar la caché de disco. Borrar `.dt-cache/thumbs/` es siempre seguro.
- Miniaturas de fuentes.
- Pose animada: los personajes salen en bind pose, como en Unity.

## Por qué rasterizado en CPU (spike del 2026-09-25)

Se descartó renderizar con el renderer real (pass offscreen nuevo en **dos**
backends, en el hilo de render y sin poder probarse sin GPU) y también se
descartó el icono con metadatos (no enseña la forma). Un spike desechable
rasterizó en CPU los modelos de `assets/` y cinco `.mat` sintéticos a 64×64:

- Las siluetas se reconocen y los cinco materiales se distinguen sin dudar
  (textura, cielo, metal brillante, plástico mate, logo).
- **El coste está en cargar, no en rasterizar.** En build Debug:
  `ModelLoader::loadAuto` tardó de 0,3 a **6,7 s** (`modelAnimation.fbx`) y el
  rasterizado de 25 a 550 ms. `loadAuto` procesa animaciones, esqueleto y todas
  las texturas, y la miniatura no necesita nada de eso: de ahí `loadPreview` y la
  caché en disco.
- Un FBX que solo trae animación (`standing idle 01.fbx`) no tiene malla y
  Assimp falla: necesita estado propio, no quedarse en fallo mudo.
- El motor **no recorta por alfa** (ningún shader hace `discard`). Un alpha test
  en la miniatura borraba texturas enteras con alfa bajo (`texture.png`): no se
  hace.
- Duda abierta: `Maw J Laygo.fbx` salió oscuro y confuso. Puede ser su textura;
  se compara contra el viewport en la verificación manual.

## Decisiones tomadas

- **Rasterizado por software en CPU**, dentro del `Decoder` que `ThumbnailCache`
  ya acepta. Ningún código GPU nuevo; el atlas, el LRU, el tope en vuelo y el
  descarte por generación no cambian.
- **Caché en disco para todas las miniaturas**, texturas incluidas.
- **Un `.mat` que hereda se pinta con valores neutros**: albedo gris claro,
  metallic 0, roughness 0.5 (los defaults de `Material`). Sin marca de «heredado».
- **Ubicación de la caché**: `<proyecto>/.dt-cache/thumbs/`, junto a la de fuentes
  (`.dt-cache/fonts`, `UiFont.cpp`). `.dt-cache/` ya está en `.gitignore` y
  `isHiddenDir` oculta del Content Browser toda carpeta que empieza por punto. El
  exportador copia solo lo que la escena referencia, así que la caché no viaja.

## Arquitectura

### `ModelLoader::loadPreview` (Core, `Renderer/ModelLoader`)

Lectura ligera con Assimp para miniaturas:

```cpp
enum class PreviewStatus { Ok, AnimationOnly, Unreadable };

struct PreviewImage { int w = 0, h = 0; std::vector<uint8_t> rgba; };   // vacía = sin textura

struct PreviewPart
{
    std::vector<glm::vec3> positions, normals, colors;
    std::vector<glm::vec2> uvs;
    std::vector<uint32_t>  indices;
    PreviewImage           albedo;            // lado mayor <= kPreviewMaxTexture (256)
    float                  metallic  = 0.0f;
    float                  roughness = 0.5f;
};

struct ModelPreview
{
    PreviewStatus            status = PreviewStatus::Unreadable;
    std::vector<PreviewPart> parts;           // una por submalla/material
    std::vector<std::filesystem::path> dependencies;   // texturas externas y sidecar
};

// No lanza. Solo geometría en bind pose y la textura difusa: sin animaciones,
// esqueleto, normal map ni ORM.
static ModelPreview loadPreview(const std::string& path);
```

- Vive en `ModelLoader` para **reutilizar** su resolución de rutas de textura
  (embebida o junto al fichero) y sus flags de importación, sin duplicarlos.
- Respeta el sidecar `<modelo>.import.json` en lo que cambia el aspecto (p. ej.
  normales generadas). La escala da igual: el encuadre se ajusta al bbox.
- Sin malla y con al menos un clip → `AnimationOnly`. Sin malla ni clips, o
  Assimp falla → `Unreadable`.
- `dependencies` lista las texturas **externas** (no las embebidas) y el sidecar
  del modelo, exista o no todavía (crearlo también invalida).

### `rasterizeThumbnail` (Editor, `Editor/ThumbnailRaster.h/.cpp`, CPU pura)

```cpp
// Casilla kThumbCell x kThumbCell RGBA8 sRGB con alfa. Unreadable si no hay
// ningún triángulo con área o el bbox es degenerado. Determinista.
ThumbnailResult rasterizeThumbnail(const std::vector<PreviewPart>& parts);
```

Lo que validó el spike:
- Vista 3/4 ortográfica (yaw 35°, pitch 25°), ajustada a las 8 esquinas del bbox
  proyectadas, con un 8 % de margen.
- Supersampling 4×4 (256² internos) y reducción con alfa premultiplicado.
- Z-buffer; caras traseras pintadas con la normal invertida (doble cara).
- Albedo: textura muestreada por UV (nearest, convertida a lineal) o color de
  vértice si la parte no tiene textura.
- Luz principal + relleno + ambiente cielo/suelo (imita el IBL); Blinn-Phong con
  `f0 = mix(0.04, albedo, metallic)` y brillo según roughness; el difuso se atenúa
  con metallic.
- Borde oscuro de 1 px alrededor de la silueta, para separarla del fondo del
  botón.
- Sin alpha test.

### `makeAssetThumbnail` (Editor, `Editor/Thumbnail.cpp`)

El nuevo `Decoder` por defecto de `ThumbnailCache`, que despacha por extensión:

| Extensión | Qué hace |
|---|---|
| `.png .jpg .jpeg .tga .bmp` | `makeThumbnail` de hoy. Dependencias: solo el fichero |
| `.fbx .obj` | `loadPreview` → `rasterizeThumbnail`. `AnimationOnly` se propaga |
| `.mat` | `loadMaterialAsset` → esfera UV procedural (48×24) con su albedo, metallic y roughness; lo heredado, neutro. Dependencias: el `.mat` y su textura de albedo |

`ThumbnailResult` gana `std::vector<std::filesystem::path> dependencies` y
`ThumbnailStatus` gana `AnimationOnly`.

### `ThumbnailDiskCache` (Editor, `Editor/ThumbnailDiskCache.h/.cpp`)

```cpp
class ThumbnailDiskCache
{
public:
    explicit ThumbnailDiskCache(std::filesystem::path dir);   // <proyecto>/.dt-cache/thumbs
    // Casilla guardada de `asset` si existe, la magia y la versión casan, y el
    // mtime de CADA dependencia guardada sigue igual. nullopt si no.
    std::optional<ThumbnailResult> load(const std::filesystem::path& asset) const;
    // Escribe a <hash>.tmp y renombra. false = no se pudo (se registra; la
    // miniatura se muestra igual).
    bool store(const std::filesystem::path& asset, const ThumbnailResult& r) const;
};
```

- Un fichero por asset: `<hash de la ruta absoluta normalizada>.bin`.
- Formato: magia, versión, ruta de origen (para descartar una colisión de hash),
  lista de (dependencia, `mtime`) con el propio asset como primera (una
  dependencia que no existe se guarda como «ausente», y que aparezca también
  invalida), estado y, si
  es `Ok`, la casilla de `kThumbCell²·4` bytes.
- Se cachean también `AnimationOnly` y `Unreadable` (un FBX corrupto no se
  reabre en cada sesión; se reintenta cuando cambia su `mtime`).
- Escritura atómica (`.tmp` + `rename`): dos editores sobre el mismo proyecto no
  dejan un fichero a medias.
- Todo en el worker: el hilo principal no lee ni escribe la caché.

### Cambios en `ThumbnailCache`

- El worker hace `disk.load(path)` → si falla, `decode(path)` → `disk.store`. El
  `ThumbnailDiskCache` es opcional (sin él, comportamiento de hoy).
- `Entry` pasa de un `mtime` a una lista de (dependencia, `mtime`). Se rellena
  con el resultado del worker. `refreshStamps()` compara **todas** y regenera si
  cambia cualquiera.
- La clave del atlas se calcula con todas las dependencias y sus `mtime`.
- `status(path)` para que el grid distinga `AnimationOnly` de un fallo.
- Tope en vuelo **por tipo**: 4 en total, de ellos como mucho **2 modelos**. Un
  FBX de varios segundos no debe frenar las miniaturas de texturas.

### Integración en `ContentBrowserPanel`

- Se piden miniaturas para los ítems visibles de tipo `Image`, `Model3D` y
  `Material` (hoy solo `Image`).
- El panel crea el `ThumbnailDiskCache` con la raíz del proyecto al crear el
  `ThumbnailCache`.
- `Ready` → `AddImage` sobre el botón, sin etiqueta, igual que las texturas.
- `AnimationOnly` → recuadro de color con etiqueta **`ANI`** y color propio.
- Pendiente, fallido o sin soporte → el icono de siempre (`3D`, `MAT`). No hay
  spinner.

## Flujo de una petición

1. El grid dibuja un ítem visible y llama a `request(path)`.
2. Entrada nueva → se encola. En el worker: `disk.load`; si acierta, esa casilla
   (sin Assimp ni stb); si no, `makeAssetThumbnail` y `disk.store`.
3. `pump()` recoge el resultado, guarda sus dependencias y sube la casilla al
   atlas.
4. Cada 0,5 s, `refreshStamps()` compara las dependencias de lo visible: reimportar
   la textura de un FBX, editar un `.mat` o pulsar Aplicar en los Import
   Settings de un modelo actualiza su miniatura en ≤0,5 s.

## Errores y casos límite

| Caso | Comportamiento |
|---|---|
| FBX corrupto o ilegible | `Unreadable`, cacheado en disco; icono `3D`; se reintenta al cambiar su `mtime` |
| FBX solo con animación | `AnimationOnly`, cacheado; icono `ANI` |
| `.mat` con textura inexistente | Esfera neutra: es lo que el motor pinta. No es un fallo |
| `.mat` roto | `loadMaterialAsset` ya hereda con aviso → esfera neutra |
| Malla degenerada (sin área) | `Unreadable`, sin NaN ni crash |
| Caché de disco ilegible, truncada o de otra versión | Se ignora y se regenera |
| Colisión de hash | La ruta guardada no casa → se ignora |
| `.dt-cache/thumbs/` no escribible | Se registra una vez; las miniaturas funcionan sin caché |
| Textura externa del FBX cambiada | Regenera por dependencia, aunque el FBX no cambie |
| Cambio de carpeta con modelos cargando | Se descartan por generación, como hoy |

## Testing

**Headless** (`dt_thumbnail_tests`, y `loadPreview` en `model_import_tests.cpp`,
que ya prueba `ModelLoader` con los sidecars de modelo):
- `rasterizeThumbnail`: un quad de frente de color conocido cubre el área
  esperada con ese color en el centro; un cubo queda encuadrado con margen; misma
  entrada → mismos bytes; metallic 1/roughness 0,2 difiere de metallic
  0/roughness 0,9; malla degenerada o vacía → `Unreadable` sin NaN; caras traseras
  pintadas.
- `loadPreview`: `assets/modelTexture.fbx` → `Ok` con **el mismo número de
  triángulos** que `ModelLoader::load` y textura ≤256 px; `standing idle 01.fbx` →
  `AnimationOnly`; bytes basura → `Unreadable`; un sidecar que cambia las normales
  cambia el resultado.
- `.mat`: con albedo, el píxel central toma el color de la textura; sin albedo,
  el gris neutro. Los valores de prueba se eligen **distintos de los defaults**.
- `ThumbnailDiskCache`: ida y vuelta byte a byte; fallo por `mtime` de una
  dependencia, por versión, por fichero truncado y por ruta distinta; directorio
  no escribible sin romper; no quedan `.tmp`.
- `ThumbnailCache`: cambiar una dependencia (no el asset) regenera tras
  `refreshStamps`; `status()` devuelve `AnimationOnly`; tope de 2 modelos en vuelo
  con 4 huecos.
- **Sabotaje uno a uno** de la comprobación de dependencias, la escritura atómica
  y el tope por tipo: cada test debe fallar al quitar su guarda.

**Manual** (Release: Debug infla ~26× el coste de las dependencias):
- Vulkan y D3D12: `assets/` y `assets/animatedCharacter/` muestran las mismas
  miniaturas en los dos backends.
- `Maw J Laygo.fbx` contra el modelo arrastrado al viewport.
- Segunda apertura del editor: miniaturas al instante, sin Assimp (medido).
- Aplicar Import Settings en un FBX y editar un `.mat`: la miniatura cambia en
  ≤0,5 s.
- Sin tirones al entrar en `animatedCharacter/` (13 FBX).
- `loadPreview` contra `loadAuto` en Release sobre `modelAnimation.fbx`. Si no
  baja claramente, se replantea la lectura ligera.

## Riesgos

1. **`loadPreview` puede no ser tan ligero como se espera** si el coste está en
   el parseo de Assimp y no en el post-proceso de animaciones. La caché en disco
   lo amortiza igualmente; la medición en Release decide si hace falta más.
2. **Divergencia con el render real.** El rasterizador no es PBR (sin IBL, sin
   normal map). Aceptado por diseño; el riesgo es un modelo cuyo aspecto dependa
   de eso, y la verificación manual lo compara contra el viewport.
3. **Memoria en el worker.** Un FBX grande con texturas embebidas enormes se
   decodifica entero antes de reducirse. Se aplica el mismo tope de 100 MP por
   textura que ya usa `makeThumbnail`: por encima, la parte va sin textura.
