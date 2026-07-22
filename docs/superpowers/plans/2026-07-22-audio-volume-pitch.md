# Volumen y pitch por clip — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Que cada `AudioClipComponent` tenga su propio volumen y pitch —serializados, editables con undo y accesibles desde Lua— y bindear los `SetIs3D`/`GetIs3D` que ya existían en C++.

**Architecture:** `volume` y `pitch` son campos de `AudioClipComponent` con clamp en el setter. El componente no habla con FMOD: delega en `AudioManager`, que gana `setChannelVolume`/`setChannelPitch` sobre `m_sfxChannels[soundId]`. `playSound` pasa a arrancar el canal pausado, aplicar volumen/pitch/posición y despausar.

**Tech Stack:** C++20, FMOD (tras `DT_FMOD_ENABLED`), nlohmann::json, Dear ImGui, sol2/Lua.

## Global Constraints

- Rangos exactos: `volume` en `[0.0f, 1.0f]`, `pitch` en `[0.5f, 2.0f]`. Defaults `1.0f` y `1.0f`.
- El clamp vive en el setter de `AudioClipComponent`, nunca en la UI ni en el binding: es el core quien garantiza el rango. Un valor fuera se recorta en silencio, no es un error.
- `AudioClipComponent` NO incluye cabeceras de FMOD. Todo acceso a FMOD pasa por `AudioManager` (que guarda los punteros como `void*`).
- Todo el código FMOD nuevo va dentro de `#ifdef DT_FMOD_ENABLED`, como el que ya hay en `AudioManager.cpp`.
- Cambiar `volume`/`pitch` NO llama a `reload()`: son propiedades del canal, no del `FMOD_MODE`.
- Build: `.\build.bat` desde la raíz vía PowerShell. Los tests se ejecutan desde la raíz del repo (usan rutas relativas a `assets/`).
- Comentarios y mensajes de log en español, como el resto del repo.

---

### Task 1: volume y pitch en AudioClipComponent

**Files:**
- Modify: `engine/include/DonTopo/Audio/AudioClipComponent.h`
- Modify: `engine/src/Audio/AudioClipComponent.cpp`
- Create: `engine/tests/audio_tests.cpp`
- Modify: `engine/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nada de tareas anteriores.
- Produces: `void AudioClipComponent::setVolume(float)`, `void setPitch(float)`, `float getVolume() const`, `float getPitch() const`. Las tareas 2-5 dependen de estos cuatro nombres exactos.

- [ ] **Step 1: Crear el ejecutable de tests**

`engine/tests/audio_tests.cpp` (fichero nuevo, completo):

```cpp
// Tests headless de AudioClipComponent: rangos de volume/pitch y su
// serialización. Plain main + asserts, sin framework — coherente con
// camera_tests.cpp y physics_tests.cpp.
//
// El componente se construye a pelo con m_audio = nullptr y soundId = -1:
// así los setters ejercitan el clamp sin necesitar FMOD ni dispositivo de
// audio. Mismo truco que usa exporter_tests.cpp.
#include "DonTopo/Audio/AudioClipComponent.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool nearlyEqual(float a, float b, float eps = 0.0001f) { return std::fabs(a - b) < eps; }

static std::shared_ptr<AudioClipComponent> makeClip()
{
    return std::make_shared<AudioClipComponent>(nullptr, "assets/audio.mp3", -1, false, false);
}

// Un clip recién creado suena tal cual está grabado: sin atenuar y sin
// alterar el tono. Si estos defaults cambiaran, toda escena guardada antes
// de esta feature sonaría distinta al recargarla.
static void test_defaults_are_neutral()
{
    auto clip = makeClip();
    CHECK(nearlyEqual(clip->getVolume(), 1.0f));
    CHECK(nearlyEqual(clip->getPitch(), 1.0f));
}

static void test_volume_clamps_to_range()
{
    auto clip = makeClip();

    clip->setVolume(0.5f);
    CHECK(nearlyEqual(clip->getVolume(), 0.5f));

    clip->setVolume(-1.0f);
    CHECK(nearlyEqual(clip->getVolume(), 0.0f));

    clip->setVolume(5.0f);
    CHECK(nearlyEqual(clip->getVolume(), 1.0f));
}

// El mínimo NO es 0: un pitch de 0 pararía el sonido en seco en vez de
// bajarlo, y FMOD no lo admite como "silencio".
static void test_pitch_clamps_to_range()
{
    auto clip = makeClip();

    clip->setPitch(1.5f);
    CHECK(nearlyEqual(clip->getPitch(), 1.5f));

    clip->setPitch(0.1f);
    CHECK(nearlyEqual(clip->getPitch(), 0.5f));

    clip->setPitch(10.0f);
    CHECK(nearlyEqual(clip->getPitch(), 2.0f));
}

// Sin AudioManager no hay canal al que empujar el valor. El setter tiene que
// guardarlo igual y no tocar un puntero nulo.
static void test_setters_survive_without_manager()
{
    auto clip = makeClip();
    clip->setVolume(0.25f);
    clip->setPitch(1.75f);
    CHECK(nearlyEqual(clip->getVolume(), 0.25f));
    CHECK(nearlyEqual(clip->getPitch(), 1.75f));
}

int main()
{
    test_defaults_are_neutral();
    test_volume_clamps_to_range();
    test_pitch_clamps_to_range();
    test_setters_survive_without_manager();

    if (g_failures == 0) std::printf("ALL AUDIO TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Registrar el ejecutable en CMake**

En `engine/tests/CMakeLists.txt`, tras el bloque de `dt_exporter_tests` (líneas 18-20):

```cmake
add_executable(dt_audio_tests audio_tests.cpp)
target_link_libraries(dt_audio_tests PRIVATE DonTopoEngine)
target_compile_features(dt_audio_tests PRIVATE cxx_std_20)
```

Y añadir `dt_audio_tests` a la lista del `foreach` que copia `fmod.dll` (línea 29), que queda así:

```cmake
        foreach(_dt_test_target dt_physics_tests dt_content_browser_tests dt_camera_tests dt_animator_tests dt_exporter_tests dt_audio_tests)
```

Sin eso, el `.exe` arranca con `STATUS_ENTRYPOINT_NOT_FOUND` en un clone limpio — el comentario de ese bloque lo explica.

- [ ] **Step 3: Ejecutar para verificar que falla**

Run: `.\build.bat`
Expected: FALLO de compilación, `error C2039: 'setVolume': no es un miembro de 'DonTopo::AudioClipComponent'` (y lo mismo para `setPitch`, `getVolume`, `getPitch`).

- [ ] **Step 4: Añadir los campos y accesores al header**

En `engine/include/DonTopo/Audio/AudioClipComponent.h`, tras `void setIs3D(bool is3D);`:

```cpp
    // Volumen y pitch son propiedades del CANAL, no del FMOD_MODE del sonido:
    // a diferencia de setLoop/setIs3D, no recargan nada y se pueden mover
    // mientras suena. El clamp vive aquí para que ni la UI ni Lua puedan
    // colar un valor fuera de rango.
    void setVolume(float volume);   // [0, 1]
    void setPitch (float pitch);    // [0.5, 2]
```

Junto a los otros getters:

```cpp
    float getVolume() const { return m_volume; }
    float getPitch()  const { return m_pitch;  }
```

Y en la zona de miembros, tras `bool m_playOnAwake = false;`:

```cpp
    float m_volume = 1.0f;
    float m_pitch  = 1.0f;
```

- [ ] **Step 5: Implementar los setters**

En `engine/src/Audio/AudioClipComponent.cpp`, tras `setIs3D`:

```cpp
void AudioClipComponent::setVolume(float volume)
{
    m_volume = std::clamp(volume, 0.0f, 1.0f);
}

void AudioClipComponent::setPitch(float pitch)
{
    m_pitch = std::clamp(pitch, 0.5f, 2.0f);
}
```

Y añadir `#include <algorithm>` arriba, junto a los includes existentes.

(La aplicación al canal vivo llega en la Task 2: aquí solo se guarda el valor, que es lo que los tests de esta tarea comprueban.)

- [ ] **Step 6: Ejecutar los tests**

Run: `.\build.bat` y después `.\build-ninja\engine\tests\dt_audio_tests.exe`
Expected: `ALL AUDIO TESTS PASSED`, exit code 0.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Audio/AudioClipComponent.h engine/src/Audio/AudioClipComponent.cpp engine/tests/audio_tests.cpp engine/tests/CMakeLists.txt
git commit -m "feat(audio): volume y pitch por clip con clamp en el setter"
```

---

### Task 2: Aplicar volumen y pitch al canal de FMOD

**Files:**
- Modify: `engine/include/DonTopo/Audio/AudioManager.h`
- Modify: `engine/src/Audio/AudioManager.cpp`
- Modify: `engine/src/Audio/AudioClipComponent.cpp`

**Interfaces:**
- Consumes: `AudioClipComponent::getVolume()`, `getPitch()` (Task 1).
- Produces: `void AudioManager::setChannelVolume(int soundId, float v)`, `void AudioManager::setChannelPitch(int soundId, float p)`, y la nueva firma `void AudioManager::playSound(int id, const glm::vec3& worldPos = {}, float volume = 1.0f, float pitch = 1.0f)`.

**Nota sobre tests:** esta tarea no lleva test automático. Lo que hace solo se puede observar con FMOD inicializado y un dispositivo de audio, y ningún test del repo reproduce sonido. Se verifica en los criterios manuales 4 y 5 de la spec. Los tests de la Task 1 deben seguir en verde.

- [ ] **Step 1: Declarar los dos setters de canal**

En `engine/include/DonTopo/Audio/AudioManager.h`, tras `void stopSound(int soundId);`:

```cpp
    // Empujan el valor al canal de la última reproducción de soundId, si
    // sigue siendo suyo. FMOD recicla los Channel*: un canal que ya terminó
    // puede haber sido reasignado a otro sonido, y escribirle el volumen se
    // lo cambiaría a un sonido ajeno. Por eso se comprueba isPlaying() y que
    // getCurrentSound() sea el sonido de ese id.
    //
    // No pasa nada si no hay canal: el valor vive en AudioClipComponent y se
    // aplicará en el siguiente playSound.
    void setChannelVolume(int soundId, float volume);
    void setChannelPitch (int soundId, float pitch);
```

Y cambiar la declaración de `playSound` (línea 28) por:

```cpp
    void playSound(int soundId, const glm::vec3& worldPos = {},
                   float volume = 1.0f, float pitch = 1.0f);
```

- [ ] **Step 2: Implementar el helper de canal vivo y los dos setters**

En `engine/src/Audio/AudioManager.cpp`, justo antes de `playSound`:

```cpp
#ifdef DT_FMOD_ENABLED
// El Channel* guardado para id, sólo si sigue sonando y sigue siendo el canal
// de ESE sonido. Devuelve nullptr en cualquier otro caso.
static FMOD::Channel* liveChannel(void* raw, void* expectedSound)
{
    auto* ch = reinterpret_cast<FMOD::Channel*>(raw);
    if (!ch) return nullptr;

    bool playing = false;
    if (ch->isPlaying(&playing) != FMOD_OK || !playing) return nullptr;

    FMOD::Sound* current = nullptr;
    if (ch->getCurrentSound(&current) != FMOD_OK) return nullptr;
    if (current != reinterpret_cast<FMOD::Sound*>(expectedSound)) return nullptr;

    return ch;
}
#endif

void AudioManager::setChannelVolume(int id, float volume)
{
#ifdef DT_FMOD_ENABLED
    if (id < 0 || id >= (int)m_sfxChannels.size() || !m_sounds[id]) return;
    if (FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]))
        ch->setVolume(volume);
#else
    (void)id; (void)volume;
#endif
}

void AudioManager::setChannelPitch(int id, float pitch)
{
#ifdef DT_FMOD_ENABLED
    if (id < 0 || id >= (int)m_sfxChannels.size() || !m_sounds[id]) return;
    if (FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]))
        ch->setPitch(pitch);
#else
    (void)id; (void)pitch;
#endif
}
```

- [ ] **Step 3: Arrancar el canal pausado en playSound**

Sustituir el cuerpo de `AudioManager::playSound` (líneas 110-125) por:

```cpp
void AudioManager::playSound(int id, const glm::vec3& worldPos, float volume, float pitch)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() || !m_sounds[id]) return;
    FMOD::Channel* ch;
    auto* snd = reinterpret_cast<FMOD::Sound*>(m_sounds[id]);
    // paused = true: hay que dejar volumen, pitch y posición puestos ANTES de
    // que suene la primera muestra. Arrancándolo sonando, un clip 3D se oye un
    // instante desde el origen del mundo y con el volumen del canal anterior.
    if (SYS->playSound(snd, SFXG, true, &ch) != FMOD_OK) return;
    m_sfxChannels[id] = ch;

    ch->setVolume(volume);
    ch->setPitch(pitch);

    FMOD_MODE mode; snd->getMode(&mode);
    if (mode & FMOD_3D) {
        FMOD_VECTOR p = { worldPos.x, worldPos.y, worldPos.z };
        FMOD_VECTOR v = { 0, 0, 0 };
        ch->set3DAttributes(&p, &v);
    }

    ch->setPaused(false);
#else
    (void)id; (void)worldPos; (void)volume; (void)pitch;
#endif
}
```

- [ ] **Step 4: Que el componente pase y empuje sus valores**

En `engine/src/Audio/AudioClipComponent.cpp`, cambiar `play` para que pase los suyos:

```cpp
void AudioClipComponent::play(const glm::vec3& worldPos)
{
    if (m_audio) m_audio->playSound(m_soundId, worldPos, m_volume, m_pitch);
}
```

Y que los setters empujen al canal vivo:

```cpp
void AudioClipComponent::setVolume(float volume)
{
    m_volume = std::clamp(volume, 0.0f, 1.0f);
    if (m_audio) m_audio->setChannelVolume(m_soundId, m_volume);
}

void AudioClipComponent::setPitch(float pitch)
{
    m_pitch = std::clamp(pitch, 0.5f, 2.0f);
    if (m_audio) m_audio->setChannelPitch(m_soundId, m_pitch);
}
```

- [ ] **Step 5: Compilar y comprobar que no hay regresión**

Run: `.\build.bat` y después `.\build-ninja\engine\tests\dt_audio_tests.exe`
Expected: build sin errores ni warnings nuevos, `ALL AUDIO TESTS PASSED`.

`AudioClipComponent::play` es el ÚNICO llamante de `playSound` en todo el repo (los demás sitios llaman a `clip->play(pos)`, cuya firma no cambia), así que no hay más call sites que tocar. Confirmarlo con:

Run: `grep -rn "playSound" engine/src engine/include sandbox runtime`
Expected: solo `AudioManager.h`, `AudioManager.cpp` y la línea de `AudioClipComponent.cpp`.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Audio/AudioManager.h engine/src/Audio/AudioManager.cpp engine/src/Audio/AudioClipComponent.cpp
git commit -m "feat(audio): aplicar volumen y pitch al canal, arrancandolo pausado"
```

---

### Task 3: Serializar volume y pitch

**Files:**
- Modify: `engine/src/Core/Scene.cpp:331-334` (escritura), `engine/src/Core/Scene.cpp:697-711` (lectura)
- Modify: `engine/tests/audio_tests.cpp`

**Interfaces:**
- Consumes: `getVolume()`/`getPitch()`/`setVolume()`/`setPitch()` (Task 1).
- Produces: las claves `"volume"` y `"pitch"` dentro del objeto `"audioClip"` del JSON de escena.

- [ ] **Step 1: Escribir los tests que fallan**

En `engine/tests/audio_tests.cpp`, añadir los includes que faltan arriba:

```cpp
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include <nlohmann/json.hpp>
```

Y estos dos tests antes de `main`:

```cpp
// El JSON tiene que llevar los dos campos: sin ellos, mover un slider y
// guardar la escena no dejaría rastro.
static void test_tojson_emits_volume_and_pitch()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    auto clip = makeClip();
    clip->setVolume(0.25f);
    clip->setPitch(1.5f);
    go->setAudioClip(clip);

    nlohmann::json j = scene.toJson();
    const nlohmann::json& node = j["root"]["children"][0];
    CHECK(node.contains("audioClip"));
    if (!node.contains("audioClip")) return;
    CHECK(nearlyEqual(node["audioClip"].value("volume", -1.0f), 0.25f));
    CHECK(nearlyEqual(node["audioClip"].value("pitch",  -1.0f), 1.5f));
}

// Back-compat: una escena guardada antes de esta feature no trae los campos y
// tiene que cargar con los valores neutros. Es lo que se rompe si alguien
// cambia el .value() de la carga por un .at().
//
// Necesita FMOD vivo: Scene::fromJson crea el clip con
// AudioManager::createAudioClipComponent, que sin sonido cargado devuelve
// nullptr. En una máquina sin dispositivo de audio el test se salta a sí
// mismo en vez de dar un falso rojo.
static void test_scene_without_volume_loads_neutral(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", false, false);
    if (!probe)
    {
        std::printf("SKIP test_scene_without_volume_loads_neutral (FMOD no disponible)\n");
        return;
    }

    nlohmann::json j = {
        { "name", "Test" },
        { "root", {
            { "name", "Root" }, { "id", 1 },
            { "position", { 0, 0, 0 } }, { "rotation", { 0, 0, 0 } }, { "scale", { 1, 1, 1 } },
            { "children", nlohmann::json::array({
                {
                    { "name", "altavoz" }, { "id", 2 },
                    { "position", { 0, 0, 0 } }, { "rotation", { 0, 0, 0 } }, { "scale", { 1, 1, 1 } },
                    { "children", nlohmann::json::array() },
                    { "audioClip", {
                        { "path", "assets/audio.mp3" },
                        { "loop", false }, { "is3D", false }, { "playOnAwake", false }
                    }}
                }
            })}
        }}
    };

    Scene loaded("Vacia");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* go = loaded.findById(2);
    CHECK(go != nullptr);
    if (!go || !go->hasAudioClip()) { CHECK(false); return; }
    CHECK(nearlyEqual(go->getAudioClip()->getVolume(), 1.0f));
    CHECK(nearlyEqual(go->getAudioClip()->getPitch(),  1.0f));
}
```

Y cambiar `main` para que cree los managers (patrón de `camera_tests.cpp`: una sola `PxFoundation` por proceso) y llame a los dos tests nuevos:

```cpp
int main()
{
    PhysicsManager pm;
    pm.init();
    AudioManager am;
    am.init();

    test_defaults_are_neutral();
    test_volume_clamps_to_range();
    test_pitch_clamps_to_range();
    test_setters_survive_without_manager();
    test_tojson_emits_volume_and_pitch();
    test_scene_without_volume_loads_neutral(pm, am);

    am.shutdown();
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL AUDIO TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Ejecutar para verificar que falla**

Run: `.\build.bat` y después `.\build-ninja\engine\tests\dt_audio_tests.exe`
Expected: FAIL en `nearlyEqual(node["audioClip"].value("volume", -1.0f), 0.25f)` — el campo no existe todavía, así que el `.value()` devuelve el `-1.0f`.

- [ ] **Step 3: Escribir los campos en toJson**

En `engine/src/Core/Scene.cpp`, sustituir el bloque de `audioClip` (líneas 331-334) por:

```cpp
            j["audioClip"] = { {"path", clip->getPath()},
                                {"loop", clip->getLoop()},
                                {"is3D", clip->getIs3D()},
                                {"playOnAwake", clip->getPlayOnAwake()},
                                {"volume", clip->getVolume()},
                                {"pitch", clip->getPitch()} };
```

- [ ] **Step 4: Leerlos en fromJson**

En el bloque `if (j.contains("audioClip"))`, junto a la línea de `setPlayOnAwake`:

```cpp
                // .value() con default: compat con escenas guardadas antes de
                // que existieran estos campos. Con .at() reventaría toda
                // escena anterior a la feature.
                clip->setPlayOnAwake(c.value("playOnAwake", false));
                clip->setVolume(c.value("volume", 1.0f));
                clip->setPitch(c.value("pitch", 1.0f));
```

- [ ] **Step 5: Ejecutar los tests**

Run: `.\build-ninja\engine\tests\dt_audio_tests.exe`
Expected: `ALL AUDIO TESTS PASSED`.

- [ ] **Step 6: Commit**

```bash
git add engine/src/Core/Scene.cpp engine/tests/audio_tests.cpp
git commit -m "feat(audio): serializar volume y pitch con back-compat"
```

---

### Task 4: Sliders con undo en Properties

**Files:**
- Modify: `engine/include/DonTopo/Editor/Command.h`
- Modify: `engine/src/Editor/PropertiesPanel.cpp:1337-1347`
- Modify: `engine/include/DonTopo/Editor/PropertiesPanel.h`

**Interfaces:**
- Consumes: `setVolume`/`getVolume`/`setPitch`/`getPitch` (Task 1), `PropertyCommand<T>` (ya existe).
- Produces: `struct AudioClipState { float volume; float pitch; };` en `Command.h`.

**Nota sobre tests:** sin test automático. `PropertiesPanel` dibuja ImGui y no hay arnés headless para él en este repo; el resto de sus secciones tampoco lo tiene. Se verifica en el criterio manual 4.

- [ ] **Step 1: Añadir el struct de estado**

En `engine/include/DonTopo/Editor/Command.h`, junto a los otros snapshots (tras `RigidbodyState`):

```cpp
// Snapshot value-type del AudioClipComponent — T de PropertyCommand<T> en la
// sección Audio Clip del panel Properties. Sólo volumen y pitch: loop, is3D y
// playOnAwake se escriben directos y no tienen undo.
struct AudioClipState {
    float volume;
    float pitch;
};
```

- [ ] **Step 2: Añadir el estado de edición al panel**

En `engine/include/DonTopo/Editor/PropertiesPanel.h`, junto a los otros miembros `m_edit*`:

```cpp
    // Snapshot al empezar el drag de los sliders de audio: un drag continuo no
    // puede empujar un comando por frame, así que se captura al activar y se
    // empuja uno solo al soltar. Mismo patrón que Transform y Rigidbody.
    bool  m_audioDragActive = false;
    float m_audioDragBeforeVolume = 1.0f;
    float m_audioDragBeforePitch  = 1.0f;
```

- [ ] **Step 3: Dibujar los sliders con undo**

En `engine/src/Editor/PropertiesPanel.cpp`, tras el checkbox de `Play On Awake` (línea 1347) y antes del `ImGui::TreePop()`:

```cpp
            // --- Volume / Pitch: snapshot al activar cualquiera de los dos,
            // un solo comando al soltar. Los valores se escriben en vivo
            // mientras se arrastra (así se oye el cambio), y el comando sólo
            // sirve para que Ctrl+Z devuelva el drag entero de una vez.
            float volume = clip->getVolume();
            float pitch  = clip->getPitch();

            const uint64_t clipOwnerId = ctx.selected->id;
            Scene* scene = ctx.scene;

            bool activated = false;
            bool committed = false;

            if (ImGui::SliderFloat("Volume", &volume, 0.0f, 1.0f, "%.2f"))
                clip->setVolume(volume);
            activated |= ImGui::IsItemActivated();
            committed |= ImGui::IsItemDeactivatedAfterEdit();

            if (ImGui::SliderFloat("Pitch", &pitch, 0.5f, 2.0f, "%.2f"))
                clip->setPitch(pitch);
            activated |= ImGui::IsItemActivated();
            committed |= ImGui::IsItemDeactivatedAfterEdit();

            if (activated && !m_audioDragActive)
            {
                m_audioDragActive       = true;
                m_audioDragBeforeVolume = clip->getVolume();
                m_audioDragBeforePitch  = clip->getPitch();
            }

            if (committed && m_audioDragActive)
            {
                m_audioDragActive = false;
                const AudioClipState before{ m_audioDragBeforeVolume, m_audioDragBeforePitch };
                const AudioClipState after { clip->getVolume(), clip->getPitch() };

                if (!nearlyEqualF(before.volume, after.volume) ||
                    !nearlyEqualF(before.pitch,  after.pitch))
                {
                    // Resuelve el GameObject por id en cada aplicación, nunca
                    // captura el puntero: sobrevive a un undo de Delete que
                    // haya reconstruido el objeto entretanto.
                    auto apply = [scene, clipOwnerId](const AudioClipState& s) {
                        GameObject* go = scene->findById(clipOwnerId);
                        if (!go || !go->hasAudioClip()) return;
                        go->getAudioClip()->setVolume(s.volume);
                        go->getAudioClip()->setPitch(s.pitch);
                    };
                    if (ctx.scene)
                        ctx.undo->push(std::make_unique<PropertyCommand<AudioClipState>>(
                            "Audio Clip de '" + ctx.selected->name + "'", before, after, apply));
                }
            }
```

Si `nearlyEqualF` no existe ya en el fichero, añadir arriba, junto a los demás helpers de fichero:

```cpp
static bool nearlyEqualF(float a, float b) { return std::fabs(a - b) < 0.0001f; }
```

`ctx.undo->push(...)` bajo `if (ctx.scene)` es exactamente lo que hace
`drawRigidbodySection` en las líneas 946-948; `EditorContext` no tiene ningún
`pushCommand`. A diferencia de esa sección, aquí NO hacen falta campos
`m_edit*` cacheados: los sliders leen y escriben el componente directamente
cada frame, y el snapshot del drag es lo único que hay que recordar.

- [ ] **Step 4: Compilar y verificar los tests**

Run: `.\build.bat` y después `.\build-ninja\engine\tests\dt_audio_tests.exe`
Expected: build sin errores ni warnings nuevos, `ALL AUDIO TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/Editor/Command.h engine/include/DonTopo/Editor/PropertiesPanel.h engine/src/Editor/PropertiesPanel.cpp
git commit -m "feat(editor): sliders de Volume y Pitch con undo por drag"
```

---

### Task 5: Bindings Lua y documentación

**Files:**
- Modify: `engine/src/Scripting/ScriptBindings.cpp:300-321`
- Modify: `Scripts/README.md`
- Create: `Scripts/AudioFade.lua`

**Interfaces:**
- Consumes: `setVolume`/`getVolume`/`setPitch`/`getPitch` (Task 1), `setIs3D`/`getIs3D` (ya existen).
- Produces: los métodos Lua `SetVolume`, `GetVolume`, `SetPitch`, `GetPitch`, `SetIs3D`, `GetIs3D` del usertype `AudioClip`.

**Nota sobre tests:** sin test automático. No hay arnés que ejecute Lua headless en este repo. Se verifica en el criterio manual 5.

- [ ] **Step 1: Añadir los seis métodos al usertype**

En `engine/src/Scripting/ScriptBindings.cpp`, dentro de `lua.new_usertype<LuaAudioClip>("AudioClip", ...)`, tras `"GetLoop"` (línea 317-321), con el mismo cuerpo que los que ya hay:

```cpp
                "SetVolume", [](const LuaAudioClip& c, float v) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    go->getAudioClip()->setVolume(v);
                },
                "GetVolume", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    return go->getAudioClip()->getVolume();
                },
                "SetPitch", [](const LuaAudioClip& c, float p) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    go->getAudioClip()->setPitch(p);
                },
                "GetPitch", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    return go->getAudioClip()->getPitch();
                },
                // Ojo: setIs3D RECARGA el sonido (unloadSound + loadSound
                // porque is3D va horneado en el FMOD_MODE) y corta lo que
                // estuviera sonando. Es configuración, no algo de llamar por
                // frame — al revés que SetVolume/SetPitch.
                "SetIs3D", [](const LuaAudioClip& c, bool b) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    go->getAudioClip()->setIs3D(b);
                },
                "GetIs3D", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    if (!go->hasAudioClip()) throw std::runtime_error("El GameObject ya no tiene AudioClip");
                    return go->getAudioClip()->getIs3D();
                },
```

El helper que resuelve el `GameObject` se llama `deref`, igual que en `Play`, `Stop`, `SetLoop` y `GetLoop` y en los usertypes de collider de más arriba.

- [ ] **Step 2: Compilar**

Run: `.\build.bat`
Expected: sin errores ni warnings nuevos.

- [ ] **Step 3: Script de ejemplo**

`Scripts/AudioFade.lua` (fichero nuevo):

```lua
-- Baja el volumen del AudioClip del GameObject hasta cero y lo para.
-- Sirve de prueba manual de SetVolume/GetVolume por frame.
AudioFade = {
    -- Segundos que tarda el fade completo
    fadeTime = 3
}

function AudioFade:Start()
    self.clip = self.entity:GetComponent("AudioClip")
    if self.clip then
        self.clip:SetVolume(1.0)
        self.clip:Play()
    end
end

function AudioFade:Update(dt)
    if not self.clip then return end

    local v = self.clip:GetVolume() - dt / self.fadeTime
    if v <= 0 then
        self.clip:SetVolume(0)
        self.clip:Stop()
        self.clip = nil
    else
        self.clip:SetVolume(v)
    end
end
```

- [ ] **Step 4: Documentar en el README de scripting**

Localizar la ruta real: `git ls-files | grep -i readme`. En la tabla/sección de `AudioClip`, junto a `Play`, `Stop`, `SetLoop` y `GetLoop`, añadir:

```markdown
| `clip:SetVolume(v)` | Volumen del clip, recortado a `[0, 1]`. Seguro de llamar en `Update`: sólo escribe en el canal. |
| `clip:GetVolume()` | Volumen actual. |
| `clip:SetPitch(p)` | Pitch del clip, recortado a `[0.5, 2]`. `2.0` es una octava arriba y el doble de velocidad. Seguro en `Update`. |
| `clip:GetPitch()` | Pitch actual. |
| `clip:SetIs3D(b)` | Cambia entre 2D y 3D. **Recarga el sonido y corta lo que esté sonando**: es configuración, no lo llames por frame. |
| `clip:GetIs3D()` | `true` si el clip es 3D. |

Ver `Scripts/AudioFade.lua` para un fade completo.
```

Respetar el formato que ya use ese README (si no es una tabla, adaptarlo a lo que haya).

- [ ] **Step 5: Verificar la batería completa**

Run: `.\build.bat`, y después cada uno de los seis ejecutables de `build-ninja\engine\tests\`
Expected: los seis en verde (`dt_animator_tests: OK`, `ALL PHYSICS TESTS PASSED`, `ALL CAMERA TESTS PASSED`, `ALL CONTENT BROWSER TESTS PASSED`, `dt_exporter_tests: OK`, `ALL AUDIO TESTS PASSED`).

- [ ] **Step 6: Commit**

```bash
git add engine/src/Scripting/ScriptBindings.cpp Scripts/AudioFade.lua Scripts/README.md
git commit -m "feat(scripting): bindings de volumen, pitch e is3D del AudioClip"
```

---

## Verificación manual (usuario, requiere GUI y altavoces)

No la puede hacer ningún agente de este entorno. Corresponde a los criterios 4 y 5 de la spec:

- [ ] Con un clip sonando, mover el slider **Volume**: el volumen cambia al momento.
- [ ] Ídem con **Pitch**: cambia el tono y la velocidad.
- [ ] `Ctrl+Z` tras un drag deshace el drag **entero**, no frame a frame. Un segundo `Ctrl+Z` no deshace el mismo drag otra vez.
- [ ] Guardar la escena, recargarla: volumen y pitch se conservan.
- [ ] Cargar una escena guardada **antes** de esta feature: carga sin error, con volumen y pitch a 1.0.
- [ ] `Scripts/AudioFade.lua` sobre un GameObject con AudioClip: al dar a Play, el sonido se desvanece en 3 segundos.
- [ ] Desde Lua, `GetIs3D()` devuelve lo que marca el checkbox del editor, y `SetIs3D(true)` lo cambia (cortando el sonido, que es lo esperado).
