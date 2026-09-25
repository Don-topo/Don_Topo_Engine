# Ajustes de importación por modelo (FBX) — Plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Un `.fbx` lleva cinco ajustes de fichero (escala, normales, tangentes, voltear UVs, importar animaciones) en su sidecar `<fbx>.import.json`, que el `ModelLoader` aplica solo, editables desde el Content Browser; Aplicar recarga en vivo todos los objetos que usan ese modelo.

**Architecture:** `Core/ImportSettings` añade `ModelImportSettings` (`type: "model"`, mismo lector tolerante). `ModelLoader::load/loadSkinned/loadAnimationClips` leen el sidecar ellos mismos, así que los cuatro callers y el juego exportado lo heredan sin cambios. La reconstrucción de fuentes de animación de un skinned, hoy inline en `Scene::fromJson`, se extrae a `applyAnimationSourceConfig` para que carga y reimport compartan una sola copia. `reimportModelUsers` (fichero nuevo) hace remove + carga + `setMesh` + alta en GPU por objeto siguiendo la receta de `MeshComponentCommand`; el modal "Import Settings" gana el tipo Modelo; el exportador copia el sidecar del FBX y el de cada fuente de animación.

**Tech Stack:** C++20, Assimp (`aiProcess_GlobalScale`, `aiProcess_RemoveComponent`), nlohmann_json, ImGui. Tests planos `main()` + `CHECK`, `dt_add_test`.

**Spec:** `docs/superpowers/specs/2026-09-25-model-import-settings-design.md`

## Global Constraints

- **Sin dependencias de terceros nuevas.**
- **Sin sidecar, el resultado es idéntico al de hoy**: flags de Assimp exactamente `Triangulate | FlipUVs | GenNormals | CalcTangentSpace`, sin escala. Los tests existentes no pueden cambiar de resultado.
- **Sidecar** `{ "version": 1, "type": "model", "scale", "normals", "calcTangents", "flipUVs", "importAnimations" }`; **guardar el defecto BORRA el fichero**; escritura por temporal + `rename`.
- **Lectura tolerante, nunca lanza**: ausente (sin aviso), JSON roto, > 64 KiB, `version` ≠ 1, `type` ≠ `model`, campo de tipo equivocado → su defecto + aviso; un campo malo deja los demás leídos. `scale`: NaN o ≤ 0 → 1; > 1000 → 1000; < 0.001 → 0.001 (con aviso). `normals` desconocido → `file`.
- **`normals`**: `file` = `GenNormals` (genera solo si faltan); `smooth` y `flat` **regeneran siempre** (`RemoveComponent` con `aiComponent_NORMALS` + `GenSmoothNormals` / `GenNormals`).
- **Cada FBX usa SU sidecar**, incluidas las fuentes de animación externas (`loadAnimationClips` aplica la escala del fichero de animación).
- **`importAnimations = false`**: la fuente builtin se registra igual, con 0 clips.
- **Aviso del loader por `stderr`** con prefijo `[ModelImport]` (`printf` no llega al Log Console, mismo criterio que `[AudioImport]`).
- **CRLF**: el repo va en CRLF. Tras cada `Edit`, comprobar `git diff --stat`. No usar `sed -i` ni Get-Content/Set-Content en PowerShell.
- **Los tests se ejecutan desde la raíz del repo** (`.\build-ninja\engine\tests\dt_xxx.exe`): 5 de ellos fallan si no (`assets/` no resuelve).
- **Build**: `.\build.bat > $env:TEMP\b.log 2>&1` desde PowerShell; leer solo los errores. Si el enlazado de `Sandbox.exe` da `LNK1201`, hay un `Sandbox.exe` vivo: `Get-Process Sandbox -ErrorAction SilentlyContinue | Stop-Process -Force` y repetir. Ninja regenera solo al cambiar un `CMakeLists.txt`.
- **Suite completa**: ~3-5 min (`dt_asset_loader_tests` solo ~2m30, no es un cuelgue). Hoy `total 34`; tras la Task 2, `total 35`. Correrla con el script de `[Diagnostics.Process]::Start` (abajo), en segundo plano.

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

1. **Sidecar hostil o corrupto junto a un FBX** (`scale` 0, negativo, `1e30`, texto, JSON roto): el modelo carga igual con el defecto o el valor acotado, sin lanzar y sin colapsar la malla. → Task 1 (lector) y Task 2 (loader).
2. **Reimport de un FBX que ya no carga** (borrado o corrupto entre medias): los objetos se quedan **intactos** con su malla anterior y hay un aviso; nunca queda un objeto sin malla. → Task 4.
3. **Reimport de un personaje con clips renombrados, fuente externa y Animator**: sobreviven los nombres, la fuente externa y el estado del Animator resuelve su clip. → Task 4.
4. **Sin sidecar, ni un vértice cambia** respecto a hoy (UV volteadas, normales generadas, tangentes calculadas, escala 1). → Task 2.
5. **`normals = smooth/flat` con un FBX que YA trae normales** regenera de verdad (no deja las del fichero). → Task 2.
6. **Sidecar de un FBX y de sus fuentes de animación externas viaja al export**, y un FBX sin sidecar no añade nada. → Task 6.

---

## File Structure

| Fichero | Responsabilidad |
|---|---|
| `engine/include/DonTopo/Core/ImportSettings.h`, `engine/src/Core/ImportSettings.cpp` (modif.) | `NormalsMode`, `ModelImportSettings`, `clampModelScale`, `load/saveModelImportSettings` |
| `engine/src/Renderer/ModelLoader.cpp` (modif.) | Flags de Assimp desde los ajustes; `importAnimations`; escala de `loadAnimationClips` |
| `engine/include/DonTopo/Renderer/SkinnedMeshAnimations.h`, `engine/src/Renderer/SkinnedMeshAnimations.cpp` (modif.) | `AnimationSourceConfig`, `animationSourceConfigOf`, `applyAnimationSourceConfig` |
| `engine/src/Core/Scene.cpp` (modif.) | `fromJson` usa `applyAnimationSourceConfig` en vez del bucle inline |
| `engine/include/DonTopo/Editor/ModelReimport.h`, `engine/src/Editor/ModelReimport.cpp` (**nuevos**) | `reimportModelUsers`: recarga en vivo los objetos de un FBX |
| `engine/include/DonTopo/Editor/ContentBrowserPanel.h`, `engine/src/Editor/ContentBrowserPanel.cpp` (modif.) | `ImportSettingsKind::Model`, `applyModelImportSettings`, modal |
| `engine/src/Editor/GameExporter.cpp` (modif.) | El FBX y sus fuentes de animación viajan con su sidecar |
| `engine/CMakeLists.txt`, `engine/tests/CMakeLists.txt` (modif.) | Fuente nueva del editor; test nuevo `dt_model_import_tests` |
| `engine/tests/import_settings_tests.cpp`, `animator_tests.cpp`, `content_browser_tests.cpp`, `exporter_tests.cpp` (modif.); `engine/tests/model_import_tests.cpp` (**nuevo**) | Tests |
| `docs/assets-editor-audit.md`, `README.md` (modif.) | Cierre de U8 y documentación |

---

### Task 1: Formato del sidecar de modelo (Core)

**Files:**
- Modify: `engine/include/DonTopo/Core/ImportSettings.h` (tras el bloque de audio, antes de `sameAssetPath`)
- Modify: `engine/src/Core/ImportSettings.cpp` (tras `saveAudioImportSettings`)
- Test: `engine/tests/import_settings_tests.cpp`

**Interfaces:**
- Produces: `enum class NormalsMode : uint8_t { File, Smooth, Flat }`; `struct ModelImportSettings { float scale = 1.0f; NormalsMode normals = NormalsMode::File; bool calcTangents = true, flipUVs = true, importAnimations = true; }`; `kModelScaleMin = 0.001f`, `kModelScaleMax = 1000.0f`; `operator==`, `isDefault`; `float clampModelScale(float)`; `ModelImportSettings loadModelImportSettings(const std::filesystem::path& asset, std::string* warning = nullptr)`; `bool saveModelImportSettings(const std::filesystem::path& asset, const ModelImportSettings&, std::string* error = nullptr)`.

- [ ] **Step 1: Write the failing tests** — en `import_settings_tests.cpp`, justo antes de `int main()`:

```cpp
// ── Modelos ──────────────────────────────────────────────────────────────────

static void test_model_missing_is_default_without_warning()
{
    const fs::path d = makeDir();
    std::string warning = "x";
    CHECK(isDefault(loadModelImportSettings(d / "nave.fbx", &warning)));
    CHECK(warning.empty());
}

static void test_model_roundtrip_and_default_removes_sidecar()
{
    const fs::path d = makeDir();
    const fs::path asset = d / "nave.fbx";
    ModelImportSettings in;
    in.scale            = 0.01f;
    in.normals          = NormalsMode::Smooth;
    in.calcTangents     = false;
    in.flipUVs          = false;
    in.importAnimations = false;
    std::string err;
    CHECK(saveModelImportSettings(asset, in, &err));
    CHECK(fs::exists(importSidecarPath(asset)));
    CHECK(loadModelImportSettings(asset) == in);

    // Guardar el defecto borra el sidecar; con el fichero ya ausente tampoco es error.
    CHECK(saveModelImportSettings(asset, ModelImportSettings{}, &err));
    CHECK(!fs::exists(importSidecarPath(asset)));
    CHECK(saveModelImportSettings(asset, ModelImportSettings{}, &err));
}

// Review Focus 1.
static void test_model_scale_hostile_values()
{
    const fs::path d = makeDir();
    struct Case { const char* name; const char* json; float want; bool warns; };
    const Case cases[] = {
        { "cero.fbx",   R"({"version":1,"type":"model","scale":0})",       1.0f,    true  },
        { "neg.fbx",    R"({"version":1,"type":"model","scale":-3})",      1.0f,    true  },
        { "enorme.fbx", R"({"version":1,"type":"model","scale":1e30})",    1000.0f, true  },
        { "minusc.fbx", R"({"version":1,"type":"model","scale":0.00001})", 0.001f,  true  },
        { "texto.fbx",  R"({"version":1,"type":"model","scale":"2"})",     1.0f,    true  },
        { "borde.fbx",  R"({"version":1,"type":"model","scale":0.001})",   0.001f,  false },
        { "bien.fbx",   R"({"version":1,"type":"model","scale":2.5})",     2.5f,    false },
    };
    for (const Case& c : cases)
    {
        writeText(importSidecarPath(d / c.name), c.json);
        std::string warning;
        const ModelImportSettings s = loadModelImportSettings(d / c.name, &warning);
        CHECK(s.scale == c.want);
        CHECK(warning.empty() == !c.warns);
    }

    // Guardar un NaN: se acota a 1 y, al ser el defecto, no deja sidecar.
    ModelImportSettings nan;
    nan.scale = std::numeric_limits<float>::quiet_NaN();
    std::string err;
    CHECK(saveModelImportSettings(d / "nan.fbx", nan, &err));
    CHECK(!fs::exists(importSidecarPath(d / "nan.fbx")));

    // Y una escala fuera de rango se guarda ya acotada.
    ModelImportSettings big;
    big.scale = 1e9f;
    CHECK(saveModelImportSettings(d / "big.fbx", big, &err));
    CHECK(loadModelImportSettings(d / "big.fbx").scale == kModelScaleMax);
}

static void test_model_unknown_normals_keeps_the_other_fields()
{
    const fs::path d = makeDir();
    writeText(importSidecarPath(d / "a.fbx"),
              R"({"version":1,"type":"model","normals":"raro","scale":2,"flipUVs":false})");
    std::string w;
    const ModelImportSettings s = loadModelImportSettings(d / "a.fbx", &w);
    CHECK(s.normals == NormalsMode::File);       // desconocido -> file
    CHECK(s.scale == 2.0f);                       // los demas campos sobreviven
    CHECK(!s.flipUVs);
    CHECK(!w.empty());

    writeText(importSidecarPath(d / "b.fbx"),
              R"({"version":1,"type":"model","calcTangents":"si","importAnimations":false})");
    w.clear();
    const ModelImportSettings t = loadModelImportSettings(d / "b.fbx", &w);
    CHECK(t.calcTangents);                        // tipo equivocado -> su defecto (true)
    CHECK(!t.importAnimations);
    CHECK(!w.empty());
}

static void test_model_broken_and_huge_are_default()
{
    const fs::path d = makeDir();
    writeText(importSidecarPath(d / "roto.fbx"), "{ esto no es json");
    std::string w;
    CHECK(isDefault(loadModelImportSettings(d / "roto.fbx", &w)));
    CHECK(!w.empty());

    writeText(importSidecarPath(d / "grande.fbx"), std::string(5 * 1024 * 1024, 'x'));
    w.clear();
    CHECK(isDefault(loadModelImportSettings(d / "grande.fbx", &w)));
    CHECK(!w.empty());
}

static void test_model_type_crossing()
{
    const fs::path d = makeDir();
    const fs::path asset = d / "x.fbx";
    TextureImportSettings tex;
    tex.mipmaps = true;
    std::string err;
    CHECK(saveTextureImportSettings(asset, tex, &err));
    std::string w;
    CHECK(isDefault(loadModelImportSettings(asset, &w)));     // texture leido como model
    CHECK(!w.empty());

    ModelImportSettings m;
    m.scale = 2.0f;
    CHECK(saveModelImportSettings(asset, m, &err));
    w.clear();
    CHECK(isDefault(loadTextureImportSettings(asset, &w)));   // model leido como texture
    CHECK(!w.empty());
    w.clear();
    CHECK(isDefault(loadAudioImportSettings(asset, &w)));     // ... y como audio
    CHECK(!w.empty());
}
```

Y en `main()`, antes de `test_audio_gain_math();`:

```cpp
    test_model_missing_is_default_without_warning();
    test_model_roundtrip_and_default_removes_sidecar();
    test_model_scale_hostile_values();
    test_model_unknown_normals_keeps_the_other_fields();
    test_model_broken_and_huge_are_default();
    test_model_type_crossing();
```

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 5`
Expected: errores de compilación (`ModelImportSettings`, `NormalsMode`, `loadModelImportSettings` sin declarar).

- [ ] **Step 3: Implement**

`ImportSettings.h`, justo antes de `bool sameAssetPath(...)` (la línea `// Dos rutas que nombran el mismo fichero`):

```cpp
// ── Modelos ──────────────────────────────────────────────────────────────────

// De donde salen las normales de la malla. File = las del fichero (y planas si
// faltan), que es lo que pasaba antes de que existiera el ajuste; Smooth y Flat
// las REGENERAN siempre, aunque el fichero las traiga.
enum class NormalsMode : uint8_t { File, Smooth, Flat };

// Propiedades del FICHERO del modelo, no del objeto: el Transform.scale del
// GameObject va aparte y se multiplica encima. La escala se hornea en la
// geometria al importar (FBX en cm frente a m).
struct ModelImportSettings
{
    float       scale            = 1.0f;               // [kModelScaleMin, kModelScaleMax]
    NormalsMode normals          = NormalsMode::File;
    bool        calcTangents     = true;
    bool        flipUVs          = true;
    bool        importAnimations = true;
};
inline constexpr float kModelScaleMin = 0.001f;
inline constexpr float kModelScaleMax = 1000.0f;

inline bool operator==(const ModelImportSettings& a, const ModelImportSettings& b)
{
    return a.scale == b.scale && a.normals == b.normals && a.calcTangents == b.calcTangents &&
           a.flipUVs == b.flipUVs && a.importAnimations == b.importAnimations;
}
inline bool isDefault(const ModelImportSettings& s) { return s == ModelImportSettings{}; }

// NaN o <= 0 -> 1 (una escala 0 colapsaria la malla); el resto se acota a
// [0.001, 1000] (+inf da 1000).
float clampModelScale(float scale);

// Misma tolerancia que texturas y audio: nunca lanza.
ModelImportSettings loadModelImportSettings(const std::filesystem::path& asset,
                                            std::string* warning = nullptr);
// Guardar el defecto BORRA el sidecar. La escala se escribe ya acotada.
bool saveModelImportSettings(const std::filesystem::path& asset,
                             const ModelImportSettings& settings,
                             std::string* error = nullptr);

```

`ImportSettings.cpp`. En el namespace anónimo, junto a `colorSpaceName`, añadir:

```cpp
const char* normalsModeName(NormalsMode m)
{
    switch (m)
    {
        case NormalsMode::Smooth: return "smooth";
        case NormalsMode::Flat:   return "flat";
        case NormalsMode::File:   break;
    }
    return "file";
}
```

Y tras `saveAudioImportSettings` (antes de `sameAssetPath`):

```cpp
float clampModelScale(float scale)
{
    if (std::isnan(scale) || scale <= 0.0f) return 1.0f;
    return std::clamp(scale, kModelScaleMin, kModelScaleMax);
}

ModelImportSettings loadModelImportSettings(const std::filesystem::path& asset, std::string* warning)
{
    ModelImportSettings out;
    const std::optional<nlohmann::json> j = readSidecar(asset, "model", warning);
    if (!j) return out;

    std::string problems;
    if (const auto it = j->find("scale"); it != j->end())
    {
        if (it->is_number())
        {
            // En double y con literales: kModelScaleMin (float) como double no es
            // 0.001 exacto, y un 0.001 escrito a mano saldria "fuera de rango".
            const double v = it->get<double>();
            if (std::isnan(v) || v <= 0.0)
                problems += "scale no es positivo (se usa 1). ";
            else if (v > 1000.0)
            {
                out.scale = kModelScaleMax;
                problems += "scale fuera de rango (acotado). ";
            }
            else if (v < 0.001)
            {
                out.scale = kModelScaleMin;
                problems += "scale fuera de rango (acotado). ";
            }
            else out.scale = static_cast<float>(v);
        }
        else problems += "scale no es numerico (se usa 1). ";
    }
    if (const auto it = j->find("normals"); it != j->end())
    {
        const std::string v = it->is_string() ? it->get<std::string>() : std::string();
        if      (v == "file")   out.normals = NormalsMode::File;
        else if (v == "smooth") out.normals = NormalsMode::Smooth;
        else if (v == "flat")   out.normals = NormalsMode::Flat;
        else problems += "normals desconocido (se usa file). ";
    }
    auto readBool = [&](const char* key, bool& dst)
    {
        const auto it = j->find(key);
        if (it == j->end()) return;
        if (it->is_boolean()) dst = it->get<bool>();
        else problems += std::string(key) + " no es booleano (se usa el defecto). ";
    };
    readBool("calcTangents",     out.calcTangents);
    readBool("flipUVs",          out.flipUVs);
    readBool("importAnimations", out.importAnimations);

    if (!problems.empty() && warning) *warning = problems;
    return out;
}

bool saveModelImportSettings(const std::filesystem::path& asset, const ModelImportSettings& settings,
                             std::string* error)
{
    ModelImportSettings s = settings;
    s.scale = clampModelScale(s.scale);
    if (isDefault(s))
        return removeSidecar(asset, error);

    nlohmann::json j;
    j["version"]          = 1;
    j["type"]             = "model";
    j["scale"]            = s.scale;
    j["normals"]          = normalsModeName(s.normals);
    j["calcTangents"]     = s.calcTangents;
    j["flipUVs"]          = s.flipUVs;
    j["importAnimations"] = s.importAnimations;
    return writeSidecar(asset, j, error);
}
```

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 5; .\build-ninja\engine\tests\dt_import_settings_tests.exe; "exit $LASTEXITCODE"`
Expected: `ALL IMPORT SETTINGS TESTS PASSED`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/Core/ImportSettings.h engine/src/Core/ImportSettings.cpp engine/tests/import_settings_tests.cpp
git commit -m "feat(core): sidecar de importacion de modelos (escala, normales, tangentes, UVs, animaciones)"
```

---

### Task 2: El `ModelLoader` lee los ajustes

**Files:**
- Modify: `engine/src/Renderer/ModelLoader.cpp` (`load` `:82-85`, `loadSkinned` `:172-178`, bucle de animaciones `:342-343`, `loadAnimationClips` `:435-443`; includes y helpers arriba)
- Create: `engine/tests/model_import_tests.cpp`
- Modify: `engine/tests/CMakeLists.txt`

**Interfaces:**
- Consumes (Task 1): `ModelImportSettings`, `NormalsMode`, `loadModelImportSettings`, `saveModelImportSettings`.
- Produces: `ModelLoader::load / loadSkinned / loadAnimationClips / loadAuto` aplican el sidecar de SU `path`, sin cambio de firma.

- [ ] **Step 1: Write the failing tests** — `engine/tests/model_import_tests.cpp` (nuevo):

```cpp
// Test headless de como ModelLoader lee los ajustes de importacion de un modelo
// (escala, normales, tangentes, UVs, animaciones). Sin GPU. Desde la raiz del repo:
// usa assets/modelAnimation.fbx, que el test copia a una carpeta temporal para
// poner el sidecar sin ensuciar assets/.
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Core/ImportSettings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

using namespace DonTopo;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static fs::path makeDir(const char* name)
{
    std::error_code ec;
    fs::path d = fs::temp_directory_path(ec) / name;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

static void writeText(const fs::path& p, const std::string& s)
{
    std::ofstream(p, std::ios::binary) << s;
}

static void writeSettings(const fs::path& asset, const ModelImportSettings& s)
{
    std::string err;
    CHECK(saveModelImportSettings(asset, s, &err));
}

// Dos triangulos plegados que comparten la arista p1-p3. T1 esta en el plano XY
// (normal +Z); T2 va inclinado (normal (1,-1,1)/sqrt3). En T1 la u crece a lo largo
// de +Y, asi que su tangente correcta es (0,1,0) y NO el fallback (1,0,0). El
// importador de OBJ desenrolla los vertices por cara: los indices 0-2 son T1
// (p1,p2,p3) y los 3-5 son T2 (p1,p3,p4).
static const char* kFoldedObj =
    "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 1\n"
    "vt 0 0\nvt 0 1\nvt 1 1\nvt 1 0\n"
    "f 1/1 2/2 3/3\n"
    "f 1/1 3/3 4/4\n";

// Lo mismo, pero el fichero TRAE normales (todas +Z, tambien las de T2, que son
// "mentira"): sirve para probar que smooth/flat las regeneran.
static const char* kFoldedObjWithNormals =
    "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 1\n"
    "vt 0 0\nvt 0 1\nvt 1 1\nvt 1 0\n"
    "vn 0 0 1\n"
    "f 1/1/1 2/2/1 3/3/1\n"
    "f 1/1/1 3/3/1 4/4/1\n";

static fs::path writeObj(const char* dirName, const char* text)
{
    const fs::path obj = makeDir(dirName) / "plegado.obj";
    writeText(obj, text);
    return obj;
}

static const Vertex* vertexAt(const Mesh& m, size_t tri, const glm::vec3& pos)
{
    for (size_t k = 0; k < 3; ++k)
    {
        const Vertex& v = m.vertices[m.indices[tri * 3 + k]];
        if (glm::length(v.pos - pos) < 1e-4f) return &v;
    }
    return nullptr;
}

static bool foldedShape(const Mesh& m)
{
    return m.vertices.size() == 6 && m.indices.size() == 6;
}

static glm::vec3 kFaceNormal2 = glm::normalize(glm::vec3(1.0f, -1.0f, 1.0f));

// Review Focus 4: sin sidecar, exactamente lo de antes.
static void test_defaults_match_the_old_flags()
{
    const fs::path obj = writeObj("dt_model_import_defaults", kFoldedObj);
    const Mesh m = ModelLoader::load(obj.string());
    CHECK(foldedShape(m));
    if (!foldedShape(m)) return;

    const Vertex* p2 = vertexAt(m, 0, { 1, 0, 0 });
    CHECK(p2 != nullptr);
    if (!p2) return;
    CHECK(p2->uv.y == 0.0f);                                     // FlipUVs: v = 1 - 1
    CHECK(glm::length(p2->normal - glm::vec3(0, 0, 1)) < 1e-3f); // GenNormals: plana de T1
    CHECK(std::abs(p2->tangent.y) > 0.9f);                       // CalcTangentSpace corrio
    CHECK(vertexAt(m, 0, { 2, 0, 0 }) == nullptr);               // escala 1
}

static void test_scale_multiplies_positions()
{
    const fs::path obj = writeObj("dt_model_import_scale", kFoldedObj);
    ModelImportSettings s;
    s.scale = 2.0f;
    writeSettings(obj, s);
    const Mesh m = ModelLoader::load(obj.string());
    CHECK(foldedShape(m));
    if (!foldedShape(m)) return;
    CHECK(vertexAt(m, 0, { 2, 0, 0 }) != nullptr);
    CHECK(vertexAt(m, 0, { 2, 2, 0 }) != nullptr);
    CHECK(vertexAt(m, 0, { 1, 0, 0 }) == nullptr);
}

static void test_flip_uvs_off_keeps_v()
{
    const fs::path obj = writeObj("dt_model_import_flip", kFoldedObj);
    ModelImportSettings s;
    s.flipUVs = false;
    writeSettings(obj, s);
    const Mesh m = ModelLoader::load(obj.string());
    CHECK(foldedShape(m));
    if (!foldedShape(m)) return;
    const Vertex* p2 = vertexAt(m, 0, { 1, 0, 0 });
    CHECK(p2 != nullptr);
    if (p2) CHECK(p2->uv.y == 1.0f);
}

static void test_tangents_off_uses_the_fallback()
{
    const fs::path obj = writeObj("dt_model_import_tangents", kFoldedObj);
    ModelImportSettings s;
    s.calcTangents = false;
    writeSettings(obj, s);
    const Mesh m = ModelLoader::load(obj.string());
    CHECK(foldedShape(m));
    for (const Vertex& v : m.vertices)
        CHECK(glm::length(v.tangent - glm::vec3(1, 0, 0)) < 1e-6f);
}

// Review Focus 5.
static void test_flat_normals_regenerate_even_when_the_file_has_them()
{
    const fs::path obj = writeObj("dt_model_import_flat", kFoldedObjWithNormals);

    // Por defecto (file) se respetan las del fichero: T2 sale con +Z.
    {
        const Mesh m = ModelLoader::load(obj.string());
        CHECK(foldedShape(m));
        if (!foldedShape(m)) return;
        const Vertex* v = vertexAt(m, 1, { 0, 1, 1 });
        CHECK(v != nullptr);
        if (v) CHECK(glm::length(v->normal - glm::vec3(0, 0, 1)) < 1e-3f);
    }

    ModelImportSettings s;
    s.normals = NormalsMode::Flat;
    writeSettings(obj, s);
    const Mesh m = ModelLoader::load(obj.string());
    CHECK(foldedShape(m));
    if (!foldedShape(m)) return;
    const Vertex* t1 = vertexAt(m, 0, { 1, 0, 0 });
    const Vertex* t2 = vertexAt(m, 1, { 0, 1, 1 });
    CHECK(t1 != nullptr && t2 != nullptr);
    if (t1) CHECK(glm::length(t1->normal - glm::vec3(0, 0, 1)) < 1e-3f);
    if (t2) CHECK(glm::dot(t2->normal, kFaceNormal2) > 0.99f);   // regenerada, ya no +Z
}

static void test_smooth_normals_average_the_shared_vertices()
{
    const fs::path obj = writeObj("dt_model_import_smooth", kFoldedObj);

    ModelImportSettings s;
    s.normals = NormalsMode::Smooth;
    writeSettings(obj, s);
    const Mesh smooth = ModelLoader::load(obj.string());
    CHECK(foldedShape(smooth));
    if (!foldedShape(smooth)) return;

    // p1 esta en las dos caras: suave = misma normal en las dos copias, y ni
    // la de T1 (+Z) ni la de T2.
    const Vertex* a = vertexAt(smooth, 0, { 0, 0, 0 });
    const Vertex* b = vertexAt(smooth, 1, { 0, 0, 0 });
    CHECK(a != nullptr && b != nullptr);
    if (!a || !b) return;
    CHECK(glm::length(a->normal - b->normal) < 1e-3f);
    CHECK(glm::dot(a->normal, glm::vec3(0, 0, 1)) < 0.999f);
    CHECK(glm::dot(a->normal, kFaceNormal2) < 0.999f);

    // Planas: cada copia conserva la de SU cara.
    s.normals = NormalsMode::Flat;
    writeSettings(obj, s);
    const Mesh flat = ModelLoader::load(obj.string());
    CHECK(foldedShape(flat));
    if (!foldedShape(flat)) return;
    const Vertex* fa = vertexAt(flat, 0, { 0, 0, 0 });
    const Vertex* fb = vertexAt(flat, 1, { 0, 0, 0 });
    CHECK(fa != nullptr && fb != nullptr);
    if (fa && fb) CHECK(glm::length(fa->normal - fb->normal) > 0.1f);
}

// Review Focus 1: un sidecar hostil nunca tumba la carga.
static void test_hostile_sidecar_never_breaks_the_load()
{
    const fs::path obj = writeObj("dt_model_import_hostile", kFoldedObj);

    writeText(importSidecarPath(obj), "{ esto no es json");
    Mesh m = ModelLoader::load(obj.string());
    CHECK(foldedShape(m) && vertexAt(m, 0, { 1, 0, 0 }) != nullptr);

    writeText(importSidecarPath(obj), R"({"version":1,"type":"model","scale":0})");
    m = ModelLoader::load(obj.string());
    CHECK(foldedShape(m) && vertexAt(m, 0, { 1, 0, 0 }) != nullptr);   // 0 -> 1, no colapsa

    writeText(importSidecarPath(obj), R"({"version":1,"type":"model","scale":1e30})");
    m = ModelLoader::load(obj.string());
    CHECK(foldedShape(m) && vertexAt(m, 0, { 1000, 0, 0 }) != nullptr);   // acotado a 1000
}

static void test_missing_model_still_throws()
{
    bool threw = false;
    try { (void)ModelLoader::load("no_existe_dt_model.obj"); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

// ── Skinned: el personaje de prueba del repo, copiado a una carpeta temporal ──

static fs::path copyCharacter(const char* dirName)
{
    const fs::path dst = makeDir(dirName) / "char.fbx";
    std::error_code ec;
    fs::copy_file("assets/modelAnimation.fbx", dst, fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    return dst;
}

static float maxAbsCoord(const SkinnedMesh& m)
{
    float r = 0.0f;
    for (const SkinnedVertex& v : m.skinnedVertices)
        r = std::max({ r, std::abs(v.position.x), std::abs(v.position.y), std::abs(v.position.z) });
    return r;
}

// |b - 2a| dentro de una tolerancia relativa: la razon, no un valor absoluto.
static bool doubled(float a, float b)
{
    return std::abs(b - 2.0f * a) <= 1e-3f * std::max(1.0f, std::abs(a));
}

static const BoneKeyframe* firstPosKey(const std::vector<AnimationClip>& clips)
{
    for (const AnimationClip& c : clips)
        for (const BoneChannel& ch : c.channels)
            if (!ch.posKeys.empty()) return &ch.posKeys[0];
    return nullptr;
}

// Riesgo del spec: GlobalScale frente a las unidades propias del importador FBX.
// Se comprueba la RAZON con y sin ajuste sobre el FBX real. Si esto falla no se
// debilita el test: es el hallazgo que el spec dejo anotado.
static void test_skinned_scale_scales_geometry_bones_and_clips()
{
    const fs::path fbx = copyCharacter("dt_model_import_skinned_scale");
    const SkinnedMesh base = ModelLoader::loadSkinned(fbx.string());
    CHECK(!base.skinnedVertices.empty());
    if (base.skinnedVertices.empty()) return;

    ModelImportSettings s;
    s.scale = 2.0f;
    writeSettings(fbx, s);
    const SkinnedMesh scaled = ModelLoader::loadSkinned(fbx.string());
    CHECK(scaled.skinnedVertices.size() == base.skinnedVertices.size());

    CHECK(doubled(maxAbsCoord(base), maxAbsCoord(scaled)));

    // Un hueso cuyo offset tenga traslacion apreciable: se duplica.
    bool checkedBone = false;
    for (size_t i = 0; i < base.skeleton.inverseBindPose.size() &&
                       i < scaled.skeleton.inverseBindPose.size(); ++i)
    {
        const glm::vec3 t0(base.skeleton.inverseBindPose[i][3]);
        if (glm::length(t0) < 1e-3f) continue;
        const glm::vec3 t1(scaled.skeleton.inverseBindPose[i][3]);
        CHECK(doubled(t0.x, t1.x) && doubled(t0.y, t1.y) && doubled(t0.z, t1.z));
        checkedBone = true;
        break;
    }
    CHECK(checkedBone);

    // La primera clave de traslacion de un clip: se duplica.
    const BoneKeyframe* k0 = firstPosKey(base.animationClips);
    const BoneKeyframe* k1 = firstPosKey(scaled.animationClips);
    CHECK(k0 != nullptr && k1 != nullptr);
    if (k0 && k1) CHECK(doubled(k0->value.x, k1->value.x) && doubled(k0->value.y, k1->value.y) &&
                        doubled(k0->value.z, k1->value.z));
}

static void test_import_animations_off_leaves_the_builtin_source_empty()
{
    const fs::path fbx = copyCharacter("dt_model_import_anim_off");
    const SkinnedMesh base = ModelLoader::loadSkinned(fbx.string());
    CHECK(!base.animationClips.empty());          // precondicion: el fixture trae clips

    ModelImportSettings s;
    s.importAnimations = false;
    writeSettings(fbx, s);
    const SkinnedMesh off = ModelLoader::loadSkinned(fbx.string());
    CHECK(off.animationClips.empty());
    CHECK(off.animationSources.size() == 1);
    if (off.animationSources.size() == 1)
    {
        CHECK(off.animationSources[0].builtin);
        CHECK(off.animationSources[0].clipNames.empty());
    }
    CHECK(off.skinnedVertices.size() == base.skinnedVertices.size());   // la malla sigue
}

// Cada FBX usa SU sidecar: los clips de una fuente externa se escalan con el del
// propio fichero de animacion.
static void test_animation_source_uses_its_own_scale()
{
    const fs::path fbx = copyCharacter("dt_model_import_clip_scale");
    const SkinnedMesh base = ModelLoader::loadSkinned(fbx.string());
    const LoadedClips c0 = ModelLoader::loadAnimationClips(fbx.string(), base.skeleton);
    const BoneKeyframe* k0 = firstPosKey(c0.clips);
    CHECK(k0 != nullptr);
    if (!k0) return;
    const BoneKeyframe copy0 = *k0;

    ModelImportSettings s;
    s.scale = 2.0f;
    writeSettings(fbx, s);
    const LoadedClips c1 = ModelLoader::loadAnimationClips(fbx.string(), base.skeleton);
    const BoneKeyframe* k1 = firstPosKey(c1.clips);
    CHECK(k1 != nullptr);
    if (k1) CHECK(doubled(copy0.value.x, k1->value.x) && doubled(copy0.value.y, k1->value.y) &&
                  doubled(copy0.value.z, k1->value.z));
}

int main()
{
    test_defaults_match_the_old_flags();
    test_scale_multiplies_positions();
    test_flip_uvs_off_keeps_v();
    test_tangents_off_uses_the_fallback();
    test_flat_normals_regenerate_even_when_the_file_has_them();
    test_smooth_normals_average_the_shared_vertices();
    test_hostile_sidecar_never_breaks_the_load();
    test_missing_model_still_throws();
    test_skinned_scale_scales_geometry_bones_and_clips();
    test_import_animations_off_leaves_the_builtin_source_empty();
    test_animation_source_uses_its_own_scale();

    if (g_failures == 0) std::printf("ALL MODEL IMPORT TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
```

En `engine/tests/CMakeLists.txt`, tras la línea de `dt_texture_import_tests`:

```cmake
dt_add_test(dt_model_import_tests model_import_tests.cpp DonTopoCore)
```

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C|FAILED' | Select-Object -First 5; .\build-ninja\engine\tests\dt_model_import_tests.exe; "exit $LASTEXITCODE"`
Expected: compila; FAIL en `test_scale_multiplies_positions`, `test_flip_uvs_off_keeps_v`, `test_tangents_off_uses_the_fallback`, `test_flat_normals_regenerate_…`, `test_smooth_normals_…`, `test_hostile_sidecar_…` (el `1e30`), `test_skinned_scale_…`, `test_import_animations_off_…` y `test_animation_source_uses_its_own_scale`; **pasan** `test_defaults_match_the_old_flags` y `test_missing_model_still_throws` (el loader ignora hoy el sidecar). Si `test_defaults_match_the_old_flags` falla, el fixture OBJ no produce lo que el comentario dice: corregir el fixture, no el test.

- [ ] **Step 3: Implement** — `ModelLoader.cpp`.

3a. Includes (junto a los de Assimp, y `<cstdio>`):

```cpp
#include "DonTopo/Core/ImportSettings.h"
#include <assimp/config.h>
#include <cstdio>
```

3b. Tras `aiToGlm` (dentro de `namespace DonTopo`), los helpers:

```cpp
    // Ajustes de importacion del fichero (<fbx>.import.json). Nunca lanza: un
    // sidecar roto da el defecto y un aviso por stderr (el Log Console no llega a
    // este nivel; mismo canal que [AudioImport]).
    static ModelImportSettings readModelSettings(const std::string& path)
    {
        std::string warning;
        const ModelImportSettings s = loadModelImportSettings(path, &warning);
        if (!warning.empty())
            std::fprintf(stderr, "[ModelImport] %s: %s\n",
                         std::filesystem::path(path).filename().string().c_str(), warning.c_str());
        return s;
    }

    // Sin sidecar: exactamente Triangulate | FlipUVs | GenNormals | CalcTangentSpace,
    // que era lo fijo antes de que existieran los ajustes.
    static unsigned int assimpFlags(const ModelImportSettings& s)
    {
        unsigned int f = aiProcess_Triangulate;
        if (s.flipUVs)      f |= aiProcess_FlipUVs;
        if (s.calcTangents) f |= aiProcess_CalcTangentSpace;
        switch (s.normals)
        {
            case NormalsMode::File:   f |= aiProcess_GenNormals; break;          // solo si faltan
            case NormalsMode::Smooth: f |= aiProcess_RemoveComponent | aiProcess_GenSmoothNormals; break;
            case NormalsMode::Flat:   f |= aiProcess_RemoveComponent | aiProcess_GenNormals; break;
        }
        if (s.scale != 1.0f) f |= aiProcess_GlobalScale;
        return f;
    }

    static void configureImporter(Assimp::Importer& importer, const ModelImportSettings& s)
    {
        // Smooth y Flat descartan las normales del fichero ANTES de generarlas.
        if (s.normals != NormalsMode::File)
            importer.SetPropertyInteger(AI_CONFIG_PP_RVC_FLAGS, aiComponent_NORMALS);
        if (s.scale != 1.0f)
            importer.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, s.scale);
    }
```

3c. `load` — sustituir:

```cpp
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GenNormals | aiProcess_CalcTangentSpace );
```

por:

```cpp
        const ModelImportSettings settings = readModelSettings(path);
        Assimp::Importer importer;
        configureImporter(importer, settings);
        const aiScene* scene = importer.ReadFile(path, assimpFlags(settings));
```

3d. `loadSkinned` — sustituir:

```cpp
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate | aiProcess_FlipUVs |
        aiProcess_GenNormals  | aiProcess_CalcTangentSpace /*|
        aiProcess_LimitBoneWeights*/);
```

por:

```cpp
        const ModelImportSettings settings = readModelSettings(path);
        Assimp::Importer importer;
        configureImporter(importer, settings);
        const aiScene* scene = importer.ReadFile(path, assimpFlags(settings));
```

y el bucle de animaciones de `loadSkinned` (el que sigue a `// --- Animaciones: todas las del fichero ---`):

```cpp
        for (uint32_t a = 0; a < scene->mNumAnimations; a++)
```

pasa a:

```cpp
        // importAnimations = false: sin clips, pero la fuente builtin de abajo se
        // registra igual (la UI necesita una fila que represente al modelo).
        for (uint32_t a = 0; settings.importAnimations && a < scene->mNumAnimations; a++)
```

3e. `loadAnimationClips` — sustituir:

```cpp
        Assimp::Importer importer;
        // Flags mínimos: aquí no se construye geometría, así que triangulate,
        // normales y tangentes serían trabajo tirado. Assimp lee las
        // animaciones igual.
        const aiScene* scene = importer.ReadFile(path, 0);
```

por:

```cpp
        // Cada FBX usa SU sidecar: las claves de traslacion de este fichero tienen
        // que estar en las unidades del esqueleto al que se mapean, asi que la
        // escala se aplica aqui tambien. El resto de ajustes no le afectan.
        const ModelImportSettings settings = readModelSettings(path);
        Assimp::Importer importer;
        if (settings.scale != 1.0f)
            importer.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, settings.scale);
        // Flags mínimos: aquí no se construye geometría, así que triangulate,
        // normales y tangentes serían trabajo tirado. Assimp lee las
        // animaciones igual.
        const aiScene* scene = importer.ReadFile(
            path, settings.scale != 1.0f ? static_cast<unsigned int>(aiProcess_GlobalScale) : 0u);
```

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C|FAILED' | Select-Object -First 5; .\build-ninja\engine\tests\dt_model_import_tests.exe; "exit $LASTEXITCODE"`
Expected: `ALL MODEL IMPORT TESTS PASSED`, exit 0.

**Si `test_skinned_scale_…` o `test_animation_source_uses_its_own_scale` fallan por la razón (no por un error del test): PARAR.** Es el riesgo del spec (`GlobalScale` frente a las unidades propias del importador FBX). No relajar la tolerancia ni borrar el test: ledgerar un `Ruling:` y, como fallback, escalar a mano tras la carga en `loadSkinned` (`skinnedVertices[i].position.xyz`, la columna de traslación de cada `skeleton.inverseBindPose[i]`, y los `value` de todos los `posKeys`) y en `loadAnimationClips` (`posKeys`), multiplicando por `settings.scale` y sin activar `GlobalScale` para skinned.

- [ ] **Step 5: Comprobar que nada existente cambió**

Run: `.\build-ninja\engine\tests\dt_animator_tests.exe | Select-Object -Last 2; "exit $LASTEXITCODE"; .\build-ninja\engine\tests\dt_scene_async_tests.exe | Select-Object -Last 2; "exit $LASTEXITCODE"`
Expected: los dos terminan en `PASSED`, exit 0 (el loader sin sidecar es idéntico).

- [ ] **Step 6: Commit**

```bash
git add engine/src/Renderer/ModelLoader.cpp engine/tests/model_import_tests.cpp engine/tests/CMakeLists.txt
git commit -m "feat(renderer): ModelLoader aplica los ajustes de importacion del sidecar del modelo"
```

---

### Task 3: Extraer `applyAnimationSourceConfig` de `Scene::fromJson`

**Files:**
- Modify: `engine/include/DonTopo/Renderer/SkinnedMeshAnimations.h`
- Modify: `engine/src/Renderer/SkinnedMeshAnimations.cpp`
- Modify: `engine/src/Core/Scene.cpp` (bloque `:2184-2250`)
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Produces: `struct AnimationSourceConfig { std::string path; bool builtin = false; std::vector<std::string> clipNames; }`; `std::vector<AnimationSourceConfig> animationSourceConfigOf(const SkinnedMesh&)`; `void applyAnimationSourceConfig(SkinnedMesh&, const std::vector<AnimationSourceConfig>&, std::vector<std::string>& warnings)`. Sin cambio de comportamiento para `Scene::fromJson`.

- [ ] **Step 1: Write the failing tests** — en `animator_tests.cpp`, justo después de `test_missing_animation_source_does_not_break_load` (que termina antes del siguiente comentario), añadir:

```cpp
// Task 3 del plan de import settings de modelos: la configuracion de fuentes de una
// malla (renames de la builtin, fuentes externas con sus nombres) se captura y se
// reaplica sobre una malla recien cargada — lo que hace el reimport de un modelo.
static void test_animation_source_config_roundtrip()
{
    auto viejo = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    std::vector<std::string> w;
    CHECK(addAnimationSource(*viejo, "assets/modelAnimation.fbx", w));
    CHECK(viejo->animationSources.size() == 2u);
    if (viejo->animationSources.size() != 2u) return;
    const std::string builtinName  = viejo->animationSources[0].clipNames[0];
    const std::string importedName = viejo->animationSources[1].clipNames[0];
    CHECK(renameClip(*viejo, builtinName, "CaminarRenombrado"));
    CHECK(renameClip(*viejo, importedName, "SaltoRenombrado"));

    const std::vector<AnimationSourceConfig> cfg = animationSourceConfigOf(*viejo);
    CHECK(cfg.size() == 2u);
    if (cfg.size() != 2u) return;
    CHECK(cfg[0].builtin && cfg[0].clipNames[0] == "CaminarRenombrado");
    CHECK(!cfg[1].builtin && cfg[1].path == "assets/modelAnimation.fbx" &&
          cfg[1].clipNames[0] == "SaltoRenombrado");

    SkinnedMesh nuevo = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    std::vector<std::string> avisos;
    applyAnimationSourceConfig(nuevo, cfg, avisos);
    CHECK(avisos.empty());
    CHECK(nuevo.animationSources.size() == 2u);
    if (nuevo.animationSources.size() == 2u)
    {
        CHECK(nuevo.animationSources[0].builtin);
        CHECK(nuevo.animationSources[0].clipNames[0] == "CaminarRenombrado");
        CHECK(!nuevo.animationSources[1].builtin);
        CHECK(nuevo.animationSources[1].clipNames[0] == "SaltoRenombrado");
    }
    bool encontrado = false;
    for (const auto& c : nuevo.animationClips)
        if (c.name == "SaltoRenombrado") encontrado = true;
    CHECK(encontrado);
}

// Una fuente externa cuyo fichero ya no esta: se avisa y se sigue, sin tocar la malla.
static void test_animation_source_config_missing_source_warns_and_continues()
{
    SkinnedMesh nuevo = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    const size_t clipsAntes = nuevo.animationClips.size();

    std::vector<AnimationSourceConfig> cfg(1);
    cfg[0].path      = "assets/no_existe.fbx";
    cfg[0].builtin   = false;
    cfg[0].clipNames = { "Fantasma" };

    std::vector<std::string> avisos;
    applyAnimationSourceConfig(nuevo, cfg, avisos);
    CHECK(!avisos.empty());
    CHECK(nuevo.animationSources.size() == 1u);
    CHECK(nuevo.animationClips.size() == clipsAntes);
}

// Sin builtin (malla sin fuentes): la entrada builtin se ignora sin lanzar.
static void test_animation_source_config_builtin_on_empty_mesh_is_ignored()
{
    SkinnedMesh vacia;
    std::vector<AnimationSourceConfig> cfg(1);
    cfg[0].builtin   = true;
    cfg[0].clipNames = { "X" };
    std::vector<std::string> avisos;
    applyAnimationSourceConfig(vacia, cfg, avisos);
    CHECK(vacia.animationSources.empty());
}
```

Y en `main()`, justo después de `test_missing_animation_source_does_not_break_load(pm, am);`:

```cpp
    test_animation_source_config_roundtrip();
    test_animation_source_config_missing_source_warns_and_continues();
    test_animation_source_config_builtin_on_empty_mesh_is_ignored();
```

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 5`
Expected: errores de compilación (`AnimationSourceConfig`, `animationSourceConfigOf`, `applyAnimationSourceConfig` sin declarar).

- [ ] **Step 3: Implement**

3a. `SkinnedMeshAnimations.h` — al final del namespace, tras `applyClipNamesPositionally`:

```cpp

    // Configuracion de UNA fuente de animacion de una malla skinned, tal y como la
    // guarda la escena. Es lo que hay que recordar de la malla vieja para
    // reconstruirla sobre una recien cargada (reimport de un modelo) y lo que
    // Scene::fromJson lee de su JSON: una sola forma de aplicarla.
    struct AnimationSourceConfig
    {
        std::string              path;
        bool                     builtin = false;
        std::vector<std::string> clipNames;   // nombres finales, en orden
    };

    // Las fuentes de mesh, en el mismo orden.
    std::vector<AnimationSourceConfig> animationSourceConfigOf(const SkinnedMesh& mesh);

    // Reaplica `sources` sobre una malla recien cargada por loadSkinned: la fuente
    // BUILTIN ya existe y solo recupera los NOMBRES (posicionalmente, de una
    // vez: encadenar renameClip colisiona consigo mismo ante un swap de dos
    // nombres); las externas se reanaden con esos nombres. Una fuente externa que
    // ya no carga (movida, borrada, otro rig) se AVISA y se sigue: perder la
    // escena entera por eso seria mucho peor, y los estados que usaran sus clips
    // los marca bindClips como huerfanos. Nunca lanza.
    void applyAnimationSourceConfig(SkinnedMesh& mesh,
                                    const std::vector<AnimationSourceConfig>& sources,
                                    std::vector<std::string>& warnings);
```

3b. `SkinnedMeshAnimations.cpp` — al final del fichero, dentro del `namespace DonTopo`:

```cpp
    std::vector<AnimationSourceConfig> animationSourceConfigOf(const SkinnedMesh& mesh)
    {
        std::vector<AnimationSourceConfig> out;
        out.reserve(mesh.animationSources.size());
        for (const AnimationSource& s : mesh.animationSources)
            out.push_back({ s.path, s.builtin, s.clipNames });
        return out;
    }

    void applyAnimationSourceConfig(SkinnedMesh& mesh,
                                    const std::vector<AnimationSourceConfig>& sources,
                                    std::vector<std::string>& warnings)
    {
        for (const AnimationSourceConfig& src : sources)
        {
            if (src.builtin)
            {
                // Hasta el menor de los dos tamanos: un FBX reexportado con mas o
                // menos clips no debe romper la carga.
                if (mesh.animationSources.empty()) continue;
                applyClipNamesPositionally(mesh, mesh.animationSources[0], src.clipNames, warnings);
                continue;
            }

            std::vector<std::string> sourceWarnings;
            const std::vector<std::string> names = src.clipNames;
            if (!addAnimationSource(mesh, src.path, sourceWarnings, &names))
                for (const std::string& w : sourceWarnings)
                    warnings.push_back(w);
        }
    }
```

3c. `Scene.cpp` — sustituir el bloque completo que empieza en el comentario `// Fuentes de animación. La builtin ya la creó loadSkinned:` (`:2184`) y termina en el cierre del `if (!compartida && j["mesh"].contains("animationSources")) { ... }` (`:2250`, justo antes de `if (compartida) node->setMesh(compartida);`) por:

```cpp
                    // Fuentes de animación. La builtin ya la creó loadSkinned: de
                    // ella solo se recuperan los NOMBRES (un rename), y se aplican
                    // POSICIONALMENTE de una sola vez hasta el menor de los dos
                    // tamaños (ver applyAnimationSourceConfig). Una malla
                    // compartida ya las trae.
                    if (!compartida && j["mesh"].contains("animationSources"))
                    {
                        std::vector<DonTopo::AnimationSourceConfig> fuentes;
                        for (const auto& sj : j["mesh"]["animationSources"])
                        {
                            DonTopo::AnimationSourceConfig cfg;
                            cfg.path    = sj.value("path", std::string());
                            cfg.builtin = sj.value("builtin", false);

                            // Los nombres se aplican POSICIONALMENTE, así que
                            // una lista a medias no es "casi bien": corre todos
                            // los nombres siguientes un puesto y renombra los
                            // clips equivocados. O entra entera o no entra
                            // ninguna — mismo criterio que jsonToMat4 con la
                            // matriz. Antes, un solo elemento que no fuera
                            // string lanzaba y se perdía la escena entera.
                            if (sj.contains("clips"))
                            {
                                const nlohmann::json& cj = sj["clips"];
                                bool clipsOk = cj.is_array();
                                for (size_t ci = 0; clipsOk && ci < cj.size(); ++ci)
                                    clipsOk = cj[ci].is_string();
                                if (clipsOk)
                                    cfg.clipNames = cj.get<std::vector<std::string>>();
                                else if (warnings)
                                    warnings->push_back("mesh de '" + node->name +
                                                         "'.animationSources.clips: lista corrupta en la "
                                                         "escena, se ignoran los nombres guardados de esa fuente");
                            }
                            fuentes.push_back(std::move(cfg));
                        }

                        // Al warnings del parámetro (Scene::lastWarnings(), lo que
                        // lee el Log Console), no a stdout: en un build sin consola
                        // un printf es invisible. Primero salen los avisos de
                        // parseo y luego los de aplicar; ningún test fija el orden.
                        std::vector<std::string> aplicaAvisos;
                        DonTopo::applyAnimationSourceConfig(*mesh, fuentes, aplicaAvisos);
                        if (warnings)
                            for (const std::string& w : aplicaAvisos)
                                warnings->push_back(w);
                    }
```

Nota: la rama `if (!mesh && !compartida)` de arriba garantiza que `mesh` es no nulo cuando `!compartida`, como antes.

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C|FAILED' | Select-Object -First 5; .\build-ninja\engine\tests\dt_animator_tests.exe | Select-Object -Last 2; "exit $LASTEXITCODE"`
Expected: `ALL ANIMATOR TESTS PASSED` (o el mensaje final de ese binario), exit 0. El test de guardado/carga de fuentes (`test_missing_animation_source_does_not_break_load` y los de renames) sigue verde: es la no-regresión de la extracción.

- [ ] **Step 5: Comprobar los demás caminos que cargan escenas**

Run: `.\build-ninja\engine\tests\dt_scene_async_tests.exe | Select-Object -Last 2; "exit $LASTEXITCODE"; .\build-ninja\engine\tests\dt_material_texture_tests.exe | Select-Object -Last 2; "exit $LASTEXITCODE"`
Expected: `PASSED`, exit 0 en ambos.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Renderer/SkinnedMeshAnimations.h engine/src/Renderer/SkinnedMeshAnimations.cpp engine/src/Core/Scene.cpp engine/tests/animator_tests.cpp
git commit -m "refactor(renderer): extrae applyAnimationSourceConfig de Scene::fromJson para compartirlo con el reimport"
```

---

### Task 4: `reimportModelUsers` — recargar en vivo los objetos de un FBX

**Files:**
- Create: `engine/include/DonTopo/Editor/ModelReimport.h`, `engine/src/Editor/ModelReimport.cpp`
- Modify: `engine/CMakeLists.txt` (fuentes de `DonTopoEditor`)
- Test: `engine/tests/content_browser_tests.cpp`

**Interfaces:**
- Consumes (Task 1-3): `ModelLoader::loadAuto` (lee el sidecar), `animationSourceConfigOf`, `applyAnimationSourceConfig`, `sameAssetPath`, `AnimatorComponent::rebindClips`.
- Produces: `struct ModelReimportResult { int reimported = 0; int skipped = 0; std::vector<std::string> warnings; }`; `ModelReimportResult reimportModelUsers(GameObject* sceneRoot, const std::filesystem::path& fbx, EditorRenderer* renderer)` — `renderer` puede ser `nullptr` (tests headless: solo la parte de CPU).

- [ ] **Step 1: Write the failing tests** — en `content_browser_tests.cpp`.

Includes nuevos junto a los demás (si no están ya: `<algorithm>` y `<cmath>` para `std::max`/`std::abs`):

```cpp
#include <algorithm>
#include <cmath>
#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Editor/ModelReimport.h"
#include "DonTopo/Renderer/SkinnedMeshAnimations.h"
```

Tests, justo antes de `int main()`:

```cpp
// ── reimportModelUsers ───────────────────────────────────────────────────────

static fs::path writeTriangleObj(const char* dirName)
{
    std::error_code ec;
    const fs::path d = fs::temp_directory_path(ec) / dirName;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    const fs::path obj = d / "tri.obj";
    std::ofstream(obj) << "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n";
    return obj;
}

static float maxAbsX(const Mesh& m)
{
    float r = 0.0f;
    for (const Vertex& v : m.vertices) r = std::max(r, std::abs(v.pos.x));
    return r;
}

// Los objetos de ese FBX se recargan con los ajustes nuevos; los demas ni se tocan.
static void test_reimport_replaces_the_meshes_of_that_fbx_only()
{
    const fs::path obj = writeTriangleObj("dt_cb_reimport_static");
    GameObject root("root");
    GameObject* a = root.addChild("A");
    GameObject* b = root.addChild("B");
    GameObject* c = root.addChild("C");   // procedural: sin sourcePath
    a->setMesh(ModelLoader::loadAuto(obj.string()));
    b->setMesh(ModelLoader::loadAuto(obj.string()));
    c->setMesh(std::make_shared<Mesh>());
    CHECK(maxAbsX(*a->getMesh()) == 1.0f);

    // Un override de textura del objeto A: tiene que sobrevivir al reimport.
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/x.png";
    a->materialOverrides = { ov };

    ModelImportSettings s;
    s.scale = 2.0f;
    std::string err;
    CHECK(saveModelImportSettings(obj, s, &err));

    const ModelReimportResult r = reimportModelUsers(&root, obj, nullptr);
    CHECK(r.reimported == 2);
    CHECK(r.skipped == 0);
    CHECK(r.warnings.empty());
    CHECK(maxAbsX(*a->getMesh()) == 2.0f);
    CHECK(maxAbsX(*b->getMesh()) == 2.0f);
    CHECK(c->getMesh()->sourcePath.empty());                          // no era de ese FBX
    CHECK(a->getMesh()->sourcePath == obj.string());
    CHECK(a->materialOverrides[0].albedo == "assets/x.png");
    CHECK(a->getMesh()->material.texturePath == "assets/x.png");      // reaplicado sobre la malla nueva
}

// Review Focus 2: el FBX ya no carga -> los objetos se quedan como estaban.
static void test_reimport_of_an_unloadable_fbx_leaves_the_objects_intact()
{
    const fs::path obj = writeTriangleObj("dt_cb_reimport_missing");
    GameObject root("root");
    GameObject* a = root.addChild("A");
    a->setMesh(ModelLoader::loadAuto(obj.string()));
    const Mesh* antes = a->getMesh().get();

    std::error_code ec;
    fs::remove(obj, ec);                                               // el fichero desaparece

    const ModelReimportResult r = reimportModelUsers(&root, obj, nullptr);
    CHECK(r.reimported == 0);
    CHECK(r.skipped == 1);
    CHECK(r.warnings.size() == 1);
    CHECK(a->hasMesh());
    CHECK(a->getMesh().get() == antes);
    CHECK(maxAbsX(*a->getMesh()) == 1.0f);
}

// Sin usuarios no hay nada que hacer ni que avisar.
static void test_reimport_without_users_is_a_noop()
{
    const fs::path obj = writeTriangleObj("dt_cb_reimport_nousers");
    GameObject root("root");
    root.addChild("A")->setMesh(std::make_shared<Mesh>());
    const ModelReimportResult r = reimportModelUsers(&root, obj, nullptr);
    CHECK(r.reimported == 0 && r.skipped == 0 && r.warnings.empty());
    const ModelReimportResult r2 = reimportModelUsers(nullptr, obj, nullptr);
    CHECK(r2.reimported == 0 && r2.skipped == 0);
}

// Defensivo: un objeto con malla y a la vez con una carga asincrona en vuelo no
// se toca (mismo criterio que MeshComponentCommand::put).
static void test_reimport_skips_an_object_with_a_pending_load()
{
    const fs::path obj = writeTriangleObj("dt_cb_reimport_pending");
    GameObject root("root");
    GameObject* a = root.addChild("A");
    a->setMesh(ModelLoader::loadAuto(obj.string()));
    a->pendingMeshJob = 7;

    ModelImportSettings s;
    s.scale = 2.0f;
    std::string err;
    CHECK(saveModelImportSettings(obj, s, &err));

    const ModelReimportResult r = reimportModelUsers(&root, obj, nullptr);
    CHECK(r.reimported == 0);
    CHECK(r.skipped == 1);
    CHECK(r.warnings.size() == 1);
    CHECK(maxAbsX(*a->getMesh()) == 1.0f);
}

// Review Focus 3: un personaje con clip renombrado, fuente externa y Animator.
static void test_reimport_skinned_keeps_the_animation_config_and_rebinds()
{
    std::error_code ec;
    const fs::path d = fs::temp_directory_path(ec) / "dt_cb_reimport_skinned";
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    const fs::path fbx = d / "char.fbx";
    fs::copy_file("assets/modelAnimation.fbx", fbx, fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);

    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned(fbx.string()));
    std::vector<std::string> w;
    CHECK(addAnimationSource(*mesh, fbx.string(), w));
    CHECK(mesh->animationSources.size() == 2u);
    if (mesh->animationSources.size() != 2u) return;
    CHECK(renameClip(*mesh, mesh->animationSources[0].clipNames[0], "CaminarRenombrado"));
    CHECK(renameClip(*mesh, mesh->animationSources[1].clipNames[0], "SaltoRenombrado"));
    const float extentAntes = [&] {
        float r = 0.0f;
        for (const SkinnedVertex& v : mesh->skinnedVertices) r = std::max(r, std::abs(v.position.x));
        return r;
    }();

    GameObject root("root");
    GameObject* go = root.addChild("Personaje");
    go->setMesh(mesh);
    auto anim = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State st;
    st.name = "Salto"; st.clipName = "SaltoRenombrado";
    anim->addState(st);
    go->setAnimator(anim);

    ModelImportSettings s;
    s.scale = 2.0f;
    std::string err;
    CHECK(saveModelImportSettings(fbx, s, &err));

    const ModelReimportResult r = reimportModelUsers(&root, fbx, nullptr);
    CHECK(r.reimported == 1);
    CHECK(r.skipped == 0);

    const SkinnedMesh* nuevo = go->getSkinnedMesh();
    CHECK(nuevo != nullptr);
    if (!nuevo) return;
    CHECK(nuevo != mesh.get());                                        // malla nueva, no la vieja
    float extentDespues = 0.0f;
    for (const SkinnedVertex& v : nuevo->skinnedVertices)
        extentDespues = std::max(extentDespues, std::abs(v.position.x));
    CHECK(std::abs(extentDespues - 2.0f * extentAntes) <= 1e-3f * std::max(1.0f, extentAntes));

    CHECK(nuevo->animationSources.size() == 2u);
    if (nuevo->animationSources.size() == 2u)
    {
        CHECK(nuevo->animationSources[0].builtin);
        CHECK(nuevo->animationSources[0].clipNames[0] == "CaminarRenombrado");
        CHECK(!nuevo->animationSources[1].builtin);
        CHECK(nuevo->animationSources[1].clipNames[0] == "SaltoRenombrado");
    }
    CHECK(go->getAnimator()->states()[0].clipIndex >= 0);              // rebindClips lo resolvio
}
```

Y en `main()`, antes de `test_classify_material_extension();`:

```cpp
    test_reimport_replaces_the_meshes_of_that_fbx_only();
    test_reimport_of_an_unloadable_fbx_leaves_the_objects_intact();
    test_reimport_without_users_is_a_noop();
    test_reimport_skips_an_object_with_a_pending_load();
    test_reimport_skinned_keeps_the_animation_config_and_rebinds();
```

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 5`
Expected: error de compilación: no existe `DonTopo/Editor/ModelReimport.h`.

- [ ] **Step 3: Implement**

`engine/include/DonTopo/Editor/ModelReimport.h`:

```cpp
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace DonTopo {

class EditorRenderer;
class GameObject;

struct ModelReimportResult {
    int                      reimported = 0;   // objetos con la malla ya sustituida
    int                      skipped    = 0;   // no se tocaron (carga en vuelo, o el FBX no carga)
    std::vector<std::string> warnings;         // ya formateados para el Log Console
};

// Vuelve a importar `fbx` (con los ajustes que su sidecar tenga ahora: el
// ModelLoader los lee solo) y sustituye la malla de CADA objeto de la escena que
// venga de ese fichero, conservando transform, hijos, colliders, overrides de
// material y Animator.
//
// Una carga por sourcePath distinto: compartida entre los estaticos, una copia por
// objeto en los skinned (a esa copia se le reaplica la config de fuentes de
// animacion de la malla vieja y se hace rebindClips del Animator).
//
// Si el FBX ya no carga, los objetos se quedan INTACTOS con su malla anterior y
// hay un aviso: nunca queda un objeto sin malla por un reimport fallido.
//
// `renderer` puede ser nullptr (tests headless): entonces solo se cambia la parte
// de CPU. Con renderer, cada objeto pasa por removeMeshComponent -> setMesh ->
// addStaticMesh/addSkinnedMesh (la receta de MeshComponentCommand), con UN solo
// flushUploadsAndWait al final: rebuildStaticMesh NO soporta cambiar vertices.
ModelReimportResult reimportModelUsers(GameObject* sceneRoot,
                                       const std::filesystem::path& fbx,
                                       EditorRenderer* renderer);

} // namespace DonTopo
```

`engine/src/Editor/ModelReimport.cpp`:

```cpp
#include "DonTopo/Editor/ModelReimport.h"

#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/ImportSettings.h"
#include "DonTopo/Renderer/EditorRenderer.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/SkinnedMeshAnimations.h"

#include <exception>
#include <map>
#include <memory>

namespace DonTopo {

ModelReimportResult reimportModelUsers(GameObject* sceneRoot, const std::filesystem::path& fbx,
                                       EditorRenderer* renderer)
{
    ModelReimportResult r;
    if (!sceneRoot) return r;

    // Agrupa por el sourcePath EXACTO que lleva la malla: una carga por grupo.
    std::map<std::string, std::vector<GameObject*>> groups;
    sceneRoot->traverse([&](GameObject* go)
    {
        if (!go->hasMesh()) return;
        const std::string& sp = go->getMesh()->sourcePath;
        if (sp.empty() || !sameAssetPath(sp, fbx)) return;
        groups[sp].push_back(go);
    });

    bool anyRegistered = false;
    for (auto& [sourcePath, users] : groups)
    {
        std::shared_ptr<Mesh> fresh;
        try
        {
            fresh = ModelLoader::loadAuto(sourcePath);
        }
        catch (const std::exception& e)
        {
            r.warnings.push_back("Reimport de '" + fbx.filename().string() + "' fallido: " + e.what() +
                                 " (los objetos se quedan como estaban)");
            r.skipped += static_cast<int>(users.size());
            continue;
        }

        for (GameObject* go : users)
        {
            // Defensivo: un objeto con malla Y una carga asincrona en vuelo no se
            // toca (mismo criterio que MeshComponentCommand::put).
            if (go->pendingMeshJob != 0)
            {
                r.warnings.push_back("Reimport: '" + go->name + "' tiene una carga en curso, se salta");
                ++r.skipped;
                continue;
            }

            std::shared_ptr<const Mesh> next;
            if (const auto* sk = dynamic_cast<const SkinnedMesh*>(fresh.get()))
            {
                auto copy = std::make_shared<SkinnedMesh>(*sk);
                // Antes de soltar la malla vieja: de ella salen los renames y las
                // fuentes externas que el usuario tenia.
                if (const SkinnedMesh* old = go->getSkinnedMesh())
                    applyAnimationSourceConfig(*copy, animationSourceConfigOf(*old), r.warnings);
                next = std::move(copy);
            }
            else
            {
                next = fresh;   // estatica: compartida; quien la edite la copia (editMesh)
            }

            if (renderer) renderer->removeMeshComponent(go);
            else          go->setMesh(nullptr);
            go->setMesh(next);
            // DESPUES de setMesh, que baja los base*Taken: applyMaterialOverrides
            // recaptura como baseline lo que trae la malla NUEVA y reaplica encima
            // los overrides del objeto y su .mat.
            applyMaterialOverrides(*go);

            if (renderer)
            {
                if (const SkinnedMesh* sk = go->getSkinnedMesh())
                    go->skinnedRenderIndex = renderer->addSkinnedMesh(*sk, nullptr);
                else
                    go->staticRenderIndex = renderer->addStaticMesh(*go->getMesh(), nullptr);
                anyRegistered = true;
            }
            if (go->hasAnimator())
                if (const SkinnedMesh* sk = go->getSkinnedMesh())
                    go->getAnimator()->rebindClips(*sk, &r.warnings);

            ++r.reimported;
        }
    }

    // Sin esperar, los objetos recargados aparecerian ~2 frames tarde.
    if (renderer && anyRegistered) renderer->flushUploadsAndWait();
    return r;
}

} // namespace DonTopo
```

`engine/CMakeLists.txt` — en `DonTopoEditor`, tras `src/Editor/Command.cpp`:

```cmake
    src/Editor/ModelReimport.cpp
```

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C|FAILED' | Select-Object -First 5; .\build-ninja\engine\tests\dt_content_browser_tests.exe | Select-Object -Last 4; "exit $LASTEXITCODE"`
Expected: `ALL CONTENT BROWSER TESTS PASSED`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/Editor/ModelReimport.h engine/src/Editor/ModelReimport.cpp engine/CMakeLists.txt engine/tests/content_browser_tests.cpp
git commit -m "feat(editor): reimportModelUsers recarga en vivo los objetos de un FBX sin perder overrides ni Animator"
```

---

### Task 5: Modal "Import Settings" para modelos

**Files:**
- Modify: `engine/include/DonTopo/Editor/ContentBrowserPanel.h` (`:182`, tras `applyAudioImportSettings`, miembros `:333`)
- Modify: `engine/src/Editor/ContentBrowserPanel.cpp` (`importSettingsKindFor` `:715`, `applyAudioImportSettings` `:726`, menú `:1507-1520`, modal `:1775-1845`, includes)
- Test: `engine/tests/content_browser_tests.cpp`

**Interfaces:**
- Consumes (Task 1, 4): `ModelImportSettings`, `loadModelImportSettings`, `saveModelImportSettings`, `kModelScaleMin/Max`, `NormalsMode`, `reimportModelUsers`, `ModelReimportResult`.
- Produces: `ImportSettingsKind::Model`; `struct ModelImportApplyResult { bool ok = false; std::string error; int refreshed = 0; }`; `ModelImportApplyResult applyModelImportSettings(const std::filesystem::path& asset, const ModelImportSettings& settings, const std::function<int(const std::filesystem::path&)>& reimport)`.

- [ ] **Step 1: Write the failing tests** — en `content_browser_tests.cpp`.

Cambiar la línea de `test_import_settings_menu_kind`:

```cpp
    CHECK(importSettingsKindFor(".fbx", false)  == ImportSettingsKind::None);   // modelos: siguiente spec
```

por:

```cpp
    CHECK(importSettingsKindFor(".fbx", false)  == ImportSettingsKind::Model);
    CHECK(importSettingsKindFor(".GLB", false)  == ImportSettingsKind::Model);   // cualquier modelo 3D
    CHECK(importSettingsKindFor(".fbx", true)   == ImportSettingsKind::None);    // una carpeta
```

Y antes de `int main()`:

```cpp
// ── applyModelImportSettings ─────────────────────────────────────────────────

static void test_apply_model_writes_sidecar_and_reimports_once()
{
    std::error_code ec;
    const fs::path d = fs::temp_directory_path(ec) / "dt_cb_apply_model";
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    const fs::path fbx = d / "nave.fbx";
    std::ofstream(fbx) << "fbx";

    ModelImportSettings s;
    s.scale = 0.5f;
    int calls = 0;
    fs::path seen;
    const ModelImportApplyResult r = applyModelImportSettings(
        fbx, s, [&](const fs::path& p) { ++calls; seen = p; return 3; });
    CHECK(r.ok);
    CHECK(r.error.empty());
    CHECK(r.refreshed == 3);
    CHECK(calls == 1);
    CHECK(seen == fbx);
    CHECK(loadModelImportSettings(fbx) == s);
}

// Un fallo de escritura no recarga nada y el modal muestra el error.
static void test_apply_model_write_failure_does_not_reimport()
{
    std::error_code ec;
    const fs::path d = fs::temp_directory_path(ec) / "dt_cb_apply_model_fail";
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    const fs::path fbx = d / "carpeta_que_no_existe" / "nave.fbx";

    ModelImportSettings s;
    s.scale = 0.5f;
    int calls = 0;
    const ModelImportApplyResult r = applyModelImportSettings(
        fbx, s, [&](const fs::path&) { ++calls; return 1; });
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    CHECK(calls == 0);
    CHECK(r.refreshed == 0);
}

// Sin reimport (tests, o sin escena) solo se escribe.
static void test_apply_model_without_reimport_only_writes()
{
    std::error_code ec;
    const fs::path d = fs::temp_directory_path(ec) / "dt_cb_apply_model_plain";
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    const fs::path fbx = d / "nave.fbx";
    std::ofstream(fbx) << "fbx";

    ModelImportSettings s;
    s.flipUVs = false;
    const ModelImportApplyResult r = applyModelImportSettings(fbx, s, nullptr);
    CHECK(r.ok);
    CHECK(r.refreshed == 0);
    CHECK(loadModelImportSettings(fbx) == s);
}
```

Y en `main()`, antes de `test_classify_material_extension();`:

```cpp
    test_apply_model_writes_sidecar_and_reimports_once();
    test_apply_model_write_failure_does_not_reimport();
    test_apply_model_without_reimport_only_writes();
```

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 5`
Expected: error de compilación (`ImportSettingsKind::Model`, `ModelImportApplyResult`, `applyModelImportSettings` sin declarar).

- [ ] **Step 3: Implement**

3a. `ContentBrowserPanel.h`. Cambiar:

```cpp
enum class ImportSettingsKind { None, Texture, Audio };
```

por:

```cpp
enum class ImportSettingsKind { None, Texture, Audio, Model };
```

y, tras la declaración de `applyAudioImportSettings`:

```cpp

struct ModelImportApplyResult {
    bool        ok = false;
    std::string error;       // causa si !ok (el modal la muestra y no se cierra)
    int         refreshed = 0;   // objetos que reimport dice haber recargado
};
// Escribe el sidecar del modelo (el defecto lo borra) y, SOLO si se pudo, llama a
// `reimport(asset)` una vez: en el panel recarga en vivo los objetos de la escena
// que usan ese FBX (reimportModelUsers) y devuelve cuantos. Sin `reimport` solo se
// escribe. Un fallo de escritura no recarga nada.
ModelImportApplyResult applyModelImportSettings(
    const std::filesystem::path& asset,
    const ModelImportSettings& settings,
    const std::function<int(const std::filesystem::path&)>& reimport);
```

y el miembro, tras `AudioImportSettings    m_importAudioEdit;`:

```cpp
    ModelImportSettings    m_importModelEdit;
```

3b. `ContentBrowserPanel.cpp`. Añadir `#include "DonTopo/Editor/ModelReimport.h"` junto a los demás includes de `DonTopo/Editor/`.

`importSettingsKindFor`:

```cpp
        case AssetKind::Image: return ImportSettingsKind::Texture;
        case AssetKind::Audio: return ImportSettingsKind::Audio;
```

pasa a:

```cpp
        case AssetKind::Image:   return ImportSettingsKind::Texture;
        case AssetKind::Audio:   return ImportSettingsKind::Audio;
        case AssetKind::Model3D: return ImportSettingsKind::Model;
```

Tras `applyAudioImportSettings` (antes de `countSceneReferences`):

```cpp
ModelImportApplyResult applyModelImportSettings(
    const std::filesystem::path& asset,
    const ModelImportSettings& settings,
    const std::function<int(const std::filesystem::path&)>& reimport)
{
    ModelImportApplyResult r;
    if (!saveModelImportSettings(asset, settings, &r.error))
        return r;                                    // nada recargado si no se pudo escribir
    r.ok = true;
    if (reimport) r.refreshed = reimport(asset);
    return r;
}
```

Menú contextual (`:1507`): cambiar el comentario y la carga:

```cpp
                // Ajustes de importacion: solo de UN asset con ajustes (textura, audio o modelo).
```

y

```cpp
                    if (importKind == ImportSettingsKind::Audio)
                        m_importAudioEdit = loadAudioImportSettings(path);
                    else
                        m_importEdit = loadTextureImportSettings(path);
```

por:

```cpp
                    if (importKind == ImportSettingsKind::Audio)
                        m_importAudioEdit = loadAudioImportSettings(path);
                    else if (importKind == ImportSettingsKind::Model)
                        m_importModelEdit = loadModelImportSettings(path);
                    else
                        m_importEdit = loadTextureImportSettings(path);
```

Modal — el comentario de arriba (`// Ajustes de importacion de UNA textura (menu contextual)...`) pasa a decir "de UN asset (textura, audio o modelo)". Y el cuerpo, donde hoy hay `if (m_importKind == ImportSettingsKind::Audio) { ... } else { ...textura... }`, añadir una rama entre las dos:

```cpp
            else if (m_importKind == ImportSettingsKind::Model)
            {
                ImGui::DragFloat("Scale", &m_importModelEdit.scale, 0.01f,
                                 kModelScaleMin, kModelScaleMax, "%.4f");
                int normals = static_cast<int>(m_importModelEdit.normals);
                if (ImGui::Combo("Normals", &normals,
                                 "Del fichero (planas si faltan)\0Suaves (regenera)\0Planas (regenera)\0"))
                    m_importModelEdit.normals = static_cast<NormalsMode>(normals);
                ImGui::Checkbox("Recalcular tangentes", &m_importModelEdit.calcTangents);
                ImGui::Checkbox("Voltear UVs", &m_importModelEdit.flipUVs);
                ImGui::Checkbox("Importar animaciones", &m_importModelEdit.importAnimations);
                ImGui::TextDisabled("Se aplica a todos los objetos que usan este modelo.");
                ImGui::TextDisabled("Los colliders no se re-dimensionan.");
            }
```

(queda `if (Audio) {...} else if (Model) {...} else {...}`).

Y en los botones, entre el `if (apply && m_importKind == ImportSettingsKind::Audio) { ... }` y el `else if (apply)` de textura, otra rama:

```cpp
            else if (apply && m_importKind == ImportSettingsKind::Model)
            {
                const ModelImportApplyResult r = applyModelImportSettings(
                    m_importTarget, m_importModelEdit,
                    [&](const std::filesystem::path& p)
                    {
                        const ModelReimportResult mr = reimportModelUsers(sceneRoot, p, ctx.renderer);
                        for (const std::string& w : mr.warnings) ctx.pushLog(w);
                        return mr.reimported;
                    });
                if (r.ok)
                {
                    ctx.pushLog("Import settings aplicados: " + m_importTarget.filename().string() +
                                " (" + std::to_string(r.refreshed) + " objeto(s) recargados)");
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    m_importError = r.error;   // el modal NO se cierra
                }
            }
```

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C|FAILED' | Select-Object -First 5; .\build-ninja\engine\tests\dt_content_browser_tests.exe | Select-Object -Last 4; "exit $LASTEXITCODE"`
Expected: `ALL CONTENT BROWSER TESTS PASSED`, exit 0.

- [ ] **Step 5: Smoke del editor** (la UI es ImGui puro, sin seam headless)

Run: `Get-Process Sandbox -ErrorAction SilentlyContinue | Stop-Process -Force; $p = Start-Process .\build-ninja\sandbox\Sandbox.exe -WorkingDirectory .\build-ninja\sandbox -PassThru; Start-Sleep 7; if ($p.HasExited) { "EXITED early code $($p.ExitCode)" } else { "alive"; $p.CloseMainWindow() | Out-Null; Start-Sleep 2; if (-not $p.HasExited) { $p.Kill() } }`
Expected: `alive`. (Ningún agente puede verificar lo visual: lo hace el usuario al final.)

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Editor/ContentBrowserPanel.h engine/src/Editor/ContentBrowserPanel.cpp engine/tests/content_browser_tests.cpp
git commit -m "feat(editor): Import Settings de modelos en el Content Browser con recarga en vivo de sus usuarios"
```

---

### Task 6: Exportador — el sidecar del FBX y el de sus fuentes de animación

**Files:**
- Modify: `engine/src/Editor/GameExporter.cpp` (`collectSceneAssets`, bloque `if (go->hasMesh())` `:263-280`)
- Test: `engine/tests/exporter_tests.cpp`

**Interfaces:**
- Consumes (Task 1): `saveModelImportSettings`. El helper `addWithSidecar` ya existe (`GameExporter.cpp:251`).
- Produces: `collectSceneAssets` añade `<fbx>.import.json` junto al FBX y a cada fuente de animación que lo tenga.

- [ ] **Step 1: Write the failing test** — en `exporter_tests.cpp`, justo antes de `int main()`:

```cpp
// Review Focus 6: el sidecar de un modelo viaja con el FBX, y el de cada fuente de
// animacion externa con SU fichero (cada FBX usa su propio sidecar).
static void test_model_sidecar_travels_with_the_fbx_and_its_animation_sources(const fs::path& root)
{
    std::error_code ec;
    const fs::path hero = root / "assets" / "hero.fbx";
    const fs::path run  = root / "assets" / "run.fbx";
    const fs::path idle = root / "assets" / "idle.fbx";          // sin sidecar
    const fs::path prop = root / "assets" / "prop.fbx";          // estatico, sin sidecar
    for (const fs::path& p : { hero, run, idle, prop })
        std::ofstream(p) << "fbx";

    ModelImportSettings s;
    s.scale = 0.01f;
    std::string err;
    CHECK(saveModelImportSettings(hero, s, &err));
    CHECK(saveModelImportSettings(run,  s, &err));

    Scene scene;
    auto* personaje = scene.addGameObject("personaje");
    auto skinned = std::make_shared<SkinnedMesh>();
    skinned->sourcePath = hero.string();
    skinned->animationSources.push_back({ hero.string(), true,  {} });
    skinned->animationSources.push_back({ run.string(),  false, {} });
    skinned->animationSources.push_back({ idle.string(), false, {} });
    personaje->setMesh(skinned);
    scene.addGameObject("prop")->setMesh(makeMesh(prop));

    const std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    std::vector<std::string> pkg;
    for (const ExportAsset& a : assets) pkg.push_back(a.packagePath);
    auto has = [&](const std::string& p) { return std::find(pkg.begin(), pkg.end(), p) != pkg.end(); };

    CHECK(has("assets/hero.fbx"));
    CHECK(has("assets/hero.fbx.import.json"));
    CHECK(has("assets/run.fbx.import.json"));
    CHECK(has("assets/idle.fbx"));
    CHECK(!has("assets/idle.fbx.import.json"));
    CHECK(has("assets/prop.fbx"));
    CHECK(!has("assets/prop.fbx.import.json"));
    for (const ExportAsset& a : assets)
        if (a.packagePath.find(".import.json") != std::string::npos) CHECK(a.existsOnDisk);

    for (const fs::path& p : { hero, run, idle, prop })
    {
        fs::remove(importSidecarPath(p), ec);
        fs::remove(p, ec);
    }
}
```

Y en `main()`, tras `test_texture_sidecar_travels_with_the_texture(root);`:

```cpp
    test_model_sidecar_travels_with_the_fbx_and_its_animation_sources(root);
```

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C|FAILED' | Select-Object -First 5; .\build-ninja\engine\tests\dt_exporter_tests.exe | Select-Object -First 4; "exit $LASTEXITCODE"`
Expected: `FAIL` en `has("assets/hero.fbx.import.json")` y `has("assets/run.fbx.import.json")` (hoy solo se añaden los FBX).

- [ ] **Step 3: Implement** — `GameExporter.cpp`, en el bloque `if (go->hasMesh())`:

```cpp
            add(go->getMesh()->sourcePath);

            if (const SkinnedMesh* sm = go->getSkinnedMesh())
                for (const AnimationSource& src : sm->animationSources)
                    add(src.path);   // la fuente builtin repite sourcePath; add() deduplica
```

pasa a:

```cpp
            // Cada FBX lleva SU sidecar de ajustes de modelo (escala, normales...):
            // ModelLoader lo lee junto al fichero, tambien dentro del paquete.
            addWithSidecar(go->getMesh()->sourcePath);

            if (const SkinnedMesh* sm = go->getSkinnedMesh())
                for (const AnimationSource& src : sm->animationSources)
                    addWithSidecar(src.path);   // la builtin repite sourcePath; add() deduplica
```

(Y actualizar el comentario de `addWithSidecar` en `:247-250`: "Un asset con ajustes de importacion (textura de material, clip de audio, modelo) lleva su sidecar".)

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C|FAILED' | Select-Object -First 5; .\build-ninja\engine\tests\dt_exporter_tests.exe | Select-Object -Last 2; "exit $LASTEXITCODE"`
Expected: `OK`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add engine/src/Editor/GameExporter.cpp engine/tests/exporter_tests.cpp
git commit -m "feat(export): el FBX y sus fuentes de animacion viajan con su sidecar de ajustes de modelo"
```

---

### Task 7: Documentación y cierre del audit

**Files:**
- Modify: `docs/assets-editor-audit.md` (ítem "Panel de \"import settings\" por asset", `:172-176`)
- Modify: `README.md` (párrafo del Content Browser, tras la frase del audio)

- [ ] **Step 1:** Con `Edit` (nunca reescribir el fichero entero; comprobar `git diff --stat` tras cada cambio):
  - Audit: localizar `- [ ] **Panel de "import settings" por asset**` y marcarlo **CERRADO** (`[x]`), reemplazando su texto por una frase: sidecar `<asset>.import.json` con `type` (texturas, audio y modelos), edición desde "Import Settings…" del Content Browser, el `ModelLoader` lo lee solo y Aplicar recarga en vivo los usuarios del modelo; specs `2026-09-24-texture-import-settings-design.md`, `2026-09-24-audio-import-settings-design.md` y `2026-09-25-model-import-settings-design.md`.
  - README: tras "...and a gain change reaches a clip that is already playing." añadir: `For models (.fbx and other 3D formats) it offers a uniform scale, the normals mode (from the file, smooth or flat), recalculating tangents, flipping UVs and importing the embedded animations, again in the same sidecar; applying it reloads every object in the scene that uses that model, keeping its transform, material overrides and Animator (it is not undoable). Each FBX uses its own settings, so a character and its animation files (a Mixamo download, for example) need the same scale.`
- [ ] **Step 2: Suite completa** en segundo plano con el snippet: `SUMMARY total 35, fallos:` vacío y `SANDBOX alive after 7s`.
- [ ] **Step 3: Commit**

```bash
git add docs/assets-editor-audit.md README.md
git commit -m "docs: cerrar los import settings por asset (modelos) en el audit"
```

---

## Self-Review

**Spec coverage.**
- §1 Modelo y sidecar (Core) → Task 1.
- §2 Consumo en `ModelLoader` (flags, escala en clips, `importAnimations`) → Task 2.
- §3 Extracción de `applyAnimationSourceConfig` → Task 3.
- §4 Aplicar en vivo (`applyModelImportSettings`, `reimportModelUsers`) → Tasks 4 y 5.
- §5 UI (modal, `importSettingsKindFor`) → Task 5.
- §6 Ciclo de vida y export → ciclo genérico sin cambios (cubierto por los tests de sidecar existentes); exportador en Task 6.
- Verificación del spec: `import_settings_tests` (Task 1), `model_loader` → `model_import_tests` (Task 2, con OBJ controlados y el FBX real del repo), no regresión de escena/Animator (Tasks 2-3 y suite), `content_browser_tests` (Tasks 4-5), `exporter_tests` (Task 6). Lo visual (modal, aspecto tras cambiar escala/normales, Vulkan y D3D12) es verificación manual del usuario.
- Riesgos del spec: `GlobalScale` frente al factor de unidades del importador FBX → tests de razón con parada explícita y fallback en Task 2; Aplicar no deshacible, Mixamo y colliders → documentados en README (Task 7) y en el modal; coste de reimport de un skinned pesado → una carga por FBX (Task 4).

**Placeholder scan.** Todos los pasos de código llevan el cuerpo completo. La única sustitución por descripción es el bloque de `Scene.cpp:2184-2250` (Task 3, paso 3c): se identifica por su comentario inicial y su cierre, y el reemplazo va completo.

**Type consistency.** `ModelImportSettings`/`NormalsMode`/`loadModelImportSettings`/`saveModelImportSettings`/`kModelScaleMin`/`kModelScaleMax` (Task 1) se usan con esos nombres en 2, 4, 5 y 6. `AnimationSourceConfig`/`animationSourceConfigOf`/`applyAnimationSourceConfig` (Task 3) en 3 y 4. `ModelReimportResult`/`reimportModelUsers` (Task 4) en 5. `ModelImportApplyResult`/`applyModelImportSettings`/`ImportSettingsKind::Model` (Task 5) solo dentro de la Task 5.

**Review Focus.** 1→Tasks 1 y 2, 2→Task 4, 3→Task 4, 4→Task 2, 5→Task 2, 6→Task 6.

## Entrega (para quien ejecute)

Al terminar la Task 7, pedir al usuario la verificación manual en GUI, **en Vulkan y en D3D12** (recordar que el backend sale del último proyecto abierto): clic derecho sobre un `.fbx` → Import Settings…; con Scale 0.01 el modelo ya en escena se ve 100 veces más pequeño y un personaje skinned sigue animándose sin deformarse ni perder su Animator; Flat frente a Suaves cambia el sombreado; Voltear UVs invierte la textura; "Importar animaciones" apagado deja al personaje sin clips y el Animator avisa de huérfanos; mover y renombrar el FBX conservan los ajustes; el juego exportado los respeta; cierre limpio en D3D12 y syncval de Vulkan con control positivo. Ningún agente puede verificar lo visual: decirlo así en el mensaje final.
