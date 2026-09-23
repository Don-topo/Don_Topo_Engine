# Importación real de assets externos — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Permitir traer un fichero de fuera del proyecto (FBX, textura, audio o fuente) al editor por dos caminos — drag&drop desde el Explorador de Windows sobre el Content Browser, y los 18 diálogos "Browse..." de Properties — copiándolo al proyecto en vez de rechazarlo, que es lo único que hace hoy.

**Architecture:** Un módulo nuevo (`AssetImport.h/.cpp`) centraliza qué extensiones se pueden importar y cómo se copian, sin sobreescribir nada. El Content Browser lo consume a partir de una cola de ficheros soltados sobre la ventana (`glfwSetDropCallback`, registrado en `sandbox/src/main.cpp` para los dos backends) que llega al panel por un campo nuevo de `EditorContext`. Los 18 diálogos de Properties reutilizan el mismo módulo cambiando la firma de `canAcceptAsset` de `bool` a `std::optional<std::filesystem::path>`.

**Tech Stack:** C++20, GLFW (`glfwSetDropCallback`, ya en el proyecto — sin dependencia nueva), Dear ImGui, `std::filesystem`, CMake + Ninja + MSVC.

**Spec:** `docs/superpowers/specs/2026-09-23-external-asset-import-design.md`

## Global Constraints

- Build: `.\configure.bat` + `.\build.bat` desde la raíz del repo, en PowerShell (nunca `cmake` crudo desde Bash) — este plan añade un `.cpp` nuevo, así que hace falta reconfigurar antes del primer build de cada tarea que lo toque.
- Tests: `main()` + macro `CHECK`, sin framework, registrados con el helper `dt_add_test(nombre fichero.cpp LIB)` de `engine/tests/CMakeLists.txt` — no usar el patrón `add_executable` manual de planes antiguos.
- Toda llamada a `std::filesystem` que toque disco usa la sobrecarga con `std::error_code`. Nada de excepciones en el bucle de render.
- Comentarios de código en español, como el resto del repo.
- No añadir dependencias nuevas de terceros (constraint del encargo de auditoría original).
- Mantener paridad Vulkan/D3D12 en cualquier wiring de `sandbox/src/main.cpp`: los dos backends son ramas independientes de `main()` con su propio bucle, y cada callback de ventana que se añada a una rama se añade a la otra.
- Importar nunca sobreescribe: un nombre ya ocupado en el destino es un rechazo (`RejectedNameConflict`), no una fusión ni un renombrado automático.

## Review Focus

- Fichero soltado que es una **carpeta**, no un fichero (arrastrar una carpeta entera desde el Explorador) — `importExternalAsset` no debe intentar `copy_file` sobre un directorio. Task 1.
- **Conflicto de nombre**: el destino ya tiene un fichero con ese nombre — debe rechazarse sin tocarlo, nunca sobreescribirlo. Task 1.
- **Lote mixto**: varios ficheros soltados a la vez, unos válidos y otros no (conflicto, extensión no soportada, fuera del rect de la ventana) — ninguno debe abortar el procesado de los demás. Task 2.
- **Sin proyecto abierto** (`ctx.project == nullptr`, tests headless o arranque antes del selector): `canAcceptAsset` debe seguir devolviendo el path tal cual, sin intentar copiar nada — es el comportamiento actual y no debe cambiar. Task 4 (verificado por inspección del diff, no por test automático — ver nota en esa tarea).
- **Edición bloqueada** (`ctx.editingLocked == true`) con un fichero externo de extensión importable: el veto de "carga de escena en curso" debe seguir ganando ANTES de intentar copiar nada. Task 4 (verificado por inspección del diff, mismo motivo).

---

### Task 1: Módulo `AssetImport` — copia de ficheros externos

**Files:**
- Create: `engine/include/DonTopo/Editor/AssetImport.h`
- Create: `engine/src/Editor/AssetImport.cpp`
- Create: `engine/tests/asset_import_tests.cpp`
- Modify: `engine/CMakeLists.txt:152` (registrar `AssetImport.cpp` en la lib `DonTopoEditor`)
- Modify: `engine/tests/CMakeLists.txt` (registrar `dt_asset_import_tests`)

**Interfaces:**
- Consumes: nada (primera tarea).
- Produces: `bool DonTopo::isImportableExtension(const std::string&)`, `std::filesystem::path DonTopo::importedAssetDestDir(const std::filesystem::path& projectRoot, const std::string& ext)`, `DonTopo::AssetImportOutcome DonTopo::importExternalAsset(const std::filesystem::path& source, const std::filesystem::path& destDir)`, `std::string DonTopo::describeImportResult(const AssetImportOutcome&)`, y los tipos `DonTopo::AssetImportResult` (enum class: `Copied`, `RejectedExtension`, `RejectedNameConflict`, `RejectedCopyFailed`) y `DonTopo::AssetImportOutcome` (struct: `result`, `destPath`, `errorMessage`) — los usan las Tasks 2 y 4.

- [ ] **Step 1: Escribir el test que falla**

Crear `engine/tests/asset_import_tests.cpp`:

```cpp
// Test headless de AssetImport (sin GUI). Plain main + CHECK, mismo patron
// que content_browser_tests.cpp.
#include "DonTopo/Editor/AssetImport.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

using namespace DonTopo;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static fs::path makeFixture()
{
    std::error_code ec;
    fs::path root = fs::temp_directory_path(ec) / "dt_asset_import_test";
    fs::remove_all(root, ec);
    fs::create_directories(root / "source", ec);
    fs::create_directories(root / "dest", ec);
    return root;
}

static void test_is_importable_extension()
{
    CHECK(isImportableExtension(".fbx"));
    CHECK(isImportableExtension(".PNG"));   // mayusculas
    CHECK(isImportableExtension(".wav"));
    CHECK(isImportableExtension(".ttf"));
    CHECK(!isImportableExtension(".txt"));
    CHECK(!isImportableExtension(".spv"));
    CHECK(!isImportableExtension(""));
}

static void test_imported_asset_dest_dir()
{
    fs::path root = fs::path("proyecto");
    CHECK(importedAssetDestDir(root, ".fbx") == root / "assets" / "Imported" / "Meshes");
    CHECK(importedAssetDestDir(root, ".WAV") == root / "assets" / "Imported" / "Audio");
    CHECK(importedAssetDestDir(root, ".png") == root / "assets" / "Imported" / "Textures");
    CHECK(importedAssetDestDir(root, ".ttf") == root / "assets" / "Imported" / "Fonts");
    CHECK(importedAssetDestDir(root, ".xyz").empty());
}

static void test_import_copies_file(const fs::path& root)
{
    fs::path source = root / "source" / "modelo.fbx";
    std::ofstream(source) << "contenido-fbx";
    fs::path destDir = root / "dest" / "copia1";

    AssetImportOutcome outcome = importExternalAsset(source, destDir);

    CHECK(outcome.result == AssetImportResult::Copied);
    CHECK(outcome.destPath == destDir / "modelo.fbx");
    CHECK(fs::exists(outcome.destPath));
    std::ifstream in(outcome.destPath);
    std::stringstream ss; ss << in.rdbuf();
    CHECK(ss.str() == "contenido-fbx");
}

static void test_import_missing_source_fails(const fs::path& root)
{
    AssetImportOutcome outcome = importExternalAsset(root / "no_existe.fbx", root / "dest");
    CHECK(outcome.result == AssetImportResult::RejectedCopyFailed);
    CHECK(!outcome.errorMessage.empty());
}

// Soltar una CARPETA (no un fichero) desde el Explorador: no debe intentar
// copy_file sobre un directorio.
static void test_import_directory_source_fails(const fs::path& root)
{
    AssetImportOutcome outcome = importExternalAsset(root / "source", root / "dest");
    CHECK(outcome.result == AssetImportResult::RejectedCopyFailed);
}

// El destino ya tiene un fichero con ese nombre: se rechaza SIN tocarlo, no
// se sobreescribe ni se renombra en automatico.
static void test_import_name_conflict_does_not_overwrite(const fs::path& root)
{
    fs::path destDir = root / "dest" / "copia2";
    std::error_code ec;
    fs::create_directories(destDir, ec);
    std::ofstream(destDir / "textura.png") << "version-vieja";

    fs::path source = root / "source" / "textura.png";
    std::ofstream(source) << "version-nueva";

    AssetImportOutcome outcome = importExternalAsset(source, destDir);

    CHECK(outcome.result == AssetImportResult::RejectedNameConflict);
    std::ifstream in(destDir / "textura.png");
    std::stringstream ss; ss << in.rdbuf();
    CHECK(ss.str() == "version-vieja");
}

int main()
{
    fs::path root = makeFixture();
    test_is_importable_extension();
    test_imported_asset_dest_dir();
    test_import_copies_file(root);
    test_import_missing_source_fails(root);
    test_import_directory_source_fails(root);
    test_import_name_conflict_does_not_overwrite(root);
    std::error_code ec;
    fs::remove_all(root, ec);
    if (g_failures == 0) std::printf("ALL ASSET IMPORT TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Registrar el test en CMake**

En `engine/tests/CMakeLists.txt`, añadir junto a `dt_content_browser_tests`:

```cmake
dt_add_test(dt_asset_import_tests asset_import_tests.cpp DonTopoEditor)
```

- [ ] **Step 3: Compilar para verificar que falla**

Run: `.\configure.bat` luego `.\build.bat`
Expected: FALLO — `AssetImport.h` no existe todavía (`fatal error: DonTopo/Editor/AssetImport.h: No such file or directory` o equivalente de MSVC).

- [ ] **Step 4: Crear el header**

Crear `engine/include/DonTopo/Editor/AssetImport.h`:

```cpp
#pragma once
#include <filesystem>
#include <string>

namespace DonTopo {

// Resultado de intentar copiar un fichero externo (fuera del proyecto o del
// workspace compartido del motor) a una carpeta del proyecto. No hay
// sobreescritura: un nombre ya ocupado en destino es un rechazo, no un
// reemplazo — el usuario borra o renombra a mano primero.
enum class AssetImportResult {
    Copied,
    RejectedExtension,
    RejectedNameConflict,
    RejectedCopyFailed,
};

struct AssetImportOutcome {
    AssetImportResult result = AssetImportResult::RejectedCopyFailed;
    // Valido solo si result == Copied.
    std::filesystem::path destPath;
    // Vacio salvo RejectedCopyFailed (mensaje de std::error_code).
    std::string errorMessage;
};

// true si ext (con el punto, cualquier combinacion de mayusc/minusc) es uno
// de los tipos que el editor sabe importar hoy: mismo set que ya usa el
// drag&drop interno del grid del Content Browser. Unica fuente de verdad —
// ContentBrowserPanel y PropertiesPanel la comparten en vez de mantener cada
// uno su propia lista.
bool isImportableExtension(const std::string& ext);

// Carpeta destino por tipo de extension, bajo projectRoot/assets/Imported/:
// Meshes, Audio, Textures o Fonts. Vacio si la extension no es importable
// (isImportableExtension(ext) == false) — el llamante decide que hacer con
// eso, esta funcion no rechaza nada por su cuenta.
std::filesystem::path importedAssetDestDir(const std::filesystem::path& projectRoot,
                                            const std::string& ext);

// Copia source a destDir/source.filename(). No sobreescribe: si el destino ya
// existe, devuelve RejectedNameConflict sin tocar disco. Crea destDir si no
// existe. Nunca lanza — std::filesystem con la sobrecarga de std::error_code
// en todas las llamadas a disco.
AssetImportOutcome importExternalAsset(const std::filesystem::path& source,
                                        const std::filesystem::path& destDir);

// Mensaje de log legible para un resultado que NO es Copied (para Copied el
// llamante ya tiene destPath y compone su propio mensaje de exito).
std::string describeImportResult(const AssetImportOutcome& outcome);

} // namespace DonTopo
```

- [ ] **Step 5: Implementar**

Crear `engine/src/Editor/AssetImport.cpp`:

```cpp
#include "DonTopo/Editor/AssetImport.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <system_error>

namespace DonTopo {

namespace {
std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
} // namespace

bool isImportableExtension(const std::string& ext)
{
    static const std::set<std::string> kImportable = {
        ".fbx",
        ".wav", ".mp3", ".ogg", ".flac",
        ".png", ".jpg", ".jpeg", ".bmp", ".tga",
        ".ttf", ".otf", ".ttc"};
    return kImportable.count(toLower(ext)) != 0;
}

std::filesystem::path importedAssetDestDir(const std::filesystem::path& projectRoot,
                                            const std::string& ext)
{
    static const std::set<std::string> kAudio = {".wav", ".mp3", ".ogg", ".flac"};
    static const std::set<std::string> kImage = {".png", ".jpg", ".jpeg", ".bmp", ".tga"};
    static const std::set<std::string> kFont  = {".ttf", ".otf", ".ttc"};

    const std::string lower = toLower(ext);
    const std::filesystem::path imported = projectRoot / "assets" / "Imported";
    if (lower == ".fbx")     return imported / "Meshes";
    if (kAudio.count(lower)) return imported / "Audio";
    if (kImage.count(lower)) return imported / "Textures";
    if (kFont.count(lower))  return imported / "Fonts";
    return {};
}

AssetImportOutcome importExternalAsset(const std::filesystem::path& source,
                                        const std::filesystem::path& destDir)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec) || ec)
        return { AssetImportResult::RejectedCopyFailed, {}, "El origen no es un fichero" };

    std::filesystem::create_directories(destDir, ec);
    ec.clear();

    const std::filesystem::path dest = destDir / source.filename();
    const bool ok = std::filesystem::copy_file(source, dest, ec);
    if (!ok)
    {
        if (ec == std::errc::file_exists)
            return { AssetImportResult::RejectedNameConflict, {}, "" };
        return { AssetImportResult::RejectedCopyFailed, {}, ec.message() };
    }
    return { AssetImportResult::Copied, dest, "" };
}

std::string describeImportResult(const AssetImportOutcome& outcome)
{
    switch (outcome.result)
    {
        case AssetImportResult::Copied:              return "importado";
        case AssetImportResult::RejectedExtension:    return "extension no soportada para importar";
        case AssetImportResult::RejectedNameConflict: return "ya existe un fichero con ese nombre en el destino";
        case AssetImportResult::RejectedCopyFailed:
            return outcome.errorMessage.empty() ? "fallo de copia" : outcome.errorMessage;
    }
    return "fallo de copia";
}

} // namespace DonTopo
```

- [ ] **Step 6: Registrar en la librería**

En `engine/CMakeLists.txt`, dentro del `add_library(DonTopoEditor STATIC ...)`, añadir junto a `src/Editor/ContentBrowserPanel.cpp` (línea 152):

```cmake
    src/Editor/AssetImport.cpp
```

- [ ] **Step 7: Compilar y correr el test**

Run: `.\configure.bat` luego `.\build.bat`
Expected: build OK.

Run: `.\build-ninja\engine\tests\dt_asset_import_tests.exe`
Expected: `ALL ASSET IMPORT TESTS PASSED`, exit code 0.

- [ ] **Step 8: Verificar que no se rompió nada existente**

Run: `.\build-ninja\engine\tests\dt_content_browser_tests.exe`
Expected: `ALL CONTENT BROWSER TESTS PASSED`, exit code 0.

- [ ] **Step 9: Commit**

```bash
git add engine/include/DonTopo/Editor/AssetImport.h engine/src/Editor/AssetImport.cpp \
        engine/tests/asset_import_tests.cpp engine/tests/CMakeLists.txt engine/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(editor): AssetImport, copia de ficheros externos al proyecto

isImportableExtension/importedAssetDestDir/importExternalAsset/
describeImportResult, compartido por el drop del Content Browser (Task 2) y
los 18 dialogos Browse... de Properties (Task 4). No sobreescribe: un nombre
ya ocupado en destino se rechaza en vez de fusionarse.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Content Browser consume el drop OS-level

**Files:**
- Modify: `engine/include/DonTopo/Editor/EditorContext.h`
- Modify: `engine/include/DonTopo/Editor/ContentBrowserPanel.h`
- Modify: `engine/src/Editor/ContentBrowserPanel.cpp`
- Modify: `engine/tests/content_browser_tests.cpp`

**Interfaces:**
- Consumes: `DonTopo::AssetImportResult`, `DonTopo::AssetImportOutcome`, `DonTopo::isImportableExtension`, `DonTopo::importExternalAsset`, `DonTopo::describeImportResult` (Task 1).
- Produces: `struct DonTopo::DroppedFile { std::filesystem::path path; float screenX; float screenY; }`, `EditorContext::takeDroppedFiles` (`std::function<std::vector<DroppedFile>()>`), `std::vector<AssetImportOutcome> DonTopo::importDroppedFilesInto(const std::vector<DroppedFile>&, float rectX, float rectY, float rectW, float rectH, const std::filesystem::path& targetDir)` — los usa la Task 3 (`takeDroppedFiles`) y el propio panel.

- [ ] **Step 1: Añadir `DroppedFile` y `takeDroppedFiles` a `EditorContext`**

En `engine/include/DonTopo/Editor/EditorContext.h`, añadir `#include <vector>` a los includes, y justo antes de `struct EditorContext {`:

```cpp
// Fichero soltado sobre la ventana del editor desde fuera del proceso (drag
// desde el Explorador de Windows), con la posicion de pantalla en la que
// cayo. screenX/screenY son coordenadas de pantalla de ImGui (las mismas que
// ImGui::GetMousePos()/GetWindowPos()), no coordenadas de la ventana GLFW.
struct DroppedFile {
    std::filesystem::path path;
    float screenX = 0.0f;
    float screenY = 0.0f;
};
```

Y dentro de `struct EditorContext`, al final (después de `requestSaveScene`):

```cpp
    // Vacia la cola de ficheros soltados sobre la ventana este frame (drop
    // OS-level via glfwSetDropCallback, no el drag&drop interno de ImGui
    // payloads DT_ASSET_PATH). Se consume una vez: llamarlo dos veces en el
    // mismo frame devuelve vacio la segunda. Vacio/no asignado en los tests
    // headless y en runtime — solo lo rellena EditorUI::draw() a partir de
    // EditorUI::m_droppedFilesProvider (Task 3).
    std::function<std::vector<DroppedFile>()> takeDroppedFiles;
```

- [ ] **Step 2: Build (solo header, verificar que parsea)**

Run: `.\build.bat`
Expected: compila sin error.

- [ ] **Step 3: Escribir el test que falla**

En `engine/tests/content_browser_tests.cpp`, añadir el include `#include "DonTopo/Editor/AssetImport.h"` junto a los demás, y esta función antes de `int main()`:

```cpp
// importDroppedFilesInto: dentro del rect se importa, fuera se ignora en
// silencio, extension no importable se rechaza, y un conflicto de nombre no
// aborta el resto del lote (las cuatro reglas del diseño de drop externo).
static void test_import_dropped_files_into(const fs::path& root)
{
    std::error_code ec;
    fs::path externalDir = root / "external_src";
    fs::create_directories(externalDir, ec);
    fs::path destDir = root / "import_dest";
    fs::create_directories(destDir, ec);

    std::ofstream(externalDir / "nuevo.png")     << "nuevo";
    std::ofstream(externalDir / "conflicto.png") << "version-nueva";
    std::ofstream(destDir     / "conflicto.png") << "version-vieja"; // ya existe
    std::ofstream(externalDir / "notas.txt")     << "no importable";
    std::ofstream(externalDir / "lejos.png")     << "fuera del rect";

    const float rectX = 0.0f, rectY = 0.0f, rectW = 100.0f, rectH = 100.0f;
    std::vector<DroppedFile> dropped = {
        { externalDir / "nuevo.png",     50.0f, 50.0f },   // dentro
        { externalDir / "conflicto.png", 50.0f, 50.0f },   // dentro, conflicto
        { externalDir / "notas.txt",     50.0f, 50.0f },   // dentro, no importable
        { externalDir / "lejos.png",     500.0f, 500.0f }, // fuera del rect
    };

    std::vector<AssetImportOutcome> outcomes =
        importDroppedFilesInto(dropped, rectX, rectY, rectW, rectH, destDir);

    // "lejos.png" ni siquiera genera una entrada: cayo fuera del rect.
    CHECK(outcomes.size() == 3);
    if (outcomes.size() == 3)
    {
        CHECK(outcomes[0].result == AssetImportResult::Copied);
        CHECK(outcomes[0].destPath == destDir / "nuevo.png");
        CHECK(outcomes[1].result == AssetImportResult::RejectedNameConflict);
        CHECK(outcomes[2].result == AssetImportResult::RejectedExtension);
    }
    CHECK(fs::exists(destDir / "nuevo.png"));
    CHECK(!fs::exists(destDir / "lejos.png"));
    std::ifstream in(destDir / "conflicto.png");
    std::stringstream ss; ss << in.rdbuf();
    CHECK(ss.str() == "version-vieja"); // el conflicto no lo toco
}
```

Añadir `#include <sstream>` a los includes del fichero, y la llamada en `main()`, antes de `fs::remove_all(root, ec);`:

```cpp
    test_import_dropped_files_into(root);
```

- [ ] **Step 4: Compilar para verificar que falla**

Run: `.\build.bat`
Expected: FALLO — `importDroppedFilesInto`/`DroppedFile` no declarados todavía en `ContentBrowserPanel.h`.

- [ ] **Step 5: Declarar `importDroppedFilesInto` en el header**

En `engine/include/DonTopo/Editor/ContentBrowserPanel.h`:
- Sustituir `struct EditorContext;` por `#include "DonTopo/Editor/EditorContext.h"` (ya no basta forward-declarar: se necesita el tipo completo `DroppedFile`).
- Añadir `#include "DonTopo/Editor/AssetImport.h"`.
- Añadir, junto a `listVisibleSubdirs`:

```cpp
// Importa cada DroppedFile cuyo (screenX, screenY) caiga dentro del rect
// (rectX, rectY, rectW, rectH) a targetDir; los que caen fuera no generan
// ninguna entrada en el resultado (se ignoran en silencio). Un fallo
// individual (extension no importable, conflicto de nombre) no aborta el
// resto del lote. Declarada aquí, no en el anonymous namespace del .cpp,
// para que el test headless pueda enlazarla.
std::vector<AssetImportOutcome> importDroppedFilesInto(
    const std::vector<DroppedFile>& dropped,
    float rectX, float rectY, float rectW, float rectH,
    const std::filesystem::path& targetDir);
```

- [ ] **Step 6: Implementar**

En `engine/src/Editor/ContentBrowserPanel.cpp`:
- Añadir `#include "DonTopo/Editor/AssetImport.h"` junto a los demás includes.
- Dentro del `namespace { ... }` (anonymous namespace, líneas 15-160), añadir junto a los demás helpers:

```cpp
// Contencion de rect simple: borde superior/izquierdo inclusive, inferior/
// derecho exclusivo — estandar para hit-test de rects en pantalla.
bool pointInsideRect(float px, float py, float rectX, float rectY, float rectW, float rectH)
{
    return px >= rectX && px < rectX + rectW && py >= rectY && py < rectY + rectH;
}
```

- En `namespace DonTopo { ... }`, justo después de `listVisibleSubdirs`, añadir:

```cpp
std::vector<AssetImportOutcome> importDroppedFilesInto(
    const std::vector<DroppedFile>& dropped,
    float rectX, float rectY, float rectW, float rectH,
    const std::filesystem::path& targetDir)
{
    std::vector<AssetImportOutcome> out;
    for (const DroppedFile& f : dropped)
    {
        if (!pointInsideRect(f.screenX, f.screenY, rectX, rectY, rectW, rectH))
            continue;
        std::string ext = f.path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (!isImportableExtension(ext))
        {
            out.push_back({ AssetImportResult::RejectedExtension, {}, "" });
            continue;
        }
        out.push_back(importExternalAsset(f.path, targetDir));
    }
    return out;
}
```

- [ ] **Step 7: Compilar y correr el test**

Run: `.\build.bat`
Expected: build OK.

Run: `.\build-ninja\engine\tests\dt_content_browser_tests.exe`
Expected: `ALL CONTENT BROWSER TESTS PASSED`, exit code 0.

- [ ] **Step 8: Consumir el drop en `ContentBrowserPanel::draw`**

En `ContentBrowserPanel.cpp`, dentro de `draw()`, justo después de:

```cpp
    if (m_currentDir.empty())
        m_currentDir = m_projectRoot.string();
```

y antes del comentario `// Left: árbol de carpetas`, insertar:

```cpp
    // Drop OS-level (Explorer -> ventana): se consume una vez por frame, y
    // solo importa lo que cae dentro del rect de ESTA ventana — soltar sobre
    // otro panel dockeado no hace nada aquí (nadie más lo reclama).
    if (ctx.takeDroppedFiles)
    {
        const ImVec2 winPos  = ImGui::GetWindowPos();
        const ImVec2 winSize = ImGui::GetWindowSize();
        std::vector<AssetImportOutcome> outcomes = importDroppedFilesInto(
            ctx.takeDroppedFiles(), winPos.x, winPos.y, winSize.x, winSize.y, m_currentDir);
        for (const AssetImportOutcome& o : outcomes)
        {
            if (o.result == AssetImportResult::Copied)
            {
                ctx.pushLog("Asset importado: " + o.destPath.filename().string());
                m_scanned = false;
            }
            else
            {
                ctx.pushLog("Import rechazado: " + describeImportResult(o));
            }
        }
    }
```

- [ ] **Step 9: Compilar**

Run: `.\build.bat`
Expected: build OK.

- [ ] **Step 10: Commit**

```bash
git add engine/include/DonTopo/Editor/EditorContext.h \
        engine/include/DonTopo/Editor/ContentBrowserPanel.h \
        engine/src/Editor/ContentBrowserPanel.cpp \
        engine/tests/content_browser_tests.cpp
git commit -m "$(cat <<'EOF'
feat(editor): Content Browser importa ficheros soltados desde fuera del proceso

EditorContext::takeDroppedFiles + importDroppedFilesInto: cada dropped file
dentro del rect de la ventana se importa via AssetImport, uno por uno, sin
que un fallo (conflicto de nombre, extension no soportada) aborte el resto
del lote. Sin proveedor asignado (tests headless, o hasta la Task 3) la cola
llega vacia y el panel no hace nada nuevo.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Wiring extremo a extremo — `EditorUI` + `sandbox/main.cpp` (Vulkan y D3D12)

**Files:**
- Modify: `engine/include/DonTopo/Editor/EditorUI.h`
- Modify: `engine/src/Editor/EditorUI.cpp:980-1051` (construcción de `ctx`)
- Modify: `sandbox/src/main.cpp:258-264` (rama D3D12), `:302` (rama D3D12), `:844-893` (rama Vulkan), `:903` (rama Vulkan)

**Interfaces:**
- Consumes: `DonTopo::DroppedFile`, `EditorContext::takeDroppedFiles` (Task 2).
- Produces: `EditorUI::setDroppedFilesProvider(std::function<std::vector<DroppedFile>()>)` — no lo consume ninguna tarea posterior (última tarea de wiring).

Esta tarea es pura plumbing (registrar un callback GLFW y conectarlo al `EditorContext`); no hay lógica nueva que testear headless — igual que el resto de callbacks de `main.cpp`, se verifica a mano en el Step final.

- [ ] **Step 1: `EditorUI::setDroppedFilesProvider`**

En `engine/include/DonTopo/Editor/EditorUI.h`, añadir `#include "DonTopo/Editor/EditorContext.h"` junto a los demás includes de paneles, y en la sección pública, junto a `setAssetLoader`:

```cpp
    // Lo rellena main() antes del bucle: drena la cola de ficheros soltados
    // sobre la ventana desde fuera del proceso (glfwSetDropCallback). Vacío
    // por defecto — sin proveedor, ctx.takeDroppedFiles llega vacía al
    // Content Browser y no se importa nada, mismo patrón que setAssetLoader.
    void setDroppedFilesProvider(std::function<std::vector<DroppedFile>()> fn)
    { m_droppedFilesProvider = std::move(fn); }
```

Y en la sección privada, junto a `m_assetLoader`:

```cpp
    // Ver setDroppedFilesProvider. Vacío en tests headless.
    std::function<std::vector<DroppedFile>()> m_droppedFilesProvider;
```

- [ ] **Step 2: Build**

Run: `.\build.bat`
Expected: compila sin error (el miembro nuevo no lo usa nadie todavía).

- [ ] **Step 3: Conectar al `EditorContext` de `EditorUI::draw`**

En `engine/src/Editor/EditorUI.cpp`, dentro del bloque `EditorContext ctx{ ... };` (empieza en la línea 980), añadir una línea nueva justo antes del `};` de cierre (línea 1051), después de la lambda de `requestSaveScene`:

```cpp
        m_droppedFilesProvider,
    };
```

(sustituye la línea `};` sola que cierra hoy el bloque — el resto de las líneas 980-1050 no cambian).

- [ ] **Step 4: Build**

Run: `.\build.bat`
Expected: build OK.

- [ ] **Step 5: Registrar el drop callback — rama Vulkan**

En `sandbox/src/main.cpp`, sustituir (líneas 844-846):

```cpp
        struct AppCtx { DonTopo::Camera* cam; DonTopo::Renderer* rnd; DonTopo::EditorUI* ed; };
        AppCtx ctx{ &camera, &renderer, &editor };
        glfwSetWindowUserPointer(window.getNativeWindow(), &ctx);
```

por:

```cpp
        struct AppCtx {
            DonTopo::Camera* cam;
            DonTopo::Renderer* rnd;
            DonTopo::EditorUI* ed;
            std::vector<DonTopo::DroppedFile> drops;
        };
        AppCtx ctx{ &camera, &renderer, &editor, {} };
        glfwSetWindowUserPointer(window.getNativeWindow(), &ctx);
```

Después del bloque `glfwSetKeyCallback(...)` que termina en la línea 893 (`});`), añadir:

```cpp

        glfwSetDropCallback(window.getNativeWindow(), [](GLFWwindow* w, int count, const char** paths) {
            auto* ctx = static_cast<AppCtx*>(glfwGetWindowUserPointer(w));
            double x, y;
            glfwGetCursorPos(w, &x, &y);
            for (int i = 0; i < count; ++i)
                ctx->drops.push_back({ std::filesystem::path(paths[i]), (float)x, (float)y });
        });
```

Y justo después de `editor.setAssetLoader(&assetLoader);` (línea 903), añadir:

```cpp
        editor.setDroppedFilesProvider([&ctx]() {
            std::vector<DonTopo::DroppedFile> out;
            out.swap(ctx.drops);
            return out;
        });
```

- [ ] **Step 6: Registrar el drop callback — rama D3D12**

En `sandbox/src/main.cpp`, sustituir (líneas 258-264):

```cpp
            glfwSetWindowUserPointer(window.getNativeWindow(), &d3d12);
            glfwSetFramebufferSizeCallback(
                window.getNativeWindow(), [](GLFWwindow* w, int width, int height) {
                    auto* r = static_cast<DonTopo::D3D12::D3D12Renderer*>(glfwGetWindowUserPointer(w));
                    if (r)
                        r->resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
                });
```

por:

```cpp
            // Antes el user pointer era &d3d12 a secas: el drop de ficheros
            // externos necesita una cola propia además del renderer, así que
            // gana un struct pequeño en vez de un puntero suelto — mismo
            // patrón que el AppCtx del camino de Vulkan.
            struct D3D12WindowCtx {
                DonTopo::D3D12::D3D12Renderer* renderer;
                std::vector<DonTopo::DroppedFile> drops;
            };
            D3D12WindowCtx d3dWindowCtx{ &d3d12, {} };
            glfwSetWindowUserPointer(window.getNativeWindow(), &d3dWindowCtx);
            glfwSetFramebufferSizeCallback(
                window.getNativeWindow(), [](GLFWwindow* w, int width, int height) {
                    auto* c = static_cast<D3D12WindowCtx*>(glfwGetWindowUserPointer(w));
                    if (c->renderer)
                        c->renderer->resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
                });
            glfwSetDropCallback(window.getNativeWindow(), [](GLFWwindow* w, int count, const char** paths) {
                auto* c = static_cast<D3D12WindowCtx*>(glfwGetWindowUserPointer(w));
                double x, y;
                glfwGetCursorPos(w, &x, &y);
                for (int i = 0; i < count; ++i)
                    c->drops.push_back({ std::filesystem::path(paths[i]), (float)x, (float)y });
            });
```

Y justo después de `editor.setAssetLoader(&d3dAssets);` (línea 302), añadir:

```cpp
            editor.setDroppedFilesProvider([&d3dWindowCtx]() {
                std::vector<DonTopo::DroppedFile> out;
                out.swap(d3dWindowCtx.drops);
                return out;
            });
```

- [ ] **Step 7: Build**

Run: `.\build.bat`
Expected: build OK en los dos backends (el proyecto compila ambos en el mismo build).

- [ ] **Step 8: Verificación manual — Vulkan**

Run: `.\build-ninja\sandbox\Sandbox.exe`

1. Abrir o crear un proyecto (arranca en Vulkan por defecto).
2. Arrastrar un `.png` desde el Explorador de Windows sobre la ventana del Content Browser.
3. Esperado: el fichero aparece copiado bajo `assets/Imported/Textures/` dentro del proyecto, visible en el grid tras el import, y el Log Console muestra "Asset importado: <nombre>".
4. Repetir soltando sobre el Viewport en vez del Content Browser: esperado, no pasa nada (ni log, ni copia).
5. Arrastrar el MISMO fichero otra vez sobre el Content Browser: esperado, log "Import rechazado: ya existe un fichero con ese nombre en el destino", el fichero original no cambia.

- [ ] **Step 9: Verificación manual — D3D12**

Cambiar el backend a D3D12 desde el menú View (o `project.json`) y reiniciar el Sandbox. Repetir los 5 puntos del Step 8.

- [ ] **Step 10: Commit**

```bash
git add engine/include/DonTopo/Editor/EditorUI.h engine/src/Editor/EditorUI.cpp sandbox/src/main.cpp
git commit -m "$(cat <<'EOF'
feat(editor): drop de ficheros externos sobre la ventana, Vulkan y D3D12

glfwSetDropCallback en las dos ramas de sandbox/main.cpp encola (path,
posicion del cursor) en una cola local a cada backend; EditorUI la drena por
frame via setDroppedFilesProvider y la cuelga de EditorContext::takeDroppedFiles,
que el Content Browser ya sabe consumir (tarea anterior).

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Unificación de los 18 diálogos "Browse..." de Properties

**Files:**
- Modify: `engine/src/Editor/PropertiesPanel.cpp:1-40` (includes), `:75-99` (`canAcceptAsset`), y los 18 call-sites listados en el Step 2.

**Interfaces:**
- Consumes: `DonTopo::AssetImportResult`, `DonTopo::isImportableExtension`, `DonTopo::importedAssetDestDir`, `DonTopo::importExternalAsset`, `DonTopo::describeImportResult` (Task 1). Independiente de las Tasks 2 y 3 (no toca el Content Browser ni el drop OS-level).
- Produces: nada que consuman tareas posteriores (última tarea del plan).

Sin test nuevo: la lógica de copia ya está probada en la Task 1 (`importExternalAsset`); lo que cambia aquí es plumbing de UI — `canAcceptAsset` es una función de anonymous namespace local a este fichero, no exportable a un test headless sin construir un `EditorContext` completo (physics/renderer/scene/scriptManager), y el propio repo ya trata este tipo de guardas de UI como "verificado a mano" (ver A11 en `docs/animation-audit.md`). Los dos casos del Review Focus que le tocan (`ctx.project == nullptr`, `ctx.editingLocked == true`) se verifican leyendo el diff del Step 3: el orden de los `if` no cambia respecto al original, solo lo que devuelve la última rama.

- [ ] **Step 1: Include nuevo**

En `engine/src/Editor/PropertiesPanel.cpp`, añadir junto a los demás includes (línea 1-40):

```cpp
#include "DonTopo/Editor/AssetImport.h"
```

Y `#include <optional>` junto a `<algorithm>`/`<cctype>` si no está ya (comprobar con `git grep -n "include <optional>" engine/src/Editor/PropertiesPanel.cpp` antes de añadirlo).

- [ ] **Step 2: Cambiar la firma y el cuerpo de `canAcceptAsset`**

Sustituir (líneas 75-99):

```cpp
bool canAcceptAsset(const DonTopo::EditorContext& ctx, const std::filesystem::path& path)
{
    // Veto mientras el modal de Load Scene está activo: la escena sobre la que
    // se abrió el diálogo está siendo reemplazada, así que aplicar la elección
    // escribiría en un objeto que ya no es el que el usuario tenía delante. Con
    // línea en el Log, que es lo único que distingue esto de "el Browse no ha
    // hecho nada".
    if (ctx.editingLocked)
    {
        ctx.logModule("Project", "Carga de escena en curso: el asset elegido se descarta");
        return false;
    }

    if (!ctx.project || !ctx.project->valid()) return true;
    if (ctx.project->contains(path))           return true;

    // El workspace lo crea el selector al arrancar, así que este contains()
    // responde sobre una carpeta que existe; si aun así fallara, contains()
    // devuelve false y el asset se trata como compartido, no como ajeno.
    const DonTopo::ProjectContext workspace(DonTopo::ProjectContext::workspaceDir());
    if (!workspace.contains(path)) return true;

    ctx.logModule("Project", "Asset de otro proyecto, rechazado: " + path.string());
    return false;
}
```

por:

```cpp
std::optional<std::filesystem::path> canAcceptAsset(const DonTopo::EditorContext& ctx,
                                                     const std::filesystem::path& path)
{
    // Veto mientras el modal de Load Scene está activo: la escena sobre la que
    // se abrió el diálogo está siendo reemplazada, así que aplicar la elección
    // escribiría en un objeto que ya no es el que el usuario tenía delante. Con
    // línea en el Log, que es lo único que distingue esto de "el Browse no ha
    // hecho nada".
    if (ctx.editingLocked)
    {
        ctx.logModule("Project", "Carga de escena en curso: el asset elegido se descarta");
        return std::nullopt;
    }

    if (!ctx.project || !ctx.project->valid()) return path;
    if (ctx.project->contains(path))           return path;

    // El workspace lo crea el selector al arrancar, así que este contains()
    // responde sobre una carpeta que existe; si aun así fallara, contains()
    // devuelve false y el asset se trata como compartido, no como ajeno.
    const DonTopo::ProjectContext workspace(DonTopo::ProjectContext::workspaceDir());
    if (!workspace.contains(path)) return path;

    // Asset fuera del proyecto y del workspace compartido: antes se
    // rechazaba sin más. Ahora, si la extensión es de las que el editor ya
    // sabe importar, se copia al proyecto en vez de descartarse — mismo
    // mecanismo que el drop externo sobre el Content Browser.
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (!DonTopo::isImportableExtension(ext))
    {
        ctx.logModule("Project", "Asset de otro proyecto, rechazado: " + path.string());
        return std::nullopt;
    }

    const std::filesystem::path destDir = DonTopo::importedAssetDestDir(ctx.project->root(), ext);
    const DonTopo::AssetImportOutcome outcome = DonTopo::importExternalAsset(path, destDir);
    if (outcome.result != DonTopo::AssetImportResult::Copied)
    {
        ctx.logModule("Project", "Import rechazado (" + path.filename().string() + "): " +
                      DonTopo::describeImportResult(outcome));
        return std::nullopt;
    }
    ctx.logModule("Project", "Asset importado: " + outcome.destPath.filename().string());
    return outcome.destPath;
}
```

- [ ] **Step 3: Compilar para ver los 18 errores**

Run: `.\build.bat`
Expected: FALLO — 18 errores de conversión `bool`/`std::optional<std::filesystem::path>` en los `if (...->IsOk() && canAcceptAsset(...))`, uno por diálogo. Cada error apunta a una de las líneas de la tabla del Step 4.

- [ ] **Step 4: Reescribir los 18 call-sites**

Mismo patrón en los 18: `if (X->IsOk() && canAcceptAsset(ctx, X->GetFilePathName())) F(...);` pasa a resolver una sola vez y usar `resolved->string()`. Tabla con el `X`/`F` exactos de cada uno (todos en `engine/src/Editor/PropertiesPanel.cpp`):

| Diálogo (`X`) | Antes | Después |
|---|---|---|
| `m_fontFileDialog` | `if (m_fontFileDialog->IsOk() && canAcceptAsset(ctx, m_fontFileDialog->GetFilePathName())) setButtonAssetPath(ctx, m_fontDlgOwner, /*isFont=*/true, m_fontFileDialog->GetFilePathName());` | `if (m_fontFileDialog->IsOk()) { if (auto resolved = canAcceptAsset(ctx, m_fontFileDialog->GetFilePathName())) setButtonAssetPath(ctx, m_fontDlgOwner, /*isFont=*/true, resolved->string()); }` |
| `m_uiAtlasFileDialog` | `if (m_uiAtlasFileDialog->IsOk() && canAcceptAsset(ctx, m_uiAtlasFileDialog->GetFilePathName())) setButtonAssetPath(ctx, m_uiAtlasDlgOwner, /*isFont=*/false, m_uiAtlasFileDialog->GetFilePathName());` | `if (m_uiAtlasFileDialog->IsOk()) { if (auto resolved = canAcceptAsset(ctx, m_uiAtlasFileDialog->GetFilePathName())) setButtonAssetPath(ctx, m_uiAtlasDlgOwner, /*isFont=*/false, resolved->string()); }` |
| `m_textFontFileDialog` | `if (m_textFontFileDialog->IsOk() && canAcceptAsset(ctx, m_textFontFileDialog->GetFilePathName())) setTextFontPath(ctx, m_textFontDlgOwner, m_textFontFileDialog->GetFilePathName());` | `if (m_textFontFileDialog->IsOk()) { if (auto resolved = canAcceptAsset(ctx, m_textFontFileDialog->GetFilePathName())) setTextFontPath(ctx, m_textFontDlgOwner, resolved->string()); }` |
| `m_barAtlasFileDialog` | `if (m_barAtlasFileDialog->IsOk() && canAcceptAsset(ctx, m_barAtlasFileDialog->GetFilePathName())) setProgressBarImagePath(ctx, m_barAtlasDlgOwner, m_barAtlasDlgField, m_barAtlasFileDialog->GetFilePathName());` | `if (m_barAtlasFileDialog->IsOk()) { if (auto resolved = canAcceptAsset(ctx, m_barAtlasFileDialog->GetFilePathName())) setProgressBarImagePath(ctx, m_barAtlasDlgOwner, m_barAtlasDlgField, resolved->string()); }` |
| `m_panelAtlasFileDialog` | `if (m_panelAtlasFileDialog->IsOk() && canAcceptAsset(ctx, m_panelAtlasFileDialog->GetFilePathName())) setPanelAtlasPath(ctx, m_panelAtlasDlgOwner, m_panelAtlasFileDialog->GetFilePathName());` | `if (m_panelAtlasFileDialog->IsOk()) { if (auto resolved = canAcceptAsset(ctx, m_panelAtlasFileDialog->GetFilePathName())) setPanelAtlasPath(ctx, m_panelAtlasDlgOwner, resolved->string()); }` |
| `m_imageAtlasFileDialog` | `if (m_imageAtlasFileDialog->IsOk() && canAcceptAsset(ctx, m_imageAtlasFileDialog->GetFilePathName())) setImageAtlasPath(ctx, m_imageAtlasDlgOwner, m_imageAtlasFileDialog->GetFilePathName());` | `if (m_imageAtlasFileDialog->IsOk()) { if (auto resolved = canAcceptAsset(ctx, m_imageAtlasFileDialog->GetFilePathName())) setImageAtlasPath(ctx, m_imageAtlasDlgOwner, resolved->string()); }` |
| `m_sliderAtlasFileDialog` | `if (m_sliderAtlasFileDialog->IsOk() && canAcceptAsset(ctx, m_sliderAtlasFileDialog->GetFilePathName())) setSliderAtlasPath(ctx, m_sliderAtlasDlgOwner, m_sliderAtlasFileDialog->GetFilePathName());` | `if (m_sliderAtlasFileDialog->IsOk()) { if (auto resolved = canAcceptAsset(ctx, m_sliderAtlasFileDialog->GetFilePathName())) setSliderAtlasPath(ctx, m_sliderAtlasDlgOwner, resolved->string()); }` |
| `m_checkboxAtlasFileDialog` | `if (m_checkboxAtlasFileDialog->IsOk() && canAcceptAsset(ctx, m_checkboxAtlasFileDialog->GetFilePathName())) setCheckboxAtlasPath(ctx, m_checkboxAtlasDlgOwner, m_checkboxAtlasFileDialog->GetFilePathName());` | `if (m_checkboxAtlasFileDialog->IsOk()) { if (auto resolved = canAcceptAsset(ctx, m_checkboxAtlasFileDialog->GetFilePathName())) setCheckboxAtlasPath(ctx, m_checkboxAtlasDlgOwner, resolved->string()); }` |
| `m_toggleAtlasFileDialog` | `if (m_toggleAtlasFileDialog->IsOk() && canAcceptAsset(ctx, m_toggleAtlasFileDialog->GetFilePathName())) setToggleAtlasPath(ctx, m_toggleAtlasDlgOwner, m_toggleAtlasFileDialog->GetFilePathName());` | `if (m_toggleAtlasFileDialog->IsOk()) { if (auto resolved = canAcceptAsset(ctx, m_toggleAtlasFileDialog->GetFilePathName())) setToggleAtlasPath(ctx, m_toggleAtlasDlgOwner, resolved->string()); }` |
| `m_scrollbarAtlasFileDialog` | `if (m_scrollbarAtlasFileDialog->IsOk() && canAcceptAsset(ctx, m_scrollbarAtlasFileDialog->GetFilePathName())) setScrollbarAtlasPath(ctx, m_scrollbarAtlasDlgOwner, m_scrollbarAtlasFileDialog->GetFilePathName());` | `if (m_scrollbarAtlasFileDialog->IsOk()) { if (auto resolved = canAcceptAsset(ctx, m_scrollbarAtlasFileDialog->GetFilePathName())) setScrollbarAtlasPath(ctx, m_scrollbarAtlasDlgOwner, resolved->string()); }` |
| `m_inputFieldAtlasFileDialog` | `if (m_inputFieldAtlasFileDialog->IsOk() && canAcceptAsset(ctx, m_inputFieldAtlasFileDialog->GetFilePathName())) setInputFieldAtlasPath(ctx, m_inputFieldAtlasDlgOwner, m_inputFieldAtlasFileDialog->GetFilePathName());` | `if (m_inputFieldAtlasFileDialog->IsOk()) { if (auto resolved = canAcceptAsset(ctx, m_inputFieldAtlasFileDialog->GetFilePathName())) setInputFieldAtlasPath(ctx, m_inputFieldAtlasDlgOwner, resolved->string()); }` |
| `m_inputFieldFontFileDialog` | `if (m_inputFieldFontFileDialog->IsOk() && canAcceptAsset(ctx, m_inputFieldFontFileDialog->GetFilePathName())) setInputFieldFontPath(ctx, m_inputFieldFontDlgOwner, m_inputFieldFontFileDialog->GetFilePathName());` | `if (m_inputFieldFontFileDialog->IsOk()) { if (auto resolved = canAcceptAsset(ctx, m_inputFieldFontFileDialog->GetFilePathName())) setInputFieldFontPath(ctx, m_inputFieldFontDlgOwner, resolved->string()); }` |
| `m_dropdownAtlasFileDialog` | `if (m_dropdownAtlasFileDialog->IsOk() && canAcceptAsset(ctx, m_dropdownAtlasFileDialog->GetFilePathName())) setDropdownAtlasPath(ctx, m_dropdownAtlasDlgOwner, m_dropdownAtlasFileDialog->GetFilePathName());` | `if (m_dropdownAtlasFileDialog->IsOk()) { if (auto resolved = canAcceptAsset(ctx, m_dropdownAtlasFileDialog->GetFilePathName())) setDropdownAtlasPath(ctx, m_dropdownAtlasDlgOwner, resolved->string()); }` |
| `m_dropdownFontFileDialog` | `if (m_dropdownFontFileDialog->IsOk() && canAcceptAsset(ctx, m_dropdownFontFileDialog->GetFilePathName())) setDropdownFontPath(ctx, m_dropdownFontDlgOwner, m_dropdownFontFileDialog->GetFilePathName());` | `if (m_dropdownFontFileDialog->IsOk()) { if (auto resolved = canAcceptAsset(ctx, m_dropdownFontFileDialog->GetFilePathName())) setDropdownFontPath(ctx, m_dropdownFontDlgOwner, resolved->string()); }` |
| `m_scrollViewAtlasFileDialog` | `if (m_scrollViewAtlasFileDialog->IsOk() && canAcceptAsset(ctx, m_scrollViewAtlasFileDialog->GetFilePathName())) setScrollViewAtlasPath(ctx, m_scrollViewAtlasDlgOwner, m_scrollViewAtlasFileDialog->GetFilePathName());` | `if (m_scrollViewAtlasFileDialog->IsOk()) { if (auto resolved = canAcceptAsset(ctx, m_scrollViewAtlasFileDialog->GetFilePathName())) setScrollViewAtlasPath(ctx, m_scrollViewAtlasDlgOwner, resolved->string()); }` |
| `m_meshFileDialog` | `if (m_meshFileDialog->IsOk() && canAcceptAsset(ctx, m_meshFileDialog->GetFilePathName())) loadMeshForSelected(ctx, m_meshDlgOwner, m_meshFileDialog->GetFilePathName());` | `if (m_meshFileDialog->IsOk()) { if (auto resolved = canAcceptAsset(ctx, m_meshFileDialog->GetFilePathName())) loadMeshForSelected(ctx, m_meshDlgOwner, resolved->string()); }` |
| `m_textureFileDialog` | `if (m_textureFileDialog->IsOk() && canAcceptAsset(ctx, m_textureFileDialog->GetFilePathName())) assignMaterialTexture(ctx, m_textureDlgOwner, m_textureDlgMaterial, m_textureDlgSlot, m_textureFileDialog->GetFilePathName());` | `if (m_textureFileDialog->IsOk()) { if (auto resolved = canAcceptAsset(ctx, m_textureFileDialog->GetFilePathName())) assignMaterialTexture(ctx, m_textureDlgOwner, m_textureDlgMaterial, m_textureDlgSlot, resolved->string()); }` |
| `m_audioFileDialog` | `if (m_audioFileDialog->IsOk() && canAcceptAsset(ctx, m_audioFileDialog->GetFilePathName())) loadAudioClipForSelected(ctx, m_audioFileDialog->GetFilePathName());` | `if (m_audioFileDialog->IsOk()) { if (auto resolved = canAcceptAsset(ctx, m_audioFileDialog->GetFilePathName())) loadAudioClipForSelected(ctx, resolved->string()); }` |

Aplicar los 18 reemplazos exactos de la tabla (`Antes` → `Después`) en sus 18 ubicaciones dentro de `PropertiesPanel.cpp` (usar `git grep -n "canAcceptAsset(ctx,"` para localizarlas todas antes de empezar, y volver a correrlo después para confirmar que las 18 siguen ahí — la sustitución no borra ninguna).

- [ ] **Step 5: Compilar**

Run: `.\build.bat`
Expected: build OK, cero errores de conversión.

Run: `git grep -c "canAcceptAsset(ctx," engine/src/Editor/PropertiesPanel.cpp`
Expected: `18` (mismo recuento que antes de tocar nada — ninguna llamada se perdió).

- [ ] **Step 6: Correr toda la suite existente**

Run: `.\build-ninja\engine\tests\dt_content_browser_tests.exe`
Run: `.\build-ninja\engine\tests\dt_asset_import_tests.exe`
Expected: los dos, `ALL ... TESTS PASSED`, exit code 0.

- [ ] **Step 7: Verificación manual**

Run: `.\build-ninja\sandbox\Sandbox.exe`

1. Con un proyecto abierto, seleccionar un GameObject, Properties → Add Mesh → Browse..., navegar a un `.fbx` que esté FUERA de la carpeta del proyecto (p. ej. en el Escritorio) y aceptarlo.
2. Esperado: el mesh se carga sobre el GameObject, el Log muestra "Asset importado: <nombre>.fbx", y el fichero aparece copiado en `assets/Imported/Meshes/` dentro del proyecto (visible en el Content Browser).
3. Repetir el mismo `.fbx` una segunda vez desde el mismo Browse externo: esperado, Log "Import rechazado (<nombre>.fbx): ya existe un fichero con ese nombre en el destino", y el mesh NO se recarga (la sección Mesh no cambia).
4. Con la escena teniendo cambios sin guardar, abrir Load Scene (deja `ctx.editingLocked = true` mientras el modal está en vuelo) y, sin cerrarlo, intentar Add Audio Clip → Browse... con un fichero externo: esperado, Log "Carga de escena en curso: el asset elegido se descarta" — el veto de edición bloqueada sigue ganando antes de intentar copiar nada.

- [ ] **Step 8: Commit**

```bash
git add engine/src/Editor/PropertiesPanel.cpp
git commit -m "$(cat <<'EOF'
feat(editor): los 18 dialogos Browse... de Properties importan ficheros externos

canAcceptAsset pasa de bool a std::optional<std::filesystem::path>: un asset
fuera del proyecto y del workspace compartido, si su extension es de las
importables, se copia a assets/Imported/<Tipo>/ en vez de rechazarse sin mas.
Mismo AssetImport que ya usa el drop del Content Browser.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```
