# AudioClip Component Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Añadir un componente AudioClip (un único clip de audio por GameObject) editable desde el panel Properties: Browse (ImGuiFileDialog) o drag&drop desde Content Browser para asignarlo, checkboxes "Loop" e "Is 3D?" que controlan de verdad la reproducción FMOD, botones Play/Stop de previsualización, botón "x" para quitarlo, sin crashear en ningún caso.

**Architecture:** `AudioClipComponent` (nuevo) encapsula un `soundId` de `AudioManager` + los flags `is3D`/`loop`, y decide cuándo recargar el sonido en FMOD (mismo patrón que `BoxCollider` envolviendo un actor PhysX). `AudioManager` gana un factory `createAudioClipComponent(path, is3D, loop)` (igual que `PhysicsManager::createXColliderComponent`) y tres piezas nuevas: `loadSound` con parámetro `loop`, `unloadSound(id)`, `stopSound(id)`. `AudioManager` deja de ser exclusivo de `sandbox/main.cpp`: se inyecta como puntero no-propietario en `EditorUI` vía `Renderer::setAudioManager` (mismo patrón que `setPhysicsManager`). `EditorUI::loadAudioClipForSelected(path)` es el único punto de entrada tanto para Browse como para drag&drop, con `IGFD::FileDialog` en instancia propia (nunca el singleton compartido con Content Browser ni con el diálogo de Mesh). Se corrige además un bug de orden de destrucción en `sandbox/main.cpp` (audio se destruía antes que root).

**Tech Stack:** C++20, Vulkan, ImGui, ImGuiFileDialog, FMOD (`DT_FMOD_ENABLED`, no-op si no está instalado), CMake + Ninja (preset `debug`).

## Global Constraints

- No hay framework de tests en el repo (sin gtest/ctest). Verificación = build (`cmake --build --preset debug`) + ejecutar `build-ninja/sandbox/Sandbox.exe` + revisar visualmente, igual que el resto del proyecto.
- Formatos soportados: `.wav`, `.mp3`, `.ogg`, `.flac` (validación case-insensitive por extensión).
- Un GameObject solo puede tener un AudioClip: `hasAudioClip()` ya true → no-op (hay que quitarlo con "x" antes de asignar otro), y la entrada "Audio Clip" del popup Add aparece deshabilitada.
- `loop`/`is3D` van horneados en el `FMOD_MODE` al crear el `FMOD::Sound` — togglearlos en el editor recarga el sonido (`unloadSound` + `loadSound`), nunca muta un sonido ya creado.
- No hay serialización de escena (no existe para ningún componente todavía) — AudioClip no la necesita.
- Spec completo: `docs/superpowers/specs/2026-07-08-audioclip-component-design.md`.

---

### Task 1: Backend — `AudioClipComponent`, extensiones de `AudioManager`, slot en `GameObject`

**Files:**
- Create: `engine/include/DonTopo/AudioClipComponent.h`
- Create: `engine/src/AudioClipComponent.cpp`
- Modify: `engine/include/DonTopo/AudioManager.h`
- Modify: `engine/src/AudioManager.cpp`
- Modify: `engine/include/DonTopo/GameObject.h`
- Modify: `engine/CMakeLists.txt`

**Interfaces:**
- Produces: `class AudioClipComponent` con `play(worldPos)`, `stop()`, `setLoop(bool)`, `setIs3D(bool)`, `getLoop() const`, `getIs3D() const`, `getPath() const`.
- Produces: `AudioManager::loadSound(path, is3D=true, loop=false) -> int`, `AudioManager::unloadSound(int)`, `AudioManager::stopSound(int)`, `AudioManager::createAudioClipComponent(path, is3D, loop) -> std::shared_ptr<AudioClipComponent>`.
- Produces: `GameObject::setAudioClip(std::shared_ptr<AudioClipComponent>)`, `GameObject::getAudioClip() const -> const std::shared_ptr<AudioClipComponent>&`, `GameObject::hasAudioClip() const -> bool`.

Este task no cambia ningún comportamiento visible todavía — nada invoca estos métodos nuevos hasta el Task 3. Verificación = solo compila.

- [ ] **Step 1: Extender `AudioManager.h`**

En `engine/include/DonTopo/AudioManager.h`, reemplazar el archivo completo por:

```cpp
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <glm/glm.hpp>

namespace DonTopo {

class AudioClipComponent;

class AudioManager {
public:
    AudioManager() = default;
    ~AudioManager();
    AudioManager(const AudioManager&)            = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    void init();
    void update(const glm::vec3& listenerPos,
                const glm::vec3& listenerForward,
                const glm::vec3& listenerUp);
    void shutdown();

    int  loadSound(const std::string& path, bool is3D = true, bool loop = false);
    void unloadSound(int soundId);
    int  loadBGM(const std::string& path);

    void playSound(int soundId, const glm::vec3& worldPos = {});
    void stopSound(int soundId);
    void playBGM(int bgmId);
    void stopBGM();
    void pauseBGM(bool paused);

    void setMasterVolume(float v);
    void setSfxVolume   (float v);
    void setBgmVolume   (float v);

    // Carga path con el modo dado (is3D/loop horneados en el FMOD_MODE) y
    // envuelve el soundId resultante en un AudioClipComponent listo para
    // colgar de un GameObject (GameObject::setAudioClip). nullptr si
    // loadSound falla (fichero inválido/no soportado por FMOD).
    std::shared_ptr<AudioClipComponent> createAudioClipComponent(const std::string& path, bool is3D, bool loop);

private:
#ifdef DT_FMOD_ENABLED
    void* m_system   = nullptr;  // FMOD::System*
    void* m_sfxGroup = nullptr;  // FMOD::ChannelGroup*
    void* m_bgmGroup = nullptr;  // FMOD::ChannelGroup*
    void* m_bgmCh    = nullptr;  // FMOD::Channel* (currently playing BGM)
    std::vector<void*> m_sounds;      // FMOD::Sound* SFX clips
    std::vector<void*> m_sfxChannels; // FMOD::Channel* de la última reproducción de cada id (paralelo a m_sounds)
    std::vector<void*> m_bgmSounds;   // FMOD::Sound* BGM streams
#endif
};

} // namespace DonTopo
```

- [ ] **Step 2: Implementar las extensiones en `AudioManager.cpp`**

En `engine/src/AudioManager.cpp`, añadir el include junto a los existentes (línea 1):

```cpp
#include "DonTopo/AudioManager.h"
#include "DonTopo/AudioClipComponent.h"
```

Reemplazar `AudioManager::loadSound` (líneas 69-81 actuales):

```cpp
int AudioManager::loadSound(const std::string& path, bool is3D)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system) return -1;
    FMOD_MODE mode = is3D ? (FMOD_3D | FMOD_LOOP_OFF) : (FMOD_2D | FMOD_LOOP_OFF);
    FMOD::Sound* snd;
    if (SYS->createSound(path.c_str(), mode, nullptr, &snd) != FMOD_OK) return -1;
    m_sounds.push_back(snd);
    return (int)m_sounds.size() - 1;
#else
    return -1;
#endif
}
```

por:

```cpp
int AudioManager::loadSound(const std::string& path, bool is3D, bool loop)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system) return -1;
    FMOD_MODE mode = (is3D ? FMOD_3D : FMOD_2D) | (loop ? FMOD_LOOP_NORMAL : FMOD_LOOP_OFF);
    FMOD::Sound* snd;
    if (SYS->createSound(path.c_str(), mode, nullptr, &snd) != FMOD_OK) return -1;
    m_sounds.push_back(snd);
    m_sfxChannels.push_back(nullptr);
    return (int)m_sounds.size() - 1;
#else
    (void)loop;
    return -1;
#endif
}

void AudioManager::unloadSound(int id)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() || !m_sounds[id]) return;
    reinterpret_cast<FMOD::Sound*>(m_sounds[id])->release();
    m_sounds[id] = nullptr;
    m_sfxChannels[id] = nullptr;
#endif
}
```

Reemplazar `AudioManager::playSound` (líneas 97-111 actuales):

```cpp
void AudioManager::playSound(int id, const glm::vec3& worldPos)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size()) return;
    FMOD::Channel* ch;
    auto* snd = reinterpret_cast<FMOD::Sound*>(m_sounds[id]);
    if (SYS->playSound(snd, SFXG, false, &ch) != FMOD_OK) return;
    FMOD_MODE mode; snd->getMode(&mode);
    if (mode & FMOD_3D) {
        FMOD_VECTOR p = { worldPos.x, worldPos.y, worldPos.z };
        FMOD_VECTOR v = { 0, 0, 0 };
        ch->set3DAttributes(&p, &v);
    }
#endif
}
```

por:

```cpp
void AudioManager::playSound(int id, const glm::vec3& worldPos)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() || !m_sounds[id]) return;
    FMOD::Channel* ch;
    auto* snd = reinterpret_cast<FMOD::Sound*>(m_sounds[id]);
    if (SYS->playSound(snd, SFXG, false, &ch) != FMOD_OK) return;
    m_sfxChannels[id] = ch;
    FMOD_MODE mode; snd->getMode(&mode);
    if (mode & FMOD_3D) {
        FMOD_VECTOR p = { worldPos.x, worldPos.y, worldPos.z };
        FMOD_VECTOR v = { 0, 0, 0 };
        ch->set3DAttributes(&p, &v);
    }
#endif
}

void AudioManager::stopSound(int id)
{
#ifdef DT_FMOD_ENABLED
    if (id < 0 || id >= (int)m_sfxChannels.size() || !m_sfxChannels[id]) return;
    reinterpret_cast<FMOD::Channel*>(m_sfxChannels[id])->stop();
#endif
}
```

Añadir `createAudioClipComponent` al final del archivo, antes del cierre `} // namespace DonTopo` (línea 162 actual):

```cpp
std::shared_ptr<AudioClipComponent> AudioManager::createAudioClipComponent(const std::string& path, bool is3D, bool loop)
{
    int id = loadSound(path, is3D, loop);
    if (id < 0) return nullptr;
    return std::make_shared<AudioClipComponent>(this, path, id, is3D, loop);
}
```

- [ ] **Step 3: Crear `AudioClipComponent.h`**

Crear `engine/include/DonTopo/AudioClipComponent.h`:

```cpp
#pragma once
#include <string>
#include <glm/glm.hpp>

namespace DonTopo {

class AudioManager;

// Componente único de audio por GameObject. Envuelve un soundId de
// AudioManager; loop/is3D van horneados en el FMOD_MODE del sonido, así
// que cambiarlos recarga el clip (unloadSound + loadSound) en vez de
// mutar el sonido existente.
class AudioClipComponent {
public:
    AudioClipComponent(AudioManager* audio, std::string path, int soundId, bool is3D, bool loop);
    ~AudioClipComponent();

    AudioClipComponent(const AudioClipComponent&)            = delete;
    AudioClipComponent& operator=(const AudioClipComponent&) = delete;

    void play(const glm::vec3& worldPos);
    void stop();

    // No-op si el valor no cambia (evita recargas del sonido en cada frame).
    void setLoop(bool loop);
    void setIs3D(bool is3D);

    bool getLoop() const  { return m_loop; }
    bool getIs3D() const  { return m_is3D; }
    const std::string& getPath() const { return m_path; }

private:
    void reload();

    AudioManager* m_audio;
    std::string   m_path;
    int           m_soundId;
    bool          m_is3D;
    bool          m_loop;
};

} // namespace DonTopo
```

- [ ] **Step 4: Crear `AudioClipComponent.cpp`**

Crear `engine/src/AudioClipComponent.cpp`:

```cpp
#include "DonTopo/AudioClipComponent.h"
#include "DonTopo/AudioManager.h"

namespace DonTopo {

AudioClipComponent::AudioClipComponent(AudioManager* audio, std::string path, int soundId, bool is3D, bool loop)
    : m_audio(audio)
    , m_path(std::move(path))
    , m_soundId(soundId)
    , m_is3D(is3D)
    , m_loop(loop)
{
}

AudioClipComponent::~AudioClipComponent()
{
    if (m_audio) m_audio->unloadSound(m_soundId);
}

void AudioClipComponent::play(const glm::vec3& worldPos)
{
    if (m_audio) m_audio->playSound(m_soundId, worldPos);
}

void AudioClipComponent::stop()
{
    if (m_audio) m_audio->stopSound(m_soundId);
}

void AudioClipComponent::setLoop(bool loop)
{
    if (loop == m_loop) return;
    m_loop = loop;
    reload();
}

void AudioClipComponent::setIs3D(bool is3D)
{
    if (is3D == m_is3D) return;
    m_is3D = is3D;
    reload();
}

void AudioClipComponent::reload()
{
    if (!m_audio) return;
    m_audio->unloadSound(m_soundId);
    m_soundId = m_audio->loadSound(m_path, m_is3D, m_loop);
}

} // namespace DonTopo
```

- [ ] **Step 5: Registrar el nuevo `.cpp` en CMake**

En `engine/CMakeLists.txt`, reemplazar (línea 14):

```cmake
    src/AudioManager.cpp
```

por:

```cmake
    src/AudioManager.cpp
    src/AudioClipComponent.cpp
```

- [ ] **Step 6: Añadir el slot AudioClip a `GameObject`**

En `engine/include/DonTopo/GameObject.h`, añadir el include (línea 11):

```cpp
#include "DonTopo/PlaneCollider.h"
#include "DonTopo/AudioClipComponent.h"
```

Añadir el trío `set/get/has`, justo después del bloque `hasAnyCollider()` (líneas 44-50 actuales):

```cpp
            // true si tiene cualquiera de los 4 tipos de collider — los 4 son
            // mutuamente excluyentes (impuesto por EditorUI, no por esta clase),
            // usado como guard único en el popup "Add".
            bool hasAnyCollider() const
            {
                return m_boxCollider || m_sphereCollider || m_capsuleCollider || m_planeCollider;
            }

            void setAudioClip(std::shared_ptr<AudioClipComponent> clip) { m_audioClip = std::move(clip); }
            const std::shared_ptr<AudioClipComponent>& getAudioClip() const { return m_audioClip; }
            bool hasAudioClip() const { return m_audioClip != nullptr; }
```

Añadir el campo privado, justo después de `m_planeCollider` (línea 77 actual):

```cpp
            std::shared_ptr<PlaneCollider> m_planeCollider;
            std::shared_ptr<AudioClipComponent> m_audioClip;
```

- [ ] **Step 7: Compilar**

Run: `cmake --build --preset debug`
Expected: build termina sin error. Nada invoca todavía los métodos nuevos — si el compilador avisa de función no usada, es esperado en este punto.

- [ ] **Step 8: Commit**

```bash
git add engine/include/DonTopo/AudioManager.h engine/src/AudioManager.cpp engine/include/DonTopo/AudioClipComponent.h engine/src/AudioClipComponent.cpp engine/include/DonTopo/GameObject.h engine/CMakeLists.txt
git commit -m "feat(audio): backend de AudioClipComponent y extensiones de AudioManager"
```

---

### Task 2: Wiring — inyectar `AudioManager` en `EditorUI` y arreglar orden de destrucción en sandbox

**Files:**
- Modify: `engine/include/DonTopo/Renderer.h`
- Modify: `engine/include/DonTopo/EditorUI.h`
- Modify: `sandbox/src/main.cpp`

**Interfaces:**
- Consumes: nada nuevo (usa clases ya existentes).
- Produces: `Renderer::setAudioManager(AudioManager*)`, `EditorUI::setAudioManager(AudioManager*)`, campo `EditorUI::m_audio` (consumido por Task 3).

Este task no cambia ningún comportamiento visible en la UI todavía (no hay sección Audio Clip aún) — verificación = compila + smoke test (la app arranca y cierra sin crash).

- [ ] **Step 1: Forward-declarar `AudioManager` y añadir `setAudioManager` en `Renderer.h`**

En `engine/include/DonTopo/Renderer.h`, reemplazar (líneas 19-21):

```cpp
    class Window;
    class GameObject;
    class PhysicsManager;
```

por:

```cpp
    class Window;
    class GameObject;
    class PhysicsManager;
    class AudioManager;
```

Reemplazar (línea 37):

```cpp
            void setPhysicsManager(PhysicsManager* physics) { m_editorUI.setPhysicsManager(physics); }
```

por:

```cpp
            void setPhysicsManager(PhysicsManager* physics) { m_editorUI.setPhysicsManager(physics); }
            void setAudioManager(AudioManager* audio) { m_editorUI.setAudioManager(audio); }
```

- [ ] **Step 2: Forward-declarar `AudioManager` y añadir `setAudioManager`/`m_audio` en `EditorUI.h`**

En `engine/include/DonTopo/EditorUI.h`, reemplazar (líneas 14-22):

```cpp
class GameObject;
class Mesh;
class PhysicsManager;
class BoxCollider;
class SphereCollider;
class CapsuleCollider;
class PlaneCollider;
class Renderer;
class Camera;
```

por:

```cpp
class GameObject;
class Mesh;
class PhysicsManager;
class AudioManager;
class BoxCollider;
class SphereCollider;
class CapsuleCollider;
class PlaneCollider;
class Renderer;
class Camera;
```

Reemplazar (línea 44):

```cpp
    void setPhysicsManager(PhysicsManager* physics) { m_physics = physics; }
```

por:

```cpp
    void setPhysicsManager(PhysicsManager* physics) { m_physics = physics; }
    // Puntero no-propietario: AudioManager vive fuera del EditorUI (ver
    // wiring en sandbox/main.cpp), mismo patrón que m_physics. Necesario
    // para cargar/reproducir clips desde la sección Audio Clip del panel
    // Properties.
    void setAudioManager(AudioManager* audio) { m_audio = audio; }
```

Reemplazar (líneas 167-168):

```cpp
    PhysicsManager* m_physics = nullptr;
    Renderer*       m_renderer = nullptr;
```

por:

```cpp
    PhysicsManager* m_physics = nullptr;
    Renderer*       m_renderer = nullptr;
    AudioManager*   m_audio = nullptr;
```

- [ ] **Step 3: Arreglar orden de destrucción y conectar `AudioManager` en `sandbox/main.cpp`**

En `sandbox/src/main.cpp`, reemplazar (líneas 30-39):

```cpp
        DonTopo::Renderer renderer;

        // physics se declara antes que root: en cualquier salida de scope (normal
        // o por excepción) root se destruye primero (liberando los BoxCollider de
        // sus GameObject) y physics se destruye después — nunca al revés, evitando
        // que ~BoxCollider() libere un PxRigidDynamic sobre una PxScene ya liberada.
        DonTopo::PhysicsManager physics;
        physics.init();

        DonTopo::GameObject root("root");
```

por:

```cpp
        DonTopo::Renderer renderer;

        // physics y audio se declaran antes que root: en cualquier salida de scope
        // (normal o por excepción) root se destruye primero (liberando los
        // BoxCollider/AudioClipComponent de sus GameObject) y physics/audio se
        // destruyen después — nunca al revés, evitando que ~BoxCollider() libere un
        // PxRigidDynamic sobre una PxScene ya liberada, o que ~AudioClipComponent()
        // llame a AudioManager::unloadSound sobre un AudioManager ya destruido.
        DonTopo::PhysicsManager physics;
        physics.init();

        DonTopo::AudioManager audio;
        audio.init();

        DonTopo::GameObject root("root");
```

Reemplazar (líneas 112-119 tras el cambio anterior, buscar por contenido):

```cpp
        DonTopo::Camera camera({0.0f, 90.0f, 300.0f});

        DonTopo::AudioManager audio;
        audio.init();
        //int bgm = audio.loadBGM("assets/audio.mp3");
        //if (bgm >= 0) audio.playBGM(bgm);

        renderer.init(window, meshes);
```

por:

```cpp
        DonTopo::Camera camera({0.0f, 90.0f, 300.0f});

        //int bgm = audio.loadBGM("assets/audio.mp3");
        //if (bgm >= 0) audio.playBGM(bgm);

        renderer.init(window, meshes);
```

Reemplazar (línea 121, buscar por contenido):

```cpp
        renderer.setPhysicsManager(&physics);
```

por:

```cpp
        renderer.setPhysicsManager(&physics);
        renderer.setAudioManager(&audio);
```

- [ ] **Step 4: Compilar**

Run: `cmake --build --preset debug`
Expected: build termina sin error.

- [ ] **Step 5: Smoke test manual**

Run: `build-ninja/sandbox/Sandbox.exe`

1. La app arranca sin crash (mismo comportamiento que antes del cambio — no hay UI nueva todavía).
2. Cerrar la app con la X o Alt+F4 → no crash al cerrar (confirma que el nuevo orden de declaración de `audio`/`physics`/`root` no rompe la destrucción normal).

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Renderer.h engine/include/DonTopo/EditorUI.h sandbox/src/main.cpp
git commit -m "feat(audio): inyectar AudioManager en EditorUI y fijar orden de destrucción en sandbox"
```

---

### Task 3: UI — sección "Audio Clip" en Properties (Browse + Play/Stop/Loop/Is3D + remove)

**Files:**
- Modify: `engine/include/DonTopo/EditorUI.h`
- Modify: `engine/src/EditorUI.cpp`

**Interfaces:**
- Consumes: `AudioManager::createAudioClipComponent` (Task 1), `AudioClipComponent::play/stop/setLoop/setIs3D/getLoop/getIs3D/getPath` (Task 1), `GameObject::setAudioClip/getAudioClip/hasAudioClip` (Task 1), `EditorUI::m_audio` (Task 2).
- Produces: `void EditorUI::drawAudioClipSection()`, `void EditorUI::drawAudioClipDialog()`, `void EditorUI::loadAudioClipForSelected(const std::string&)`.

- [ ] **Step 1: Declarar los métodos y campos nuevos en `EditorUI.h`**

Reemplazar (bloque de métodos privados, buscar por contenido):

```cpp
    void drawBoxColliderSection();
    void drawSphereColliderSection();
    void drawCapsuleColliderSection();
    void drawPlaneColliderSection();
    void drawMeshSection();
    void drawMeshDialog();
    void drawAddComponentButton();
    // Crea un GameObject hijo de parent con el mesh dado, lo registra en el
    // Renderer (staticRenderIndex) y lo deja sin collider. No-op si parent o
    // m_renderer son nullptr.
    void createBasicShape(GameObject* parent, const std::string& name, std::shared_ptr<Mesh> mesh);
    // Único punto de entrada para asignar un Mesh a m_selected (Browse o
    // drag&drop convergen aquí). No-op si no hay selección, no hay Renderer,
    // o m_selected ya tiene mesh. Extensión no .fbx o fallo de Assimp
    // escriben m_meshLoadError sin modificar m_selected.
    void loadMeshForSelected(const std::string& path);
    void drawContentBrowser();
```

por:

```cpp
    void drawBoxColliderSection();
    void drawSphereColliderSection();
    void drawCapsuleColliderSection();
    void drawPlaneColliderSection();
    void drawMeshSection();
    void drawMeshDialog();
    void drawAudioClipSection();
    void drawAudioClipDialog();
    void drawAddComponentButton();
    // Crea un GameObject hijo de parent con el mesh dado, lo registra en el
    // Renderer (staticRenderIndex) y lo deja sin collider. No-op si parent o
    // m_renderer son nullptr.
    void createBasicShape(GameObject* parent, const std::string& name, std::shared_ptr<Mesh> mesh);
    // Único punto de entrada para asignar un Mesh a m_selected (Browse o
    // drag&drop convergen aquí). No-op si no hay selección, no hay Renderer,
    // o m_selected ya tiene mesh. Extensión no .fbx o fallo de Assimp
    // escriben m_meshLoadError sin modificar m_selected.
    void loadMeshForSelected(const std::string& path);
    // Único punto de entrada para asignar un AudioClip a m_selected (Browse o
    // drag&drop convergen aquí). No-op si no hay selección, no hay
    // AudioManager, o m_selected ya tiene AudioClip. Extensión no soportada o
    // fallo de FMOD escriben m_audioLoadError sin modificar m_selected.
    void loadAudioClipForSelected(const std::string& path);
    void drawContentBrowser();
```

Reemplazar (bloque de campos de Content Browser, buscar por contenido):

```cpp
    bool m_dlgOpen = false;
    bool m_meshDlgOpen = false;
    bool m_scanned = false;
```

por:

```cpp
    bool m_dlgOpen = false;
    bool m_meshDlgOpen = false;
    bool m_audioDlgOpen = false;
    bool m_scanned = false;
```

Reemplazar (declaración de `m_meshFileDialog`, buscar por contenido):

```cpp
    // Instancia propia de ImGuiFileDialog para "Add > Mesh", separada del
    // singleton IGFD::FileDialog::Instance() que usa Content Browser: la
    // librería documenta que Instance() no soporta 2 diálogos concurrentes
    // (mismo estado interno de lista de ficheros/thumbnails/columnas);
    // compartirlo causaba corrupción al redimensionar el popup de Mesh
    // mientras Content Browser seguía dibujando su panel embebido el mismo
    // frame. unique_ptr porque IGFD::FileDialog es tipo incompleto aquí.
    std::unique_ptr<IGFD::FileDialog> m_meshFileDialog;
```

por:

```cpp
    // Instancia propia de ImGuiFileDialog para "Add > Mesh", separada del
    // singleton IGFD::FileDialog::Instance() que usa Content Browser: la
    // librería documenta que Instance() no soporta 2 diálogos concurrentes
    // (mismo estado interno de lista de ficheros/thumbnails/columnas);
    // compartirlo causaba corrupción al redimensionar el popup de Mesh
    // mientras Content Browser seguía dibujando su panel embebido el mismo
    // frame. unique_ptr porque IGFD::FileDialog es tipo incompleto aquí.
    std::unique_ptr<IGFD::FileDialog> m_meshFileDialog;
    // Misma razón que m_meshFileDialog: instancia propia, nunca compartida
    // con el singleton de Content Browser ni con m_meshFileDialog.
    std::unique_ptr<IGFD::FileDialog> m_audioFileDialog;
```

Reemplazar (bloque final de campos, buscar por contenido):

```cpp
    // Mensaje del último intento fallido de carga de Mesh (vacío si no hay
    // error pendiente); se limpia al cambiar de selección o al cargar bien.
    std::string     m_meshLoadError;
    // GameObject para el que se pulsó "Add > Mesh" (revela la sección
    // Browse/drop hasta que se asigne un mesh o se pulse "x" para quitarlo).
    // nullptr = sección oculta. No se limpia al cambiar de selección: si el
    // usuario vuelve al mismo GameObject sin haber completado la carga, la
    // sección sigue visible (igual que dejar un diálogo de collider a medias).
    GameObject*     m_meshAddRequestedFor = nullptr;
};
```

por:

```cpp
    // Mensaje del último intento fallido de carga de Mesh (vacío si no hay
    // error pendiente); se limpia al cambiar de selección o al cargar bien.
    std::string     m_meshLoadError;
    // GameObject para el que se pulsó "Add > Mesh" (revela la sección
    // Browse/drop hasta que se asigne un mesh o se pulse "x" para quitarlo).
    // nullptr = sección oculta. No se limpia al cambiar de selección: si el
    // usuario vuelve al mismo GameObject sin haber completado la carga, la
    // sección sigue visible (igual que dejar un diálogo de collider a medias).
    GameObject*     m_meshAddRequestedFor = nullptr;
    // Mismo patrón que m_meshLoadError/m_meshAddRequestedFor pero para el
    // componente AudioClip.
    std::string     m_audioLoadError;
    GameObject*     m_audioClipAddRequestedFor = nullptr;
};
```

- [ ] **Step 2: Includes nuevos en `EditorUI.cpp`**

Reemplazar (líneas 1-21):

```cpp
#include "DonTopo/EditorUI.h"
#include "DonTopo/GameObject.h"
#include "DonTopo/PhysicsManager.h"
#include "DonTopo/BoxCollider.h"
#include "DonTopo/SphereCollider.h"
#include "DonTopo/CapsuleCollider.h"
#include "DonTopo/PlaneCollider.h"
#include "DonTopo/Gizmos.h"
#include "DonTopo/Renderer.h"
#include "DonTopo/Camera.h"
#include "DonTopo/Cube.h"
#include "DonTopo/Sphere.h"
#include "DonTopo/Plane.h"
#include "DonTopo/Capsule.h"
#include "DonTopo/ModelLoader.h"
#include <imgui.h>
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
```

por:

```cpp
#include "DonTopo/EditorUI.h"
#include "DonTopo/GameObject.h"
#include "DonTopo/PhysicsManager.h"
#include "DonTopo/AudioManager.h"
#include "DonTopo/AudioClipComponent.h"
#include "DonTopo/BoxCollider.h"
#include "DonTopo/SphereCollider.h"
#include "DonTopo/CapsuleCollider.h"
#include "DonTopo/PlaneCollider.h"
#include "DonTopo/Gizmos.h"
#include "DonTopo/Renderer.h"
#include "DonTopo/Camera.h"
#include "DonTopo/Cube.h"
#include "DonTopo/Sphere.h"
#include "DonTopo/Plane.h"
#include "DonTopo/Capsule.h"
#include "DonTopo/ModelLoader.h"
#include <imgui.h>
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <set>
```

- [ ] **Step 3: Inicializar `m_audioFileDialog` en el constructor**

Reemplazar (líneas 104-107):

```cpp
EditorUI::EditorUI()
    : m_meshFileDialog(std::make_unique<IGFD::FileDialog>())
{
}
```

por:

```cpp
EditorUI::EditorUI()
    : m_meshFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_audioFileDialog(std::make_unique<IGFD::FileDialog>())
{
}
```

- [ ] **Step 4: Drenar el diálogo de audio cada frame desde `draw()`**

Reemplazar (líneas 111-120):

```cpp
void EditorUI::draw(VkDescriptorSet viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView)
{
    drawDockSpace();
    drawScene(sceneRoot);
    drawSelectionGizmo();
    drawViewport(viewportTexture, cameraView);
    drawProperties();
    drawMeshDialog();
    drawContentBrowser();
}
```

por:

```cpp
void EditorUI::draw(VkDescriptorSet viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView)
{
    drawDockSpace();
    drawScene(sceneRoot);
    drawSelectionGizmo();
    drawViewport(viewportTexture, cameraView);
    drawProperties();
    drawMeshDialog();
    drawAudioClipDialog();
    drawContentBrowser();
}
```

- [ ] **Step 5: Limpiar `m_audioLoadError` al cambiar de selección y llamar a `drawAudioClipSection()`**

Reemplazar (dentro de `drawProperties`, buscar por contenido):

```cpp
    if (m_propsCachedFor != m_selected)
    {
        glm::vec3 skew;
        glm::vec4 perspective;
        glm::quat orientation;
        glm::decompose(m_selected->localTransform, m_editScale, orientation, m_editPosition, skew, perspective);
        m_editRotationDeg = glm::degrees(glm::eulerAngles(orientation));
        m_propsCachedFor = m_selected;
        m_meshLoadError.clear();
    }
```

por:

```cpp
    if (m_propsCachedFor != m_selected)
    {
        glm::vec3 skew;
        glm::vec4 perspective;
        glm::quat orientation;
        glm::decompose(m_selected->localTransform, m_editScale, orientation, m_editPosition, skew, perspective);
        m_editRotationDeg = glm::degrees(glm::eulerAngles(orientation));
        m_propsCachedFor = m_selected;
        m_meshLoadError.clear();
        m_audioLoadError.clear();
    }
```

Reemplazar (llamadas a las secciones de componente, buscar por contenido):

```cpp
    drawBoxColliderSection();
    drawSphereColliderSection();
    drawCapsuleColliderSection();
    drawPlaneColliderSection();
    drawMeshSection();
    drawAddComponentButton();
```

por:

```cpp
    drawBoxColliderSection();
    drawSphereColliderSection();
    drawCapsuleColliderSection();
    drawPlaneColliderSection();
    drawMeshSection();
    drawAudioClipSection();
    drawAddComponentButton();
```

- [ ] **Step 6: Implementar `drawAudioClipSection` y `drawAudioClipDialog`**

Añadir, justo después del cierre de `EditorUI::drawMeshDialog()` (después de `}` que sigue a `m_meshDlgOpen = false;`):

```cpp
void EditorUI::drawAudioClipSection()
{
    // Oculto por defecto: solo se dibuja si ya tiene AudioClip, o si se
    // pulsó "Add > Audio Clip" para este GameObject concreto
    // (m_audioClipAddRequestedFor).
    if (!m_selected->hasAudioClip() && m_audioClipAddRequestedFor != m_selected)
        return;

    ImGui::Separator();

    if (m_selected->hasAudioClip())
    {
        auto& clip = m_selected->getAudioClip();
        bool sectionOpen = ImGui::TreeNodeEx("Audio Clip", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
        bool removeClicked = ImGui::SmallButton("x");

        if (sectionOpen)
        {
            std::string fname = std::filesystem::path(clip->getPath()).filename().string();
            ImGui::Text("%s", fname.c_str());

            ImGui::BeginDisabled(m_audio == nullptr);
            if (ImGui::Button("Play"))
            {
                glm::vec3 worldPos(m_selected->worldTransform[3]);
                clip->play(worldPos);
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop"))
                clip->stop();
            ImGui::EndDisabled();

            bool loop = clip->getLoop();
            if (ImGui::Checkbox("Loop", &loop))
                clip->setLoop(loop);

            bool is3D = clip->getIs3D();
            if (ImGui::Checkbox("Is 3D?", &is3D))
                clip->setIs3D(is3D);

            ImGui::TreePop();
        }

        if (removeClicked)
        {
            m_selected->setAudioClip(nullptr);
            // Vuelve a ocultar la sección tras quitar el clip — hay que
            // pulsar "Add > Audio Clip" de nuevo para reabrirla.
            m_audioClipAddRequestedFor = nullptr;
        }

        return;
    }

    ImGui::Text("Audio Clip");
    ImGui::BeginDisabled(m_audio == nullptr);
    if (ImGui::Button("Browse..."))
    {
        m_audioDlgOpen = true;
        IGFD::FileDialogConfig cfg;
        cfg.path = "assets";
        // Key plana sin "##" (mismo motivo documentado en drawMeshSection
        // para AddMeshDlg: con prefijo "##" el título concatenado generaba
        // 4 almohadillas seguidas y rompía el ID persistido de ImGui).
        m_audioFileDialog->OpenDialog("AddAudioDlg", "Choose Audio", ".wav,.mp3,.ogg,.flac", cfg);
    }
    ImGui::EndDisabled();

    ImGui::BeginChild("##AudioDropZone", ImVec2(0, 40), true);
    ImGui::TextDisabled("Drop audio here");
    ImGui::EndChild();

    if (!m_audioLoadError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_audioLoadError.c_str());
}

void EditorUI::drawAudioClipDialog()
{
    // Se ejecuta cada frame independientemente de m_selected/hasAudioClip():
    // si no se drena aquí, cambiar de selección mientras el diálogo está
    // abierto deja m_audioDlgOpen atascado en true (mismo motivo que
    // drawMeshDialog).
    if (m_audioDlgOpen && m_audioFileDialog->Display("AddAudioDlg"))
    {
        if (m_audioFileDialog->IsOk())
            loadAudioClipForSelected(m_audioFileDialog->GetFilePathName());
        m_audioFileDialog->Close();
        m_audioDlgOpen = false;
    }
}
```

- [ ] **Step 7: Implementar `loadAudioClipForSelected`**

Añadir, justo después del cierre de `EditorUI::loadMeshForSelected` (después de la línea que hoy es `engine/src/EditorUI.cpp:286`, junto a `loadMeshForSelected`):

```cpp
void EditorUI::loadAudioClipForSelected(const std::string& path)
{
    if (!m_selected || !m_audio || m_selected->hasAudioClip())
        return;

    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    static const std::set<std::string> kValidExt = {".wav", ".mp3", ".ogg", ".flac"};
    if (!kValidExt.count(ext))
    {
        m_audioLoadError = "Formato no soportado: " + ext;
        return;
    }

    auto clip = m_audio->createAudioClipComponent(path, /*is3D=*/false, /*loop=*/false);
    if (!clip)
    {
        m_audioLoadError = "No se pudo cargar el audio";
        return;
    }
    m_selected->setAudioClip(std::move(clip));
    m_audioLoadError.clear();
}
```

- [ ] **Step 8: Añadir "Audio Clip" al popup Add**

Reemplazar (dentro de `drawAddComponentButton`, buscar por contenido):

```cpp
        bool alreadyHasMesh = m_selected->hasMesh();
        ImGui::BeginDisabled(alreadyHasMesh);
        if (ImGui::Selectable("Mesh") && !alreadyHasMesh)
            m_meshAddRequestedFor = m_selected;
        ImGui::EndDisabled();

        ImGui::EndPopup();
```

por:

```cpp
        bool alreadyHasMesh = m_selected->hasMesh();
        ImGui::BeginDisabled(alreadyHasMesh);
        if (ImGui::Selectable("Mesh") && !alreadyHasMesh)
            m_meshAddRequestedFor = m_selected;
        ImGui::EndDisabled();

        bool alreadyHasAudio = m_selected->hasAudioClip();
        ImGui::BeginDisabled(alreadyHasAudio);
        if (ImGui::Selectable("Audio Clip") && !alreadyHasAudio)
            m_audioClipAddRequestedFor = m_selected;
        ImGui::EndDisabled();

        ImGui::EndPopup();
```

- [ ] **Step 9: Compilar**

Run: `cmake --build --preset debug`
Expected: build termina sin error.

- [ ] **Step 10: Verificación manual**

Run: `build-ninja/sandbox/Sandbox.exe`

1. Seleccionar un GameObject sin AudioClip → sección Audio Clip oculta.
2. Add > Audio Clip → aparece sección con botón "Browse..." y zona "Drop audio here".
3. Click "Browse..." → seleccionar un `.wav`/`.mp3` válido de `assets/` (si no hay ninguno, copiar un fichero de audio de prueba a `assets/` primero) → la sección pasa a mostrar el nombre del fichero con Play/Stop/Loop/Is 3D?/x.
4. Click "Play" → se oye el audio. Click "Stop" → se corta. Repetir varias veces sin crash.
5. Activar "Loop", Play → el audio se repite indefinidamente hasta pulsar Stop.
6. Activar "Is 3D?" y Play de nuevo (con la cámara en una posición distinta al GameObject) → confirmar que el comportamiento cambia respecto al modo 2D (paneo/atenuación por distancia).
7. Togglear Loop/Is 3D? varias veces seguidas (con y sin reproducción activa) → no crashea.
8. Click "x" → sección se oculta, Add > Audio Clip vuelve a estar disponible.
9. Con un AudioClip ya asignado, abrir Add → "Audio Clip" aparece deshabilitado.
10. Intentar Browse de un fichero no soportado (p.ej. `.fbx`) → mensaje de error rojo, no crashea.
11. Añadir AudioClip a un GameObject y borrar el GameObject entero (Delete) → no crash al borrar ni al cerrar la app.

- [ ] **Step 11: Commit**

```bash
git add engine/include/DonTopo/EditorUI.h engine/src/EditorUI.cpp
git commit -m "feat(editor): sección Audio Clip en Properties con Browse, Play/Stop, Loop e Is 3D"
```

---

### Task 4: Drag&drop desde Content Browser

**Files:**
- Modify: `engine/src/EditorUI.cpp` (`drawContentBrowser`, drag source; `drawAudioClipSection`, drag target)

**Interfaces:**
- Consumes: `EditorUI::loadAudioClipForSelected(const std::string&)` (Task 3).
- Produces: extiende el payload ImGui `"DT_ASSET_PATH"` ya existente (fuente ahora también para `.wav/.mp3/.ogg/.flac`, no solo `.fbx`; target nuevo en la sección Audio Clip).

- [ ] **Step 1: Extender el drag source de Content Browser a extensiones de audio**

Reemplazar (dentro de `drawContentBrowser`, buscar por contenido):

```cpp
        for (auto& path : m_assets) {
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
```

por:

```cpp
        static const std::set<std::string> kDraggableExt = {".fbx", ".wav", ".mp3", ".ogg", ".flac"};

        for (auto& path : m_assets) {
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
```

Reemplazar (buscar por contenido):

```cpp
            if (ext == ".fbx" && ImGui::BeginDragDropSource())
            {
                std::string fullPath = path.string();
                ImGui::SetDragDropPayload("DT_ASSET_PATH", fullPath.c_str(), fullPath.size() + 1);
                ImGui::Text("%s", fullPath.c_str());
                ImGui::EndDragDropSource();
            }
```

por:

```cpp
            if (kDraggableExt.count(ext) && ImGui::BeginDragDropSource())
            {
                std::string fullPath = path.string();
                ImGui::SetDragDropPayload("DT_ASSET_PATH", fullPath.c_str(), fullPath.size() + 1);
                ImGui::Text("%s", fullPath.c_str());
                ImGui::EndDragDropSource();
            }
```

- [ ] **Step 2: Drag target en la zona de drop de `drawAudioClipSection`**

Reemplazar (dentro de `drawAudioClipSection`, añadida en Task 3):

```cpp
    ImGui::BeginChild("##AudioDropZone", ImVec2(0, 40), true);
    ImGui::TextDisabled("Drop audio here");
    ImGui::EndChild();
```

por:

```cpp
    ImGui::BeginChild("##AudioDropZone", ImVec2(0, 40), true);
    ImGui::TextDisabled("Drop audio here");
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DT_ASSET_PATH"))
            loadAudioClipForSelected(std::string(static_cast<const char*>(payload->Data)));
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();
```

- [ ] **Step 3: Compilar**

Run: `cmake --build --preset debug`
Expected: build termina sin error.

- [ ] **Step 4: Verificación manual**

Run: `build-ninja/sandbox/Sandbox.exe`

1. Seleccionar un GameObject sin AudioClip, Add > Audio Clip → en Content Browser, arrastrar un asset `.mp3`/`.wav`/`.ogg`/`.flac` hasta la zona "Drop audio here" → suelta → el clip se carga igual que con Browse.
2. Arrastrar un asset `.fbx` sobre la misma zona → confirmar que también funciona como fuente de drag (no se rompió el caso Mesh) pero que soltarlo sobre la zona de Audio Clip no lo asigna como AudioClip (extensión rechazada, mensaje de error).
3. Arrastrar un asset de imagen (`.png`) → no debe iniciar drag (no está en `kDraggableExt`).
4. Con un GameObject que ya tiene AudioClip (zona de drop no visible), confirmar que no hay forma de soltar un segundo audio sobre él sin antes pulsar "x".
5. Repetir el ciclo completo: drag&drop para asignar → Play/Stop/Loop/Is 3D? → "x" para quitar → confirmar que no crashea en ningún punto.
6. Repetir la prueba de borrado del GameObject completo (Step 11 de Task 3) tras haber asignado el AudioClip vía drag&drop.

- [ ] **Step 5: Commit**

```bash
git add engine/src/EditorUI.cpp
git commit -m "feat(editor): drag&drop de audio desde Content Browser a la sección Audio Clip"
```
