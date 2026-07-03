# FMOD Audio Module Design

> **For agentic workers:** Use superpowers:writing-plans then superpowers:executing-plans to implement this spec.

**Goal:** Add a full-featured `AudioManager` that wraps FMOD Core API — 2D/3D SFX, streaming BGM, per-group volume control. Compiles as no-op when FMOD is not installed.

**Architecture:** Standalone `AudioManager` class, mirroring the `Renderer` pattern. Sandbox creates it directly. The entire FMOD-specific implementation is guarded by `#ifdef DT_FMOD_ENABLED`.

**Tech Stack:** C++20, FMOD Core API (fmod.hpp), CMake `find_package(FMOD)` already in root CMakeLists.

---

## Global Constraints

- `AudioManager` compiles and links whether or not FMOD is installed (no-op when missing)
- No FMOD headers leak into public `AudioManager.h` — `fmod.hpp` included only in `AudioManager.cpp`
- All methods are no-ops and return safe defaults when `DT_FMOD_ENABLED` is not defined
- `DT_FMOD_ENABLED` compile definition already set by `engine/CMakeLists.txt` when FMOD found

---

## FMOD Setup — descarga de la API

FMOD requiere cuenta gratuita. La descarga no puede automatizarse vía FetchContent (requiere autenticación). Flujo manual único:

1. Crear cuenta en [fmod.com/download](https://www.fmod.com/download) (gratuita para uso no comercial / ingresos < $200k/año)
2. Descargar **FMOD Studio API — Windows** → elegir la versión **zip** (no el installer)
3. Extraer el zip → aparece una carpeta con estructura `api/core/inc/`, `api/core/lib/x64/`, etc.
4. Mover/renombrar esa carpeta a **`<raíz del proyecto>/third_party/fmod/`**
5. Re-ejecutar CMake — `FindFMOD.cmake` busca `third_party/fmod/` como primera opción; `FMOD::FMOD` se crea automáticamente
6. `fmod.dll` se copia al directorio del ejecutable en cada build vía `POST_BUILD` (ver sección CMake)

Estructura esperada tras el paso 4:

```text
third_party/
  fmod/
    api/
      core/
        inc/        ← fmod.hpp, fmod_errors.h, ...
        lib/
          x64/      ← fmod_vc.lib, fmod.dll
          x86/      ← ...
```

`third_party/fmod/` debe añadirse a `.gitignore` (binarios propietarios, no van al repo).

---

## AudioManager API

**File:** `engine/include/DonTopo/AudioManager.h`

No FMOD types in the public header — only standard types and GLM.

```cpp
#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace DonTopo {

class AudioManager {
public:
    AudioManager() = default;
    ~AudioManager();
    AudioManager(const AudioManager&)            = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    void init();
    // Called once per frame: updates 3D listener position
    void update(const glm::vec3& listenerPos,
                const glm::vec3& listenerForward,
                const glm::vec3& listenerUp);
    void shutdown();

    // Load a one-shot or looping sound effect.
    // is3D=true: sound attenuates with distance; worldPos passed to playSound().
    // Returns a sound ID (≥0), or -1 on failure.
    int  loadSound(const std::string& path, bool is3D = true);

    // Load BGM as a streaming file (FMOD_CREATESTREAM).
    // Returns a BGM slot ID (≥0), or -1 on failure.
    int  loadBGM(const std::string& path);

    // Play a previously loaded SFX at a world position (ignored for 2D sounds).
    void playSound(int soundId, const glm::vec3& worldPos = {});

    // Play BGM slot, stopping any currently playing BGM first.
    void playBGM(int bgmId);
    void stopBGM();
    void pauseBGM(bool paused);

    // Volume controls (0.0 – 1.0). Applied to channel groups.
    void setMasterVolume(float v);
    void setSfxVolume   (float v);
    void setBgmVolume   (float v);

private:
#ifdef DT_FMOD_ENABLED
    // Forward-declared as void* to avoid including fmod.hpp in the header.
    // Cast to FMOD::System* / FMOD::ChannelGroup* / FMOD::Sound* in the .cpp.
    void* m_system   = nullptr;  // FMOD::System*
    void* m_sfxGroup = nullptr;  // FMOD::ChannelGroup*
    void* m_bgmGroup = nullptr;  // FMOD::ChannelGroup*
    void* m_bgmCh    = nullptr;  // FMOD::Channel* (currently playing BGM)
    std::vector<void*> m_sounds;    // FMOD::Sound* — SFX clips
    std::vector<void*> m_bgmSounds; // FMOD::Sound* — BGM streams
#endif
};

} // namespace DonTopo
```

**Implementation file:** `engine/src/AudioManager.cpp`

```cpp
#include "DonTopo/AudioManager.h"

#ifdef DT_FMOD_ENABLED
#include <fmod.hpp>
#include <fmod_errors.h>
#include <stdexcept>

// Helper cast macros (private to this TU)
#define SYS   reinterpret_cast<FMOD::System*>(m_system)
#define SFXG  reinterpret_cast<FMOD::ChannelGroup*>(m_sfxGroup)
#define BGMG  reinterpret_cast<FMOD::ChannelGroup*>(m_bgmGroup)
#define BGMCH reinterpret_cast<FMOD::Channel*>(m_bgmCh)

static void fmodCheck(FMOD_RESULT r, const char* ctx) {
    if (r != FMOD_OK)
        throw std::runtime_error(std::string(ctx) + ": " + FMOD_ErrorString(r));
}
#endif

namespace DonTopo {

AudioManager::~AudioManager() { shutdown(); }

void AudioManager::init()
{
#ifdef DT_FMOD_ENABLED
    FMOD::System* sys;
    fmodCheck(FMOD::System_Create(&sys), "FMOD::System_Create");
    fmodCheck(sys->init(512, FMOD_INIT_NORMAL | FMOD_INIT_3D_RIGHTHANDED, nullptr), "FMOD init");

    FMOD::ChannelGroup* sfx;
    FMOD::ChannelGroup* bgm;
    fmodCheck(sys->createChannelGroup("SFX", &sfx), "createChannelGroup SFX");
    fmodCheck(sys->createChannelGroup("BGM", &bgm), "createChannelGroup BGM");

    m_system   = sys;
    m_sfxGroup = sfx;
    m_bgmGroup = bgm;
#endif
}

void AudioManager::update(const glm::vec3& pos, const glm::vec3& fwd, const glm::vec3& up)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system) return;
    FMOD_VECTOR p   = { pos.x, pos.y, pos.z };
    FMOD_VECTOR vel = { 0,0,0 };
    FMOD_VECTOR f   = { fwd.x, fwd.y, fwd.z };
    FMOD_VECTOR u   = { up.x,  up.y,  up.z  };
    SYS->set3DListenerAttributes(0, &p, &vel, &f, &u);
    SYS->update();
#endif
}

void AudioManager::shutdown()
{
#ifdef DT_FMOD_ENABLED
    for (auto* s : m_sounds)    if (s) reinterpret_cast<FMOD::Sound*>(s)->release();
    for (auto* s : m_bgmSounds) if (s) reinterpret_cast<FMOD::Sound*>(s)->release();
    m_sounds.clear(); m_bgmSounds.clear();
    if (SFXG) SFXG->release();
    if (BGMG) BGMG->release();
    if (SYS)  { SYS->close(); SYS->release(); }
    m_system = m_sfxGroup = m_bgmGroup = m_bgmCh = nullptr;
#endif
}

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

int AudioManager::loadBGM(const std::string& path)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system) return -1;
    FMOD::Sound* snd;
    FMOD_MODE mode = FMOD_2D | FMOD_LOOP_NORMAL | FMOD_CREATESTREAM;
    if (SYS->createSound(path.c_str(), mode, nullptr, &snd) != FMOD_OK) return -1;
    m_bgmSounds.push_back(snd);
    return (int)m_bgmSounds.size() - 1;
#else
    return -1;
#endif
}

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
        FMOD_VECTOR v = { 0,0,0 };
        ch->set3DAttributes(&p, &v);
    }
#endif
}

void AudioManager::playBGM(int bgmId)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || bgmId < 0 || bgmId >= (int)m_bgmSounds.size()) return;
    stopBGM();
    FMOD::Channel* ch;
    SYS->playSound(reinterpret_cast<FMOD::Sound*>(m_bgmSounds[bgmId]), BGMG, false, &ch);
    m_bgmCh = ch;
#endif
}

void AudioManager::stopBGM()
{
#ifdef DT_FMOD_ENABLED
    if (BGMCH) { BGMCH->stop(); m_bgmCh = nullptr; }
#endif
}

void AudioManager::pauseBGM(bool paused)
{
#ifdef DT_FMOD_ENABLED
    if (BGMCH) BGMCH->setPaused(paused);
#endif
}

void AudioManager::setMasterVolume(float v)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system) return;
    FMOD::ChannelGroup* master;
    SYS->getMasterChannelGroup(&master);
    master->setVolume(v);
#endif
}

void AudioManager::setSfxVolume(float v)
{
#ifdef DT_FMOD_ENABLED
    if (SFXG) SFXG->setVolume(v);
#endif
}

void AudioManager::setBgmVolume(float v)
{
#ifdef DT_FMOD_ENABLED
    if (BGMG) BGMG->setVolume(v);
#endif
}

} // namespace DonTopo
```

---

## CMake Changes

### `engine/CMakeLists.txt` — add AudioManager source

```cmake
add_library(DonTopoEngine STATIC
    src/Engine.cpp
    src/Window.cpp
    src/Renderer.cpp
    src/ModelLoader.cpp
    src/Camera.cpp
    src/SceneNode.cpp
    src/AudioManager.cpp   # NEW
)
```

### `sandbox/CMakeLists.txt` — copy fmod.dll on Windows POST_BUILD

Add after the existing `add_custom_command` blocks:

```cmake
# Copy FMOD runtime DLL next to the executable (Windows only)
if(FMOD_FOUND AND WIN32)
    get_filename_component(_fmod_lib_dir "${FMOD_LIBRARY}" DIRECTORY)
    set(_fmod_dll "${_fmod_lib_dir}/fmod.dll")
    if(EXISTS "${_fmod_dll}")
        add_custom_command(TARGET Sandbox POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_fmod_dll}"
                "$<TARGET_FILE_DIR:Sandbox>/fmod.dll"
            COMMENT "Copying fmod.dll to output"
        )
    endif()
endif()
```

`FMOD_LIBRARY` is set by `FindFMOD.cmake`. On a default install the DLL lives alongside `fmod_vc.lib` in `api/core/lib/x64/`, so the `get_filename_component(DIRECTORY)` resolves correctly.

---

## File Changes Summary

| Action | File |
|--------|------|
| Create | `engine/include/DonTopo/AudioManager.h` |
| Create | `engine/src/AudioManager.cpp` |
| Modify | `engine/CMakeLists.txt` — add `src/AudioManager.cpp` |
| Modify | `sandbox/CMakeLists.txt` — add fmod.dll POST_BUILD copy |

---

## Sandbox Usage Example

```cpp
#include "DonTopo/AudioManager.h"

DonTopo::AudioManager audio;
audio.init();

int bgm  = audio.loadBGM("assets/audio/theme.mp3");
int step = audio.loadSound("assets/audio/footstep.wav", /*is3D=*/true);

audio.setBgmVolume(0.4f);
audio.playBGM(bgm);

// Per frame:
audio.update(cameraPos, cameraForward, cameraUp);

// On event:
audio.playSound(step, characterWorldPos);
```
