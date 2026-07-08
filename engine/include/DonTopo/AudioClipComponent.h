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
