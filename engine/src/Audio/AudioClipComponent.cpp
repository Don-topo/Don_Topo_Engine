#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Audio/AudioManager.h"

#include <algorithm>
#include <cmath>

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
    if (m_audio) m_audio->playSound(m_soundId, worldPos, m_volume, m_pitch);
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

void AudioClipComponent::reload()
{
    if (!m_audio) return;
    m_audio->unloadSound(m_soundId);
    m_soundId = m_audio->loadSound(m_path, m_is3D, m_loop);
}

} // namespace DonTopo
