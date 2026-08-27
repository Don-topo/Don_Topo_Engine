#pragma once
#include <string>
#include <glm/glm.hpp>

#include "DonTopo/Audio/AudioBus.h"

namespace DonTopo {

class AudioManager;

// Componente único de audio por GameObject. Envuelve un soundId de
// AudioManager; loop/is3D van horneados en el FMOD_MODE del sonido, así
// que cambiarlos recarga el clip (unloadSound + loadSound) en vez de
// mutar el sonido existente.
class AudioClipComponent {
public:
    AudioClipComponent(AudioManager* audio, std::string path, int soundId, bool is3D, bool loop,
                        AudioLoadMode loadMode = AudioLoadMode::Sample);
    ~AudioClipComponent();

    AudioClipComponent(const AudioClipComponent&)            = delete;
    AudioClipComponent& operator=(const AudioClipComponent&) = delete;

    void play(const glm::vec3& worldPos);
    void stop();

    // Dispara una voz suelta que se solapa con lo que ya suene, en vez de
    // cortarlo como hace play(). Usa el volumen y el pitch del componente, pero
    // la voz resultante queda fuera de su alcance: stop(), setVolume/setPitch e
    // isPlaying() no la ven, y el seguimiento 3D por frame tampoco. Para clips
    // cortos (pasos, disparos, impactos), no para loops.
    void playOneShot(const glm::vec3& worldPos);

    // Estado de la voz, no del componente: no se serializa ni sobrevive a un
    // stop. isPlaying() sigue el criterio de FMOD y de Unity — una voz pausada
    // cuenta como sonando; isPaused() es lo que las distingue.
    bool isPlaying() const;
    bool isPaused()  const;
    // Conservan la posición de reproducción, al revés que stop().
    void pause();
    void resume();

    // Empuja la posición del dueño a la voz que esté sonando, para que un clip
    // 3D siga al GameObject en vez de quedarse donde estaba al llamar a play().
    // Pensado para llamarse una vez por frame (lo hace Scene::update); no-op
    // barato si el clip es 2D o si no hay nada sonando.
    void updateSpatial(const glm::vec3& worldPos);

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

    // El fichero no se pudo abrir (ausente, formato no soportado, datos
    // corruptos). No se sabe al construir el componente: FMOD carga en su hilo
    // y el fallo aparece frames después, así que esto se consulta, no se
    // cachea. Definido en el .cpp para no arrastrar AudioManager.h al header.
    bool hasLoadError() const;

    // Bus de salida. No toca el FMOD_MODE, así que cambiarlo NO recarga el
    // sonido — pero sí es propiedad del canal: solo surte efecto en la próxima
    // reproducción, porque el grupo se elige al arrancar la voz.
    AudioBus getBus() const { return m_bus; }
    void setBus(AudioBus bus) { m_bus = bus; }

    // Modo de carga. Como loop e is3D, va horneado en el FMOD_MODE del sonido:
    // cambiarlo RECARGA el clip y corta lo que estuviera sonando.
    AudioLoadMode getLoadMode() const { return m_loadMode; }
    void setLoadMode(AudioLoadMode mode);

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
    // Sfx por defecto: es por donde salía TODO antes de que existieran los
    // buses, así que una escena vieja suena igual.
    AudioBus      m_bus = AudioBus::Sfx;
    // Sample por defecto: es como se cargaba TODO antes de que se pudiera
    // elegir, asi que una escena vieja se comporta igual.
    AudioLoadMode m_loadMode = AudioLoadMode::Sample;
    float         m_volume = 1.0f;
    float         m_pitch  = 1.0f;
    // Defaults a la escala de este repo (primitivas de 50 unidades), no a los
    // de FMOD.
    float         m_minDistance = 1.0f;
    float         m_maxDistance = 100.0f;
};

} // namespace DonTopo
