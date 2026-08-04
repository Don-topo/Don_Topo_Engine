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

    // Retorna al instante: FMOD carga en su hilo interno (FMOD_NONBLOCKING). El
    // id es válido desde ya, pero un playSound antes de que termine la carga no
    // reproduce nada — no es un error, solo aún no está listo.
    int  loadSound(const std::string& path, bool is3D = true, bool loop = false);
    void unloadSound(int soundId);
    int  loadBGM(const std::string& path);

    void playSound(int soundId, const glm::vec3& worldPos = {},
                   float volume = 1.0f, float pitch = 1.0f);
    void stopSound(int soundId);

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

    // Atenuación 3D del sonido: por debajo de minDistance suena a volumen
    // pleno, y de ahí a maxDistance va cayendo. Se escribe en el FMOD::Sound
    // (vale pa las reproducciones futuras) Y en el canal vivo si lo hay, con la
    // misma comprobación que los dos setters de arriba. No-op si el sonido no
    // se cargó con FMOD_3D: en 2D no hay atenuación que ajustar.
    void setSound3DMinMaxDistance(int soundId, float minDistance, float maxDistance);

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
