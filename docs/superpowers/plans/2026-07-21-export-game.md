# Export Game Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Exportar desde el editor una carpeta autocontenida y jugable con la escena actual: runtime pre-compilado, `game.scene` y solo los assets referenciados.

**Architecture:** Un target CMake nuevo `DonTopoRuntime` reusa `DonTopoEngine` con `Renderer` en modo *headless* (sin ImGui, blit offscreen→swapchain). Un módulo `GameExporter` de funciones libres y puras recolecta los assets referenciados por la escena, reescribe los paths del JSON a relativos y copia el paquete. El editor lo invoca desde `File > Export Game...`.

**Tech Stack:** C++20, Vulkan, CMake + Ninja, nlohmann::json, ImGuiFileDialog (IGFD), sol2/Lua, PhysX (estático), FMOD.

**Spec:** `docs/superpowers/specs/2026-07-21-export-game-design.md`

## Global Constraints

- Build: `.\configure.bat` y `.\build.bat` desde **PowerShell** (vcvarsall + Ninja). Nunca `cmake` crudo desde Bash.
- `configure.bat` solo hace falta tras cambiar un `CMakeLists.txt`; `build.bat` basta para el resto.
- Los tests son ejecutables sueltos con `main()` + macro `CHECK`, sin framework y **sin `ctest`** (no hay `enable_testing()` en el repo). Se ejecutan a mano: `.\build-ninja\engine\tests\<nombre>.exe`.
- Éxito de un test = imprime `OK` y devuelve exit code 0.
- No crear ficheros fuera de `engine/src/Editor/`, `engine/include/DonTopo/Editor/`, `runtime/`, `engine/tests/`, `docs/`.
- No mover ni borrar ningún fichero existente.
- No tocar `shaders/`, el pipeline SPIR-V ni el layout de `assets/`.
- No modificar la lógica de render, física, animación ni scripting. `sandbox/src/main.cpp` no se toca en ninguna tarea.
- Los `.spv` no están trackeados en git: nunca commitearlos.
- Comentarios y mensajes de log en español, como el resto del repo.
- Si tras editar un header aparece un crash con puntero basura: build stale de Ninja. Borrar los `.obj` afectados y reconstruir antes de tocar el código nuevo.

---

## File Structure

| Fichero | Responsabilidad |
|---|---|
| `engine/include/DonTopo/Editor/GameExporter.h` (nuevo) | API pública del exportador: `ExportAsset`, `ExportResult`, `exportPathKey`, `collectSceneAssets`, `rewriteScenePaths`, `writeExportPackage` |
| `engine/src/Editor/GameExporter.cpp` (nuevo) | Implementación de las cuatro funciones |
| `engine/tests/exporter_tests.cpp` (nuevo) | Tests headless de las tres funciones testables |
| `runtime/CMakeLists.txt` (nuevo) | Target `DonTopoRuntime` + POST_BUILD que lo copia junto al editor |
| `runtime/main.cpp` (nuevo) | Loop del juego sin editor |
| `engine/CMakeLists.txt` | Añade `src/Editor/GameExporter.cpp` |
| `engine/tests/CMakeLists.txt` | Añade el target `dt_exporter_tests` |
| `CMakeLists.txt` (raíz) | `add_subdirectory(runtime)` tras `add_subdirectory(sandbox)` |
| `engine/include/DonTopo/Renderer/Renderer.h` | `setHeadless`, `m_headless`, `isPlaying()` |
| `engine/src/Renderer/Renderer.cpp` | Guardas de ImGui + blit headless |
| `engine/include/DonTopo/Editor/EditorUI.h` | Estado del diálogo de export |
| `engine/src/Editor/EditorUI.cpp` | Menú `File`, diálogos y llamada al exportador |

Orden de dependencias: Tareas 1-3 (lógica pura) son independientes del resto. Tarea 4 (headless) habilita la Tarea 5 (runtime). Tarea 6 (UI) consume 1-3 y necesita que 5 exista para encontrar el `.exe`.

---

## Task 1: `exportPathKey` + `collectSceneAssets`

**Files:**
- Create: `engine/include/DonTopo/Editor/GameExporter.h`
- Create: `engine/src/Editor/GameExporter.cpp`
- Create: `engine/tests/exporter_tests.cpp`
- Modify: `engine/CMakeLists.txt:44` (tras `src/Editor/ScriptEditorPanel.cpp`)
- Modify: `engine/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Scene::traverse`, `GameObject::{hasMesh,getMesh,getSkinnedMesh,hasAudioClip,getAudioClip,hasScripts,getScripts}`, `Mesh::{sourcePath,material}`, `Material::{texturePath,normalMapPath,metallicRoughnessPath}`, `SkinnedMesh::{materials,animationSources}`, `AudioClipComponent::getPath()`, `ScriptComponent::scriptName`.
- Produces:
  - `struct ExportAsset { std::string sourcePath; std::string packagePath; bool existsOnDisk; }`
  - `std::string exportPathKey(const std::string& path)`
  - `std::vector<ExportAsset> collectSceneAssets(Scene&, const std::filesystem::path& projectRoot, const std::map<std::string, std::filesystem::path>& scriptPaths)`
  - `struct ExportResult { bool ok; int fileCount; std::uintmax_t totalBytes; std::vector<std::string> messages; }`

- [ ] **Step 1: Crear el header con la API completa**

Crear `engine/include/DonTopo/Editor/GameExporter.h`:

```cpp
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>

namespace DonTopo {

class Scene;

// Un asset que el paquete exportado debe contener.
struct ExportAsset {
    // Ruta absoluta en disco, canonicalizada. Origen de la copia.
    std::string sourcePath;
    // Ruta relativa a la raíz del paquete, con '/' como separador:
    // "assets/model.fbx". Destino de la copia y valor que acaba en el
    // .scene exportado.
    std::string packagePath;
    // false si sourcePath no existe en disco: dispara el aborto del export
    // antes de copiar nada.
    bool        existsOnDisk = false;
};

// Resultado de escribir el paquete, para volcar al Log Console.
struct ExportResult {
    bool                     ok         = false;
    int                      fileCount  = 0;
    std::uintmax_t           totalBytes = 0;
    std::vector<std::string> messages;
};

// Clave canónica de un path para comparar y deduplicar: weakly_canonical +
// minúsculas + '/' como separador. Windows es case-insensitive pero
// std::filesystem::path::operator== no lo es, y los paths llegan mezclados
// (absolutos de IGFD, relativos de un .scene editado a mano). Mismo criterio
// que samePath() en ContentBrowserPanel.cpp:25, expuesto aquí porque
// rewriteScenePaths y sus tests necesitan generar exactamente la misma clave.
std::string exportPathKey(const std::string& path);

// Todos los assets que la escena referencia: meshes de origen, fuentes de
// animación, texturas de material, audio clips y los .lua de los
// ScriptComponent. Deduplicado y ordenado por packagePath.
//
// scriptPaths mapea ScriptComponent::scriptName -> fichero .lua (lo construye
// el llamador desde ScriptManager::getRegistry()). Un nombre ausente se
// ignora en silencio: el .lua llega igual al paquete porque Scripts/ se copia
// entera.
//
// NO incluye el skybox, los shaders, Scripts/ ni el ejecutable: eso lo añade
// writeExportPackage. Esta función responde solo a "qué referencia la escena".
std::vector<ExportAsset> collectSceneAssets(
    Scene& scene,
    const std::filesystem::path& projectRoot,
    const std::map<std::string, std::filesystem::path>& scriptPaths);

// Reescribe in-place mesh.sourcePath, mesh.animationSources[].path y
// audioClip.path de todo el árbol a su packagePath. sourceToPackage va
// keyeado por exportPathKey(sourcePath). Devuelve cuántos paths se
// reescribieron. Las texturas no aparecen aquí porque el .scene no las
// serializa: ModelLoader las deriva como dirname(fbx)/filename.
int rewriteScenePaths(nlohmann::json& sceneJson,
                      const std::map<std::string, std::string>& sourceToPackage);

// Crea <destDir>/<gameName>/ (borrando su contenido si ya existía: la
// confirmación es responsabilidad de la UI), copia el runtime, los assets,
// el skybox, shaders/*.spv, Scripts/ y fmod.dll, y escribe game.scene.
ExportResult writeExportPackage(const std::vector<ExportAsset>& assets,
                                const nlohmann::json& rewrittenScene,
                                const std::filesystem::path& destDir,
                                const std::string& gameName,
                                const std::filesystem::path& projectRoot,
                                const std::filesystem::path& scriptsDir,
                                const std::filesystem::path& runtimeExe);

} // namespace DonTopo
```

- [ ] **Step 2: Escribir el test que falla**

Crear `engine/tests/exporter_tests.cpp`:

```cpp
// Test headless del exportador de juego (sin GUI, sin GPU, sin PhysX, sin
// Lua). Plain main + asserts, mismo patrón que content_browser_tests.cpp.
//
// Los Mesh se construyen a mano en vez de vía ModelLoader: el test no
// necesita geometría real, solo los campos de path. Y no se crea ningún
// collider, así que no se instancia PhysicsManager — el motor solo admite una
// PxFoundation por proceso.
#include "DonTopo/Editor/GameExporter.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Scripting/ScriptComponent.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <system_error>
#include <vector>

using namespace DonTopo;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Crea un proyecto de mentira en disco con los ficheros que la escena de
// prueba referenciará. Devuelve la raíz canonicalizada.
static fs::path makeProjectFixture()
{
    std::error_code ec;
    fs::path root = fs::temp_directory_path(ec) / "dt_exporter_test";
    fs::remove_all(root, ec);
    fs::create_directories(root / "assets" / "chars", ec);
    fs::create_directories(root / "Scripts", ec);
    std::ofstream(root / "assets" / "hero.fbx")            << "fbx";
    std::ofstream(root / "assets" / "hero_diffuse.png")    << "png";
    std::ofstream(root / "assets" / "chars" / "enemy.fbx") << "fbx";
    std::ofstream(root / "assets" / "step.wav")            << "wav";
    std::ofstream(root / "Scripts" / "Player.lua")         << "-- lua";
    fs::path canon = fs::canonical(root, ec);
    return ec ? root : canon;
}

// Mesh estático con path de origen y, opcionalmente, textura difusa.
static std::shared_ptr<Mesh> makeMesh(const fs::path& source, const fs::path& diffuse = {})
{
    auto m = std::make_shared<Mesh>();
    m->sourcePath = source.string();
    if (!diffuse.empty())
        m->material.texturePath = diffuse.string();
    return m;
}

// Criterio de aceptación 2: 2 meshes + 1 textura + 1 script + 1 audio -> 5
// paths exactos, ni uno más.
static void test_collects_exactly_referenced(const fs::path& root)
{
    Scene scene;

    auto* hero = scene.addGameObject("hero");
    hero->setMesh(makeMesh(root / "assets" / "hero.fbx", root / "assets" / "hero_diffuse.png"));
    hero->addScript(std::make_unique<ScriptComponent>("Player", hero));

    auto* enemy = scene.addGameObject("enemy");
    enemy->setMesh(makeMesh(root / "assets" / "chars" / "enemy.fbx"));
    enemy->setAudioClip(std::make_shared<AudioClipComponent>(
        nullptr, (root / "assets" / "step.wav").string(), -1, false, false));

    std::map<std::string, fs::path> scriptPaths{ { "Player", root / "Scripts" / "Player.lua" } };
    std::vector<ExportAsset> assets = collectSceneAssets(scene, root, scriptPaths);

    CHECK(assets.size() == 5);
    for (const ExportAsset& a : assets)
        CHECK(a.existsOnDisk);

    // Ordenado por packagePath: el orden es determinista y comprobable.
    std::vector<std::string> pkg;
    for (const ExportAsset& a : assets) pkg.push_back(a.packagePath);
    CHECK(std::find(pkg.begin(), pkg.end(), "assets/hero.fbx")         != pkg.end());
    CHECK(std::find(pkg.begin(), pkg.end(), "assets/hero_diffuse.png") != pkg.end());
    CHECK(std::find(pkg.begin(), pkg.end(), "assets/chars/enemy.fbx")  != pkg.end());
    CHECK(std::find(pkg.begin(), pkg.end(), "assets/step.wav")         != pkg.end());
    CHECK(std::find(pkg.begin(), pkg.end(), "Scripts/Player.lua")      != pkg.end());
}

// Mesh procedural (Cube/Sphere/Plane/Capsule): sourcePath vacío, geometría ya
// serializada en el .scene. No aporta ningún asset.
static void test_procedural_mesh_contributes_nothing(const fs::path& root)
{
    Scene scene;
    auto* cube = scene.addGameObject("cube");
    cube->setMesh(std::make_shared<Mesh>()); // sourcePath vacío

    CHECK(collectSceneAssets(scene, root, {}).empty());
}

// Dos GameObjects con el mismo FBX -> una sola entrada.
static void test_deduplicates_shared_mesh(const fs::path& root)
{
    Scene scene;
    scene.addGameObject("a")->setMesh(makeMesh(root / "assets" / "hero.fbx"));
    scene.addGameObject("b")->setMesh(makeMesh(root / "assets" / "hero.fbx"));

    CHECK(collectSceneAssets(scene, root, {}).size() == 1);
}

// Las animationSources extra se recolectan; la fuente builtin comparte valor
// con sourcePath y no debe duplicarlo.
static void test_animation_sources(const fs::path& root)
{
    std::error_code ec;
    std::ofstream(root / "assets" / "run.fbx") << "fbx";

    Scene scene;
    auto skinned = std::make_shared<SkinnedMesh>();
    skinned->sourcePath = (root / "assets" / "hero.fbx").string();
    skinned->animationSources.push_back({ (root / "assets" / "hero.fbx").string(), true,  {} });
    skinned->animationSources.push_back({ (root / "assets" / "run.fbx").string(),  false, {} });
    scene.addGameObject("hero")->setMesh(skinned);

    std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    CHECK(assets.size() == 2);
}

// Asset fuera de la raíz del proyecto -> assets/_external/, con sufijo ante
// colisión de nombres.
static void test_external_assets(const fs::path& root)
{
    std::error_code ec;
    fs::path outside = fs::temp_directory_path(ec) / "dt_exporter_outside";
    fs::remove_all(outside, ec);
    fs::create_directories(outside / "a", ec);
    fs::create_directories(outside / "b", ec);
    std::ofstream(outside / "a" / "prop.fbx") << "fbx";
    std::ofstream(outside / "b" / "prop.fbx") << "fbx";

    Scene scene;
    scene.addGameObject("a")->setMesh(makeMesh(outside / "a" / "prop.fbx"));
    scene.addGameObject("b")->setMesh(makeMesh(outside / "b" / "prop.fbx"));

    std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    CHECK(assets.size() == 2);
    CHECK(assets[0].packagePath != assets[1].packagePath);
    for (const ExportAsset& a : assets)
        CHECK(a.packagePath.rfind("assets/_external/", 0) == 0);

    fs::remove_all(outside, ec);
}

// Un asset referenciado que no está en disco se marca, no se filtra: el
// llamador necesita listarlos en el error.
static void test_missing_asset_flagged(const fs::path& root)
{
    Scene scene;
    scene.addGameObject("ghost")->setMesh(makeMesh(root / "assets" / "no_existe.fbx"));

    std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    CHECK(assets.size() == 1);
    CHECK(!assets[0].existsOnDisk);
}

int main()
{
    fs::path root = makeProjectFixture();

    test_collects_exactly_referenced(root);
    test_procedural_mesh_contributes_nothing(root);
    test_deduplicates_shared_mesh(root);
    test_animation_sources(root);
    test_external_assets(root);
    test_missing_asset_flagged(root);

    std::error_code ec;
    fs::remove_all(root, ec);

    if (g_failures) { std::printf("%d FAILURES\n", g_failures); return 1; }
    std::printf("OK\n");
    return 0;
}
```

- [ ] **Step 3: Registrar los targets en CMake**

En `engine/CMakeLists.txt`, tras la línea `src/Editor/ScriptEditorPanel.cpp` (línea 44):

```cmake
    src/Editor/ScriptEditorPanel.cpp
    src/Editor/GameExporter.cpp
```

En `engine/tests/CMakeLists.txt`, tras el bloque de `dt_animator_tests`:

```cmake
add_executable(dt_exporter_tests exporter_tests.cpp)
target_link_libraries(dt_exporter_tests PRIVATE DonTopoEngine)
target_compile_features(dt_exporter_tests PRIVATE cxx_std_20)
```

Y añadir el target a la lista del `foreach` de copia de `fmod.dll` en ese mismo fichero:

```cmake
        foreach(_dt_test_target dt_physics_tests dt_content_browser_tests dt_camera_tests dt_animator_tests dt_exporter_tests)
```

- [ ] **Step 4: Verificar que el test falla**

Desde PowerShell:

```
.\configure.bat
.\build.bat
```

Esperado: FALLO de compilación en `exporter_tests.cpp` con `unresolved external symbol` para `collectSceneAssets` (el header existe, el .cpp aún no), o error de que `engine/src/Editor/GameExporter.cpp` no existe. Ambos son el fallo esperado.

- [ ] **Step 5: Implementar `exportPathKey` y `collectSceneAssets`**

Crear `engine/src/Editor/GameExporter.cpp`:

```cpp
#include "DonTopo/Editor/GameExporter.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Scripting/ScriptComponent.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace fs = std::filesystem;

namespace {

// true si p está dentro de dir (ambos ya canonicalizados y en minúsculas).
bool keyUnderDir(const std::string& p, const std::string& dir)
{
    if (dir.empty() || p.size() <= dir.size()) return false;
    if (p.compare(0, dir.size(), dir) != 0)    return false;
    return p[dir.size()] == '/';
}

// Todos los materiales de un GameObject, sea la malla estática o skinned.
// loadSkinned nunca puebla el Material heredado de Mesh: reparte uno por
// submalla en SkinnedMesh::materials. Mismo criterio que materialsOf() en
// ContentBrowserPanel.cpp — mirar solo `material` dejaría fuera las texturas
// de cualquier personaje con rig.
std::vector<const DonTopo::Material*> materialsOf(const DonTopo::GameObject* go)
{
    std::vector<const DonTopo::Material*> out;
    if (!go->hasMesh()) return out;
    if (const DonTopo::SkinnedMesh* sm = go->getSkinnedMesh())
        for (const DonTopo::Material& m : sm->materials)
            out.push_back(&m);
    else
        out.push_back(&go->getMesh()->material);
    return out;
}

} // namespace

namespace DonTopo {

std::string exportPathKey(const std::string& path)
{
    if (path.empty()) return {};
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(fs::path(path), ec);
    std::string s = (ec ? fs::path(path) : canon).generic_string();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::vector<ExportAsset> collectSceneAssets(
    Scene& scene,
    const fs::path& projectRoot,
    const std::map<std::string, fs::path>& scriptPaths)
{
    std::vector<ExportAsset> out;
    std::map<std::string, size_t> seen;                 // key -> índice en out
    std::map<std::string, std::string> externalTaken;   // packagePath -> key dueño
    const std::string rootKey = exportPathKey(projectRoot.string());

    auto add = [&](const std::string& raw)
    {
        if (raw.empty()) return;
        const std::string key = exportPathKey(raw);
        if (key.empty() || seen.count(key)) return;

        std::error_code ec;
        fs::path abs = fs::weakly_canonical(fs::path(raw), ec);
        if (ec) abs = fs::path(raw);

        std::string packagePath;
        if (keyUnderDir(key, rootKey))
        {
            // Dentro del proyecto: se conserva la jerarquía tal cual. Es lo
            // que hace que las texturas se reencuentren solas en el runtime:
            // ModelLoader las deriva como dirname(fbx)/filename.
            packagePath = fs::relative(abs, projectRoot, ec).generic_string();
            if (ec || packagePath.empty()) packagePath = "assets/_external/" + abs.filename().string();
        }
        else
        {
            packagePath = "assets/_external/" + abs.filename().string();
        }

        // Colisión de nombres entre dos assets externos distintos: sufijo.
        if (auto it = externalTaken.find(packagePath); it != externalTaken.end() && it->second != key)
        {
            const fs::path p = abs;
            for (int n = 1; ; ++n)
            {
                std::string candidate = "assets/_external/" + p.stem().string() + "_" +
                                        std::to_string(n) + p.extension().string();
                if (!externalTaken.count(candidate)) { packagePath = candidate; break; }
            }
        }
        externalTaken[packagePath] = key;

        ExportAsset a;
        a.sourcePath   = abs.string();
        a.packagePath  = packagePath;
        a.existsOnDisk = fs::exists(abs, ec) && !ec;
        seen[key] = out.size();
        out.push_back(std::move(a));
    };

    scene.traverse([&](GameObject* go)
    {
        if (go->hasMesh())
        {
            // sourcePath vacío = mesh procedural: su geometría ya viaja
            // dentro del .scene, no hay fichero que copiar.
            add(go->getMesh()->sourcePath);

            if (const SkinnedMesh* sm = go->getSkinnedMesh())
                for (const AnimationSource& src : sm->animationSources)
                    add(src.path);   // la fuente builtin repite sourcePath; add() deduplica

            for (const Material* m : materialsOf(go))
            {
                // Los embedded* no aportan path: viajan dentro del FBX.
                add(m->texturePath);
                add(m->normalMapPath);
                add(m->metallicRoughnessPath);
            }
        }

        if (go->hasAudioClip())
            add(go->getAudioClip()->getPath());

        for (const auto& s : go->getScripts())
        {
            auto it = scriptPaths.find(s->scriptName);
            if (it != scriptPaths.end())
                add(it->second.string());
        }
    });

    std::sort(out.begin(), out.end(),
              [](const ExportAsset& a, const ExportAsset& b) { return a.packagePath < b.packagePath; });
    return out;
}

} // namespace DonTopo
```

- [ ] **Step 6: Verificar que el test pasa**

```
.\build.bat
.\build-ninja\engine\tests\dt_exporter_tests.exe
```

Esperado: `OK`, exit code 0.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Editor/GameExporter.h engine/src/Editor/GameExporter.cpp engine/tests/exporter_tests.cpp engine/CMakeLists.txt engine/tests/CMakeLists.txt
git commit -m "feat(editor): recolectar los assets referenciados por la escena"
```

---

## Task 2: `rewriteScenePaths`

**Files:**
- Modify: `engine/src/Editor/GameExporter.cpp`
- Modify: `engine/tests/exporter_tests.cpp`

**Interfaces:**
- Consumes: `exportPathKey` y `ExportAsset` de la Tarea 1; `Scene::toJson()`.
- Produces: `int rewriteScenePaths(nlohmann::json& sceneJson, const std::map<std::string, std::string>& sourceToPackage)`. El mapa va keyeado por `exportPathKey(asset.sourcePath)` y el valor es `asset.packagePath`.

- [ ] **Step 1: Escribir el test que falla**

Añadir a `engine/tests/exporter_tests.cpp`, antes de `main()` (y añadir `#include <nlohmann/json.hpp>` a los includes del fichero):

```cpp
// Tras reescribir, ningún path del .scene puede seguir siendo absoluto: el
// paquete se ejecuta en otra máquina y otro directorio.
static void test_rewrite_makes_paths_relative(const fs::path& root)
{
    Scene scene;

    auto* hero = scene.addGameObject("hero");
    auto skinned = std::make_shared<SkinnedMesh>();
    skinned->sourcePath = (root / "assets" / "hero.fbx").string();
    skinned->animationSources.push_back({ (root / "assets" / "run.fbx").string(), false, {} });
    hero->setMesh(skinned);
    hero->setAudioClip(std::make_shared<AudioClipComponent>(
        nullptr, (root / "assets" / "step.wav").string(), -1, false, false));

    std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    std::map<std::string, std::string> sourceToPackage;
    for (const ExportAsset& a : assets)
        sourceToPackage[exportPathKey(a.sourcePath)] = a.packagePath;

    nlohmann::json j = scene.toJson();
    int rewritten = rewriteScenePaths(j, sourceToPackage);

    // hero.fbx (mesh) + hero.fbx (animationSource builtin? no lo hay) +
    // run.fbx + step.wav = 3 campos reescritos.
    CHECK(rewritten == 3);

    const nlohmann::json& node = j["root"]["children"][0];
    CHECK(node["mesh"]["sourcePath"].get<std::string>() == "assets/hero.fbx");
    CHECK(node["mesh"]["animationSources"][0]["path"].get<std::string>() == "assets/run.fbx");
    CHECK(node["audioClip"]["path"].get<std::string>() == "assets/step.wav");

    // Ningún path absoluto residual (en Windows: sin ':' de unidad).
    CHECK(node["mesh"]["sourcePath"].get<std::string>().find(':') == std::string::npos);
    CHECK(node["audioClip"]["path"].get<std::string>().find(':') == std::string::npos);
}

// Un path que no está en el mapa se deja intacto, no se borra ni se vacía.
static void test_rewrite_leaves_unknown_paths(const fs::path& root)
{
    nlohmann::json j;
    j["version"] = 1;
    j["root"] = { { "name", "root" },
                  { "mesh", { { "sourcePath", "C:/otro/sitio/x.fbx" } } },
                  { "children", nlohmann::json::array() } };

    int rewritten = rewriteScenePaths(j, {});
    CHECK(rewritten == 0);
    CHECK(j["root"]["mesh"]["sourcePath"].get<std::string>() == "C:/otro/sitio/x.fbx");
}
```

Y registrarlos en `main()`, tras `test_missing_asset_flagged(root);`:

```cpp
    test_rewrite_makes_paths_relative(root);
    test_rewrite_leaves_unknown_paths(root);
```

- [ ] **Step 2: Verificar que el test falla**

```
.\build.bat
```

Esperado: FALLO de enlazado, `unresolved external symbol ... rewriteScenePaths`.

- [ ] **Step 3: Implementar `rewriteScenePaths`**

Añadir `#include <nlohmann/json.hpp>` a los includes de `engine/src/Editor/GameExporter.cpp` y, dentro del `namespace DonTopo`, tras `collectSceneAssets`:

```cpp
namespace {

// Reescribe un campo de path si el mapa lo conoce. Devuelve 1 si tocó algo.
int rewriteField(nlohmann::json& holder, const char* field,
                 const std::map<std::string, std::string>& sourceToPackage)
{
    if (!holder.contains(field) || !holder[field].is_string()) return 0;
    const std::string current = holder[field].get<std::string>();
    if (current.empty()) return 0;
    auto it = sourceToPackage.find(DonTopo::exportPathKey(current));
    if (it == sourceToPackage.end()) return 0;
    holder[field] = it->second;
    return 1;
}

int rewriteNode(nlohmann::json& node, const std::map<std::string, std::string>& sourceToPackage)
{
    int n = 0;
    if (node.contains("mesh") && node["mesh"].is_object())
    {
        nlohmann::json& mesh = node["mesh"];
        n += rewriteField(mesh, "sourcePath", sourceToPackage);
        if (mesh.contains("animationSources") && mesh["animationSources"].is_array())
            for (nlohmann::json& src : mesh["animationSources"])
                n += rewriteField(src, "path", sourceToPackage);
    }
    if (node.contains("audioClip") && node["audioClip"].is_object())
        n += rewriteField(node["audioClip"], "path", sourceToPackage);

    if (node.contains("children") && node["children"].is_array())
        for (nlohmann::json& child : node["children"])
            n += rewriteNode(child, sourceToPackage);
    return n;
}

} // namespace

int rewriteScenePaths(nlohmann::json& sceneJson,
                      const std::map<std::string, std::string>& sourceToPackage)
{
    // Acepta tanto el documento completo de Scene::toJson() ({version, root})
    // como un nodo suelto, para que los tests puedan armar el JSON a mano.
    if (sceneJson.contains("root") && sceneJson["root"].is_object())
        return rewriteNode(sceneJson["root"], sourceToPackage);
    return rewriteNode(sceneJson, sourceToPackage);
}
```

Nota: ese `namespace { ... }` anónimo debe ir **dentro** de `namespace DonTopo`, después de `collectSceneAssets` y antes de `rewriteScenePaths`. El `namespace {}` del principio del fichero queda como está.

- [ ] **Step 4: Verificar que el test pasa**

```
.\build.bat
.\build-ninja\engine\tests\dt_exporter_tests.exe
```

Esperado: `OK`, exit code 0.

- [ ] **Step 5: Commit**

```bash
git add engine/src/Editor/GameExporter.cpp engine/tests/exporter_tests.cpp
git commit -m "feat(editor): reescribir a relativos los paths del .scene exportado"
```

---

## Task 3: `writeExportPackage`

**Files:**
- Modify: `engine/src/Editor/GameExporter.cpp`
- Modify: `engine/tests/exporter_tests.cpp`

**Interfaces:**
- Consumes: `ExportAsset`, `ExportResult` (Tarea 1); `FileManager::writeJson` (`engine/include/DonTopo/Files/FileManager.h`).
- Produces: `ExportResult writeExportPackage(const std::vector<ExportAsset>&, const nlohmann::json&, const std::filesystem::path& destDir, const std::string& gameName, const std::filesystem::path& projectRoot, const std::filesystem::path& scriptsDir, const std::filesystem::path& runtimeExe)`.

- [ ] **Step 1: Escribir el test que falla**

Añadir a `engine/tests/exporter_tests.cpp` antes de `main()`:

```cpp
// El paquete contiene el exe renombrado, game.scene, los assets del plan, el
// skybox, los shaders y Scripts/ — y ningún asset del proyecto que la escena
// no referencie (criterio de aceptación 3).
static void test_package_contents(const fs::path& root)
{
    std::error_code ec;

    // Completar el fixture con lo que writeExportPackage añade por su cuenta.
    fs::create_directories(root / "assets" / "skybox", ec);
    for (const char* face : { "px", "nx", "py", "ny", "pz", "nz" })
        std::ofstream(root / "assets" / "skybox" / (std::string(face) + ".png")) << "png";
    fs::create_directories(root / "shaders", ec);
    std::ofstream(root / "shaders" / "triangle.vert.spv") << "spv";
    // Asset del proyecto que la escena NO referencia: no debe acabar copiado.
    std::ofstream(root / "assets" / "huerfano.fbx") << "fbx";
    // Runtime de mentira.
    std::ofstream(root / "DonTopoRuntime.exe") << "MZ";

    Scene scene;
    scene.addGameObject("hero")->setMesh(makeMesh(root / "assets" / "hero.fbx"));
    std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});

    fs::path dest = fs::temp_directory_path(ec) / "dt_exporter_out";
    fs::remove_all(dest, ec);

    ExportResult r = writeExportPackage(assets, scene.toJson(), dest, "MiJuego",
                                        root, root / "Scripts", root / "DonTopoRuntime.exe");

    const fs::path pkg = dest / "MiJuego";
    CHECK(r.ok);
    CHECK(fs::exists(pkg / "MiJuego.exe"));
    CHECK(fs::exists(pkg / "game.scene"));
    CHECK(fs::exists(pkg / "assets" / "hero.fbx"));
    CHECK(fs::exists(pkg / "assets" / "skybox" / "px.png"));
    CHECK(fs::exists(pkg / "assets" / "skybox" / "nz.png"));
    CHECK(fs::exists(pkg / "shaders" / "triangle.vert.spv"));
    CHECK(fs::exists(pkg / "Scripts" / "Player.lua"));
    // Criterio 3: el asset no referenciado se queda fuera.
    CHECK(!fs::exists(pkg / "assets" / "huerfano.fbx"));
    CHECK(r.fileCount > 0);
    CHECK(r.totalBytes > 0);

    fs::remove_all(dest, ec);
}

// Re-exportar sobre una carpeta existente la deja limpia: nada de un export
// anterior sobrevive.
static void test_package_overwrite_is_clean(const fs::path& root)
{
    std::error_code ec;
    fs::path dest = fs::temp_directory_path(ec) / "dt_exporter_out2";
    fs::remove_all(dest, ec);
    fs::create_directories(dest / "MiJuego" / "assets", ec);
    std::ofstream(dest / "MiJuego" / "assets" / "basura_vieja.fbx") << "x";

    Scene scene;
    scene.addGameObject("hero")->setMesh(makeMesh(root / "assets" / "hero.fbx"));

    ExportResult r = writeExportPackage(collectSceneAssets(scene, root, {}), scene.toJson(),
                                        dest, "MiJuego", root, root / "Scripts",
                                        root / "DonTopoRuntime.exe");
    CHECK(r.ok);
    CHECK(!fs::exists(dest / "MiJuego" / "assets" / "basura_vieja.fbx"));

    fs::remove_all(dest, ec);
}

// Sin binario de runtime no se exporta nada: error explícito y carpeta sin crear.
static void test_missing_runtime_aborts(const fs::path& root)
{
    std::error_code ec;
    fs::path dest = fs::temp_directory_path(ec) / "dt_exporter_out3";
    fs::remove_all(dest, ec);

    Scene scene;
    ExportResult r = writeExportPackage({}, scene.toJson(), dest, "MiJuego",
                                        root, root / "Scripts", root / "no_existe_runtime.exe");
    CHECK(!r.ok);
    CHECK(!r.messages.empty());

    fs::remove_all(dest, ec);
}
```

Registrar en `main()`, tras `test_rewrite_leaves_unknown_paths(root);`:

```cpp
    test_package_contents(root);
    test_package_overwrite_is_clean(root);
    test_missing_runtime_aborts(root);
```

- [ ] **Step 2: Verificar que el test falla**

```
.\build.bat
```

Esperado: FALLO de enlazado, `unresolved external symbol ... writeExportPackage`.

- [ ] **Step 3: Implementar `writeExportPackage`**

Añadir `#include "DonTopo/Files/FileManager.h"` a los includes de `engine/src/Editor/GameExporter.cpp` y, al final del `namespace DonTopo`:

```cpp
ExportResult writeExportPackage(const std::vector<ExportAsset>& assets,
                                const nlohmann::json& rewrittenScene,
                                const fs::path& destDir,
                                const std::string& gameName,
                                const fs::path& projectRoot,
                                const fs::path& scriptsDir,
                                const fs::path& runtimeExe)
{
    ExportResult r;
    std::error_code ec;

    if (!fs::exists(runtimeExe, ec) || ec)
    {
        r.messages.push_back("Export cancelado: no se encuentra " + runtimeExe.string() +
                             ". Compila el target DonTopoRuntime.");
        return r;
    }

    const fs::path pkg = destDir / gameName;

    // Borrado + recreado: si se copiara encima, el paquete arrastraría assets
    // huérfanos de un export anterior y dejaría de cumplir "solo los
    // referenciados". La confirmación al usuario la pide la UI antes de
    // llamar aquí.
    fs::remove_all(pkg, ec);
    fs::create_directories(pkg, ec);
    if (ec)
    {
        r.messages.push_back("Export fallido: no se pudo crear " + pkg.string());
        return r;
    }

    auto copyOne = [&](const fs::path& from, const fs::path& to) -> bool
    {
        std::error_code cec;
        fs::create_directories(to.parent_path(), cec);
        if (!fs::copy_file(from, to, fs::copy_options::overwrite_existing, cec))
        {
            r.messages.push_back("No se pudo copiar " + from.string());
            return false;
        }
        std::error_code sec;
        std::uintmax_t size = fs::file_size(to, sec);
        if (!sec) r.totalBytes += size;
        ++r.fileCount;
        return true;
    };

    bool ok = copyOne(runtimeExe, pkg / (gameName + ".exe"));

    for (const ExportAsset& a : assets)
        ok = copyOne(fs::path(a.sourcePath), pkg / fs::path(a.packagePath)) && ok;

    // Skybox: el runtime lo tiene hardcoded (initSkybox con assets/skybox/*),
    // así que va siempre aunque la escena no lo "referencie".
    for (const char* face : { "px", "nx", "py", "ny", "pz", "nz" })
    {
        const fs::path from = projectRoot / "assets" / "skybox" / (std::string(face) + ".png");
        if (fs::exists(from, ec))
            ok = copyOne(from, pkg / "assets" / "skybox" / from.filename()) && ok;
    }

    // shaders/*.spv a la raíz del paquete: Renderer::createPipeline los abre
    // como "shaders/<nombre>.spv" relativo al CWD.
    {
        std::error_code dec;
        for (fs::directory_iterator it(projectRoot / "shaders", dec), end; !dec && it != end; it.increment(dec))
        {
            if (it->path().extension() != ".spv") continue;
            ok = copyOne(it->path(), pkg / "shaders" / it->path().filename()) && ok;
        }
    }

    // Scripts/ entera: los .lua se referencian por nombre y pueden hacer
    // require entre ellos, así que filtrar por referencias los rompería.
    if (fs::exists(scriptsDir, ec))
    {
        std::error_code rec;
        for (fs::recursive_directory_iterator it(scriptsDir, rec), end; !rec && it != end; it.increment(rec))
        {
            if (!it->is_regular_file()) continue;
            std::error_code relEc;
            fs::path rel = fs::relative(it->path(), scriptsDir, relEc);
            if (relEc) continue;
            ok = copyOne(it->path(), pkg / "Scripts" / rel) && ok;
        }
    }

    if (fs::exists(projectRoot / "fmod.dll", ec))
        ok = copyOne(projectRoot / "fmod.dll", pkg / "fmod.dll") && ok;

    if (!FileManager::writeJson((pkg / "game.scene").string(), rewrittenScene))
    {
        r.messages.push_back("No se pudo escribir game.scene");
        ok = false;
    }
    else
    {
        ++r.fileCount;
    }

    r.ok = ok;
    if (ok)
        r.messages.push_back("Export completado en " + pkg.string() + ": " +
                             std::to_string(r.fileCount) + " ficheros, " +
                             std::to_string(r.totalBytes / 1024) + " KB");
    return r;
}
```

- [ ] **Step 4: Verificar que el test pasa**

```
.\build.bat
.\build-ninja\engine\tests\dt_exporter_tests.exe
```

Esperado: `OK`, exit code 0.

- [ ] **Step 5: Commit**

```bash
git add engine/src/Editor/GameExporter.cpp engine/tests/exporter_tests.cpp
git commit -m "feat(editor): escribir el paquete exportado en disco"
```

---

## Task 4: Modo headless del Renderer

**Files:**
- Modify: `engine/include/DonTopo/Renderer/Renderer.h:41` y la zona de miembros privados
- Modify: `engine/src/Renderer/Renderer.cpp` (líneas 66, 116-122, 182, 363, 724-741, 2367)

**Interfaces:**
- Consumes: nada de tareas anteriores.
- Produces: `void Renderer::setHeadless(bool)`. Debe llamarse **antes** de `init()`. Con `true`, `isPlaying()` devuelve siempre `true` y no se inicializa ni se dibuja ImGui.

No hay test automatizado: es código Vulkan que necesita GPU y ventana. Se valida compilando y comprobando que el editor sigue funcionando igual (el runtime lo ejercita en la Tarea 5).

- [ ] **Step 1: Añadir el flag al header**

En `engine/include/DonTopo/Renderer/Renderer.h`, sustituir la línea 41:

```cpp
            bool isPlaying() const { return m_editorUI.isPlaying(); }
```

por:

```cpp
            // En headless no hay editor que pulse Play: el runtime arranca
            // jugando desde el frame 0. Esto es además lo que hace que
            // currentFrameCamera() elija el CameraComponent de la escena en
            // vez de la cámara de vuelo del editor.
            bool isPlaying() const { return m_headless || m_editorUI.isPlaying(); }
            // Modo runtime: ni ImGui ni paneles. Solo tiene efecto si se
            // llama ANTES de init() — initImGui/createOffscreenImages leen el
            // flag durante la inicialización.
            void setHeadless(bool headless) { m_headless = headless; }
```

Y junto a `bool m_wireframeMode = false;` (línea 332), añadir:

```cpp
            bool                            m_headless                          = false;
```

- [ ] **Step 2: Guardar la inicialización y el shutdown de ImGui**

En `engine/src/Renderer/Renderer.cpp:66`, sustituir:

```cpp
        initImGui(window.getNativeWindow()); // necesita m_renderPass + m_swapChainImages.size()
```

por:

```cpp
        // necesita m_renderPass + m_swapChainImages.size()
        if (!m_headless) initImGui(window.getNativeWindow());
```

En la línea 182, sustituir:

```cpp
        shutdownImGui();
```

por:

```cpp
        if (!m_headless) shutdownImGui();
```

- [ ] **Step 3: Guardar el frame de ImGui en `drawFrame`**

En `engine/src/Renderer/Renderer.cpp`, sustituir el bloque de las líneas 115-122:

```cpp
        // ── Construir frame ImGui (antes de grabar el command buffer) ─────────────
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        m_editorUI.draw(m_offscreenDescSet[m_currentFrame], m_sceneRoot, m_viewMatrix);

        ImGui::Render();
```

por:

```cpp
        // ── Construir frame ImGui (antes de grabar el command buffer) ─────────────
        // En headless no hay contexto ImGui que alimentar: el runtime blitea
        // la imagen offscreen directamente al swapchain (ver recordCommandBuffer).
        if (!m_headless)
        {
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            m_editorUI.draw(m_offscreenDescSet[m_currentFrame], m_sceneRoot, m_viewMatrix);

            ImGui::Render();
        }
```

- [ ] **Step 4: Añadir los usage flags de transferencia**

En `engine/src/Renderer/Renderer.cpp:363`, sustituir:

```cpp
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
```

por:

```cpp
        // TRANSFER_DST: en modo headless la imagen offscreen se blitea aquí en
        // vez de dibujarse ImGui encima. El flag va incondicional para que
        // editor y runtime compartan el mismo camino de creación de recursos.
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
```

En la línea 2367 (dentro de `createOffscreenImages`), sustituir:

```cpp
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
```

por:

```cpp
                // TRANSFER_SRC: origen del blit al swapchain en headless.
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
```

Y en el mismo `createOffscreenImages`, envolver el registro en ImGui (línea 2386):

```cpp
            // Registrar la textura en ImGui para obtener el VkDescriptorSet.
            // En headless nadie la muestrea: el descriptor set se queda nulo y
            // destroyOffscreenImages ya comprueba antes de liberarlo.
            if (!m_headless)
            {
                m_offscreenDescSet[i] = ImGui_ImplVulkan_AddTexture(
                    m_offscreenSampler, m_offscreenView[i],
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
```

- [ ] **Step 5: Sustituir el pass 2 por el blit en headless**

En `engine/src/Renderer/Renderer.cpp`, sustituir el bloque completo `// ── Pass 2: ImGui → swapchain ──` (líneas 724-741) por:

```cpp
        // ── Pass 2: ImGui → swapchain ─────────────────────────────────────────────
        if (!m_headless)
        {
            VkClearValue clearColor{};
            clearColor.color = {0.12f, 0.12f, 0.12f, 1.0f};

            VkRenderPassBeginInfo rpInfo{};
            rpInfo.sType               = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpInfo.renderPass          = m_renderPass;
            rpInfo.framebuffer         = m_swapChainFramebuffers[imageIndex];
            rpInfo.renderArea.extent   = m_swapChainExtent;
            rpInfo.renderArea.offset   = {0, 0};
            rpInfo.clearValueCount     = 1;
            rpInfo.pClearValues        = &clearColor;

            vkCmdBeginRenderPass(m_commandBuffers[m_currentFrame], &rpInfo, VK_SUBPASS_CONTENTS_INLINE);
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), m_commandBuffers[m_currentFrame]);
            vkCmdEndRenderPass(m_commandBuffers[m_currentFrame]);
        }
        else
        {
            // ── Pass 2 (headless): offscreen → swapchain ──────────────────────────
            // Sin editor no hay quien muestree la imagen offscreen, así que se
            // copia tal cual a la imagen de presentación. Es un blit 1:1: la
            // offscreen se crea con el mismo formato y extent que el swapchain
            // (createOffscreenImages). El renderpass offscreen declara
            // initialLayout=UNDEFINED, así que no hay que restaurar su layout
            // después del blit.
            VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];
            const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

            VkImageMemoryBarrier toTransferSrc{};
            toTransferSrc.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toTransferSrc.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toTransferSrc.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toTransferSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransferSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransferSrc.image               = m_offscreenImage[m_currentFrame];
            toTransferSrc.subresourceRange    = range;
            toTransferSrc.srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            toTransferSrc.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;

            VkImageMemoryBarrier toTransferDst{};
            toTransferDst.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toTransferDst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            toTransferDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toTransferDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransferDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransferDst.image               = m_swapChainImages[imageIndex];
            toTransferDst.subresourceRange    = range;
            toTransferDst.srcAccessMask       = 0;
            toTransferDst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;

            VkImageMemoryBarrier preBarriers[] = { toTransferSrc, toTransferDst };
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 2, preBarriers);

            VkImageBlit blit{};
            blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.srcOffsets[0]  = { 0, 0, 0 };
            blit.srcOffsets[1]  = { (int32_t)m_swapChainExtent.width, (int32_t)m_swapChainExtent.height, 1 };
            blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.dstOffsets[0]  = { 0, 0, 0 };
            blit.dstOffsets[1]  = { (int32_t)m_swapChainExtent.width, (int32_t)m_swapChainExtent.height, 1 };

            vkCmdBlitImage(cmd,
                m_offscreenImage[m_currentFrame], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                m_swapChainImages[imageIndex],     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &blit, VK_FILTER_NEAREST);

            VkImageMemoryBarrier toPresent{};
            toPresent.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toPresent.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toPresent.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toPresent.image               = m_swapChainImages[imageIndex];
            toPresent.subresourceRange    = range;
            toPresent.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            toPresent.dstAccessMask       = 0;

            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                0, 0, nullptr, 0, nullptr, 1, &toPresent);
        }
```

- [ ] **Step 6: Compilar y comprobar que el editor sigue igual**

```
.\build.bat
```

Esperado: compila sin errores y sin warnings nuevos. Ningún camino del editor cambia de comportamiento: todas las guardas nuevas evalúan `m_headless == false`.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Renderer/Renderer.h engine/src/Renderer/Renderer.cpp
git commit -m "feat(renderer): modo headless sin ImGui con blit offscreen a swapchain"
```

---

## Task 5: Target `DonTopoRuntime`

**Files:**
- Create: `runtime/CMakeLists.txt`
- Create: `runtime/main.cpp`
- Modify: `CMakeLists.txt` (raíz, última línea)

**Interfaces:**
- Consumes: `Renderer::setHeadless(bool)` (Tarea 4).
- Produces: ejecutable `DonTopoRuntime`, copiado por POST_BUILD a `$<TARGET_FILE_DIR:Sandbox>/DonTopoRuntime.exe`. Acepta `<exe> [ruta.scene]`, por defecto `game.scene` junto al ejecutable.

- [ ] **Step 1: Crear el CMakeLists del runtime**

Crear `runtime/CMakeLists.txt`:

```cmake
add_executable(DonTopoRuntime main.cpp)

target_link_libraries(DonTopoRuntime
    PRIVATE
        DonTopoEngine
)

target_compile_features(DonTopoRuntime PRIVATE cxx_std_20)

# El exportador del editor copia este binario al paquete, y lo busca en su
# propio directorio de trabajo (el mismo donde vive Sandbox.exe). Por eso este
# add_subdirectory va DESPUÉS del de sandbox en el CMakeLists raíz: el
# generator expression necesita el target Sandbox ya definido.
add_custom_command(TARGET DonTopoRuntime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:DonTopoRuntime>
        $<TARGET_FILE_DIR:Sandbox>/DonTopoRuntime.exe
    COMMENT "Copying DonTopoRuntime.exe next to the editor"
)
```

En `CMakeLists.txt` (raíz), tras `add_subdirectory(sandbox)`:

```cmake
add_subdirectory(sandbox)
add_subdirectory(runtime)
```

- [ ] **Step 2: Escribir el main del runtime**

Crear `runtime/main.cpp`:

```cpp
// Runtime del juego: carga un .scene y lo ejecuta. Es el wiring de
// sandbox/src/main.cpp menos todo lo del editor — sin ImGui, sin gizmos de
// depuración, sin hot reload y en Play desde el frame 0.
#include "DonTopo/Core/Engine.h"
#include "DonTopo/Core/Window.h"
#include "DonTopo/Core/Input.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Renderer/Renderer.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Scripting/ScriptManager.h"

#include <GLFW/glfw3.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// Directorio del ejecutable. El paquete exportado usa rutas relativas
// (assets/, shaders/, Scripts/), así que el runtime fija su CWD aquí: sin
// esto, lanzar el juego desde otra carpeta no encontraría nada.
std::filesystem::path executableDir()
{
#ifdef _WIN32
    wchar_t buffer[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (n == 0 || n == MAX_PATH)
        return std::filesystem::current_path();
    return std::filesystem::path(buffer).parent_path();
#else
    std::error_code ec;
    std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return std::filesystem::current_path();
    return self.parent_path();
#endif
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const std::filesystem::path exeDir = executableDir();
        std::error_code ec;
        std::filesystem::current_path(exeDir, ec);

        const std::string scenePath = (argc > 1) ? argv[1] : "game.scene";

        DonTopo::Engine engine;
        DonTopo::Window window;
        window.init(1280, 720, exeDir.stem().string().c_str(), nullptr);
        DonTopo::Input::init(window.getNativeWindow());
        DonTopo::Renderer renderer;

        // Orden de declaración calcado de sandbox/src/main.cpp:38-55, y por
        // los mismos motivos: los ScriptComponent guardan sol::table cuyo
        // destructor toca la VM Lua, y los colliders liberan actores sobre la
        // PxScene. Destruir en otro orden revienta al salir.
        DonTopo::PhysicsManager physics;
        physics.init();

        DonTopo::AudioManager audio;
        audio.init();

        DonTopo::ScriptManager scriptManager;

        DonTopo::Scene scene;

        if (!scene.load(scenePath, physics, audio))
        {
            std::cerr << "Error: no se pudo cargar la escena '" << scenePath << "'" << std::endl;
            return EXIT_FAILURE;
        }

        // Antes de init(): initImGui y createOffscreenImages leen el flag.
        renderer.setHeadless(true);

        std::vector<DonTopo::GameObject*> allNodes;
        scene.traverse([&](DonTopo::GameObject* go) { allNodes.push_back(go); });

        // Pasada 1: meshes estáticos -> Renderer::init(meshes)
        std::vector<DonTopo::Mesh> meshes;
        for (auto* go : allNodes)
        {
            if (go->hasMesh() && !go->isSkinned())
            {
                go->staticRenderIndex = (int)meshes.size();
                meshes.push_back(*go->getMesh());
            }
        }

        renderer.init(window, meshes);
        renderer.setSceneRoot(&scene.getRoot());
        renderer.setScene(&scene);
        renderer.setPhysicsManager(&physics);
        renderer.setAudioManager(&audio);

        renderer.initSkybox({
            "assets/skybox/px.png",
            "assets/skybox/nx.png",
            "assets/skybox/py.png",
            "assets/skybox/ny.png",
            "assets/skybox/pz.png",
            "assets/skybox/nz.png",
        });

        // Pasada 2: meshes animados, después de init como exige el Renderer.
        for (auto* go : allNodes)
        {
            if (go->hasMesh() && go->isSkinned())
                go->skinnedRenderIndex = renderer.addSkinnedMesh(*go->getSkinnedMesh());
        }

        // Mismas luces que el editor: la escena no las serializa.
        renderer.setLights({
            { glm::vec4(0.0f, 500.0f, 300.0f, 1.0f),     glm::vec4(1.0f, 0.95f, 0.8f, 1.0f) },
            { glm::vec4(-300.0f, 200.0f, -200.0f, 1.0f), glm::vec4(0.4f, 0.5f, 1.0f, 0.8f) },
        });

        scriptManager.setScene(&scene);
        scriptManager.setPhysicsManager(&physics);
        scriptManager.setAudioManager(&audio);
        scriptManager.setLogCallback([](const std::string& msg) {
            std::cout << msg << std::endl;
        });
        scriptManager.setOnInstantiated([&renderer](DonTopo::GameObject* go) {
            go->traverse([&renderer](DonTopo::GameObject* n) {
                if (!n->hasMesh()) return;
                if (n->isSkinned()) n->skinnedRenderIndex = renderer.addSkinnedMesh(*n->getSkinnedMesh());
                else                n->staticRenderIndex  = renderer.addStaticMesh(*n->getMesh());
            });
        });
        scriptManager.setOnDestroying([&renderer](DonTopo::GameObject* go) {
            renderer.removeGameObject(go);
        });
        // Scripts/ va dentro del paquete, junto al ejecutable — a diferencia
        // del editor, que la busca subiendo directorios hacia el repo.
        scriptManager.init("Scripts");
        renderer.setScriptManager(&scriptManager);

        glfwSetWindowUserPointer(window.getNativeWindow(), &renderer);
        glfwSetFramebufferSizeCallback(window.getNativeWindow(), [](GLFWwindow* w, int, int) {
            static_cast<DonTopo::Renderer*>(glfwGetWindowUserPointer(w))->notifyResize();
        });
        glfwSetKeyCallback(window.getNativeWindow(), [](GLFWwindow* w, int key, int, int action, int) {
            if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
                glfwSetWindowShouldClose(w, GLFW_TRUE);
        });

        scriptManager.onPlayStart();

        while (!window.shouldClose())
        {
            DonTopo::Input::update();

            auto now = std::chrono::high_resolution_clock::now();
            static auto last = now;
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;

            audio.update(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            physics.stepSimulation(dt);
            scene.update(dt, physics);
            scriptManager.update(dt);

            scene.traverse([&](DonTopo::GameObject* go) {
                if (go->staticRenderIndex >= 0)
                    renderer.setTransform(go->staticRenderIndex, go->worldTransform);

                if (go->skinnedRenderIndex >= 0)
                {
                    if (const auto& anim = go->getAnimator())
                    {
                        anim->update(dt, /*playing=*/true);
                        renderer.setAnimationState(go->skinnedRenderIndex,
                                                    (uint32_t)anim->currentClipIndex(),
                                                    anim->animTime());
                    }
                    else
                    {
                        renderer.updateAnimation(go->skinnedRenderIndex, dt);
                    }
                    renderer.setSkinnedTransform(go->skinnedRenderIndex, go->worldTransform);
                }
            });

            renderer.drawFrame(window);
            window.pollEvents();
        }

        scriptManager.onPlayStop();
        scene.shutdown(physics, audio);
        audio.shutdown();
        physics.shutdown();
        renderer.shutdown();
        window.shutdown();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return 0;
}
```

- [ ] **Step 3: Compilar**

```
.\configure.bat
.\build.bat
```

Esperado: compilan `Sandbox` y `DonTopoRuntime` sin errores. Si falla por includes que faltan (`AnimatorComponent.h`, `glm`), añadirlos al bloque de includes de `runtime/main.cpp` — no cambiar la lógica.

- [ ] **Step 4: Verificar que el binario se copió junto al editor**

```
Test-Path .\build-ninja\sandbox\DonTopoRuntime.exe
```

Esperado: `True`.

- [ ] **Step 5: Commit**

```bash
git add runtime/CMakeLists.txt runtime/main.cpp CMakeLists.txt
git commit -m "feat(runtime): target DonTopoRuntime que ejecuta un .scene sin editor"
```

---

## Task 6: `File > Export Game...` en el editor

**Files:**
- Modify: `engine/include/DonTopo/Editor/EditorUI.h`
- Modify: `engine/src/Editor/EditorUI.cpp` (`drawMenuBar` en la línea 106, `draw` en la 65)

**Interfaces:**
- Consumes: `collectSceneAssets`, `rewriteScenePaths`, `writeExportPackage`, `exportPathKey` (Tareas 1-3); `DonTopoRuntime.exe` junto al editor (Tarea 5); `ScriptManager::{getRegistry,scriptsDirPath}`; `Scene::{findCamera,toJson}`.
- Produces: nada que consuman otras tareas.

- [ ] **Step 1: Declarar el estado y los métodos en el header**

En `engine/include/DonTopo/Editor/EditorUI.h`, tras la declaración de `bool reloadSceneFromJson(const nlohmann::json& j);`:

```cpp
    // Export Game — drena el diálogo de carpeta destino y pinta los dos
    // popups modales (nombre y confirmación de sobrescritura). Se llama cada
    // frame desde draw(), igual que drawSceneDialog.
    void drawExportDialog();
    // Ejecuta el export completo con m_exportDestDir y m_exportNameBuffer ya
    // fijados. Vuelca al Log Console tanto los errores como el resumen.
    void runExport();
```

Y tras el bloque de `m_sceneIOError`:

```cpp
    // Export Game — instancia propia de diálogo por el mismo motivo que
    // m_sceneFileDialog: IGFD::FileDialog::Instance() no soporta diálogos
    // concurrentes.
    std::unique_ptr<IGFD::FileDialog> m_exportDialog;
    bool        m_exportDlgOpen          = false;
    std::string m_exportDestDir;
    char        m_exportNameBuffer[64]   = "Game";
    bool        m_openExportNamePopup    = false;
    bool        m_openExportConfirmPopup = false;
```

- [ ] **Step 2: Inicializar el diálogo y llamar al drenaje**

En `engine/src/Editor/EditorUI.cpp`, en el constructor `EditorUI::EditorUI()`, junto a la creación de `m_sceneFileDialog`, añadir:

```cpp
    m_exportDialog = std::make_unique<IGFD::FileDialog>();
```

En `EditorUI::draw`, tras `drawSceneDialog();` (línea 65):

```cpp
    drawExportDialog();
```

- [ ] **Step 3: Añadir el menú File**

En `EditorUI::drawMenuBar` (línea 106), **antes** del `if (ImGui::BeginMenu("View"))`:

```cpp
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Export Game...", nullptr, false, m_scene != nullptr))
            {
                IGFD::FileDialogConfig cfg;
                cfg.path  = ".";
                cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_HideColumnDate |
                            ImGuiFileDialogFlags_DisableThumbnailMode |
                            ImGuiFileDialogFlags_DisablePlaceMode;
                // filters = nullptr -> IGFD selecciona carpeta, no fichero.
                m_exportDialog->OpenDialog("ExportDlg", "Carpeta destino del export", nullptr, cfg);
                m_exportDlgOpen = true;
            }
            ImGui::EndMenu();
        }
```

- [ ] **Step 4: Implementar `drawExportDialog` y `runExport`**

Añadir los includes necesarios al principio de `engine/src/Editor/EditorUI.cpp`:

```cpp
#include "DonTopo/Editor/GameExporter.h"
#include "DonTopo/Scripting/ScriptManager.h"
```

Y las dos funciones, junto a `drawSceneDialog`:

```cpp
void EditorUI::drawExportDialog()
{
    // Se ejecuta cada frame aunque m_exportDlgOpen sea false, mismo motivo
    // que drawSceneDialog: hay que drenar el diálogo aunque el usuario lo
    // cierre sin confirmar.
    if (m_exportDlgOpen && m_exportDialog->Display("ExportDlg"))
    {
        if (m_exportDialog->IsOk())
        {
            m_exportDestDir = m_exportDialog->GetCurrentPath();
            m_openExportNamePopup = true;
        }
        m_exportDialog->Close();
        m_exportDlgOpen = false;
    }

    if (m_openExportNamePopup)
    {
        ImGui::OpenPopup("Export Game");
        m_openExportNamePopup = false;
    }

    if (ImGui::BeginPopupModal("Export Game", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Destino: %s", m_exportDestDir.c_str());
        ImGui::InputText("Nombre", m_exportNameBuffer, sizeof(m_exportNameBuffer));

        const bool nameOk = m_exportNameBuffer[0] != '\0';
        ImGui::BeginDisabled(!nameOk);
        if (ImGui::Button("Export"))
        {
            std::error_code ec;
            const std::filesystem::path pkg =
                std::filesystem::path(m_exportDestDir) / m_exportNameBuffer;
            if (std::filesystem::exists(pkg, ec))
                m_openExportConfirmPopup = true;
            else
                runExport();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (m_openExportConfirmPopup)
    {
        ImGui::OpenPopup("Sobrescribir export");
        m_openExportConfirmPopup = false;
    }

    if (ImGui::BeginPopupModal("Sobrescribir export", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                           "La carpeta '%s' ya existe.", m_exportNameBuffer);
        ImGui::Text("Se borrara todo su contenido antes de exportar.");
        if (ImGui::Button("Borrar y exportar"))
        {
            runExport();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void EditorUI::runExport()
{
    namespace fs = std::filesystem;

    if (!m_scene)
    {
        m_logPanel.push("Export cancelado: no hay escena abierta");
        return;
    }
    // Sin camara el juego no podria renderizar: se falla aqui, donde el
    // usuario puede arreglarlo, y no en un .exe que abre una ventana negra.
    if (!m_scene->findCamera())
    {
        m_logPanel.push("Export cancelado: la escena no tiene camara (Add > Camera en Properties)");
        return;
    }

    std::error_code ec;
    fs::path projectRoot = fs::current_path(ec);
    if (ec) projectRoot = ".";
    fs::path canon = fs::canonical(projectRoot, ec);
    if (!ec) projectRoot = canon;

    const fs::path runtimeExe = projectRoot / "DonTopoRuntime.exe";
    if (!fs::exists(runtimeExe, ec))
    {
        m_logPanel.push("Export cancelado: falta " + runtimeExe.string() +
                        ". Compila el target DonTopoRuntime.");
        return;
    }

    std::map<std::string, fs::path> scriptPaths;
    if (m_scriptManager)
        for (const auto& [name, cls] : m_scriptManager->getRegistry())
            scriptPaths[name] = cls.path;

    std::vector<ExportAsset> assets = collectSceneAssets(*m_scene, projectRoot, scriptPaths);

    std::vector<std::string> missing;
    for (const ExportAsset& a : assets)
        if (!a.existsOnDisk) missing.push_back(a.sourcePath);
    if (!missing.empty())
    {
        m_logPanel.push("Export cancelado: faltan en disco " +
                        std::to_string(missing.size()) + " assets referenciados:");
        for (const std::string& m : missing)
            m_logPanel.push("  " + m);
        return;
    }

    std::map<std::string, std::string> sourceToPackage;
    for (const ExportAsset& a : assets)
        sourceToPackage[exportPathKey(a.sourcePath)] = a.packagePath;

    nlohmann::json sceneJson = m_scene->toJson();
    rewriteScenePaths(sceneJson, sourceToPackage);

    const fs::path scriptsDir = m_scriptManager ? m_scriptManager->scriptsDirPath()
                                                : projectRoot / "Scripts";

    ExportResult result = writeExportPackage(assets, sceneJson, m_exportDestDir,
                                             m_exportNameBuffer, projectRoot,
                                             scriptsDir, runtimeExe);
    for (const std::string& msg : result.messages)
        m_logPanel.push(msg);
    if (!result.ok)
        m_logPanel.push("Export FALLIDO");
}
```

- [ ] **Step 5: Compilar**

```
.\build.bat
```

Esperado: compila sin errores ni warnings nuevos.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Editor/EditorUI.h engine/src/Editor/EditorUI.cpp
git commit -m "feat(editor): menu File > Export Game con dialogo y confirmacion"
```

---

## Task 7: Verificación final

**Files:** ninguno (solo ejecución).

- [ ] **Step 1: Build limpio de todo**

```
.\configure.bat
.\build.bat
```

Esperado: `Sandbox` y `DonTopoRuntime` compilan sin errores ni warnings nuevos (criterio de aceptación 1).

- [ ] **Step 2: Ejecutar los 5 ejecutables de test**

```
.\build-ninja\engine\tests\dt_physics_tests.exe
.\build-ninja\engine\tests\dt_content_browser_tests.exe
.\build-ninja\engine\tests\dt_camera_tests.exe
.\build-ninja\engine\tests\dt_animator_tests.exe
.\build-ninja\engine\tests\dt_exporter_tests.exe
```

Esperado: los cinco imprimen `OK` y devuelven 0 (criterio de aceptación 2). Ejecutarlos **uno a uno**, no en paralelo ni en el mismo proceso: el motor solo admite una `PxFoundation` por proceso.

- [ ] **Step 3: Comprobar que `sandbox/src/main.cpp` no ha cambiado**

```bash
git diff --stat main -- sandbox/src/main.cpp
```

Esperado: salida vacía (criterio de aceptación 5).

- [ ] **Step 4: Parar y entregar los pasos de verificación manual**

El criterio de aceptación 4 exige GUI y lo verifica el usuario. Entregarle estos pasos exactos:

1. Lanzar `.\build-ninja\sandbox\Sandbox.exe`.
2. Cargar o construir una escena que tenga una cámara (`Add > Camera` en Properties si no la tiene) y guardarla.
3. `File > Export Game...`, elegir una carpeta **fuera del repo** (p.ej. `C:\Temp`), nombre `MiJuego`, pulsar Export.
4. Comprobar en el Log Console el resumen de ficheros copiados y tamaño.
5. Abrir `C:\Temp\MiJuego\` y verificar que `assets/` solo tiene lo que la escena usa, más `assets/skybox/`.
6. Ejecutar `C:\Temp\MiJuego\MiJuego.exe` con doble clic: debe abrir ventana, renderizar la escena desde la cámara de la escena, correr física y scripts, y **no** mostrar ninguna ventana de ImGui. `ESC` cierra.

---

## Self-Review

**Cobertura del spec:**

| Sección del spec | Tarea |
|---|---|
| `collectSceneAssets` + `exportPathKey` | 1 |
| `packagePath` y `assets/_external/` | 1 |
| `rewriteScenePaths` | 2 |
| `writeExportPackage`, skybox, shaders, Scripts/, fmod.dll | 3 |
| Sobrescritura limpia de la carpeta destino | 3 (borrado) + 6 (confirmación) |
| Modo headless del Renderer | 4 |
| `DonTopoRuntime` + luces hardcoded + CWD del exe | 5 |
| UI `File > Export Game...` | 6 |
| Contrato de errores (sin cámara, assets ausentes, runtime ausente) | 6 (cámara, assets) + 3 (runtime) |
| Los 7 tests del spec | 1 (tests 1-5, 7) + 2 (test 6) |
| Criterios de aceptación 1, 2, 3, 5 | 7 |
| Criterio de aceptación 4 (manual) | 7, Step 4 |

**Consistencia de tipos:** `ExportAsset`/`ExportResult` se definen una sola vez (Tarea 1, header) y las Tareas 2, 3 y 6 usan exactamente esos nombres de campo. `exportPathKey` es la única función de normalización y la usan la recolección (Tarea 1), la reescritura (Tarea 2) y el llamador (Tarea 6) — no hay una segunda convención de claves. La firma de `writeExportPackage` incluye `scriptsDir` en las Tareas 1 (declaración), 3 (implementación y tests) y 6 (llamada), con el mismo orden de parámetros.
