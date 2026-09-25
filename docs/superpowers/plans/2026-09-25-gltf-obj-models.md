# Modelos glTF y OBJ en el editor — Plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `.fbx`, `.obj`, `.gltf` y `.glb` se tratan igual en todo el editor (clasificar, Add Mesh, soltar, importar, miniaturas, fuentes de animación, export), con el importador glTF de Assimp compilado.

**Architecture:** `ModelLoader` gana tres funciones puras que son la única fuente de verdad: `isSupportedModelExtension` (+ `supportedModelFilter`), `resolveModelTexture` (subcarpeta primero, nombre suelto de respaldo) y `modelCompanionFiles` (`mtllib` de un `.obj`; `buffers`/`images` externos de un `.gltf`). El editor sustituye sus cinco listas de extensiones por la primera; el loader y el preview resuelven texturas con la segunda; el exportador y las miniaturas usan la tercera. `ASSIMP_BUILD_GLTF_IMPORTER` pasa a ON.

**Tech Stack:** C++20, Assimp 5.3.1, nlohmann_json, ImGuiFileDialog. Tests planos `main()` + `CHECK`, `dt_add_test`.

**Spec:** `docs/superpowers/specs/2026-09-25-gltf-obj-models-design.md`

## Global Constraints

- **Formatos de modelo**: exactamente `.fbx`, `.obj`, `.gltf`, `.glb`, sin distinguir mayúsculas. Ninguna otra lista de extensiones de modelo en el repo.
- **Sin dependencias de terceros nuevas**; Draco sigue apagado.
- **Texturas externas**: primero `modelDir / ruta relativa` si es relativa, sin `..` y existe; si no, `modelDir / nombre suelto` (lo de siempre). FBX/OBJ que hoy funcionan no cambian de resultado.
- **Ficheros asociados**: rutas relativas a la carpeta del modelo, sin duplicados, URIs de glTF decodificadas de `%XX`; `data:`, absolutas (`has_root_path`) y con `..` se ignoran; `.glb` y `.fbx` → vacío.
- **Export**: cada asociado se coloca en `dirname(packagePath del modelo) / relativa`, y se añade antes que las texturas del material.
- **`kThumbDiskVersion` = 3.**
- **CRLF**: tras cada `Edit`, `git diff --stat`. Nada de `sed -i` ni Get-Content/Set-Content. Para borrar bloques, Python con `splitlines(keepends=True)`.
- **Tests desde la raíz del repo**, salvo `dt_exporter_tests`, que va desde `build-ninja/engine/tests` y cuenta ficheros del cwd (no dejar temporales ahí).
- **Build de un target**: `cmd /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul && chcp 65001 >nul && cmake --build build-ninja --target <target>'`. Cambiar `CMakeLists.txt` raíz reconfigura y recompila Assimp (varios minutos: en segundo plano).
- **GCC 13 en el CI de Linux**: includes explícitos (`<cstring>`, `<cctype>`, `<fstream>`…).
- **Suite**: `total 35` antes y después.

## Review Focus

1. **Un `.gltf` cuyo `.bin` falta o se movió**: la carga falla con error (no crash) y la miniatura queda `Unreadable` y se regenera al aparecer el `.bin`. → Task 2 (`test_gltf_missing_bin_fails_cleanly`, `test_thumbnail_gltf_declares_its_bin`).
2. **Mayúsculas en la extensión** (`HERO.GLB` arrastrado del Explorador): se acepta en todos los sitios. → Task 1 (`test_supported_model_extensions`).
3. **Un FBX que ya funcionaba sigue cargando su textura**: el cambio de resolución no puede romperlo. → Task 1 (`test_resolve_texture_falls_back_to_the_bare_name`) y la suite con los FBX de `assets/`.
4. **Modelo externo con textura en subcarpeta exportado**: modelo, `.bin` y textura siguen en la misma relación dentro de `assets/_external/N`. → Task 4 (`test_gltf_outside_the_project_keeps_its_companions_together`).
5. **URI con espacios (`%20`)**: el asociado se encuentra, se vigila y se empaqueta con su nombre real. → Task 1 (`test_companions_of_gltf`) y Task 4.

---

## File Structure

| Fichero | Responsabilidad |
|---|---|
| `engine/include/DonTopo/Renderer/ModelLoader.h`, `engine/src/Renderer/ModelLoader.cpp` (modif.) | `isSupportedModelExtension`, `supportedModelFilter`, `resolveModelTexture`, `modelCompanionFiles`; usarlas en `load`, `loadSkinned`, `loadPreview` |
| `CMakeLists.txt` (modif., línea ~112) | `ASSIMP_BUILD_GLTF_IMPORTER ON` |
| `engine/include/DonTopo/Editor/ThumbnailDiskCache.h` (modif.) | versión 3 |
| `engine/src/Editor/ContentBrowserPanel.cpp`, `PropertiesPanel.cpp`, `AnimatorPanel.cpp`, `AssetImport.cpp`, `Thumbnail.cpp` (modif.) | usar la lista única |
| `engine/src/Editor/GameExporter.cpp` (modif.) | asociados en `collectSceneAssets` |
| `engine/tests/model_import_tests.cpp`, `thumbnail_tests.cpp`, `asset_import_tests.cpp`, `content_browser_tests.cpp`, `exporter_tests.cpp` (modif.) | tests |
| `README.md` (modif., líneas 67 y 773) | formatos |

---

### Task 1: Las tres funciones puras en `ModelLoader`

**Files:**
- Modify: `engine/include/DonTopo/Renderer/ModelLoader.h`, `engine/src/Renderer/ModelLoader.cpp`
- Test: `engine/tests/model_import_tests.cpp`

**Interfaces:**
- Produces (dentro de `class ModelLoader`, públicas):

```cpp
static bool isSupportedModelExtension(const std::string& ext);
static const char* supportedModelFilter();     // "Models{.fbx,.obj,.gltf,.glb}"
static std::filesystem::path resolveModelTexture(const std::filesystem::path& modelDir,
                                                 const std::string& raw);
static std::vector<std::string> modelCompanionFiles(const std::string& path);
```

- [ ] **Step 1: Tests que fallan**

En `model_import_tests.cpp` añadir `#include <cstring>` y, antes de `int main()`:

```cpp
// ── Formatos, texturas en subcarpeta y ficheros asociados ────────────────────

static void test_supported_model_extensions()
{
    for (const char* e : { ".fbx", ".FBX", ".obj", ".gltf", ".GLB", ".glb" })
        CHECK(ModelLoader::isSupportedModelExtension(e));
    for (const char* e : { ".dae", ".blend", ".png", "", "fbx" })
        CHECK(!ModelLoader::isSupportedModelExtension(e));
    const std::string filter = ModelLoader::supportedModelFilter();
    for (const char* e : { ".fbx", ".obj", ".gltf", ".glb" })
        CHECK(filter.find(e) != std::string::npos);
}

static void test_resolve_texture_prefers_the_subfolder()
{
    const fs::path dir = makeDir("dt_resolve_sub");
    fs::create_directories(dir / "textures");
    writeText(dir / "textures" / "x.tga", "sub");
    writeText(dir / "x.tga", "root");                         // homonima junto al modelo
    CHECK(sameAssetPath(ModelLoader::resolveModelTexture(dir, "textures/x.tga"), dir / "textures" / "x.tga"));
}

// Review Focus 3: lo de siempre (nombre suelto) sigue funcionando.
static void test_resolve_texture_falls_back_to_the_bare_name()
{
    const fs::path dir = makeDir("dt_resolve_bare");
    writeText(dir / "x.tga", "root");
    CHECK(sameAssetPath(ModelLoader::resolveModelTexture(dir, "textures/x.tga"), dir / "x.tga"));
    CHECK(sameAssetPath(ModelLoader::resolveModelTexture(dir, "x.tga"), dir / "x.tga"));
    CHECK(sameAssetPath(ModelLoader::resolveModelTexture(dir, "../fuera/x.tga"), dir / "x.tga"));
    CHECK(sameAssetPath(ModelLoader::resolveModelTexture(dir, "C:/artista/x.tga"), dir / "x.tga"));
    CHECK(sameAssetPath(ModelLoader::resolveModelTexture(dir, "/home/artista/x.tga"), dir / "x.tga"));
}

static void test_companions_of_obj()
{
    const fs::path dir = makeDir("dt_companions_obj");
    writeText(dir / "m.obj", "mtllib a.mtl\n# comentario\nmtllib   sub/b.mtl  \r\nmtllib a.mtl\nv 0 0 0\n");
    const std::vector<std::string> c = ModelLoader::modelCompanionFiles((dir / "m.obj").string());
    CHECK(c.size() == 2);
    if (c.size() == 2) { CHECK(c[0] == "a.mtl"); CHECK(c[1] == "sub/b.mtl"); }
}

// Review Focus 5: %20 se decodifica; data:, absolutas y .. se ignoran.
static void test_companions_of_gltf()
{
    const fs::path dir = makeDir("dt_companions_gltf");
    writeText(dir / "m.gltf", R"({"asset":{"version":"2.0"},
        "buffers":[{"uri":"tri.bin","byteLength":4},{"uri":"data:application/octet-stream;base64,AAAA","byteLength":3}],
        "images":[{"uri":"textures/rojo%20x.tga"},{"uri":"../fuera.png"},{"uri":"/abs.png"},{"uri":"C:/abs.png"},{"uri":"tri.bin"}]})");
    const std::vector<std::string> c = ModelLoader::modelCompanionFiles((dir / "m.gltf").string());
    CHECK(c.size() == 2);
    if (c.size() == 2) { CHECK(c[0] == "tri.bin"); CHECK(c[1] == "textures/rojo x.tga"); }
}

static void test_companions_of_other_formats_are_empty()
{
    const fs::path dir = makeDir("dt_companions_other");
    writeText(dir / "m.glb", "glTF");
    writeText(dir / "roto.gltf", "{ esto no es json");
    CHECK(ModelLoader::modelCompanionFiles((dir / "m.glb").string()).empty());
    CHECK(ModelLoader::modelCompanionFiles("assets/modelTexture.fbx").empty());
    CHECK(ModelLoader::modelCompanionFiles((dir / "roto.gltf").string()).empty());
    CHECK(ModelLoader::modelCompanionFiles((dir / "no_existe.obj").string()).empty());
}
```

En `main()`, antes del `if (g_failures == 0)`:

```cpp
    test_supported_model_extensions();
    test_resolve_texture_prefers_the_subfolder();
    test_resolve_texture_falls_back_to_the_bare_name();
    test_companions_of_obj();
    test_companions_of_gltf();
    test_companions_of_other_formats_are_empty();
```

- [ ] **Step 2: Compilar y ver que falla** — `dt_model_import_tests`. Expected: `isSupportedModelExtension` no es miembro de `ModelLoader`.

- [ ] **Step 3: Declarar en `ModelLoader.h`** (dentro de la clase, tras `decodePreviewImage`):

```cpp
            // Formatos de modelo que Assimp tiene compilados. La UNICA lista: el
            // editor entero pregunta aqui (clasificar, Add Mesh, importar,
            // miniaturas, Animator). Sin distinguir mayusculas; ext con el punto.
            static bool isSupportedModelExtension(const std::string& ext);
            // Filtro para ImGuiFileDialog con los mismos formatos.
            static const char* supportedModelFilter();

            // Ruta de una textura externa referenciada por el modelo: primero la
            // ruta relativa TAL CUAL respecto a modelDir (textures/x.png); si no
            // existe, el nombre suelto junto al modelo, que es lo de siempre. Una
            // ruta absoluta o que salga de la carpeta (..) solo prueba el nombre.
            static std::filesystem::path resolveModelTexture(const std::filesystem::path& modelDir,
                                                             const std::string& raw);

            // Ficheros que el modelo lee ademas de si mismo, en rutas RELATIVAS a
            // su carpeta (separador /), sin duplicados: los mtllib de un .obj y
            // los buffers[].uri e images[].uri externos de un .gltf (decodificados
            // de %XX; data:, absolutas y con .. fuera). .glb y .fbx: ninguno.
            // Nunca lanza: un fichero ilegible devuelve vacio.
            static std::vector<std::string> modelCompanionFiles(const std::string& path);
```

- [ ] **Step 4: Implementar en `ModelLoader.cpp`**

Añadir `#include <nlohmann/json.hpp>` y `#include <algorithm>`. Tras `loadAuto`:

```cpp
    namespace
    {
        std::string lowerExtOf(const std::filesystem::path& p)
        {
            std::string e = p.extension().string();
            for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return e;
        }

        // Relativa, sin raiz y sin componentes "..": se queda dentro de la carpeta.
        bool staysInside(const std::filesystem::path& rel)
        {
            if (rel.empty() || rel.has_root_path()) return false;
            for (const auto& part : rel)
                if (part == "..") return false;
            return true;
        }

        std::string percentDecode(const std::string& s)
        {
            std::string out;
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == '%' && i + 2 < s.size() &&
                    std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                    std::isxdigit(static_cast<unsigned char>(s[i + 2])))
                {
                    out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
                    i += 2;
                }
                else out += s[i];
            }
            return out;
        }

        void addCompanion(std::vector<std::string>& out, const std::string& raw)
        {
            if (raw.empty() || raw.rfind("data:", 0) == 0) return;
            // "C:/x" no tiene raiz en Linux ni "/x" nombre de unidad en Windows:
            // se rechazan las dos formas en cualquier plataforma.
            if (raw.size() > 1 && raw[1] == ':') return;
            const std::filesystem::path rel = std::filesystem::path(raw).lexically_normal();
            if (!staysInside(rel)) return;
            const std::string s = rel.generic_string();
            if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
        }
    }

    bool ModelLoader::isSupportedModelExtension(const std::string& ext)
    {
        std::string e = ext;
        for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return e == ".fbx" || e == ".obj" || e == ".gltf" || e == ".glb";
    }

    const char* ModelLoader::supportedModelFilter()
    {
        return "Models{.fbx,.obj,.gltf,.glb}";
    }

    std::filesystem::path ModelLoader::resolveModelTexture(const std::filesystem::path& modelDir,
                                                           const std::string& raw)
    {
        namespace fs = std::filesystem;
        const fs::path byName = modelDir / fs::path(raw).filename();
        const bool looksAbsolute = raw.size() > 1 && raw[1] == ':';
        const fs::path rel = fs::path(raw).lexically_normal();
        if (!looksAbsolute && staysInside(rel) && rel != rel.filename())
        {
            std::error_code ec;
            const fs::path sub = modelDir / rel;
            if (fs::exists(sub, ec) && !ec) return sub;
        }
        return byName;
    }

    std::vector<std::string> ModelLoader::modelCompanionFiles(const std::string& path)
    {
        namespace fs = std::filesystem;
        std::vector<std::string> out;
        try
        {
            const std::string ext = lowerExtOf(path);
            if (ext == ".obj")
            {
                std::ifstream in{ fs::path(path) };
                std::string line;
                while (std::getline(in, line))
                {
                    if (line.rfind("mtllib", 0) != 0 || line.size() < 7 ||
                        !std::isspace(static_cast<unsigned char>(line[6])))
                        continue;
                    size_t b = 7, e = line.size();
                    while (b < e && std::isspace(static_cast<unsigned char>(line[b]))) ++b;
                    while (e > b && std::isspace(static_cast<unsigned char>(line[e - 1]))) --e;
                    addCompanion(out, line.substr(b, e - b));
                }
            }
            else if (ext == ".gltf")
            {
                std::ifstream in{ fs::path(path) };
                const nlohmann::json j = nlohmann::json::parse(in, nullptr, /*allow_exceptions*/ false);
                if (!j.is_object()) return {};
                for (const char* key : { "buffers", "images" })
                {
                    const auto it = j.find(key);
                    if (it == j.end() || !it->is_array()) continue;
                    for (const auto& item : *it)
                        if (item.is_object() && item.contains("uri") && item["uri"].is_string())
                            addCompanion(out, percentDecode(item["uri"].get<std::string>()));
                }
            }
        }
        catch (...) { return {}; }
        return out;
    }
```

Nota: `addCompanion` rechaza `data:` antes de decodificar porque se llama con la URI ya decodificada; un `data:` no contiene `%` que cambie su prefijo.

- [ ] **Step 5: Compilar y ejecutar** — Expected: `ALL MODEL IMPORT TESTS PASSED`.

- [ ] **Step 6: Sabotaje** (uno a uno, revertir): (a) en `resolveModelTexture` devolver siempre `byName` → debe fallar `test_resolve_texture_prefers_the_subfolder`; (b) quitar `percentDecode` → debe fallar `test_companions_of_gltf`; (c) quitar el `if (!staysInside(rel)) return;` → debe fallar `test_companions_of_gltf`.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Renderer/ModelLoader.h engine/src/Renderer/ModelLoader.cpp engine/tests/model_import_tests.cpp
git commit -m "feat(renderer): lista unica de formatos de modelo, texturas en subcarpeta y ficheros asociados"
```

---

### Task 2: Importador glTF y el loader con la resolución nueva

**Files:**
- Modify: `CMakeLists.txt:111-112`
- Modify: `engine/src/Renderer/ModelLoader.cpp` (líneas ~218, ~473, `stampObjMaterialLibraries` ~607-636 y ~691)
- Modify: `engine/include/DonTopo/Editor/ThumbnailDiskCache.h`
- Test: `engine/tests/model_import_tests.cpp`, `engine/tests/thumbnail_tests.cpp`

**Interfaces:**
- Consumes: `resolveModelTexture`, `modelCompanionFiles` (Task 1); `stampFile` (`Core/FileStamp.h`).
- Produces: `ModelLoader::load/loadSkinned/loadAuto/loadPreview` aceptan `.gltf` y `.glb`.

- [ ] **Step 1: Tests que fallan**

En `model_import_tests.cpp`, antes de `int main()`:

```cpp
// ── glTF ─────────────────────────────────────────────────────────────────────

static std::string base64(const std::vector<uint8_t>& in)
{
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < in.size(); i += 3)
    {
        uint32_t n = static_cast<uint32_t>(in[i]) << 16;
        if (i + 1 < in.size()) n |= static_cast<uint32_t>(in[i + 1]) << 8;
        if (i + 2 < in.size()) n |= in[i + 2];
        out += T[(n >> 18) & 63];
        out += T[(n >> 12) & 63];
        out += i + 1 < in.size() ? T[(n >> 6) & 63] : '=';
        out += i + 2 < in.size() ? T[n & 63] : '=';
    }
    return out;
}

// Un triangulo: 3 posiciones (36 bytes) + 3 UV (24 bytes) = 60 bytes.
static std::vector<uint8_t> triangleBuffer()
{
    const float data[15] = { 0, 0, 0,  1, 0, 0,  0, 1, 0,   0, 0,  1, 0,  0, 1 };
    std::vector<uint8_t> b(sizeof(data));
    std::memcpy(b.data(), data, sizeof(data));
    return b;
}

// bufferUri vacio = sin "uri" (el buffer va en el chunk BIN de un .glb).
static std::string triangleGltfJson(const std::string& bufferUri, const std::string& imageUri)
{
    std::string j = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
                    R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1})";
    if (!imageUri.empty()) j += R"(,"material":0)";
    j += R"(}]}],)";
    if (!imageUri.empty())
        j += R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)"
             R"("textures":[{"source":0}],"images":[{"uri":")" + imageUri + R"("}],)";
    j += R"("buffers":[{)";
    if (!bufferUri.empty()) j += R"("uri":")" + bufferUri + R"(",)";
    j += R"("byteLength":60}],)"
         R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24}],)"
         R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
         R"({"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"}]})";
    return j;
}

static void writeGlb(const fs::path& p)
{
    std::string json = triangleGltfJson("", "");
    while (json.size() % 4) json += ' ';
    const std::vector<uint8_t> bin = triangleBuffer();          // 60: ya multiplo de 4
    std::ofstream f(p, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    u32(0x46546C67); u32(2); u32(static_cast<uint32_t>(12 + 8 + json.size() + 8 + bin.size()));
    u32(static_cast<uint32_t>(json.size())); u32(0x4E4F534A); f.write(json.data(), static_cast<std::streamsize>(json.size()));
    u32(static_cast<uint32_t>(bin.size()));  u32(0x004E4942); f.write(reinterpret_cast<const char*>(bin.data()), static_cast<std::streamsize>(bin.size()));
}

static bool hasUvX1(const Mesh& m)
{
    for (const Vertex& v : m.vertices) if (std::abs(v.uv.x - 1.0f) < 1e-4f) return true;
    return false;
}

static void test_gltf_with_embedded_buffer_loads()
{
    const fs::path dir = makeDir("dt_gltf_embedded");
    writeText(dir / "tri.gltf", triangleGltfJson("data:application/octet-stream;base64," + base64(triangleBuffer()), ""));
    const Mesh m = ModelLoader::load((dir / "tri.gltf").string());
    CHECK(m.indices.size() == 3);
    CHECK(hasUvX1(m));
}

// Buffer externo y textura en subcarpeta: la textura se resuelve a textures/.
static void test_gltf_with_external_bin_and_subfolder_texture()
{
    const fs::path dir = makeDir("dt_gltf_external");
    fs::create_directories(dir / "textures");
    const std::vector<uint8_t> bin = triangleBuffer();
    std::ofstream(dir / "tri.bin", std::ios::binary).write(reinterpret_cast<const char*>(bin.data()), static_cast<std::streamsize>(bin.size()));
    writeTga(dir / "textures" / "rojo.tga", 4, 4, [](int, int) { return Rgba{ 255, 0, 0, 255 }; });
    writeText(dir / "tri.gltf", triangleGltfJson("tri.bin", "textures/rojo.tga"));
    const Mesh m = ModelLoader::load((dir / "tri.gltf").string());
    CHECK(m.indices.size() == 3);
    CHECK(!m.material.texturePath.empty());
    if (!m.material.texturePath.empty())
        CHECK(sameAssetPath(m.material.texturePath, dir / "textures" / "rojo.tga"));

    const ModelPreview p = ModelLoader::loadPreview((dir / "tri.gltf").string());
    CHECK(p.status == PreviewStatus::Ok);
    CHECK(hasDependency(p, dir / "tri.bin"));
    CHECK(hasDependency(p, dir / "textures" / "rojo.tga"));
    CHECK(p.parts.size() == 1 && !p.parts[0].albedo.rgba.empty());
}

static void test_glb_loads()
{
    const fs::path dir = makeDir("dt_glb");
    writeGlb(dir / "tri.glb");
    const std::shared_ptr<Mesh> m = ModelLoader::loadAuto((dir / "tri.glb").string());
    CHECK(m && m->indices.size() == 3);
}

// Review Focus 1: sin su .bin, error limpio (no crash) y preview Unreadable.
static void test_gltf_missing_bin_fails_cleanly()
{
    const fs::path dir = makeDir("dt_gltf_missing_bin");
    writeText(dir / "tri.gltf", triangleGltfJson("no_esta.bin", ""));
    bool threw = false;
    try { (void)ModelLoader::load((dir / "tri.gltf").string()); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    const ModelPreview p = ModelLoader::loadPreview((dir / "tri.gltf").string());
    CHECK(p.status == PreviewStatus::Unreadable);
    CHECK(hasDependency(p, dir / "no_esta.bin"));      // vigilado: se regenera cuando aparezca
}
```

Y en `main()`:

```cpp
    test_gltf_with_embedded_buffer_loads();
    test_gltf_with_external_bin_and_subfolder_texture();
    test_glb_loads();
    test_gltf_missing_bin_fails_cleanly();
```

`test_gltf_missing_bin_fails_cleanly` pide que la dependencia se declare aunque Assimp falle: en `loadPreview` los asociados se sellan **antes** de `ReadFile` (Step 4).

En `thumbnail_tests.cpp`, antes de `int main()`:

```cpp
// Un .gltf declara su .bin: reexportar el buffer regenera la miniatura.
static void test_thumbnail_gltf_declares_its_bin(const fs::path& dir)
{
    std::ofstream(dir / "tri.bin", std::ios::binary) << std::string(60, '\0');
    std::ofstream(dir / "tri.gltf") << R"({"asset":{"version":"2.0"},"buffers":[{"uri":"tri.bin","byteLength":60}]})";
    const ThumbnailResult r = makeAssetThumbnail(dir / "tri.gltf");
    CHECK(hasDep(r, dir / "tri.bin"));
}
```

y `test_thumbnail_gltf_declares_its_bin(dir);` en `main()`.

- [ ] **Step 2: Compilar y ver que falla** — `dt_model_import_tests` y `dt_thumbnail_tests`. Expected: los tests de glTF fallan (Assimp no tiene importador: `load` lanza; el preview no declara el `.bin`).

- [ ] **Step 3: Habilitar el importador**

En `CMakeLists.txt`, tras `set(ASSIMP_BUILD_OBJ_IMPORTER        ON  CACHE BOOL "" FORCE)`:

```cmake
set(ASSIMP_BUILD_GLTF_IMPORTER       ON  CACHE BOOL "" FORCE)
```

Y cambiar el comentario de la línea 106 a `# Assimp — importación de modelos 3D (FBX, OBJ, glTF/GLB)`.

- [ ] **Step 4: Loader y preview con la resolución nueva**

En `ModelLoader.cpp`:
- Línea ~218 (`load`): `outPath = (modelDir / fs::path(raw).filename()).string();` → `outPath = resolveModelTexture(modelDir, raw).string();`
- Línea ~473 (`loadSkinned`): `else outPath = (modelDir / fs::path(raw).filename()).string();` → `else outPath = resolveModelTexture(modelDir, raw).string();`
- Línea ~691 (`loadPreview`): `const fs::path ext = modelDir / fs::path(texPath.C_Str()).filename();` → `const fs::path ext = resolveModelTexture(modelDir, texPath.C_Str());`
- Borrar la función `stampObjMaterialLibraries` entera (Python con `splitlines(keepends=True)`, comprobando la primera y la última línea del bloque) y sustituir su llamada en `loadPreview` por:

```cpp
            // Lo que el modelo lee ademas de si mismo (.mtl, .bin, texturas de un
            // .gltf), sellado ANTES de que Assimp lo lea.
            for (const std::string& rel : modelCompanionFiles(path))
                out.dependencies.push_back(stampFile(fs::path(path).parent_path() / fs::path(rel)));
```

Si tras compilar `test_gltf_with_external_bin_and_subfolder_texture` falla SOLO en `texturePath` vacío (riesgo 2 de la spec: el importador glTF2 no rellena `aiTextureType_DIFFUSE`), en los tres sitios que piden la difusa (`load`, `loadSkinned` y `albedoOf` de `loadPreview`) probar `aiTextureType_BASE_COLOR` cuando `aiTextureType_DIFFUSE` no dé nada, y anotarlo como Ruling.

En `ThumbnailDiskCache.h`:

```cpp
// 2: los .obj declaran sus .mtl como dependencia (las entradas de la 1 no los tienen).
// 3: los .gltf declaran sus .bin e imagenes (modelCompanionFiles).
inline constexpr uint32_t kThumbDiskVersion = 3;
```

- [ ] **Step 5: Compilar (en segundo plano: recompila Assimp) y ejecutar** los dos tests. Expected: `ALL MODEL IMPORT TESTS PASSED` y `ALL THUMBNAIL TESTS PASSED`.

- [ ] **Step 6: Sabotaje**: (a) volver a `modelDir / filename` en `load` → debe fallar `test_gltf_with_external_bin_and_subfolder_texture`; (b) quitar el bucle de asociados en `loadPreview` → deben fallar `test_gltf_missing_bin_fails_cleanly` y `test_thumbnail_gltf_declares_its_bin`.

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt engine/src/Renderer/ModelLoader.cpp engine/include/DonTopo/Editor/ThumbnailDiskCache.h engine/tests/model_import_tests.cpp engine/tests/thumbnail_tests.cpp
git commit -m "feat(renderer): importador glTF/GLB y texturas en subcarpeta"
```

---

### Task 3: El editor usa la lista única

**Files:**
- Modify: `engine/src/Editor/ContentBrowserPanel.cpp:236`, `engine/src/Editor/PropertiesPanel.cpp:388-394, 8018, 8023`, `engine/src/Editor/AnimatorPanel.cpp:1880-1886, 2087, 2103`, `engine/src/Editor/AssetImport.cpp:20-44`, `engine/src/Editor/Thumbnail.cpp` (`isModelThumbnailPath`)
- Test: `engine/tests/asset_import_tests.cpp`, `engine/tests/content_browser_tests.cpp`, `engine/tests/thumbnail_tests.cpp`

**Interfaces:**
- Consumes: `ModelLoader::isSupportedModelExtension`, `ModelLoader::supportedModelFilter` (Task 1).

- [ ] **Step 1: Tests que fallan**

`asset_import_tests.cpp`, dentro de `test_is_importable_extension`:

```cpp
    for (const char* e : { ".obj", ".gltf", ".GLB" })
        CHECK(isImportableExtension(e));
```

y en `test_imported_asset_dest_dir`:

```cpp
    for (const char* e : { ".obj", ".gltf", ".glb" })
        CHECK(importedAssetDestDir(root, e) == root / "assets" / "Imported" / "Meshes");
```

`content_browser_tests.cpp`, en `test_classify_asset`:

```cpp
    CHECK(classifyAsset(".obj",  false) == AssetKind::Model3D);
    CHECK(classifyAsset(".gltf", false) == AssetKind::Model3D);
    CHECK(classifyAsset(".dae",  false) == AssetKind::Other);    // no compilado: no se promete
```

`thumbnail_tests.cpp`, en `test_is_model_thumbnail_path`, sustituir `CHECK(!isModelThumbnailPath("x.glb"));` por:

```cpp
    CHECK(isModelThumbnailPath("x.glb"));
    CHECK(isModelThumbnailPath("x.GLTF"));
    CHECK(!isModelThumbnailPath("x.dae"));
```

- [ ] **Step 2: Compilar y ejecutar** `dt_asset_import_tests`, `dt_content_browser_tests`, `dt_thumbnail_tests`. Expected: fallan las comprobaciones de `.obj`/`.gltf`/`.glb` en importación y en miniaturas (`classifyAsset` ya las aceptaba: esas líneas fijan el comportamiento).

- [ ] **Step 3: Sustituir las listas**

- `ContentBrowserPanel.cpp:236`: `if (e == ".fbx" || e == ".obj" || e == ".gltf" || e == ".glb")            return AssetKind::Model3D;` → `if (ModelLoader::isSupportedModelExtension(e))                           return AssetKind::Model3D;` (añadir `#include "DonTopo/Renderer/ModelLoader.h"` si no está).
- `AssetImport.cpp`: quitar `".fbx",` de `kImportable` y, en `isImportableExtension`, `return ModelLoader::isSupportedModelExtension(ext) || kImportable.count(toLower(ext)) != 0;`. En `importedAssetDestDir`: `if (ModelLoader::isSupportedModelExtension(lower)) return imported / "Meshes";`. Incluir `ModelLoader.h`.
- `Thumbnail.cpp`, `isModelThumbnailPath`: `return ModelLoader::isSupportedModelExtension(path.extension().string());`.
- `PropertiesPanel.cpp:388-394`:

```cpp
    const std::string ext = std::filesystem::path(path).extension().string();
    if (!ModelLoader::isSupportedModelExtension(ext))
    {
        m_meshLoadError = "Formato no soportado: " + ext;
        return;
    }
```

  Línea 8018: `m_meshFileDialog->OpenDialog("AddMeshDlg", "Choose Model", ModelLoader::supportedModelFilter(), cfg);` y actualizar en el comentario de encima el ejemplo `"Choose FBX####AddMeshDlg"` → `"Choose Model####AddMeshDlg"`. Línea 8023: `ImGui::TextDisabled("Drop a model here");`.
- `AnimatorPanel.cpp:1880-1886`: lo mismo que Properties (`m_animSrcError`). Línea 2087: `m_animSrcDialog->OpenDialog("AddAnimSrcDlg", "Choose Animation Source", ModelLoader::supportedModelFilter(), cfg);`. Línea 2103: `ImGui::TextDisabled("(o arrastra un modelo aquí)");` y en el comentario de encima «drop target de .fbx» → «drop target de modelos».

Comprobar con `rg -n '"\.fbx"' engine/src` que no queda ninguna comparación de extensión de modelo fuera de `ModelLoader.cpp`.

- [ ] **Step 4: Compilar y ejecutar** los tres tests. Expected: los tres `ALL ... PASSED`.

- [ ] **Step 5: Commit**

```bash
git add engine/src/Editor/ContentBrowserPanel.cpp engine/src/Editor/PropertiesPanel.cpp engine/src/Editor/AnimatorPanel.cpp engine/src/Editor/AssetImport.cpp engine/src/Editor/Thumbnail.cpp engine/tests/asset_import_tests.cpp engine/tests/content_browser_tests.cpp engine/tests/thumbnail_tests.cpp
git commit -m "feat(editor): .obj, .gltf y .glb en Add Mesh, importacion, miniaturas y Animator"
```

---

### Task 4: El exportador empaqueta los ficheros asociados

**Files:**
- Modify: `engine/src/Editor/GameExporter.cpp:215-284` (`add`, nueva `addModel`)
- Test: `engine/tests/exporter_tests.cpp`

**Interfaces:**
- Consumes: `ModelLoader::modelCompanionFiles` (Task 1).

- [ ] **Step 1: Tests que fallan**

En `exporter_tests.cpp`, añadir `#include "DonTopo/Renderer/ModelLoader.h"` y, antes de `int main()`:

```cpp
static std::vector<std::string> packagePaths(const std::vector<ExportAsset>& assets)
{
    std::vector<std::string> pkg;
    for (const ExportAsset& a : assets) pkg.push_back(a.packagePath);
    return pkg;
}

static bool contains(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

static void writeGltfWithCompanions(const fs::path& dir)
{
    std::error_code ec;
    fs::create_directories(dir / "textures", ec);
    std::ofstream(dir / "tri.gltf") << R"({"asset":{"version":"2.0"},)"
        R"("buffers":[{"uri":"tri.bin","byteLength":60}],"images":[{"uri":"textures/rojo%20x.tga"}]})";
    std::ofstream(dir / "tri.bin") << "bin";
    std::ofstream(dir / "textures" / "rojo x.tga") << "tga";
}

// Dentro del proyecto: la jerarquia se conserva y el .bin y la textura viajan.
static void test_gltf_inside_the_project_travels_with_its_companions(const fs::path& root)
{
    const fs::path dir = root / "assets" / "gl";
    writeGltfWithCompanions(dir);
    Scene scene;
    scene.addGameObject("g")->setMesh(makeMesh(dir / "tri.gltf", dir / "textures" / "rojo x.tga"));
    const std::vector<std::string> pkg = packagePaths(collectSceneAssets(scene, root, {}));
    CHECK(contains(pkg, "assets/gl/tri.gltf"));
    CHECK(contains(pkg, "assets/gl/tri.bin"));
    CHECK(contains(pkg, "assets/gl/textures/rojo x.tga"));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Review Focus 4: fuera del proyecto, el .bin y la textura de la subcarpeta quedan
// en la MISMA assets/_external/N que el .gltf, con su subcarpeta.
static void test_gltf_outside_the_project_keeps_its_companions_together(const fs::path& root)
{
    std::error_code ec;
    const fs::path outside = fs::temp_directory_path(ec) / "dt_exporter_gltf_outside";
    fs::remove_all(outside, ec);
    writeGltfWithCompanions(outside);
    Scene scene;
    scene.addGameObject("g")->setMesh(makeMesh(outside / "tri.gltf", outside / "textures" / "rojo x.tga"));
    const std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    std::string gltfPkg;
    for (const ExportAsset& a : assets)
        if (exportPathKey(a.sourcePath) == exportPathKey((outside / "tri.gltf").string())) gltfPkg = a.packagePath;
    CHECK(!gltfPkg.empty());
    const std::string base = fs::path(gltfPkg).parent_path().generic_string();
    const std::vector<std::string> pkg = packagePaths(assets);
    CHECK(contains(pkg, base + "/tri.bin"));
    CHECK(contains(pkg, base + "/textures/rojo x.tga"));
    CHECK(assets.size() == 3);                                  // la textura del material no se duplica
    fs::remove_all(outside, ec);
}

static void test_obj_travels_with_its_mtl(const fs::path& root)
{
    const fs::path dir = root / "assets" / "o";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream(dir / "cube.obj") << "mtllib cube.mtl\nv 0 0 0\n";
    std::ofstream(dir / "cube.mtl") << "newmtl m\n";
    Scene scene;
    scene.addGameObject("o")->setMesh(makeMesh(dir / "cube.obj"));
    const std::vector<std::string> pkg = packagePaths(collectSceneAssets(scene, root, {}));
    CHECK(contains(pkg, "assets/o/cube.obj"));
    CHECK(contains(pkg, "assets/o/cube.mtl"));
    fs::remove_all(dir, ec);
}
```

En `main()`, tras `test_model_sidecar_travels_with_the_fbx_and_its_animation_sources(root);`:

```cpp
    test_gltf_inside_the_project_travels_with_its_companions(root);
    test_gltf_outside_the_project_keeps_its_companions_together(root);
    test_obj_travels_with_its_mtl(root);
```

- [ ] **Step 2: Compilar y ejecutar** `dt_exporter_tests` **desde `build-ninja/engine/tests`**. Expected: fallan las comprobaciones de `.bin`, `.mtl` y textura.

- [ ] **Step 3: Implementar en `collectSceneAssets`**

`add` gana un segundo parámetro opcional, la ruta de paquete forzada:

```cpp
    auto add = [&](const std::string& raw, const std::string& forcedPackagePath = {})
    {
        if (raw.empty()) return;
        const std::string key = exportPathKey(raw);
        if (key.empty() || seen.count(key)) return;

        std::error_code ec;
        fs::path abs = fs::weakly_canonical(fs::path(raw), ec);
        if (ec) abs = fs::path(raw);

        std::string packagePath = forcedPackagePath;
        if (!packagePath.empty())
        {
            // Asociado de un modelo: su sitio lo fija el modelo (ver addModel).
        }
        else if (keyUnderDir(key, rootKey))
```

(el resto del cuerpo igual, cambiando el `if` original por `else if`).

Tras `addWithSidecar`:

```cpp
    // Un modelo lleva su sidecar y los ficheros que lee ademas de si mismo (.mtl
    // de un .obj, .bin e imagenes de un .gltf), colocados RESPECTO a la carpeta
    // del modelo en el paquete: el runtime los busca en la misma ruta relativa, y
    // fuera del proyecto otra assets/_external/N rompería esa relacion. Se
    // añaden antes que las texturas del material para que la dedup conserve
    // esta colocacion.
    auto addModel = [&](const std::string& raw)
    {
        if (raw.empty()) return;
        addWithSidecar(raw);
        const auto it = seen.find(exportPathKey(raw));
        if (it == seen.end()) return;
        const fs::path modelPkgDir = fs::path(out[it->second].packagePath).parent_path();
        const fs::path modelDir    = fs::path(raw).parent_path();
        for (const std::string& rel : ModelLoader::modelCompanionFiles(raw))
            add((modelDir / fs::path(rel)).string(), (modelPkgDir / fs::path(rel)).generic_string());
    };
```

Y en el `traverse`: `addWithSidecar(go->getMesh()->sourcePath);` → `addModel(go->getMesh()->sourcePath);`, y `addWithSidecar(src.path);` → `addModel(src.path);`. Incluir `DonTopo/Renderer/ModelLoader.h`.

- [ ] **Step 4: Compilar y ejecutar** `dt_exporter_tests` desde `build-ninja/engine/tests`. Expected: `ALL EXPORTER TESTS PASSED` (o el mensaje final que imprima el fichero), incluidos los tests anteriores de externos y sidecars.

- [ ] **Step 5: Sabotaje**: (a) `addModel` sin `forcedPackagePath` (llamar a `add` con un solo argumento) → debe fallar `test_gltf_outside_the_project_keeps_its_companions_together`; (b) añadir los asociados DESPUÉS de las texturas del material (mover `addModel` a detrás del bucle de `materialsOf`) → debe fallar el mismo test.

- [ ] **Step 6: Commit**

```bash
git add engine/src/Editor/GameExporter.cpp engine/tests/exporter_tests.cpp
git commit -m "feat(export): los .obj y .gltf viajan con su .mtl, .bin y texturas en su ruta relativa"
```

---

### Task 5: Documentación, suite y verificación

**Files:**
- Modify: `README.md:67, 773`

- [ ] **Step 1: README**

Línea 67: `| Model loading | FBX / OBJ, embedded textures supported |` → `| Model loading | FBX, OBJ, glTF / GLB (no Draco), embedded textures and textures in subfolders |`.
Línea 773: `For models (`.fbx` and` → `For models (`.fbx`, `.obj`, `.gltf` and `.glb`) it offers` y ajustar la línea 774 (`other 3D formats) it offers a uniform scale,` → `a uniform scale,`) para que la frase siga leyéndose bien. `git diff --stat`.

- [ ] **Step 2: Suite completa** con el snippet de la suite (build completo + 35 tests + Sandbox), en segundo plano. Expected: `SUMMARY total 35, fallos:` vacío y `SANDBOX alive after 7s`. `dt_exporter_tests` se lanza desde su propia carpeta en ese snippet como en planes anteriores; si falla solo por el cwd, relanzarlo desde `build-ninja/engine/tests` antes de investigar.

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs: formatos de modelo glTF/GLB y OBJ en el README"
```

- [ ] **Step 4: Verificación manual (usuario)** — Release, Vulkan y D3D12:
1. Arrastrar desde el Explorador un `.glb` real con textura y un `.obj` con `.mtl`: se importan a `Imported/Meshes`, tienen miniatura, se añaden a un objeto (Add Mesh y soltando) y se ven con textura.
2. Import Settings (escala) en el `.glb`: se aplica.
3. Exportar esa escena: el juego carga los dos modelos con textura.
4. Un `.gltf` con animación como fuente del Animator de un personaje con el mismo esqueleto.
