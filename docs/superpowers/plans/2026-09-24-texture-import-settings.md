# Ajustes de importación por textura — Plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cada textura de material puede llevar un sidecar `<asset>.import.json` con espacio de color (auto/sRGB/lineal) y mipmaps, editable desde el Content Browser y respetado por el editor y el juego exportado en Vulkan y D3D12.

**Architecture:** Un módulo de Core (`ImportSettings`) lee y escribe el sidecar sin tocar GPU. `decodeMaterialTexture` (el decodificador central de materiales) lo lee y devuelve, junto a los píxeles, el espacio de color pedido y la cadena de mips generada en CPU; los dos backends resuelven el formato con una única función pura (`resolveSrgb`) y suben todos los niveles. El Content Browser mueve/renombra/borra/copia el sidecar con el asset, el exportador lo incluye y un modal de "Import Settings…" lo edita y reconstruye los materiales afectados.

**Tech Stack:** C++20, nlohmann_json (ya en `DonTopoCore`), stb_image, Vulkan y D3D12 (D3D12MA), ImGui. Tests planos `main()` + `CHECK` registrados con `dt_add_test`, ejecutados desde la raíz del repo.

**Spec:** `docs/superpowers/specs/2026-09-24-texture-import-settings-design.md`

## Global Constraints

- **Sin dependencias de terceros nuevas** (nlohmann_json y stb_image ya están).
- **Defecto = comportamiento actual**: `colorSpace = auto`, `mipmaps = false`. Un proyecto sin sidecars no cambia ni un byte de lo que se sube a GPU ni de las claves de caché.
- **El sidecar solo existe si difiere del defecto**: guardar un valor por defecto borra el fichero.
- **Lectura tolerante, nunca lanza**: fichero ausente, JSON roto, `version` ≠ 1, `type` ≠ `texture` o enum desconocido → valores por defecto (por campo cuando se pueda) y un aviso, sin abortar la carga.
- **Alcance de la v1**: solo texturas de MATERIAL (albedo/normal/ORM; estáticos y skinned; Vulkan y D3D12). UI, skybox, splash y miniaturas quedan fuera.
- **El formato (sRGB/UNORM) lo decide únicamente `resolveSrgb`** en los caminos de material de los dos backends.
- **Cadena de mips en CPU** (filtro de caja ponderado por alfa), para que ambos backends la suban con la misma entrada.
- **CRLF**: el repo va en CRLF. Tras cada `Edit`, comprobar `git diff --stat` (un cambio pequeño no debe salir como miles de líneas). No usar `sed -i` ni Get-Content/Set-Content en PowerShell.
- **Los tests se ejecutan desde la raíz del repo** (`.\build-ninja\engine\tests\dt_xxx.exe`), no desde `build-ninja/engine/tests`.
- **Build**: `.\build.bat` desde PowerShell (redirigir la salida a un fichero y leer la cola; no volcar el log entero).
- **Extensible por tipo**: el campo `type` del sidecar es el discriminador; la v1 solo lee `texture`. Modelos y audio son specs posteriores (ver la nota de memoria `pending_import_settings_models_audio`).

## Review Focus

Entradas y fallos que la spec implica y que ningún test "de camino feliz" cubre; cada uno tiene su test en la tarea indicada:

1. **Sidecar hostil o corrupto** (fichero de MB, JSON anidado 100 000 niveles, tipos equivocados, enum desconocido): debe dar valores por defecto sin lanzar, sin desbordar la pila y sin bloquear el editor. → Task 1.
2. **Texturas 1×1, 1×N y no potencia de 2 con mipmaps activados**: cadena correcta hasta 1×1 y, en 1×1, sin niveles extra. → Task 2.
3. **Mismo fichero en dos slots o con ajustes distintos no comparte imagen** (clave de caché de texturas y clave de malla compartida). → Task 2.
4. **Mover/renombrar cuando el destino ya tiene sidecar**: se rechaza sin sobrescribir el del destino y sin tocar el origen. → Task 6.
5. **Borrar la textura no deja el sidecar huérfano; un sidecar huérfano de fuera del editor se ignora sin error**, y el grid no lista los `.import.json`. → Task 6.
6. **Aplicar con el sidecar no escribible** (carpeta de solo lectura): el modal muestra el error y no cierra, y no reconstruye nada. → Task 8.

---

## File Structure

| Fichero | Responsabilidad |
|---|---|
| `engine/include/DonTopo/Core/ImportSettings.h` / `engine/src/Core/ImportSettings.cpp` (crear) | Tipos del ajuste, ruta del sidecar, lectura/escritura tolerante, mover/copiar/borrar el sidecar. Sin GPU ni ImGui. |
| `engine/include/DonTopo/Renderer/TextureImport.h` / `engine/src/Renderer/TextureImport.cpp` (crear) | `TextureMip`, `mipLevelCount`, `buildMipChain`, `resolveSrgb`, `textureKeySuffix`. Puro. |
| `engine/include/DonTopo/Renderer/SharedTextureCache.h` (modificar) | `makeTextureKey` acepta un sufijo de ajustes. |
| `engine/src/Renderer/SharedGpuMesh.cpp` (modificar) | `makeSharedMeshKey` añade los ajustes de las tres rutas al final de la clave, solo si no son los de siempre. |
| `engine/include/DonTopo/Renderer/MaterialTextureSource.h` / `.cpp` (modificar) | `DecodedTexture` lleva `colorSpace` y `mips`; `decodeMaterialTexture` los rellena. |
| `engine/include/DonTopo/Renderer/AsyncAssetLoader.h` / `engine/src/Renderer/AsyncAssetLoader.cpp` (modificar) | `DecodedImage` lleva `colorSpace` y `mips`. |
| `engine/include/DonTopo/Renderer/GpuResources.h` / `engine/src/Renderer/GpuResources.cpp`, `Renderer.h`, `Renderer.cpp` (modificar) | Vulkan: imágenes con niveles, formato resuelto, sampler con `maxLod` libre, vistas con todos los niveles. |
| `engine/src/Renderer/D3D12/D3D12Renderer.cpp` (modificar) | D3D12: `uploadTexture` con niveles, `uploadMaterialTexture` por `TextureKind`, SRV con el formato real del recurso. |
| `engine/include/DonTopo/Editor/ContentBrowserPanel.h` / `engine/src/Editor/ContentBrowserPanel.cpp` (modificar) | Filtro del listado, mover/renombrar/borrar con sidecar, modal "Import Settings…", `applyTextureImportSettings`. |
| `engine/src/Editor/AssetImport.cpp` (modificar) | La importación externa copia el sidecar. |
| `engine/src/Editor/GameExporter.cpp` (modificar) | El exportador copia el sidecar de las texturas de material. |
| `engine/tests/import_settings_tests.cpp`, `engine/tests/texture_import_tests.cpp` (crear); `content_browser_tests.cpp`, `exporter_tests.cpp`, `shared_texture_cache_tests.cpp`, `shared_gpu_mesh_tests.cpp` (ampliar) | Tests. |
| `engine/CMakeLists.txt`, `engine/tests/CMakeLists.txt` (modificar) | Registro de fuentes y tests. |
| `docs/assets-editor-audit.md`, `README.md` (modificar) | Cierre de U8 y sección de documentación. |

Un modificado por tarea y un commit por tarea. **Las tareas 4 y 5 (GPU) no se pueden probar con tests automáticos que abran un device**: su verificación es build limpio, suite completa, tests de política (grep) y arranque de `Sandbox` en cada backend; el resto lo verifica el usuario en GUI (spec, "Verificación").

---

### Task 1: `ImportSettings` (Core) — sidecar tolerante

**Files:**
- Create: `engine/include/DonTopo/Core/ImportSettings.h`
- Create: `engine/src/Core/ImportSettings.cpp`
- Create: `engine/tests/import_settings_tests.cpp`
- Modify: `engine/CMakeLists.txt` (añadir `src/Core/ImportSettings.cpp` junto a `src/Core/JobSystem.cpp`, línea ~41, dentro de `add_library(DonTopoCore ...)`)
- Modify: `engine/tests/CMakeLists.txt` (añadir `dt_add_test(dt_import_settings_tests import_settings_tests.cpp DonTopoCore)` tras `dt_thumbnail_tests`)

**Interfaces:**
- Produces (namespace `DonTopo`, todo lo usan las tareas 2–8):
  ```cpp
  enum class ColorSpaceOverride : uint8_t { Auto, Srgb, Linear };
  struct TextureImportSettings {
      ColorSpaceOverride colorSpace = ColorSpaceOverride::Auto;
      bool               mipmaps    = false;
  };
  inline bool operator==(const TextureImportSettings&, const TextureImportSettings&);
  inline bool isDefault(const TextureImportSettings&);
  inline constexpr const char* kImportSidecarSuffix = ".import.json";
  std::filesystem::path importSidecarPath(const std::filesystem::path& asset);   // "<asset>.import.json"
  bool isImportSidecar(const std::filesystem::path& p);
  TextureImportSettings loadTextureImportSettings(const std::filesystem::path& asset, std::string* warning = nullptr);
  bool saveTextureImportSettings(const std::filesystem::path& asset, const TextureImportSettings&, std::string* error = nullptr);
  ```
  (`moveImportSidecar` / `copyImportSidecar` / `removeImportSidecar` / `importSidecarConflict` los añade la Task 6.)

- [ ] **Step 1: Write the failing test** — crear `engine/tests/import_settings_tests.cpp`:

```cpp
// Test headless de ImportSettings (sin GUI, sin GPU). Plain main + CHECK,
// mismo patron que content_browser_tests.cpp. Se ejecuta desde la raiz del repo.
#include "DonTopo/Core/ImportSettings.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using namespace DonTopo;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static fs::path makeDir()
{
    std::error_code ec;
    fs::path d = fs::temp_directory_path(ec) / "dt_import_settings_test";
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

static void writeText(const fs::path& p, const std::string& s)
{
    std::ofstream(p, std::ios::binary) << s;
}

static void test_sidecar_path_and_detection()
{
    CHECK(importSidecarPath(fs::path("a") / "foto.png") == fs::path("a") / "foto.png.import.json");
    CHECK(isImportSidecar(fs::path("a") / "foto.png.import.json"));
    CHECK(isImportSidecar("FOTO.PNG.IMPORT.JSON"));
    CHECK(!isImportSidecar("scene.json"));
    CHECK(!isImportSidecar("foto.png"));
    CHECK(!isImportSidecar(".import.json"));     // sin nombre delante: no es de nadie
}

static void test_missing_file_is_default_without_warning()
{
    const fs::path d = makeDir();
    std::string warning = "x";
    const TextureImportSettings s = loadTextureImportSettings(d / "no_existe.png", &warning);
    CHECK(isDefault(s));
    CHECK(warning.empty());            // ausente no es un problema, es lo normal
}

static void test_roundtrip_and_default_removes_sidecar()
{
    const fs::path d = makeDir();
    const fs::path asset = d / "foto.png";
    std::string err;

    TextureImportSettings in;
    in.colorSpace = ColorSpaceOverride::Linear;
    in.mipmaps    = true;
    CHECK(saveTextureImportSettings(asset, in, &err));
    CHECK(fs::exists(importSidecarPath(asset)));
    CHECK(loadTextureImportSettings(asset) == in);

    // Guardar el defecto BORRA el fichero.
    CHECK(saveTextureImportSettings(asset, TextureImportSettings{}, &err));
    CHECK(!fs::exists(importSidecarPath(asset)));
    // Y guardar el defecto sin fichero previo no es un error.
    CHECK(saveTextureImportSettings(asset, TextureImportSettings{}, &err));
}

static void test_broken_json_is_default_with_warning()
{
    const fs::path d = makeDir();
    const fs::path asset = d / "roto.png";
    writeText(importSidecarPath(asset), "{ esto no es json");
    std::string warning;
    CHECK(isDefault(loadTextureImportSettings(asset, &warning)));
    CHECK(!warning.empty());
}

static void test_wrong_version_type_and_shape_are_default()
{
    const fs::path d = makeDir();
    const struct { const char* name; const char* json; } bad[] = {
        { "v2.png",    R"({"version":2,"type":"texture","colorSpace":"linear","mipmaps":true})" },
        { "audio.png", R"({"version":1,"type":"audio","colorSpace":"linear","mipmaps":true})"   },
        { "notype.png",R"({"version":1,"colorSpace":"linear","mipmaps":true})"                  },
        { "array.png", R"([1,2,3])"                                                              },
        { "verstr.png",R"({"version":"1","type":"texture","colorSpace":"linear"})"              },
    };
    for (const auto& b : bad)
    {
        writeText(importSidecarPath(d / b.name), b.json);
        std::string warning;
        CHECK(isDefault(loadTextureImportSettings(d / b.name, &warning)));
        CHECK(!warning.empty());
    }
}

static void test_unknown_field_values_keep_the_good_field()
{
    const fs::path d = makeDir();
    writeText(importSidecarPath(d / "a.png"),
              R"({"version":1,"type":"texture","colorSpace":"hdr","mipmaps":true})");
    std::string w1;
    const TextureImportSettings a = loadTextureImportSettings(d / "a.png", &w1);
    CHECK(a.colorSpace == ColorSpaceOverride::Auto);   // desconocido -> auto
    CHECK(a.mipmaps);                                   // el otro campo sobrevive
    CHECK(!w1.empty());

    writeText(importSidecarPath(d / "b.png"),
              R"({"version":1,"type":"texture","colorSpace":"srgb","mipmaps":"si"})");
    std::string w2;
    const TextureImportSettings b = loadTextureImportSettings(d / "b.png", &w2);
    CHECK(b.colorSpace == ColorSpaceOverride::Srgb);
    CHECK(!b.mipmaps);
    CHECK(!w2.empty());
}

// Review Focus 1: entradas hostiles.
static void test_hostile_sidecars_do_not_throw_or_blow_the_stack()
{
    const fs::path d = makeDir();
    // 200 KB de '[': anidado a 200000 niveles. Un parseo recursivo sin tope
    // reventaria la pila; el tope de tamano lo corta antes de parsear.
    writeText(importSidecarPath(d / "nested.png"), std::string(200 * 1024, '['));
    std::string warning;
    CHECK(isDefault(loadTextureImportSettings(d / "nested.png", &warning)));
    CHECK(!warning.empty());

    // 5 MB de basura.
    writeText(importSidecarPath(d / "big.png"), std::string(5 * 1024 * 1024, 'x'));
    warning.clear();
    CHECK(isDefault(loadTextureImportSettings(d / "big.png", &warning)));
    CHECK(!warning.empty());

    // Un DIRECTORIO con el nombre del sidecar: no es un fichero regular.
    fs::create_directories(importSidecarPath(d / "dir.png"));
    CHECK(isDefault(loadTextureImportSettings(d / "dir.png")));

    // Fichero vacio.
    writeText(importSidecarPath(d / "empty.png"), "");
    CHECK(isDefault(loadTextureImportSettings(d / "empty.png")));
}

static void test_unicode_path()
{
    const fs::path d = makeDir() / fs::path(u8"ñandú");
    std::error_code ec;
    fs::create_directories(d, ec);
    const fs::path asset = d / fs::path(u8"tabló.png");
    TextureImportSettings in;
    in.mipmaps = true;
    std::string err;
    CHECK(saveTextureImportSettings(asset, in, &err));
    CHECK(loadTextureImportSettings(asset) == in);
}

static void test_save_into_missing_folder_reports_error()
{
    const fs::path d = makeDir();
    TextureImportSettings in;
    in.mipmaps = true;
    std::string err;
    CHECK(!saveTextureImportSettings(d / "no_existe" / "x.png", in, &err));
    CHECK(!err.empty());
}

int main()
{
    test_sidecar_path_and_detection();
    test_missing_file_is_default_without_warning();
    test_roundtrip_and_default_removes_sidecar();
    test_broken_json_is_default_with_warning();
    test_wrong_version_type_and_shape_are_default();
    test_unknown_field_values_keep_the_good_field();
    test_hostile_sidecars_do_not_throw_or_blow_the_stack();
    test_unicode_path();
    test_save_into_missing_folder_reports_error();

    if (g_failures == 0) std::printf("ALL IMPORT SETTINGS TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Register the test and run it to verify it fails**

Añadir la línea de `dt_add_test` indicada en `engine/tests/CMakeLists.txt`. Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error' | Select-Object -First 5`
Expected: FAIL de compilación — `DonTopo/Core/ImportSettings.h: No such file`.

- [ ] **Step 3: Write minimal implementation**

`engine/include/DonTopo/Core/ImportSettings.h`:

```cpp
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace DonTopo {

// Espacio de color con el que se interpreta una textura de material. Auto = lo
// decide el slot del material (color base sRGB, normal/ORM lineal), que es lo
// que pasaba antes de que existiera el ajuste.
enum class ColorSpaceOverride : uint8_t { Auto, Srgb, Linear };

struct TextureImportSettings
{
    ColorSpaceOverride colorSpace = ColorSpaceOverride::Auto;
    bool               mipmaps    = false;
};

inline bool operator==(const TextureImportSettings& a, const TextureImportSettings& b)
{
    return a.colorSpace == b.colorSpace && a.mipmaps == b.mipmaps;
}
inline bool isDefault(const TextureImportSettings& s) { return s == TextureImportSettings{}; }

// El ajuste vive en "<asset>.import.json", junto al asset, y solo existe si
// difiere del defecto.
inline constexpr const char* kImportSidecarSuffix = ".import.json";

std::filesystem::path importSidecarPath(const std::filesystem::path& asset);
bool                  isImportSidecar(const std::filesystem::path& p);

// Nunca lanza. Ausente = defecto y SIN aviso. Roto, de version o tipo
// desconocido o mayor de 64 KiB = defecto y `warning` explica por que. Un valor
// desconocido en un campo suelto deja los demas campos leidos.
TextureImportSettings loadTextureImportSettings(const std::filesystem::path& asset,
                                                std::string* warning = nullptr);

// Guardar el defecto BORRA el sidecar. false = no se pudo (y `error` lo dice).
bool saveTextureImportSettings(const std::filesystem::path& asset,
                               const TextureImportSettings& settings,
                               std::string* error = nullptr);

} // namespace DonTopo
```

`engine/src/Core/ImportSettings.cpp`:

```cpp
#include "DonTopo/Core/ImportSettings.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>
#include <system_error>

namespace DonTopo {

namespace {

// Un sidecar legitimo son ~100 bytes. El tope existe para que un fichero de
// megas, o un JSON anidado a 100000 niveles, no llegue nunca al parser.
constexpr std::uintmax_t kMaxSidecarBytes = 64 * 1024;

const char* colorSpaceName(ColorSpaceOverride c)
{
    switch (c)
    {
        case ColorSpaceOverride::Srgb:   return "srgb";
        case ColorSpaceOverride::Linear: return "linear";
        case ColorSpaceOverride::Auto:   break;
    }
    return "auto";
}

} // namespace

std::filesystem::path importSidecarPath(const std::filesystem::path& asset)
{
    std::filesystem::path p = asset;
    p += kImportSidecarSuffix;
    return p;
}

bool isImportSidecar(const std::filesystem::path& p)
{
    const auto name = p.filename().native();
    const auto suffix = std::filesystem::path(kImportSidecarSuffix).native();
    if (name.size() <= suffix.size()) return false;
    const size_t offset = name.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i)
    {
        auto c = name[offset + i];
        if (c >= 'A' && c <= 'Z') c = static_cast<decltype(c)>(c + 32);
        if (c != suffix[i]) return false;
    }
    return true;
}

TextureImportSettings loadTextureImportSettings(const std::filesystem::path& asset,
                                                std::string* warning)
{
    TextureImportSettings out;
    if (warning) warning->clear();
    auto warn = [&](const std::string& m) { if (warning) *warning = m; };

    const std::filesystem::path sidecar = importSidecarPath(asset);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(sidecar, ec) || ec)
        return out;                                   // ausente: lo normal, sin aviso

    const std::uintmax_t size = std::filesystem::file_size(sidecar, ec);
    if (ec || size > kMaxSidecarBytes)
    {
        warn("sidecar ilegible o demasiado grande; se usan los valores por defecto");
        return out;
    }
    if (size == 0)
        return out;

    std::ifstream in(sidecar, std::ios::binary);
    if (!in)
    {
        warn("no se pudo abrir el sidecar; se usan los valores por defecto");
        return out;
    }
    const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };

    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object())
    {
        warn("JSON invalido; se usan los valores por defecto");
        return out;
    }

    const auto version = j.find("version");
    if (version == j.end() || !version->is_number_integer() || version->get<long long>() != 1)
    {
        warn("version de sidecar desconocida; se usan los valores por defecto");
        return out;
    }
    const auto type = j.find("type");
    if (type == j.end() || !type->is_string() || type->get<std::string>() != "texture")
    {
        warn("el sidecar no es de tipo texture; se usan los valores por defecto");
        return out;
    }

    std::string problems;
    if (const auto it = j.find("colorSpace"); it != j.end())
    {
        const std::string v = it->is_string() ? it->get<std::string>() : std::string();
        if      (v == "auto")   out.colorSpace = ColorSpaceOverride::Auto;
        else if (v == "srgb")   out.colorSpace = ColorSpaceOverride::Srgb;
        else if (v == "linear") out.colorSpace = ColorSpaceOverride::Linear;
        else problems += "colorSpace desconocido (se usa auto). ";
    }
    if (const auto it = j.find("mipmaps"); it != j.end())
    {
        if (it->is_boolean()) out.mipmaps = it->get<bool>();
        else                  problems += "mipmaps no es booleano (se usa false). ";
    }
    if (!problems.empty()) warn(problems);
    return out;
}

bool saveTextureImportSettings(const std::filesystem::path& asset,
                               const TextureImportSettings& settings, std::string* error)
{
    const std::filesystem::path sidecar = importSidecarPath(asset);
    std::error_code ec;

    if (isDefault(settings))
    {
        std::filesystem::remove(sidecar, ec);         // ausente tampoco es error
        if (ec)
        {
            if (error) *error = "no se pudo borrar " + sidecar.string() + ": " + ec.message();
            return false;
        }
        return true;
    }

    nlohmann::json j;
    j["version"]    = 1;
    j["type"]       = "texture";
    j["colorSpace"] = colorSpaceName(settings.colorSpace);
    j["mipmaps"]    = settings.mipmaps;

    // Fichero temporal + rename: un corte a mitad no deja un sidecar a medias.
    std::filesystem::path tmp = sidecar;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            if (error) *error = "no se pudo escribir " + sidecar.string();
            return false;
        }
        out << j.dump(2) << '\n';
        if (!out)
        {
            if (error) *error = "escritura incompleta de " + sidecar.string();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::rename(tmp, sidecar, ec);
    if (ec)
    {
        if (error) *error = "no se pudo renombrar a " + sidecar.string() + ": " + ec.message();
        std::error_code rmEc;
        std::filesystem::remove(tmp, rmEc);
        return false;
    }
    return true;
}

} // namespace DonTopo
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; .\build-ninja\engine\tests\dt_import_settings_tests.exe`
Expected: `ALL IMPORT SETTINGS TESTS PASSED`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/Core/ImportSettings.h engine/src/Core/ImportSettings.cpp engine/tests/import_settings_tests.cpp engine/CMakeLists.txt engine/tests/CMakeLists.txt
git commit -m "feat(core): ImportSettings, sidecar .import.json tolerante para las texturas"
```

---

### Task 2: Mips en CPU, `resolveSrgb` y claves de caché

**Files:**
- Create: `engine/include/DonTopo/Renderer/TextureImport.h`
- Create: `engine/src/Renderer/TextureImport.cpp`
- Create: `engine/tests/texture_import_tests.cpp`
- Modify: `engine/include/DonTopo/Renderer/SharedTextureCache.h:18-30` (`makeTextureKey`)
- Modify: `engine/src/Renderer/SharedGpuMesh.cpp` (`makeSharedMeshKey`, tras la construcción de `key`, antes del `return`)
- Modify: `engine/CMakeLists.txt` (`src/Renderer/TextureImport.cpp` junto a `src/Renderer/MaterialTextureSource.cpp`, línea ~60)
- Modify: `engine/tests/CMakeLists.txt` (`dt_add_test(dt_texture_import_tests texture_import_tests.cpp DonTopoCore)`)
- Modify: `engine/tests/shared_texture_cache_tests.cpp`, `engine/tests/shared_gpu_mesh_tests.cpp` (un test cada uno)

**Interfaces:**
- Consumes (Task 1): `ColorSpaceOverride`, `TextureImportSettings`, `loadTextureImportSettings`, `isDefault`.
- Produces:
  ```cpp
  // Renderer/TextureImport.h
  struct TextureMip { uint32_t w = 0, h = 0; std::vector<uint8_t> rgba; };
  uint32_t                mipLevelCount(uint32_t w, uint32_t h);                    // 1 + floor(log2(max(w,h))); 1 si w o h == 0
  std::vector<TextureMip> buildMipChain(const uint8_t* rgba, uint32_t w, uint32_t h); // niveles 1..N-1 (sin el 0)
  bool                    resolveSrgb(TextureKind kind, ColorSpaceOverride o);
  std::string             textureKeySuffix(const std::string& path);                // "" si path vacio o ajustes por defecto
  // SharedTextureCache.h
  inline std::string makeTextureKey(const std::string& path, const std::vector<uint8_t>& embedded,
                                    TextureKind kind, const std::string& settingsSuffix = {});
  ```

- [ ] **Step 1: Write the failing tests** — crear `engine/tests/texture_import_tests.cpp` (la Task 3 y la 4 añaden más tests aquí):

```cpp
// Test headless de TextureImport (cadena de mips, resolucion sRGB, claves) y de
// como decodeMaterialTexture lee los ajustes. Sin GPU. Desde la raiz del repo.
#include "DonTopo/Renderer/TextureImport.h"
#include "DonTopo/Renderer/SharedTextureCache.h"
#include "DonTopo/Core/ImportSettings.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace DonTopo;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static void test_mip_level_count()
{
    CHECK(mipLevelCount(0, 0) == 1);
    CHECK(mipLevelCount(1, 1) == 1);
    CHECK(mipLevelCount(2, 2) == 2);
    CHECK(mipLevelCount(4, 4) == 3);
    CHECK(mipLevelCount(5, 3) == 3);     // 5x3 -> 2x1 -> 1x1
    CHECK(mipLevelCount(1, 8) == 4);     // 1x8 -> 1x4 -> 1x2 -> 1x1
    CHECK(mipLevelCount(256, 256) == 9);
    CHECK(mipLevelCount(300, 100) == 9);
}

// Review Focus 2.
static void test_mip_chain_shapes()
{
    const std::vector<uint8_t> red4x4(4 * 4 * 4, 0);
    std::vector<uint8_t> px(4 * 4 * 4);
    for (size_t i = 0; i < 16; ++i) { px[i * 4 + 0] = 255; px[i * 4 + 1] = 0; px[i * 4 + 2] = 0; px[i * 4 + 3] = 255; }

    auto chain = buildMipChain(px.data(), 4, 4);
    CHECK(chain.size() == 2);
    CHECK(chain[0].w == 2 && chain[0].h == 2 && chain[0].rgba.size() == 2 * 2 * 4);
    CHECK(chain[1].w == 1 && chain[1].h == 1 && chain[1].rgba.size() == 4);
    for (const TextureMip& m : chain)
        for (size_t i = 0; i < m.rgba.size(); i += 4)
            CHECK(m.rgba[i] == 255 && m.rgba[i + 1] == 0 && m.rgba[i + 2] == 0 && m.rgba[i + 3] == 255);

    // 1x1: sin niveles extra.
    const uint8_t one[4] = { 1, 2, 3, 255 };
    CHECK(buildMipChain(one, 1, 1).empty());

    // No potencia de 2 y tiras 1xN.
    std::vector<uint8_t> a(5 * 3 * 4, 200);
    chain = buildMipChain(a.data(), 5, 3);
    CHECK(chain.size() == 2);
    CHECK(chain[0].w == 2 && chain[0].h == 1);
    CHECK(chain[1].w == 1 && chain[1].h == 1);

    std::vector<uint8_t> strip(1 * 8 * 4, 90);
    chain = buildMipChain(strip.data(), 1, 8);
    CHECK(chain.size() == 3);
    CHECK(chain[0].w == 1 && chain[0].h == 4);
    CHECK(chain[1].w == 1 && chain[1].h == 2);
    CHECK(chain[2].w == 1 && chain[2].h == 1);

    // Entrada invalida: vacio, sin lanzar.
    CHECK(buildMipChain(nullptr, 4, 4).empty());
    CHECK(buildMipChain(px.data(), 0, 4).empty());
}

// El nivel 0 no se toca y los pixeles transparentes no oscurecen a los opacos.
static void test_mip_chain_alpha_weighted_and_input_untouched()
{
    // 2x1: un pixel rojo opaco y uno TRANSPARENTE negro.
    std::vector<uint8_t> px = { 255, 0, 0, 255,   0, 0, 0, 0 };
    const std::vector<uint8_t> before = px;
    const auto chain = buildMipChain(px.data(), 2, 1);
    CHECK(px == before);
    CHECK(chain.size() == 1);
    CHECK(chain[0].w == 1 && chain[0].h == 1);
    CHECK(chain[0].rgba[0] == 255);    // el rojo no se oscurece a 128
    CHECK(chain[0].rgba[3] == 128);    // el alfa si es la media
}

static void test_resolve_srgb_table()
{
    const TextureKind kinds[] = { TextureKind::BaseColor, TextureKind::Normal, TextureKind::Orm };
    for (TextureKind k : kinds)
    {
        CHECK(resolveSrgb(k, ColorSpaceOverride::Srgb));
        CHECK(!resolveSrgb(k, ColorSpaceOverride::Linear));
    }
    CHECK(resolveSrgb(TextureKind::BaseColor, ColorSpaceOverride::Auto));
    CHECK(!resolveSrgb(TextureKind::Normal,   ColorSpaceOverride::Auto));
    CHECK(!resolveSrgb(TextureKind::Orm,      ColorSpaceOverride::Auto));
}

static fs::path makeDir()
{
    std::error_code ec;
    fs::path d = fs::temp_directory_path(ec) / "dt_texture_import_test";
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

// Review Focus 3.
static void test_texture_key_suffix_and_key()
{
    const fs::path d = makeDir();
    const std::string a = (d / "a.png").string();

    CHECK(textureKeySuffix("").empty());
    CHECK(textureKeySuffix(a).empty());                       // sin sidecar: la clave de hoy
    CHECK(makeTextureKey(a, {}, TextureKind::BaseColor, textureKeySuffix(a)) ==
          makeTextureKey(a, {}, TextureKind::BaseColor));     // y no cambia

    TextureImportSettings lin;
    lin.colorSpace = ColorSpaceOverride::Linear;
    std::string err;
    CHECK(saveTextureImportSettings(d / "a.png", lin, &err));
    const std::string sufLin = textureKeySuffix(a);
    CHECK(!sufLin.empty());

    TextureImportSettings mips;
    mips.mipmaps = true;
    CHECK(saveTextureImportSettings(d / "a.png", mips, &err));
    const std::string sufMips = textureKeySuffix(a);
    CHECK(!sufMips.empty());
    CHECK(sufMips != sufLin);

    CHECK(makeTextureKey(a, {}, TextureKind::BaseColor, sufLin) !=
          makeTextureKey(a, {}, TextureKind::BaseColor, sufMips));
    CHECK(makeTextureKey(a, {}, TextureKind::BaseColor, sufLin) !=
          makeTextureKey(a, {}, TextureKind::BaseColor));

    // Sin ruta, el sufijo no cuenta (una textura embebida no tiene sidecar).
    const std::vector<uint8_t> emb = { 1, 2, 3 };
    CHECK(makeTextureKey("", emb, TextureKind::BaseColor, "#s") ==
          makeTextureKey("", emb, TextureKind::BaseColor));
}

int main()
{
    test_mip_level_count();
    test_mip_chain_shapes();
    test_mip_chain_alpha_weighted_and_input_untouched();
    test_resolve_srgb_table();
    test_texture_key_suffix_and_key();

    if (g_failures == 0) std::printf("ALL TEXTURE IMPORT TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
```

Añadir además, en `engine/tests/shared_gpu_mesh_tests.cpp`, un test (y llamarlo desde su `main()`), sobre el patrón de `makeMesh` que ya tiene el fichero — leerlo primero para copiar la firma exacta de `makeMesh`:

```cpp
// Review Focus 3: dos mallas identicas con el mismo fichero de textura pero con
// ajustes de importacion distintos NO comparten entrada en VRAM.
static void test_key_changes_with_texture_import_settings()
{
    std::error_code ec;
    const fs::path d = fs::temp_directory_path(ec) / "dt_meshkey_import_test";
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);

    Mesh a = makeMesh("A");
    a.material.texturePath = (d / "t.png").string();
    const std::string sinSidecar = makeSharedMeshKey(a);

    TextureImportSettings s;
    s.mipmaps = true;
    std::string err;
    CHECK(saveTextureImportSettings(d / "t.png", s, &err));
    const std::string conMips = makeSharedMeshKey(a);
    CHECK(conMips != sinSidecar);

    // El prefijo de geometria (los dos primeros campos) no se altera: lo usa
    // rebuildStaticMesh para saber si la geometria sigue siendo la misma.
    auto prefijo = [](const std::string& k) { return k.substr(0, k.find('|', k.find('|') + 1)); };
    CHECK(prefijo(conMips) == prefijo(sinSidecar));

    // Sin sidecar de ningun tipo, la clave es la de siempre.
    Mesh b = makeMesh("A");
    b.material.texturePath = (d / "otra.png").string();
    CHECK(makeSharedMeshKey(b).find("|ts") == std::string::npos);
    fs::remove_all(d, ec);
}
```
(incluir `"DonTopo/Core/ImportSettings.h"` y `<filesystem>` en ese fichero si no están. Si `makeMesh` devuelve por valor un `Mesh`, el código de arriba vale tal cual; si devuelve `shared_ptr<Mesh>`, usar `->`.)

Y en `engine/tests/shared_texture_cache_tests.cpp`, un test:

```cpp
static void test_suffix_separates_same_file_same_kind()
{
    reiniciar();
    SharedTextureCache<int> c;
    const int a = c.acquire(makeTextureKey("a.png", {}, TextureKind::BaseColor, "#am"), crear);
    const int b = c.acquire(makeTextureKey("a.png", {}, TextureKind::BaseColor, "#lm"), crear);
    CHECK(a != b);
    CHECK(g_creadas == 2);
}
```
(añadir la llamada en su `main()`).

- [ ] **Step 2: Run to verify RED**

Registrar `dt_texture_import_tests` en `engine/tests/CMakeLists.txt`. Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error' | Select-Object -First 8`
Expected: fallo de compilación — `TextureImport.h` no existe, `makeTextureKey` no acepta 4 argumentos.

- [ ] **Step 3: Implement**

`engine/include/DonTopo/Renderer/TextureImport.h`:

```cpp
#pragma once
#include "DonTopo/Core/ImportSettings.h"
#include "DonTopo/Renderer/SharedTextureCache.h"   // TextureKind

#include <cstdint>
#include <string>
#include <vector>

namespace DonTopo
{
    // Un nivel de mip RGBA8. El nivel 0 es la propia imagen y no se repite aqui.
    struct TextureMip
    {
        uint32_t             w = 0, h = 0;
        std::vector<uint8_t> rgba;
    };

    // Niveles de una cadena completa hasta 1x1: 1 + floor(log2(max(w, h))). 1 si
    // alguna dimension es 0.
    uint32_t mipLevelCount(uint32_t w, uint32_t h);

    // Niveles 1..N-1 de la cadena (dimension max(1, d/2) cada vez, igual que
    // Vulkan y D3D12), con filtro de caja PONDERADO POR ALFA: un pixel transparente
    // no oscurece a sus vecinos opacos. Vacio si no hay nada que hacer (1x1,
    // puntero nulo, dimension 0). No modifica la entrada. Se promedia el valor
    // codificado, tambien en sRGB (limitacion conocida, ver la spec).
    std::vector<TextureMip> buildMipChain(const uint8_t* rgba, uint32_t w, uint32_t h);

    // Auto = lo decide el slot (solo el color base es sRGB); Srgb/Linear lo pisan.
    // Es el UNICO sitio que decide el formato de una textura de material.
    bool resolveSrgb(TextureKind kind, ColorSpaceOverride o);

    // Sufijo de clave de caché con los ajustes de importacion de `path`. Vacio si
    // la ruta esta vacia o los ajustes son los de siempre, asi las claves de hoy
    // no cambian. Lee el sidecar de disco.
    std::string textureKeySuffix(const std::string& path);
}
```

`engine/src/Renderer/TextureImport.cpp`:

```cpp
#include "DonTopo/Renderer/TextureImport.h"

#include <algorithm>

namespace DonTopo
{
    uint32_t mipLevelCount(uint32_t w, uint32_t h)
    {
        if (w == 0 || h == 0) return 1;
        uint32_t levels = 1;
        for (uint32_t d = std::max(w, h); d > 1; d >>= 1) ++levels;
        return levels;
    }

    std::vector<TextureMip> buildMipChain(const uint8_t* rgba, uint32_t w, uint32_t h)
    {
        std::vector<TextureMip> chain;
        if (!rgba || w == 0 || h == 0) return chain;

        const uint32_t levels = mipLevelCount(w, h);
        chain.reserve(levels > 0 ? levels - 1 : 0);

        const uint8_t* src  = rgba;
        uint32_t       sw   = w, sh = h;
        for (uint32_t level = 1; level < levels; ++level)
        {
            const uint32_t dw = std::max(1u, sw / 2);
            const uint32_t dh = std::max(1u, sh / 2);
            TextureMip mip;
            mip.w = dw;
            mip.h = dh;
            mip.rgba.assign(static_cast<size_t>(dw) * dh * 4, 0);

            for (uint32_t y = 0; y < dh; ++y)
            {
                const uint32_t y0 = static_cast<uint32_t>(static_cast<uint64_t>(y) * sh / dh);
                uint32_t       y1 = static_cast<uint32_t>(static_cast<uint64_t>(y + 1) * sh / dh);
                if (y1 <= y0) y1 = y0 + 1;
                for (uint32_t x = 0; x < dw; ++x)
                {
                    const uint32_t x0 = static_cast<uint32_t>(static_cast<uint64_t>(x) * sw / dw);
                    uint32_t       x1 = static_cast<uint32_t>(static_cast<uint64_t>(x + 1) * sw / dw);
                    if (x1 <= x0) x1 = x0 + 1;

                    uint64_t sumR = 0, sumG = 0, sumB = 0, sumA = 0;
                    for (uint32_t sy = y0; sy < y1; ++sy)
                        for (uint32_t sx = x0; sx < x1; ++sx)
                        {
                            const uint8_t* p = src + (static_cast<size_t>(sy) * sw + sx) * 4;
                            const uint64_t a = p[3];
                            sumR += p[0] * a;
                            sumG += p[1] * a;
                            sumB += p[2] * a;
                            sumA += a;
                        }
                    const uint64_t count = static_cast<uint64_t>(x1 - x0) * (y1 - y0);
                    uint8_t* d = &mip.rgba[(static_cast<size_t>(y) * dw + x) * 4];
                    if (sumA > 0)
                    {
                        d[0] = static_cast<uint8_t>((sumR + sumA / 2) / sumA);
                        d[1] = static_cast<uint8_t>((sumG + sumA / 2) / sumA);
                        d[2] = static_cast<uint8_t>((sumB + sumA / 2) / sumA);
                    }
                    d[3] = static_cast<uint8_t>((sumA + count / 2) / count);
                }
            }
            chain.push_back(std::move(mip));
            src = chain.back().rgba.data();
            sw  = dw;
            sh  = dh;
        }
        return chain;
    }

    bool resolveSrgb(TextureKind kind, ColorSpaceOverride o)
    {
        switch (o)
        {
            case ColorSpaceOverride::Srgb:   return true;
            case ColorSpaceOverride::Linear: return false;
            case ColorSpaceOverride::Auto:   break;
        }
        return kind == TextureKind::BaseColor;
    }

    std::string textureKeySuffix(const std::string& path)
    {
        if (path.empty()) return {};
        const TextureImportSettings s = loadTextureImportSettings(path);
        if (isDefault(s)) return {};
        std::string out = "#";
        out += s.colorSpace == ColorSpaceOverride::Srgb ? 's'
             : s.colorSpace == ColorSpaceOverride::Linear ? 'l' : 'a';
        if (s.mipmaps) out += 'm';
        return out;
    }
}
```

`SharedTextureCache.h` — cambiar `makeTextureKey`:

```cpp
    inline std::string makeTextureKey(const std::string& path, const std::vector<uint8_t>& embedded,
                                      TextureKind kind, const std::string& settingsSuffix = {})
    {
        if (path.empty() && embedded.empty()) return {};
        const char tipo = kind == TextureKind::BaseColor ? 'c' : (kind == TextureKind::Normal ? 'n' : 'o');
        // Los ajustes de importacion son de un FICHERO: sin ruta no hay sidecar.
        const std::string sufijo = path.empty() ? std::string() : settingsSuffix;
        if (!embedded.empty())
        {
            uint64_t h = 1469598103934665603ull;
            for (uint8_t b : embedded) { h ^= b; h *= 1099511628211ull; }
            return std::string(1, tipo) + ":emb:" + std::to_string(embedded.size()) + ":" + std::to_string(h) + sufijo;
        }
        return std::string(1, tipo) + ":" + path + sufijo;
    }
```
(`sufijo` va por valor a propósito: una referencia ligada a un ternario que mezcla un temporal y un lvalue dependería de la extensión de vida del temporal.)

`SharedGpuMesh.cpp` — incluir `"DonTopo/Renderer/TextureImport.h"` y, justo antes del `return` de `makeSharedMeshKey` (leer el final de la función, líneas ~80-100, y colocarlo tras el último `key += ...`):

```cpp
        // Ajustes de importacion de las tres texturas, AL FINAL (el prefijo de
        // geometria que compara rebuildStaticMesh son los dos primeros campos) y
        // solo si alguno no es el de siempre: las claves de hoy no cambian.
        const std::string ts0 = textureKeySuffix(m.texturePath);
        const std::string ts1 = textureKeySuffix(m.normalMapPath);
        const std::string ts2 = textureKeySuffix(m.metallicRoughnessPath);
        if (!ts0.empty() || !ts1.empty() || !ts2.empty())
        {
            key += "|ts";
            key += ts0; key += '|'; key += ts1; key += '|'; key += ts2;
        }
```

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; .\build-ninja\engine\tests\dt_texture_import_tests.exe; .\build-ninja\engine\tests\dt_shared_texture_cache_tests.exe; .\build-ninja\engine\tests\dt_shared_gpu_mesh_tests.exe`
Expected: los tres terminan con su "PASSED" y exit 0.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/Renderer/TextureImport.h engine/src/Renderer/TextureImport.cpp engine/include/DonTopo/Renderer/SharedTextureCache.h engine/src/Renderer/SharedGpuMesh.cpp engine/tests/texture_import_tests.cpp engine/tests/shared_texture_cache_tests.cpp engine/tests/shared_gpu_mesh_tests.cpp engine/CMakeLists.txt engine/tests/CMakeLists.txt
git commit -m "feat(renderer): cadena de mips en CPU, resolveSrgb y claves de cache con los ajustes de importacion"
```

---

### Task 3: `decodeMaterialTexture` lee los ajustes

**Files:**
- Modify: `engine/include/DonTopo/Renderer/MaterialTextureSource.h:42-47` (`DecodedTexture`)
- Modify: `engine/src/Renderer/MaterialTextureSource.cpp` (`decodeMaterialTexture`)
- Modify: `engine/include/DonTopo/Renderer/AsyncAssetLoader.h:18-26` (`DecodedImage`)
- Modify: `engine/src/Renderer/AsyncAssetLoader.cpp:23-37` (`decodeSlot`)
- Modify: `engine/tests/texture_import_tests.cpp` (ampliar)

**Interfaces:**
- Consumes (Tasks 1–2): `loadTextureImportSettings`, `buildMipChain`, `TextureMip`, `ColorSpaceOverride`.
- Produces:
  ```cpp
  struct DecodedTexture { int w = 0, h = 0; std::unique_ptr<unsigned char, StbPixelsFree> pixels;
                          ColorSpaceOverride colorSpace = ColorSpaceOverride::Auto;
                          std::vector<TextureMip> mips;   // niveles 1..N-1; vacio si mipmaps esta apagado
                          explicit operator bool() const; };
  struct DecodedImage { Slot slot; int w, h; std::vector<uint8_t> pixels;
                        ColorSpaceOverride colorSpace = ColorSpaceOverride::Auto;
                        std::vector<TextureMip> mips; };
  ```

- [ ] **Step 1: Write the failing tests** — añadir a `engine/tests/texture_import_tests.cpp` (incluir `"DonTopo/Renderer/MaterialTextureSource.h"` y `<fstream>`, y llamar los tests desde `main()`):

```cpp
static void writeTga(const fs::path& p, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    std::ofstream o(p, std::ios::binary);
    const uint8_t hd[18] = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                             static_cast<uint8_t>(w & 255), static_cast<uint8_t>(w >> 8),
                             static_cast<uint8_t>(h & 255), static_cast<uint8_t>(h >> 8), 32, 0x28 };
    o.write(reinterpret_cast<const char*>(hd), 18);
    for (int i = 0; i < w * h; ++i) { o.put(static_cast<char>(b)); o.put(static_cast<char>(g)); o.put(static_cast<char>(r)); o.put(static_cast<char>(a)); }
}

static void test_decode_without_sidecar_is_unchanged()
{
    const fs::path d = makeDir();
    writeTga(d / "t.tga", 8, 4, 10, 20, 30, 255);
    const DecodedTexture tex = decodeMaterialTexture((d / "t.tga").string(), {});
    CHECK(tex && tex.w == 8 && tex.h == 4);
    CHECK(tex.colorSpace == ColorSpaceOverride::Auto);
    CHECK(tex.mips.empty());
}

static void test_decode_with_sidecar_carries_settings_and_mips()
{
    const fs::path d = makeDir();
    writeTga(d / "t.tga", 8, 4, 10, 20, 30, 255);
    TextureImportSettings s;
    s.colorSpace = ColorSpaceOverride::Linear;
    s.mipmaps    = true;
    std::string err;
    CHECK(saveTextureImportSettings(d / "t.tga", s, &err));

    const DecodedTexture tex = decodeMaterialTexture((d / "t.tga").string(), {});
    CHECK(tex);
    CHECK(tex.colorSpace == ColorSpaceOverride::Linear);
    CHECK(tex.mips.size() == mipLevelCount(8, 4) - 1);          // 8x4 -> 4x2 -> 2x1 -> 1x1
    CHECK(!tex.mips.empty() && tex.mips.front().w == 4 && tex.mips.front().h == 2);
    CHECK(!tex.mips.empty() && tex.mips.back().w == 1 && tex.mips.back().h == 1);
}

static void test_decode_1x1_with_mipmaps_has_no_extra_levels()
{
    const fs::path d = makeDir();
    writeTga(d / "one.tga", 1, 1, 1, 2, 3, 255);
    TextureImportSettings s;
    s.mipmaps = true;
    std::string err;
    CHECK(saveTextureImportSettings(d / "one.tga", s, &err));
    const DecodedTexture tex = decodeMaterialTexture((d / "one.tga").string(), {});
    CHECK(tex && tex.mips.empty());
}

// Un sidecar roto no impide cargar la textura: defecto.
static void test_decode_with_broken_sidecar_falls_back_to_default()
{
    const fs::path d = makeDir();
    writeTga(d / "t.tga", 4, 4, 1, 2, 3, 255);
    std::ofstream(importSidecarPath(d / "t.tga")) << "{ roto";
    const DecodedTexture tex = decodeMaterialTexture((d / "t.tga").string(), {});
    CHECK(tex && tex.colorSpace == ColorSpaceOverride::Auto && tex.mips.empty());
}

// Las embebidas del FBX no tienen sidecar: siempre el defecto.
static void test_decode_embedded_ignores_sidecars()
{
    const fs::path d = makeDir();
    writeTga(d / "t.tga", 4, 4, 1, 2, 3, 255);
    std::ifstream in(d / "t.tga", std::ios::binary);
    const std::vector<uint8_t> bytes{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    TextureImportSettings s;
    s.mipmaps = true;
    std::string err;
    CHECK(saveTextureImportSettings(d / "t.tga", s, &err));
    const DecodedTexture tex = decodeMaterialTexture("", bytes);
    CHECK(tex && tex.colorSpace == ColorSpaceOverride::Auto && tex.mips.empty());
}
```
(añadir `#include <iterator>`.)

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error' | Select-Object -First 5`
Expected: fallo de compilación — `DecodedTexture` no tiene `colorSpace` ni `mips`.

- [ ] **Step 3: Implement**

`MaterialTextureSource.h`: `#include "DonTopo/Renderer/TextureImport.h"` y ampliar `DecodedTexture`:

```cpp
    struct DecodedTexture
    {
        int w = 0, h = 0;
        std::unique_ptr<unsigned char, StbPixelsFree> pixels;
        // Ajustes de importacion del fichero (sidecar). Las texturas embebidas y
        // las que no tienen sidecar llevan el defecto: Auto y sin mips.
        ColorSpaceOverride      colorSpace = ColorSpaceOverride::Auto;
        std::vector<TextureMip> mips;   // niveles 1..N-1; vacio si mipmaps esta apagado
        explicit operator bool() const { return pixels != nullptr; }
    };
```

`MaterialTextureSource.cpp`: dentro del `case TextureSource::Path:` no se toca el switch; añadir tras `out.pixels.reset(px);`:

```cpp
        out.pixels.reset(px);
        if (!px) { out.w = out.h = 0; return out; }

        // Solo las de FICHERO tienen sidecar. Se lee despues de decodificar bien:
        // un fichero que no se lee no gasta una lectura mas.
        if (chooseTextureSource(path, embedded) == TextureSource::Path)
        {
            std::string warning;
            const TextureImportSettings s = loadTextureImportSettings(path, &warning);
            if (!warning.empty())
                std::fprintf(stderr, "[TextureImport] %s: %s\n", path.c_str(), warning.c_str());
            out.colorSpace = s.colorSpace;
            if (s.mipmaps)
                out.mips = buildMipChain(px, static_cast<uint32_t>(out.w), static_cast<uint32_t>(out.h));
        }
        return out;
```
(sustituye el `if (!px) out.w = out.h = 0; return out;` final; añadir `#include <cstdio>`.)

`AsyncAssetLoader.h`: en `DecodedImage` (tras `pixels`), añadir `ColorSpaceOverride colorSpace = ColorSpaceOverride::Auto;` y `std::vector<TextureMip> mips;` con el `#include "DonTopo/Renderer/TextureImport.h"`. `AsyncAssetLoader.cpp` `decodeSlot`: tras `img.pixels.assign(...)`, añadir `img.colorSpace = tex.colorSpace; img.mips = tex.mips;`. Hay que copiar (no mover) `tex.mips` porque `tex` es `const`.

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; .\build-ninja\engine\tests\dt_texture_import_tests.exe; .\build-ninja\engine\tests\dt_material_texture_tests.exe; .\build-ninja\engine\tests\dt_asset_loader_tests.exe`
Expected: los tres pasan (los dos últimos no deben cambiar: sin sidecar, nada cambia).

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/Renderer/MaterialTextureSource.h engine/src/Renderer/MaterialTextureSource.cpp engine/include/DonTopo/Renderer/AsyncAssetLoader.h engine/src/Renderer/AsyncAssetLoader.cpp engine/tests/texture_import_tests.cpp
git commit -m "feat(renderer): decodeMaterialTexture lee el sidecar y devuelve espacio de color y mips"
```

---

### Task 4: Vulkan — niveles de mip, formato resuelto y sampler con LOD libre

**Files:**
- Modify: `engine/include/DonTopo/Renderer/GpuResources.h` (declaraciones de `createImage`, `uploadPixelsToImage`, `createTextureImage`, `createNormalMapImage`, `createTextureImageFromPixels` → `createMaterialImageFromPixels`, `createNormalMapImageFromPixels` (se eliminan); leerlas en las líneas ~55-105 antes de editar)
- Modify: `engine/src/Renderer/GpuResources.cpp:123-154` (`createImage`), `:156-165` (declaraciones del namespace anónimo), `:179-218` (`uploadPixelsToImage`), `:301-341` (`grabarTransicion`), `:361-426` (`createTextureImage`, `createNormalMapImage`), `:428-443` (`createTextureImageView`), `:445-462` (`createTextureSampler`), `:560-572` (funciones `FromPixels`)
- Modify: `engine/include/DonTopo/Renderer/Renderer.h:1142-1147` (`MaterialImage` + campo `format`)
- Modify: `engine/src/Renderer/Renderer.cpp:3382-3426` (`createSharedGpuMesh`), `:3973-4012` (skinned `pedir`), `:4639-4666` (rebuild)
- Modify: `engine/tests/texture_import_tests.cpp` (test de política)

**Interfaces:**
- Consumes (Tasks 2–3): `DecodedTexture::colorSpace/mips`, `DecodedImage::colorSpace/mips`, `resolveSrgb`, `TextureMip`, `textureKeySuffix`.
- Produces (firmas nuevas, `GpuResources`):
  ```cpp
  void createImage(uint32_t w, uint32_t h, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage,
                   VkMemoryPropertyFlags props, VkImage& image, VkDeviceMemory& memory, uint32_t mipLevels = 1);
  void uploadPixelsToImage(const void* pixels, uint32_t w, uint32_t h, VkFormat fmt, VkImage& img,
                           VkDeviceMemory& mem, TransferBatch* batch = nullptr,
                           const TextureMip* mips = nullptr, size_t mipCount = 0);
  void createTextureImage(const std::string& path, const std::vector<uint8_t>& embedded, VkImage& img,
                          VkDeviceMemory& mem, TransferBatch* batch = nullptr, VkFormat* outFormat = nullptr);
  void createNormalMapImage(/* idem */ ..., VkFormat* outFormat = nullptr);
  void createMaterialImageFromPixels(const uint8_t* rgba, uint32_t w, uint32_t h, VkFormat fmt,
                                     const TextureMip* mips, size_t mipCount,
                                     VkImage& img, VkDeviceMemory& mem, TransferBatch* batch);
  ```
  (`batch` conserva el valor por defecto que ya tuviera cada declaración; leerlo en el `.h`.)

- [ ] **Step 1: Write the failing policy test** — añadir a `texture_import_tests.cpp` (incluir `<fstream>`, `<sstream>`, y llamarlo desde `main()`); rojo hasta que los tres cambios de Vulkan estén hechos (y los de D3D12 los añade la Task 5):

```cpp
static std::string readAll(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
static size_t countOf(const std::string& hay, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size())) ++n;
    return n;
}

// Politica: el formato de una textura de material lo decide resolveSrgb, no un
// literal en cada uploader. Si alguien vuelve a hardcodear el formato de un slot,
// los ajustes de importacion dejan de respetarse en silencio.
static void test_policy_vulkan_material_format_comes_from_resolveSrgb()
{
    const std::string res = readAll("engine/src/Renderer/GpuResources.cpp");
    const std::string rnd = readAll("engine/src/Renderer/Renderer.cpp");
    CHECK(!res.empty() && !rnd.empty());
    CHECK(countOf(res, "resolveSrgb(") >= 2);                       // createTextureImage y createNormalMapImage
    CHECK(countOf(rnd, "createTextureImageView(obj.textureImage, obj.textureView);") == 0);
    CHECK(countOf(rnd, "createTextureImageView(gpu.textureImage, gpu.textureView);") == 0);
    CHECK(countOf(rnd, "createTextureImageView(mgfx.textureImage, mgfx.textureView);") == 0);
    CHECK(countOf(rnd, "normalView, VK_FORMAT_R8G8B8A8_UNORM)") == 0);
    CHECK(countOf(rnd, "ormView, VK_FORMAT_R8G8B8A8_UNORM)") == 0);
}
```

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; .\build-ninja\engine\tests\dt_texture_import_tests.exe`
Expected: `FAIL: countOf(res, "resolveSrgb(") >= 2` y los seis `countOf(rnd, ...) == 0`.

- [ ] **Step 3: Implement (GpuResources)**

1. **`createImage`**: añadir el parámetro final `uint32_t mipLevels = 1` (en el `.h`, no repetir el valor por defecto en el `.cpp`) y sustituir `imageInfo.mipLevels = 1;` por `imageInfo.mipLevels = mipLevels;`.

2. **`grabarTransicion`**: añadir el parámetro `uint32_t levelCount = 1` al prototipo del namespace anónimo (`:161`, el valor por defecto va SOLO en el prototipo) y en la definición; `barrier.subresourceRange.levelCount = levelCount;`. `transitionImageLayout` y `createBlankImage` siguen llamando con el defecto (1).

3. **`uploadPixelsToImage`**: sustituir el cuerpo completo (líneas 179–218) por:

```cpp
void GpuResources::uploadPixelsToImage(const void* pixels, uint32_t w, uint32_t h, VkFormat fmt,
                                       VkImage& img, VkDeviceMemory& mem, TransferBatch* batch,
                                       const TextureMip* mips, size_t mipCount)
{
    const uint32_t levels = 1 + static_cast<uint32_t>(mipCount);

    VkDeviceSize total = static_cast<VkDeviceSize>(w) * h * 4;
    for (size_t i = 0; i < mipCount; ++i)
        total += static_cast<VkDeviceSize>(mips[i].w) * mips[i].h * 4;

    VkBuffer       staging    = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    createBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 staging, stagingMem);

    void* data = nullptr;
    vkMapMemory(m_gpu.device(), stagingMem, 0, total, 0, &data);

    // Todos los niveles en UN staging y UNA llamada de copia: el nivel i ocupa
    // w*h*4 bytes (multiplo de 4, el alineado que pide RGBA8) a continuacion del
    // anterior.
    std::vector<VkBufferImageCopy> regions(levels);
    VkDeviceSize offset = 0;
    auto poner = [&](uint32_t level, const void* src, uint32_t lw, uint32_t lh) {
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(lw) * lh * 4;
        memcpy(static_cast<uint8_t*>(data) + offset, src, static_cast<size_t>(bytes));
        VkBufferImageCopy& r = regions[level];
        r                                 = {};
        r.bufferOffset                    = offset;
        r.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.mipLevel       = level;
        r.imageSubresource.baseArrayLayer = 0;
        r.imageSubresource.layerCount     = 1;
        r.imageExtent                     = { lw, lh, 1 };
        offset += bytes;
    };
    poner(0, pixels, w, h);
    for (size_t i = 0; i < mipCount; ++i)
        poner(static_cast<uint32_t>(i) + 1, mips[i].rgba.data(), mips[i].w, mips[i].h);
    vkUnmapMemory(m_gpu.device(), stagingMem);

    createImage(w, h, fmt, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img, mem, levels);

    // Un solo scope para las tres: sin batch eso es un submit en vez de tres. La
    // barrera cubre TODOS los niveles: un nivel sin transicionar es exactamente el
    // aviso de layout que la validacion de sincronizacion caza.
    {
        CmdScope scope(m_gpu, batch);
        grabarTransicion(scope.cmd, img, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels);
        vkCmdCopyBufferToImage(scope.cmd, staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               levels, regions.data());
        grabarTransicion(scope.cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, levels);
    }
    // El destructor del scope ya ha esperado si no habia batch, asi que el
    // staging se puede soltar. Con batch la copia sigue en vuelo y se libera al
    // senalar la fence.
    if (batch)
        batch->addStaging(staging, stagingMem);
    else
    {
        vkDestroyBuffer(m_gpu.device(), staging, nullptr);
        vkFreeMemory(m_gpu.device(), stagingMem, nullptr);
    }
}
```
(`grabarCopiaABufferImagen` se sigue usando desde `copyBufferToImage`; no borrarla.)

4. **`createTextureImageView`**: `viewInfo.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;` (cubre imágenes de 1 nivel y de N).

5. **`createTextureSampler`**: añadir `samplerInfo.minLod = 0.0f; samplerInfo.maxLod = VK_LOD_CLAMP_NONE;` (con `maxLod` a 0 los mips no se leerían nunca). Las imágenes de un solo nivel siguen leyendo el nivel 0, así que `UiSpriteBatch`, que comparte este sampler, no cambia.

6. **`createTextureImage` / `createNormalMapImage`**: añadir el último parámetro `VkFormat* outFormat = nullptr` y hacer que **todas** las salidas lo rellenen:
   - `createTextureImage`: rama de blanco compartido → `if (outFormat) *outFormat = VK_FORMAT_R8G8B8A8_SRGB;`. Con textura: `const bool srgb = resolveSrgb(TextureKind::BaseColor, tex.colorSpace); const VkFormat fmt = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;` **solo si `tex` decodificó**; si no hay píxeles (damero / blanco 1×1), `fmt = VK_FORMAT_R8G8B8A8_SRGB` como hoy. Al final `if (outFormat) *outFormat = fmt;` y `uploadPixelsToImage(pixels, w, h, fmt, img, mem, batch, tex.mips.data(), tex.mips.size());` (los mips son vacíos cuando se usó un relleno, porque `tex` no decodificó).
   - `createNormalMapImage`: igual con `TextureKind::Normal`; plana compartida → `UNORM`; relleno plano → `UNORM`.
   `createNormalMapImage` se usa también para ORM: `resolveSrgb(Normal, o) == resolveSrgb(Orm, o)` para todo `o`, así que no hace falta un tercer `kind`.

7. **Sustituir** `createTextureImageFromPixels` y `createNormalMapImageFromPixels` (`:560-572`, y sus declaraciones en el `.h`, y no las usa nada más que `Renderer.cpp`) por `createMaterialImageFromPixels(rgba, w, h, fmt, mips, mipCount, img, mem, batch)` que llama a `uploadPixelsToImage(rgba, w, h, fmt, img, mem, batch, mips, mipCount)`.

- [ ] **Step 4: Implement (Renderer)**

1. **`Renderer.h` `MaterialImage`**: añadir `VkFormat format = VK_FORMAT_UNDEFINED;` (no entra en `operator==`).

2. **`createSharedGpuMesh` (`Renderer.cpp:3382-3426`)**. Declarar tres formatos y usarlos en imagen y vista:

```cpp
        VkFormat albedoFmt = VK_FORMAT_R8G8B8A8_SRGB;
        if (const DecodedImage* albedo = findSlot(DecodedImage::Albedo))
        {
            albedoFmt = resolveSrgb(TextureKind::BaseColor, albedo->colorSpace)
                            ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
            m_res.createMaterialImageFromPixels(albedo->pixels.data(),
                                                (uint32_t)albedo->w, (uint32_t)albedo->h, albedoFmt,
                                                albedo->mips.data(), albedo->mips.size(),
                                                obj.textureImage, obj.textureMem, batch);
        }
        else
            m_res.createTextureImage(mesh.material.texturePath, mesh.material.embeddedTexture,
                                     obj.textureImage, obj.textureMem, batch, &albedoFmt);
        m_res.createTextureImageView(obj.textureImage, obj.textureView, albedoFmt);
```
   y análogo para normal (`TextureKind::Normal`, `normalFmt` por defecto `UNORM`, vista `createTextureImageView(obj.normalImage, obj.normalView, normalFmt)`) y ORM (`TextureKind::Orm`, `ormFmt` por defecto `UNORM`; en las ramas `createNormalMapImage(..., batch, &ormFmt)` y de blanca compartida `ormFmt` queda en `UNORM`; vista `createTextureImageView(obj.ormImage, obj.ormView, ormFmt)`).

3. **Skinned `pedir` (`Renderer.cpp:3973-4012`)**: la clave lleva el sufijo y el formato viaja en `MaterialImage`:

```cpp
            auto pedir = [&](const std::string& ruta, const std::vector<uint8_t>& emb, TextureKind tipo,
                             VkImage& img, VkDeviceMemory& mem, VkFormat& fmt) {
                const MaterialImage m = m_skinnedTextures.acquire(
                    makeTextureKey(ruta, emb, tipo, textureKeySuffix(ruta)), [&] {
                    MaterialImage nueva;
                    if (tipo == TextureKind::BaseColor)
                        m_res.createTextureImage(ruta, emb, nueva.image, nueva.mem, batch, &nueva.format);
                    else
                        m_res.createNormalMapImage(ruta, emb, nueva.image, nueva.mem, batch, &nueva.format);
                    return nueva;
                });
                img = m.image;
                mem = m.mem;
                fmt = m.format;
            };
```
   Con acierto de caché, `acquire` devuelve la entrada guardada, que ya lleva su `format`. Con clave vacía (sin textura) el `create` se ejecuta y también rellena `format`. Declarar `VkFormat albedoFmt, normalFmt, ormFmt` por material y usar las tres en `createTextureImageView(...)`. Caso ORM sin textura (`sharedWhiteOrm`): `ormFmt = VK_FORMAT_R8G8B8A8_UNORM`.

4. **Rebuild (`Renderer.cpp:4639-4666`)**: mismo patrón con `&albedoFmt`, `&normalFmt`, `&ormFmt` (síncrono, sin batch) y vistas con esos formatos. Ajustar el comentario "SRGB en la difusa (el que createTextureImage hardcodea)" — ahora dice "el formato que devuelve createTextureImage".

5. Comprobar con `git grep -n "createTextureImageFromPixels\|createNormalMapImageFromPixels" -- engine` que no queda ningún llamante.

- [ ] **Step 5: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C|error:' | Select-Object -First 8; .\build-ninja\engine\tests\dt_texture_import_tests.exe`
Expected: sin errores de compilación; el test de política pasa (D3D12 aún no entra en él).
Luego la suite completa desde la raíz:

```powershell
$fail=@(); Get-ChildItem .\build-ninja\engine\tests\dt_*.exe | ForEach-Object { & $_.FullName > $null 2>&1; if ($LASTEXITCODE -ne 0) { $fail += $_.Name } }; "fallos: $($fail -join ', ')"
```
Expected: `fallos: ` (vacío). Smoke: comprobar en `project.json` del último proyecto que el backend es Vulkan y lanzar `.\build-ninja\sandbox\Sandbox.exe` unos 6 s (sin tocar la GUI el proyecto no se abre: solo prueba que arranca). **Nunca dejar modificado `project.json`** (si hay que forzar un backend, respaldarlo antes y restaurarlo verificando que queda byte-idéntico).

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Renderer/GpuResources.h engine/src/Renderer/GpuResources.cpp engine/include/DonTopo/Renderer/Renderer.h engine/src/Renderer/Renderer.cpp engine/tests/texture_import_tests.cpp
git commit -m "feat(vulkan): texturas de material con mips, formato resuelto por resolveSrgb y sampler con LOD libre"
```

---

### Task 5: D3D12 — niveles de mip y formato resuelto

**Files:**
- Modify: `engine/src/Renderer/D3D12/D3D12Renderer.cpp`: declaraciones `:1763` (`uploadTexture`) y `:1770` (`uploadMaterialTexture`); `uploadTexture` `:2474-2599`; `uploadMaterialTexture` `:2601-2617`; `createTexture2DSrv` `:2619-2631`; `pedirTextura` `:3537-3549`; reuso en `addStaticMesh` `:10092-10144`; `rebuildStaticMesh` `:10792-10818`
- Modify: `engine/tests/texture_import_tests.cpp` (ampliar el test de política)

**Interfaces:**
- Consumes (Tasks 2–3): `DecodedTexture::colorSpace/mips`, `resolveSrgb`, `TextureKind`, `textureKeySuffix`.
- Produces:
  ```cpp
  D3D12MA::Allocation* uploadTexture(const void* pixels, UINT width, UINT height, UINT arraySize,
                                     DXGI_FORMAT format, UINT bytesPerPixel, UINT srvIndex,
                                     const TextureMip* mips = nullptr, size_t mipCount = 0);
  D3D12MA::Allocation* uploadMaterialTexture(const std::string& path, const std::vector<uint8_t>& embedded,
                                             TextureKind kind, UINT srvIndex);   // antes: bool srgb
  static DXGI_FORMAT   formatOf(D3D12MA::Allocation* a);                            // a->GetResource()->GetDesc().Format
  ```
  (Los `mips` solo valen con `arraySize == 1`.)

- [ ] **Step 1: Extend the failing policy test** — añadir a `texture_import_tests.cpp`:

```cpp
static void test_policy_d3d12_material_format_comes_from_resolveSrgb()
{
    const std::string d3d = readAll("engine/src/Renderer/D3D12/D3D12Renderer.cpp");
    CHECK(!d3d.empty());
    CHECK(countOf(d3d, "resolveSrgb(") >= 1);                                       // uploadMaterialTexture
    // La regla antigua ("srgb ? UNORM_SRGB : UNORM" segun el slot) y las vistas de
    // reuso con el formato escrito a mano ya no existen.
    CHECK(countOf(d3d, "srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM") == 0);
    CHECK(countOf(d3d, "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, slot + 0)") == 0);
    // Y la firma de uploadMaterialTexture ya no recibe un bool srgb.
    CHECK(countOf(d3d, "const std::string& path, const std::vector<uint8_t>& embedded, bool srgb, UINT srvIndex") == 0);
}
```
(llamarlo desde `main()`).

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; .\build-ninja\engine\tests\dt_texture_import_tests.exe`
Expected: FAIL en los cuatro `countOf(d3d, ...)`.

- [ ] **Step 3: Implement**

1. **`uploadTexture`**: nuevos parámetros finales `const TextureMip* mips = nullptr, size_t mipCount = 0` (en la declaración de la línea 1763; el valor por defecto solo allí). Cambios en el cuerpo:
   - `const UINT mipLevels = 1 + static_cast<UINT>(mipCount);` (con `arraySize > 1` los mips no se usan: `mipCount` vale 0) y `texDesc.MipLevels = static_cast<UINT16>(mipLevels);`.
   - `const UINT subresources = mipLevels * arraySize;` y dimensionar `footprints`, `rowCounts`, `rowSizes` con `subresources`; `GetCopyableFootprints(&texDesc, 0, subresources, 0, ...)`.
   - Bucle de copia al staging: para cada `i` en `[0, subresources)`: `slice = i / mipLevels`, `level = i % mipLevels`; origen y ancho de fila de origen:
     ```cpp
     const uint8_t* src = level == 0
         ? source + static_cast<UINT64>(slice) * height * width * bytesPerPixel
         : mips[level - 1].rgba.data();
     const UINT srcWidth = level == 0 ? width : mips[level - 1].w;
     for (UINT row = 0; row < rowCounts[i]; ++row)
         std::memcpy(mapped + footprints[i].Offset + static_cast<UINT64>(row) * footprints[i].Footprint.RowPitch,
                     src + static_cast<UINT64>(row) * srcWidth * bytesPerPixel,
                     static_cast<size_t>(rowSizes[i]));
     ```
   - Bucle de `CopyTextureRegion`: `dst.SubresourceIndex = i;` y `src.PlacedFootprint = footprints[i];` para cada `i`.
   - SRV: `srvDesc.Texture2D.MipLevels = mipLevels;` (rama `arraySize == 1`); la rama de array se queda en `1`.

2. **`uploadMaterialTexture`**: firma con `TextureKind kind` en lugar de `bool srgb`; dentro:
   ```cpp
   const bool srgb = resolveSrgb(kind, tex.colorSpace);
   const DXGI_FORMAT format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
   return uploadTexture(tex.pixels.get(), static_cast<UINT>(tex.w), static_cast<UINT>(tex.h), 1,
                        format, 4, srvIndex, tex.mips.data(), tex.mips.size());
   ```
   Conservar el comentario "sRGB para el color base y lineal para las normales", reescrito para que diga que el slot da el valor por defecto y el sidecar puede pisarlo.

3. **`createTexture2DSrv`**: `srvDesc.Texture2D.MipLevels = static_cast<UINT>(-1);` (todos los niveles desde el más detallado; para un recurso de 1 nivel equivale a 1).

4. **`formatOf`**: helper estático en el mismo `.cpp`, antes de su primer uso: `static DXGI_FORMAT formatOf(D3D12MA::Allocation* a) { return a->GetResource()->GetDesc().Format; }`. **Es lo que evita guardar el formato en cada objeto**: el recurso ya lo sabe y las vistas de reuso lo leen de él.

5. **Llamantes de `uploadMaterialTexture`**:
   - `pedirTextura` (`:3537`): quitar `const bool srgb = ...`; `skinnedTextures.acquire(makeTextureKey(ruta, emb, tipo, textureKeySuffix(ruta)), [&] { return uploadMaterialTexture(ruta, emb, tipo, srvIndex); }, &creada)`; en acierto `createTexture2DSrv(a->GetResource(), formatOf(a), srvIndex);`.
   - `addStaticMesh` reuso (`:10092-10113`): las tres `createTexture2DSrv(object.xxxAllocation->GetResource(), <literal>, slot+N)` de recursos PROPIOS pasan a `formatOf(object.xxxAllocation)`; los de la rama `else` (neutros globales `d.baseColorAllocation`, etc.) conservan `DXGI_FORMAT_R8G8B8A8_UNORM`.
   - `addStaticMesh` no reuso (`:10115-10144`): `true`→`TextureKind::BaseColor`, la normal `false`→`TextureKind::Normal`, el ORM `false`→`TextureKind::Orm`.
   - `rebuildStaticMesh` (`:10792-10818`): idem (`BaseColor`, `Normal`, `Orm`).
   Comprobar con `git grep -n "uploadMaterialTexture(" -- engine` que no queda ningún llamante con `true`/`false`.

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C|error:' | Select-Object -First 8; .\build-ninja\engine\tests\dt_texture_import_tests.exe`
Expected: compila y la política pasa. Suite completa como en la Task 4. Smoke D3D12 (solo si es viable sin tocar `project.json` del usuario; si no, dejarlo anotado como verificación del usuario en el ledger con `Ruling:`).

- [ ] **Step 5: Commit**

```bash
git add engine/src/Renderer/D3D12/D3D12Renderer.cpp engine/tests/texture_import_tests.cpp
git commit -m "feat(d3d12): texturas de material con mips y formato resuelto por resolveSrgb"
```

---

### Task 6: El sidecar acompaña al asset (listado, mover, renombrar, borrar, importar)

**Files:**
- Modify: `engine/include/DonTopo/Core/ImportSettings.h`, `engine/src/Core/ImportSettings.cpp` (helpers de ciclo de vida)
- Modify: `engine/include/DonTopo/Editor/ContentBrowserPanel.h` (declarar `renameAssetFile`, `removeAssetPath`), `engine/src/Editor/ContentBrowserPanel.cpp` (`listVisibleEntries` `:240`, `moveAsset` `:351`, renombrar `:1395-1420`, borrar `:1457-1475`)
- Modify: `engine/src/Editor/AssetImport.cpp` (`importExternalAsset`, `describeImportResult`)
- Modify: `engine/tests/import_settings_tests.cpp`, `engine/tests/content_browser_tests.cpp`, `engine/tests/asset_import_tests.cpp`

**Interfaces:**
- Consumes (Task 1): `importSidecarPath`, `isImportSidecar`.
- Produces:
  ```cpp
  // ImportSettings.h
  bool importSidecarConflict(const std::filesystem::path& from, const std::filesystem::path& to); // ambos sidecars existen
  bool moveImportSidecar(const std::filesystem::path& oldAsset, const std::filesystem::path& newAsset, std::string* error = nullptr); // true si no habia
  bool copyImportSidecar(const std::filesystem::path& srcAsset, const std::filesystem::path& dstAsset, std::string* error = nullptr); // true si no habia
  void removeImportSidecar(const std::filesystem::path& asset);                                    // silencioso
  // ContentBrowserPanel.h
  struct RenameFileOutcome { bool ok = false; std::string error; std::string warning; };
  RenameFileOutcome renameAssetFile(const std::filesystem::path& from, const std::filesystem::path& to, bool isDir);
  std::error_code   removeAssetPath(const std::filesystem::path& path, bool isDir);
  ```

- [ ] **Step 1: Write the failing tests**

En `import_settings_tests.cpp` (llamar desde `main()`):

```cpp
static void test_move_copy_remove_sidecar()
{
    const fs::path d = makeDir();
    TextureImportSettings s;
    s.mipmaps = true;
    std::string err;

    // Sin sidecar: mover y copiar son un no-op que va bien.
    CHECK(moveImportSidecar(d / "a.png", d / "b.png", &err));
    CHECK(copyImportSidecar(d / "a.png", d / "c.png", &err));
    CHECK(!fs::exists(importSidecarPath(d / "b.png")));

    CHECK(saveTextureImportSettings(d / "a.png", s, &err));
    CHECK(copyImportSidecar(d / "a.png", d / "c.png", &err));
    CHECK(loadTextureImportSettings(d / "c.png") == s);
    CHECK(fs::exists(importSidecarPath(d / "a.png")));           // copiar no quita el original

    CHECK(moveImportSidecar(d / "a.png", d / "b.png", &err));
    CHECK(!fs::exists(importSidecarPath(d / "a.png")));
    CHECK(loadTextureImportSettings(d / "b.png") == s);

    removeImportSidecar(d / "b.png");
    CHECK(!fs::exists(importSidecarPath(d / "b.png")));
    removeImportSidecar(d / "b.png");                             // ya no hay: no pasa nada
}

// Review Focus 4: si el destino YA tiene sidecar, hay conflicto y no se pisa.
static void test_sidecar_conflict_detection()
{
    const fs::path d = makeDir();
    TextureImportSettings s;
    s.mipmaps = true;
    std::string err;
    CHECK(!importSidecarConflict(d / "a.png", d / "b.png"));
    CHECK(saveTextureImportSettings(d / "a.png", s, &err));
    CHECK(!importSidecarConflict(d / "a.png", d / "b.png"));      // solo el origen
    CHECK(saveTextureImportSettings(d / "b.png", s, &err));
    CHECK(importSidecarConflict(d / "a.png", d / "b.png"));
}
```

En `content_browser_tests.cpp` (patrón `makeFixture`/`CHECK` del propio fichero; leer cómo crea sus carpetas temporales y reutilizarlo; incluir `DonTopo/Core/ImportSettings.h`):

```cpp
static void test_list_hides_import_sidecars()
{
    // carpeta con foto.png, foto.png.import.json y escena.json
    // -> listVisibleEntries devuelve foto.png y escena.json, NO el sidecar.
}
static void test_move_asset_carries_sidecar()
{
    // foto.png con sidecar en A/ -> moveAsset(A/foto.png, B/): B/foto.png y
    // B/foto.png.import.json existen, A/ no tiene ninguno.
}
// Review Focus 4.
static void test_move_asset_rejects_when_destination_sidecar_exists()
{
    // A/foto.png con sidecar; B/foto.png NO existe pero B/foto.png.import.json SI
    // (huerfano) -> moveAsset devuelve RejectedNameConflict; A/foto.png y su sidecar
    // siguen donde estaban; el sidecar de B no se ha tocado (mismo contenido).
}
static void test_rename_asset_file_carries_sidecar_and_rejects_conflict()
{
    // renameAssetFile(A/x.png, A/y.png, false) con sidecar en x -> ok, y.png.import.json existe.
    // Con y.png.import.json ya existente -> ok == false, error no vacio, x.png y su sidecar intactos.
    // Una carpeta (isDir == true) se renombra sin tocar sidecars.
}
// Review Focus 5.
static void test_remove_asset_path_removes_sidecar()
{
    // removeAssetPath(A/foto.png, false) -> no queda ni foto.png ni foto.png.import.json.
    // removeAssetPath(carpeta, true) -> la carpeta y todo lo de dentro desaparece.
    // Un sidecar HUERFANO (sin asset) no da error al listar ni al mover otro asset.
}
```
Escribir el cuerpo de cada uno completo con el helper de fixture del fichero (crear ficheros con `std::ofstream(p) << "x"`, escribir el sidecar con `saveTextureImportSettings`, comprobar con `fs::exists`/`loadTextureImportSettings`); los comentarios de arriba son el contrato de cada test, no un sustituto de su código. Llamarlos desde `main()`.

En `asset_import_tests.cpp`:

```cpp
static void test_import_external_copies_sidecar()
{
    // origen: source/foto.png + source/foto.png.import.json; importExternalAsset -> Copied,
    // dest/foto.png.import.json existe con los mismos ajustes; el origen conserva su sidecar.
    // Sin sidecar en el origen: Copied y no aparece ninguno en el destino.
}
```

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error' | Select-Object -First 8`
Expected: fallo de compilación — `moveImportSidecar`, `importSidecarConflict`, `copyImportSidecar`, `removeImportSidecar`, `renameAssetFile`, `removeAssetPath` no existen.

- [ ] **Step 3: Implement**

`ImportSettings.h/.cpp` — añadir:

```cpp
bool importSidecarConflict(const std::filesystem::path& from, const std::filesystem::path& to)
{
    std::error_code a, b;
    return std::filesystem::exists(importSidecarPath(from), a) && !a &&
           std::filesystem::exists(importSidecarPath(to), b) && !b;
}

bool moveImportSidecar(const std::filesystem::path& oldAsset, const std::filesystem::path& newAsset,
                       std::string* error)
{
    const std::filesystem::path from = importSidecarPath(oldAsset);
    std::error_code ec;
    if (!std::filesystem::exists(from, ec) || ec) return true;        // nada que mover
    std::filesystem::rename(from, importSidecarPath(newAsset), ec);
    if (ec) { if (error) *error = ec.message(); return false; }
    return true;
}

bool copyImportSidecar(const std::filesystem::path& srcAsset, const std::filesystem::path& dstAsset,
                       std::string* error)
{
    const std::filesystem::path from = importSidecarPath(srcAsset);
    std::error_code ec;
    if (!std::filesystem::exists(from, ec) || ec) return true;
    std::filesystem::copy_file(from, importSidecarPath(dstAsset),
                               std::filesystem::copy_options::skip_existing, ec);
    if (ec) { if (error) *error = ec.message(); return false; }
    return true;
}

void removeImportSidecar(const std::filesystem::path& asset)
{
    std::error_code ec;
    std::filesystem::remove(importSidecarPath(asset), ec);
}
```
(declaraciones en el `.h`).

`ContentBrowserPanel.cpp`:
- `listVisibleEntries`: dentro del bucle, tras el filtro de carpetas ocultas: `if (isFile && isImportSidecar(entry.path())) continue;`.
- `moveAsset`: tras el `exists(dest)` (conflicto de nombre): `if (!isDir && importSidecarConflict(src, dest)) return { MoveResult::RejectedNameConflict, {}, "" };`. Tras el `rename` correcto: `if (!isDir) { std::string sidecarError; if (!moveImportSidecar(src, dest, &sidecarError)) return { MoveResult::Moved, dest, "El asset se movio pero no su .import.json: " + sidecarError }; }`. Buscar con `git grep -n "moveAsset(" -- engine/src` los llamantes y hacer que registren en el log (`ctx.pushLog`) cualquier `message` no vacío de un `Moved`.
- Nuevas funciones:

```cpp
RenameFileOutcome renameAssetFile(const std::filesystem::path& from, const std::filesystem::path& to, bool isDir)
{
    RenameFileOutcome out;
    if (!isDir && importSidecarConflict(from, to))
    {
        out.error = "Ya existe un .import.json con ese nombre";
        return out;
    }
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (ec) { out.error = ec.message(); return out; }
    out.ok = true;
    if (!isDir)
    {
        std::string sidecarError;
        if (!moveImportSidecar(from, to, &sidecarError))
            out.warning = "El asset se renombro pero no su .import.json: " + sidecarError;
    }
    return out;
}

std::error_code removeAssetPath(const std::filesystem::path& path, bool isDir)
{
    std::error_code ec;
    if (isDir) std::filesystem::remove_all(path, ec);
    else       std::filesystem::remove(path, ec);
    if (!ec && !isDir) removeImportSidecar(path);
    return ec;
}
```
- Panel de renombrar (`:1406-1419`): sustituir `std::filesystem::rename(...)` por `const RenameFileOutcome r = renameAssetFile(m_assetRenameTarget, newPath, m_assetRenameIsDir);`; en error `m_assetRenameError = r.error`; en éxito, además de lo que ya hace, `if (!r.warning.empty()) ctx.pushLog(r.warning);`.
- Panel de borrar (`:1459-1463`): sustituir el `remove_all`/`remove` por `const std::error_code removeEc = removeAssetPath(target, isDir);`.

`AssetImport.cpp`: en `importExternalAsset`, tras el `copy_file` correcto y antes del `return Copied`: 

```cpp
    std::string sidecarError;
    if (!copyImportSidecar(source, dest, &sidecarError))
        return { AssetImportResult::Copied, dest, "no se pudo copiar el .import.json: " + sidecarError, source };
    return { AssetImportResult::Copied, dest, "", source };
```
y `describeImportResult`: `case Copied: return outcome.errorMessage.empty() ? "importado" : "importado (" + outcome.errorMessage + ")";`.

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; .\build-ninja\engine\tests\dt_import_settings_tests.exe; .\build-ninja\engine\tests\dt_content_browser_tests.exe; .\build-ninja\engine\tests\dt_asset_import_tests.exe`
Expected: los tres pasan. Sabotaje: quitar temporalmente el `if (isFile && isImportSidecar(...)) continue;` y comprobar que `test_list_hides_import_sidecars` falla; restaurarlo.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/Core/ImportSettings.h engine/src/Core/ImportSettings.cpp engine/include/DonTopo/Editor/ContentBrowserPanel.h engine/src/Editor/ContentBrowserPanel.cpp engine/src/Editor/AssetImport.cpp engine/tests/import_settings_tests.cpp engine/tests/content_browser_tests.cpp engine/tests/asset_import_tests.cpp
git commit -m "feat(editor): el sidecar de importacion acompana al asset al mover, renombrar, borrar e importar"
```

---

### Task 7: El exportador copia el sidecar

**Files:**
- Modify: `engine/src/Editor/GameExporter.cpp` (`collectSceneAssets`, `:214-264`)
- Modify: `engine/tests/exporter_tests.cpp`

**Interfaces:**
- Consumes (Task 1): `importSidecarPath`.
- Produces: `collectSceneAssets` devuelve además un `ExportAsset` por cada `<textura de material>.import.json` que exista (`packagePath` = el de la textura + `.import.json`).

- [ ] **Step 1: Write the failing test** — en `exporter_tests.cpp` (incluir `"DonTopo/Core/ImportSettings.h"`; llamarlo desde `main()` con el `root` del fixture, como los demás):

```cpp
// El runtime lee "<textura>.import.json" relativo a la textura: tiene que viajar
// en el paquete con la misma jerarquia. Solo las texturas de MATERIAL.
static void test_texture_sidecar_travels_with_the_texture(const fs::path& root)
{
    std::error_code ec;
    const fs::path tex = root / "assets" / "tablero.png";
    std::ofstream(tex) << "png";
    TextureImportSettings s;
    s.mipmaps = true;
    std::string err;
    CHECK(saveTextureImportSettings(tex, s, &err));

    const fs::path plain = root / "assets" / "liso.png";        // sin sidecar
    std::ofstream(plain) << "png";

    Scene scene;
    auto* go = scene.addGameObject("suelo");
    go->setMesh(makeMesh(root / "assets" / "hero.fbx", tex));
    auto* go2 = scene.addGameObject("pared");
    go2->setMesh(makeMesh(root / "assets" / "chars" / "enemy.fbx", plain));

    const std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    std::vector<std::string> pkg;
    for (const ExportAsset& a : assets) pkg.push_back(a.packagePath);

    CHECK(std::find(pkg.begin(), pkg.end(), "assets/tablero.png")             != pkg.end());
    CHECK(std::find(pkg.begin(), pkg.end(), "assets/tablero.png.import.json") != pkg.end());
    CHECK(std::find(pkg.begin(), pkg.end(), "assets/liso.png.import.json")    == pkg.end());
    for (const ExportAsset& a : assets)
        if (a.packagePath == "assets/tablero.png.import.json") CHECK(a.existsOnDisk);

    fs::remove(tex, ec);
    fs::remove(importSidecarPath(tex), ec);
    fs::remove(plain, ec);
}
```

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; .\build-ninja\engine\tests\dt_exporter_tests.exe`
Expected: `FAIL: ... "assets/tablero.png.import.json" != pkg.end()`.

- [ ] **Step 3: Implement** — en `collectSceneAssets`, tras la lambda `add` (línea ~244) y antes de `scene.traverse`, añadir:

```cpp
    // Una textura de material con ajustes de importacion lleva su sidecar: el
    // runtime lo busca junto a la textura. Es un ExportAsset mas: comparte la
    // carpeta de origen, asi que la numeracion de assets/_external/N y la
    // jerarquia dentro del proyecto salen iguales que las de la textura.
    auto addTexture = [&](const std::string& raw)
    {
        if (raw.empty()) return;
        add(raw);
        const fs::path sidecar = importSidecarPath(fs::path(raw));
        std::error_code sec;
        if (fs::exists(sidecar, sec) && !sec)
            add(sidecar.string());
    };
```
y en el bucle de materiales (`:258-264`) sustituir `add(m->texturePath); add(m->normalMapPath); add(m->metallicRoughnessPath);` por `addTexture(...)` de las tres. Incluir `"DonTopo/Core/ImportSettings.h"`.

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; .\build-ninja\engine\tests\dt_exporter_tests.exe`
Expected: pasa entero (incluidos los tests de recuento existentes: solo se añade el sidecar cuando existe).

- [ ] **Step 5: Commit**

```bash
git add engine/src/Editor/GameExporter.cpp engine/tests/exporter_tests.cpp
git commit -m "feat(export): el paquete incluye el sidecar de importacion de las texturas de material"
```

---

### Task 8: Modal "Import Settings…" y reconstrucción de materiales

**Files:**
- Modify: `engine/include/DonTopo/Editor/ContentBrowserPanel.h` (`TextureImportApplyResult`, `applyTextureImportSettings`, estado del modal)
- Modify: `engine/src/Editor/ContentBrowserPanel.cpp` (menú contextual `:1261-1282`, modal junto a los de Rename/Delete `:1361-1500`)
- Modify: `engine/tests/content_browser_tests.cpp`

**Interfaces:**
- Consumes (Tasks 1, 6): `saveTextureImportSettings`, `loadTextureImportSettings`, `TextureImportSettings`; `materialsOf`/`tocaAlgunMaterial`/`samePath` (ya en el `.cpp`), `EditorRenderer::rebuildStaticMesh/rebuildSkinnedMesh`.
- Produces:
  ```cpp
  struct TextureImportApplyResult { bool ok = false; std::string error; int refreshed = 0; };
  TextureImportApplyResult applyTextureImportSettings(GameObject* sceneRoot, const std::filesystem::path& asset,
                                                      const TextureImportSettings& settings,
                                                      const std::function<void(GameObject&)>& rebuild);
  ```

- [ ] **Step 1: Write the failing test** — en `content_browser_tests.cpp` (llamarlo desde `main()`; reutilizar `makeSkinnedFixture` o construir un `GameObject` con `Mesh` a mano como hace `exporter_tests.cpp`):

```cpp
static void test_apply_writes_sidecar_and_rebuilds_only_users()
{
    // Escena: A usa foto.png, B usa otra.png, C sin mesh.
    // apply(root, foto.png, {Linear, mipmaps}, rebuild) -> ok, refreshed == 1,
    // rebuild se llamo solo con A, y foto.png.import.json existe con los ajustes.
    // apply con el defecto -> borra el sidecar y sigue reconstruyendo A (vuelve al
    // comportamiento anterior).
}
// Review Focus 6.
static void test_apply_reports_write_failure_and_rebuilds_nothing()
{
    // asset dentro de una carpeta que no existe: ok == false, error no vacio,
    // refreshed == 0 y rebuild NO se llamo.
}
static void test_apply_without_renderer_still_writes()
{
    // rebuild vacio (std::function nula): ok, sidecar escrito, refreshed == 0.
}
```
Cuerpos completos con el patrón de fixtures del fichero (contador de llamadas en una `std::vector<GameObject*>` capturada por el lambda `rebuild`).

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error' | Select-Object -First 5`
Expected: `applyTextureImportSettings` no existe.

- [ ] **Step 3: Implement**

`ContentBrowserPanel.cpp` (namespace `DonTopo`, junto a `countSceneReferences`):

```cpp
TextureImportApplyResult applyTextureImportSettings(GameObject* sceneRoot,
                                                    const std::filesystem::path& asset,
                                                    const TextureImportSettings& settings,
                                                    const std::function<void(GameObject&)>& rebuild)
{
    TextureImportApplyResult r;
    if (!saveTextureImportSettings(asset, settings, &r.error))
        return r;                                    // nada reconstruido si no se pudo escribir
    r.ok = true;
    if (!sceneRoot || !rebuild) return r;

    sceneRoot->traverse([&](GameObject* go)
    {
        auto coincide = [&](const std::string& field)
        {
            return !field.empty() && samePath(field, asset);
        };
        if (!go->hasMesh() || !tocaAlgunMaterial(go, coincide)) return;
        rebuild(*go);
        ++r.refreshed;
    });
    return r;
}
```

Modal (`ContentBrowserPanel.h`: `std::filesystem::path m_importTarget; TextureImportSettings m_importEdit; std::string m_importError; bool m_openImportPopup = false;`):
- Menú contextual (dentro del `BeginPopupContextItem`, tras "Rename"): visible solo si `selCount <= 1 && classifyAsset(path.extension().string(), isDir) == AssetKind::Image`:
  ```cpp
  if (selCount <= 1 && !isDir && classifyAsset(path.extension().string(), false) == AssetKind::Image &&
      ImGui::MenuItem("Import Settings..."))
  {
      m_importTarget    = path;
      m_importEdit      = loadTextureImportSettings(path);
      m_importError.clear();
      m_openImportPopup = true;
  }
  ```
- Modal (mismo patrón que Rename/Delete, `ImGui::OpenPopup("Import Settings")` cuando `m_openImportPopup`): título con el nombre del fichero; un `ImGui::Combo("Color space", ...)` con `"Auto (por slot)\0sRGB\0Linear\0"` mapeado a `ColorSpaceOverride`; `ImGui::Checkbox("Mipmaps", &m_importEdit.mipmaps)`; texto de ayuda gris "Auto: color base sRGB, normal y ORM lineal."; si `m_importError` no vacío, en rojo; botones **Aplicar** y **Cancelar**. Aplicar:
  ```cpp
  const TextureImportApplyResult r = applyTextureImportSettings(
      sceneRoot, m_importTarget, m_importEdit,
      [&ctx](GameObject& go) {
          if (!ctx.renderer) return;
          if (const SkinnedMesh* sm = go.getSkinnedMesh(); sm && go.skinnedRenderIndex >= 0)
              ctx.renderer->rebuildSkinnedMesh(go.skinnedRenderIndex, *sm);
          else if (go.staticRenderIndex >= 0)
              ctx.renderer->rebuildStaticMesh(go.staticRenderIndex, *go.getMesh());
      });
  if (r.ok) { ctx.pushLog("Import settings aplicados: " + m_importTarget.filename().string() +
                          " (" + std::to_string(r.refreshed) + " objeto(s) actualizados)");
              ImGui::CloseCurrentPopup(); }
  else      m_importError = r.error;      // el modal NO se cierra
  ```
  (el rebuild es el mismo par que `MaterialTextureCommand::apply`, `Command.cpp:738-741`.)
- El listado de miniaturas no cambia: los ajustes no afectan a `makeThumbnail`.

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; .\build-ninja\engine\tests\dt_content_browser_tests.exe`
Expected: pasa. Sabotaje: hacer que `applyTextureImportSettings` reconstruya aunque `save` falle → `test_apply_reports_write_failure_and_rebuilds_nothing` debe fallar; restaurar.
Luego suite completa (31 + los 2 nuevos = 33/33) y `Sandbox` arrancando.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/Editor/ContentBrowserPanel.h engine/src/Editor/ContentBrowserPanel.cpp engine/tests/content_browser_tests.cpp
git commit -m "feat(editor): modal Import Settings del Content Browser y reconstruccion de los materiales afectados"
```

---

### Task 9: Documentación y cierre del audit

**Files:**
- Modify: `docs/assets-editor-audit.md` (tabla "Estado vigente": fila U8 → **CERRADO (texturas)**, y la comparación con Unity: capacidad 6 → **EXISTE para texturas**; añadir el commit)
- Modify: `README.md` (párrafo del Content Browser: qué son los ajustes de importación, dónde se guardan y qué hacen; una línea sobre el formato del sidecar)
- Modify: `docs/superpowers/specs/2026-09-24-texture-import-settings-design.md` (solo si la implementación se aparta de la spec: dejar constancia)

- [ ] **Step 1: Editar** `docs/assets-editor-audit.md` y `README.md` con `Edit` (nunca reescribir el fichero entero) y comprobar `git diff --stat` (pocas líneas, sin ruido CRLF). En la fila U8 del audit, dejar explícito que modelos y audio siguen abiertos como siguiente spec.
- [ ] **Step 2: Suite completa** desde la raíz: `fallos:` vacío.
- [ ] **Step 3: Commit**

```bash
git add docs/assets-editor-audit.md README.md
git commit -m "docs: cerrar U8 para texturas y documentar los ajustes de importacion"
```

---

## Self-Review

**Spec coverage.**
- §1 Modelo y sidecar → Task 1 (formato, defecto, tolerancia, tope de tamaño, `type` discriminador).
- §2 Consumidores: decodificador central + `resolveSrgb` + cadena de mips → Tasks 2–3; Vulkan (imagen con niveles, vista, sampler `maxLod`, formato, async) → Task 4; D3D12 (`uploadTexture` con niveles, SRV, formato desde el recurso) → Task 5; clave de caché de texturas y `makeSharedMeshKey` → Task 2; cargador asíncrono (`DecodedImage`) → Tasks 3–4.
- §3 UI (menú contextual solo con una imagen, modal, Aplicar/Cancelar, error sin cerrar, rebuild) → Task 8.
- §4 Ciclo de vida (mover, renombrar, borrar, importar, exportar, huérfano, listado sin `.import.json`) → Tasks 6–7. "Duplicar" no aparece: el Content Browser no tiene operación de duplicar (comprobado en el menú contextual: solo Rename/Delete/Create Folder).
- Verificación de la spec: los tests automáticos están en las Tasks 1–3 y 6–8; lo de GPU (mips visibles, colores, refresco, exportado, D3D12MA, syncval con control positivo) no es automatizable y queda **para el usuario** (ver "Entrega").
- Limitación conocida (mips promediados sobre el valor codificado también en sRGB) anotada en `TextureImport.h`.

**Placeholder scan.** Los tests de las Tasks 6 y 8 que el plan describe con comentarios de contrato (`content_browser_tests.cpp`, `asset_import_tests.cpp`) exigen escribir el cuerpo completo con el helper de fixtures de cada fichero; el plan lo dice explícitamente y da los nombres, entradas y `CHECK`s esperados. El resto de pasos lleva el código.

**Type consistency.** `ColorSpaceOverride`/`TextureImportSettings`/`isDefault`/`loadTextureImportSettings`/`saveTextureImportSettings` (Task 1) se usan con esos nombres en 2–8. `TextureMip`, `mipLevelCount`, `buildMipChain`, `resolveSrgb`, `textureKeySuffix` (Task 2) idem. `DecodedTexture::colorSpace/mips` (Task 3) → `GpuResources` y `D3D12Renderer` (4, 5). `createMaterialImageFromPixels` sustituye a las dos `FromPixels` y se define y consume en la Task 4. `uploadMaterialTexture(..., TextureKind, ...)` cambia de `bool` a `TextureKind` en la declaración, la definición y todos los llamantes de la Task 5 (comprobado con `git grep`).

**Review Focus.** 1→Task 1, 2→Task 2, 3→Tasks 2 (claves de textura y de malla), 4→Task 6, 5→Task 6, 6→Task 8.

## Entrega (para quien ejecute)

Al terminar la Task 9, pedir al usuario la verificación manual en GUI **en Vulkan y en D3D12** (spec, "Verificación"): tablero con mips visibles a distancia; un normal map marcado `linear` y un albedo marcado `srgb`/`linear` cambian de color como se espera; Aplicar refresca sin reiniciar; mover y renombrar conservan los ajustes; el juego exportado los respeta; cierre limpio en D3D12 (D3D12MA); syncval de Vulkan (`build-ninja/sandbox/vk_layer_settings.txt`) con control positivo, sin avisos de layout ni de nivel. **El editor arranca con el backend del último proyecto** (`project.json`): confirmar cuál es antes de dar por verificado ninguno de los dos, y no dejar `project.json` modificado.
