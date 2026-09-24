# Ajustes de importación por clip de audio — Plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cada clip de audio puede llevar un sidecar `<clip>.import.json` con ganancia en dB y "forzar a mono", editable desde el Content Browser y respetado por el editor y por el juego exportado.

**Architecture:** `ImportSettings` (Core) gana el tipo `audio` reutilizando el envoltorio del sidecar de texturas. `AudioManager` lee el sidecar al crear un sonido, guarda los ajustes y el último volumen del componente **por soundId**, y multiplica la ganancia en un único helper (`voiceVolume`) usado por los tres sitios donde se fija el volumen de una voz. El mono es una matriz de mezcla por voz (solo sonidos 2D). Cambiar los ajustes en caliente pasa por `AudioManager::refreshImportSettings(path)`, y el modal del Content Browser lo llama al Aplicar.

**Tech Stack:** C++20, FMOD Core (`DT_FMOD_ENABLED`), nlohmann_json, ImGui. Tests planos `main()` + `CHECK` con `dt_add_test`, ejecutados desde la raíz del repo.

**Spec:** `docs/superpowers/specs/2026-09-24-audio-import-settings-design.md`

## Global Constraints

- **Sin dependencias de terceros nuevas.**
- **Defecto = comportamiento actual**: `gainDb = 0`, `forceMono = false`. Sin sidecar el volumen de cada voz es **exactamente** el de antes (`volume * 1.0f`) y no se llama a `setMixMatrix`.
- **El sidecar solo existe si difiere del defecto**: guardar el defecto lo borra.
- **Lectura tolerante, nunca lanza**: ausente (sin aviso), vacío, JSON roto, > 64 KiB, `version` ≠ 1, `type` ≠ `audio`, campo con tipo equivocado → defecto (por campo cuando se pueda) y aviso. `gainDb` no finito → 0; fuera de [-30, +12] → acotado.
- **La ganancia se multiplica en UN solo sitio**: `AudioManager::voiceVolume`. Ningún `->setVolume(volume)` de canal de voz fuera de él (test de política).
- **Ganancia y mono son propiedades de la VOZ, no del `FMOD_MODE`**: no recargan el sonido y no entran en `soundKey`.
- **Mono solo en sonidos 2D**; un clip de un canal o cualquier fallo al leer la matriz deja la voz como está.
- **Los sonidos con FMOD ausente** (`DT_FMOD_ENABLED` sin definir, como el CI de Linux): los métodos nuevos de `AudioManager` existen FUERA del `#ifdef` con implementación neutra (`-1`, defecto, no-op) para que los tests compilen; los tests que necesitan FMOD hacen SKIP por `am.available()`.
- **CRLF**: el repo va en CRLF. Tras cada `Edit`, comprobar `git diff --stat` (un cambio pequeño no debe salir como miles de líneas). No usar `sed -i` ni Get-Content/Set-Content en PowerShell.
- **Los tests se ejecutan desde la raíz del repo** (`.\build-ninja\engine\tests\dt_xxx.exe`).
- **Build**: `.\build.bat > $env:TEMP\b.log 2>&1` desde PowerShell y leer solo la cola/los errores.
- **Suite completa**: tarda ~3-5 min (`dt_asset_loader_tests` solo ~2m30) y `Start-Process` no devuelve `ExitCode`. Usar un script con `[Diagnostics.Process]::Start` (ver "Cómo correr la suite") **en segundo plano**. Para `task-done` usar solo los tests dirigidos de la tarea (la suite entera pasa de los 120 s de la herramienta) y ledgerar la suite completa aparte.
- **Todo el audio de estos tests necesita FMOD con dispositivo**: si `am.available()` es falso el test imprime SKIP y no se puede observar el RED. Esta máquina lo tiene (los tests de audio existentes corren).

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

Guardarlo como fichero en el scratchpad de la sesión y lanzarlo con `run_in_background`. Esperado hoy: `total 33` (34 al añadir ningún test nuevo NO: los tests de este plan se añaden a ficheros existentes) y `fallos:` vacío.

## Review Focus

Entradas y fallos que la spec implica y que un test de camino feliz no cubre; cada uno tiene su test en la tarea indicada:

1. **`gainDb` hostil en el sidecar** (`1e30`, `-1e30`, `1e999`, cadena, `null`, y `NaN` pasado directamente al clamp): acotado o defecto, sin lanzar y sin producir una ganancia infinita. → Task 1.
2. **Slot de sonido reciclado**: un clip nuevo que reutiliza el slot de otro no hereda su ganancia ni su volumen guardado. → Task 2.
3. **Refresco selectivo**: solo cambian los soundId de esa ruta (los dos del mismo fichero como 2D y como 3D, sí; otro fichero, no). → Task 2.
4. **Ganancia compuesta dos veces**: tras refrescar, un `setChannelVolume` del componente no acumula la ganancia. → Task 2.
5. **Quitar el sidecar y refrescar** devuelve la voz al volumen sin ganancia. → Task 2.
6. **Mono en un clip de un canal o en un sonido 3D**: no-op, sin crash. → Task 3.
7. **Audio fuera del proyecto** (`assets/_external/N/`): su sidecar viaja también. → Task 4.
8. **El modal no aparece con varios assets seleccionados ni en carpetas**, y con un fallo de escritura no llama al refresco ni se cierra. → Task 5.

---

## File Structure

| Fichero | Responsabilidad |
|---|---|
| `engine/include/DonTopo/Core/ImportSettings.h` / `engine/src/Core/ImportSettings.cpp` (modificar) | `AudioImportSettings`, lector/escritor de tipo `audio`, clamp y ganancia lineal, `sameAssetPath`; el envoltorio común del sidecar se extrae a `readSidecar`. |
| `engine/include/DonTopo/Audio/AudioManager.h` / `engine/src/Audio/AudioManager.cpp` (modificar) | Ajustes por soundId, `voiceVolume`, `refreshImportSettings`, mono por voz, getters de observación. |
| `engine/src/Editor/GameExporter.cpp` (modificar) | El sidecar de un clip viaja con el clip (`addTexture` → `addWithSidecar`). |
| `engine/include/DonTopo/Editor/ContentBrowserPanel.h` / `engine/src/Editor/ContentBrowserPanel.cpp` (modificar) | `applyAudioImportSettings`, entrada de menú y modal para audios. |
| `engine/tests/import_settings_tests.cpp`, `audio_tests.cpp`, `exporter_tests.cpp`, `content_browser_tests.cpp` (ampliar) | Tests. No hay ficheros de test nuevos ni cambios de CMake. |
| `docs/assets-editor-audit.md`, `README.md` (modificar) | Cierre de la parte de audio de U8. |

Un commit por tarea. Ninguna tarea toca la GPU.

---

### Task 1: `AudioImportSettings` en Core

**Files:**
- Modify: `engine/include/DonTopo/Core/ImportSettings.h`
- Modify: `engine/src/Core/ImportSettings.cpp`
- Test: `engine/tests/import_settings_tests.cpp`

**Interfaces:**
- Consumes: lo que ya hay en `ImportSettings.h` (`TextureImportSettings`, `importSidecarPath`, `isImportSidecar`, `kImportSidecarSuffix`).
- Produces (namespace `DonTopo`; las usan las tareas 2–5):
  ```cpp
  struct AudioImportSettings { float gainDb = 0.0f; bool forceMono = false; };
  inline constexpr float kAudioGainMinDb = -30.0f;
  inline constexpr float kAudioGainMaxDb = 12.0f;
  inline bool operator==(const AudioImportSettings&, const AudioImportSettings&);
  inline bool isDefault(const AudioImportSettings&);
  float clampAudioGainDb(float gainDb);              // acota a [-30, +12]; NaN -> 0
  float audioGainLinear(float gainDb);               // 10^(dB/20), con el dB acotado antes
  AudioImportSettings loadAudioImportSettings(const std::filesystem::path& asset, std::string* warning = nullptr);
  bool saveAudioImportSettings(const std::filesystem::path& asset, const AudioImportSettings&, std::string* error = nullptr);
  bool sameAssetPath(const std::filesystem::path& a, const std::filesystem::path& b);   // mismo fichero, exista o no
  ```

- [ ] **Step 1: Write the failing tests** — en `import_settings_tests.cpp` añadir (incluir `<cmath>` y `<limits>`; llamarlos desde `main()`):

```cpp
static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

static void test_audio_gain_math()
{
    CHECK(near(audioGainLinear(0.0f), 1.0f, 0.0f));          // EXACTO: sin ajuste el volumen no cambia
    CHECK(near(audioGainLinear(-6.0f), 0.501187f, 1e-4f));
    CHECK(near(audioGainLinear(12.0f), 3.981072f, 1e-3f));
    CHECK(near(audioGainLinear(-30.0f), 0.031623f, 1e-4f));
    // Fuera de rango se acota ANTES de convertir.
    CHECK(near(audioGainLinear(1000.0f), audioGainLinear(12.0f), 1e-3f));
    CHECK(near(audioGainLinear(-1000.0f), audioGainLinear(-30.0f), 1e-5f));
    // Review Focus 1: NaN no llega a la ganancia.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(clampAudioGainDb(nan) == 0.0f);
    CHECK(clampAudioGainDb(inf) == kAudioGainMaxDb);
    CHECK(clampAudioGainDb(-inf) == kAudioGainMinDb);
    CHECK(near(audioGainLinear(nan), 1.0f, 0.0f));
}

static void test_audio_roundtrip_and_default_removes_sidecar()
{
    const fs::path d = makeDir();
    const fs::path clip = d / "disparo.wav";
    std::string err;

    AudioImportSettings in;
    in.gainDb    = -6.5f;
    in.forceMono = true;
    CHECK(saveAudioImportSettings(clip, in, &err));
    CHECK(fs::exists(importSidecarPath(clip)));
    CHECK(loadAudioImportSettings(clip) == in);

    CHECK(saveAudioImportSettings(clip, AudioImportSettings{}, &err));      // el defecto BORRA
    CHECK(!fs::exists(importSidecarPath(clip)));
    CHECK(saveAudioImportSettings(clip, AudioImportSettings{}, &err));      // y sin fichero previo no es error

    // Guardar un dB fuera de rango escribe el valor ACOTADO.
    AudioImportSettings hot;
    hot.gainDb = 50.0f;
    CHECK(saveAudioImportSettings(clip, hot, &err));
    CHECK(loadAudioImportSettings(clip).gainDb == kAudioGainMaxDb);
}

static void test_audio_missing_and_broken_are_default()
{
    const fs::path d = makeDir();
    std::string warning = "x";
    CHECK(isDefault(loadAudioImportSettings(d / "no_existe.wav", &warning)));
    CHECK(warning.empty());                                    // ausente no es un problema

    writeText(importSidecarPath(d / "roto.wav"), "{ esto no es json");
    warning.clear();
    CHECK(isDefault(loadAudioImportSettings(d / "roto.wav", &warning)));
    CHECK(!warning.empty());
}

// Un sidecar de textura leido como audio (y al reves) da el defecto con aviso.
static void test_audio_type_crossing()
{
    const fs::path d = makeDir();
    std::string err;
    TextureImportSettings tex;
    tex.mipmaps = true;
    CHECK(saveTextureImportSettings(d / "x.wav", tex, &err));
    std::string warning;
    CHECK(isDefault(loadAudioImportSettings(d / "x.wav", &warning)));
    CHECK(!warning.empty());

    AudioImportSettings au;
    au.gainDb = 3.0f;
    CHECK(saveAudioImportSettings(d / "y.png", au, &err));
    warning.clear();
    CHECK(isDefault(loadTextureImportSettings(d / "y.png", &warning)));
    CHECK(!warning.empty());
}

// Review Focus 1: valores hostiles en gainDb.
static void test_audio_hostile_gain_values()
{
    const fs::path d = makeDir();
    struct Case { const char* name; const char* json; float wantGain; bool wantMono; };
    const Case cases[] = {
        { "big.wav",   R"({"version":1,"type":"audio","gainDb":1e30,"forceMono":true})",    kAudioGainMaxDb, true  },
        { "small.wav", R"({"version":1,"type":"audio","gainDb":-1e30,"forceMono":false})",  kAudioGainMinDb, false },
        { "str.wav",   R"({"version":1,"type":"audio","gainDb":"alto","forceMono":true})",  0.0f,            true  },
        { "null.wav",  R"({"version":1,"type":"audio","gainDb":null,"forceMono":true})",    0.0f,            true  },
        { "mono.wav",  R"({"version":1,"type":"audio","gainDb":2.5,"forceMono":"si"})",     2.5f,            false },
    };
    for (const Case& c : cases)
    {
        writeText(importSidecarPath(d / c.name), c.json);
        std::string warning;
        const AudioImportSettings s = loadAudioImportSettings(d / c.name, &warning);
        CHECK(s.gainDb == c.wantGain);
        CHECK(s.forceMono == c.wantMono);
        CHECK(std::isfinite(s.gainDb));
        CHECK(!warning.empty());
    }
    // 1e999 desborda el double: el parser lo da como infinito o lo descarta; en
    // los dos casos el resultado es finito y sin lanzar.
    writeText(importSidecarPath(d / "inf.wav"), R"({"version":1,"type":"audio","gainDb":1e999})");
    CHECK(std::isfinite(loadAudioImportSettings(d / "inf.wav").gainDb));

    // Sidecar hostil de tamano: mismo tope que las texturas.
    writeText(importSidecarPath(d / "huge.wav"), std::string(5 * 1024 * 1024, 'x'));
    std::string warning;
    CHECK(isDefault(loadAudioImportSettings(d / "huge.wav", &warning)));
    CHECK(!warning.empty());
}

static void test_same_asset_path()
{
    const fs::path d = makeDir();
    writeText(d / "a.wav", "x");
    CHECK(sameAssetPath(d / "a.wav", d / "a.wav"));
    CHECK(sameAssetPath(d / "sub" / ".." / "a.wav", d / "a.wav"));      // normaliza
    CHECK(!sameAssetPath(d / "a.wav", d / "b.wav"));
    // Ninguno existe: se compara lexicamente, sin lanzar.
    CHECK(sameAssetPath(d / "no" / "x.wav", d / "no" / "x.wav"));
    CHECK(!sameAssetPath(d / "no" / "x.wav", d / "no" / "y.wav"));
    CHECK(!sameAssetPath("", d / "a.wav"));
}
```

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 5`
Expected: FAIL de compilación — `audioGainLinear`, `AudioImportSettings`, `sameAssetPath`… no existen.

- [ ] **Step 3: Implement**

`ImportSettings.h` — añadir antes del bloque "Ciclo de vida del sidecar":

```cpp
// ── Audio ────────────────────────────────────────────────────────────────────

// Propiedades del FICHERO de audio, no del componente: la ganancia se suma al
// volumen de toda voz de ese clip y el mono mezcla sus canales a uno.
struct AudioImportSettings
{
    float gainDb    = 0.0f;    // [kAudioGainMinDb, kAudioGainMaxDb]
    bool  forceMono = false;
};
inline constexpr float kAudioGainMinDb = -30.0f;
inline constexpr float kAudioGainMaxDb = 12.0f;

inline bool operator==(const AudioImportSettings& a, const AudioImportSettings& b)
{
    return a.gainDb == b.gainDb && a.forceMono == b.forceMono;
}
inline bool isDefault(const AudioImportSettings& s) { return s == AudioImportSettings{}; }

// Acota a [-30, +12] dB; NaN -> 0. Una ganancia sin techo, o NaN metida en un
// canal de FMOD, ensordece o silencia sin decir nada.
float clampAudioGainDb(float gainDb);
// 10^(dB/20), con el dB acotado antes. 0 dB da EXACTAMENTE 1.0f.
float audioGainLinear(float gainDb);

// Misma tolerancia que las texturas (ver loadTextureImportSettings): nunca lanza.
AudioImportSettings loadAudioImportSettings(const std::filesystem::path& asset,
                                            std::string* warning = nullptr);
// Guardar el defecto BORRA el sidecar. El dB se escribe ya acotado.
bool saveAudioImportSettings(const std::filesystem::path& asset,
                             const AudioImportSettings& settings,
                             std::string* error = nullptr);

// Dos rutas que nombran el mismo fichero (exista o no). Existentes: equivalent;
// si no, la forma canonica debil de cada una, y en ultimo caso la lexica.
bool sameAssetPath(const std::filesystem::path& a, const std::filesystem::path& b);
```

`ImportSettings.cpp` — (a) añadir `#include <cmath>`, `<algorithm>`, `<optional>` arriba; (b) en el namespace anónimo añadir `readSidecar` (es el cuerpo de validación que hoy vive dentro de `loadTextureImportSettings`, movido y parametrizado por el tipo esperado) y `writeSidecar`; (c) reescribir `loadTextureImportSettings` y `saveTextureImportSettings` sobre ellos; (d) añadir lo nuevo.

```cpp
// Dentro del namespace anonimo, tras kMaxSidecarBytes:

// Lee y valida el ENVOLTORIO comun de un sidecar (existencia, tamano, JSON,
// version y tipo). nullopt = usar el defecto: `warning` explica por que salvo si
// el fichero simplemente no existe o esta vacio (lo normal, sin aviso).
std::optional<nlohmann::json> readSidecar(const std::filesystem::path& asset,
                                          const char* expectedType, std::string* warning)
{
    if (warning) warning->clear();
    auto warn = [&](const std::string& m) { if (warning) *warning = m; };

    const std::filesystem::path sidecar = importSidecarPath(asset);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(sidecar, ec) || ec)
        return std::nullopt;

    const std::uintmax_t size = std::filesystem::file_size(sidecar, ec);
    if (ec || size > kMaxSidecarBytes)
    {
        warn("sidecar ilegible o demasiado grande; se usan los valores por defecto");
        return std::nullopt;
    }
    if (size == 0)
        return std::nullopt;

    std::ifstream in(sidecar, std::ios::binary);
    if (!in)
    {
        warn("no se pudo abrir el sidecar; se usan los valores por defecto");
        return std::nullopt;
    }
    const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };

    nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object())
    {
        warn("JSON invalido; se usan los valores por defecto");
        return std::nullopt;
    }
    const auto version = j.find("version");
    if (version == j.end() || !version->is_number_integer() || version->get<long long>() != 1)
    {
        warn("version de sidecar desconocida; se usan los valores por defecto");
        return std::nullopt;
    }
    const auto type = j.find("type");
    if (type == j.end() || !type->is_string() || type->get<std::string>() != expectedType)
    {
        warn(std::string("el sidecar no es de tipo ") + expectedType +
             "; se usan los valores por defecto");
        return std::nullopt;
    }
    return j;
}

// Escritura por fichero temporal + rename: un corte a mitad no deja un sidecar a medias.
bool writeSidecar(const std::filesystem::path& asset, const nlohmann::json& j, std::string* error)
{
    const std::filesystem::path sidecar = importSidecarPath(asset);
    std::error_code ec;
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
            out.close();
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

// Guardar el defecto = quitar el sidecar; ausente tampoco es error.
bool removeSidecar(const std::filesystem::path& asset, std::string* error)
{
    std::error_code ec;
    const std::filesystem::path sidecar = importSidecarPath(asset);
    std::filesystem::remove(sidecar, ec);
    if (ec)
    {
        if (error) *error = "no se pudo borrar " + sidecar.string() + ": " + ec.message();
        return false;
    }
    return true;
}
```

`loadTextureImportSettings` pasa a ser:

```cpp
TextureImportSettings loadTextureImportSettings(const std::filesystem::path& asset,
                                                std::string* warning)
{
    TextureImportSettings out;
    const std::optional<nlohmann::json> j = readSidecar(asset, "texture", warning);
    if (!j) return out;

    std::string problems;
    if (const auto it = j->find("colorSpace"); it != j->end())
    {
        const std::string v = it->is_string() ? it->get<std::string>() : std::string();
        if      (v == "auto")   out.colorSpace = ColorSpaceOverride::Auto;
        else if (v == "srgb")   out.colorSpace = ColorSpaceOverride::Srgb;
        else if (v == "linear") out.colorSpace = ColorSpaceOverride::Linear;
        else problems += "colorSpace desconocido (se usa auto). ";
    }
    if (const auto it = j->find("mipmaps"); it != j->end())
    {
        if (it->is_boolean()) out.mipmaps = it->get<bool>();
        else                  problems += "mipmaps no es booleano (se usa false). ";
    }
    if (!problems.empty() && warning) *warning = problems;
    return out;
}
```

`saveTextureImportSettings` pasa a ser:

```cpp
bool saveTextureImportSettings(const std::filesystem::path& asset,
                               const TextureImportSettings& settings, std::string* error)
{
    if (isDefault(settings))
        return removeSidecar(asset, error);

    nlohmann::json j;
    j["version"]    = 1;
    j["type"]       = "texture";
    j["colorSpace"] = colorSpaceName(settings.colorSpace);
    j["mipmaps"]    = settings.mipmaps;
    return writeSidecar(asset, j, error);
}
```

Y lo nuevo (al final, antes de las funciones del ciclo de vida):

```cpp
float clampAudioGainDb(float gainDb)
{
    if (std::isnan(gainDb)) return 0.0f;
    return std::clamp(gainDb, kAudioGainMinDb, kAudioGainMaxDb);
}

float audioGainLinear(float gainDb)
{
    const float db = clampAudioGainDb(gainDb);
    if (db == 0.0f) return 1.0f;                       // exacto: sin ajuste no se toca el volumen
    return std::pow(10.0f, db / 20.0f);
}

AudioImportSettings loadAudioImportSettings(const std::filesystem::path& asset, std::string* warning)
{
    AudioImportSettings out;
    const std::optional<nlohmann::json> j = readSidecar(asset, "audio", warning);
    if (!j) return out;

    std::string problems;
    if (const auto it = j->find("gainDb"); it != j->end())
    {
        if (it->is_number())
        {
            const double v = it->get<double>();
            if (!std::isfinite(v))
                problems += "gainDb no es finito (se usa 0). ";
            else
            {
                out.gainDb = clampAudioGainDb(static_cast<float>(v));
                if (static_cast<double>(out.gainDb) != v)
                    problems += "gainDb fuera de rango (acotado). ";
            }
        }
        else problems += "gainDb no es numerico (se usa 0). ";
    }
    if (const auto it = j->find("forceMono"); it != j->end())
    {
        if (it->is_boolean()) out.forceMono = it->get<bool>();
        else                  problems += "forceMono no es booleano (se usa false). ";
    }
    if (!problems.empty() && warning) *warning = problems;
    return out;
}

bool saveAudioImportSettings(const std::filesystem::path& asset, const AudioImportSettings& settings,
                             std::string* error)
{
    AudioImportSettings s = settings;
    s.gainDb = clampAudioGainDb(s.gainDb);
    if (isDefault(s))
        return removeSidecar(asset, error);

    nlohmann::json j;
    j["version"]   = 1;
    j["type"]      = "audio";
    j["gainDb"]    = s.gainDb;
    j["forceMono"] = s.forceMono;
    return writeSidecar(asset, j, error);
}

bool sameAssetPath(const std::filesystem::path& a, const std::filesystem::path& b)
{
    if (a.empty() || b.empty()) return false;
    std::error_code ec;
    if (std::filesystem::equivalent(a, b, ec) && !ec) return true;
    ec.clear();
    const std::filesystem::path ca = std::filesystem::weakly_canonical(a, ec);
    if (ec) return a.lexically_normal() == b.lexically_normal();
    ec.clear();
    const std::filesystem::path cb = std::filesystem::weakly_canonical(b, ec);
    if (ec) return a.lexically_normal() == b.lexically_normal();
    return ca == cb;
}
```

Quitar del `.cpp` las copias antiguas del cuerpo de validación y de escritura que ya no se usan (`saveTextureImportSettings` y `loadTextureImportSettings` originales), y dejar `colorSpaceName` donde está.

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 5; .\build-ninja\engine\tests\dt_import_settings_tests.exe; "exit $LASTEXITCODE"`
Expected: `ALL IMPORT SETTINGS TESTS PASSED`, exit 0 (los tests de texturas anteriores siguen pasando: el refactor no cambia su comportamiento). Correr también `dt_texture_import_tests.exe` y `dt_content_browser_tests.exe` (usan el sidecar de texturas).

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/Core/ImportSettings.h engine/src/Core/ImportSettings.cpp engine/tests/import_settings_tests.cpp
git commit -m "feat(core): AudioImportSettings, sidecar type audio con ganancia acotada y sameAssetPath"
```

---

### Task 2: Ganancia por sonido y refresco en `AudioManager`

**Files:**
- Modify: `engine/include/DonTopo/Audio/AudioManager.h` (público: tras `setChannelPitch` `:176`; privado: tras `m_soundKeys` `:361`)
- Modify: `engine/src/Audio/AudioManager.cpp` (`loadSound` `:237`, `unloadSound` `:314`, `setChannelVolume` `:404`, `playSound` `:451`/`:482`, `playSoundOneShot` `:646`/`:668`)
- Test: `engine/tests/audio_tests.cpp`

**Interfaces:**
- Consumes (Task 1): `AudioImportSettings`, `loadAudioImportSettings`, `audioGainLinear`, `sameAssetPath`.
- Produces (públicos, **fuera** de `#ifdef DT_FMOD_ENABLED` con implementación neutra sin FMOD):
  ```cpp
  AudioImportSettings getSoundImportSettings(int soundId) const;   // defecto si el id no existe
  void  refreshImportSettings(const std::string& path);            // ver abajo
  float getChannelVolume(int soundId) const;                       // volumen REAL de la voz viva, -1 si no hay
  ```
  Privado (dentro del `#ifdef`): `float voiceVolume(int id, float volume) const;`, `std::vector<AudioImportSettings> m_soundImport;`, `std::vector<float> m_soundVolume;` (paralelos a `m_sounds`).

- [ ] **Step 1: Write the failing tests** — en `audio_tests.cpp`. Añadir includes (`"DonTopo/Core/ImportSettings.h"`, `<fstream>`, `<cstdint>`, `<sstream>`) y estos helpers y tests (llamarlos desde `main()` tras `test_mute_time_and_global_pause`):

```cpp
// WAV PCM 16 bit generado en el test: `channels` canales, silencio, `seconds`
// de duracion. Sirve para probar ganancia y mono sin depender de un asset.
static void writeWav(const std::filesystem::path& p, int channels, double seconds)
{
    const uint32_t rate = 44100;
    const uint32_t frames = static_cast<uint32_t>(rate * seconds);
    const uint16_t ch = static_cast<uint16_t>(channels);
    const uint32_t dataBytes = frames * ch * 2;
    auto w32 = [](std::ofstream& o, uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); };
    auto w16 = [](std::ofstream& o, uint16_t v) { o.write(reinterpret_cast<const char*>(&v), 2); };
    std::ofstream o(p, std::ios::binary);
    o.write("RIFF", 4); w32(o, 36 + dataBytes); o.write("WAVE", 4);
    o.write("fmt ", 4); w32(o, 16); w16(o, 1); w16(o, ch); w32(o, rate);
    w32(o, rate * ch * 2); w16(o, static_cast<uint16_t>(ch * 2)); w16(o, 16);
    o.write("data", 4); w32(o, dataBytes);
    const std::string zeros(dataBytes, '\0');
    o.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
}

static bool waitReady(AudioManager& am, int id)
{
    for (int i = 0; i < 600 && am.getSoundState(id) == AudioManager::SoundLoadState::Loading; ++i)
    {
        am.update(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return am.getSoundState(id) == AudioManager::SoundLoadState::Ready;
}

static std::filesystem::path audioTestDir(const char* name)
{
    std::error_code ec;
    std::filesystem::path d = std::filesystem::temp_directory_path(ec) / name;
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d, ec);
    return d;
}

static void setGain(const std::filesystem::path& clip, float db, bool mono = false)
{
    AudioImportSettings s;
    s.gainDb    = db;
    s.forceMono = mono;
    std::string err;
    CHECK(saveAudioImportSettings(clip, s, &err));
}

// Sin FMOD los getters son neutros.
static void test_import_getters_are_neutral_without_a_sound(AudioManager& am)
{
    CHECK(isDefault(am.getSoundImportSettings(-1)));
    CHECK(isDefault(am.getSoundImportSettings(123456)));
    CHECK(am.getChannelVolume(-1) < 0.0f);
    CHECK(am.getChannelVolume(123456) < 0.0f);
    am.refreshImportSettings("no/existe.wav");           // no-op sin sonidos, sin lanzar
    am.refreshImportSettings("");
}

static void test_import_gain_applies_and_refreshes(AudioManager& am)
{
    if (!am.available()) { std::printf("SKIP test_import_gain_applies_and_refreshes (FMOD no disponible)\n"); return; }
    const auto d = audioTestDir("dt_audio_import_gain");
    const std::string a = (d / "a.wav").string();
    const std::string b = (d / "b.wav").string();
    writeWav(a, 2, 3.0);
    writeWav(b, 2, 3.0);
    setGain(a, -6.0f);                                   // factor 0.501187

    // a como 2D y como 3D (flags distintos = dos sonidos), y b sin sidecar.
    const int a2d = am.loadSound(a, false, true);
    const int a3d = am.loadSound(a, true, true);
    const int bId = am.loadSound(b, false, true);
    if (a2d < 0 || a3d < 0 || bId < 0) { CHECK(false); return; }
    CHECK(a2d != a3d);
    if (!waitReady(am, a2d) || !waitReady(am, a3d) || !waitReady(am, bId)) { CHECK(false); return; }

    CHECK(am.getSoundImportSettings(a2d).gainDb == -6.0f);
    CHECK(am.getSoundImportSettings(a3d).gainDb == -6.0f);
    CHECK(isDefault(am.getSoundImportSettings(bId)));

    am.playSound(a2d, {}, 0.8f);
    am.playSound(a3d, {}, 0.8f);
    am.playSound(bId, {}, 0.8f);
    CHECK(nearlyEqual(am.getChannelVolume(a2d), 0.8f * 0.501187f, 1e-3f));
    CHECK(nearlyEqual(am.getChannelVolume(a3d), 0.8f * 0.501187f, 1e-3f));
    CHECK(am.getChannelVolume(bId) == 0.8f);             // sin sidecar: EXACTO, como antes

    // Review Focus 3: el refresco cambia los dos ids de a y NO el de b.
    setGain(a, 6.0f);                                    // factor 1.995262
    am.refreshImportSettings(a);
    CHECK(am.getSoundImportSettings(a2d).gainDb == 6.0f);
    CHECK(nearlyEqual(am.getChannelVolume(a2d), 0.8f * 1.995262f, 1e-3f));
    CHECK(nearlyEqual(am.getChannelVolume(a3d), 0.8f * 1.995262f, 1e-3f));
    CHECK(am.getChannelVolume(bId) == 0.8f);

    // Review Focus 4: tras refrescar, un setChannelVolume del componente NO
    // acumula la ganancia (no es volumenDelCanal x factor).
    am.setChannelVolume(a2d, 0.5f);
    CHECK(nearlyEqual(am.getChannelVolume(a2d), 0.5f * 1.995262f, 1e-3f));
    am.refreshImportSettings(a);                         // refrescar otra vez con el mismo sidecar
    CHECK(nearlyEqual(am.getChannelVolume(a2d), 0.5f * 1.995262f, 1e-3f));

    // Review Focus 5: quitar el sidecar y refrescar devuelve el volumen limpio.
    setGain(a, 0.0f);                                    // el defecto borra el fichero
    CHECK(!std::filesystem::exists(importSidecarPath(a)));
    am.refreshImportSettings(a);
    CHECK(isDefault(am.getSoundImportSettings(a2d)));
    CHECK(am.getChannelVolume(a2d) == 0.5f);

    // Un path distinto o inexistente no toca nada.
    am.refreshImportSettings((d / "otra.wav").string());
    CHECK(am.getChannelVolume(bId) == 0.8f);

    am.stopSound(a2d); am.stopSound(a3d); am.stopSound(bId);
    am.unloadSound(a2d); am.unloadSound(a3d); am.unloadSound(bId);
}

// Review Focus 2: un slot reciclado no hereda ganancia ni volumen guardado.
static void test_import_recycled_slot_does_not_inherit(AudioManager& am)
{
    if (!am.available()) { std::printf("SKIP test_import_recycled_slot_does_not_inherit (FMOD no disponible)\n"); return; }
    const auto d = audioTestDir("dt_audio_import_recycle");
    const std::string a = (d / "a.wav").string();
    const std::string b = (d / "b.wav").string();
    writeWav(a, 2, 3.0);
    writeWav(b, 2, 3.0);
    setGain(a, 12.0f);

    const int idA = am.loadSound(a, false, true);
    if (idA < 0 || !waitReady(am, idA)) { CHECK(false); return; }
    am.playSound(idA, {}, 0.9f);
    CHECK(am.getSoundImportSettings(idA).gainDb == 12.0f);
    am.stopSound(idA);
    am.unloadSound(idA);                                  // libera el slot

    const int idB = am.loadSound(b, false, true);         // sin sidecar: reutiliza el slot de a
    if (idB < 0 || !waitReady(am, idB)) { CHECK(false); return; }
    CHECK(idB == idA);                                    // el test ejercita el reciclado de verdad
    CHECK(isDefault(am.getSoundImportSettings(idB)));
    am.playSound(idB, {}, 0.7f);
    CHECK(am.getChannelVolume(idB) == 0.7f);
    // refrescar b no le mete la ganancia de a.
    am.refreshImportSettings(b);
    CHECK(am.getChannelVolume(idB) == 0.7f);
    am.stopSound(idB);
    am.unloadSound(idB);
}

// Politica: la ganancia se multiplica en UN sitio. Si alguien vuelve a escribir
// ch->setVolume(volume) en un camino de voz, ese camino ignora el sidecar en
// silencio (los one-shots no se pueden observar desde un test, por eso el grep).
static void test_policy_gain_is_applied_only_in_voiceVolume()
{
    std::ifstream in("engine/src/Audio/AudioManager.cpp", std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();
    CHECK(!src.empty());
    auto countOf = [&](const std::string& needle) {
        size_t n = 0;
        for (size_t at = src.find(needle); at != std::string::npos; at = src.find(needle, at + needle.size())) ++n;
        return n;
    };
    CHECK(countOf("setVolume(volume)") == 0);                 // ningun canal recibe el volumen a pelo
    CHECK(countOf("voiceVolume(") >= 4);                      // definicion + 3 usos (mas el refresco)
}
```

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 5`
Expected: fallo de compilación (`getSoundImportSettings`, `getChannelVolume`, `refreshImportSettings` no existen).

- [ ] **Step 3: Minimal scaffolding so the tests compile and show a BEHAVIOUR red**

Añadir a `AudioManager.h` (público, junto a `setChannelPitch`) las tres declaraciones y `#include "DonTopo/Core/ImportSettings.h"`; en `AudioManager.cpp` (fuera de `#ifdef`, junto a los demás métodos) implementaciones **ingenuas**: `getSoundImportSettings` devuelve `{}`, `refreshImportSettings` no hace nada, `getChannelVolume` devuelve `-1.0f`. Build, y en `main()` de `audio_tests.cpp` registrar los tests. Run: `.\build-ninja\engine\tests\dt_audio_tests.exe`
Expected: FAIL en `test_import_gain_applies_and_refreshes` (`getSoundImportSettings(a2d).gainDb == -6.0f`, `getChannelVolume`…), en `test_import_recycled_slot_does_not_inherit` y en `test_policy_gain_is_applied_only_in_voiceVolume` (`countOf("setVolume(volume)") == 0` da 3).

- [ ] **Step 4: Implement**

`AudioManager.h`, privado dentro del `#ifdef DT_FMOD_ENABLED` (tras `m_soundKeys`):

```cpp
    // Ajustes de importacion del fichero de cada sonido (sidecar), y el ULTIMO
    // volumen que el componente pidio para su voz (playSound / setChannelVolume).
    // Paralelos a m_sounds y reciclados con el slot. m_soundVolume existe para que
    // refreshImportSettings reaplique voiceVolume(id, volumenDelComponente) en vez
    // de multiplicar el volumen que ya lleve el canal (compondria la ganancia dos
    // veces). Los one-shots NO lo tocan: su voz no se puede alcanzar.
    std::vector<AudioImportSettings> m_soundImport;
    std::vector<float>               m_soundVolume;

    // Volumen efectivo de una voz de `id`: el pedido por el componente por la
    // ganancia del fichero. El UNICO sitio donde se multiplica la ganancia.
    float voiceVolume(int id, float volume) const;
```

`AudioManager.cpp`:

1. `voiceVolume` (dentro de `#ifdef DT_FMOD_ENABLED`, junto a `liveChannel`, antes de `setChannelVolume`):

```cpp
float AudioManager::voiceVolume(int id, float volume) const
{
    if (id < 0 || id >= static_cast<int>(m_soundImport.size())) return volume;
    return volume * audioGainLinear(m_soundImport[id].gainDb);
}
```

2. `loadSound`: justo tras `if (SYS->createSound(...) != FMOD_OK) return -1;` leer los ajustes, y guardarlos en las dos ramas de reserva de slot:

```cpp
    std::string importWarning;
    const AudioImportSettings importSettings = loadAudioImportSettings(path, &importWarning);
    if (!importWarning.empty())
        std::fprintf(stderr, "[AudioImport] %s: %s\n", path.c_str(), importWarning.c_str());
```
   Rama con slot reciclado: añadir `m_soundImport[id] = importSettings; m_soundVolume[id] = 1.0f;`. Rama `else`: `m_soundImport.push_back(importSettings); m_soundVolume.push_back(1.0f);`. (Solo cuando se CREA el sonido: un acierto de caché ya devolvió antes con sus ajustes.) Incluir `<cstdio>`.

3. `unloadSound`: en el bloque que limpia el slot añadir `m_soundImport[id] = AudioImportSettings{}; m_soundVolume[id] = 1.0f;`.

4. `setChannelVolume`: 

```cpp
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return;
    m_soundVolume[id] = volume;                       // lo ultimo que pidio el componente
    if (FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]))
        ch->setVolume(voiceVolume(id, volume));
```

5. `playSound`: sustituir `ch->setVolume(volume);` por `m_soundVolume[id] = volume; ch->setVolume(voiceVolume(id, volume));`. `playSoundOneShot`: sustituir `ch->setVolume(volume);` por `ch->setVolume(voiceVolume(id, volume));` (sin tocar `m_soundVolume`).

6. Sustituir las tres implementaciones ingenuas por las reales, ubicadas tras `setChannelPitch`:

```cpp
AudioImportSettings AudioManager::getSoundImportSettings(int id) const
{
#ifdef DT_FMOD_ENABLED
    if (id < 0 || id >= static_cast<int>(m_soundImport.size()) || !m_sounds[id])
        return {};
    return m_soundImport[id];
#else
    (void)id;
    return {};
#endif
}

void AudioManager::refreshImportSettings(const std::string& path)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system) return;
    for (size_t i = 0; i < m_sounds.size(); ++i)
    {
        if (!m_sounds[i] || !sameAssetPath(m_soundPaths[i], path)) continue;
        std::string warning;
        m_soundImport[i] = loadAudioImportSettings(m_soundPaths[i], &warning);
        if (!warning.empty())
            std::fprintf(stderr, "[AudioImport] %s: %s\n", m_soundPaths[i].c_str(), warning.c_str());
        // La voz viva se reajusta al momento con el volumen del COMPONENTE, no con
        // el del canal. El mono aplica desde la siguiente reproduccion.
        if (FMOD::Channel* ch = liveChannel(m_sfxChannels[i], m_sounds[i]))
            ch->setVolume(voiceVolume(static_cast<int>(i), m_soundVolume[i]));
    }
#else
    (void)path;
#endif
}

float AudioManager::getChannelVolume(int id) const
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return -1.0f;
    FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]);
    float v = -1.0f;
    if (!ch || ch->getVolume(&v) != FMOD_OK) return -1.0f;
    return v;
#else
    (void)id;
    return -1.0f;
#endif
}
```
   Los comentarios de los tres métodos en el `.h`: `getSoundImportSettings` = "ajustes del sidecar del sonido; defecto si el id no existe"; `refreshImportSettings` = "relee el sidecar de `path` y lo aplica a TODOS los sonidos vivos de esa ruta (2D y 3D, loop o no) y reajusta el volumen de su voz viva; los de otras rutas no se tocan"; `getChannelVolume` = "volumen REAL de la voz viva de soundId (la que siguen setChannelVolume y el seguimiento 3D), -1 si no hay voz. Existe para que la ganancia sea observable desde un test".

- [ ] **Step 5: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 5; .\build-ninja\engine\tests\dt_audio_tests.exe | Select-Object -Last 6; "exit $LASTEXITCODE"`
Expected: `ALL AUDIO TESTS PASSED`, exit 0, sin `FAIL` ni `SKIP` de los tres tests nuevos. Sabotaje de uno en uno (parchear → compilar → ejecutar → revertir): (a) en `refreshImportSettings` cambiar `m_soundVolume[i]` por `1.0f` → falla el check de `0.5f * 1.995262f`; (b) quitar `m_soundImport[id] = AudioImportSettings{}` de `unloadSound` **y** la asignación del slot reciclado → falla `isDefault(getSoundImportSettings(idB))`; (c) volver a poner `ch->setVolume(volume)` en `playSoundOneShot` → falla el test de política.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Audio/AudioManager.h engine/src/Audio/AudioManager.cpp engine/tests/audio_tests.cpp
git commit -m "feat(audio): ganancia de importacion por sonido en AudioManager y refresco en caliente"
```

---

### Task 3: Forzar a mono en la voz

**Files:**
- Modify: `engine/include/DonTopo/Audio/AudioManager.h` (público: `isVoiceForcedMono`)
- Modify: `engine/src/Audio/AudioManager.cpp` (helper estático `applyForceMono`, `playSound`, `playSoundOneShot`, getter)
- Test: `engine/tests/audio_tests.cpp`

**Interfaces:**
- Consumes (Task 2): `m_soundImport`, `liveChannel`, `writeWav`, `waitReady`, `audioTestDir`, `setGain` (helpers del propio `audio_tests.cpp`).
- Produces: `bool AudioManager::isVoiceForcedMono(int soundId) const` — true si la matriz de mezcla de la voz viva de ese id es la de mono (todas las entradas a 1/N en las dos salidas frontales). Fuera del `#ifdef`, `false` sin FMOD.

- [ ] **Step 1: Write the failing tests** — en `audio_tests.cpp` (llamar desde `main()`):

```cpp
// Sin FMOD el getter es neutro.
static void test_mono_getter_is_neutral_without_a_voice(AudioManager& am)
{
    CHECK(!am.isVoiceForcedMono(-1));
    CHECK(!am.isVoiceForcedMono(123456));
}

static void test_import_force_mono_on_a_2d_stereo_voice(AudioManager& am)
{
    if (!am.available()) { std::printf("SKIP test_import_force_mono_on_a_2d_stereo_voice (FMOD no disponible)\n"); return; }
    const auto d = audioTestDir("dt_audio_import_mono");
    const std::string stereoMono = (d / "m.wav").string();   // estereo CON forceMono
    const std::string stereoPlain = (d / "p.wav").string();  // estereo sin sidecar
    const std::string oneCh = (d / "one.wav").string();      // un canal CON forceMono
    const std::string stereo3d = (d / "s3d.wav").string();   // estereo CON forceMono, cargado 3D
    writeWav(stereoMono, 2, 3.0);
    writeWav(stereoPlain, 2, 3.0);
    writeWav(oneCh, 1, 3.0);
    writeWav(stereo3d, 2, 3.0);
    setGain(stereoMono, 0.0f, /*mono=*/true);
    setGain(oneCh, 0.0f, true);
    setGain(stereo3d, 0.0f, true);

    const int m  = am.loadSound(stereoMono, false, true);
    const int p  = am.loadSound(stereoPlain, false, true);
    const int o  = am.loadSound(oneCh, false, true);
    const int s3 = am.loadSound(stereo3d, true, true);
    if (m < 0 || p < 0 || o < 0 || s3 < 0) { CHECK(false); return; }
    if (!waitReady(am, m) || !waitReady(am, p) || !waitReady(am, o) || !waitReady(am, s3)) { CHECK(false); return; }
    CHECK(am.getSoundImportSettings(m).forceMono);

    am.playSound(m, {}, 1.0f);
    am.playSound(p, {}, 1.0f);
    am.playSound(o, {}, 1.0f);
    am.playSound(s3, {}, 1.0f);
    CHECK(am.isVoiceForcedMono(m));            // estereo 2D con el ajuste: mezclada a mono
    CHECK(!am.isVoiceForcedMono(p));           // sin ajuste: matriz de fabrica
    CHECK(!am.isVoiceForcedMono(o));           // Review Focus 6: un canal, no-op sin crash
    CHECK(!am.isVoiceForcedMono(s3));          // Review Focus 6: 3D, no se toca
    CHECK(am.isSoundPlaying(o));               // y sigue sonando

    // Otra reproduccion de la misma voz conserva el mono (se aplica en cada arranque).
    am.stopSound(m);
    am.playSound(m, {}, 1.0f);
    CHECK(am.isVoiceForcedMono(m));

    am.stopSound(m); am.stopSound(p); am.stopSound(o); am.stopSound(s3);
    am.unloadSound(m); am.unloadSound(p); am.unloadSound(o); am.unloadSound(s3);
}
```

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 3`
Expected: fallo de compilación (`isVoiceForcedMono` no existe). Añadir la declaración y una implementación ingenua (`return false;`), compilar y ejecutar `dt_audio_tests.exe`.
Expected: `FAIL: am.isVoiceForcedMono(m)` (dos veces).

- [ ] **Step 3: Implement**

`AudioManager.cpp`, dentro de `#ifdef DT_FMOD_ENABLED` junto a `liveChannel` (añadir `<algorithm>` y `<cmath>` si faltan):

```cpp
// Mezcla a mono la voz de un sonido marcado forceMono: cada una de las dos
// salidas frontales recibe la suma de TODAS las entradas a 1/N (el pico no pasa
// de 1 aunque las entradas vayan en fase); el resto de salidas (surround, LFE)
// queda en silencio. Solo sonidos 2D: en 3D el panner espacial de FMOD manda
// sobre la matriz y un emisor 3D con spread 0 ya suena como un punto. Un clip de
// un canal, o cualquier fallo al leer la matriz, deja la voz como esta.
static void applyForceMono(FMOD::Channel* ch, FMOD::Sound* snd)
{
    FMOD_MODE mode = 0;
    if (snd->getMode(&mode) != FMOD_OK || (mode & FMOD_3D)) return;
    int out = 0, in = 0;
    if (ch->getMixMatrix(nullptr, &out, &in) != FMOD_OK || in < 2 || out < 1) return;
    std::vector<float> m(static_cast<size_t>(out) * in, 0.0f);
    const float g = 1.0f / static_cast<float>(in);
    for (int row = 0; row < std::min(out, 2); ++row)
        for (int col = 0; col < in; ++col)
            m[static_cast<size_t>(row) * in + col] = g;
    ch->setMixMatrix(m.data(), out, in, in);
}
```

Llamarlo en `playSound` y en `playSoundOneShot`, **antes** de `ch->setPaused(false)` y tras fijar el volumen:

```cpp
    if (m_soundImport[id].forceMono) applyForceMono(ch, snd);
```

Getter (junto a `getChannelVolume`; fuera del `#ifdef` con la versión neutra):

```cpp
bool AudioManager::isVoiceForcedMono(int id) const
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return false;
    FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]);
    int out = 0, in = 0;
    if (!ch || ch->getMixMatrix(nullptr, &out, &in) != FMOD_OK || in < 2 || out < 1) return false;
    std::vector<float> m(static_cast<size_t>(out) * in, 0.0f);
    if (ch->getMixMatrix(m.data(), &out, &in, in) != FMOD_OK) return false;
    const float g = 1.0f / static_cast<float>(in);
    for (int row = 0; row < std::min(out, 2); ++row)
        for (int col = 0; col < in; ++col)
            if (std::fabs(m[static_cast<size_t>(row) * in + col] - g) > 1e-4f) return false;
    return true;
#else
    (void)id;
    return false;
#endif
}
```
Comentario del `.h`: "¿La voz viva de soundId lleva la matriz de mono (cada entrada a 1/N en las dos salidas frontales)? Existe para que forzar a mono sea observable desde un test: quitar la llamada a applyForceMono dejaba la feature entera sin efecto y la suite en verde."

**Si `getMixMatrix(nullptr, &out, &in)` no devuelve las dimensiones** (el test de un canal 2D estéreo sigue en rojo con `in == 0`): sustituir la consulta de dimensiones por `Sound::getFormat(nullptr, nullptr, &in, nullptr)` para `in` y `System::getSoftwareFormat(nullptr, &speakerMode, nullptr)` + `System::getSpeakerModeChannels(speakerMode, &out)` para `out`, en `applyForceMono` y en `isVoiceForcedMono` (la segunda necesita el sonido: `m_sounds[id]`), y ledgerar un `Ruling:`.

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 3; .\build-ninja\engine\tests\dt_audio_tests.exe | Select-Object -Last 5; "exit $LASTEXITCODE"`
Expected: `ALL AUDIO TESTS PASSED`, exit 0. Sabotaje: quitar la llamada `applyForceMono(ch, snd)` de `playSound` → falla `isVoiceForcedMono(m)`; restaurar. Sabotaje 2: quitar el `|| (mode & FMOD_3D)` → falla `!isVoiceForcedMono(s3)` **o** no falla (si FMOD ignora la matriz en 3D): en ese caso ledgerar `Ruling:` explicando que el guard 3D se mantiene por la spec aunque el test no lo distinga.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/Audio/AudioManager.h engine/src/Audio/AudioManager.cpp engine/tests/audio_tests.cpp
git commit -m "feat(audio): forzar a mono la voz de los clips 2D marcados en su sidecar"
```

---

### Task 4: El exportador copia el sidecar del clip

**Files:**
- Modify: `engine/src/Editor/GameExporter.cpp` (`addTexture` `:251`, usos `:276-278`, audio `:282-283`)
- Test: `engine/tests/exporter_tests.cpp`

**Interfaces:**
- Consumes (Task 1): `importSidecarPath`, `saveAudioImportSettings`.
- Produces: `collectSceneAssets` devuelve además un `ExportAsset` por cada `<clip>.import.json` que exista.

- [ ] **Step 1: Write the failing test** — en `exporter_tests.cpp` (llamarlo desde `main()` con `root`; incluir `"DonTopo/Audio/AudioClipComponent.h"` si no está — ya lo está):

```cpp
// El sidecar de un clip viaja con el clip, con la misma jerarquia (o
// assets/_external/N si el clip esta fuera del proyecto: Review Focus 7).
static void test_audio_sidecar_travels_with_the_clip(const fs::path& root)
{
    std::error_code ec;
    AudioImportSettings s;
    s.gainDb = -3.0f;
    std::string err;

    const fs::path inside = root / "assets" / "step.wav";
    CHECK(saveAudioImportSettings(inside, s, &err));
    const fs::path plain = root / "assets" / "chars" / "plain.wav";
    std::ofstream(plain) << "wav";                                   // sin sidecar

    // Un clip fuera del proyecto, con sidecar.
    const fs::path outside = fs::temp_directory_path(ec) / "dt_exporter_audio_outside" / "voz.wav";
    fs::create_directories(outside.parent_path(), ec);
    std::ofstream(outside) << "wav";
    CHECK(saveAudioImportSettings(outside, s, &err));

    Scene scene;
    auto* a = scene.addGameObject("con_sidecar");
    a->setAudioClip(std::make_shared<AudioClipComponent>(nullptr, inside.string(), -1, false, false));
    auto* b = scene.addGameObject("sin_sidecar");
    b->setAudioClip(std::make_shared<AudioClipComponent>(nullptr, plain.string(), -1, false, false));
    auto* c = scene.addGameObject("fuera");
    c->setAudioClip(std::make_shared<AudioClipComponent>(nullptr, outside.string(), -1, false, false));

    const std::vector<ExportAsset> assets = collectSceneAssets(scene, root, {});
    std::vector<std::string> pkg;
    for (const ExportAsset& x : assets) pkg.push_back(x.packagePath);
    auto has = [&](const std::string& p) { return std::find(pkg.begin(), pkg.end(), p) != pkg.end(); };

    CHECK(has("assets/step.wav"));
    CHECK(has("assets/step.wav.import.json"));
    CHECK(has("assets/chars/plain.wav"));
    CHECK(!has("assets/chars/plain.wav.import.json"));
    // El de fuera: el sidecar cae en la MISMA subcarpeta _external que su clip.
    std::string outClip, outSide;
    for (const std::string& p : pkg)
    {
        if (p.find("_external") == std::string::npos) continue;
        if (p.size() > 16 && p.substr(p.size() - 16) == ".wav.import.json") outSide = p;
        else if (p.size() > 4 && p.substr(p.size() - 4) == ".wav") outClip = p;
    }
    CHECK(!outClip.empty() && outSide == outClip + ".import.json");
    for (const ExportAsset& x : assets)
        if (x.packagePath.find(".import.json") != std::string::npos) CHECK(x.existsOnDisk);

    fs::remove(importSidecarPath(inside), ec);
    fs::remove(plain, ec);
    fs::remove_all(outside.parent_path(), ec);
}
```

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; .\build-ninja\engine\tests\dt_exporter_tests.exe | Select-Object -First 4; "exit $LASTEXITCODE"`
Expected: `FAIL: has("assets/step.wav.import.json")` y `outSide == outClip + ".import.json"`.

- [ ] **Step 3: Implement** — en `GameExporter.cpp`: renombrar la lambda `addTexture` a `addWithSidecar` (definición y los tres usos de materiales; actualizar su comentario: "un asset con ajustes de importación lleva su sidecar: el runtime lo busca junto al asset") y usarla para el audio:

```cpp
        if (go->hasAudioClip())
            addWithSidecar(go->getAudioClip()->getPath());
```

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; .\build-ninja\engine\tests\dt_exporter_tests.exe | Select-Object -Last 2; "exit $LASTEXITCODE"`
Expected: `OK`, exit 0 (incluye el test de texturas, que sigue pasando por el mismo helper).

- [ ] **Step 5: Commit**

```bash
git add engine/src/Editor/GameExporter.cpp engine/tests/exporter_tests.cpp
git commit -m "feat(export): el paquete incluye el sidecar de importacion de los clips de audio"
```

---

### Task 5: Modal "Import Settings…" para audios

**Files:**
- Modify: `engine/include/DonTopo/Editor/ContentBrowserPanel.h` (`AudioImportApplyResult`, `applyAudioImportSettings`, estado del modal `:266-270`)
- Modify: `engine/src/Editor/ContentBrowserPanel.cpp` (menú contextual `:1350-1359`, modal `:1586-1642`, `applyAudioImportSettings` junto a `applyTextureImportSettings`; incluir `"DonTopo/Audio/AudioManager.h"`)
- Test: `engine/tests/content_browser_tests.cpp`

**Interfaces:**
- Consumes (Tasks 1–2): `saveAudioImportSettings`, `loadAudioImportSettings`, `kAudioGainMinDb/MaxDb`, `AudioManager::refreshImportSettings`, `EditorContext::audio`.
- Produces:
  ```cpp
  struct AudioImportApplyResult { bool ok = false; std::string error; };
  AudioImportApplyResult applyAudioImportSettings(const std::filesystem::path& asset,
                                                  const AudioImportSettings& settings,
                                                  const std::function<void(const std::string&)>& refresh);
  ```

- [ ] **Step 1: Write the failing tests** — en `content_browser_tests.cpp` (llamarlos desde `main()`):

```cpp
static void test_apply_audio_writes_sidecar_and_refreshes_once()
{
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "dt_cb_apply_audio";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    const fs::path clip = base / "disparo.wav";
    std::ofstream(clip) << "wav";

    std::vector<std::string> refreshed;
    const auto refresh = [&](const std::string& p) { refreshed.push_back(p); };

    AudioImportSettings s;
    s.gainDb    = -4.0f;
    s.forceMono = true;
    AudioImportApplyResult r = applyAudioImportSettings(clip, s, refresh);
    CHECK(r.ok);
    CHECK(r.error.empty());
    CHECK(refreshed.size() == 1 && refreshed[0] == clip.string());
    CHECK(loadAudioImportSettings(clip) == s);

    // El defecto borra el sidecar y aun asi refresca (la voz vuelve a su volumen).
    r = applyAudioImportSettings(clip, AudioImportSettings{}, refresh);
    CHECK(r.ok);
    CHECK(refreshed.size() == 2);
    CHECK(!fs::exists(importSidecarPath(clip)));
}

// Review Focus 8: si el sidecar no se puede escribir, no se refresca nada.
static void test_apply_audio_write_failure_does_not_refresh()
{
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "dt_cb_apply_audio_fail";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    int calls = 0;
    AudioImportSettings s;
    s.gainDb = 3.0f;
    const AudioImportApplyResult r = applyAudioImportSettings(
        base / "no_existe_carpeta" / "x.wav", s, [&](const std::string&) { ++calls; });
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    CHECK(calls == 0);
}

static void test_apply_audio_without_audio_manager_still_writes()
{
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "dt_cb_apply_audio_norefresh";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    const fs::path clip = base / "a.wav";
    std::ofstream(clip) << "wav";
    AudioImportSettings s;
    s.forceMono = true;
    const AudioImportApplyResult r = applyAudioImportSettings(clip, s, {});
    CHECK(r.ok);
    CHECK(loadAudioImportSettings(clip) == s);
}

// Review Focus 8: que assets ofrecen "Import Settings...".
static void test_import_settings_menu_kind()
{
    CHECK(importSettingsKindFor(".png", false)  == ImportSettingsKind::Texture);
    CHECK(importSettingsKindFor(".WAV", false)  == ImportSettingsKind::Audio);
    CHECK(importSettingsKindFor(".mp3", false)  == ImportSettingsKind::Audio);
    CHECK(importSettingsKindFor(".ogg", false)  == ImportSettingsKind::Audio);
    CHECK(importSettingsKindFor(".flac", false) == ImportSettingsKind::Audio);
    CHECK(importSettingsKindFor(".fbx", false)  == ImportSettingsKind::None);   // modelos: siguiente spec
    CHECK(importSettingsKindFor(".lua", false)  == ImportSettingsKind::None);
    CHECK(importSettingsKindFor(".wav", true)   == ImportSettingsKind::None);   // una carpeta
}
```
Añadir al test el `#include "DonTopo/Audio/AudioManager.h"` solo si hace falta (no lo hace: el refresh es un `std::function`).

- [ ] **Step 2: Run to verify RED**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 4`
Expected: fallo de compilación (`applyAudioImportSettings`, `AudioImportApplyResult`, `ImportSettingsKind`, `importSettingsKindFor` no existen).

- [ ] **Step 3: Implement**

`ContentBrowserPanel.h`, junto a `TextureImportApplyResult`:

```cpp
// Que ajustes de importacion ofrece un asset. El menu contextual y el modal se
// deciden por esto, no por comprobaciones sueltas: un tipo nuevo (modelos) se
// anade aqui.
enum class ImportSettingsKind { None, Texture, Audio };
ImportSettingsKind importSettingsKindFor(const std::string& ext, bool isDir);

struct AudioImportApplyResult {
    bool        ok = false;
    std::string error;       // causa si !ok (el modal la muestra y no se cierra)
};

// Escribe los ajustes de importacion del clip (el defecto borra el sidecar) y, si
// se pudo, llama a `refresh` con la ruta para que las voces cargadas de ese
// fichero los reciban sin reiniciar. Si el sidecar no se puede escribir no se
// llama a `refresh`. `refresh` nula = solo se escribe (sin audio).
AudioImportApplyResult applyAudioImportSettings(const std::filesystem::path& asset,
                                                const AudioImportSettings& settings,
                                                const std::function<void(const std::string&)>& refresh);
```
Estado del modal (miembros junto a `m_importEdit`):

```cpp
    ImportSettingsKind     m_importKind = ImportSettingsKind::None;
    AudioImportSettings    m_importAudioEdit;
```

`ContentBrowserPanel.cpp` — funciones nuevas junto a `applyTextureImportSettings`:

```cpp
ImportSettingsKind importSettingsKindFor(const std::string& ext, bool isDir)
{
    if (isDir) return ImportSettingsKind::None;
    switch (classifyAsset(ext, false))
    {
        case AssetKind::Image: return ImportSettingsKind::Texture;
        case AssetKind::Audio: return ImportSettingsKind::Audio;
        default:               return ImportSettingsKind::None;
    }
}

AudioImportApplyResult applyAudioImportSettings(const std::filesystem::path& asset,
                                                const AudioImportSettings& settings,
                                                const std::function<void(const std::string&)>& refresh)
{
    AudioImportApplyResult r;
    if (!saveAudioImportSettings(asset, settings, &r.error))
        return r;                                    // nada refrescado si no se pudo escribir
    r.ok = true;
    if (refresh) refresh(asset.string());
    return r;
}
```

Menú contextual (sustituye el bloque de `:1350-1359`):

```cpp
                // Ajustes de importacion: solo de UN asset con ajustes (textura o audio).
                const ImportSettingsKind importKind = importSettingsKindFor(path.extension().string(), isDir);
                if (selCount <= 1 && importKind != ImportSettingsKind::None &&
                    ImGui::MenuItem("Import Settings..."))
                {
                    m_importTarget = path;
                    m_importKind   = importKind;
                    if (importKind == ImportSettingsKind::Audio)
                        m_importAudioEdit = loadAudioImportSettings(path);
                    else
                        m_importEdit = loadTextureImportSettings(path);
                    m_importError.clear();
                    m_openImportPopup = true;
                }
```

Modal: el cuerpo entre el nombre del fichero y el bloque de error pasa a bifurcar por `m_importKind`:

```cpp
            if (m_importKind == ImportSettingsKind::Audio)
            {
                // Sin aplicar nada hasta "Aplicar": edita una copia, asi que un
                // SliderFloat normal basta (no hay escritura por frame que diferir).
                ImGui::SliderFloat("Gain (dB)", &m_importAudioEdit.gainDb,
                                   kAudioGainMinDb, kAudioGainMaxDb, "%.1f dB");
                ImGui::Checkbox("Force mono", &m_importAudioEdit.forceMono);
                ImGui::TextDisabled("La ganancia se suma al volumen del componente.");
                ImGui::TextDisabled("Mono: solo clips 2D y desde la proxima reproduccion.");
            }
            else
            {
                int colorSpace = static_cast<int>(m_importEdit.colorSpace);
                if (ImGui::Combo("Color space", &colorSpace, "Auto (por slot)\0sRGB\0Linear\0"))
                    m_importEdit.colorSpace = static_cast<ColorSpaceOverride>(colorSpace);
                ImGui::Checkbox("Mipmaps", &m_importEdit.mipmaps);
                ImGui::TextDisabled("Auto: color base sRGB, normal y ORM lineal.");
            }
```
y el bloque `if (apply)` bifurca igual: el rama de texturas queda **como está**; la de audio:

```cpp
                if (m_importKind == ImportSettingsKind::Audio)
                {
                    const AudioImportApplyResult r = applyAudioImportSettings(
                        m_importTarget, m_importAudioEdit,
                        [&ctx](const std::string& p) { if (ctx.audio) ctx.audio->refreshImportSettings(p); });
                    if (r.ok)
                    {
                        ctx.pushLog("Import settings aplicados: " + m_importTarget.filename().string());
                        ImGui::CloseCurrentPopup();
                    }
                    else
                    {
                        m_importError = r.error;   // el modal NO se cierra
                    }
                }
                else
                {
                    /* el bloque actual de texturas, sin cambios */
                }
```
Actualizar el comentario del modal ("de UNA textura" → "de UN asset con ajustes"). Incluir `"DonTopo/Audio/AudioManager.h"` arriba del `.cpp`.

- [ ] **Step 4: Run to verify GREEN**

Run: `.\build.bat > $env:TEMP\b.log 2>&1; Select-String $env:TEMP\b.log -Pattern 'error C' | Select-Object -First 4; .\build-ninja\engine\tests\dt_content_browser_tests.exe | Select-Object -Last 3; "exit $LASTEXITCODE"`
Expected: `ALL CONTENT BROWSER TESTS PASSED`, exit 0. Sabotaje: hacer que `applyAudioImportSettings` llame a `refresh` aunque `save` falle → falla `calls == 0`; restaurar. Luego la suite completa en segundo plano (snippet) y `Sandbox.exe` vivo 7 s (sin tocar `project.json`).

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/Editor/ContentBrowserPanel.h engine/src/Editor/ContentBrowserPanel.cpp engine/tests/content_browser_tests.cpp
git commit -m "feat(editor): modal Import Settings para clips de audio (ganancia y mono)"
```

---

### Task 6: Documentación y cierre del audit

**Files:**
- Modify: `docs/assets-editor-audit.md` (fila U8 de la tabla "Estado vigente" y línea de la capacidad 6)
- Modify: `README.md` (el párrafo del Content Browser donde ya se describe "Import Settings...")
- Modify: memoria del proyecto `pending_import_settings_models_audio.md` (fuera del repo)

- [ ] **Step 1:** Con `Edit` (nunca reescribir el fichero entero; `git diff --stat` debe ser pequeño):
  - Audit, fila U8: pasa de "CERRADO para texturas de material" a "**CERRADO para texturas de material y clips de audio**", añadiendo "audio: ganancia (dB) y forzar a mono, sidecar `type: audio`; **modelos siguen abiertos**". Línea de la capacidad 6: "EXISTE para texturas de material y audio (modelos, sin hacer)".
  - README: en el párrafo de "Import Settings..." añadir una frase: "For audio clips (`.wav/.mp3/.ogg/.flac`) it offers a gain in dB (-30 to +12) added to every voice of that file and a Force mono switch for 2D clips; both are also saved in `<name>.import.json`."
- [ ] **Step 2:** Actualizar la nota de memoria `pending_import_settings_models_audio.md`: audio hecho (fecha y commit), quedan **modelos** (escala, normales, importar animaciones) como última parte de U8; y renombrar la línea del índice en `MEMORY.md` si su gancho ya no describe el estado.
- [ ] **Step 3: Suite completa** en segundo plano con el snippet: `SUMMARY total 33, fallos:` vacío.
- [ ] **Step 4: Commit**

```bash
git add docs/assets-editor-audit.md README.md
git commit -m "docs: cerrar U8 para audio y documentar la ganancia y el mono de importacion"
```

---

## Self-Review

**Spec coverage.**
- §1 Modelo y sidecar (tipo `audio`, tolerancia, clamp a [-30, +12], NaN→0, cruce de tipos, escritura tmp+rename, defecto borra) → Task 1.
- §2 Consumo: datos por sonido (`m_soundImport`), lectura solo al crear el sonido, `voiceVolume` único, tres sitios, política por grep, refresco selectivo con `m_soundVolume`, slot reciclado → Task 2. Mono 2D con matriz 1/N a las dos salidas frontales, un canal no-op → Task 3.
- §3 UI (menú para un solo audio, modal por tipo, Aplicar con refresco, error sin cerrar, sin `ctx.audio` solo escribe) → Task 5. La spec dice "`DeferredSliderFloat` si aplica": el plan usa un `SliderFloat` normal porque el modal edita una copia y no escribe por frame; queda dicho en el código.
- §4 Ciclo de vida: mover/renombrar/borrar/importar/listado ya genéricos (verificado en la implementación de texturas); exportador → Task 4.
- Verificación: tests de sidecar, `AudioManager` con FMOD, política, exportador, `applyAudioImportSettings`; el oído queda para el usuario (Entrega).

**Placeholder scan.** Los tests están completos; el único paso con dos ramas es el fallback de las dimensiones de la matriz en la Task 3, con el código alternativo descrito y un `Ruling:` obligado si se usa.

**Type consistency.** `AudioImportSettings`/`clampAudioGainDb`/`audioGainLinear`/`loadAudioImportSettings`/`saveAudioImportSettings`/`sameAssetPath` (Task 1) se usan con esos nombres en 2–5. `getSoundImportSettings`/`refreshImportSettings`/`getChannelVolume` (Task 2) se usan en los tests de las Tasks 2–3; `isVoiceForcedMono` (Task 3). `applyAudioImportSettings`/`AudioImportApplyResult`/`ImportSettingsKind`/`importSettingsKindFor` se definen y se consumen dentro de la Task 5.

**Review Focus.** 1→Task 1, 2→Task 2, 3→Task 2, 4→Task 2, 5→Task 2, 6→Task 3, 7→Task 4, 8→Task 5.

## Entrega (para quien ejecute)

Al terminar la Task 6, pedir al usuario la verificación **a oído** (spec, "Verificación"), en el editor y en el juego exportado: un clip a -12 dB suena más bajo que sin ajuste y a +6 dB más alto; Force mono en un clip estéreo 2D con paneo deja de tener paneo entre altavoces; Aplicar con el clip sonando reajusta el volumen sin cortarlo; mover y renombrar conservan los ajustes; el exportado los respeta; cierre limpio. Ningún agente puede comprobar el sonido: dilo así. Si la ganancia +12 dB satura en un clip fuerte, es el riesgo anotado en la spec (FMOD no limita).
