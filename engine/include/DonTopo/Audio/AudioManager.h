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
