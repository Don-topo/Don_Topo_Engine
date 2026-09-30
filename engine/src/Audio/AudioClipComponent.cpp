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
    // The distances travel with the call, they are not written to the FMOD::Sound:
    // since the sound is shared between clips (cache by path+mode),
    // writing them there would change the attenuation radius of all the objects
    // that use the same file.
    if (!m_audio) return;
    m_audio->playSound(m_soundId, worldPos, m_volume, m_pitch, m_bus,
                       m_minDistance, m_maxDistance,
                       m_spread, m_stereoPan, m_dopplerLevel);
    // After starting the voice: this way an object saved as muted stays muted
    // when played, instead of sounding until someone mutes it again.
    if (m_mute) m_audio->setSoundMute(m_soundId, true);
}

void AudioClipComponent::stop()
{
    if (m_audio) m_audio->stopSound(m_soundId);
}

void AudioClipComponent::playOneShot(const glm::vec3& worldPos)
{
    // Two things go together here because they come from the same fact: the voice of a
    // one-shot is not stored anywhere.
    //
    // 1) The distances have to travel in the call, or the shot ends up
    //    with FMOD's factory ones (1 / 10000) and would sound just as loud at
    //    900 units as at 5.
    // 2) A muted clip does not even fire: there would be no way to silence that voice
    //    afterwards, so leaving here is the only thing that respects the mute.
    if (!m_audio || m_mute) return;
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

void AudioClipComponent::setMute(bool mute)
{
    m_mute = mute;
    // It is pushed to the live voice so that the change is heard right away; and it also travels in
    // play(), so that a new playback is born muted if applicable.
    if (m_audio) m_audio->setSoundMute(m_soundId, mute);
}

float AudioClipComponent::getTime() const
{
    return m_audio ? m_audio->getSoundTime(m_soundId) : -1.0f;
}

void AudioClipComponent::setTime(float seconds)
{
    if (m_audio) m_audio->setSoundTime(m_soundId, seconds);
}

void AudioClipComponent::updateSpatial(const glm::vec3& worldPos, float dt)
{
    // The gate by m_is3D is local (a bool), so a 2D clip does not even
    // enter AudioManager: this is called per frame and for every clip of the
    // scene, and most are 2D.
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

// The following three do NOT reload: they belong to the voice. Same non-finite guard as
// volume/pitch — a NaN here would end up in the .scene as "null".
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
    // std::clamp(NaN, lo, hi) returns NaN: every comparison with NaN is
    // false, so the clamp below does NOT stop it (an infinity, on the other
    // hand, is clamped fine: clamp(+inf,0,1) == 1.0 — the truly dangerous one
    // is NaN). A NaN here ends up serialized in the .scene as
    // "null" (nlohmann has no way to write NaN) and that is the chain
    // that took down the whole Scene::fromJson over a single corrupt field (see
    // Scene.cpp). It is rejected here, before the clamp, keeping the previous
    // value. No log: this component has no channel to the Log Console.
    // When the call comes from Lua (ScriptBindings.cpp), the binding DOES
    // warn before getting here — but this setter is also called
    // directly without going through Lua: the Volume slider of the Inspector
    // (PropertiesPanel.cpp) and the apply() of the Undo/Redo of that same slider
    // call setVolume/setPitch directly. Through those two paths a corrupt
    // value is discarded here WITHOUT any feedback to the user (neither Log nor
    // UI) — there is no contradiction with the guard, just an asymmetry of warning
    // channel pending to be resolved if some day the slider needs to warn.
    if (!std::isfinite(volume)) return;
    m_volume = std::clamp(volume, 0.0f, 1.0f);
    if (m_audio) m_audio->setChannelVolume(m_soundId, m_volume);
}

void AudioClipComponent::setPitch(float pitch)
{
    // Same reasoning as setVolume: NaN slips through the clamp, Inf does not.
    if (!std::isfinite(pitch)) return;
    m_pitch = std::clamp(pitch, 0.5f, 2.0f);
    if (m_audio) m_audio->setChannelPitch(m_soundId, m_pitch);
}

void AudioClipComponent::setMinDistance(float d)
{
    // Same reasoning as setVolume with NaN: it is rejected before the clamp.
    if (!std::isfinite(d)) return;
    m_minDistance = std::clamp(d, 0.1f, 50.0f);
    // The invariant min <= max lives here, not in the UI: a hand-edited .scene
    // cannot install an inverted attenuation either.
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
    // The no-op in 2D is decided by AudioManager looking at the sound's FMOD_MODE:
    // this way the value stored here is not lost when toggling is3D.
    if (m_audio) m_audio->setSound3DMinMaxDistance(m_soundId, m_minDistance, m_maxDistance);
}

void AudioClipComponent::reload()
{
    if (!m_audio) return;
    m_audio->unloadSound(m_soundId);
    m_soundId = m_audio->loadSound(m_path, m_is3D, m_loop, m_loadMode, m_rolloff);
    // The new sound starts with FMOD's default min/max: the component's
    // has to be rewritten onto it (it matters when going from 2D to 3D).
    applyDistances();
}

} // namespace DonTopo
