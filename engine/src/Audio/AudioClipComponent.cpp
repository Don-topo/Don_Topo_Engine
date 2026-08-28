#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Audio/AudioManager.h"

#include <algorithm>
#include <cmath>

namespace DonTopo {

AudioClipComponent::AudioClipComponent(AudioManager* audio, std::string path, int soundId, bool is3D, bool loop,
                                        AudioLoadMode loadMode)
    : m_audio(audio)
    , m_path(std::move(path))
    , m_soundId(soundId)
    , m_is3D(is3D)
    , m_loop(loop)
    , m_loadMode(loadMode)
{
    applyDistances();
}

AudioClipComponent::~AudioClipComponent()
{
    if (m_audio) m_audio->unloadSound(m_soundId);
}

void AudioClipComponent::play(const glm::vec3& worldPos)
{
    // Las distancias viajan con la llamada, no se escriben en el FMOD::Sound:
    // desde que el sonido se comparte entre clips (caché por path+modo),
    // escribirlas allí le cambiaría el radio de atenuación a todos los objetos
    // que usen el mismo fichero.
    if (m_audio)
        m_audio->playSound(m_soundId, worldPos, m_volume, m_pitch, m_bus,
                           m_minDistance, m_maxDistance,
                           m_spread, m_stereoPan, m_dopplerLevel);
}

void AudioClipComponent::stop()
{
    if (m_audio) m_audio->stopSound(m_soundId);
}

void AudioClipComponent::playOneShot(const glm::vec3& worldPos)
{
    // Con distancias, como play(): la voz del one-shot no se guarda en ningún
    // sitio, así que si no se las damos al arrancar se queda con las de fábrica
    // de FMOD (1 / 10000) y un disparo 3D se oiría igual de fuerte a 900
    // unidades que a 5.
    if (m_audio)
        m_audio->playSoundOneShot(m_soundId, worldPos, m_volume, m_pitch, m_bus,
                                  m_minDistance, m_maxDistance,
                                  m_spread, m_stereoPan, m_dopplerLevel);
}

bool AudioClipComponent::isPlaying() const
{
    return m_audio && m_audio->isSoundPlaying(m_soundId);
}

bool AudioClipComponent::isPaused() const
{
    return m_audio && m_audio->isSoundPaused(m_soundId);
}

void AudioClipComponent::pause()
{
    if (m_audio) m_audio->setSoundPaused(m_soundId, true);
}

void AudioClipComponent::resume()
{
    if (m_audio) m_audio->setSoundPaused(m_soundId, false);
}

void AudioClipComponent::updateSpatial(const glm::vec3& worldPos, float dt)
{
    // El gate por m_is3D es local (un bool), así que un clip 2D ni siquiera
    // entra en AudioManager: esto se llama por frame y por cada clip de la
    // escena, y la mayoría son 2D.
    if (!m_audio || !m_is3D) return;
    m_audio->setSoundPosition(m_soundId, worldPos, dt);
}

bool AudioClipComponent::hasLoadError() const
{
    return m_audio && m_audio->getSoundState(m_soundId) == AudioManager::SoundLoadState::Failed;
}

void AudioClipComponent::setLoop(bool loop)
{
    if (loop == m_loop) return;
    m_loop = loop;
    reload();
}

void AudioClipComponent::setLoadMode(AudioLoadMode mode)
{
    if (mode == m_loadMode) return;
    m_loadMode = mode;
    reload();
}

void AudioClipComponent::setRolloff(AudioRolloff rolloff)
{
    if (rolloff == m_rolloff) return;
    m_rolloff = rolloff;
    reload();
}

// Los tres siguientes NO recargan: son de la voz. Mismo guard de no-finitos que
// volume/pitch — un NaN aqui acabaria en el .scene como "null".
void AudioClipComponent::setSpread(float degrees)
{
    if (!std::isfinite(degrees)) return;
    m_spread = std::clamp(degrees, 0.0f, 360.0f);
}

void AudioClipComponent::setStereoPan(float pan)
{
    if (!std::isfinite(pan)) return;
    m_stereoPan = std::clamp(pan, -1.0f, 1.0f);
}

void AudioClipComponent::setDopplerLevel(float level)
{
    if (!std::isfinite(level)) return;
    m_dopplerLevel = std::clamp(level, 0.0f, 5.0f);
}

void AudioClipComponent::setIs3D(bool is3D)
{
    if (is3D == m_is3D) return;
    m_is3D = is3D;
    reload();
}

void AudioClipComponent::setVolume(float volume)
{
    // std::clamp(NaN, lo, hi) devuelve NaN: toda comparación con NaN es
    // falsa, así que el clamp de abajo NO lo detiene (un infinito, en
    // cambio, sí se clampa bien: clamp(+inf,0,1) == 1.0 — el peligroso de
    // verdad es el NaN). Un NaN aquí acaba serializado en el .scene como
    // "null" (nlohmann no tiene forma de escribir NaN) y esa es la cadena
    // que tumbaba Scene::fromJson entero por un solo campo corrupto (ver
    // Scene.cpp). Se rechaza aquí, antes del clamp, conservando el valor
    // anterior. Sin log: este componente no tiene canal al Log Console.
    // Cuando la llamada viene de Lua (ScriptBindings.cpp), el binding SÍ
    // avisa antes de llegar aquí — pero este setter también se llama
    // directamente sin pasar por Lua: el slider de Volume del Inspector
    // (PropertiesPanel.cpp) y el apply() del Undo/Redo de ese mismo slider
    // llaman a setVolume/setPitch a pelo. Por esas dos rutas un valor
    // corrupto se descarta aquí SIN ningún feedback al usuario (ni Log ni
    // UI) — no hay contradicción con el guard, solo una asimetría de canal
    // de aviso pendiente de resolver si algún día el slider necesita avisar.
    if (!std::isfinite(volume)) return;
    m_volume = std::clamp(volume, 0.0f, 1.0f);
    if (m_audio) m_audio->setChannelVolume(m_soundId, m_volume);
}

void AudioClipComponent::setPitch(float pitch)
{
    // Mismo razonamiento que setVolume: NaN se cuela por el clamp, Inf no.
    if (!std::isfinite(pitch)) return;
    m_pitch = std::clamp(pitch, 0.5f, 2.0f);
    if (m_audio) m_audio->setChannelPitch(m_soundId, m_pitch);
}

void AudioClipComponent::setMinDistance(float d)
{
    // Mismo razonamiento que setVolume con el NaN: se rechaza antes del clamp.
    if (!std::isfinite(d)) return;
    m_minDistance = std::clamp(d, 0.1f, 50.0f);
    // El invariante min <= max vive aquí, no en la UI: un .scene editado a mano
    // tampoco puede instalar una atenuación invertida.
    if (m_maxDistance < m_minDistance) m_maxDistance = m_minDistance;
    applyDistances();
}

void AudioClipComponent::setMaxDistance(float d)
{
    if (!std::isfinite(d)) return;
    m_maxDistance = std::clamp(d, 1.0f, 1000.0f);
    if (m_maxDistance < m_minDistance) m_minDistance = m_maxDistance;
    applyDistances();
}

void AudioClipComponent::applyDistances()
{
    // El no-op en 2D lo decide AudioManager mirando el FMOD_MODE del sonido:
    // así el valor guardado aquí no se pierde al alternar is3D.
    if (m_audio) m_audio->setSound3DMinMaxDistance(m_soundId, m_minDistance, m_maxDistance);
}

void AudioClipComponent::reload()
{
    if (!m_audio) return;
    m_audio->unloadSound(m_soundId);
    m_soundId = m_audio->loadSound(m_path, m_is3D, m_loop, m_loadMode, m_rolloff);
    // El sonido nuevo arranca con el min/max por defecto de FMOD: hay que
    // reescribirle el del componente (importa al pasar de 2D a 3D).
    applyDistances();
}

} // namespace DonTopo
