# Modelos estáticos de varias piezas — Plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Un modelo estático de varias piezas añadido con Add Mesh aparece como hijos del objeto seleccionado, uno por pieza, con la transformación de su nodo y su material, sin tocar el render.

**Architecture:** `ModelLoader::loadStatic` lee el fichero una vez y devuelve todas sus mallas y las apariciones de cada una en los nodos (`ModelPiece`). `Mesh::piece` identifica la malla del fichero y la escena lo guarda. La caché de precarga pasa a indexar por `meshCacheKey(sourcePath, piece)`. El cargador asíncrono sirve cada pieza desde un solo ReadFile y entrega la lista de piezas. El editor crea los hijos con `insertModelPieces` y apila un `CompositeCommand` de `CreateGameObjectCommand` que llevan su propia caché de mallas. Reimport y miniaturas pasan a entender piezas.

**Tech Stack:** C++20, Assimp 5.3.1, glm, nlohmann_json. Tests planos `main()` + `CHECK`/`assert`, `dt_add_test`.

**Spec:** `docs/superpowers/specs/2026-09-26-static-model-pieces-design.md`

> **Revisión al escribir el plan:** la spec hablaba de descomponer la transformación en posición/rotación/escala con `glm::decompose`. La escena guarda `localTransform` como matriz 4×4 (`Scene.cpp:1134`, `mat4ToJson`), así que el hijo guarda la matriz de la pieza **tal cual**. No hay descomposición que pueda fallar: una matriz con algún valor no finito pasa a identidad con aviso. La prueba de miniatura por «más píxeles cubiertos» se sustituye por una comprobación directa de las partes del preview (`test_preview_draws_every_piece_in_place`): el encuadre al bbox hacía frágil comparar coberturas.

## Global Constraints

- **Escenas existentes intactas**: una malla sin `"piece"` es la pieza 0 = `scene->mMeshes[0]` sin transformación, idéntico a hoy. La pieza 0 usa como clave de caché el `sourcePath` a secas.
- **Un fichero de una sola aparición se comporta como hoy** (malla en el objeto seleccionado, sin transformación). Solo con **más de una** aparición se crean hijos.
- **Transformación de una pieza**: producto de los nodos desde los hijos de la raíz hasta el nodo, **sin** la raíz; la traslación × `ModelImportSettings::scale`.
- **Mallas sin ningún triángulo** no generan pieza.
- **Clave de caché**: `meshCacheKey(p, 0) == p`; `meshCacheKey(p, n) == p + "#piece=" + std::to_string(n)` para `n > 0`.
- **Sin cambios de render** en ningún backend.
- **`kThumbDiskVersion` = 4.**
- **CRLF**: tras cada `Edit`, `git diff --stat`. Nada de `sed -i` ni Get-Content/Set-Content. Para mover o borrar bloques, Python con `splitlines(keepends=True)`, y comparar a máquina el cuerpo movido contra el original (nota de memoria «Verificar un refactor de corta-pega»).
- **Tests desde la raíz del repo**, salvo `dt_exporter_tests` (desde `build-ninja/engine/tests`).
- **Build de un target**: `cmd /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul && chcp 65001 >nul && cmake --build build-ninja --target <target>'`.
- **GCC 13 en el CI de Linux**: includes explícitos.
- **Suite**: `total 35` antes y después (no hay ejecutables nuevos).

## Review Focus

1. **Borrar el padre de un modelo de varias piezas y deshacer**: cada hijo recupera **su** malla, no la de su hermano. → Task 2 (`testDeleteUndoKeepsEachPiece`).
2. **Reimportar un fichero reexportado con menos piezas**: el objeto cuya pieza ya no existe conserva su malla anterior y hay un aviso; los demás se recargan. → Task 5 (`test_reimport_missing_piece_keeps_the_old_mesh`).
3. **Deshacer y rehacer el Add Mesh de varias piezas no vuelve a leer el fichero** (sería síncrono en el hilo principal). → Task 4 (`test_insert_model_pieces_uses_the_given_meshes`, con un `sourcePath` que no existe en disco).
4. **Una escena de antes** (sin `"piece"`) carga igual, y una malla de pieza 0 no escribe el campo. → Task 2 (`testPieceRoundTripsThroughJson`, `testPieceCacheKeys`).
5. **Un fichero de una sola malla en un nodo transformado** sigue sin transformación y sin hijos. → Task 1 (`test_single_mesh_file_is_one_piece`) y la condición `pieces.size() > 1` de la Task 4.

---

## File Structure

| Fichero | Responsabilidad |
|---|---|
| `engine/include/DonTopo/Renderer/Mesh.h` (modif.) | `int piece = 0` |
| `engine/include/DonTopo/Renderer/ModelLoader.h`, `engine/src/Renderer/ModelLoader.cpp` (modif.) | `ModelPiece`, `StaticModel`, `loadStatic`, `load(path, piece)`, helpers `meshFromAssimp` y `collectPieces`; preview con todas las piezas |
| `engine/include/DonTopo/Core/Scene.h`, `engine/src/Core/Scene.cpp` (modif.) | `meshCacheKey`; `"piece"` en JSON; `collectMeshes` y `nodeFromJson` por pieza |
| `engine/include/DonTopo/Renderer/AsyncAssetLoader.h`, `engine/src/Renderer/AsyncAssetLoader.cpp` (modif.) | petición por pieza, `loadStatic` en el job, `LoadedMesh::piece/pieces/pieceMeshes` |
| `runtime/main.cpp` (modif.) | precarga por `(sourcePath, piece)` |
| `engine/include/DonTopo/Editor/Command.h`, `engine/src/Editor/Command.cpp` (modif.) | `CompositeCommand`, caché en `CreateGameObjectCommand`, `insertModelPieces` |
| `engine/src/Editor/EditorUI.cpp` (modif.) | Add Mesh de varias piezas en `onAssetsLoaded` |
| `engine/src/Editor/ModelReimport.cpp` (modif.) | recarga por pieza |
| `engine/include/DonTopo/Editor/ThumbnailDiskCache.h` (modif.) | versión 4 |
| `engine/tests/gltf_fixtures.h` (nuevo) | `.gltf` de tres piezas para varios tests |
| `engine/tests/model_import_tests.cpp`, `scene_async_tests.cpp`, `asset_loader_tests.cpp`, `render_settings_undo_tests.cpp`, `camera_tests.cpp`, `content_browser_tests.cpp` (modif.) | tests |
| `README.md`, `docs/superpowers/specs/2026-09-25-gltf-obj-models-design.md` (modif.) | quitar la limitación |

---

### Task 1: `ModelLoader::loadStatic` y `load(path, piece)`

**Files:**
- Modify: `engine/include/DonTopo/Renderer/Mesh.h`
- Modify: `engine/include/DonTopo/Renderer/ModelLoader.h`, `engine/src/Renderer/ModelLoader.cpp` (`load`, líneas ~140-235)
- Create: `engine/tests/gltf_fixtures.h`
- Test: `engine/tests/model_import_tests.cpp`

**Interfaces:**
- Produces:

```cpp
// Mesh.h, en struct Mesh, tras sourcePath:
        // Indice de la malla dentro del fichero (scene->mMeshes[piece]). 0 = la
        // primera, que es lo unico que se cargaba antes de las piezas.
        int                     piece = 0;

// ModelLoader.h, en namespace DonTopo, antes de class ModelLoader:
    struct ModelPiece
    {
        int         piece = 0;        // indice en scene->mMeshes
        std::string name;             // nombre del nodo (o de la malla si el nodo no tiene)
        glm::mat4   transform{1.0f};  // relativa a la raiz; traslacion x escala del sidecar
    };

    struct StaticModel
    {
        std::vector<Mesh>       meshes;   // una por scene->mMeshes; meshes[i].piece == i
        std::vector<ModelPiece> pieces;   // apariciones, en profundidad
    };

// dentro de class ModelLoader:
            static StaticModel loadStatic(const std::string& path);
            static Mesh load(const std::string& path, int piece);
```

- [ ] **Step 1: Fixture compartido**

Crear `engine/tests/gltf_fixtures.h`:

```cpp
#pragma once
// .gltf escritos a mano para los tests de piezas. En un namespace propio para
// no chocar con los helpers de cada fichero de test.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace dt_fixture {

inline std::string base64(const std::vector<uint8_t>& in)
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

// Dos mallas y tres nodos en la raiz de la escena:
//   A: malla 0 (triangulo en XY),  traslacion (5, 0, 0)
//   B: malla 1 (triangulo en YZ),  escala 2
//   C: malla 0 otra vez,           traslacion (0, 0, -3)
// Buffer: posiciones A (36 B) + UV (24 B) + posiciones B (36 B) = 96 bytes.
inline void writeThreePieceGltf(const std::filesystem::path& p)
{
    const float data[24] = { 0, 0, 0,  1, 0, 0,  0, 1, 0,     // posiciones A
                             0, 0,  1, 0,  0, 1,              // UV
                             0, 0, 0,  0, 1, 0,  0, 0, 1 };   // posiciones B
    std::vector<uint8_t> buf(sizeof(data));
    std::memcpy(buf.data(), data, sizeof(data));
    std::ofstream(p) << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1,2]}],)"
        R"("nodes":[{"name":"A","mesh":0,"translation":[5,0,0]},)"
                 R"({"name":"B","mesh":1,"scale":[2,2,2]},)"
                 R"({"name":"C","mesh":0,"translation":[0,0,-3]}],)"
        R"("meshes":[{"name":"triA","primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1}}]},)"
                  R"({"name":"triB","primitives":[{"attributes":{"POSITION":2,"TEXCOORD_0":1}}]}],)"
        R"("buffers":[{"uri":"data:application/octet-stream;base64,)" << base64(buf) << R"(","byteLength":96}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24},)"
                       R"({"buffer":0,"byteOffset":60,"byteLength":36}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
                     R"({"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},)"
                     R"({"bufferView":2,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[0,1,1]}]})";
}

} // namespace dt_fixture
```

- [ ] **Step 2: Tests que fallan**

En `model_import_tests.cpp` añadir `#include "gltf_fixtures.h"` y `#include <glm/gtc/matrix_transform.hpp>`, y antes de `int main()`:

```cpp
// ── Piezas de un modelo estatico ─────────────────────────────────────────────

static bool nearMat(const glm::mat4& a, const glm::mat4& b)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(a[c][r] - b[c][r]) > 1e-4f) return false;
    return true;
}

static bool hasVertex(const Mesh& m, const glm::vec3& p)
{
    for (const Vertex& v : m.vertices) if (glm::length(v.pos - p) < 1e-4f) return true;
    return false;
}

static void test_load_static_lists_every_piece()
{
    const fs::path dir = makeDir("dt_pieces_three");
    dt_fixture::writeThreePieceGltf(dir / "casa.gltf");
    try
    {
        const StaticModel m = ModelLoader::loadStatic((dir / "casa.gltf").string());
        CHECK(m.meshes.size() == 2);
        CHECK(m.pieces.size() == 3);
        if (m.meshes.size() == 2) { CHECK(m.meshes[0].piece == 0); CHECK(m.meshes[1].piece == 1); }
        if (m.pieces.size() != 3) return;
        CHECK(m.pieces[0].piece == 0 && m.pieces[0].name == "A");
        CHECK(m.pieces[1].piece == 1 && m.pieces[1].name == "B");
        CHECK(m.pieces[2].piece == 0 && m.pieces[2].name == "C");   // malla reutilizada
        CHECK(nearMat(m.pieces[0].transform, glm::translate(glm::mat4(1.0f), glm::vec3(5, 0, 0))));
        CHECK(nearMat(m.pieces[1].transform, glm::scale(glm::mat4(1.0f), glm::vec3(2.0f))));
        CHECK(nearMat(m.pieces[2].transform, glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, -3))));
    }
    catch (const std::exception& e) { std::printf("  %s\n", e.what()); CHECK(false); }
}

static void test_load_piece_gives_that_mesh()
{
    const fs::path dir = makeDir("dt_pieces_load");
    dt_fixture::writeThreePieceGltf(dir / "casa.gltf");
    try
    {
        const Mesh b = ModelLoader::load((dir / "casa.gltf").string(), 1);
        CHECK(b.piece == 1);
        CHECK(hasVertex(b, { 0, 0, 1 }) && !hasVertex(b, { 1, 0, 0 }));   // triangulo en YZ, sin transformar
        const Mesh a0 = ModelLoader::load((dir / "casa.gltf").string());
        const Mesh a1 = ModelLoader::load((dir / "casa.gltf").string(), 0);
        CHECK(a0.piece == 0 && a0.vertices.size() == a1.vertices.size() && hasVertex(a0, { 1, 0, 0 }));
    }
    catch (const std::exception& e) { std::printf("  %s\n", e.what()); CHECK(false); }

    bool threw = false;
    try { (void)ModelLoader::load((dir / "casa.gltf").string(), 7); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

// Review Focus 5: una sola malla = una sola pieza (y el editor no crea hijos).
static void test_single_mesh_file_is_one_piece()
{
    const fs::path obj = writeObj("dt_pieces_single", kFoldedObj);
    const StaticModel m = ModelLoader::loadStatic(obj.string());
    CHECK(m.meshes.size() == 1);
    CHECK(m.pieces.size() == 1);
}

// La escala del sidecar lleva las piezas juntas: tambien multiplica la traslacion.
static void test_sidecar_scale_moves_the_pieces_too()
{
    const fs::path dir = makeDir("dt_pieces_scale");
    const fs::path gltf = dir / "casa.gltf";
    dt_fixture::writeThreePieceGltf(gltf);
    ModelImportSettings s;
    s.scale = 2.0f;
    writeSettings(gltf, s);
    const StaticModel m = ModelLoader::loadStatic(gltf.string());
    CHECK(m.pieces.size() == 3);
    if (m.pieces.size() == 3) CHECK(std::abs(m.pieces[0].transform[3].x - 10.0f) < 1e-4f);
    if (!m.meshes.empty()) CHECK(hasVertex(m.meshes[0], { 2, 0, 0 }));
}
```

Y en `main()`:

```cpp
    test_load_static_lists_every_piece();
    test_load_piece_gives_that_mesh();
    test_single_mesh_file_is_one_piece();
    test_sidecar_scale_moves_the_pieces_too();
```

- [ ] **Step 3: Compilar y ver que falla** — `dt_model_import_tests`. Expected: `StaticModel`/`loadStatic` no declarados.

- [ ] **Step 4: Declarar** en `Mesh.h` y `ModelLoader.h` lo del bloque *Produces*, con estos comentarios encima de `loadStatic` y `load(path, piece)`:

```cpp
            // Un solo ReadFile: todas las mallas del fichero y donde aparece cada
            // una en los nodos. Para modelos SIN huesos (con huesos, loadSkinned).
            // La transformacion de cada pieza es relativa a la raiz (la de un FBX
            // lleva la conversion de unidades) y su traslacion va por la escala
            // del sidecar. Las mallas sin triangulos no son pieza. Lanza como load.
            static StaticModel loadStatic(const std::string& path);

            // La malla `piece` del fichero, sin transformacion. load(path) es
            // load(path, 0). Pieza fuera de rango: std::runtime_error.
            static Mesh load(const std::string& path, int piece);
```

- [ ] **Step 5: Implementar en `ModelLoader.cpp`**

1. **Extraer el cuerpo de `load`** a una función estática del fichero, **moviendo** (no reescribiendo) el código desde `Mesh mesh;` hasta justo antes de `return mesh;`:

```cpp
    // Una aiMesh a Mesh con su material. Es el cuerpo que tenia load(path),
    // movido tal cual; `ai` sustituye a scene->mMeshes[0].
    static Mesh meshFromAssimp(const aiScene* scene, const aiMesh* ai,
                               const ModelImportSettings& settings, const std::string& path)
    {
        // ... cuerpo movido, sin cambios salvo quitar `aiMesh* ai = scene->mMeshes[0];` ...
        return mesh;
    }
```

   Comprobar a máquina que el cuerpo movido es idéntico al original salvo esa línea (Python: comparar las líneas del bloque viejo con las del nuevo).

2. **Recorrido de piezas**:

```cpp
    static bool hasTriangles(const aiMesh* m)
    {
        for (uint32_t f = 0; f < m->mNumFaces; ++f)
            if (m->mFaces[f].mNumIndices == 3) return true;
        return false;
    }

    // Apariciones de cada malla en los nodos, en profundidad. La raiz NO aporta
    // su transformacion (en FBX lleva unidades y ejes); sus mallas, si tiene,
    // van con identidad.
    static std::vector<ModelPiece> collectPieces(const aiScene* scene, float scale)
    {
        std::vector<ModelPiece> out;
        std::function<void(const aiNode*, const glm::mat4&)> walk = [&](const aiNode* node, const glm::mat4& m)
        {
            for (uint32_t k = 0; k < node->mNumMeshes; ++k)
            {
                const uint32_t idx = node->mMeshes[k];
                if (idx >= scene->mNumMeshes || !hasTriangles(scene->mMeshes[idx])) continue;
                ModelPiece p;
                p.piece = static_cast<int>(idx);
                p.name  = node->mName.length > 0 ? node->mName.C_Str() : scene->mMeshes[idx]->mName.C_Str();
                p.transform = m;
                p.transform[3].x *= scale;
                p.transform[3].y *= scale;
                p.transform[3].z *= scale;
                out.push_back(std::move(p));
            }
            for (uint32_t c = 0; c < node->mNumChildren; ++c)
                walk(node->mChildren[c], m * aiToGlm(node->mChildren[c]->mTransformation));
        };
        if (!scene->mRootNode) return out;
        // La raiz se recorre con identidad; cada hijo entra con SU transformacion.
        walk(scene->mRootNode, glm::mat4(1.0f));
        return out;
    }
```

   Ojo: `walk` recibe la matriz **ya compuesta** del nodo que visita; la raíz entra con identidad y cada hijo con `m * transformación del hijo`.

3. **`load(path)`** pasa a `return load(path, 0);`. **`load(path, piece)`**:

```cpp
    Mesh ModelLoader::load(const std::string& path, int piece)
    {
        const ModelImportSettings settings = readModelSettings(path);
        Assimp::Importer importer;
        configureImporter(importer, settings);
        const aiScene* scene = importer.ReadFile(path, assimpFlags(settings));
        if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
            throw std::runtime_error("Assimp: " + std::string(importer.GetErrorString()));
        if (piece < 0 || static_cast<uint32_t>(piece) >= scene->mNumMeshes)
            throw std::runtime_error("'" + path + "' no tiene la pieza " + std::to_string(piece) +
                                     " (tiene " + std::to_string(scene->mNumMeshes) + ")");
        Mesh mesh = meshFromAssimp(scene, scene->mMeshes[piece], settings, path);
        mesh.piece = piece;
        return mesh;
    }

    StaticModel ModelLoader::loadStatic(const std::string& path)
    {
        const ModelImportSettings settings = readModelSettings(path);
        Assimp::Importer importer;
        configureImporter(importer, settings);
        const aiScene* scene = importer.ReadFile(path, assimpFlags(settings));
        if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
            throw std::runtime_error("Assimp: " + std::string(importer.GetErrorString()));
        StaticModel out;
        out.meshes.reserve(scene->mNumMeshes);
        for (uint32_t i = 0; i < scene->mNumMeshes; ++i)
        {
            out.meshes.push_back(meshFromAssimp(scene, scene->mMeshes[i], settings, path));
            out.meshes.back().piece = static_cast<int>(i);
        }
        out.pieces = collectPieces(scene, settings.scale);
        return out;
    }
```

- [ ] **Step 6: Compilar y ejecutar** — Expected: `ALL MODEL IMPORT TESTS PASSED` (incluidos todos los tests anteriores de `load`, que no deben cambiar).

- [ ] **Step 7: Sabotaje** (uno a uno): (a) en `collectPieces` componer también la raíz (`walk(root, aiToGlm(root->mTransformation))`) no debe romper el fixture (la raíz de glTF es identidad): anotar que el riesgo de la raíz de FBX solo lo cubre la verificación manual; (b) quitar la multiplicación de la traslación por `scale` → debe fallar `test_sidecar_scale_moves_the_pieces_too`; (c) quitar la comprobación de rango en `load` → `test_load_piece_gives_that_mesh` debe fallar o reventar.

- [ ] **Step 8: Commit**

```bash
git add engine/include/DonTopo/Renderer/Mesh.h engine/include/DonTopo/Renderer/ModelLoader.h engine/src/Renderer/ModelLoader.cpp engine/tests/gltf_fixtures.h engine/tests/model_import_tests.cpp
git commit -m "feat(renderer): ModelLoader::loadStatic, todas las piezas de un modelo estatico y donde aparece cada una"
```

---

### Task 2: La escena guarda la pieza y la caché de precarga indexa por pieza

**Files:**
- Modify: `engine/include/DonTopo/Core/Scene.h` (junto a `PreloadedMeshCache`, línea ~34)
- Modify: `engine/src/Core/Scene.cpp` (serialización ~1146, `nodeFromJson` ~2070-2275, `collectMeshes` ~3828)
- Test: `engine/tests/scene_async_tests.cpp`

**Interfaces:**
- Consumes: `Mesh::piece`, `ModelLoader::load(path, piece)` (Task 1).
- Produces (Scene.h, `namespace DonTopo`):

```cpp
// Clave de PreloadedMeshCache: el sourcePath para la pieza 0 (las caches de
// antes siguen valiendo) y "<sourcePath>#piece=<n>" para las demas. Sin la pieza
// en la clave, el undo de Delete de un modelo de varias piezas daria la misma
// malla a todos sus hijos.
std::string meshCacheKey(const std::string& sourcePath, int piece);
```

- [ ] **Step 1: Tests que fallan** — en `scene_async_tests.cpp`, antes de `int main()`:

```cpp
void testPieceCacheKeys()
{
    CHECK(DonTopo::meshCacheKey("a/b.fbx", 0) == "a/b.fbx", "la pieza 0 conserva la clave de siempre");
    CHECK(DonTopo::meshCacheKey("a/b.fbx", 2) == "a/b.fbx#piece=2", "las demas llevan la pieza");
}

std::shared_ptr<DonTopo::Mesh> fakePiece(const std::string& src, int piece, const char* name)
{
    auto m = std::make_shared<DonTopo::Mesh>();
    m->sourcePath = src;
    m->piece      = piece;
    m->name       = name;
    DonTopo::Vertex v{};
    v.pos = glm::vec3(static_cast<float>(piece), 0.0f, 0.0f);
    m->vertices.push_back(v);
    return m;
}

// Review Focus 4: "piece" va y vuelve; la pieza 0 no escribe el campo; un nodo
// con "piece" se sirve de la entrada de SU pieza en la cache.
void testPieceRoundTripsThroughJson()
{
    const std::string src = "assets/__no_existe__.gltf";
    DonTopo::Scene scene;
    auto* p0 = scene.addGameObject("p0");
    auto* p1 = scene.addGameObject("p1");
    p0->setMesh(fakePiece(src, 0, "malla0"));
    p1->setMesh(fakePiece(src, 1, "malla1"));
    const nlohmann::json j = scene.toJson();
    const auto& kids = j["root"]["children"];
    CHECK(kids.size() == 2, "dos hijos serializados");
    if (kids.size() != 2) return;
    CHECK(!kids[0]["mesh"].contains("piece"), "la pieza 0 no escribe el campo");
    CHECK(kids[1]["mesh"].value("piece", -1) == 1, "la pieza 1 se guarda");

    DonTopo::PreloadedMeshCache cache;
    cache[DonTopo::meshCacheKey(src, 0)] = fakePiece(src, 0, "cache0");
    cache[DonTopo::meshCacheKey(src, 1)] = fakePiece(src, 1, "cache1");
    DonTopo::Scene back;
    CHECK(back.fromJson(j, physics(), audio(), nullptr, &cache), "recarga con cache");
    DonTopo::GameObject* b0 = nullptr;
    DonTopo::GameObject* b1 = nullptr;
    back.traverse([&](DonTopo::GameObject* go) { if (go->name == "p0") b0 = go; if (go->name == "p1") b1 = go; });
    CHECK(b0 && b0->hasMesh() && b0->getMesh()->name == "cache0", "p0 recibe la pieza 0");
    CHECK(b1 && b1->hasMesh() && b1->getMesh()->name == "cache1" && b1->getMesh()->piece == 1,
          "p1 recibe la pieza 1 y la conserva");
}

// Review Focus 1: lo que hace el undo de Delete (collectMeshes + subtreeToJson +
// insertFromJson con esa cache) devuelve a cada hijo SU malla.
void testDeleteUndoKeepsEachPiece()
{
    const std::string src = "assets/__no_existe__.gltf";
    DonTopo::Scene scene;
    auto* casa = scene.addGameObject("Casa");
    auto* paredes = scene.addGameObject("Paredes", casa);
    auto* tejado  = scene.addGameObject("Tejado", casa);
    paredes->setMesh(fakePiece(src, 0, "paredes"));
    tejado->setMesh(fakePiece(src, 1, "tejado"));

    const DonTopo::PreloadedMeshCache cache = DonTopo::Scene::collectMeshes(casa);
    const nlohmann::json snap = scene.subtreeToJson(casa);
    scene.removeGameObject(casa);
    DonTopo::GameObject* back = scene.insertFromJson(snap, nullptr, 0, physics(), audio(), &cache);
    CHECK(back && back->children.size() == 2, "Casa vuelve con sus dos hijos");
    if (!back || back->children.size() != 2) return;
    for (const auto& child : back->children)
    {
        CHECK(child->hasMesh(), "cada hijo recupera malla");
        if (!child->hasMesh()) continue;
        const std::string esperado = child->name == "Paredes" ? "paredes" : "tejado";
        CHECK(child->getMesh()->name == esperado, "cada hijo recupera SU malla, no la de su hermano");
    }
}
```

Y en `main()`, tras `testPreloadedCacheConsulted();`:

```cpp
    testPieceCacheKeys();
    testPieceRoundTripsThroughJson();
    testDeleteUndoKeepsEachPiece();
```

(Si `children` no es un vector de `unique_ptr`/punteros con `->`, ajustar el acceso al tipo real de `GameObject::children`; comprobarlo en `GameObject.h` antes de compilar.)

- [ ] **Step 2: Compilar y ver que falla** — `dt_scene_async_tests`. Expected: `meshCacheKey` no declarado.

- [ ] **Step 3: Implementar**

- `Scene.h`: declarar `meshCacheKey` (bloque *Produces*). `Scene.cpp`, en `namespace DonTopo`:

```cpp
    std::string meshCacheKey(const std::string& sourcePath, int piece)
    {
        return piece == 0 ? sourcePath : sourcePath + "#piece=" + std::to_string(piece);
    }
```

- Serialización (~1146), tras construir `meshJson`: `if (mesh->piece != 0) meshJson["piece"] = mesh->piece;`
- `nodeFromJson` (~2070), junto a `sourcePath`:

```cpp
            // Que pieza del fichero (Mesh::piece). Ausente o invalida = 0, que es
            // lo que cargaban las escenas de antes.
            int piece = 0;
            if (const auto it = j["mesh"].find("piece"); it != j["mesh"].end() && it->is_number_integer())
                piece = std::max(0, it->get<int>());
```

  En la rama estática (~2250-2273): `preloaded->find(sourcePath)` → `preloaded->find(meshCacheKey(sourcePath, piece))`; `loader->requestMesh(sourcePath, node->id)` → `loader->requestMesh(sourcePath, node->id, piece)` (la firma nueva llega en la Task 3: hasta entonces, dejar esta línea con dos argumentos y cambiarla en la Task 3); `ModelLoader::load(sourcePath)` → `ModelLoader::load(sourcePath, piece)`. La rama skinned (~2156) no cambia (un personaje siempre es la pieza 0, cuya clave es el `sourcePath`).
- `collectMeshes` (~3836): `out[ruta] = n->getMesh();` → `out[meshCacheKey(ruta, n->getMesh()->piece)] = n->getMesh();`

- [ ] **Step 4: Compilar y ejecutar** — Expected: `scene_async_tests OK`.

- [ ] **Step 5: Sabotaje**: (a) `collectMeshes` con la clave `ruta` a secas → debe fallar `testDeleteUndoKeepsEachPiece`; (b) no escribir `"piece"` → debe fallar `testPieceRoundTripsThroughJson`.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Core/Scene.h engine/src/Core/Scene.cpp engine/tests/scene_async_tests.cpp
git commit -m "feat(core): la escena guarda la pieza de cada malla y la cache de precarga indexa por pieza"
```

---

### Task 3: Cargador asíncrono por pieza y precarga del runtime

**Files:**
- Modify: `engine/include/DonTopo/Renderer/AsyncAssetLoader.h` (`LoadedMesh` ~35, `requestMesh` ~74, `PendingGroup` ~108, `buildResultFor` ~115)
- Modify: `engine/src/Renderer/AsyncAssetLoader.cpp` (`requestMesh` ~66, `buildResultFor` ~161, `runJob` ~186)
- Modify: `engine/src/Core/Scene.cpp` (la llamada a `requestMesh` de la Task 2)
- Modify: `runtime/main.cpp:~299-350`
- Test: `engine/tests/asset_loader_tests.cpp`

**Interfaces:**
- Consumes: `loadStatic`, `ModelPiece`, `StaticModel` (Task 1); `meshCacheKey` (Task 2).
- Produces:

```cpp
// LoadedMesh, nuevos campos:
        int                                       piece = 0;
        std::vector<ModelPiece>                   pieces;        // apariciones, si es estatico con > 1
        std::vector<std::shared_ptr<const Mesh>>  pieceMeshes;   // todas las mallas del fichero, si pieces no esta vacio
// AsyncAssetLoader:
            JobSystem::JobId requestMesh(const std::string& path, uint64_t targetId, int piece = 0);
```

- [ ] **Step 1: Test que falla** — en `asset_loader_tests.cpp` añadir `#include "gltf_fixtures.h"` y:

```cpp
// Dos piezas del mismo fichero: UN ReadFile, cada objeto su pieza, y la lista de
// apariciones viaja con el resultado.
void testPiecesShareOneReadFile()
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "dt_loader_pieces";
    std::filesystem::create_directories(dir);
    const std::string gltf = (dir / "casa.gltf").string();
    dt_fixture::writeThreePieceGltf(gltf);

    DonTopo::JobSystem js;
    js.start();
    DonTopo::AsyncAssetLoader loader(js);
    loader.requestMesh(gltf, 10, 0);
    loader.requestMesh(gltf, 11, 1);
    std::vector<DonTopo::LoadedMesh> got = drain(loader, 2);
    assert(got.size() == 2);
    assert(loader.readFileCount() == 1 && "dos piezas del mismo fichero = un solo ReadFile");
    for (const auto& r : got)
    {
        assert(r.error.empty());
        assert(r.mesh != nullptr);
        const int esperada = r.targetId == 11 ? 1 : 0;
        assert(r.piece == esperada && r.mesh->piece == esperada);
        assert(r.pieces.size() == 3);
        assert(r.pieceMeshes.size() == 2);
    }
    js.shutdown();
}
```

y `testPiecesShareOneReadFile();` en `main()` **antes** del `if (fbx.empty())` (no depende del FBX del repo).

- [ ] **Step 2: Compilar y ver que falla** — `dt_asset_loader_tests`. Expected: `requestMesh` no acepta 3 argumentos.

- [ ] **Step 3: Implementar**

- `PendingGroup::waiters` pasa a `std::vector<Waiter>` con `struct Waiter { JobSystem::JobId job; uint64_t targetId; int piece; };` (declarado junto a `PendingGroup`). Actualizar `requestMesh` (`group.waiters.push_back({ id, targetId, piece })`), `cancel` (`w->job == id`) y cualquier uso de `.first/.second`.
- `runJob(path)`:

```cpp
        LoadedMesh loaded;
        loaded.path = path;
        std::shared_ptr<StaticModel> model;     // solo si es estatico
        try
        {
            if (ModelLoader::hasBones(path))
            {
                loaded.mesh = ModelLoader::loadAuto(path);   // personaje: entero, como siempre
                if (!loaded.mesh) loaded.error = "No se pudo cargar el modelo: " + path;
            }
            else
            {
                model = std::make_shared<StaticModel>(ModelLoader::loadStatic(path));
                if (model->meshes.empty()) loaded.error = "'" + path + "' no tiene mallas";
            }
        }
        // (los dos catch de siempre, que ademas ponen model = nullptr)
```

  Los waiters se sirven con `buildResultFor(loaded, model.get(), waiter)`.
- `buildResultFor(const LoadedMesh& src, const StaticModel* model, const Waiter& w)`:

```cpp
        LoadedMesh out;
        out.job = w.job; out.targetId = w.targetId; out.path = src.path; out.error = src.error;
        out.piece = w.piece;
        if (model)
        {
            if (w.piece < 0 || static_cast<size_t>(w.piece) >= model->meshes.size())
            {
                out.error = "'" + src.path + "' no tiene la pieza " + std::to_string(w.piece);
                return out;
            }
            out.mesh = std::make_shared<Mesh>(model->meshes[w.piece]);
            const Material& mat = out.mesh->material;
            decodeSlot(mat.texturePath,           mat.embeddedTexture,           DecodedImage::Albedo, out.images);
            decodeSlot(mat.normalMapPath,         mat.embeddedNormalMap,         DecodedImage::Normal, out.images);
            decodeSlot(mat.metallicRoughnessPath, mat.embeddedMetallicRoughness, DecodedImage::ORM,    out.images);
            if (model->pieces.size() > 1)
            {
                out.pieces = model->pieces;
                for (const Mesh& m : model->meshes) out.pieceMeshes.push_back(std::make_shared<const Mesh>(m));
            }
            return out;
        }
        // personaje: lo de siempre (copia profunda de src.mesh; images vacias)
```

  La decodificación de texturas sale de `runJob` al `buildResultFor` para que cada waiter decodifique **su** pieza (antes se decodificaba `Mesh::material` una vez y se copiaba).
- `Scene.cpp`: `loader->requestMesh(sourcePath, node->id)` → `loader->requestMesh(sourcePath, node->id, piece)`.
- `runtime/main.cpp`: `uniquePaths` pasa a `std::set<std::pair<std::string, int>>` recogiendo `(sourcePath, piece)` (leer `"piece"` como en `nodeFromJson`, entero ≥ 0, por defecto 0); `requestMesh(p.first, reqId++, p.second)`; al drenar, `preloaded[DonTopo::meshCacheKey(r.path, r.piece)] = r.mesh;`. Añadir `#include <set>` y `#include <utility>`.

- [ ] **Step 4: Compilar y ejecutar** `dt_asset_loader_tests` y `dt_scene_async_tests`. Expected: `asset_loader_tests OK` y `scene_async_tests OK`. Compilar también el target `Runtime` (o como se llame el ejecutable de `runtime/`; buscarlo con `rg -n "add_executable" runtime/CMakeLists.txt`).

- [ ] **Step 5: Sabotaje**: servir `model->meshes[0]` a todos los waiters → debe fallar el assert de `r.mesh->piece == esperada`.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Renderer/AsyncAssetLoader.h engine/src/Renderer/AsyncAssetLoader.cpp engine/src/Core/Scene.cpp runtime/main.cpp engine/tests/asset_loader_tests.cpp
git commit -m "feat(renderer): el cargador asincrono sirve cada pieza desde un solo ReadFile"
```

---

### Task 4: Add Mesh de varias piezas en el editor

**Files:**
- Modify: `engine/include/DonTopo/Editor/Command.h`, `engine/src/Editor/Command.cpp` (`CreateGameObjectCommand` ~222/86, `duplicateAsSibling` ~40)
- Modify: `engine/src/Editor/EditorUI.cpp:1102-1130` (`onAssetsLoaded`)
- Test: `engine/tests/render_settings_undo_tests.cpp`, `engine/tests/camera_tests.cpp`

**Interfaces:**
- Consumes: `ModelPiece` (Task 1); `meshCacheKey`, `PreloadedMeshCache` (Task 2); `LoadedMesh::pieces/pieceMeshes` (Task 3).
- Produces (Command.h):

```cpp
// Varios comandos como UN paso de undo: execute en orden, undo en orden inverso.
class CompositeCommand : public ICommand {
public:
    explicit CompositeCommand(std::string label) : m_label(std::move(label)) {}
    void add(std::unique_ptr<ICommand> cmd) { m_cmds.push_back(std::move(cmd)); }
    bool empty() const { return m_cmds.empty(); }
    void execute() override { for (auto& c : m_cmds) c->execute(); }
    void undo() override { for (auto it = m_cmds.rbegin(); it != m_cmds.rend(); ++it) (*it)->undo(); }
    std::string label() const override { return m_label; }
private:
    std::string m_label;
    std::vector<std::unique_ptr<ICommand>> m_cmds;
};

// CreateGameObjectCommand gana, como ultimo parametro:
//     PreloadedMeshCache preloaded = {}
// y execute() lo pasa a insertFromJson: con las mallas ya en RAM, rehacer no lee disco.

// Crea un hijo de `parent` por aparicion de `pieces`, al final de sus hijos, con
// el nombre y la transformacion de la pieza y la malla meshes[piece] (sin leer
// disco). Una transformacion con algun valor no finito pasa a identidad con un
// aviso en `warnings`. Devuelve los hijos creados, en orden; indices de render a
// -1 (el llamante registra). Es el seam que se prueba sin GPU.
std::vector<GameObject*> insertModelPieces(Scene& scene, GameObject* parent, const std::string& sourcePath,
                                           const std::vector<ModelPiece>& pieces,
                                           const std::vector<std::shared_ptr<const Mesh>>& meshes,
                                           PhysicsManager& physics, AudioManager& audio,
                                           std::vector<std::string>* warnings);
```

- [ ] **Step 1: Tests que fallan**

`render_settings_undo_tests.cpp`, antes de `main()`:

```cpp
// CompositeCommand: un solo paso; execute en orden, undo al reves.
struct RecordingCommand : public ICommand
{
    std::vector<std::string>& log;
    std::string               name;
    RecordingCommand(std::vector<std::string>& l, std::string n) : log(l), name(std::move(n)) {}
    void execute() override { log.push_back("do " + name); }
    void undo() override { log.push_back("undo " + name); }
    std::string label() const override { return name; }
};

static void test_composite_command_order()
{
    std::vector<std::string> log;
    CompositeCommand group("grupo");
    group.add(std::make_unique<RecordingCommand>(log, "a"));
    group.add(std::make_unique<RecordingCommand>(log, "b"));
    group.execute();
    group.undo();
    CHECK(log == std::vector<std::string>({ "do a", "do b", "undo b", "undo a" }));
    CHECK(group.label() == "grupo");
}
```

(ajustar `CHECK` al macro del fichero y llamar `test_composite_command_order();` en `main()`).

`camera_tests.cpp`, junto a los tests de `duplicateAsSibling`:

```cpp
// Review Focus 3: los hijos se crean con las mallas dadas, sin leer disco (el
// sourcePath no existe), cada uno con SU pieza, nombre y transformacion.
static void test_insert_model_pieces_uses_the_given_meshes(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* casa = scene.addGameObject("Casa");
    const std::string src = "assets/__no_existe__.gltf";
    auto m0 = std::make_shared<Mesh>(); m0->sourcePath = src; m0->piece = 0; m0->name = "triA";
    auto m1 = std::make_shared<Mesh>(); m1->sourcePath = src; m1->piece = 1; m1->name = "triB";
    std::vector<ModelPiece> pieces(3);
    pieces[0] = { 0, "A", glm::translate(glm::mat4(1.0f), glm::vec3(5, 0, 0)) };
    pieces[1] = { 1, "B", glm::scale(glm::mat4(1.0f), glm::vec3(2.0f)) };
    pieces[2] = { 0, "C", glm::mat4(std::numeric_limits<float>::quiet_NaN()) };   // no finita
    std::vector<std::string> warnings;
    const std::vector<GameObject*> kids =
        insertModelPieces(scene, casa, src, pieces, { m0, m1 }, pm, am, &warnings);
    CHECK(kids.size() == 3);
    CHECK(casa->children.size() == 3);
    CHECK(!casa->hasMesh());
    if (kids.size() != 3) return;
    CHECK(kids[0]->name == "A" && kids[0]->hasMesh() && kids[0]->getMesh()->name == "triA");
    CHECK(kids[1]->name == "B" && kids[1]->hasMesh() && kids[1]->getMesh()->piece == 1);
    CHECK(kids[0]->localTransform[3].x == 5.0f);
    CHECK(kids[2]->localTransform == glm::mat4(1.0f));   // la no finita pasa a identidad
    CHECK(warnings.size() == 1);
}
```

(llamarlo en `main()` con los `PhysicsManager`/`AudioManager` que ya usan los tests de duplicado; añadir `#include <limits>` y `#include <glm/gtc/matrix_transform.hpp>` si faltan).

- [ ] **Step 2: Compilar y ver que falla** — `dt_render_settings_undo_tests` y `dt_camera_tests`. Expected: `CompositeCommand`/`insertModelPieces` no declarados.

- [ ] **Step 3: Implementar**

- `Command.h`: `CompositeCommand` (bloque *Produces*, header-only), el parámetro `PreloadedMeshCache preloaded = {}` en `CreateGameObjectCommand` (miembro `PreloadedMeshCache m_preloaded;`) y la declaración de `insertModelPieces`. Incluir `DonTopo/Renderer/ModelLoader.h` para `ModelPiece`.
- `Command.cpp`, `CreateGameObjectCommand::execute`: `m_scene.insertFromJson(m_snapshot, parent, m_index, m_physics, m_audio, m_preloaded.empty() ? nullptr : &m_preloaded);`
- `insertModelPieces`:

```cpp
std::vector<GameObject*> insertModelPieces(Scene& scene, GameObject* parent, const std::string& sourcePath,
                                           const std::vector<ModelPiece>& pieces,
                                           const std::vector<std::shared_ptr<const Mesh>>& meshes,
                                           PhysicsManager& physics, AudioManager& audio,
                                           std::vector<std::string>* warnings)
{
    std::vector<GameObject*> out;
    if (!parent) return out;
    for (const ModelPiece& p : pieces)
    {
        if (p.piece < 0 || static_cast<size_t>(p.piece) >= meshes.size() || !meshes[p.piece]) continue;
        glm::mat4 local = p.transform;
        const float* v = glm::value_ptr(local);
        if (!std::all_of(v, v + 16, [](float f) { return std::isfinite(f); }))
        {
            local = glm::mat4(1.0f);
            if (warnings) warnings->push_back("La pieza '" + p.name + "' traia una transformacion invalida: se usa la identidad");
        }
        nlohmann::json localJson = nlohmann::json::array();
        for (int i = 0; i < 16; ++i) localJson.push_back(glm::value_ptr(local)[i]);
        const nlohmann::json j = {
            { "name", p.name.empty() ? std::string("Pieza ") + std::to_string(p.piece) : p.name },
            { "localTransform", localJson },
            { "mesh", { { "sourcePath", sourcePath }, { "name", meshes[p.piece]->name }, { "skinned", false },
                        { "visible", true }, { "piece", p.piece } } },
            { "children", nlohmann::json::array() } };
        PreloadedMeshCache cache;
        cache[meshCacheKey(sourcePath, p.piece)] = meshes[p.piece];
        if (GameObject* node = scene.insertFromJson(j, parent, parent->children.size(), physics, audio, &cache))
            out.push_back(node);
    }
    return out;
}
```

  (Incluir `<algorithm>`, `<cmath>`, `<glm/gtc/type_ptr.hpp>`.)
- `EditorUI.cpp`, `onAssetsLoaded`: mover la llamada a `consumeUserMeshJob` al principio del cuerpo del bucle y, antes de `applyLoadedMesh`:

```cpp
        const bool delUsuario = m_propertiesPanel.consumeUserMeshJob(r.targetId, r.job);
        // Varias piezas en un modelo estatico: el seleccionado pasa a ser el padre
        // y cada pieza un hijo. Un solo paso de undo, con las mallas ya cargadas en
        // cada comando para que rehacer no lea el fichero.
        if (delUsuario && r.error.empty() && r.pieces.size() > 1)
        {
            if (GameObject* parent = scene.findById(r.targetId))
            {
                parent->pendingMeshJob = 0;
                std::vector<std::string> warnings;
                const std::vector<GameObject*> kids = insertModelPieces(
                    scene, parent, r.path, r.pieces, r.pieceMeshes, *m_physics, *m_audio, &warnings);
                for (const std::string& w : warnings) m_logPanel.push(w);
                auto group = std::make_unique<CompositeCommand>(
                    "Añadir modelo '" + std::filesystem::path(r.path).stem().string() + "' a '" + parent->name + "'");
                for (GameObject* kid : kids)
                {
                    renderer.registerGameObject(kid);
                    PreloadedMeshCache cache;
                    cache[meshCacheKey(r.path, kid->getMesh()->piece)] = kid->getMesh();
                    const size_t index = static_cast<size_t>(
                        std::find_if(parent->children.begin(), parent->children.end(),
                                     [&](const auto& c) { return &*c == kid; }) - parent->children.begin());
                    group->add(std::make_unique<CreateGameObjectCommand>(
                        scene, *m_physics, *m_audio, renderer, group->label(), parent->id, index,
                        scene.subtreeToJson(kid), std::move(cache)));
                }
                renderer.flushUploadsAndWait();
                if (!group->empty()) m_undoHistory.push(std::move(group));   // sin execute: ya estan creados
                m_propertiesPanel.invalidateCaches();
            }
            continue;
        }
```

  y quitar el `consumeUserMeshJob` de más abajo (queda `if (ok && delUsuario)`). Ajustar `&*c == kid` al tipo real de `children` (si es `std::vector<std::unique_ptr<GameObject>>`, `c.get() == kid`). Si `m_physics`/`m_audio` no son accesibles aquí, usar los mismos miembros que usa el duplicado (`EditorUI.cpp:1222`).

- [ ] **Step 4: Compilar** `dt_render_settings_undo_tests`, `dt_camera_tests` y `Sandbox`, y ejecutar los dos tests. Expected: los dos pasan; `Sandbox` compila.

- [ ] **Step 5: Sabotaje**: (a) `CompositeCommand::undo` en orden directo → debe fallar `test_composite_command_order`; (b) en `insertModelPieces` no pasar la caché a `insertFromJson` → debe fallar `test_insert_model_pieces_uses_the_given_meshes` (sin caché, el `sourcePath` inexistente deja los hijos sin malla).

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Editor/Command.h engine/src/Editor/Command.cpp engine/src/Editor/EditorUI.cpp engine/tests/render_settings_undo_tests.cpp engine/tests/camera_tests.cpp
git commit -m "feat(editor): Add Mesh de un modelo de varias piezas crea un hijo por pieza en un solo paso de undo"
```

---

### Task 5: Reimport por pieza y miniatura con todas las piezas

**Files:**
- Modify: `engine/src/Editor/ModelReimport.cpp:41-56` (carga por grupo)
- Modify: `engine/src/Renderer/ModelLoader.cpp` (`loadPreview`)
- Modify: `engine/include/DonTopo/Editor/ThumbnailDiskCache.h`
- Test: `engine/tests/content_browser_tests.cpp`, `engine/tests/model_import_tests.cpp`

**Interfaces:**
- Consumes: `loadStatic`, `collectPieces` (Task 1).

- [ ] **Step 1: Tests que fallan**

`content_browser_tests.cpp` (añadir `#include "gltf_fixtures.h"` y `#include "DonTopo/Editor/ModelReimport.h"` si faltan), junto a los tests de reimport existentes:

```cpp
// Review Focus 2: reimport por pieza. Cada objeto recarga SU pieza; el de una
// pieza que ya no existe conserva su malla y hay aviso.
static void test_reimport_missing_piece_keeps_the_old_mesh()
{
    const fs::path dir = fs::temp_directory_path() / "dt_reimport_pieces";
    fs::create_directories(dir);
    const fs::path gltf = dir / "casa.gltf";
    dt_fixture::writeThreePieceGltf(gltf);

    Scene scene("Test");
    GameObject* a = scene.addGameObject("A");
    GameObject* b = scene.addGameObject("B");
    GameObject* x = scene.addGameObject("X");
    auto old = std::make_shared<Mesh>(); old->sourcePath = gltf.string(); old->name = "vieja";
    auto pa = std::make_shared<Mesh>(*old); pa->piece = 0;
    auto pb = std::make_shared<Mesh>(*old); pb->piece = 1;
    auto px = std::make_shared<Mesh>(*old); px->piece = 9;      // no existe en el fichero
    a->setMesh(pa); b->setMesh(pb); x->setMesh(px);

    const ModelReimportResult r = reimportModelUsers(scene.getRoot(), gltf, nullptr);
    CHECK(a->getMesh()->piece == 0 && !a->getMesh()->vertices.empty());
    CHECK(b->getMesh()->piece == 1 && !b->getMesh()->vertices.empty());
    CHECK(x->getMesh()->name == "vieja");                          // intacto
    CHECK(!r.warnings.empty());
}
```

(usar el accesor real de la raíz de `Scene`; ver cómo lo hacen los tests de reimport de ese fichero).

`model_import_tests.cpp`:

```cpp
// La miniatura pinta TODAS las piezas en su sitio: el preview trae una parte por
// aparicion, con la transformacion aplicada.
static void test_preview_draws_every_piece_in_place()
{
    const fs::path dir = makeDir("dt_pieces_preview");
    dt_fixture::writeThreePieceGltf(dir / "casa.gltf");
    const ModelPreview p = ModelLoader::loadPreview((dir / "casa.gltf").string());
    CHECK(p.status == PreviewStatus::Ok);
    CHECK(p.parts.size() == 3);
    if (p.parts.size() != 3) return;
    auto hasPos = [](const PreviewPart& part, const glm::vec3& q) {
        for (const glm::vec3& v : part.positions) if (glm::length(v - q) < 1e-4f) return true;
        return false;
    };
    CHECK(hasPos(p.parts[0], { 6, 0, 0 }));    // (1,0,0) de A trasladado 5 en X
    CHECK(hasPos(p.parts[1], { 0, 2, 0 }));    // (0,1,0) de B escalado 2
    CHECK(hasPos(p.parts[2], { 1, 0, -3 }));   // (1,0,0) de C trasladado -3 en Z
}
```

y los dos en su `main()`.

- [ ] **Step 2: Compilar y ver que falla** — `dt_content_browser_tests` y `dt_model_import_tests`. Expected: el reimport deja las piezas 1/9 con la geometría de la 0 (o falla el aviso), y el preview trae 1 parte.

- [ ] **Step 3: Implementar**

- `ModelReimport.cpp`, en el bucle por grupo, antes del `loadAuto`:

```cpp
        // Estatico: una lectura con todas las piezas y cada objeto recarga la
        // SUYA. La que ya no existe deja el objeto como estaba, con aviso (mismo
        // contrato que una recarga fallida).
        if (!ModelLoader::hasBones(sourcePath))
        {
            StaticModel model;
            try { model = ModelLoader::loadStatic(sourcePath); }
            catch (const std::exception& e)
            {
                r.warnings.push_back("Reimport de '" + fbx.filename().string() + "' fallido: " + e.what() +
                                     " (los objetos se quedan como estaban)");
                r.skipped += static_cast<int>(users.size());
                continue;
            }
            std::map<int, std::shared_ptr<const Mesh>> byPiece;
            for (GameObject* go : users)
            {
                if (go->pendingMeshJob != 0)
                {
                    r.warnings.push_back("Reimport: '" + go->name + "' tiene una carga en curso, se salta");
                    ++r.skipped;
                    continue;
                }
                const int piece = go->getMesh()->piece;
                if (piece < 0 || static_cast<size_t>(piece) >= model.meshes.size())
                {
                    r.warnings.push_back("Reimport: '" + go->name + "' usaba la pieza " + std::to_string(piece) +
                                         ", que ya no esta en el fichero: se queda como estaba");
                    ++r.skipped;
                    continue;
                }
                auto& next = byPiece[piece];
                if (!next) next = std::make_shared<const Mesh>(model.meshes[piece]);
                if (renderer) renderer->removeMeshComponent(go);
                else          go->setMesh(nullptr);
                go->setMesh(next);
                applyMaterialOverrides(*go);
                // ...el mismo registro en GPU y el mismo contador de recargados que
                // el camino de siempre (copiar las lineas que siguen a
                // applyMaterialOverrides en el bucle existente)...
            }
            continue;
        }
```

  Leer el final del bucle existente (tras `applyMaterialOverrides(*go);`) y replicar **exactamente** sus líneas de registro en GPU y de contador dentro de este bloque; si son más de 3 líneas, extraerlas a una lambda local `finishReload(GameObject*)` usada por los dos caminos (Ruling si se extrae).
- `ModelLoader.cpp`, `loadPreview`: en la rama sin huesos, sustituir `meshCount = 1` por el recorrido de `collectPieces(scene, settings.scale)`:
  - con **más de una** aparición, una `PreviewPart` por aparición, con posiciones × `transform` y normales × `transpose(inverse(mat3(transform)))` normalizadas (la textura se sigue cacheando por material con `albedoOf`);
  - con **una o ninguna**, lo de hoy (malla 0 sin transformar), para que la miniatura coincida con lo que pinta el motor.
- `ThumbnailDiskCache.h`: comentario `// 4: el preview de un modelo estatico pinta todas sus piezas.` y `kThumbDiskVersion = 4`.

- [ ] **Step 4: Compilar y ejecutar** los dos tests. Expected: los dos `ALL ... PASSED`, incluidos los tests anteriores de preview (`test_preview_matches_the_engine_triangle_count` sigue pasando porque sus modelos tienen una aparición o son skinned).

- [ ] **Step 5: Sabotaje**: (a) en el reimport estático usar siempre `model.meshes[0]` → debe fallar `test_reimport_missing_piece_keeps_the_old_mesh`; (b) en el preview no aplicar `transform` → debe fallar `test_preview_draws_every_piece_in_place`.

- [ ] **Step 6: Commit**

```bash
git add engine/src/Editor/ModelReimport.cpp engine/src/Renderer/ModelLoader.cpp engine/include/DonTopo/Editor/ThumbnailDiskCache.h engine/tests/content_browser_tests.cpp engine/tests/model_import_tests.cpp
git commit -m "feat(editor): reimport por pieza y miniatura con todas las piezas del modelo"
```

---

### Task 6: Documentación, suite y verificación

- [ ] **Step 1: Docs**
  - `docs/superpowers/specs/2026-09-25-gltf-obj-models-design.md`: en «Limitaciones conocidas», tachar la primera viñeta con una nota «Resuelto el 2026-09-26: `2026-09-26-static-model-pieces-design.md`».
  - `README.md`, junto a la línea de Import Settings de modelos (~773): añadir una frase «A static model made of several pieces (a glTF house with walls, roof and windows) is added as one child per piece under the selected object, each with its node transform and its own material, in a single undo step.»
  - `git diff --stat`.
- [ ] **Step 2: Suite completa** (script de la suite en el scratchpad, en segundo plano). Expected: `SUMMARY total 35, fallos:` vacío y `SANDBOX alive after 7s`. Si el enlace de `Sandbox.exe` da `LNK1201`, hay un editor abierto: comprobarlo con `Get-Process Sandbox` y relanzar cuando se cierre (no matarlo).
- [ ] **Step 3: Commit**

```bash
git add README.md docs/superpowers/specs/2026-09-25-gltf-obj-models-design.md
git commit -m "docs: modelos estaticos de varias piezas en el README y la limitacion resuelta"
```

- [ ] **Step 4: Verificación manual (usuario)** — Release, Vulkan y D3D12:
  1. Un `.glb` real de varias piezas y materiales con Add Mesh: se ve completo y cada hijo con su textura.
  2. Deshacer y rehacer el Add Mesh (sin tirones).
  3. Borrar el padre y deshacer: cada pieza con su malla.
  4. Guardar, cerrar y recargar la escena.
  5. Import Settings con otra escala en ese `.glb`: las piezas siguen juntas.
  6. Exportar y abrir el juego.
  7. **Un FBX de varias piezas**: las piezas no salen ×100 ni giradas (riesgo 1 de la spec).
