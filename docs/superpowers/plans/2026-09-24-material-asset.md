# Material como asset independiente (.mat) — Plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Un `.mat` es un fichero del proyecto que varios objetos referencian por slot; editarlo actualiza a todos sus usuarios, y las overrides del objeto siguen mandando encima.

**Architecture:** `Core/MaterialAsset` añade el formato y la lectura/escritura tolerante. `MaterialOverride` gana un campo `matAsset`; `applyMaterialOverrides` calcula, por slot, un valor "efectivo" (override del objeto > `.mat` > modelo) y alimenta el mismo mecanismo de baseline que ya existe, sin tocarlo. Encima de esa capa: serialización de escena, `discardOverriddenDecodedImages`, un modal de edición en el Content Browser, un comando de asignación en Properties, ciclo de vida por ruta y el exportador — cada uno un cambio pequeño y aislado sobre un mecanismo existente.

**Tech Stack:** C++20, nlohmann_json, ImGui + ImGuiFileDialog, Vulkan/D3D12 (solo se llama a `rebuildStaticMesh`/`rebuildSkinnedMesh`, ya existentes; ninguna tarea toca su interior). Tests planos `main()` + `CHECK`, `dt_add_test`, ejecutados desde la raíz del repo.

**Spec:** `docs/superpowers/specs/2026-09-24-material-asset-design.md`

## Global Constraints

- **Sin dependencias de terceros nuevas.**
- **Un campo ausente o vacío en el `.mat` significa "heredar del modelo"**: `""` para las tres texturas, `-1.0f` para `metallic`/`roughness` (mismo centinela que `MaterialOverride` ya usa).
- **Guardar un `.mat` SIEMPRE escribe el fichero**, aunque todo sea "heredar" (a diferencia del sidecar de importación, que borra en el defecto).
- **Rutas de textura dentro del `.mat`, relativas a la carpeta del propio `.mat`**; en memoria (`MaterialAsset::albedo/normal/orm`) son siempre absolutas.
- **Lectura tolerante, nunca lanza**: ausente (sin aviso especial, ver "avisos" abajo), vacío, JSON roto, > 64 KiB, `version` ≠ 1, `type` ≠ `material`, campo de tipo equivocado → ese campo hereda. `metallic`/`roughness` se acotan a [0, 1]; no finitos → heredar.
- **Referencia viva por slot**: `override del objeto > valor del .mat > modelo`. "Clear" del objeto (override vacío) cae al `.mat`; desvincular el `.mat` (`matAsset = ""`) cae al modelo. Sin `matAsset`, resultado idéntico a hoy — ningún test existente puede cambiar de resultado.
- **Avisos**: un `.mat` inexistente o ilegible es tolerado en silencio en el camino caliente (`applyMaterialOverrides`, que no tiene canal de log y corre en cada clon — presupuesto medido de 24,5 ms/clon, ver `GameObject.cpp`); el aviso vive en `collectMaterialOverrideWarnings`, que solo se llama al cargar una escena (mismo patrón que el resto de avisos de `Scene::fromJson`).
- **CRLF**: el repo va en CRLF. Tras cada `Edit`, comprobar `git diff --stat`. No usar `sed -i` ni Get-Content/Set-Content en PowerShell.
- **Los tests se ejecutan desde la raíz del repo** (`.\build-ninja\engine\tests\dt_xxx.exe`).
- **Build**: `.\build.bat > $env:TEMP\b.log 2>&1` desde PowerShell; leer solo la cola/los errores.
- **Suite completa**: ~3-5 min (`dt_asset_loader_tests` solo ~2m30, no es un cuelgue). Correrla con el script de `[Diagnostics.Process]::Start` (ver "Cómo correr la suite"), en segundo plano. Para `task-done`, usar los tests dirigidos de la tarea; ledgerar la suite completa aparte.
- **Ninguna tarea toca GPU directamente**: las que llaman a `rebuildStaticMesh`/`rebuildSkinnedMesh` lo hacen a través de la misma `EditorRenderer*` (posiblemente `nullptr` en tests, como ya hace `MaterialTextureCommand`).

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
```

Guardarlo en el scratchpad de la sesión y lanzarlo con `run_in_background`. Hoy: `total 33`; con este plan, `total 34` (un fichero de test nuevo, Task 1); `fallos:` vacío.

## Review Focus

1. **Un `.mat` inexistente o con JSON roto en un objeto ya cargado**: el objeto se dibuja con su modelo (heredar todo), sin lanzar y sin romper la carga de la escena. → Task 2.
2. **Dos objetos que comparten el mismo `.mat`**: cambiarlo desde el modal actualiza a los DOS, no solo al primero. → Task 6.
3. **Un `.mat` con solo `roughness` puesto** deja intactas las tres texturas del modelo (no las vacía). → Task 2.
4. **Renombrar o mover un `.mat` en uso** actualiza `matAsset` en la escena; uno que no lo usa no se toca. → Task 8.
5. **Borrar un `.mat` en uso**: el diálogo de borrado lo cuenta, y tras confirmar el objeto vuelve a su modelo (no se queda con la última textura resuelta). → Task 8.
6. **Una entrada de escena con SOLO `matAsset`** (sin texturas ni factores propios) no se omite al guardar, y una escena vieja sin el campo carga igual que hoy. → Task 3.

---

## File Structure

| Fichero | Responsabilidad |
|---|---|
| `engine/include/DonTopo/Core/MaterialAsset.h` / `engine/src/Core/MaterialAsset.cpp` (crear) | Tipo `MaterialAsset`, lectura/escritura tolerante, rutas relativas a la carpeta del `.mat`. |
| `engine/include/DonTopo/Core/GameObject.h` (modificar) | `MaterialOverride::matAsset`. |
| `engine/src/Core/GameObject.cpp` (modificar) | `applyMaterialOverrides` resuelve el valor efectivo por slot; `collectMaterialOverrideWarnings` avisa de un `.mat` inválido. |
| `engine/src/Core/Scene.cpp` (modificar) | Serialización de `matAsset` en `mesh.materials`. |
| `engine/include/DonTopo/Renderer/AsyncAssetLoader.h` / `.cpp` (modificar) | `discardOverriddenDecodedImages` considera el `.mat`. |
| `engine/include/DonTopo/Editor/ContentBrowserPanel.h` / `engine/src/Editor/ContentBrowserPanel.cpp` (modificar) | `AssetKind::Material`, `classifyAsset`, filtro, icono, Create → Material, `applyMaterialAssetSettings`, modal de edición, ciclo de vida (rename/delete). |
| `engine/include/DonTopo/Editor/Command.h` / `engine/src/Editor/Command.cpp` (modificar) | `setMaterialAssetOverride`, `MaterialAssetCommand`. |
| `engine/include/DonTopo/Editor/PropertiesPanel.h` / `engine/src/Editor/PropertiesPanel.cpp` (modificar) | Fila "Material asset" por slot: drop/Browse/Clear, `assignMaterialAsset`. |
| `engine/src/Editor/GameExporter.cpp` (modificar) | `collectSceneAssets` añade el `.mat`; `rewriteNode` reescribe `matAsset`. |
| `engine/tests/material_asset_tests.cpp` (crear); `material_texture_tests.cpp`, `content_browser_tests.cpp`, `exporter_tests.cpp` (ampliar) | Tests. |
| `engine/CMakeLists.txt`, `engine/tests/CMakeLists.txt` (modificar) | Registro de fuentes y del test nuevo. |
| `docs/assets-editor-audit.md`, `README.md` (modificar) | Cierre del punto del audit. |

Un commit por tarea.

---

### Task 1: `Core/MaterialAsset` — formato y lectura/escritura tolerante

**Files:**
- Create: `engine/include/DonTopo/Core/MaterialAsset.h`
- Create: `engine/src/Core/MaterialAsset.cpp`
- Create: `engine/tests/material_asset_tests.cpp`
- Modify: `engine/CMakeLists.txt` (añadir `src/Core/MaterialAsset.cpp` junto a `src/Core/ImportSettings.cpp`)
- Modify: `engine/tests/CMakeLists.txt` (añadir `dt_add_test(dt_material_asset_tests material_asset_tests.cpp DonTopoCore)` tras `dt_import_settings_tests`)

**Interfaces:**
- Produces (namespace `DonTopo`; lo usan las tareas 2, 4, 5, 6, 7, 9):
  ```cpp
  struct MaterialAsset {
      std::string albedo, normal, orm;   // absolutas en memoria; "" = heredar del modelo
      float metallic  = -1.0f;           // -1 = heredar; si no, [0, 1]
      float roughness = -1.0f;
  };
  inline bool operator==(const MaterialAsset& a, const MaterialAsset& b);
  inline bool isDefault(const MaterialAsset& m);   // las tres rutas vacías y los dos factores en -1

  MaterialAsset loadMaterialAsset(const std::filesystem::path& mat, std::string* warning = nullptr);
  bool saveMaterialAsset(const std::filesystem::path& mat, const MaterialAsset&, std::string* error = nullptr);
  ```

- [ ] **Step 1: Write the failing tests**

```cpp
// Test headless de MaterialAsset (sin GUI, sin GPU). Plain main + CHECK, mismo
// patron que import_settings_tests.cpp. Se ejecuta desde la raiz del repo.
#include "DonTopo/Core/MaterialAsset.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>

using namespace DonTopo;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static fs::path makeDir()
{
    std::error_code ec;
    fs::path d = fs::temp_directory_path(ec) / "dt_material_asset_test";
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}
static void writeText(const fs::path& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; }

static void test_default_is_all_inherit()
{
    CHECK(isDefault(MaterialAsset{}));
    MaterialAsset m; m.albedo = "x";
    CHECK(!isDefault(m));
}

static void test_missing_file_is_default_without_warning()
{
    const fs::path d = makeDir();
    std::string warning = "x";
    CHECK(isDefault(loadMaterialAsset(d / "no_existe.mat", &warning)));
    CHECK(warning.empty());
}

static void test_roundtrip_always_writes_the_file()
{
    const fs::path d = makeDir();
    const fs::path mat = d / "rojo.mat";
    std::string err;

    // Guardar el defecto (todo heredar) SI escribe el fichero: a diferencia del
    // sidecar de importacion, un .mat es un asset con nombre.
    CHECK(saveMaterialAsset(mat, MaterialAsset{}, &err));
    CHECK(fs::exists(mat));
    CHECK(isDefault(loadMaterialAsset(mat)));

    MaterialAsset in;
    in.roughness = 0.25f;
    CHECK(saveMaterialAsset(mat, in, &err));
    const MaterialAsset out = loadMaterialAsset(mat);
    CHECK(out.roughness == 0.25f);
    CHECK(out.albedo.empty() && out.normal.empty() && out.orm.empty());
    CHECK(out.metallic == -1.0f);         // solo roughness estaba puesto
}

static void test_broken_and_wrong_type_are_default_with_warning()
{
    const fs::path d = makeDir();
    writeText(d / "roto.mat", "{ esto no es json");
    std::string warning;
    CHECK(isDefault(loadMaterialAsset(d / "roto.mat", &warning)));
    CHECK(!warning.empty());

    writeText(d / "textura.mat", R"({"version":1,"type":"texture","metallic":0.5})");
    warning.clear();
    CHECK(isDefault(loadMaterialAsset(d / "textura.mat", &warning)));
    CHECK(!warning.empty());

    writeText(d / "v2.mat", R"({"version":2,"type":"material","metallic":0.5})");
    warning.clear();
    CHECK(isDefault(loadMaterialAsset(d / "v2.mat", &warning)));
    CHECK(!warning.empty());
}

// Review Focus (formato): valores hostiles en metallic/roughness.
static void test_hostile_factor_values_are_clamped_or_inherited()
{
    const fs::path d = makeDir();
    struct Case { const char* name; const char* json; float wantMetallic; };
    const Case cases[] = {
        { "big.mat",  R"({"version":1,"type":"material","metallic":1e30})",   1.0f },
        { "neg.mat",  R"({"version":1,"type":"material","metallic":-1e30})",  0.0f },   // NO -1: se acota a 0, no se confunde con "heredar"
        { "str.mat",  R"({"version":1,"type":"material","metallic":"alto"})", -1.0f },
        { "nan.mat",  R"({"version":1,"type":"material","metallic":null})",   -1.0f },
    };
    for (const Case& c : cases)
    {
        writeText(d / c.name, c.json);
        std::string warning;
        const MaterialAsset m = loadMaterialAsset(d / c.name, &warning);
        CHECK(m.metallic == c.wantMetallic);
        CHECK(std::isfinite(m.metallic));
        if (c.wantMetallic != -1.0f) CHECK(!warning.empty()) ; // el valor fuera de rango SI avisa (se acoto)
    }
    // Sidecar hostil de tamano, mismo tope que ImportSettings.
    writeText(d / "huge.mat", std::string(5 * 1024 * 1024, 'x'));
    std::string warning;
    CHECK(isDefault(loadMaterialAsset(d / "huge.mat", &warning)));
    CHECK(!warning.empty());
}

// Las rutas de textura se guardan RELATIVAS a la carpeta del .mat, y se
// resuelven a absolutas al leer.
static void test_texture_paths_are_relative_to_the_mat_folder()
{
    const fs::path d = makeDir();
    fs::create_directories(d / "textures");
    const fs::path tex = d / "textures" / "piel.png";
    writeText(tex, "png");

    MaterialAsset in;
    in.albedo = tex.string();
    std::string err;
    CHECK(saveMaterialAsset(d / "piel.mat", in, &err));

    // El fichero en disco guarda una ruta relativa (no la absoluta del test).
    std::ifstream raw(d / "piel.mat");
    std::string text{ std::istreambuf_iterator<char>(raw), std::istreambuf_iterator<char>() };
    CHECK(text.find(d.string()) == std::string::npos);
    CHECK(text.find("textures") != std::string::npos);

    const MaterialAsset out = loadMaterialAsset(d / "piel.mat");
    CHECK(fs::equivalent(out.albedo, tex));
}

static void test_unicode_path_and_missing_folder_error()
{
    const fs::path d = makeDir() / fs::path(u8"ñandú");
    std::error_code ec;
    fs::create_directories(d, ec);
    MaterialAsset in; in.metallic = 0.1f;
    std::string err;
    CHECK(saveMaterialAsset(d / fs::path(u8"tabló.mat"), in, &err));
    CHECK(loadMaterialAsset(d / fs::path(u8"tabló.mat")) == in);

    CHECK(!saveMaterialAsset(makeDir() / "no_existe" / "x.mat", in, &err));
    CHECK(!err.empty());
}

int main()
{
    test_default_is_all_inherit();
    test_missing_file_is_default_without_warning();
    test_roundtrip_always_writes_the_file();
    test_broken_and_wrong_type_are_default_with_warning();
    test_hostile_factor_values_are_clamped_or_inherited();
    test_texture_paths_are_relative_to_the_mat_folder();
    test_unicode_path_and_missing_folder_error();

    if (g_failures == 0) std::printf("ALL MATERIAL ASSET TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 5`
Expected: FAIL de compilación — `DonTopo/Core/MaterialAsset.h: No such file`.

- [ ] **Step 3: Write minimal implementation**

`MaterialAsset.h`:

```cpp
#pragma once
#include <filesystem>
#include <string>

namespace DonTopo {

// Un material reutilizable, referenciado por ruta desde un slot de objeto (ver
// MaterialOverride::matAsset). Un campo vacio/-1 significa "heredar del modelo":
// un .mat con solo roughness deja intactas las tres texturas y el metallic.
struct MaterialAsset
{
    std::string albedo, normal, orm;   // absolutas en memoria; "" = heredar
    float       metallic  = -1.0f;     // -1 = heredar; si no, [0, 1]
    float       roughness = -1.0f;
};

inline bool operator==(const MaterialAsset& a, const MaterialAsset& b)
{
    return a.albedo == b.albedo && a.normal == b.normal && a.orm == b.orm &&
           a.metallic == b.metallic && a.roughness == b.roughness;
}
inline bool isDefault(const MaterialAsset& m) { return m == MaterialAsset{}; }

// Nunca lanza. Ausente = todo heredar, SIN aviso (es lo normal: un slot puede no
// tener .mat). Roto, de version/tipo desconocido, mayor de 64 KiB, o un campo con
// el tipo equivocado -> ese campo (o todo el fichero) hereda, CON aviso.
MaterialAsset loadMaterialAsset(const std::filesystem::path& mat, std::string* warning = nullptr);

// SIEMPRE escribe el fichero, aunque `asset` sea el defecto (un .mat es un asset
// con nombre, no un ajuste opcional que desaparece). false = no se pudo, y
// `error` dice por que.
bool saveMaterialAsset(const std::filesystem::path& mat, const MaterialAsset& asset,
                       std::string* error = nullptr);

} // namespace DonTopo
```

`MaterialAsset.cpp`:

```cpp
#include "DonTopo/Core/MaterialAsset.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>

namespace DonTopo {

namespace {
constexpr std::uintmax_t kMaxMaterialAssetBytes = 64 * 1024;

// Ruta relativa a `base` si es posible (con ".." si hace falta subir), o
// absoluta tal cual si no se puede expresar relativa (otra unidad).
std::string toRelativeToFolder(const std::string& path, const std::filesystem::path& base)
{
    if (path.empty()) return path;
    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(path, base, ec);
    if (ec || rel.empty()) return path;
    return rel.generic_string();
}

// La inversa: relativa a `base`, o ya absoluta.
std::string fromRelativeToFolder(const std::string& stored, const std::filesystem::path& base)
{
    if (stored.empty()) return stored;
    std::filesystem::path p(stored);
    if (p.is_absolute()) return stored;
    return std::filesystem::weakly_canonical(base / p).string();
}

float clampFactor(float v)
{
    if (!std::isfinite(v)) return -1.0f;   // no finito: como si no estuviera puesto
    return std::clamp(v, 0.0f, 1.0f);
}
} // namespace

MaterialAsset loadMaterialAsset(const std::filesystem::path& mat, std::string* warning)
{
    MaterialAsset out;
    if (warning) warning->clear();
    auto warn = [&](const std::string& m) { if (warning) *warning = m; };

    std::error_code ec;
    if (!std::filesystem::is_regular_file(mat, ec) || ec)
        return out;                                    // ausente: lo normal, sin aviso

    const std::uintmax_t size = std::filesystem::file_size(mat, ec);
    if (ec || size > kMaxMaterialAssetBytes)
    {
        warn("material ilegible o demasiado grande; se hereda todo del modelo");
        return out;
    }
    if (size == 0) return out;

    std::ifstream in(mat, std::ios::binary);
    if (!in) { warn("no se pudo abrir el material; se hereda todo del modelo"); return out; }
    const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };

    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object())
    {
        warn("JSON invalido; se hereda todo del modelo");
        return out;
    }
    const auto version = j.find("version");
    if (version == j.end() || !version->is_number_integer() || version->get<long long>() != 1)
    {
        warn("version de material desconocida; se hereda todo del modelo");
        return out;
    }
    const auto type = j.find("type");
    if (type == j.end() || !type->is_string() || type->get<std::string>() != "material")
    {
        warn("el fichero no es de tipo material; se hereda todo del modelo");
        return out;
    }

    const std::filesystem::path folder = mat.parent_path();
    std::string problems;
    auto readPath = [&](const char* key, std::string& dst)
    {
        const auto it = j.find(key);
        if (it == j.end()) return;
        if (it->is_string()) dst = fromRelativeToFolder(it->get<std::string>(), folder);
        else problems += std::string(key) + " no es una ruta valida (se hereda). ";
    };
    readPath("albedo", out.albedo);
    readPath("normal", out.normal);
    readPath("orm",    out.orm);

    auto readFactor = [&](const char* key, float& dst)
    {
        const auto it = j.find(key);
        if (it == j.end()) return;
        if (!it->is_number()) { problems += std::string(key) + " no es numerico (se hereda). "; return; }
        const double v = it->get<double>();
        if (!std::isfinite(v)) { problems += std::string(key) + " no es finito (se hereda). "; return; }
        dst = clampFactor(static_cast<float>(v));
        if (static_cast<double>(dst) != v)
            problems += std::string(key) + " fuera de rango (acotado). ";
    };
    readFactor("metallic",  out.metallic);
    readFactor("roughness", out.roughness);

    if (!problems.empty()) warn(problems);
    return out;
}

bool saveMaterialAsset(const std::filesystem::path& mat, const MaterialAsset& asset, std::string* error)
{
    const std::filesystem::path folder = mat.parent_path();
    nlohmann::json j;
    j["version"]  = 1;
    j["type"]     = "material";
    if (!asset.albedo.empty()) j["albedo"] = toRelativeToFolder(asset.albedo, folder);
    if (!asset.normal.empty()) j["normal"] = toRelativeToFolder(asset.normal, folder);
    if (!asset.orm.empty())    j["orm"]    = toRelativeToFolder(asset.orm,    folder);
    if (asset.metallic  >= 0.0f) j["metallic"]  = asset.metallic;
    if (asset.roughness >= 0.0f) j["roughness"] = asset.roughness;

    std::error_code ec;
    std::filesystem::path tmp = mat;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { if (error) *error = "no se pudo escribir " + mat.string(); return false; }
        out << j.dump(2) << '\n';
        if (!out)
        {
            if (error) *error = "escritura incompleta de " + mat.string();
            out.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::rename(tmp, mat, ec);
    if (ec)
    {
        if (error) *error = "no se pudo renombrar a " + mat.string() + ": " + ec.message();
        std::error_code rmEc;
        std::filesystem::remove(tmp, rmEc);
        return false;
    }
    return true;
}

} // namespace DonTopo
```

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 5; .\build-ninja\engine\tests\dt_material_asset_tests.exe; "exit $LASTEXITCODE"`
Expected: `ALL MATERIAL ASSET TESTS PASSED`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/Core/MaterialAsset.h engine/src/Core/MaterialAsset.cpp engine/tests/material_asset_tests.cpp engine/CMakeLists.txt engine/tests/CMakeLists.txt
git commit -m "feat(core): MaterialAsset, formato .mat tolerante con rutas relativas a su carpeta"
```

---

### Task 2: Capas — `matAsset` en `MaterialOverride` y resolución efectiva

**Files:**
- Modify: `engine/include/DonTopo/Core/GameObject.h:83-100` (`MaterialOverride`)
- Modify: `engine/src/Core/GameObject.cpp:137-257` (`collectMaterialOverrideWarnings`, `applyMaterialOverrides`)
- Test: `engine/tests/material_texture_tests.cpp`

**Interfaces:**
- Consumes (Task 1): `MaterialAsset`, `loadMaterialAsset`, `isDefault`.
- Produces: `MaterialOverride::matAsset` (ruta absoluta en memoria; `""` = sin `.mat`), usado por las Tasks 3, 4, 6, 7, 8, 9.

- [ ] **Step 1: Write the failing tests** — en `material_texture_tests.cpp` (llamarlos desde `main()`; usar `makeStaticFixture`/`makeSkinnedFixture` ya existentes en el fichero):

```cpp
static std::filesystem::path matAssetTestDir(const char* name)
{
    std::error_code ec;
    std::filesystem::path d = std::filesystem::temp_directory_path(ec) / name;
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d, ec);
    return d;
}

// El .mat manda cuando NO hay override del objeto para ese campo.
static void test_mat_asset_supplies_texture_when_no_override()
{
    const auto d = matAssetTestDir("dt_matasset_no_override");
    MaterialAsset m; m.albedo = (d / "rojo.png").string();
    std::string err;
    CHECK(saveMaterialAsset(d / "x.mat", m, &err));

    auto go = makeStaticFixture();   // material.texturePath = "assets/fbx_albedo.png"
    go->materialOverrides.push_back(MaterialOverride{});
    go->materialOverrides[0].matAsset = (d / "x.mat").string();
    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.texturePath == m.albedo);
}

// La override del OBJETO manda sobre el .mat.
static void test_object_override_wins_over_mat_asset()
{
    const auto d = matAssetTestDir("dt_matasset_object_wins");
    MaterialAsset m; m.albedo = (d / "rojo.png").string();
    std::string err;
    CHECK(saveMaterialAsset(d / "x.mat", m, &err));

    auto go = makeStaticFixture();
    MaterialOverride ov; ov.matAsset = (d / "x.mat").string();
    ov.albedo = (d / "azul.png").string();
    go->materialOverrides.push_back(ov);
    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.texturePath == ov.albedo);
}

// Review Focus 3: un .mat con SOLO roughness deja las tres texturas del FBX.
static void test_mat_asset_with_only_one_field_leaves_the_rest_alone()
{
    const auto d = matAssetTestDir("dt_matasset_partial");
    MaterialAsset m; m.roughness = 0.2f;
    std::string err;
    CHECK(saveMaterialAsset(d / "x.mat", m, &err));

    auto go = makeStaticFixture();
    const std::string fbxAlbedo = go->getMesh()->material.texturePath;
    go->materialOverrides.push_back(MaterialOverride{});
    go->materialOverrides[0].matAsset = (d / "x.mat").string();
    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.texturePath == fbxAlbedo);   // intacta
    CHECK(go->getMesh()->material.roughness == 0.2f);
}

// Clear del objeto (override vacio) cae al .mat, no al modelo.
static void test_clear_falls_back_to_mat_asset_not_model()
{
    const auto d = matAssetTestDir("dt_matasset_clear");
    MaterialAsset m; m.albedo = (d / "rojo.png").string();
    std::string err;
    CHECK(saveMaterialAsset(d / "x.mat", m, &err));

    auto go = makeStaticFixture();
    const std::string fbxAlbedo = go->getMesh()->material.texturePath;
    MaterialOverride ov; ov.matAsset = (d / "x.mat").string();
    ov.albedo = (d / "azul.png").string();
    go->materialOverrides.push_back(ov);
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.texturePath == ov.albedo);

    go->materialOverrides[0].albedo.clear();                  // Clear
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.texturePath == m.albedo);   // al .mat, no al FBX
    CHECK(go->getMesh()->material.texturePath != fbxAlbedo);
}

// Desvincular el .mat (matAsset = "") cae al modelo.
static void test_unlinking_mat_asset_falls_back_to_model()
{
    const auto d = matAssetTestDir("dt_matasset_unlink");
    MaterialAsset m; m.albedo = (d / "rojo.png").string();
    std::string err;
    CHECK(saveMaterialAsset(d / "x.mat", m, &err));

    auto go = makeStaticFixture();
    const std::string fbxAlbedo = go->getMesh()->material.texturePath;
    go->materialOverrides.push_back(MaterialOverride{});
    go->materialOverrides[0].matAsset = (d / "x.mat").string();
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.texturePath == m.albedo);

    go->materialOverrides[0].matAsset.clear();
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.texturePath == fbxAlbedo);
}

// Review Focus 1: un .mat inexistente hereda todo, sin lanzar.
static void test_missing_mat_asset_inherits_everything()
{
    auto go = makeStaticFixture();
    const std::string fbxAlbedo = go->getMesh()->material.texturePath;
    go->materialOverrides.push_back(MaterialOverride{});
    go->materialOverrides[0].matAsset = "no/existe/en/el/repo.mat";
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.texturePath == fbxAlbedo);
}

// El .mat funciona igual en skinned, en un indice distinto de 0.
static void test_mat_asset_on_skinned_slot_two()
{
    const auto d = matAssetTestDir("dt_matasset_skinned");
    MaterialAsset m; m.metallic = 0.7f;
    std::string err;
    CHECK(saveMaterialAsset(d / "x.mat", m, &err));

    auto go = makeSkinnedFixture();   // 3 materiales
    MaterialOverride ov; ov.index = 2; ov.matAsset = (d / "x.mat").string();
    go->materialOverrides.push_back(ov);
    applyMaterialOverrides(*go);

    CHECK(go->getSkinnedMesh()->materials[2].metallic == 0.7f);
    CHECK(go->getSkinnedMesh()->materials[0].metallic == 0.0f);   // el 0 no se toca
}

// Sin matAsset (con o sin otras overrides), el resultado es IDENTICO a hoy.
static void test_no_mat_asset_is_unchanged()
{
    auto go = makeStaticFixture();
    const std::string before = go->getMesh()->material.texturePath;
    go->materialOverrides.push_back(MaterialOverride{});   // sin matAsset, sin nada
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.texturePath == before);
}
```

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 5`
Expected: fallo de compilación — `MaterialOverride` no tiene `matAsset`.

- [ ] **Step 3: Implement**

`GameObject.h`, dentro de `MaterialOverride`, tras `roughness`/`base*`:

```cpp
        // Ruta de un .mat (Core/MaterialAsset) del que este slot toma valores
        // cuando el objeto no trae su propia override para ese campo. "" = sin
        // material asset. Vive aqui y no en Material por el mismo motivo que
        // las tres texturas: Material no distingue "esto lo puso el modelo" de
        // "esto lo puso el usuario", y sin esa distincion no hay Clear posible.
        std::string matAsset;
```

`GameObject.cpp` — incluir `#include "DonTopo/Core/MaterialAsset.h"`. En `collectMaterialOverrideWarnings`, tras el bucle existente de índice fuera de rango, añadir un segundo bucle:

```cpp
        for (const MaterialOverride& ov : go.materialOverrides)
        {
            if (ov.matAsset.empty()) continue;
            std::string warning;
            loadMaterialAsset(ov.matAsset, &warning);
            if (!warning.empty())
                out.push_back("mesh de '" + go.name + "'.materials: material '" +
                              ov.matAsset + "': " + warning);
        }
```

En `applyMaterialOverrides`, dentro del bucle `for (MaterialOverride& ov : go.materialOverrides)`, justo después de `Material& mat = *mats[(size_t)ov.index];` y antes de las llamadas a `aplica`:

```cpp
            // El .mat (si lo hay) resuelve lo que el objeto NO overridee: se
            // computa un valor "efectivo" por campo y se alimenta al MISMO
            // mecanismo de baseline de siempre, sin tocarlo. Sin matAsset,
            // matAsset queda en su defecto (todo vacio/-1) y effective(...)
            // devuelve el override del objeto tal cual: el resultado es
            // identico al de antes de que este campo existiera.
            MaterialAsset matAsset;
            if (!ov.matAsset.empty())
                matAsset = loadMaterialAsset(ov.matAsset);   // tolerante: invalido = heredar todo

            auto effective = [](const std::string& objOverride, const std::string& matValue)
            {
                return !objOverride.empty() ? objOverride : matValue;
            };
            auto effectiveFactor = [](float objOverride, float matValue)
            {
                return objOverride >= 0.0f ? objOverride : matValue;
            };
```

Y sustituir las cinco llamadas siguientes para que pasen el valor efectivo en vez del override crudo:

```cpp
            aplica(effective(ov.albedo, matAsset.albedo), ov.baseAlbedo, ov.baseAlbedoTaken, mat.texturePath);
            aplica(effective(ov.normal, matAsset.normal), ov.baseNormal, ov.baseNormalTaken, mat.normalMapPath);
            aplica(effective(ov.orm,    matAsset.orm),    ov.baseOrm,    ov.baseOrmTaken,    mat.metallicRoughnessPath);
            ...
            aplicaFactor(effectiveFactor(ov.metallic,  matAsset.metallic),  ov.baseMetallic,  ov.baseMetallicTaken,  mat.metallic);
            aplicaFactor(effectiveFactor(ov.roughness, matAsset.roughness), ov.baseRoughness, ov.baseRoughnessTaken, mat.roughness);
```

(el resto de la función —captura de baseline, comparación `distinto`, escritura final— no cambia).

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 5; .\build-ninja\engine\tests\dt_material_texture_tests.exe | Select-Object -Last 4; "exit $LASTEXITCODE"`
Expected: `ALL MATERIAL TEXTURE TESTS PASSED` (o el mensaje de éxito que ya use el fichero), exit 0 — **todos** los tests anteriores del fichero (overrides sin `matAsset`) siguen pasando sin cambios.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/Core/GameObject.h engine/src/Core/GameObject.cpp engine/tests/material_texture_tests.cpp
git commit -m "feat(core): matAsset en MaterialOverride, resolucion efectiva objeto > .mat > modelo"
```

---

### Task 3: Serialización de escena

**Files:**
- Modify: `engine/src/Core/Scene.cpp:1179-1259` (`nodeToJson`, bloque `mesh.materials`)
- Modify: `engine/src/Core/Scene.cpp:2355-2428` (`fromJson`, mismo bloque)
- Test: `engine/tests/material_texture_tests.cpp` (o `exporter_tests.cpp`; se usa el primero, que ya tiene `test_overrides_survive_round_trip` con el mismo patrón)

**Interfaces:**
- Consumes (Task 2): `MaterialOverride::matAsset`.
- Produces: el campo `"matAsset"` en `mesh.materials[i]`, con `toStoredPath`/`fromStoredPath` (ya existentes en `Scene.cpp:1107-1123`).

- [ ] **Step 1: Write the failing test** — en `material_texture_tests.cpp`, junto a `test_overrides_survive_round_trip` (llamarlo desde `main()`):

```cpp
// Review Focus 6: una entrada con SOLO matAsset (sin texturas ni factores) no
// se omite al guardar, y sobrevive a guardar y cargar.
static void test_mat_asset_survives_round_trip_alone(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto mesh = std::make_shared<SkinnedMesh>();
    mesh->sourcePath = "assets/hero.fbx";
    mesh->materials.resize(1);
    go->setMesh(std::move(mesh));

    MaterialOverride ov; ov.index = 0; ov.matAsset = "assets/rojo.mat";
    go->materialOverrides = {ov};

    const nlohmann::json j = scene.toJson();
    CHECK(j["root"]["children"][0]["mesh"].contains("materials"));

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Personaje") leido = n; });
    CHECK(leido != nullptr);
    if (!leido) return;
    CHECK(leido->materialOverrides.size() == 1);
    if (leido->materialOverrides.empty()) return;
    CHECK(leido->materialOverrides[0].matAsset == "assets/rojo.mat");
}

// Una escena vieja, sin el campo matAsset en absoluto, carga igual que hoy.
static void test_scene_without_mat_asset_field_loads_unchanged(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->material.texturePath = "assets/fbx_albedo.png";
    go->setMesh(std::move(mesh));
    MaterialOverride ov; ov.index = 0; ov.albedo = "assets/override.png";
    go->materialOverrides = {ov};

    nlohmann::json j = scene.toJson();
    CHECK(!j["root"]["children"][0]["mesh"]["materials"][0].contains("matAsset"));

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Cubo") leido = n; });
    CHECK(leido && leido->materialOverrides.size() == 1);
    if (leido) CHECK(leido->materialOverrides[0].matAsset.empty());
}
```

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; .\build-ninja\engine\tests\dt_material_texture_tests.exe | Select-Object -First 4; "exit $LASTEXITCODE"`
Expected: `FAIL` en `j["root"]["children"][0]["mesh"].contains("materials")` (la entrada con solo `matAsset` se descarta hoy por la condición de "nada que decir").

- [ ] **Step 3: Implement**

En `nodeToJson` (`Scene.cpp:1206-1211`), la condición de descarte y la escritura del campo:

```cpp
                if (ov.albedo.empty() && ov.normal.empty() && ov.orm.empty()
                    && ov.metallic < 0.0f && ov.roughness < 0.0f && ov.matAsset.empty()) continue;
                nlohmann::json entry = { {"index", ov.index} };
                if (!ov.albedo.empty()) entry["albedo"] = toStoredPath(ov.albedo, assetRoot);
                if (!ov.normal.empty()) entry["normal"] = toStoredPath(ov.normal, assetRoot);
                if (!ov.orm.empty())    entry["orm"]    = toStoredPath(ov.orm,    assetRoot);
                if (!ov.matAsset.empty()) entry["matAsset"] = toStoredPath(ov.matAsset, assetRoot);
```

(el resto del bloque —`metallic`/`roughness`/`base*` bajo `carryOverrideBaseline`— no cambia).

En `fromJson` (`Scene.cpp:2379-2381`), junto a la lectura de `albedo`/`normal`/`orm`:

```cpp
                        ov.albedo    = fromStoredPath(entry.value("albedo", ""), assetRoot);
                        ov.normal    = fromStoredPath(entry.value("normal", ""), assetRoot);
                        ov.orm       = fromStoredPath(entry.value("orm",    ""), assetRoot);
                        ov.matAsset  = fromStoredPath(entry.value("matAsset", ""), assetRoot);
```

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; .\build-ninja\engine\tests\dt_material_texture_tests.exe | Select-Object -Last 4; "exit $LASTEXITCODE"`
Expected: pasa entero, incluidos `test_overrides_survive_round_trip` y `test_no_overrides_writes_no_key` (que no debe empezar a escribir `matAsset` para overrides que no lo tienen).

- [ ] **Step 5: Commit**

```bash
git add engine/src/Core/Scene.cpp engine/tests/material_texture_tests.cpp
git commit -m "feat(core): serializa matAsset en mesh.materials de la escena"
```

---

### Task 4: `discardOverriddenDecodedImages` considera el `.mat`

**Files:**
- Modify: `engine/src/Renderer/AsyncAssetLoader.cpp:42-59`
- Test: `engine/tests/material_texture_tests.cpp`

**Interfaces:**
- Consumes (Task 1): `MaterialAsset`, `loadMaterialAsset`, `isDefault`.
- Produces: sin cambio de firma; `discardOverriddenDecodedImages` descarta también los slots que el `.mat` del índice 0 aporte.

- [ ] **Step 1: Write the failing test** — junto a `test_discard_overridden_decoded_images_removes_only_overridden_slot` (llamarlo desde `main()`):

```cpp
static void test_discard_overridden_decoded_images_considers_mat_asset()
{
    std::error_code ec;
    const std::filesystem::path d = std::filesystem::temp_directory_path(ec) / "dt_discard_matasset";
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d, ec);
    MaterialAsset m; m.normal = (d / "n.png").string();   // solo normal
    std::string err;
    CHECK(saveMaterialAsset(d / "x.mat", m, &err));

    DecodedImage albedo; albedo.slot = DecodedImage::Albedo;
    DecodedImage normal; normal.slot = DecodedImage::Normal;
    DecodedImage orm;    orm.slot    = DecodedImage::ORM;
    std::vector<DecodedImage> images{albedo, normal, orm};

    MaterialOverride ov; ov.index = 0; ov.matAsset = (d / "x.mat").string();   // sin overrides propias
    std::vector<MaterialOverride> overrides{ov};

    discardOverriddenDecodedImages(images, overrides);

    CHECK(images.size() == 2);
    bool hasAlbedo = false, hasNormal = false;
    for (const DecodedImage& img : images)
    {
        if (img.slot == DecodedImage::Albedo) hasAlbedo = true;
        if (img.slot == DecodedImage::Normal) hasNormal = true;
    }
    CHECK(hasAlbedo);    // el .mat no aporta albedo: la decodificada del FBX se queda
    CHECK(!hasNormal);   // el .mat SI aporta normal: se descarta la del FBX
}
```

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; .\build-ninja\engine\tests\dt_material_texture_tests.exe | Select-Object -First 4; "exit $LASTEXITCODE"`
Expected: `FAIL: !hasNormal` (hoy nada mira `ov.matAsset`, así que la del FBX no se descarta).

- [ ] **Step 3: Implement**

`AsyncAssetLoader.cpp` — incluir `#include "DonTopo/Core/MaterialAsset.h"` y sustituir el cuerpo:

```cpp
    void discardOverriddenDecodedImages(std::vector<DecodedImage>& images,
                                        const std::vector<MaterialOverride>& overrides)
    {
        // r.images solo decodifica DonTopo::Mesh::material (el campo singular,
        // no SkinnedMesh::materials), así que solo el override de índice 0 le
        // afecta — ver el comentario grande de runJob(), más abajo. Un índice
        // distinto no tiene nada que descartar aquí.
        for (const MaterialOverride& ov : overrides)
        {
            if (ov.index != 0) continue;
            // El .mat cuenta igual que una override propia: si aporta esa
            // textura, la decodificada del FBX ya no es la que se va a usar.
            MaterialAsset matAsset;
            if (!ov.matAsset.empty()) matAsset = loadMaterialAsset(ov.matAsset);
            if (!ov.albedo.empty() || !matAsset.albedo.empty())
                std::erase_if(images, [](const DecodedImage& d) { return d.slot == DecodedImage::Albedo; });
            if (!ov.normal.empty() || !matAsset.normal.empty())
                std::erase_if(images, [](const DecodedImage& d) { return d.slot == DecodedImage::Normal; });
            if (!ov.orm.empty() || !matAsset.orm.empty())
                std::erase_if(images, [](const DecodedImage& d) { return d.slot == DecodedImage::ORM; });
        }
    }
```

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; .\build-ninja\engine\tests\dt_material_texture_tests.exe | Select-Object -Last 4; "exit $LASTEXITCODE"`
Expected: pasa entero, incluido `test_discard_overridden_decoded_images_ignores_other_index`.

- [ ] **Step 5: Commit**

```bash
git add engine/src/Renderer/AsyncAssetLoader.cpp engine/tests/material_texture_tests.cpp
git commit -m "fix(renderer): discardOverriddenDecodedImages descarta tambien lo que aporte el .mat"
```

---

### Task 5: `applyMaterialAssetSettings` y el tipo `Material` en el Content Browser

**Files:**
- Modify: `engine/include/DonTopo/Editor/ContentBrowserPanel.h` (`AssetKind::Material`, `applyMaterialAssetSettings`)
- Modify: `engine/src/Editor/ContentBrowserPanel.cpp` (`classifyAsset`, filtro, icono, `applyMaterialAssetSettings`, "Create → Material")
- Test: `engine/tests/content_browser_tests.cpp`

**Interfaces:**
- Consumes (Tasks 1, 2): `MaterialAsset`, `saveMaterialAsset`, `sameAssetPath` (ya existe en `Core/ImportSettings.h`, comparación de rutas tolerante).
- Produces:
  ```cpp
  enum class AssetKind { Folder, Model3D, Audio, Image, Font, Scene, Script, Shader, Material, Other };
  struct MaterialAssetApplyResult { bool ok = false; std::string error; int refreshed = 0; };
  MaterialAssetApplyResult applyMaterialAssetSettings(GameObject* sceneRoot, const std::filesystem::path& mat,
                                                      const MaterialAsset& asset,
                                                      const std::function<void(GameObject&)>& rebuild);
  std::string uniqueMaterialName(const std::filesystem::path& dir);
  ```

- [ ] **Step 1: Write the failing tests** — en `content_browser_tests.cpp` (llamarlos desde `main()`; el fichero ya incluye `GameObject.h`, `Renderer/Mesh.h`/`SkinnedMesh.h` por trabajo previo):

```cpp
static void test_classify_material_extension()
{
    CHECK(classifyAsset(".mat", false) == AssetKind::Material);
    CHECK(classifyAsset(".MAT", false) == AssetKind::Material);
    CHECK(classifyAsset(".mat", true)  == AssetKind::Folder);   // una carpeta manda
}

static void test_unique_material_name()
{
    std::error_code ec;
    const fs::path d = fs::temp_directory_path(ec) / "dt_cb_unique_mat";
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    CHECK(uniqueMaterialName(d) == "Nuevo material.mat");
    std::ofstream(d / "Nuevo material.mat") << "x";
    CHECK(uniqueMaterialName(d) == "Nuevo material 2.mat");
}

static std::shared_ptr<Mesh> meshWithMatAssetOverride(const fs::path& mat)
{
    auto m = std::make_shared<Mesh>();
    return m;
}

// applyMaterialAssetSettings escribe el fichero y reconstruye SOLO a quien
// referencia ese .mat, sea cual sea el objeto (Review Focus 2: varios usuarios).
static void test_apply_material_asset_writes_and_refreshes_all_users()
{
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "dt_cb_apply_mat";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    const fs::path mat = base / "x.mat";
    std::string err;
    CHECK(saveMaterialAsset(mat, MaterialAsset{}, &err));

    GameObject root("root");
    GameObject* a = root.addChild("A");
    GameObject* b = root.addChild("B");
    GameObject* c = root.addChild("C");   // no usa el .mat
    a->setMesh(std::make_shared<Mesh>());
    b->setMesh(std::make_shared<Mesh>());
    c->setMesh(std::make_shared<Mesh>());
    MaterialOverride ovA; ovA.matAsset = mat.string();
    a->materialOverrides = {ovA};
    MaterialOverride ovB; ovB.matAsset = mat.string();
    b->materialOverrides = {ovB};

    std::vector<GameObject*> rebuilt;
    const auto rebuild = [&](GameObject& go) { rebuilt.push_back(&go); };

    MaterialAsset s; s.roughness = 0.3f;
    const MaterialAssetApplyResult r = applyMaterialAssetSettings(&root, mat, s, rebuild);
    CHECK(r.ok);
    CHECK(r.error.empty());
    CHECK(r.refreshed == 2);
    CHECK(rebuilt.size() == 2);
    CHECK((rebuilt[0] == a && rebuilt[1] == b) || (rebuilt[0] == b && rebuilt[1] == a));
    CHECK(loadMaterialAsset(mat) == s);
    CHECK(a->getMesh()->material.roughness == 0.3f);
    CHECK(c->getMesh()->material.roughness == 0.5f);   // el defecto de Material, sin tocar
}

// Review Focus (escritura): un fallo de escritura no reconstruye nada.
static void test_apply_material_asset_write_failure_rebuilds_nothing()
{
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "dt_cb_apply_mat_fail";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    GameObject root("root");
    root.addChild("A");
    int calls = 0;
    const MaterialAssetApplyResult r = applyMaterialAssetSettings(
        &root, base / "no_existe_carpeta" / "x.mat", MaterialAsset{}, [&](GameObject&) { ++calls; });
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    CHECK(r.refreshed == 0);
    CHECK(calls == 0);
}
```

(quitar el helper `meshWithMatAssetOverride` si no se usa; se deja el resto tal cual — `sameAssetPath` ya viene de `Core/ImportSettings.h`, incluido transitivamente por `ContentBrowserPanel.h`).

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 5`
Expected: fallo de compilación — `AssetKind::Material`, `applyMaterialAssetSettings`, `uniqueMaterialName` no existen.

- [ ] **Step 3: Implement**

`ContentBrowserPanel.h`: añadir `Material` al enum (`Folder, Model3D, Audio, Image, Font, Scene, Script, Shader, Material, Other`) y, junto a `applyTextureImportSettings`:

```cpp
struct MaterialAssetApplyResult {
    bool        ok = false;
    std::string error;
    int         refreshed = 0;
};
// Escribe el .mat y reconstruye (rebuild) cada objeto de la escena que lo
// referencie desde cualquier slot. Sin escritura, no reconstruye nada.
MaterialAssetApplyResult applyMaterialAssetSettings(GameObject* sceneRoot, const std::filesystem::path& mat,
                                                    const MaterialAsset& asset,
                                                    const std::function<void(GameObject&)>& rebuild);
// Nombre libre para un material nuevo dentro de dir, mismo patron que
// uniqueFolderName: "Nuevo material.mat", "Nuevo material 2.mat"...
std::string uniqueMaterialName(const std::filesystem::path& dir);
```

Incluir `#include "DonTopo/Core/MaterialAsset.h"` en `ContentBrowserPanel.h`.

`ContentBrowserPanel.cpp`, `classifyAsset` (`:216-228`), añadir antes del `return AssetKind::Other;`:

```cpp
    if (e == ".mat")                                                          return AssetKind::Material;
```

Filtro (`kOptions`, junto a "Shader"):

```cpp
                {"Shader", AssetKind::Shader},     {"Material", AssetKind::Material},
                {"Otros", AssetKind::Other},
```

Icono (switch de `label`/`btnColor`, junto a `Shader`):

```cpp
            case AssetKind::Material: btnColor = ImVec4(0.75f, 0.55f, 0.20f, 1.0f); label = "MAT"; break;
```

`uniqueMaterialName`, junto a `uniqueFolderName` (`:401-413`):

```cpp
std::string uniqueMaterialName(const std::filesystem::path& dir)
{
    const std::string base = "Nuevo material";
    std::error_code ec;
    if (!std::filesystem::exists(dir / (base + ".mat"), ec))
        return base + ".mat";
    for (int n = 2; ; ++n)
    {
        const std::string candidate = base + " " + std::to_string(n) + ".mat";
        if (!std::filesystem::exists(dir / candidate, ec))
            return candidate;
    }
}
```

`applyMaterialAssetSettings`, junto a `applyTextureImportSettings`:

```cpp
MaterialAssetApplyResult applyMaterialAssetSettings(GameObject* sceneRoot, const std::filesystem::path& mat,
                                                    const MaterialAsset& asset,
                                                    const std::function<void(GameObject&)>& rebuild)
{
    MaterialAssetApplyResult r;
    if (!saveMaterialAsset(mat, asset, &r.error))
        return r;                                    // nada reconstruido si no se pudo escribir
    r.ok = true;
    if (!sceneRoot || !rebuild) return r;

    sceneRoot->traverse([&](GameObject* go)
    {
        if (!go->hasMesh()) return;
        bool usaEsteMat = false;
        for (const MaterialOverride& ov : go->materialOverrides)
            if (!ov.matAsset.empty() && sameAssetPath(ov.matAsset, mat)) { usaEsteMat = true; break; }
        if (!usaEsteMat) return;
        applyMaterialOverrides(*go);
        rebuild(*go);
        ++r.refreshed;
    });
    return r;
}
```

Menú "Create" (sustituir el `MenuItem("Create Folder")` de `:1427-1448` por un submenú):

```cpp
        if (ImGui::BeginPopupContextWindow("##AssetPaneContext",
                ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
        {
            if (ImGui::BeginMenu("Create"))
            {
                if (ImGui::MenuItem("Folder"))
                {
                    const std::filesystem::path parent(m_currentDir);
                    const std::filesystem::path created = parent / uniqueFolderName(parent);
                    std::error_code mkEc;
                    std::filesystem::create_directory(created, mkEc);
                    if (mkEc)
                    {
                        ctx.pushLog("No se pudo crear la carpeta: " + mkEc.message());
                    }
                    else
                    {
                        ctx.pushLog("Carpeta creada: " + created.filename().string());
                        m_scanned = false;
                        beginAssetRename(created, /*isDir=*/true);
                    }
                }
                if (ImGui::MenuItem("Material"))
                {
                    const std::filesystem::path parent(m_currentDir);
                    const std::filesystem::path created = parent / uniqueMaterialName(parent);
                    std::string saveErr;
                    if (!saveMaterialAsset(created, MaterialAsset{}, &saveErr))
                    {
                        ctx.pushLog("No se pudo crear el material: " + saveErr);
                    }
                    else
                    {
                        ctx.pushLog("Material creado: " + created.filename().string());
                        m_scanned = false;
                        beginAssetRename(created, /*isDir=*/false);
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }
```

(`beginAssetRename` con `isDir=false` ya recorta la extensión al precargar el nombre, igual que con cualquier fichero; comprobarlo al ejecutar el Step 4 — si el rename mostrase ".mat" habría que ajustar `beginAssetRename`, pero su precarga usa `path.stem()` para ficheros, así que no debería hacer falta tocar nada).

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 5; .\build-ninja\engine\tests\dt_content_browser_tests.exe | Select-Object -Last 4; "exit $LASTEXITCODE"`
Expected: `ALL CONTENT BROWSER TESTS PASSED`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/Editor/ContentBrowserPanel.h engine/src/Editor/ContentBrowserPanel.cpp engine/tests/content_browser_tests.cpp
git commit -m "feat(editor): AssetKind::Material, Create > Material y applyMaterialAssetSettings"
```

---

### Task 6: Modal de edición del `.mat`

**Files:**
- Modify: `engine/include/DonTopo/Editor/ContentBrowserPanel.h` (estado del modal)
- Modify: `engine/src/Editor/ContentBrowserPanel.cpp` (doble clic, modal, drenado del diálogo)
- Test: `engine/tests/content_browser_tests.cpp` (solo la parte no-GUI: se cubre en la Task 5; esta tarea es UI y su prueba es la verificación manual + compilación)

**Interfaces:**
- Consumes (Tasks 1, 5): `MaterialAsset`, `loadMaterialAsset`, `applyMaterialAssetSettings`, `MaterialAssetApplyResult`.
- Produces: nada que consuman otras tareas (hoja de la UI).

- [ ] **Step 1: Implement** (sin test unitario nuevo: es ImGui puro; Review Focus 2 ya lo prueba la Task 5 a nivel de función. Verificar por build + arranque de `Sandbox`, como el resto de modales de este panel).

`ContentBrowserPanel.h` **no** declara constructor ni destructor propios ni conoce
`IGFD::FileDialog` (a diferencia de `PropertiesPanel.h`, que forward-declara
`namespace IGFD { class FileDialog; }` y por eso declara `~PropertiesPanel();`
fuera de línea): un `unique_ptr<IGFD::FileDialog>` en `ContentBrowserPanel`
necesita el mismo tratamiento, o el destructor implícito (generado donde
`EditorUI.h` declara `ContentBrowserPanel m_contentBrowserPanel;` como miembro
directo) fallaría al no ver el tipo completo. Añadir, cerca del principio del
header (junto a las declaraciones adelantadas si las hay, si no al principio del
fichero tras los `#include`):

```cpp
namespace IGFD { class FileDialog; }
```

y en `class ContentBrowserPanel { public: ... };` (`:204-207`), junto a `draw`:

```cpp
    ContentBrowserPanel();
    ~ContentBrowserPanel();
```

Junto a `m_importTarget`/`m_importEdit`:

```cpp
    // Edicion de un .mat, disparada por doble clic en el grid.
    std::filesystem::path m_matAssetTarget;
    MaterialAsset          m_matAssetEdit;
    std::string            m_matAssetError;
    bool                   m_openMatAssetPopup = false;
    bool                   m_matAssetDlgOpen     = false;
    DonTopo::MaterialTextureSlot m_matAssetDlgSlot = DonTopo::MaterialTextureSlot::Albedo;
    std::unique_ptr<IGFD::FileDialog> m_matAssetFileDialog;
```

`ContentBrowserPanel.cpp`, junto a los demás métodos (tras los `#include`, tras
añadir `#include <ImGuiFileDialog.h>`):

```cpp
ContentBrowserPanel::ContentBrowserPanel()  = default;
// Fuera de linea a proposito: el unique_ptr<IGFD::FileDialog> solo necesita el
// tipo completo AQUI, donde ImGuiFileDialog.h ya esta incluido — mismo patron
// que PropertiesPanel::~PropertiesPanel().
ContentBrowserPanel::~ContentBrowserPanel() = default;
```

`m_matAssetFileDialog` se construye perezosamente la primera vez que se abre el
modal (`if (!m_matAssetFileDialog) m_matAssetFileDialog =
std::make_unique<IGFD::FileDialog>();`, ver más abajo), así que no hace falta
tocar ninguna lista de inicialización.

`ContentBrowserPanel.cpp`: incluir `#include <ImGuiFileDialog.h>` y `#include "DonTopo/Editor/Command.h"` (para `MaterialTextureSlot`).

Doble clic, junto a los de `.lua`/`.json` (tras el bloque de `:1299-1328`):

```cpp
            if (!isDir && ext == ".mat" && ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                m_matAssetTarget = path;
                m_matAssetEdit   = loadMaterialAsset(path);
                m_matAssetError.clear();
                m_openMatAssetPopup = true;
            }
```

Modal, junto al de `Import Settings` (mismo `if (m_openImportPopup)` / `BeginPopupModal` que ya existe: añadir un bloque hermano después de su `EndPopup()`):

```cpp
        if (m_openMatAssetPopup)
        {
            if (!m_matAssetFileDialog) m_matAssetFileDialog = std::make_unique<IGFD::FileDialog>();
            ImGui::OpenPopup("Material");
            m_openMatAssetPopup = false;
        }
        if (ImGui::BeginPopupModal("Material", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("%s", m_matAssetTarget.filename().string().c_str());
            ImGui::Separator();

            struct MatSlot { const char* nombre; DonTopo::MaterialTextureSlot slot; std::string* dest; };
            const MatSlot slots[3] = {
                { "Albedo",             DonTopo::MaterialTextureSlot::Albedo, &m_matAssetEdit.albedo },
                { "Normal Map",         DonTopo::MaterialTextureSlot::Normal, &m_matAssetEdit.normal },
                { "Metallic/Roughness", DonTopo::MaterialTextureSlot::Orm,    &m_matAssetEdit.orm    },
            };
            for (const MatSlot& s : slots)
            {
                ImGui::PushID(s.nombre);
                ImGui::Text("%s: %s", s.nombre,
                            s.dest->empty() ? "Heredar del modelo"
                                            : std::filesystem::path(*s.dest).filename().string().c_str());
                if (ImGui::Button("Browse..."))
                {
                    m_matAssetDlgSlot = s.slot;
                    m_matAssetDlgOpen = true;
                    IGFD::FileDialogConfig cfg;
                    cfg.path = "assets";
                    m_matAssetFileDialog->OpenDialog("PickMatTextureDlg", "Choose image",
                                                     ".png,.jpg,.jpeg,.bmp,.tga", cfg);
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear")) s.dest->clear();
                ImGui::BeginChild((std::string("##MatDrop") + s.nombre).c_str(), ImVec2(0, 30), true);
                ImGui::TextDisabled("Drop image here");
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DT_ASSET_PATH"))
                        *s.dest = std::string(static_cast<const char*>(payload->Data));
                    ImGui::EndDragDropTarget();
                }
                ImGui::EndChild();
                ImGui::PopID();
            }

            bool heredaMetallic = m_matAssetEdit.metallic < 0.0f;
            if (ImGui::Checkbox("Heredar Metallic", &heredaMetallic))
                m_matAssetEdit.metallic = heredaMetallic ? -1.0f : 0.5f;
            if (!heredaMetallic)
                ImGui::SliderFloat("Metallic", &m_matAssetEdit.metallic, 0.0f, 1.0f, "%.2f");

            bool heredaRoughness = m_matAssetEdit.roughness < 0.0f;
            if (ImGui::Checkbox("Heredar Roughness", &heredaRoughness))
                m_matAssetEdit.roughness = heredaRoughness ? -1.0f : 0.5f;
            if (!heredaRoughness)
                ImGui::SliderFloat("Roughness", &m_matAssetEdit.roughness, 0.0f, 1.0f, "%.2f");

            if (!m_matAssetError.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_matAssetError.c_str());
            ImGui::Separator();

            const bool apply  = ImGui::Button("Aplicar");
            ImGui::SameLine();
            const bool cancel = ImGui::Button("Cancelar");
            if (apply)
            {
                const MaterialAssetApplyResult r = applyMaterialAssetSettings(
                    sceneRoot, m_matAssetTarget, m_matAssetEdit,
                    [&ctx](GameObject& go)
                    {
                        if (!ctx.renderer) return;
                        if (const SkinnedMesh* sm = go.getSkinnedMesh(); sm && go.skinnedRenderIndex >= 0)
                            ctx.renderer->rebuildSkinnedMesh(go.skinnedRenderIndex, *sm);
                        else if (go.staticRenderIndex >= 0)
                            ctx.renderer->rebuildStaticMesh(go.staticRenderIndex, *go.getMesh());
                    });
                if (r.ok)
                {
                    ctx.pushLog("Material aplicado: " + m_matAssetTarget.filename().string() +
                                " (" + std::to_string(r.refreshed) + " objeto(s) actualizados)");
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    m_matAssetError = r.error;
                }
            }
            else if (cancel)
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (m_matAssetDlgOpen && m_matAssetFileDialog->Display("PickMatTextureDlg"))
        {
            if (m_matAssetFileDialog->IsOk())
            {
                const std::string picked = m_matAssetFileDialog->GetFilePathName();
                switch (m_matAssetDlgSlot)
                {
                    case DonTopo::MaterialTextureSlot::Albedo: m_matAssetEdit.albedo = picked; break;
                    case DonTopo::MaterialTextureSlot::Normal: m_matAssetEdit.normal = picked; break;
                    case DonTopo::MaterialTextureSlot::Orm:    m_matAssetEdit.orm    = picked; break;
                }
            }
            m_matAssetFileDialog->Close();
            m_matAssetDlgOpen = false;
        }
```

(este bloque va junto a los otros `BeginPopupModal`/drenados de diálogo del mismo `draw()`, respetando el `ImGui::EndChild(); ImGui::End();` final de la función).

- [ ] **Step 2: Run to verify it compiles and the panel opens**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 5`
Expected: sin errores. Correr la suite completa (snippet) y arrancar `Sandbox.exe` ~7 s sin tocar `project.json`.

- [ ] **Step 3: Commit**

```bash
git add engine/include/DonTopo/Editor/ContentBrowserPanel.h engine/src/Editor/ContentBrowserPanel.cpp
git commit -m "feat(editor): modal de edicion de un .mat en el Content Browser"
```

---

### Task 7: `MaterialAssetCommand` y la fila "Material asset" en Properties

**Files:**
- Modify: `engine/include/DonTopo/Editor/Command.h:751-791` (junto a `MaterialTextureCommand`)
- Modify: `engine/src/Editor/Command.cpp:677-742` (junto a `setMaterialTextureOverride`/`MaterialTextureCommand`)
- Modify: `engine/include/DonTopo/Editor/PropertiesPanel.h` (declaraciones)
- Modify: `engine/src/Editor/PropertiesPanel.cpp` (`currentMaterialAsset`, `assignMaterialAsset`, fila en `drawMaterialSection`-equivalente `:8064-8123`, diálogo)
- Test: `engine/tests/material_texture_tests.cpp`

**Interfaces:**
- Consumes (Task 2): `MaterialOverride::matAsset`.
- Produces:
  ```cpp
  void setMaterialAssetOverride(GameObject& go, int materialIndex, const std::string& matAssetPath);
  class MaterialAssetCommand : public ICommand { /* antes/después de matAssetPath, por id */ };
  ```

- [ ] **Step 1: Write the failing tests** — en `material_texture_tests.cpp` (incluir `DonTopo/Editor/Command.h` ya está incluido; llamarlos desde `main()`):

```cpp
static void test_set_material_asset_override_creates_entry_and_applies()
{
    auto go = makeStaticFixture();
    setMaterialAssetOverride(*go, 0, "assets/rojo.mat");
    CHECK(go->materialOverrides.size() == 1);
    CHECK(go->materialOverrides[0].matAsset == "assets/rojo.mat");
}

static void test_material_asset_command_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    go->setMesh(std::make_shared<Mesh>());
    const uint64_t id = go->id;

    MaterialAssetCommand cmd(scene, nullptr, "Material de 'Cubo'", id, 0, "", "assets/rojo.mat");
    cmd.execute();
    CHECK(scene.findById(id)->materialOverrides[0].matAsset == "assets/rojo.mat");

    cmd.undo();
    CHECK(scene.findById(id)->materialOverrides[0].matAsset.empty());

    cmd.execute();   // redo
    CHECK(scene.findById(id)->materialOverrides[0].matAsset == "assets/rojo.mat");
}
```

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 5`
Expected: fallo de compilación — `setMaterialAssetOverride`, `MaterialAssetCommand` no existen.

- [ ] **Step 3: Implement**

`Command.h`, tras `MaterialTextureCommand` (`:790`):

```cpp
// Escribe la ruta de un .mat en el override del material `materialIndex` y lo
// aplica al Material. Simetrico a setMaterialTextureOverride.
void setMaterialAssetOverride(GameObject& go, int materialIndex, const std::string& matAssetPath);

// Cambio del .mat vinculado a UN material, por el stack de undo. Mismo patron
// que MaterialTextureCommand: resuelve por id, renderer opcional.
class MaterialAssetCommand : public ICommand {
public:
    MaterialAssetCommand(Scene& scene, EditorRenderer* renderer, std::string label,
                         uint64_t id, int materialIndex, std::string before, std::string after);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(const std::string& matAssetPath);

    Scene&          m_scene;
    EditorRenderer* m_renderer;
    std::string     m_label;
    uint64_t        m_id;
    int             m_materialIndex;
    std::string     m_before;
    std::string     m_after;
};
```

`Command.cpp`, tras `MaterialTextureCommand::apply` (`:742`):

```cpp
void setMaterialAssetOverride(GameObject& go, int materialIndex, const std::string& matAssetPath)
{
    MaterialOverride* ov = nullptr;
    for (auto& candidato : go.materialOverrides)
        if (candidato.index == materialIndex) { ov = &candidato; break; }
    if (!ov)
    {
        MaterialOverride nuevo;
        nuevo.index = materialIndex;
        go.materialOverrides.push_back(nuevo);
        ov = &go.materialOverrides.back();
    }
    ov->matAsset = matAssetPath;
    applyMaterialOverrides(go);
}

MaterialAssetCommand::MaterialAssetCommand(Scene& scene, EditorRenderer* renderer, std::string label,
                                           uint64_t id, int materialIndex,
                                           std::string before, std::string after)
    : m_scene(scene), m_renderer(renderer), m_label(std::move(label)), m_id(id),
      m_materialIndex(materialIndex), m_before(std::move(before)), m_after(std::move(after)) {}

void MaterialAssetCommand::execute() { apply(m_after); }
void MaterialAssetCommand::undo()    { apply(m_before); }

void MaterialAssetCommand::apply(const std::string& matAssetPath)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go || !go->hasMesh()) return;

    const std::vector<const Material*> mats = materialsOfMesh(*go);
    if (m_materialIndex < 0 || m_materialIndex >= (int)mats.size()) return;

    setMaterialAssetOverride(*go, m_materialIndex, matAssetPath);

    if (!m_renderer) return;
    if (const SkinnedMesh* sm = go->getSkinnedMesh(); sm && go->skinnedRenderIndex >= 0)
        m_renderer->rebuildSkinnedMesh(go->skinnedRenderIndex, *sm);
    else if (go->staticRenderIndex >= 0)
        m_renderer->rebuildStaticMesh(go->staticRenderIndex, *go->getMesh());
}
```

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 5; .\build-ninja\engine\tests\dt_material_texture_tests.exe | Select-Object -Last 4; "exit $LASTEXITCODE"`
Expected: pasa entero.

- [ ] **Step 5: Wire the Properties UI** (sin test unitario nuevo — es ImGui puro, como la Task 6; se verifica por build y por GUI). `PropertiesPanel.h`, junto a `assignMaterialTexture` (`:142`) y a `m_textureDlg*` (`:592-596`):

```cpp
    void assignMaterialAsset(EditorContext& ctx, uint64_t ownerId, int materialIndex, const std::string& path);
    ...
    uint64_t m_matAssetDlgOwner    = 0;
    int      m_matAssetDlgMaterial = 0;
    std::unique_ptr<IGFD::FileDialog> m_matAssetFileDialog;
```

`PropertiesPanel.cpp`, constructor (junto a `m_textureFileDialog(std::make_unique<IGFD::FileDialog>())`, `:316`): añadir `, m_matAssetFileDialog(std::make_unique<IGFD::FileDialog>())`.

`currentMaterialAsset`, junto a `currentOverride` (`:266-280`):

```cpp
std::string currentMaterialAsset(const DonTopo::GameObject& go, int materialIndex)
{
    for (const DonTopo::MaterialOverride& ov : go.materialOverrides)
        if (ov.index == materialIndex) return ov.matAsset;
    return {};
}
```

`assignMaterialAsset`, junto a `assignMaterialTexture` (`:8288-8362`):

```cpp
void PropertiesPanel::assignMaterialAsset(EditorContext& ctx, uint64_t ownerId, int materialIndex,
                                          const std::string& path)
{
    if (!ctx.scene) return;
    GameObject* go = ctx.scene->findById(ownerId);
    if (!go) { ctx.logModule("Mesh", "No se pudo vincular el material: el objeto ya no existe"); return; }
    if (!go->hasMesh() || ctx.editingLocked) return;

    const std::vector<const Material*> mats = materialsOfMesh(*go);
    if (materialIndex < 0 || materialIndex >= (int)mats.size())
    {
        ctx.logModule("Mesh", "No se pudo vincular el material: el material ya no existe en '" + go->name + "'");
        return;
    }
    if (!path.empty())
    {
        std::string ext = std::filesystem::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".mat")
        {
            m_textureLoadError = "Formato no soportado: " + ext;
            return;
        }
    }
    const std::string antes = currentMaterialAsset(*go, materialIndex);
    if (antes == path) return;
    m_textureLoadError.clear();

    auto cmd = std::make_unique<MaterialAssetCommand>(
        *ctx.scene, ctx.renderer,
        (path.empty() ? "Desvincular material de '" : "Material de '") + go->name + "'",
        go->id, materialIndex, antes, path);
    cmd->execute();
    if (ctx.undo) ctx.undo->push(std::move(cmd));
    ctx.pushLog((path.empty() ? "Material desvinculado de '" : "Material vinculado a '") + go->name + "'");
}
```

Fila en la sección Material, dentro del `for (int m ...)` de `:8064-8123`, tras el bucle `for (const SlotDesc& d : kSlots)` y antes del bloque de `Metallic`/`Roughness`:

```cpp
        {
            const std::string matAssetActual = currentMaterialAsset(*ctx.selected, m);
            ImGui::Text("Material asset: %s", matAssetActual.empty() ? "None"
                        : std::filesystem::path(matAssetActual).filename().string().c_str());
            drawAssetDropBox(
                ctx, "MatAsset", "Drop .mat here",
                [this, ownerId, m]() {
                    m_matAssetDlgOwner    = ownerId;
                    m_matAssetDlgMaterial = m;
                    IGFD::FileDialogConfig cfg;
                    cfg.path = "assets";
                    m_matAssetFileDialog->OpenDialog("PickMatAssetDlg", "Choose material", ".mat", cfg);
                },
                [this, &ctx, ownerId, m](const std::string& path) {
                    assignMaterialAsset(ctx, ownerId, m, path);
                });
            ImGui::BeginDisabled(ctx.editingLocked || matAssetActual.empty());
            if (ImGui::Button("Clear##MatAsset")) assignMaterialAsset(ctx, ownerId, m, "");
            ImGui::EndDisabled();
        }
```

Drenado del diálogo, junto al de `m_textureFileDialog` en `drawMeshDialog` (`:8386-8396`):

```cpp
    if (m_matAssetDlgOpen && m_matAssetFileDialog->Display("PickMatAssetDlg"))
    {
        if (m_matAssetFileDialog->IsOk())
            assignMaterialAsset(ctx, m_matAssetDlgOwner, m_matAssetDlgMaterial,
                                m_matAssetFileDialog->GetFilePathName());
        m_matAssetFileDialog->Close();
        m_matAssetDlgOpen = false;
    }
```

(`m_matAssetDlgOpen` se pone a `true` dentro del lambda `onBrowse` de arriba: añadir `m_matAssetDlgOpen = true;` junto a las otras dos asignaciones).

- [ ] **Step 6: Run to verify it compiles**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 5`
Expected: sin errores. Suite completa (snippet) + `Sandbox.exe` ~7 s.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Editor/Command.h engine/src/Editor/Command.cpp engine/include/DonTopo/Editor/PropertiesPanel.h engine/src/Editor/PropertiesPanel.cpp engine/tests/material_texture_tests.cpp
git commit -m "feat(editor): MaterialAssetCommand y fila Material asset en Properties"
```

---

### Task 8: Ciclo de vida — renombrar, mover y borrar un `.mat`

**Files:**
- Modify: `engine/src/Editor/ContentBrowserPanel.cpp:504-560` (`updateSceneReferencesForRename`)
- Modify: `engine/src/Editor/ContentBrowserPanel.cpp:639-666` (`countSceneReferences`)
- Modify: `engine/src/Editor/ContentBrowserPanel.cpp:667-...` (`detachSceneReferencesForDelete`)
- Test: `engine/tests/content_browser_tests.cpp`

**Interfaces:**
- Consumes (Task 2): `MaterialOverride::matAsset`.
- Produces: sin cambio de firma en las tres funciones.

- [ ] **Step 1: Write the failing tests** — en `content_browser_tests.cpp` (llamarlos desde `main()`; seguir el patrón de `test_rename_rewrites_skinned_material_path`/`test_detach_clears_skinned_material_path` ya existentes):

```cpp
static std::unique_ptr<GameObject> makeMatAssetFixture(const std::string& matPath)
{
    auto go = std::make_unique<GameObject>("ConMaterial");
    go->setMesh(std::make_shared<Mesh>());
    MaterialOverride ov; ov.index = 0; ov.matAsset = matPath;
    go->materialOverrides = {ov};
    return go;
}

// Review Focus 4.
static void test_count_references_finds_mat_asset()
{
    const std::string knownPath = "assets/rojo.mat";
    auto go = makeMatAssetFixture(knownPath);
    CHECK(countSceneReferences(go.get(), knownPath, /*isDir=*/false) == 1);
    CHECK(countSceneReferences(go.get(), std::string("assets/otro.mat"), false) == 0);
}

static void test_rename_rewrites_mat_asset_path()
{
    const std::string oldPath = "assets/rojo.mat";
    const std::string newPath = "assets/carmesi.mat";
    auto go = makeMatAssetFixture(oldPath);
    GameObject* selected = nullptr;
    bool isPlaying = false;
    EditorContext ctx{selected, isPlaying};
    updateSceneReferencesForRename(ctx, go.get(), oldPath, newPath, /*isDir=*/false);
    CHECK(go->materialOverrides[0].matAsset == newPath);
}

// Un objeto que NO usa el .mat renombrado no se toca.
static void test_rename_leaves_other_mat_asset_untouched()
{
    auto go = makeMatAssetFixture("assets/otro.mat");
    GameObject* selected = nullptr;
    bool isPlaying = false;
    EditorContext ctx{selected, isPlaying};
    updateSceneReferencesForRename(ctx, go.get(), "assets/rojo.mat", "assets/carmesi.mat", false);
    CHECK(go->materialOverrides[0].matAsset == "assets/otro.mat");
}

// Review Focus 5: borrar el .mat en uso vacia matAsset y el objeto vuelve al modelo.
static void test_detach_clears_mat_asset_and_falls_back_to_model()
{
    const std::string knownPath = "assets/rojo.mat";
    auto go = makeMatAssetFixture(knownPath);
    go->getMesh()->material.texturePath = "assets/rojo.png";   // lo que puso el .mat via applyMaterialOverrides en la app real
    GameObject* selected = nullptr;
    bool isPlaying = false;
    EditorContext ctx{selected, isPlaying};
    detachSceneReferencesForDelete(ctx, go.get(), knownPath, /*isDir=*/false);
    CHECK(go->materialOverrides[0].matAsset.empty());
}
```

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; .\build-ninja\engine\tests\dt_content_browser_tests.exe | Select-Object -First 6; "exit $LASTEXITCODE"`
Expected: FAIL en los cuatro tests nuevos (ninguna de las tres funciones mira `matAsset` hoy).

- [ ] **Step 3: Implement**

`updateSceneReferencesForRename` — en el bloque `if (go->hasMesh())` (junto a las overrides de textura, `:511-519`, dentro del `for (MaterialOverride& ov : go->materialOverrides)`):

```cpp
            for (MaterialOverride& ov : go->materialOverrides)
            {
                updateField(ov.albedo);
                updateField(ov.normal);
                updateField(ov.orm);
                updateField(ov.baseAlbedo);
                updateField(ov.baseNormal);
                updateField(ov.baseOrm);
                updateField(ov.matAsset);
            }
```

`countSceneReferences` — en el bloque que arma `textureMatches` (`materialsOf(go)` con `mat->texturePath`/`normalMapPath`/`metallicRoughnessPath`), añadir la comprobación de `matAsset` sobre `go->materialOverrides`:

```cpp
        bool textureMatches = false;
        for (const Material* mat : materialsOf(go))
            if (matches(mat->texturePath) || matches(mat->normalMapPath) ||
                matches(mat->metallicRoughnessPath))
                textureMatches = true;
        for (const MaterialOverride& ov : go->materialOverrides)
            if (matches(ov.matAsset)) textureMatches = true;
```

`detachSceneReferencesForDelete` — en el bucle final que limpia `go->materialOverrides` (`:754-762`):

```cpp
                for (MaterialOverride& ov : go->materialOverrides)
                {
                    if (matches(ov.albedo)) ov.albedo.clear();
                    if (matches(ov.normal)) ov.normal.clear();
                    if (matches(ov.orm))    ov.orm.clear();
                    if (matches(ov.baseAlbedo)) ov.baseAlbedo.clear();
                    if (matches(ov.baseNormal)) ov.baseNormal.clear();
                    if (matches(ov.baseOrm))    ov.baseOrm.clear();
                    if (matches(ov.matAsset))   ov.matAsset.clear();
                }
```

Ninguna de las tres funciones necesita más cambios: `updateField`/`matches` ya son closures genéricas sobre `std::string&`/`const std::string&`.

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; .\build-ninja\engine\tests\dt_content_browser_tests.exe | Select-Object -Last 4; "exit $LASTEXITCODE"`
Expected: `ALL CONTENT BROWSER TESTS PASSED`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add engine/src/Editor/ContentBrowserPanel.cpp engine/tests/content_browser_tests.cpp
git commit -m "feat(editor): renombrar, contar y borrar un .mat actualiza matAsset en la escena"
```

---

### Task 9: Exportador

**Files:**
- Modify: `engine/src/Editor/GameExporter.cpp` (`collectSceneAssets`, bloque `if (go->hasMesh())`; `rewriteNode`, bloque `mesh.materials`)
- Test: `engine/tests/exporter_tests.cpp`

**Interfaces:**
- Consumes (Task 2): `MaterialOverride::matAsset`.
- Produces: `collectSceneAssets` añade un `ExportAsset` por cada `.mat` referenciado; `rewriteScenePaths` reescribe `matAsset`.

- [ ] **Step 1: Write the failing test** — en `exporter_tests.cpp` (llamarlo desde `main()` con `root`; el fichero ya incluye `Command.h`/`GameObject.h`):

```cpp
// Review Focus (exportador): el .mat viaja con la escena, y matAsset se
// reescribe a su ruta dentro del paquete.
static void test_mat_asset_is_collected_and_rewritten(const fs::path& root)
{
    const fs::path mat = root / "assets" / "rojo.mat";
    std::string err;
    CHECK(saveMaterialAsset(mat, MaterialAsset{}, &err));

    Scene scene;
    scene.setAssetRoot(root.string());
    auto* go = scene.addGameObject("prop");
    go->setMesh(makeMesh(root / "assets" / "hero.fbx"));
    MaterialOverride ov; ov.index = 0; ov.matAsset = mat.string();
    go->materialOverrides.push_back(ov);

    std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    std::vector<std::string> pkg;
    for (const ExportAsset& a : assets) pkg.push_back(a.packagePath);
    CHECK(std::find(pkg.begin(), pkg.end(), "assets/rojo.mat") != pkg.end());

    std::map<std::string, std::string> sourceToPackage;
    for (const ExportAsset& a : assets)
        sourceToPackage[exportPathKey(a.sourcePath)] = a.packagePath;

    nlohmann::json j = scene.toJson();
    CHECK(j["root"]["children"][0]["mesh"]["materials"][0]["matAsset"].get<std::string>() == "assets/rojo.mat");

    const int rewritten = rewriteScenePaths(j, sourceToPackage);
    CHECK(rewritten >= 1);
    CHECK(j["root"]["children"][0]["mesh"]["materials"][0]["matAsset"].get<std::string>() == "assets/rojo.mat");
}
```

(la ruta reescrita coincide con la de disco porque `mat` ya está dentro de `root`, así que `toStoredPath`/el mapa `sourceToPackage` la dejan igual — es la misma forma que usa `test_rewrite_makes_paths_relative`; comprobar contra ese test si el valor esperado no coincidiera al ejecutar).

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; .\build-ninja\engine\tests\dt_exporter_tests.exe | Select-Object -First 4; "exit $LASTEXITCODE"`
Expected: `FAIL` — `.mat` no aparece en `pkg` (nada lo añade hoy).

- [ ] **Step 3: Implement**

`GameExporter.cpp`, en el bloque `if (go->hasMesh())` de `collectSceneAssets` (junto a `addWithSidecar(m->texturePath); ...`), añadir tras el bucle de materiales:

```cpp
            for (const MaterialOverride& ov : go->materialOverrides)
                if (!ov.matAsset.empty()) add(ov.matAsset);   // el .mat no lleva sidecar propio
```

`rewriteNode` (`:390-396`), junto a `rewriteField(mat, "orm", ...)`:

```cpp
        if (mesh.contains("materials") && mesh["materials"].is_array())
            for (nlohmann::json& mat : mesh["materials"])
            {
                n += rewriteField(mat, "albedo", sourceToPackage);
                n += rewriteField(mat, "normal", sourceToPackage);
                n += rewriteField(mat, "orm", sourceToPackage);
                n += rewriteField(mat, "matAsset", sourceToPackage);
            }
```

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; .\build-ninja\engine\tests\dt_exporter_tests.exe | Select-Object -Last 2; "exit $LASTEXITCODE"`
Expected: `OK`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add engine/src/Editor/GameExporter.cpp engine/tests/exporter_tests.cpp
git commit -m "feat(export): el paquete incluye el .mat referenciado y reescribe matAsset"
```

---

### Task 10: Documentación y cierre del audit

**Files:**
- Modify: `docs/assets-editor-audit.md` (fila de "Asset de Material independiente")
- Modify: `README.md` (párrafo del Content Browser)

- [ ] **Step 1:** Con `Edit` (nunca reescribir el fichero entero; comprobar `git diff --stat` tras cada cambio):
  - Audit: localizar la fila/nota de "Asset de Material independiente" (`docs/assets-editor-audit.md:177`, vista en el research previo: "Asset de Material independiente (`.mat` reutilizable, con su propio 'Create > Material'): hoy Material vive solo embebido en Mesh/GameObject"). Marcarla **CERRADO**, con una frase: sidecar `.mat` con referencia viva por slot (override del objeto > `.mat` > modelo), edición en un modal propio del Content Browser (doble clic), `MaterialAssetCommand` en Properties, ciclo de vida por ruta y exportador. Spec `docs/superpowers/specs/2026-09-24-material-asset-design.md`.
  - README: en el párrafo del Content Browser (el mismo que ya menciona "Import Settings..."), añadir una frase: materials can be saved as a reusable `.mat` asset (right-click empty space → Create → Material), assigned to any material slot by dragging it from the grid, and edited by double-clicking it; editing a shared `.mat` updates every object that references it, while each object's own texture/factor overrides still win.
- [ ] **Step 2: Suite completa** en segundo plano con el snippet: `SUMMARY total 34, fallos:` vacío.
- [ ] **Step 3: Commit**

```bash
git add docs/assets-editor-audit.md README.md
git commit -m "docs: cerrar el material como asset independiente en el audit"
```

---

## Self-Review

**Spec coverage.**
- §1 Formato y módulo (Core) → Task 1.
- §2 Referencia y capas (matAsset, resolución efectiva, serialización, `discardOverriddenDecodedImages`) → Tasks 2, 3, 4.
- §3 Cambiar el `.mat` en caliente (`applyMaterialAssetSettings`, refresco selectivo) → Task 5.
- §4 UI (Create, `AssetKind::Material`, modal de edición, fila de Properties, `MaterialAssetCommand`) → Tasks 5, 6, 7.
- §5 Ciclo de vida (renombrar, borrar, exportador) → Tasks 8, 9.
- Verificación: los tests automáticos cubren Core, capas, escena, exportador y ciclo de vida (Tasks 1-5, 8, 9); las Tasks 6-7 son UI pura (ImGui) y se verifican por compilación, arranque de `Sandbox` y la verificación manual final del usuario en GUI y en los dos backends.
- Riesgos de la spec (rebuild en cadena, textura renombrada no reescribe el `.mat`, lectura sin caché, ampliación futura) son propiedades del diseño elegido, no requisitos con test propio; quedan anotados aquí para que el ejecutor no los "corrija" por su cuenta.

**Placeholder scan.** Todos los pasos de código llevan el cuerpo completo; ningún paso dice "similar a la Task N" sin repetir el código.

**Type consistency.** `MaterialAsset`/`loadMaterialAsset`/`saveMaterialAsset`/`isDefault` (Task 1) se usan con esos nombres en 2, 4, 5, 6, 9. `MaterialOverride::matAsset` (Task 2) en 3, 4, 5, 6, 7, 8, 9. `AssetKind::Material`, `applyMaterialAssetSettings`, `MaterialAssetApplyResult`, `uniqueMaterialName` (Task 5) en 6. `setMaterialAssetOverride`, `MaterialAssetCommand` (Task 7) no los usa ninguna tarea posterior salvo la propia UI de Properties, dentro de la misma tarea.

**Review Focus.** 1→Task 2, 2→Task 5/6, 3→Task 2, 4→Task 8, 5→Task 8, 6→Task 3.

## Entrega (para quien ejecute)

Al terminar la Task 10, pedir al usuario la verificación manual en GUI, **en Vulkan y en D3D12** (recordar que el backend sale del último proyecto abierto): crear un `.mat` desde Create → Material; asignarlo a dos objetos distintos (uno estático, un personaje) arrastrándolo desde el grid; editarlo con doble clic y comprobar que Aplicar cambia a los DOS; poner una override de textura en uno de los objetos y comprobar que manda sobre el `.mat`; Clear de esa override vuelve al `.mat`, no al modelo; desvincular el `.mat` del slot vuelve al modelo; renombrar y mover el `.mat` no rompe la escena; borrar el `.mat` en uso avisa de cuántos objetos lo usan y, tras confirmar, esos objetos vuelven a su modelo; el juego exportado respeta el material; cierre limpio en D3D12 y syncval de Vulkan con control positivo. Ningún agente puede verificar lo visual: decirlo así en el mensaje final.
