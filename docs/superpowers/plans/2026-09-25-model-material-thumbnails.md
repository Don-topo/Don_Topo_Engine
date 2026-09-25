# Miniaturas de modelos y materiales — Plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** El grid del Content Browser muestra una miniatura real de cada `.fbx`/`.obj` y `.mat` (rasterizada en CPU) y guarda todas las miniaturas, también las de texturas, en una caché en disco invalidada por dependencias.

**Architecture:** `ModelLoader::loadPreview` (Core) lee con Assimp solo la geometría en bind pose y la textura difusa reducida. `rasterizeThumbnail` (Editor, CPU pura) la pinta en una casilla de 64×64. `makeAssetThumbnail` es el nuevo `Decoder` de `ThumbnailCache` y despacha por extensión (imagen, modelo, `.mat`). `ThumbnailDiskCache` guarda cada casilla con la lista de sus dependencias y sus `mtime`. `ThumbnailCache` pasa de un `mtime` a una lista de dependencias, consulta la caché de disco en el worker y limita los modelos en vuelo. Ningún código GPU nuevo: el atlas y la subida no cambian.

**Tech Stack:** C++20, Assimp, stb_image, glm, ImGui. Tests planos `main()` + `CHECK`, `dt_add_test`.

**Spec:** `docs/superpowers/specs/2026-09-25-model-material-thumbnails-design.md`

## Global Constraints

- **Sin dependencias de terceros nuevas.** Sin código GPU nuevo en ningún backend.
- **Casilla**: `kThumbCell` = 64, RGBA8 en sRGB con alfa (`Renderer/ThumbnailAtlas.h`, sin tocar).
- **Rasterizado**: vista 3/4 ortográfica (yaw 35°, pitch 25°) ajustada al bbox con 8 % de margen, supersampling 4×4, doble cara, **sin alpha test**, borde oscuro de 1 px.
- **`.mat` que hereda**: albedo neutro gris claro (`kNeutralAlbedo` = 0.6 lineal), metallic 0, roughness 0.5.
- **Textura de previsualización**: lado mayor ≤ 256 px; origen de más de 100 MP (`kPreviewMaxSourcePixels` = 100'000'000) → la parte va sin textura.
- **Caché en disco**: `<proyecto>/.dt-cache/thumbs/<fnv1a64 hex>.bin`; escritura a `.tmp` + `rename`; versión `kThumbDiskVersion` que **se sube siempre que cambie el aspecto** de las miniaturas.
- **En vuelo**: 4 decodificaciones en total, como mucho **2 modelos**.
- **Qué pinta el preview = qué pinta el motor**: un modelo con huesos, todas sus mallas (como `loadSkinned`); uno sin huesos, **solo la primera** (como `load`).
- **Avisos por `stderr`** con prefijo `[Thumbnails]` (mismo canal que `[ModelImport]`).
- **CRLF**: el repo va en CRLF. Tras cada `Edit`, `git diff --stat` para ver que el diff es del tamaño esperado. Nada de `sed -i` ni Get-Content/Set-Content de PowerShell.
- **Los tests se ejecutan desde la raíz del repo** (`.\build-ninja\engine\tests\dt_xxx.exe`): `assets/` no resuelve desde otro sitio.
- **Build**: `.\build.bat > $env:TEMP\b.log 2>&1` desde PowerShell; leer solo los errores. Si `Sandbox.exe` da `LNK1201`, hay uno vivo: `Get-Process Sandbox -ErrorAction SilentlyContinue | Stop-Process -Force` y repetir. Ninja regenera solo al cambiar un `CMakeLists.txt`.
- **Linux CI compila esto con GCC 13**: incluir explícitamente lo que se use (`<cstring>`, `<atomic>`, `<thread>`, `<limits>`…).
- **Suite completa**: `total 35` antes y después (no se añade ningún ejecutable de test). Correrla con el snippet de abajo, en segundo plano.

## Cómo correr la suite (snippet)

```powershell
Set-Location C:\Users\ruben\Documents\Don_Topo_Engine
$fail = @(); $n = 0
Get-ChildItem .\build-ninja\engine\tests\dt_*.exe | ForEach-Object {
    $n++
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $_.FullName; $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    $p = [Diagnostics.Process]::Start($psi)
    $so = $p.StandardOutput.ReadToEndAsync(); $se = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(400000)) { $p.Kill(); $fail += "$($_.Name) HUNG" }
    else { $p.WaitForExit(); if ($p.ExitCode -ne 0) { $fail += "$($_.Name) ($($p.ExitCode))" } }
}
"SUMMARY total $n, fallos: $($fail -join ', ')"
$p = Start-Process .\build-ninja\sandbox\Sandbox.exe -WorkingDirectory .\build-ninja\sandbox -PassThru
Start-Sleep 7
if ($p.HasExited) { "SANDBOX EXITED early code $($p.ExitCode)" }
else { "SANDBOX alive after 7s"; $p.CloseMainWindow() | Out-Null; Start-Sleep 2; if (-not $p.HasExited) { $p.Kill() } }
```

Guardarlo en el scratchpad de la sesión y lanzarlo con `run_in_background: true`. Esperado: `fallos:` vacío y `SANDBOX alive after 7s`.

## Review Focus

1. **Un FBX que cambia MIENTRAS se decodifica** (el usuario reexporta con el editor abierto): la miniatura no puede quedarse vieja para siempre. El `mtime` del asset se toma **antes** de decodificar, así que el cambio se ve en el siguiente `refreshStamps`. → Task 5 (`test_cache_change_during_decode_regenerates`).
2. **La textura externa de un modelo o de un `.mat` cambia sin tocar el asset**: la miniatura se regenera. → Task 3 (dependencias declaradas) y Task 5 (`test_cache_dependency_change_regenerates`).
3. **Caché de disco hostil**: truncada, de otra versión, de otra ruta con el mismo hash, o en un directorio no escribible. Nunca rompe ni enseña una miniatura ajena. → Task 4.
4. **Un modelo pesado no deja sin miniaturas a las texturas**: con 4 modelos y 2 imágenes pedidos a la vez, las imágenes arrancan en la primera tanda. → Task 5 (`test_cache_caps_models_in_flight`).
5. **Geometría basura** (NaN, triángulos sin área, índices fuera de rango, fichero sin mallas): sin crash ni NaN en la casilla; `Unreadable` o `AnimationOnly`, no un fallo mudo. → Task 1 y Task 2.

---

## File Structure

| Fichero | Responsabilidad |
|---|---|
| `engine/include/DonTopo/Renderer/ModelLoader.h`, `engine/src/Renderer/ModelLoader.cpp` (modif.) | `PreviewStatus`, `PreviewImage`, `PreviewPart`, `ModelPreview`, `loadPreview`, `loadPreviewImage`, `decodePreviewImage` |
| `engine/include/DonTopo/Editor/ThumbnailRaster.h`, `engine/src/Editor/ThumbnailRaster.cpp` (nuevos) | `rasterizeThumbnail`, `makePreviewSphere` |
| `engine/include/DonTopo/Editor/Thumbnail.h`, `engine/src/Editor/Thumbnail.cpp` (modif.) | `ThumbnailStatus::AnimationOnly`, `ThumbnailDependency`, `stampFile`, `stampDependencies`, `isModelThumbnailPath`, `makeMaterialThumbnail`, `makeAssetThumbnail`; `ThumbnailCache` con dependencias, caché de disco, `status()` y tope de modelos |
| `engine/include/DonTopo/Editor/ThumbnailDiskCache.h`, `engine/src/Editor/ThumbnailDiskCache.cpp` (nuevos) | Formato binario, `load`/`store` atómico |
| `engine/include/DonTopo/Editor/ContentBrowserPanel.h`, `engine/src/Editor/ContentBrowserPanel.cpp` (modif.) | `wantsThumbnail`, caché de disco del proyecto, etiqueta `ANI` |
| `engine/CMakeLists.txt` (modif.) | Los dos `.cpp` nuevos en la lista del editor |
| `engine/tests/model_import_tests.cpp`, `engine/tests/thumbnail_tests.cpp` (modif.) | Tests |
| `README.md`, `docs/assets-editor-audit.md` (modif.) | Documentación |

---

### Task 1: `ModelLoader::loadPreview` y la textura de previsualización

**Files:**
- Modify: `engine/include/DonTopo/Renderer/ModelLoader.h`
- Modify: `engine/src/Renderer/ModelLoader.cpp` (tras `loadAuto`, al final del namespace)
- Test: `engine/tests/model_import_tests.cpp`

**Interfaces:**
- Consumes: `readModelSettings`, `configureImporter`, `assimpFlags` (estáticas de `ModelLoader.cpp`); `importSidecarPath` (`Core/ImportSettings.h`).
- Produces (en `namespace DonTopo`, `ModelLoader.h`):

```cpp
inline constexpr int      kPreviewMaxTexture       = 256;
inline constexpr uint64_t kPreviewMaxSourcePixels  = 100'000'000;

enum class PreviewStatus { Ok, AnimationOnly, Unreadable };

struct PreviewImage
{
    int                  w = 0, h = 0;
    std::vector<uint8_t> rgba;          // w*h*4, sRGB tal cual el fichero; vacía = sin textura
};

struct PreviewPart
{
    std::vector<glm::vec3> positions, normals, colors;
    std::vector<glm::vec2> uvs;
    std::vector<uint32_t>  indices;
    PreviewImage           albedo;
    float                  metallic  = 0.0f;
    float                  roughness = 0.5f;
};

struct ModelPreview
{
    PreviewStatus                      status = PreviewStatus::Unreadable;
    std::vector<PreviewPart>           parts;
    std::vector<std::filesystem::path> dependencies;   // sidecar + texturas externas
};

// dentro de class ModelLoader:
static ModelPreview loadPreview(const std::string& path);                      // nunca lanza
static PreviewImage loadPreviewImage(const std::filesystem::path& path);       // nunca lanza
static PreviewImage decodePreviewImage(const uint8_t* bytes, size_t size);     // nunca lanza
```

- [ ] **Step 1: Escribir los tests que fallan**

En `engine/tests/model_import_tests.cpp`, añadir tras `writeSettings` un escritor de TGA (el mismo formato que usa `thumbnail_tests.cpp`) y los tests. Añadir `#include <array>`, `#include <functional>` y `#include <memory>` arriba.

```cpp
using Rgba = std::array<uint8_t, 4>;

// TGA sin comprimir de 32 bits, origen arriba a la izquierda.
static void writeTga(const fs::path& p, int w, int h, const std::function<Rgba(int, int)>& pixel)
{
    std::ofstream f(p, std::ios::binary);
    uint8_t hdr[18] = {};
    hdr[2]  = 2;
    hdr[12] = static_cast<uint8_t>(w & 0xFF);
    hdr[13] = static_cast<uint8_t>((w >> 8) & 0xFF);
    hdr[14] = static_cast<uint8_t>(h & 0xFF);
    hdr[15] = static_cast<uint8_t>((h >> 8) & 0xFF);
    hdr[16] = 32;
    hdr[17] = 0x28;
    f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const Rgba c = pixel(x, y);
            const uint8_t bgra[4] = { c[2], c[1], c[0], c[3] };
            f.write(reinterpret_cast<const char*>(bgra), 4);
        }
}

static size_t previewTriangles(const ModelPreview& p)
{
    size_t n = 0;
    for (const PreviewPart& part : p.parts) n += part.indices.size() / 3;
    return n;
}

static bool hasDependency(const ModelPreview& p, const fs::path& dep)
{
    for (const fs::path& d : p.dependencies)
        if (sameAssetPath(d, dep)) return true;
    return false;
}

// El preview pinta lo mismo que el motor: mismo numero de triangulos que loadAuto,
// en un modelo estatico con textura y en un personaje con varias submallas.
static void test_preview_matches_the_engine_triangle_count()
{
    for (const char* file : { "assets/modelTexture.fbx", "assets/modelAnimation.fbx" })
    {
        const std::shared_ptr<Mesh> engine = ModelLoader::loadAuto(file);
        const ModelPreview preview = ModelLoader::loadPreview(file);
        CHECK(preview.status == PreviewStatus::Ok);
        CHECK(engine && !engine->indices.empty());
        if (!engine) continue;
        CHECK(previewTriangles(preview) == engine->indices.size() / 3);
    }
    // El personaje trae varias submallas: una parte por cada una.
    const ModelPreview character = ModelLoader::loadPreview("assets/modelAnimation.fbx");
    CHECK(character.parts.size() > 1);
}

// La textura embebida se reduce: lado mayor <= 256.
static void test_preview_texture_is_downscaled()
{
    const ModelPreview p = ModelLoader::loadPreview("assets/modelTexture.fbx");
    bool textured = false;
    for (const PreviewPart& part : p.parts)
    {
        if (part.albedo.rgba.empty()) continue;
        textured = true;
        CHECK(std::max(part.albedo.w, part.albedo.h) <= kPreviewMaxTexture);
        CHECK(part.albedo.rgba.size() == static_cast<size_t>(part.albedo.w) * part.albedo.h * 4);
    }
    CHECK(textured);   // precondicion: el fixture trae textura
}

// Un FBX que solo trae animacion (Mixamo "without skin") no es un fallo.
static void test_preview_animation_only_file()
{
    const ModelPreview p = ModelLoader::loadPreview("assets/animatedCharacter/standing idle 01.fbx");
    CHECK(p.status == PreviewStatus::AnimationOnly);
    CHECK(p.parts.empty());
}

static void test_preview_garbage_and_missing_are_unreadable()
{
    const fs::path dir = makeDir("dt_model_preview_garbage");
    writeText(dir / "basura.fbx", "esto no es un fbx");
    CHECK(ModelLoader::loadPreview((dir / "basura.fbx").string()).status == PreviewStatus::Unreadable);
    CHECK(ModelLoader::loadPreview((dir / "no_existe.obj").string()).status == PreviewStatus::Unreadable);
}

// Textura externa: se lee, se reduce conservando la proporcion y es una dependencia.
// El sidecar tambien lo es aunque todavia no exista.
static void test_preview_external_texture_and_sidecar_are_dependencies()
{
    const fs::path dir = makeDir("dt_model_preview_external");
    writeTga(dir / "rojo.tga", 512, 128, [](int, int) { return Rgba{ 255, 0, 0, 255 }; });
    writeText(dir / "quad.mtl", "newmtl m\nmap_Kd rojo.tga\n");
    writeText(dir / "quad.obj",
              "mtllib quad.mtl\nusemtl m\n"
              "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
              "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
              "f 1/1 2/2 3/3\nf 1/1 3/3 4/4\n");
    const fs::path obj = dir / "quad.obj";
    const ModelPreview p = ModelLoader::loadPreview(obj.string());
    CHECK(p.status == PreviewStatus::Ok);
    CHECK(p.parts.size() == 1);
    if (p.parts.size() != 1) return;
    const PreviewImage& img = p.parts[0].albedo;
    CHECK(img.w == 256 && img.h == 64);
    if (!img.rgba.empty()) CHECK(img.rgba[0] == 255 && img.rgba[1] == 0 && img.rgba[2] == 0);
    CHECK(hasDependency(p, dir / "rojo.tga"));
    CHECK(hasDependency(p, importSidecarPath(obj)));
}

// El sidecar cambia el aspecto: normals = flat regenera las normales del fichero.
static void test_preview_respects_the_normals_setting()
{
    const fs::path obj = writeObj("dt_model_preview_normals", kFoldedObjWithNormals);
    auto hasTiltedNormal = [](const ModelPreview& p) {
        for (const PreviewPart& part : p.parts)
            for (const glm::vec3& n : part.normals)
                if (glm::length(n - kFaceNormal2) < 1e-3f) return true;
        return false;
    };
    CHECK(!hasTiltedNormal(ModelLoader::loadPreview(obj.string())));   // las del fichero: todas +Z
    ModelImportSettings s;
    s.normals = NormalsMode::Flat;
    writeSettings(obj, s);
    CHECK(hasTiltedNormal(ModelLoader::loadPreview(obj.string())));
}

// Una textura enorme se rechaza por su CABECERA: 65535 x 65535 sin cuerpo.
static void test_preview_image_rejects_huge_sources()
{
    const fs::path dir = makeDir("dt_model_preview_huge");
    std::ofstream f(dir / "huge.tga", std::ios::binary);
    uint8_t hdr[18] = {};
    hdr[2] = 2; hdr[12] = 0xFF; hdr[13] = 0xFF; hdr[14] = 0xFF; hdr[15] = 0xFF; hdr[16] = 32; hdr[17] = 0x28;
    f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    f.close();
    CHECK(ModelLoader::loadPreviewImage(dir / "huge.tga").rgba.empty());
    CHECK(ModelLoader::loadPreviewImage(dir / "no_existe.png").rgba.empty());
}
```

Y en `main()`, antes del `if (g_failures == 0)`:

```cpp
    test_preview_matches_the_engine_triangle_count();
    test_preview_texture_is_downscaled();
    test_preview_animation_only_file();
    test_preview_garbage_and_missing_are_unreadable();
    test_preview_external_texture_and_sidecar_are_dependencies();
    test_preview_respects_the_normals_setting();
    test_preview_image_rejects_huge_sources();
```

- [ ] **Step 2: Compilar y ver que falla**

Run: `.\build.bat > $env:TEMP\b.log 2>&1` y buscar `error` en el log.
Expected: error de compilación, `loadPreview`/`PreviewStatus` no declarados.

- [ ] **Step 3: Declarar la API en `ModelLoader.h`**

Añadir `#include <cstdint>`, `#include <filesystem>`, `#include <vector>` y `#include <glm/glm.hpp>`. Antes de `class ModelLoader`, los tipos y constantes del bloque *Produces* de arriba, con este comentario encima de `PreviewPart`:

```cpp
    // Lo minimo para pintar una miniatura: geometria en bind pose y el albedo
    // reducido. Sin animaciones, esqueleto, normal map ni ORM (ver loadPreview).
```

Y dentro de la clase, tras `loadAuto`:

```cpp
            // Lectura ligera para las miniaturas del Content Browser. Pinta lo
            // mismo que el motor (con huesos, todas las mallas como loadSkinned;
            // sin huesos, solo la primera como load) y respeta el sidecar en lo
            // que cambia el aspecto. Sin malla pero con clips -> AnimationOnly.
            // Nunca lanza.
            static ModelPreview loadPreview(const std::string& path);

            // Textura reducida a kPreviewMaxTexture en el lado mayor, conservando
            // la proporcion. Vacia si no se puede leer o si pasa de
            // kPreviewMaxSourcePixels (se mira la cabecera antes de decodificar).
            static PreviewImage loadPreviewImage(const std::filesystem::path& path);
            static PreviewImage decodePreviewImage(const uint8_t* bytes, size_t size);
```

- [ ] **Step 4: Implementar en `ModelLoader.cpp`**

Añadir `#include <stb_image.h>`, `#include <fstream>`, `#include <iterator>`, `#include <unordered_map>`, `#include <algorithm>`. Al final del namespace:

```cpp
    namespace
    {
        // Filtro de caja a kPreviewMaxTexture en el lado mayor. Lo que ya cabe no se toca.
        PreviewImage downscalePreview(const uint8_t* px, int w, int h)
        {
            PreviewImage out;
            int dw = w, dh = h;
            if (std::max(w, h) > kPreviewMaxTexture)
            {
                const double s = static_cast<double>(kPreviewMaxTexture) / std::max(w, h);
                dw = std::clamp(static_cast<int>(w * s + 0.5), 1, kPreviewMaxTexture);
                dh = std::clamp(static_cast<int>(h * s + 0.5), 1, kPreviewMaxTexture);
            }
            out.w = dw;
            out.h = dh;
            out.rgba.assign(static_cast<size_t>(dw) * dh * 4, 0);
            for (int y = 0; y < dh; ++y)
            {
                const int y0 = static_cast<int>(static_cast<int64_t>(y) * h / dh);
                const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<int64_t>(y + 1) * h / dh));
                for (int x = 0; x < dw; ++x)
                {
                    const int x0 = static_cast<int>(static_cast<int64_t>(x) * w / dw);
                    const int x1 = std::max(x0 + 1, static_cast<int>(static_cast<int64_t>(x + 1) * w / dw));
                    uint64_t sum[4] = {};
                    for (int sy = y0; sy < y1; ++sy)
                        for (int sx = x0; sx < x1; ++sx)
                            for (int c = 0; c < 4; ++c)
                                sum[c] += px[(static_cast<size_t>(sy) * w + sx) * 4 + c];
                    const uint64_t n = static_cast<uint64_t>(x1 - x0) * (y1 - y0);
                    for (int c = 0; c < 4; ++c)
                        out.rgba[(static_cast<size_t>(y) * dw + x) * 4 + c] = static_cast<uint8_t>((sum[c] + n / 2) / n);
                }
            }
            return out;
        }
    }

    PreviewImage ModelLoader::decodePreviewImage(const uint8_t* bytes, size_t size)
    {
        if (!bytes || size == 0 || size > static_cast<size_t>(INT32_MAX)) return {};
        int w = 0, h = 0, comp = 0;
        if (!stbi_info_from_memory(bytes, static_cast<int>(size), &w, &h, &comp) || w <= 0 || h <= 0)
            return {};
        if (static_cast<uint64_t>(w) * static_cast<uint64_t>(h) > kPreviewMaxSourcePixels) return {};
        std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> px(
            stbi_load_from_memory(bytes, static_cast<int>(size), &w, &h, &comp, 4), &stbi_image_free);
        if (!px) return {};
        return downscalePreview(px.get(), w, h);
    }

    PreviewImage ModelLoader::loadPreviewImage(const std::filesystem::path& path)
    {
        try
        {
            // ifstream y no stbi_load: stbi_load recibe un char* en la codepage
            // local y falla con rutas Unicode.
            std::ifstream in(path, std::ios::binary);
            if (!in) return {};
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            return decodePreviewImage(bytes.data(), bytes.size());
        }
        catch (...) { return {}; }
    }

    ModelPreview ModelLoader::loadPreview(const std::string& path)
    {
        namespace fs = std::filesystem;
        ModelPreview out;
        try
        {
            // El sidecar es dependencia exista o no: crearlo tambien cambia el aspecto.
            out.dependencies.push_back(importSidecarPath(path));

            const ModelImportSettings settings = readModelSettings(path);
            Assimp::Importer importer;
            configureImporter(importer, settings);
            // Sin tangentes: la miniatura no usa normal map.
            const aiScene* scene = importer.ReadFile(path, assimpFlags(settings) & ~aiProcess_CalcTangentSpace);
            if (!scene || !scene->mRootNode) return out;
            if (scene->mNumMeshes == 0)
            {
                // Assimp marca INCOMPLETE un fichero sin mallas: con clips es un
                // FBX de solo animacion, no uno roto.
                if (scene->mNumAnimations > 0) out.status = PreviewStatus::AnimationOnly;
                return out;
            }
            if (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) return out;

            bool skinned = false;
            for (uint32_t m = 0; m < scene->mNumMeshes; ++m)
                skinned = skinned || scene->mMeshes[m]->mNumBones > 0;
            const uint32_t meshCount = skinned ? scene->mNumMeshes : 1;

            const fs::path modelDir = fs::path(path).parent_path();
            std::unordered_map<uint32_t, PreviewImage> albedoByMaterial;
            auto albedoOf = [&](uint32_t matIndex) -> const PreviewImage& {
                auto it = albedoByMaterial.find(matIndex);
                if (it != albedoByMaterial.end()) return it->second;
                PreviewImage img;
                aiString texPath;
                if (matIndex < scene->mNumMaterials &&
                    scene->mMaterials[matIndex]->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS)
                {
                    const aiTexture* emb = scene->GetEmbeddedTexture(texPath.C_Str());
                    if (emb && emb->mHeight == 0)
                    {
                        img = decodePreviewImage(reinterpret_cast<const uint8_t*>(emb->pcData), emb->mWidth);
                    }
                    else if (emb)
                    {
                        std::vector<uint8_t> raw(static_cast<size_t>(emb->mWidth) * emb->mHeight * 4);
                        for (size_t k = 0; k < static_cast<size_t>(emb->mWidth) * emb->mHeight; ++k)
                        {
                            raw[k * 4 + 0] = emb->pcData[k].r;
                            raw[k * 4 + 1] = emb->pcData[k].g;
                            raw[k * 4 + 2] = emb->pcData[k].b;
                            raw[k * 4 + 3] = emb->pcData[k].a;
                        }
                        if (static_cast<uint64_t>(emb->mWidth) * emb->mHeight <= kPreviewMaxSourcePixels)
                            img = downscalePreview(raw.data(), static_cast<int>(emb->mWidth), static_cast<int>(emb->mHeight));
                    }
                    else
                    {
                        // Misma resolucion que load/loadSkinned: el nombre, junto al modelo.
                        const fs::path ext = modelDir / fs::path(texPath.C_Str()).filename();
                        out.dependencies.push_back(ext);
                        img = loadPreviewImage(ext);
                    }
                }
                return albedoByMaterial.emplace(matIndex, std::move(img)).first->second;
            };

            for (uint32_t m = 0; m < meshCount; ++m)
            {
                const aiMesh* ai = scene->mMeshes[m];
                PreviewPart part;
                part.positions.reserve(ai->mNumVertices);
                for (uint32_t i = 0; i < ai->mNumVertices; ++i)
                {
                    part.positions.emplace_back(ai->mVertices[i].x * settings.scale,
                                                ai->mVertices[i].y * settings.scale,
                                                ai->mVertices[i].z * settings.scale);
                    part.normals.push_back(ai->mNormals
                        ? glm::vec3(ai->mNormals[i].x, ai->mNormals[i].y, ai->mNormals[i].z)
                        : glm::vec3(0.0f, 1.0f, 0.0f));
                    part.uvs.push_back(ai->mTextureCoords[0]
                        ? glm::vec2(ai->mTextureCoords[0][i].x, ai->mTextureCoords[0][i].y)
                        : glm::vec2(0.0f));
                    part.colors.emplace_back(1.0f);
                }
                for (uint32_t f = 0; f < ai->mNumFaces; ++f)
                    if (ai->mFaces[f].mNumIndices == 3)
                        for (uint32_t j = 0; j < 3; ++j) part.indices.push_back(ai->mFaces[f].mIndices[j]);
                part.albedo = albedoOf(ai->mMaterialIndex);
                out.parts.push_back(std::move(part));
            }
            out.status = PreviewStatus::Ok;
        }
        catch (...)
        {
            out.status = PreviewStatus::Unreadable;
            out.parts.clear();
        }
        return out;
    }
```

- [ ] **Step 5: Compilar y ejecutar**

Run: `.\build.bat > $env:TEMP\b.log 2>&1` y luego, desde la raíz, `.\build-ninja\engine\tests\dt_model_import_tests.exe`.
Expected: `ALL MODEL IMPORT TESTS PASSED`.

Si falla `test_preview_animation_only_file` porque el fichero SÍ trae mallas, no se debilita el test: comprobar con un `std::printf` temporal de `mNumMeshes`/`mNumAnimations` y buscar en `assets/animatedCharacter/` un FBX con 0 mallas y ≥1 animación; si no hay ninguno, parar e informar.

- [ ] **Step 6: Sabotaje**

Uno a uno, compilar y ejecutar, y revertir: (a) quitar el `if (scene->mNumAnimations > 0)` → debe fallar `test_preview_animation_only_file`; (b) cambiar `meshCount` a `scene->mNumMeshes` siempre → debe fallar `test_preview_matches_the_engine_triangle_count` **solo si** `modelTexture.fbx` tiene más de una malla; si no falla, anotarlo en el informe (el fixture no cubre ese caso) y seguir; (c) quitar `out.dependencies.push_back(ext)` → debe fallar `test_preview_external_texture_and_sidecar_are_dependencies`.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Renderer/ModelLoader.h engine/src/Renderer/ModelLoader.cpp engine/tests/model_import_tests.cpp
git commit -m "feat(renderer): ModelLoader::loadPreview, lectura ligera de un modelo para su miniatura"
```

---

### Task 2: `rasterizeThumbnail` y la esfera de materiales

**Files:**
- Create: `engine/include/DonTopo/Editor/ThumbnailRaster.h`
- Create: `engine/src/Editor/ThumbnailRaster.cpp`
- Modify: `engine/CMakeLists.txt` (tras `src/Editor/Thumbnail.cpp`, línea ~157)
- Test: `engine/tests/thumbnail_tests.cpp`

**Interfaces:**
- Consumes: `PreviewPart`, `PreviewImage` (Task 1); `ThumbnailResult`, `ThumbnailStatus`, `kThumbCell` (`Editor/Thumbnail.h`).
- Produces:

```cpp
namespace DonTopo {
// Casilla kThumbCell x kThumbCell RGBA8 sRGB con alfa. Unreadable si ningun
// triangulo llega a cubrir un pixel (vacio, sin area, NaN, indices fuera de
// rango). Determinista: misma entrada, mismos bytes. dependencies vacia.
ThumbnailResult rasterizeThumbnail(const std::vector<PreviewPart>& parts);
// Esfera UV de radio 1 (48 x 24), colores blancos, uv con la textura dos veces
// alrededor. Base de las miniaturas de .mat.
PreviewPart makePreviewSphere();
}
```

- [ ] **Step 1: Escribir los tests que fallan**

En `engine/tests/thumbnail_tests.cpp` añadir `#include "DonTopo/Editor/ThumbnailRaster.h"` y `#include <limits>`, y antes de `main()`:

```cpp
// ── rasterizeThumbnail ───────────────────────────────────────────────────────

static PreviewImage solidImage(Rgba c)
{
    PreviewImage img;
    img.w = img.h = 1;
    img.rgba = { c[0], c[1], c[2], c[3] };
    return img;
}

static PreviewPart cubePart()
{
    PreviewPart p;
    const glm::vec3 n[6] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
    for (const glm::vec3& f : n)
    {
        const glm::vec3 u = std::abs(f.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        const glm::vec3 v = glm::cross(f, u);
        const uint32_t base = static_cast<uint32_t>(p.positions.size());
        for (const glm::vec2 c : { glm::vec2(-1, -1), glm::vec2(1, -1), glm::vec2(1, 1), glm::vec2(-1, 1) })
        {
            p.positions.push_back(f + u * c.x + v * c.y);
            p.normals.push_back(f);
            p.uvs.emplace_back(0.5f);
            p.colors.emplace_back(1.0f);
        }
        p.indices.insert(p.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
    return p;
}

static bool reddish(const Rgba& c) { return c[3] == 255 && c[0] > c[1] + 40 && c[0] > c[2] + 40; }

static void test_raster_sphere_takes_the_albedo_color()
{
    PreviewPart s = makePreviewSphere();
    s.albedo = solidImage({ 255, 0, 0, 255 });
    const ThumbnailResult r = rasterizeThumbnail({ s });
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    CHECK(reddish(pixelAt(r, 32, 32)));
    CHECK(isClear(pixelAt(r, 0, 0)));
}

static void test_raster_uses_vertex_color_without_texture()
{
    PreviewPart s = makePreviewSphere();
    for (glm::vec3& c : s.colors) c = glm::vec3(0.0f, 1.0f, 0.0f);
    const ThumbnailResult r = rasterizeThumbnail({ s });
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    const Rgba c = pixelAt(r, 32, 32);
    CHECK(c[1] > c[0] + 40 && c[1] > c[2] + 40);
}

// Encuadre: el cubo ocupa la casilla con margen y no toca el borde.
static void test_raster_frames_the_bbox_with_a_margin()
{
    const ThumbnailResult r = rasterizeThumbnail({ cubePart() });
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    int minX = 64, maxX = -1, minY = 64, maxY = -1;
    for (uint32_t y = 0; y < kThumbCell; ++y)
        for (uint32_t x = 0; x < kThumbCell; ++x)
            if (pixelAt(r, x, y)[3] > 200)
            {
                minX = std::min<int>(minX, x); maxX = std::max<int>(maxX, x);
                minY = std::min<int>(minY, y); maxY = std::max<int>(maxY, y);
            }
    CHECK(minX >= 1 && minY >= 1 && maxX <= 62 && maxY <= 62);    // margen
    CHECK(std::max(maxX - minX, maxY - minY) >= 52);               // y aun asi llena la casilla
}

static void test_raster_is_deterministic()
{
    PreviewPart s = makePreviewSphere();
    s.albedo = solidImage({ 10, 200, 90, 255 });
    CHECK(rasterizeThumbnail({ s }).rgba == rasterizeThumbnail({ s }).rgba);
}

static void test_raster_metallic_and_roughness_change_the_result()
{
    PreviewPart shiny = makePreviewSphere();
    shiny.metallic = 1.0f; shiny.roughness = 0.2f;
    PreviewPart matte = makePreviewSphere();
    matte.metallic = 0.0f; matte.roughness = 0.9f;
    CHECK(rasterizeThumbnail({ shiny }).rgba != rasterizeThumbnail({ matte }).rgba);
}

// Las dos caras de un triangulo se pintan igual (el motor no hace culling en la miniatura).
static void test_raster_draws_back_faces()
{
    auto tri = [](bool flip) {
        PreviewPart p;
        p.positions = { { -1, -1, 0 }, { 1, -1, 0 }, { 0, 1, 0 } };
        p.normals   = { { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 } };
        p.uvs       = { {}, {}, {} };
        p.colors    = { glm::vec3(1), glm::vec3(1), glm::vec3(1) };
        p.indices   = flip ? std::vector<uint32_t>{ 0, 2, 1 } : std::vector<uint32_t>{ 0, 1, 2 };
        return p;
    };
    auto covered = [](const ThumbnailResult& r) {
        int n = 0;
        for (size_t i = 3; i < r.rgba.size(); i += 4) n += r.rgba[i] == 255;
        return n;
    };
    const ThumbnailResult a = rasterizeThumbnail({ tri(false) });
    const ThumbnailResult b = rasterizeThumbnail({ tri(true) });
    CHECK(covered(a) > 100);
    CHECK(covered(a) == covered(b));
}

// Review Focus 5: basura -> Unreadable, sin NaN ni crash.
static void test_raster_degenerate_input_is_unreadable()
{
    CHECK(rasterizeThumbnail({}).status == ThumbnailStatus::Unreadable);

    PreviewPart point;
    point.positions = { { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 } };
    point.indices   = { 0, 1, 2 };
    CHECK(rasterizeThumbnail({ point }).status == ThumbnailStatus::Unreadable);

    PreviewPart nan = point;
    const float q = std::numeric_limits<float>::quiet_NaN();
    nan.positions = { { q, 0, 0 }, { 0, q, 0 }, { 0, 0, q } };
    CHECK(rasterizeThumbnail({ nan }).status == ThumbnailStatus::Unreadable);

    PreviewPart outOfRange = cubePart();
    outOfRange.indices = { 0, 1, 999 };
    CHECK(rasterizeThumbnail({ outOfRange }).status == ThumbnailStatus::Unreadable);

    // Un triangulo bueno y uno con NaN: el bueno se pinta y no hay NaN en la salida.
    PreviewPart mixed = cubePart();
    mixed.positions.push_back({ q, q, q });
    const uint32_t bad = static_cast<uint32_t>(mixed.positions.size() - 1);
    mixed.normals.push_back({ 0, 1, 0 }); mixed.uvs.emplace_back(0.0f); mixed.colors.emplace_back(1.0f);
    mixed.indices.insert(mixed.indices.end(), { 0, 1, bad });
    CHECK(rasterizeThumbnail({ mixed }).status == ThumbnailStatus::Ok);
}
```

Y en `main()`, antes de `test_icon_button_id_is_stable_when_thumbnail_appears();`:

```cpp
    test_raster_sphere_takes_the_albedo_color();
    test_raster_uses_vertex_color_without_texture();
    test_raster_frames_the_bbox_with_a_margin();
    test_raster_is_deterministic();
    test_raster_metallic_and_roughness_change_the_result();
    test_raster_draws_back_faces();
    test_raster_degenerate_input_is_unreadable();
```

- [ ] **Step 2: Compilar y ver que falla**

Run: `.\build.bat > $env:TEMP\b.log 2>&1`.
Expected: `ThumbnailRaster.h` no existe.

- [ ] **Step 3: Crear `ThumbnailRaster.h`**

```cpp
#pragma once
#include "DonTopo/Editor/Thumbnail.h"
#include "DonTopo/Renderer/ModelLoader.h"

#include <vector>

// Rasterizador por software de las miniaturas de modelos y materiales. CPU pura,
// se llama desde un worker. No es el renderer: sin IBL, sin normal map, sin
// sombras. Basta para reconocer silueta, color y brillo a 64 px (spike del
// 2026-09-25, ver docs/superpowers/specs/2026-09-25-model-material-thumbnails-design.md).
namespace DonTopo {

ThumbnailResult rasterizeThumbnail(const std::vector<PreviewPart>& parts);

PreviewPart makePreviewSphere();

} // namespace DonTopo
```

(Pegar encima de cada función el comentario del bloque *Produces*.)

- [ ] **Step 4: Crear `ThumbnailRaster.cpp`**

```cpp
#include "DonTopo/Editor/ThumbnailRaster.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace DonTopo {

namespace {

constexpr int kSS  = 4;                                     // supersampling por eje
constexpr int kRes = static_cast<int>(kThumbCell) * kSS;    // 256 internos

bool finite3(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

float toLinear(uint8_t c) { return std::pow(c / 255.0f, 2.2f); }

uint8_t toSrgb8(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(std::pow(v, 1.0f / 2.2f) * 255.0f));
}

float clamp01(float v, float fallback) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : fallback; }

glm::vec3 sampleAlbedo(const PreviewImage& img, glm::vec2 uv)
{
    float u = std::isfinite(uv.x) ? uv.x - std::floor(uv.x) : 0.0f;
    float v = std::isfinite(uv.y) ? uv.y - std::floor(uv.y) : 0.0f;
    const int x = std::clamp(static_cast<int>(u * img.w), 0, img.w - 1);
    const int y = std::clamp(static_cast<int>(v * img.h), 0, img.h - 1);
    const uint8_t* p = &img.rgba[(static_cast<size_t>(y) * img.w + x) * 4];
    return { toLinear(p[0]), toLinear(p[1]), toLinear(p[2]) };
}

} // namespace

ThumbnailResult rasterizeThumbnail(const std::vector<PreviewPart>& parts)
{
    ThumbnailResult out;   // Unreadable

    constexpr float kMax = std::numeric_limits<float>::max();
    glm::vec3 lo(kMax), hi(-kMax);
    bool any = false;
    for (const PreviewPart& part : parts)
        for (const glm::vec3& p : part.positions)
            if (finite3(p)) { lo = glm::min(lo, p); hi = glm::max(hi, p); any = true; }
    if (!any) return out;

    // Vista 3/4 desde arriba, ortografica, ajustada a las 8 esquinas del bbox.
    const glm::vec3 center = (lo + hi) * 0.5f;
    const glm::mat4 view = glm::rotate(glm::mat4(1.0f), glm::radians(25.0f), glm::vec3(1, 0, 0)) *
                           glm::rotate(glm::mat4(1.0f), glm::radians(-35.0f), glm::vec3(0, 1, 0)) *
                           glm::translate(glm::mat4(1.0f), -center);
    float extent = 1e-6f;
    for (int i = 0; i < 8; ++i)
    {
        const glm::vec3 corner((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
        const glm::vec4 q = view * glm::vec4(corner, 1.0f);
        extent = std::max({ extent, std::abs(q.x), std::abs(q.y) });
    }
    const float half = extent * 1.08f;                       // 8 % de margen

    const glm::mat3 normalView(view);
    const glm::vec3 key     = glm::normalize(glm::vec3(-0.5f, 0.8f, 0.6f));   // espacio de vista
    const glm::vec3 fill    = glm::normalize(glm::vec3(0.7f, 0.1f, 0.5f));
    const glm::vec3 halfVec = glm::normalize(key + glm::vec3(0, 0, 1));

    std::vector<glm::vec3> color(static_cast<size_t>(kRes) * kRes);
    std::vector<float>     depth(static_cast<size_t>(kRes) * kRes, kMax);
    std::vector<uint8_t>   covered(static_cast<size_t>(kRes) * kRes, 0);
    bool drew = false;

    for (const PreviewPart& part : parts)
    {
        const size_t n = part.positions.size();
        std::vector<glm::vec3> screen(n), normal(n);
        for (size_t i = 0; i < n; ++i)
        {
            const glm::vec4 q = view * glm::vec4(part.positions[i], 1.0f);
            screen[i] = { (q.x / half * 0.5f + 0.5f) * kRes, (0.5f - q.y / half * 0.5f) * kRes, -q.z };
            const glm::vec3 nn = i < part.normals.size() ? normalView * part.normals[i] : glm::vec3(0, 0, 1);
            normal[i] = (finite3(nn) && glm::length(nn) > 1e-6f) ? glm::normalize(nn) : glm::vec3(0, 0, 1);
        }
        const PreviewImage& img = part.albedo;
        const bool textured = img.w > 0 && img.h > 0 &&
                              img.rgba.size() == static_cast<size_t>(img.w) * img.h * 4;
        const float metallic  = clamp01(part.metallic, 0.0f);
        const float roughness = clamp01(part.roughness, 0.5f);
        const float shininess = std::exp2(10.0f * (1.0f - roughness) + 1.0f);

        for (size_t t = 0; t + 2 < part.indices.size(); t += 3)
        {
            const uint32_t i0 = part.indices[t], i1 = part.indices[t + 1], i2 = part.indices[t + 2];
            if (i0 >= n || i1 >= n || i2 >= n) continue;
            const glm::vec3 a = screen[i0], b = screen[i1], d = screen[i2];
            if (!finite3(a) || !finite3(b) || !finite3(d)) continue;
            const float area = (b.x - a.x) * (d.y - a.y) - (b.y - a.y) * (d.x - a.x);
            if (!(std::abs(area) > 1e-12f)) continue;

            const int x0 = std::max(0, static_cast<int>(std::floor(std::min({ a.x, b.x, d.x }))));
            const int x1 = std::min(kRes - 1, static_cast<int>(std::ceil(std::max({ a.x, b.x, d.x }))));
            const int y0 = std::max(0, static_cast<int>(std::floor(std::min({ a.y, b.y, d.y }))));
            const int y1 = std::min(kRes - 1, static_cast<int>(std::ceil(std::max({ a.y, b.y, d.y }))));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    const float px = x + 0.5f, py = y + 0.5f;
                    const float w0 = ((b.x - px) * (d.y - py) - (b.y - py) * (d.x - px)) / area;
                    const float w1 = ((d.x - px) * (a.y - py) - (d.y - py) * (a.x - px)) / area;
                    const float w2 = 1.0f - w0 - w1;
                    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                    const float z = w0 * a.z + w1 * b.z + w2 * d.z;
                    const size_t o = static_cast<size_t>(y) * kRes + x;
                    if (z >= depth[o]) continue;

                    glm::vec3 albedo(1.0f);
                    if (textured)
                    {
                        const glm::vec2 uv0 = i0 < part.uvs.size() ? part.uvs[i0] : glm::vec2(0.0f);
                        const glm::vec2 uv1 = i1 < part.uvs.size() ? part.uvs[i1] : glm::vec2(0.0f);
                        const glm::vec2 uv2 = i2 < part.uvs.size() ? part.uvs[i2] : glm::vec2(0.0f);
                        albedo = sampleAlbedo(img, w0 * uv0 + w1 * uv1 + w2 * uv2);
                    }
                    else if (i0 < part.colors.size() && i1 < part.colors.size() && i2 < part.colors.size())
                    {
                        albedo = w0 * part.colors[i0] + w1 * part.colors[i1] + w2 * part.colors[i2];
                    }

                    glm::vec3 nrm = w0 * normal[i0] + w1 * normal[i1] + w2 * normal[i2];
                    nrm = glm::length(nrm) > 1e-6f ? glm::normalize(nrm) : glm::vec3(0, 0, 1);
                    if (nrm.z < 0.0f) nrm = -nrm;                // doble cara

                    const glm::vec3 diffuse = albedo * (1.0f - metallic);
                    const glm::vec3 f0      = glm::mix(glm::vec3(0.04f), albedo, metallic);
                    const float     spec    = std::pow(std::max(0.0f, glm::dot(nrm, halfVec)), shininess) *
                                              (shininess + 8.0f) / 25.0f;
                    // Ambiente cielo/suelo: imita el IBL lo justo para dar volumen.
                    const glm::vec3 ambient = glm::mix(glm::vec3(0.18f, 0.17f, 0.16f),
                                                       glm::vec3(0.35f, 0.38f, 0.45f), nrm.y * 0.5f + 0.5f);
                    const glm::vec3 lit =
                        diffuse * (ambient + 1.1f * std::max(0.0f, glm::dot(nrm, key)) +
                                   0.3f * std::max(0.0f, glm::dot(nrm, fill))) +
                        f0 * (spec + ambient * 1.5f);
                    if (!finite3(lit)) continue;

                    color[o]   = lit;
                    depth[o]   = z;
                    covered[o] = 1;
                    drew       = true;
                }
        }
    }
    if (!drew) return out;

    // Reduccion 4x4: color medio de lo cubierto, alfa = fraccion cubierta.
    std::vector<uint8_t> tile(static_cast<size_t>(kThumbCell) * kThumbCell * 4, 0);
    for (uint32_t y = 0; y < kThumbCell; ++y)
        for (uint32_t x = 0; x < kThumbCell; ++x)
        {
            glm::vec3 sum(0.0f);
            int       count = 0;
            for (int j = 0; j < kSS; ++j)
                for (int i = 0; i < kSS; ++i)
                {
                    const size_t o = static_cast<size_t>(y * kSS + j) * kRes + x * kSS + i;
                    if (covered[o]) { sum += color[o]; ++count; }
                }
            if (count == 0) continue;
            const glm::vec3 rgb = sum / static_cast<float>(count);
            uint8_t* p = &tile[(static_cast<size_t>(y) * kThumbCell + x) * 4];
            p[0] = toSrgb8(rgb.r);
            p[1] = toSrgb8(rgb.g);
            p[2] = toSrgb8(rgb.b);
            p[3] = static_cast<uint8_t>((count * 255 + kSS * kSS / 2) / (kSS * kSS));
        }

    // Borde oscuro de 1 px alrededor de la silueta: la separa del fondo del boton.
    out.rgba = tile;
    for (uint32_t y = 0; y < kThumbCell; ++y)
        for (uint32_t x = 0; x < kThumbCell; ++x)
        {
            if (tile[(static_cast<size_t>(y) * kThumbCell + x) * 4 + 3] > 0) continue;
            int neighbour = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const int xx = static_cast<int>(x) + dx, yy = static_cast<int>(y) + dy;
                    if (xx < 0 || yy < 0 || xx >= static_cast<int>(kThumbCell) || yy >= static_cast<int>(kThumbCell))
                        continue;
                    neighbour = std::max<int>(neighbour, tile[(static_cast<size_t>(yy) * kThumbCell + xx) * 4 + 3]);
                }
            if (neighbour == 0) continue;
            uint8_t* p = &out.rgba[(static_cast<size_t>(y) * kThumbCell + x) * 4];
            p[0] = p[1] = p[2] = 15;
            p[3] = static_cast<uint8_t>(neighbour * 0.6f);
        }
    out.status = ThumbnailStatus::Ok;
    return out;
}

PreviewPart makePreviewSphere()
{
    constexpr int kSeg = 48, kRings = 24;
    PreviewPart p;
    for (int j = 0; j <= kRings; ++j)
        for (int i = 0; i <= kSeg; ++i)
        {
            const float th = glm::pi<float>() * j / kRings;
            const float ph = 2.0f * glm::pi<float>() * i / kSeg;
            const glm::vec3 n(std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph));
            p.positions.push_back(n);
            p.normals.push_back(n);
            p.uvs.emplace_back(2.0f * i / kSeg, static_cast<float>(j) / kRings);
            p.colors.emplace_back(1.0f);
        }
    for (int j = 0; j < kRings; ++j)
        for (int i = 0; i < kSeg; ++i)
        {
            const uint32_t k = static_cast<uint32_t>(j * (kSeg + 1) + i);
            p.indices.insert(p.indices.end(), { k, k + kSeg + 1, k + 1, k + 1, k + kSeg + 1, k + kSeg + 2 });
        }
    return p;
}

} // namespace DonTopo
```

- [ ] **Step 5: Añadir el fichero al build**

En `engine/CMakeLists.txt`, tras la línea `    src/Editor/Thumbnail.cpp`, añadir `    src/Editor/ThumbnailRaster.cpp`.

- [ ] **Step 6: Compilar y ejecutar**

Run: `.\build.bat > $env:TEMP\b.log 2>&1`; desde la raíz, `.\build-ninja\engine\tests\dt_thumbnail_tests.exe`.
Expected: `ALL THUMBNAIL TESTS PASSED`.

- [ ] **Step 7: Sabotaje**

Uno a uno, compilar, ejecutar y revertir: (a) quitar `if (nrm.z < 0.0f) nrm = -nrm;` → debe fallar `test_raster_draws_back_faces` (si no falla porque la cobertura no depende del sombreado, sustituir en el sabotaje por descartar los triángulos con `area < 0` y comprobar que entonces sí falla); (b) cambiar `1.08f` por `1.0f` → debe fallar `test_raster_frames_the_bbox_with_a_margin`; (c) quitar el `if (i0 >= n || ...) continue;` → `test_raster_degenerate_input_is_unreadable` debe fallar o reventar.

- [ ] **Step 8: Commit**

```bash
git add engine/include/DonTopo/Editor/ThumbnailRaster.h engine/src/Editor/ThumbnailRaster.cpp engine/CMakeLists.txt engine/tests/thumbnail_tests.cpp
git commit -m "feat(editor): rasterizador por software para las miniaturas de modelos y materiales"
```

---

### Task 3: `makeAssetThumbnail`, `.mat` y dependencias

**Files:**
- Modify: `engine/include/DonTopo/Editor/Thumbnail.h`
- Modify: `engine/src/Editor/Thumbnail.cpp`
- Test: `engine/tests/thumbnail_tests.cpp`

**Interfaces:**
- Consumes: `ModelLoader::loadPreview`, `ModelLoader::loadPreviewImage`, `PreviewStatus` (Task 1); `rasterizeThumbnail`, `makePreviewSphere` (Task 2); `loadMaterialAsset` (`Core/MaterialAsset.h`).
- Produces (en `Thumbnail.h`):

```cpp
enum class ThumbnailStatus { Ok, Unreadable, TooLarge, AnimationOnly };

// Un fichero del que depende una miniatura y su estado al generarla.
struct ThumbnailDependency
{
    std::filesystem::path path;
    bool                  exists = false;
    int64_t               mtime  = 0;    // file_time_type::time_since_epoch().count(); 0 si no existe
};
bool operator==(const ThumbnailDependency& a, const ThumbnailDependency& b);   // path, exists y mtime

struct ThumbnailResult
{
    ThumbnailStatus                  status = ThumbnailStatus::Unreadable;
    std::vector<uint8_t>             rgba;
    // Lo que declara el decodificador: SOLO las rutas (exists/mtime sin rellenar).
    // stampDependencies las sella y pone el propio asset delante.
    std::vector<ThumbnailDependency> dependencies;
};

inline constexpr float kNeutralAlbedo = 0.6f;   // lineal: el gris de un .mat que hereda

ThumbnailDependency stampFile(const std::filesystem::path& path);                  // nunca lanza
void                stampDependencies(ThumbnailResult& r, const ThumbnailDependency& self);
bool                isModelThumbnailPath(const std::filesystem::path& path);       // .fbx .obj
ThumbnailResult     makeMaterialThumbnail(const std::filesystem::path& mat);
ThumbnailResult     makeAssetThumbnail(const std::filesystem::path& path);         // el Decoder por defecto
```

- [ ] **Step 1: Escribir los tests que fallan**

En `thumbnail_tests.cpp` añadir `#include "DonTopo/Core/MaterialAsset.h"` y `#include "DonTopo/Core/ImportSettings.h"` y, antes de `main()`:

```cpp
// ── makeAssetThumbnail ───────────────────────────────────────────────────────

static bool hasDep(const ThumbnailResult& r, const fs::path& p)
{
    for (const ThumbnailDependency& d : r.dependencies)
        if (fs::path(d.path).lexically_normal() == p.lexically_normal()) return true;
    return false;
}

static void writeMat(const fs::path& p, const MaterialAsset& a)
{
    std::string err;
    CHECK(saveMaterialAsset(p, a, &err));
}

// Una imagen pasa por el decodificador de siempre: mismos bytes.
static void test_asset_thumbnail_image_is_unchanged(const fs::path& dir)
{
    writeTga(dir / "img_same.tga", 20, 10, [](int x, int) { return x < 10 ? kRed : kBlue; });
    const ThumbnailResult a = makeAssetThumbnail(dir / "img_same.tga");
    const ThumbnailResult b = makeThumbnail(dir / "img_same.tga");
    CHECK(a.status == ThumbnailStatus::Ok);
    CHECK(a.rgba == b.rgba);
}

static void test_asset_thumbnail_model_declares_its_dependencies(const fs::path& dir)
{
    writeTga(dir / "obj_tex.tga", 4, 4, [](int, int) { return kRed; });
    std::ofstream(dir / "quad.mtl") << "newmtl m\nmap_Kd obj_tex.tga\n";
    std::ofstream(dir / "quad.obj") << "mtllib quad.mtl\nusemtl m\n"
                                       "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                                       "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
                                       "f 1/1 2/2 3/3\nf 1/1 3/3 4/4\n";
    const ThumbnailResult r = makeAssetThumbnail(dir / "quad.obj");
    CHECK(r.status == ThumbnailStatus::Ok);
    CHECK(hasDep(r, dir / "obj_tex.tga"));
    CHECK(hasDep(r, importSidecarPath(dir / "quad.obj")));
}

static void test_asset_thumbnail_animation_only_fbx()
{
    const ThumbnailResult r = makeAssetThumbnail("assets/animatedCharacter/standing idle 01.fbx");
    CHECK(r.status == ThumbnailStatus::AnimationOnly);
    CHECK(r.rgba.empty());
}

static void test_material_thumbnail_takes_its_albedo(const fs::path& dir)
{
    writeTga(dir / "mat_red.tga", 4, 4, [](int, int) { return kRed; });
    MaterialAsset a;
    a.albedo = (dir / "mat_red.tga").string();
    writeMat(dir / "red.mat", a);
    const ThumbnailResult r = makeAssetThumbnail(dir / "red.mat");
    CHECK(r.status == ThumbnailStatus::Ok);
    if (!r.rgba.empty()) CHECK(reddish(pixelAt(r, 32, 32)));
    CHECK(hasDep(r, dir / "mat_red.tga"));
}

// Heredado -> gris neutro. Y los valores LEIDOS del .mat cuentan: se prueban
// roughness 0.1 frente a 0.9 y metallic 1, ninguno el default.
static void test_material_thumbnail_inherits_neutral_and_reads_factors(const fs::path& dir)
{
    writeMat(dir / "neutral.mat", MaterialAsset{});
    const ThumbnailResult n = makeAssetThumbnail(dir / "neutral.mat");
    CHECK(n.status == ThumbnailStatus::Ok);
    if (!n.rgba.empty())
    {
        const Rgba c = pixelAt(n, 32, 32);
        CHECK(std::abs(int(c[0]) - int(c[1])) <= 2 && std::abs(int(c[1]) - int(c[2])) <= 12);   // gris (el ambiente tinta un poco el azul)
    }

    MaterialAsset smooth; smooth.roughness = 0.1f;
    MaterialAsset rough;  rough.roughness  = 0.9f;
    MaterialAsset metal;  metal.metallic   = 1.0f;
    writeMat(dir / "smooth.mat", smooth);
    writeMat(dir / "rough.mat", rough);
    writeMat(dir / "metal.mat", metal);
    CHECK(makeAssetThumbnail(dir / "smooth.mat").rgba != makeAssetThumbnail(dir / "rough.mat").rgba);
    CHECK(makeAssetThumbnail(dir / "metal.mat").rgba != n.rgba);
}

// Textura que no existe: esfera neutra (es lo que pinta el motor) y la ruta sigue
// siendo dependencia, para regenerar cuando aparezca.
static void test_material_thumbnail_missing_texture_is_neutral(const fs::path& dir)
{
    MaterialAsset a;
    a.albedo = (dir / "todavia_no.tga").string();
    writeMat(dir / "pending.mat", a);
    writeMat(dir / "neutral2.mat", MaterialAsset{});
    const ThumbnailResult r = makeAssetThumbnail(dir / "pending.mat");
    CHECK(r.status == ThumbnailStatus::Ok);
    CHECK(r.rgba == makeAssetThumbnail(dir / "neutral2.mat").rgba);
    CHECK(hasDep(r, dir / "todavia_no.tga"));
}

static void test_stamp_file_and_dependencies(const fs::path& dir)
{
    const ThumbnailDependency missing = stampFile(dir / "no_hay.tga");
    CHECK(!missing.exists && missing.mtime == 0);

    const fs::path f = makeImage(dir, "stamp.tga");
    const ThumbnailDependency s = stampFile(f);
    std::error_code ec;
    CHECK(s.exists && s.mtime == static_cast<int64_t>(fs::last_write_time(f, ec).time_since_epoch().count()));

    ThumbnailResult r;
    r.dependencies = { { dir / "no_hay.tga" }, { f }, { dir / "no_hay.tga" } };   // con duplicado y con el propio asset
    stampDependencies(r, s);
    CHECK(r.dependencies.size() == 2);                       // self delante, sin duplicados
    if (r.dependencies.size() == 2)
    {
        CHECK(r.dependencies[0] == s);
        CHECK(!r.dependencies[1].exists);
    }
}

static void test_is_model_thumbnail_path()
{
    CHECK(isModelThumbnailPath("a/b/Hero.FBX"));
    CHECK(isModelThumbnailPath("x.obj"));
    CHECK(!isModelThumbnailPath("x.mat"));
    CHECK(!isModelThumbnailPath("x.png"));
    CHECK(!isModelThumbnailPath("x.glb"));
}
```

Y en `main()`:

```cpp
    test_asset_thumbnail_image_is_unchanged(dir);
    test_asset_thumbnail_model_declares_its_dependencies(dir);
    test_asset_thumbnail_animation_only_fbx();
    test_material_thumbnail_takes_its_albedo(dir);
    test_material_thumbnail_inherits_neutral_and_reads_factors(dir);
    test_material_thumbnail_missing_texture_is_neutral(dir);
    test_stamp_file_and_dependencies(dir);
    test_is_model_thumbnail_path();
```

- [ ] **Step 2: Compilar y ver que falla**

Expected: `ThumbnailDependency`/`makeAssetThumbnail` no declarados.

- [ ] **Step 3: Cambiar `Thumbnail.h`**

Sustituir `enum class ThumbnailStatus` y `struct ThumbnailResult` por los del bloque *Produces* (con sus comentarios), añadir `#include <cstdint>`, y tras `makeThumbnailFromStream` declarar `kNeutralAlbedo`, `stampFile`, `stampDependencies`, `isModelThumbnailPath`, `makeMaterialThumbnail` y `makeAssetThumbnail` con estos comentarios:

```cpp
// Estado de un fichero AHORA. Nunca lanza: si no se puede leer, exists = false.
ThumbnailDependency stampFile(const std::filesystem::path& path);

// Sella las dependencias declaradas por el decodificador y pone `self` (el
// asset, sellado ANTES de decodificar) la primera. Quita duplicados y el propio
// asset si el decodificador lo repitio.
void stampDependencies(ThumbnailResult& r, const ThumbnailDependency& self);

// .fbx/.obj: los que tarda segundos en decodificar (ver el tope de ThumbnailCache).
bool isModelThumbnailPath(const std::filesystem::path& path);

// Esfera con el albedo, metallic y roughness del .mat; lo heredado, neutro.
ThumbnailResult makeMaterialThumbnail(const std::filesystem::path& mat);

// Decodificador por extension: imagen -> makeThumbnail; .fbx/.obj -> preview
// rasterizado; .mat -> esfera. Cualquier otra cosa, Unreadable. Nunca lanza.
ThumbnailResult makeAssetThumbnail(const std::filesystem::path& path);
```

- [ ] **Step 4: Implementar en `Thumbnail.cpp`**

Añadir `#include "DonTopo/Editor/ThumbnailRaster.h"`, `#include "DonTopo/Core/MaterialAsset.h"`, `#include "DonTopo/Renderer/ModelLoader.h"`, `#include <cctype>`. Tras `makeThumbnailFromStream`:

```cpp
bool operator==(const ThumbnailDependency& a, const ThumbnailDependency& b)
{
    return a.exists == b.exists && a.mtime == b.mtime && a.path == b.path;
}

ThumbnailDependency stampFile(const std::filesystem::path& path)
{
    ThumbnailDependency d;
    d.path = path;
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (!ec)
    {
        d.exists = true;
        d.mtime  = static_cast<int64_t>(t.time_since_epoch().count());
    }
    return d;
}

void stampDependencies(ThumbnailResult& r, const ThumbnailDependency& self)
{
    std::vector<ThumbnailDependency> out{ self };
    for (const ThumbnailDependency& d : r.dependencies)
    {
        const std::filesystem::path p = d.path.lexically_normal();
        const bool seen = std::any_of(out.begin(), out.end(), [&](const ThumbnailDependency& o) {
            return o.path.lexically_normal() == p;
        });
        if (!seen) out.push_back(stampFile(d.path));
    }
    r.dependencies = std::move(out);
}

namespace {

std::string lowerExt(const std::filesystem::path& p)
{
    std::string e = p.extension().string();
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

bool isImageExt(const std::string& e)
{
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp";
}

ThumbnailResult makeModelThumbnail(const std::filesystem::path& path)
{
    const ModelPreview preview = ModelLoader::loadPreview(path.string());
    ThumbnailResult r;
    if (preview.status == PreviewStatus::AnimationOnly) r.status = ThumbnailStatus::AnimationOnly;
    else if (preview.status == PreviewStatus::Ok)       r = rasterizeThumbnail(preview.parts);
    for (const std::filesystem::path& d : preview.dependencies) r.dependencies.push_back({ d });
    return r;
}

} // namespace

bool isModelThumbnailPath(const std::filesystem::path& path)
{
    const std::string e = lowerExt(path);
    return e == ".fbx" || e == ".obj";
}

ThumbnailResult makeMaterialThumbnail(const std::filesystem::path& mat)
{
    const MaterialAsset a = loadMaterialAsset(mat);     // nunca lanza; roto -> hereda
    PreviewPart sphere = makePreviewSphere();
    std::vector<ThumbnailDependency> deps;
    if (!a.albedo.empty())
    {
        deps.push_back({ std::filesystem::path(a.albedo) });
        sphere.albedo = ModelLoader::loadPreviewImage(a.albedo);
    }
    if (sphere.albedo.rgba.empty())
        std::fill(sphere.colors.begin(), sphere.colors.end(), glm::vec3(kNeutralAlbedo));
    sphere.metallic  = a.metallic  < 0.0f ? 0.0f : a.metallic;
    sphere.roughness = a.roughness < 0.0f ? 0.5f : a.roughness;
    ThumbnailResult r = rasterizeThumbnail({ sphere });
    r.dependencies = std::move(deps);
    return r;
}

ThumbnailResult makeAssetThumbnail(const std::filesystem::path& path)
{
    try
    {
        const std::string e = lowerExt(path);
        if (isImageExt(e))           return makeThumbnail(path);
        if (isModelThumbnailPath(path)) return makeModelThumbnail(path);
        if (e == ".mat")             return makeMaterialThumbnail(path);
    }
    catch (...) {}
    return ThumbnailResult{};
}
```

- [ ] **Step 5: Compilar y ejecutar**

Run: build y `.\build-ninja\engine\tests\dt_thumbnail_tests.exe` desde la raíz.
Expected: `ALL THUMBNAIL TESTS PASSED`. Si falla el test del gris neutro por el tinte del ambiente, **no** ampliar la tolerancia a ciegas: imprimir el píxel, comprobar que R≈G y que B es el que se aparta por el ambiente azulado, y ajustar solo la tolerancia de B con un comentario que diga el valor medido.

- [ ] **Step 6: Sabotaje**

Uno a uno: (a) en `makeMaterialThumbnail` ignorar `a.roughness` (usar siempre 0.5) → debe fallar `test_material_thumbnail_inherits_neutral_and_reads_factors`; (b) no añadir la dependencia del albedo → deben fallar `test_material_thumbnail_takes_its_albedo` y `..._missing_texture_is_neutral`; (c) en `stampDependencies` no deduplicar → debe fallar `test_stamp_file_and_dependencies`.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Editor/Thumbnail.h engine/src/Editor/Thumbnail.cpp engine/tests/thumbnail_tests.cpp
git commit -m "feat(editor): decodificador de miniaturas por tipo de asset con modelos, .mat y dependencias"
```

---

### Task 4: `ThumbnailDiskCache`

**Files:**
- Create: `engine/include/DonTopo/Editor/ThumbnailDiskCache.h`
- Create: `engine/src/Editor/ThumbnailDiskCache.cpp`
- Modify: `engine/CMakeLists.txt` (tras `src/Editor/ThumbnailRaster.cpp`)
- Test: `engine/tests/thumbnail_tests.cpp`

**Interfaces:**
- Consumes: `ThumbnailResult`, `ThumbnailDependency`, `ThumbnailStatus`, `stampFile`, `kThumbCell` (Task 3).
- Produces:

```cpp
namespace DonTopo {
inline constexpr uint32_t kThumbDiskVersion = 1;

class ThumbnailDiskCache
{
public:
    explicit ThumbnailDiskCache(std::filesystem::path dir);
    std::optional<ThumbnailResult> load(const std::filesystem::path& asset) const;
    bool store(const std::filesystem::path& asset, const ThumbnailResult& r) const;
    std::filesystem::path fileFor(const std::filesystem::path& asset) const;
    const std::filesystem::path& directory() const { return m_dir; }
};
}
```

- [ ] **Step 1: Escribir los tests que fallan**

En `thumbnail_tests.cpp` añadir `#include "DonTopo/Editor/ThumbnailDiskCache.h"` y:

```cpp
// ── ThumbnailDiskCache ───────────────────────────────────────────────────────

static ThumbnailResult stampedResult(const fs::path& asset, std::vector<fs::path> deps = {})
{
    ThumbnailResult r;
    r.status = ThumbnailStatus::Ok;
    r.rgba.resize(static_cast<size_t>(kThumbCell) * kThumbCell * 4);
    for (size_t i = 0; i < r.rgba.size(); ++i) r.rgba[i] = static_cast<uint8_t>(i * 7);
    for (const fs::path& d : deps) r.dependencies.push_back({ d });
    stampDependencies(r, stampFile(asset));
    return r;
}

static void bumpMtime(const fs::path& p)
{
    std::error_code ec;
    fs::last_write_time(p, fs::last_write_time(p, ec) + std::chrono::seconds(10), ec);
}

static void test_disk_roundtrip(const fs::path& dir)
{
    const ThumbnailDiskCache disk(dir / "cache_rt");
    const fs::path asset = makeImage(dir, "disk_a.tga");
    const fs::path dep   = makeImage(dir, "disk_a_dep.tga");
    const ThumbnailResult r = stampedResult(asset, { dep, dir / "disk_a_absent.tga" });
    CHECK(disk.store(asset, r));
    const std::optional<ThumbnailResult> back = disk.load(asset);
    CHECK(back.has_value());
    if (!back) return;
    CHECK(back->status == ThumbnailStatus::Ok);
    CHECK(back->rgba == r.rgba);
    CHECK(back->dependencies.size() == 3);
    if (back->dependencies.size() == 3) CHECK(back->dependencies[0] == r.dependencies[0]);

    ThumbnailResult anim;
    anim.status = ThumbnailStatus::AnimationOnly;
    stampDependencies(anim, stampFile(dep));
    CHECK(disk.store(dep, anim));
    const auto animBack = disk.load(dep);
    CHECK(animBack && animBack->status == ThumbnailStatus::AnimationOnly && animBack->rgba.empty());
}

// Review Focus 2: cambia una dependencia (no el asset), o aparece una que no estaba.
static void test_disk_dependency_change_is_a_miss(const fs::path& dir)
{
    const ThumbnailDiskCache disk(dir / "cache_dep");
    const fs::path asset = makeImage(dir, "disk_b.tga");
    const fs::path dep   = makeImage(dir, "disk_b_dep.tga");
    const fs::path later = dir / "disk_b_later.tga";
    CHECK(disk.store(asset, stampedResult(asset, { dep, later })));
    CHECK(disk.load(asset).has_value());

    bumpMtime(dep);
    CHECK(!disk.load(asset).has_value());

    CHECK(disk.store(asset, stampedResult(asset, { dep, later })));
    makeImage(dir, "disk_b_later.tga");
    CHECK(!disk.load(asset).has_value());

    CHECK(disk.store(asset, stampedResult(asset, { dep, later })));
    bumpMtime(asset);
    CHECK(!disk.load(asset).has_value());
}

// Review Focus 3: fichero de otra version, truncado o de otra ruta con el mismo nombre.
static void test_disk_hostile_files_are_a_miss(const fs::path& dir)
{
    const ThumbnailDiskCache disk(dir / "cache_bad");
    const fs::path a = makeImage(dir, "disk_c.tga");
    const fs::path b = makeImage(dir, "disk_d.tga");
    auto readAll  = [](const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), {}); };
    auto writeAll = [](const fs::path& p, const std::string& s) { std::ofstream(p, std::ios::binary | std::ios::trunc) << s; };

    CHECK(disk.store(a, stampedResult(a)));
    const std::string good = readAll(disk.fileFor(a));

    std::string otherVersion = good;
    otherVersion[4] = static_cast<char>(otherVersion[4] + 1);          // version, tras la magia
    writeAll(disk.fileFor(a), otherVersion);
    CHECK(!disk.load(a).has_value());

    writeAll(disk.fileFor(a), good.substr(0, good.size() - 10));
    CHECK(!disk.load(a).has_value());

    writeAll(disk.fileFor(a), good + "x");                            // bytes de mas
    CHECK(!disk.load(a).has_value());

    writeAll(disk.fileFor(b), good);                                  // colision de hash simulada
    CHECK(!disk.load(b).has_value());

    writeAll(disk.fileFor(a), "");
    CHECK(!disk.load(a).has_value());
}

static void test_disk_unwritable_dir_does_not_throw(const fs::path& dir)
{
    std::ofstream(dir / "soy_un_fichero") << "x";
    const ThumbnailDiskCache disk(dir / "soy_un_fichero");
    const fs::path a = makeImage(dir, "disk_e.tga");
    CHECK(!disk.store(a, stampedResult(a)));
    CHECK(!disk.load(a).has_value());
}

static void test_disk_leaves_no_temporaries(const fs::path& dir)
{
    const ThumbnailDiskCache disk(dir / "cache_tmp");
    for (int i = 0; i < 3; ++i)
    {
        const fs::path a = makeImage(dir, ("disk_t" + std::to_string(i) + ".tga").c_str());
        CHECK(disk.store(a, stampedResult(a)));
        CHECK(disk.store(a, stampedResult(a)));                         // sobrescribe
    }
    int files = 0, temps = 0;
    for (const auto& e : fs::directory_iterator(dir / "cache_tmp"))
    {
        ++files;
        if (e.path().extension() == ".tmp") ++temps;
    }
    CHECK(files == 3);
    CHECK(temps == 0);
}

// Sin dependencias selladas no se guarda nada: no se sabria cuando invalidar.
static void test_disk_refuses_unstamped_results(const fs::path& dir)
{
    const ThumbnailDiskCache disk(dir / "cache_unstamped");
    const fs::path a = makeImage(dir, "disk_f.tga");
    ThumbnailResult r = stampedResult(a);
    r.dependencies.clear();
    CHECK(!disk.store(a, r));
}
```

En `main()`:

```cpp
    test_disk_roundtrip(dir);
    test_disk_dependency_change_is_a_miss(dir);
    test_disk_hostile_files_are_a_miss(dir);
    test_disk_unwritable_dir_does_not_throw(dir);
    test_disk_leaves_no_temporaries(dir);
    test_disk_refuses_unstamped_results(dir);
```

- [ ] **Step 2: Compilar y ver que falla**

Expected: `ThumbnailDiskCache.h` no existe.

- [ ] **Step 3: Crear `ThumbnailDiskCache.h`**

```cpp
#pragma once
#include "DonTopo/Editor/Thumbnail.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace DonTopo {

// Subir SIEMPRE que cambie el aspecto de las miniaturas (rasterizador, luces,
// encuadre, reduccion de texturas): una cache vieja no sabe que esta obsoleta.
inline constexpr uint32_t kThumbDiskVersion = 1;

// Miniaturas ya generadas, una por fichero en <proyecto>/.dt-cache/thumbs/, con
// la lista de dependencias y sus mtime. Se usa desde los workers: todo const y
// sin estado compartido salvo el aviso de "no se puede escribir", que es atomico.
class ThumbnailDiskCache
{
public:
    explicit ThumbnailDiskCache(std::filesystem::path dir);

    // La casilla guardada de asset si el fichero existe, la magia y la version
    // casan, la ruta guardada es la de asset y CADA dependencia sigue igual
    // (mismo mtime, o sigue sin existir). Nunca lanza.
    std::optional<ThumbnailResult> load(const std::filesystem::path& asset) const;

    // Escribe a un temporal unico y renombra encima. false (y un aviso por
    // stderr la primera vez) si no se pudo, o si r no trae dependencias selladas.
    bool store(const std::filesystem::path& asset, const ThumbnailResult& r) const;

    // <dir>/<fnv1a64 de la ruta absoluta normalizada, 16 hex>.bin
    std::filesystem::path        fileFor(const std::filesystem::path& asset) const;
    const std::filesystem::path& directory() const { return m_dir; }

private:
    std::filesystem::path     m_dir;
    mutable std::atomic<bool> m_warned{ false };
};

} // namespace DonTopo
```

- [ ] **Step 4: Crear `ThumbnailDiskCache.cpp`**

```cpp
#include "DonTopo/Editor/ThumbnailDiskCache.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>

namespace DonTopo {

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kMagic   = 0x48545444u;   // "DTTH"
constexpr uint32_t kMaxPath = 4096;
constexpr uint32_t kMaxDeps = 64;
constexpr size_t   kTileBytes = static_cast<size_t>(kThumbCell) * kThumbCell * 4;

std::string toUtf8(const fs::path& p)
{
    const std::u8string s = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

fs::path fromUtf8(const std::string& s)
{
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

// Absoluta y normalizada: la misma ruta pedida de dos formas da el mismo fichero.
std::string normalizedKey(const fs::path& p)
{
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec) abs = p;
    fs::path canon = fs::weakly_canonical(abs, ec);
    if (ec) canon = abs.lexically_normal();
    return toUtf8(canon);
}

uint64_t fnv1a(const std::string& s)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

void putU32(std::string& b, uint32_t v) { char t[4]; std::memcpy(t, &v, 4); b.append(t, 4); }
void putI64(std::string& b, int64_t v)  { char t[8]; std::memcpy(t, &v, 8); b.append(t, 8); }
void putStr(std::string& b, const std::string& s) { putU32(b, static_cast<uint32_t>(s.size())); b += s; }

struct Reader
{
    const std::string& b;
    size_t             pos = 0;

    bool raw(void* dst, size_t n)
    {
        if (b.size() - pos < n) return false;
        std::memcpy(dst, b.data() + pos, n);
        pos += n;
        return true;
    }
    bool u32(uint32_t& v) { return raw(&v, 4); }
    bool i64(int64_t& v)  { return raw(&v, 8); }
    bool str(std::string& s, uint32_t max)
    {
        uint32_t n = 0;
        if (!u32(n) || n > max || b.size() - pos < n) return false;
        s.assign(b.data() + pos, n);
        pos += n;
        return true;
    }
};

} // namespace

ThumbnailDiskCache::ThumbnailDiskCache(std::filesystem::path dir) : m_dir(std::move(dir)) {}

fs::path ThumbnailDiskCache::fileFor(const fs::path& asset) const
{
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.bin", static_cast<unsigned long long>(fnv1a(normalizedKey(asset))));
    return m_dir / name;
}

std::optional<ThumbnailResult> ThumbnailDiskCache::load(const fs::path& asset) const
{
    try
    {
        std::ifstream f(fileFor(asset), std::ios::binary);
        if (!f) return std::nullopt;
        const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        Reader r{ bytes };

        uint32_t magic = 0, version = 0, status = 0, depCount = 0;
        std::string key;
        if (!r.u32(magic) || magic != kMagic) return std::nullopt;
        if (!r.u32(version) || version != kThumbDiskVersion) return std::nullopt;
        if (!r.u32(status) || status > static_cast<uint32_t>(ThumbnailStatus::AnimationOnly)) return std::nullopt;
        if (!r.str(key, kMaxPath) || key != normalizedKey(asset)) return std::nullopt;   // colision de hash
        if (!r.u32(depCount) || depCount == 0 || depCount > kMaxDeps) return std::nullopt;

        ThumbnailResult out;
        out.status = static_cast<ThumbnailStatus>(status);
        for (uint32_t i = 0; i < depCount; ++i)
        {
            std::string path;
            uint32_t    exists = 0;
            int64_t     mtime  = 0;
            if (!r.str(path, kMaxPath) || !r.u32(exists) || !r.i64(mtime)) return std::nullopt;
            ThumbnailDependency stored{ fromUtf8(path), exists != 0, mtime };
            if (!(stampFile(stored.path) == stored)) return std::nullopt;                // cambio algo
            out.dependencies.push_back(std::move(stored));
        }
        if (out.status == ThumbnailStatus::Ok)
        {
            out.rgba.resize(kTileBytes);
            if (!r.raw(out.rgba.data(), kTileBytes)) return std::nullopt;
        }
        if (r.pos != bytes.size()) return std::nullopt;
        return out;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

bool ThumbnailDiskCache::store(const fs::path& asset, const ThumbnailResult& res) const
{
    if (res.dependencies.empty()) return false;
    if (res.status == ThumbnailStatus::Ok && res.rgba.size() != kTileBytes) return false;
    try
    {
        std::string b;
        putU32(b, kMagic);
        putU32(b, kThumbDiskVersion);
        putU32(b, static_cast<uint32_t>(res.status));
        putStr(b, normalizedKey(asset));
        putU32(b, static_cast<uint32_t>(res.dependencies.size()));
        for (const ThumbnailDependency& d : res.dependencies)
        {
            putStr(b, toUtf8(d.path));
            putU32(b, d.exists ? 1u : 0u);
            putI64(b, d.mtime);
        }
        if (res.status == ThumbnailStatus::Ok)
            b.append(reinterpret_cast<const char*>(res.rgba.data()), res.rgba.size());

        auto fail = [this]() {
            if (!m_warned.exchange(true))
                std::fprintf(stderr, "[Thumbnails] no se puede escribir la cache en %s: las miniaturas se generan sin guardar\n",
                             m_dir.string().c_str());
            return false;
        };

        std::error_code ec;
        fs::create_directories(m_dir, ec);
        static std::atomic<uint64_t> counter{ 0 };
        const fs::path finalPath = fileFor(asset);
        fs::path tmp = finalPath;
        tmp += "." + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + "." +
               std::to_string(counter.fetch_add(1)) + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return fail();
            f.write(b.data(), static_cast<std::streamsize>(b.size()));
            if (!f) { f.close(); fs::remove(tmp, ec); return fail(); }
        }
        fs::rename(tmp, finalPath, ec);
        if (ec)
        {
            std::error_code ec2;
            fs::remove(tmp, ec2);
            return fail();
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

} // namespace DonTopo
```

- [ ] **Step 5: Añadir al build**

En `engine/CMakeLists.txt`, tras `    src/Editor/ThumbnailRaster.cpp`, añadir `    src/Editor/ThumbnailDiskCache.cpp`.

- [ ] **Step 6: Compilar y ejecutar**

Expected: `ALL THUMBNAIL TESTS PASSED`.

- [ ] **Step 7: Sabotaje**

Uno a uno: (a) quitar la comprobación `stampFile(stored.path) == stored` → debe fallar `test_disk_dependency_change_is_a_miss`; (b) quitar `key != normalizedKey(asset)` → debe fallar `test_disk_hostile_files_are_a_miss` (colisión); (c) escribir directamente a `finalPath` sin temporal → `test_disk_leaves_no_temporaries` **no** lo detecta: anotarlo (la atomicidad solo se ve con dos procesos) y comprobar al menos que (d) quitar el `r.pos != bytes.size()` sí hace fallar el caso de bytes de más.

- [ ] **Step 8: Commit**

```bash
git add engine/include/DonTopo/Editor/ThumbnailDiskCache.h engine/src/Editor/ThumbnailDiskCache.cpp engine/CMakeLists.txt engine/tests/thumbnail_tests.cpp
git commit -m "feat(editor): cache en disco de miniaturas con invalidacion por dependencias"
```

---

### Task 5: `ThumbnailCache` con dependencias, caché de disco, `status()` y tope de modelos

**Files:**
- Modify: `engine/include/DonTopo/Editor/Thumbnail.h` (clase `ThumbnailCache`)
- Modify: `engine/src/Editor/Thumbnail.cpp` (`ThumbnailCache::*`)
- Test: `engine/tests/thumbnail_tests.cpp`

**Interfaces:**
- Consumes: `ThumbnailDependency`, `stampFile`, `stampDependencies`, `isModelThumbnailPath`, `makeAssetThumbnail` (Task 3); `ThumbnailDiskCache` (Task 4).
- Produces:

```cpp
ThumbnailCache(Runner run, Uploader upload, uint32_t maxInFlight = 4,
               uint32_t slotCapacity = kThumbSlotCount, Decoder decode = {},
               std::shared_ptr<const ThumbnailDiskCache> disk = {},
               uint32_t maxModelsInFlight = 2);
// Estado final de path: Ok si esta en el atlas; el motivo si fallo
// (Unreadable, TooLarge, AnimationOnly); nullopt si aun no se sabe.
std::optional<ThumbnailStatus> status(const std::filesystem::path& path) const;
uint32_t modelsInFlight() const;
```

> **Revisión al escribir el plan:** la spec decía que la clave del atlas se calcula con todas las dependencias. No hace falta: `refreshStamps` ya **libera la casilla** (`m_slots.release`) al detectar el cambio, y la entrada nueva no puede encontrar la antigua. La clave sigue siendo ruta + `mtime` del asset.

- [ ] **Step 1: Escribir los tests que fallan**

Ampliar `CacheHarness` para aceptar la caché de disco y el tope de modelos (sustituir el constructor):

```cpp
    explicit CacheHarness(uint32_t maxInFlight = 4, uint32_t slotCapacity = kThumbSlotCount,
                          ThumbnailCache::Decoder decoder = {},
                          std::shared_ptr<const ThumbnailDiskCache> disk = {},
                          uint32_t maxModelsInFlight = 2)
        : cache(
              [this](std::function<void()> job) {
                  if (!runnerAccepts) return false;
                  pending.push_back(std::move(job));
                  maxPendingSeen = std::max(maxPendingSeen, pending.size());
                  return true;
              },
              [this](const ThumbnailTile* tiles, size_t count) {
                  std::vector<uint32_t> slots;
                  for (size_t i = 0; i < count; ++i) slots.push_back(tiles[i].slot);
                  uploads.push_back(std::move(slots));
                  return uploadOk;
              },
              maxInFlight, slotCapacity, std::move(decoder), std::move(disk), maxModelsInFlight)
    {}
```

Y los tests:

```cpp
static ThumbnailResult okTile()
{
    ThumbnailResult r;
    r.status = ThumbnailStatus::Ok;
    r.rgba.assign(static_cast<size_t>(kThumbCell) * kThumbCell * 4, 128);
    return r;
}

// Review Focus 2: cambia la textura de la que depende la miniatura, no el asset.
static void test_cache_dependency_change_regenerates(const fs::path& dir)
{
    const fs::path asset = makeImage(dir, "dep_asset.tga");
    const fs::path dep   = makeImage(dir, "dep_texture.tga");
    int calls = 0;
    CacheHarness h(4, kThumbSlotCount, [&](const fs::path&) {
        ++calls;
        ThumbnailResult r = okTile();
        r.dependencies.push_back({ dep });
        return r;
    });
    h.cache.beginFrame();
    h.cache.request(asset);
    h.cache.pump(); h.runAll(); h.cache.pump();
    CHECK(calls == 1);

    h.cache.beginFrame();
    h.cache.refreshStamps();
    CHECK(h.cache.request(asset).has_value());          // sin cambios: sigue lista

    bumpMtime(dep);
    h.cache.beginFrame();
    h.cache.refreshStamps();
    CHECK(!h.cache.request(asset).has_value());
    h.cache.pump(); h.runAll(); h.cache.pump();
    CHECK(calls == 2);
}

// Review Focus 1: el asset cambia mientras se decodifica. El mtime se tomo ANTES,
// asi que el siguiente refreshStamps lo ve y regenera.
static void test_cache_change_during_decode_regenerates(const fs::path& dir)
{
    const fs::path asset = makeImage(dir, "racy.tga");
    int calls = 0;
    CacheHarness h(4, kThumbSlotCount, [&](const fs::path& p) {
        if (++calls == 1) bumpMtime(p);                  // "reexportado" a mitad de decodificar
        return okTile();
    });
    h.cache.beginFrame();
    h.cache.request(asset);
    h.cache.pump(); h.runAll(); h.cache.pump();
    h.cache.beginFrame();
    h.cache.refreshStamps();
    CHECK(!h.cache.request(asset).has_value());
    h.cache.pump(); h.runAll(); h.cache.pump();
    CHECK(calls == 2);
}

static void test_cache_status_reports_animation_only(const fs::path& dir)
{
    const fs::path f = makeImage(dir, "anim_only.fbx");     // el contenido da igual: decodificador falso
    CacheHarness h(4, kThumbSlotCount, [](const fs::path&) {
        ThumbnailResult r;
        r.status = ThumbnailStatus::AnimationOnly;
        return r;
    });
    h.cache.beginFrame();
    CHECK(!h.cache.status(f).has_value());               // pendiente: aun no se sabe
    h.cache.request(f);
    h.cache.pump(); h.runAll(); h.cache.pump();
    CHECK(h.cache.status(f) == ThumbnailStatus::AnimationOnly);
    h.cache.beginFrame();
    CHECK(!h.cache.request(f).has_value());
    h.cache.pump();
    CHECK(h.pending.empty());                            // no se reintenta
    CHECK(h.uploads.empty());
}

// Review Focus 4: 4 modelos y 2 imagenes a la vez -> 2 modelos + 2 imagenes en vuelo.
static void test_cache_caps_models_in_flight(const fs::path& dir)
{
    CacheHarness h(4, kThumbSlotCount, [](const fs::path&) { return okTile(); });
    h.cache.beginFrame();
    for (int i = 0; i < 4; ++i) h.cache.request(makeImage(dir, ("m" + std::to_string(i) + ".fbx").c_str()));
    for (int i = 0; i < 2; ++i) h.cache.request(makeImage(dir, ("i" + std::to_string(i) + ".tga").c_str()));
    h.cache.pump();
    CHECK(h.cache.inFlight() == 4);
    CHECK(h.cache.modelsInFlight() == 2);

    h.runAll();
    h.cache.pump();                                      // quedan los 2 modelos
    CHECK(h.cache.modelsInFlight() == 2);
    h.runAll();
    h.cache.pump();
    CHECK(h.tilesUploaded() == 6);
    CHECK(h.cache.modelsInFlight() == 0);
}

static void test_cache_disk_hit_skips_the_decoder(const fs::path& dir)
{
    auto disk = std::make_shared<ThumbnailDiskCache>(dir / "cache_hit");
    const fs::path f = makeImage(dir, "hit.tga");
    ThumbnailResult stored = okTile();
    stampDependencies(stored, stampFile(f));
    CHECK(disk->store(f, stored));

    int calls = 0;
    CacheHarness h(4, kThumbSlotCount, [&](const fs::path&) { ++calls; return okTile(); }, disk);
    h.cache.beginFrame();
    h.cache.request(f);
    h.cache.pump(); h.runAll(); h.cache.pump();
    CHECK(calls == 0);
    CHECK(h.tilesUploaded() == 1);
}

static void test_cache_stores_decoded_results_on_disk(const fs::path& dir)
{
    auto disk = std::make_shared<ThumbnailDiskCache>(dir / "cache_store");
    const fs::path f = makeImage(dir, "store.tga");
    CacheHarness h(4, kThumbSlotCount, {}, disk);         // decodificador real
    h.cache.beginFrame();
    h.cache.request(f);
    h.cache.pump(); h.runAll(); h.cache.pump();
    const auto back = disk->load(f);
    CHECK(back.has_value());
    if (back) CHECK(back->rgba == makeThumbnail(f).rgba);
}
```

En `main()`:

```cpp
    test_cache_dependency_change_regenerates(dir);
    test_cache_change_during_decode_regenerates(dir);
    test_cache_status_reports_animation_only(dir);
    test_cache_caps_models_in_flight(dir);
    test_cache_disk_hit_skips_the_decoder(dir);
    test_cache_stores_decoded_results_on_disk(dir);
```

- [ ] **Step 2: Compilar y ver que falla**

Expected: el constructor de `ThumbnailCache` no acepta 7 argumentos; `status`/`modelsInFlight` no existen.

- [ ] **Step 3: Cambiar la clase en `Thumbnail.h`**

- Declaración adelantada `class ThumbnailDiskCache;` antes de `class ThumbnailCache`.
- El constructor y los dos métodos del bloque *Produces*; comentario de `status` tal cual.
- En el comentario del `Decoder`: `// Por defecto makeAssetThumbnail.`
- `Entry` queda:

```cpp
    struct Entry
    {
        std::filesystem::path            path;
        std::vector<ThumbnailDependency> deps;              // [0] = el propio asset
        uint64_t                         key = 0;
        State                            state = State::Queued;
        ThumbnailStatus                  status = ThumbnailStatus::Ok;   // motivo si Failed
        bool                             model = false;     // cuenta para el tope de modelos
        std::vector<uint8_t>             pixels;            // solo en Decoded
        uint64_t                         lastRequestFrame = 0;
    };
```

- `Done` gana `bool model = false;`.
- `makeKey` pasa a `static uint64_t makeKey(const std::filesystem::path& path, int64_t mtime);`.
- Miembros nuevos tras `m_decode`: `std::shared_ptr<const ThumbnailDiskCache> m_disk;` y tras `m_maxInFlight`: `uint32_t m_maxModelsInFlight; uint32_t m_modelsInFlight = 0;`.
- `uint32_t modelsInFlight() const { return m_modelsInFlight; }` junto a `inFlight()`.

- [ ] **Step 4: Cambiar la implementación en `Thumbnail.cpp`**

Añadir `#include "DonTopo/Editor/ThumbnailDiskCache.h"`. Constructor:

```cpp
ThumbnailCache::ThumbnailCache(Runner run, Uploader upload, uint32_t maxInFlight,
                               uint32_t slotCapacity, Decoder decode,
                               std::shared_ptr<const ThumbnailDiskCache> disk,
                               uint32_t maxModelsInFlight)
    : m_run(std::move(run))
    , m_upload(std::move(upload))
    , m_decode(decode ? std::move(decode) : Decoder(makeAssetThumbnail))
    , m_disk(std::move(disk))
    , m_maxInFlight(maxInFlight)
    , m_maxModelsInFlight(maxModelsInFlight)
    , m_slots(slotCapacity)
{}
```

`makeKey`:

```cpp
uint64_t ThumbnailCache::makeKey(const std::filesystem::path& path, int64_t mtime)
{
    // Ruta + mtime del asset. Un cambio de dependencia no necesita otra clave:
    // refreshStamps libera la casilla antes de que la entrada se vuelva a pedir.
    const uint64_t h = std::hash<std::string>{}(path.string());
    const uint64_t t = static_cast<uint64_t>(mtime);
    return h ^ (t + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2));
}
```

En `request`, sustituir la creación de la entrada nueva:

```cpp
    if (it == m_entries.end())
    {
        Entry e;
        e.path = path;
        // Sellado AQUI, antes de decodificar: si el fichero cambia mientras el
        // worker lo lee, el siguiente refreshStamps lo ve (Review Focus 1).
        const ThumbnailDependency self = stampFile(path);
        e.deps             = { self };
        e.key              = makeKey(path, self.mtime);
        e.model            = isModelThumbnailPath(path);
        e.state            = self.exists ? State::Queued : State::Failed;
        e.status           = self.exists ? ThumbnailStatus::Ok : ThumbnailStatus::Unreadable;
        e.lastRequestFrame = m_frame;
        const bool queue   = (e.state == State::Queued);
        m_entries.emplace(id, std::move(e));
        if (queue) m_queue.push_back(id);
        return std::nullopt;
    }
```

En `pump`, el bucle de resultados:

```cpp
    for (Done& d : done)
    {
        if (m_inFlight > 0) --m_inFlight;                    // el hueco se libera siempre
        if (d.model && m_modelsInFlight > 0) --m_modelsInFlight;
        if (d.generation != m_generation) continue;          // carpeta anterior: se ignora
        const auto it = m_entries.find(d.path);
        if (it == m_entries.end() || it->second.state != State::Running) continue;
        Entry& e = it->second;
        if (!d.result.dependencies.empty()) e.deps = std::move(d.result.dependencies);
        e.status = d.result.status;
        if (d.result.status != ThumbnailStatus::Ok)
        {
            e.state = State::Failed;
            continue;
        }
        e.pixels = std::move(d.result.rgba);
        e.state  = State::Decoded;
    }
```

Y en la rama de subida fallida, junto a `e->state = State::Failed;`, añadir `e->status = ThumbnailStatus::Unreadable;`.

`startJobs` completo:

```cpp
void ThumbnailCache::startJobs()
{
    if (!m_run) return;
    for (auto q = m_queue.begin(); q != m_queue.end() && m_inFlight < m_maxInFlight;)
    {
        const auto it = m_entries.find(*q);
        if (it == m_entries.end() || it->second.state != State::Queued)
        {
            q = m_queue.erase(q);
            continue;
        }
        Entry& e = it->second;
        // Un FBX puede tardar segundos: como mucho m_maxModelsInFlight a la vez, y
        // los que esperan no bloquean a las imagenes que vienen detras en la cola.
        if (e.model && m_modelsInFlight >= m_maxModelsInFlight)
        {
            ++q;
            continue;
        }
        const std::string id = *q;
        q = m_queue.erase(q);

        e.state = State::Running;
        ++m_inFlight;
        if (e.model) ++m_modelsInFlight;
        const std::shared_ptr<Shared>                   shared = m_shared;
        const std::shared_ptr<const ThumbnailDiskCache> disk   = m_disk;
        const uint64_t                                  gen    = m_generation;
        const std::filesystem::path                     path   = e.path;
        const ThumbnailDependency                       self   = e.deps.front();
        const bool                                      model  = e.model;
        const Decoder                                   decode = m_decode;
        const bool accepted = m_run([shared, disk, gen, id, path, self, model, decode]() {
            Done d;
            d.generation = gen;
            d.path       = id;
            d.model      = model;
            // Un Done SIEMPRE llega: si algo lanza, el hueco en vuelo se
            // recogeria nunca y tras maxInFlight fallos no habria mas miniaturas.
            try
            {
                std::optional<ThumbnailResult> hit;
                if (disk) hit = disk->load(path);
                if (hit)
                {
                    d.result = std::move(*hit);
                }
                else
                {
                    d.result = decode(path);
                    stampDependencies(d.result, self);
                    if (disk) disk->store(path, d.result);
                }
            }
            catch (...) { d.result = ThumbnailResult{}; }
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->done.push_back(std::move(d));
        });
        if (!accepted)
        {
            // El pool no lo ejecutara jamas (parado): sin esto el hueco no se devuelve.
            --m_inFlight;
            if (model) --m_modelsInFlight;
            e.state  = State::Failed;
            e.status = ThumbnailStatus::Unreadable;
        }
    }
}
```

`refreshStamps`, sustituir la comparación del `mtime`:

```cpp
        const bool changed = std::any_of(e.deps.begin(), e.deps.end(), [](const ThumbnailDependency& d) {
            return !(stampFile(d.path) == d);
        });
        if (!changed)
        {
            ++it;
            continue;
        }
```

`status`:

```cpp
std::optional<ThumbnailStatus> ThumbnailCache::status(const std::filesystem::path& path) const
{
    const auto it = m_entries.find(path.string());
    if (it == m_entries.end()) return std::nullopt;
    if (it->second.state == State::Ready)  return ThumbnailStatus::Ok;
    if (it->second.state == State::Failed) return it->second.status;
    return std::nullopt;
}
```

- [ ] **Step 5: Compilar y ejecutar**

Expected: `ALL THUMBNAIL TESTS PASSED`, **incluidos todos los tests de `ThumbnailCache` que ya existían** (mtime, generación, fallos, tope de 4, resultado tardío).

- [ ] **Step 6: Sabotaje**

Uno a uno: (a) en `refreshStamps` comparar solo `e.deps.front()` → debe fallar `test_cache_dependency_change_regenerates`; (b) en el job, sellar `self` con `stampFile(path)` DESPUÉS de `decode` en vez de usar el capturado → debe fallar `test_cache_change_during_decode_regenerates`; (c) quitar el `if (e.model && m_modelsInFlight >= ...)` → debe fallar `test_cache_caps_models_in_flight`; (d) quitar el `if (hit)` (decodificar siempre) → debe fallar `test_cache_disk_hit_skips_the_decoder`.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Editor/Thumbnail.h engine/src/Editor/Thumbnail.cpp engine/tests/thumbnail_tests.cpp
git commit -m "feat(editor): ThumbnailCache invalida por dependencias, usa la cache en disco y limita los modelos en vuelo"
```

---

### Task 6: Content Browser, documentación

**Files:**
- Modify: `engine/include/DonTopo/Editor/ContentBrowserPanel.h` (junto a `assetIconButtonLabel`, línea ~24, y tras `classifyAsset`, línea ~51)
- Modify: `engine/src/Editor/ContentBrowserPanel.cpp` (`assetIconButtonLabel` ~185, creación del cache ~1209-1224, grid ~1362-1381)
- Modify: `README.md` (~línea 763), `docs/assets-editor-audit.md` (fila U3 y casilla de la §5)
- Test: `engine/tests/thumbnail_tests.cpp`

**Interfaces:**
- Consumes: `ThumbnailCache` (ctor de 7 argumentos, `status`), `ThumbnailDiskCache`, `ThumbnailStatus::AnimationOnly` (Tasks 3-5).
- Produces:

```cpp
// ContentBrowserPanel.h, tras classifyAsset:
// Los tipos del grid que tienen miniatura: imagenes, modelos y materiales.
bool wantsThumbnail(AssetKind kind);
```

- [ ] **Step 1: Test que falla**

En `thumbnail_tests.cpp`:

```cpp
static void test_wants_thumbnail_kinds()
{
    CHECK(wantsThumbnail(AssetKind::Image));
    CHECK(wantsThumbnail(AssetKind::Model3D));
    CHECK(wantsThumbnail(AssetKind::Material));
    for (AssetKind k : { AssetKind::Folder, AssetKind::Audio, AssetKind::Font, AssetKind::Scene,
                         AssetKind::Script, AssetKind::Shader, AssetKind::Other })
        CHECK(!wantsThumbnail(k));
}
```

y `test_wants_thumbnail_kinds();` en `main()`.

- [ ] **Step 2: Compilar y ver que falla** (`wantsThumbnail` no declarado).

- [ ] **Step 3: Implementar `wantsThumbnail`**

Declararlo en el `.h` con el comentario de *Produces*; en el `.cpp`, tras `assetIconButtonLabel`:

```cpp
bool wantsThumbnail(AssetKind kind)
{
    return kind == AssetKind::Image || kind == AssetKind::Model3D || kind == AssetKind::Material;
}
```

- [ ] **Step 4: Caché de disco del proyecto**

En `ContentBrowserPanel.cpp` añadir `#include "DonTopo/Editor/ThumbnailDiskCache.h"`. En la creación de `m_thumbs` (~línea 1217):

```cpp
                // Junto a la de fuentes, en una carpeta con punto: .gitignore ya
                // la excluye e isHiddenDir la oculta del propio Content Browser.
                std::shared_ptr<const ThumbnailDiskCache> disk;
                if (!m_projectRoot.empty())
                    disk = std::make_shared<ThumbnailDiskCache>(m_projectRoot / ".dt-cache" / "thumbs");
                m_thumbs = std::make_unique<ThumbnailCache>(
                    [jobs](std::function<void()> job) { return jobs->submit(std::move(job)) != 0; },
                    [renderer](const ThumbnailTile* tiles, size_t count) {
                        return renderer->uploadUiThumbnails(tiles, count);
                    },
                    4, kThumbSlotCount, ThumbnailCache::Decoder{}, std::move(disk));
```

- [ ] **Step 5: Grid: pedir modelos y materiales, etiqueta `ANI`**

Sustituir el bloque de la petición (~líneas 1376-1381):

```cpp
            // Solo lo que esta a la vista: una carpeta de miles de assets no debe
            // lanzar miles de decodificaciones.
            std::optional<UvRect> thumb;
            if (m_thumbs && wantsThumbnail(kind) &&
                ImGui::IsRectVisible(ImVec2(ICON_SIZE, ICON_SIZE)))
            {
                thumb = m_thumbs->request(path);
                // Un FBX sin malla (solo clips) no es un fallo: icono propio.
                if (!thumb && kind == AssetKind::Model3D &&
                    m_thumbs->status(path) == ThumbnailStatus::AnimationOnly)
                {
                    btnColor = ImVec4(0.10f, 0.40f, 0.60f, 1.0f);
                    label    = "ANI";
                }
            }
```

- [ ] **Step 6: Compilar, tests y suite**

Run: build; `dt_thumbnail_tests.exe` → `ALL THUMBNAIL TESTS PASSED`; luego la suite completa con el snippet (en segundo plano).
Expected: `SUMMARY total 35, fallos:` vacío y `SANDBOX alive after 7s`.

- [ ] **Step 7: Documentación**

`README.md`, línea ~763: cambiar `shows real thumbnails for textures,` por `shows real thumbnails for textures, 3D models and materials (rendered on the CPU in a worker and cached in `.dt-cache/thumbs/`; animation-only FBX files get an `ANI` icon),`.

`docs/assets-editor-audit.md`:
- Fila U3 de «Estado vigente»: estado **CERRADO**; texto: `Texturas: atlas compartido de 2048² con casillas de 64², decodificación en el JobSystem, subida en lote en Vulkan y D3D12; merge 1f7ad36. Modelos (.fbx/.obj) y materiales (.mat): rasterizado en CPU en el worker (ModelLoader::loadPreview + rasterizeThumbnail), sin código GPU nuevo; todas las miniaturas se guardan en .dt-cache/thumbs/ con invalidación por dependencias. Spec docs/superpowers/specs/2026-09-25-model-material-thumbnails-design.md. .gltf/.glb siguen sin miniatura: Assimp está compilado solo con OBJ y FBX.`
- Párrafo bajo la tabla: `la 2 a **EXISTE solo para texturas**` → `la 2 también (texturas, modelos y materiales)`.
- §5: marcar `[x]` la casilla «Miniaturas de modelos y materiales (`.mat`)».
- Línea `Estado vigente (2026-09-25)` se queda.

Tras cada `Edit`, `git diff --stat` (CRLF).

- [ ] **Step 8: Commit**

```bash
git add engine/include/DonTopo/Editor/ContentBrowserPanel.h engine/src/Editor/ContentBrowserPanel.cpp engine/tests/thumbnail_tests.cpp README.md docs/assets-editor-audit.md
git commit -m "feat(editor): miniaturas de modelos y materiales en el Content Browser, con cache en disco"
```

---

### Task 7: Medición en Release y verificación manual

**Files:**
- Temporal, NO se commitea: `<scratchpad>/preview_timing.cpp` y una línea en `engine/tests/CMakeLists.txt` que se revierte.

- [ ] **Step 1: Medir `loadPreview` frente a `loadAuto` en Release**

Crear en el scratchpad `preview_timing.cpp`:

```cpp
#include "DonTopo/Renderer/ModelLoader.h"
#include <chrono>
#include <cstdio>

using namespace DonTopo;

int main()
{
    for (const char* f : { "assets/model.fbx", "assets/modelTexture.fbx", "assets/modelAnimation.fbx",
                           "assets/animatedCharacter/Maw J Laygo.fbx" })
    {
        const auto t0 = std::chrono::steady_clock::now();
        (void)ModelLoader::loadAuto(f);
        const auto t1 = std::chrono::steady_clock::now();
        (void)ModelLoader::loadPreview(f);
        const auto t2 = std::chrono::steady_clock::now();
        std::printf("%-45s loadAuto %7.1f ms  loadPreview %7.1f ms\n", f,
                    std::chrono::duration<double, std::milli>(t1 - t0).count(),
                    std::chrono::duration<double, std::milli>(t2 - t1).count());
    }
}
```

Añadir al final de `engine/tests/CMakeLists.txt` `dt_add_test(dt_preview_timing "<ruta absoluta con / >/preview_timing.cpp" DonTopoCore)`, compilar **en el build Release** con `cmd /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul && chcp 65001 >nul && cmake --build build-ninja-release --target dt_preview_timing'` y ejecutar desde la raíz `.\build-ninja-release\engine\tests\dt_preview_timing.exe`. Después `git checkout -- engine/tests/CMakeLists.txt` y borrar el exe.

Expected: `loadPreview` claramente por debajo de `loadAuto` en `modelAnimation.fbx`. Si no baja al menos a la mitad, **parar e informar** (riesgo 1 de la spec) con la tabla.

- [ ] **Step 2: Verificación manual (Release, las dos APIs)**

Arrancar el editor Release (`.\build-ninja-release\sandbox\Sandbox.exe`, working dir `build-ninja-release\sandbox`) con el proyecto en Vulkan y luego en D3D12 (el backend sale del `project.json` del último proyecto: cambiarlo y **abrir el proyecto desde la GUI**, o no se verifica nada del otro backend). Comprobar y anotar:

1. `assets/` y `assets/animatedCharacter/`: miniaturas de modelos; `standing idle 01.fbx` y el resto de FBX de solo animación con icono `ANI`; mismas miniaturas en los dos backends.
2. `Maw J Laygo.fbx`: comparar su miniatura contra el modelo arrastrado al viewport (duda del spike).
3. Un `.mat` nuevo (Create > Material) con albedo y roughness cambiados en su modal: la miniatura cambia en ≤ 0,5 s.
4. Import Settings de un FBX con `Normals = Flat` y Aplicar: la miniatura cambia en ≤ 0,5 s.
5. Cerrar y reabrir el editor: las miniaturas salen al instante (sin el retraso de Assimp) y existe `.dt-cache/thumbs/` con ficheros `.bin` y ningún `.tmp`.
6. Entrar en `animatedCharacter/` (13 FBX): el editor no se congela.

- [ ] **Step 3: Informe**

Escribir el resultado (tabla de tiempos + los 6 puntos, con lo que no se pudo comprobar dicho como tal) en el mensaje final. Si algún punto falla, no se da la feature por terminada.
