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

    // Volumen y pitch son propiedades del CANAL, no del FMOD_MODE del sonido:
    // a diferencia de setLoop/setIs3D, no recargan nada y se pueden mover
    // mientras suena. El clamp vive aquí para que ni la UI ni Lua puedan
    // colar un valor fuera de rango.
    void setVolume(float volume);   // [0, 1]
    void setPitch (float pitch);    // [0.5, 2]

    // Atenuación del clip 3D: a menos de minDistance del listener suena a
    // volumen pleno, y de ahí hasta maxDistance va cayendo. Como volumen y
    // pitch, no recargan el sonido. No hacen nada por FMOD si el clip es 2D
    // (el valor sí se guarda: al marcar is3D se aplica sin perder lo editado).
    void setMinDistance(float d);   // [0.1, 50]
    void setMaxDistance(float d);   // [1, 1000], nunca por debajo de min

    bool getLoop() const  { return m_loop; }
    bool getIs3D() const  { return m_is3D; }
    const std::string& getPath() const { return m_path; }
    float getVolume() const { return m_volume; }
    float getPitch()  const { return m_pitch;  }
    float getMinDistance() const { return m_minDistance; }
    float getMaxDistance() const { return m_maxDistance; }

    // Si está activo, Play Mode llama play() automáticamente al entrar
    // (ver EditorUI::drawToolbar). No afecta al FMOD_MODE, no hace falta reload.
    bool getPlayOnAwake() const { return m_playOnAwake; }
    void setPlayOnAwake(bool playOnAwake) { m_playOnAwake = playOnAwake; }
    // Actualiza solo el bookkeeping del path (ej. tras un rename en disco);
    // el sonido FMOD ya cargado no cambia de contenido, no hace falta reload.
    void setPath(const std::string& path) { m_path = path; }

private:
    void reload();
    // Empuja min/max al sonido FMOD. La llaman los dos setters, el constructor
    // y reload(): un sonido recién creado arranca con el min/max por defecto de
    // FMOD (1 / 10000), no con el de este componente.
    void applyDistances();

    AudioManager* m_audio;
    std::string   m_path;
    int           m_soundId;
    bool          m_is3D;
    bool          m_loop;
    bool          m_playOnAwake = false;
    float         m_volume = 1.0f;
    float         m_pitch  = 1.0f;
    // Defaults a la escala de este repo (primitivas de 50 unidades), no a los
    // de FMOD.
    float         m_minDistance = 1.0f;
    float         m_maxDistance = 100.0f;
};

} // namespace DonTopo
