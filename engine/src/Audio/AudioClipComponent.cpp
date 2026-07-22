#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Audio/AudioManager.h"

#include <algorithm>

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

void AudioClipComponent::setVolume(float volume)
{
    m_volume = std::clamp(volume, 0.0f, 1.0f);
}

void AudioClipComponent::setPitch(float pitch)
{
    m_pitch = std::clamp(pitch, 0.5f, 2.0f);
}

void AudioClipComponent::reload()
{
    if (!m_audio) return;
    m_audio->unloadSound(m_soundId);
    m_soundId = m_audio->loadSound(m_path, m_is3D, m_loop);
}

} // namespace DonTopo
