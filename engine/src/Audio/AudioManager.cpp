#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Audio/AudioClipComponent.h"

#ifdef DT_FMOD_ENABLED
#include <fmod.hpp>
#include <fmod_errors.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>

#define SYS   reinterpret_cast<FMOD::System*>(m_system)
#define SFXG  reinterpret_cast<FMOD::ChannelGroup*>(m_sfxGroup)
#define MUSICG reinterpret_cast<FMOD::ChannelGroup*>(m_musicGroup)

static void fmodCheck(FMOD_RESULT r, const char* ctx) {
    if (r != FMOD_OK)
        throw std::runtime_error(std::string(ctx) + ": " + FMOD_ErrorString(r));
}
#endif

namespace DonTopo {

AudioManager::~AudioManager() { shutdown(); }

std::string AudioManager::outputWarningFor(int fmodOutputType)
{
#ifdef DT_FMOD_ENABLED
    if (fmodOutputType == FMOD_OUTPUTTYPE_NOSOUND)
        return "Audio without output: FMOD found no sound device and nothing will "
               "play. On Linux, libpulse0 or libasound2t64 is missing.";
#endif
    (void)fmodOutputType;
    return {};
}

bool AudioManager::init()
{
#ifdef DT_FMOD_ENABLED
    // Reentrancy: without this guard, a second init() overwrote the three pointers
    // leaving the previous System and ChannelGroups alive and ownerless.
    if (m_system) return true;

    FMOD::System* sys = nullptr;
    try
    {
        fmodCheck(FMOD::System_Create(&sys), "FMOD::System_Create");
        fmodCheck(sys->init(512, FMOD_INIT_NORMAL | FMOD_INIT_3D_RIGHTHANDED, nullptr), "FMOD init");

        FMOD::ChannelGroup* sfx;
        FMOD::ChannelGroup* music;
        fmodCheck(sys->createChannelGroup("SFX", &sfx), "createChannelGroup SFX");
        fmodCheck(sys->createChannelGroup("Music", &music), "createChannelGroup Music");

        m_system   = sys;
        m_sfxGroup = sfx;
        m_musicGroup = music;

        // init() "works" even if there is nowhere to send the sound: FMOD falls back to
        // NOSOUND silently. The chosen output is queried so that it can be warned about.
        FMOD_OUTPUTTYPE salida = FMOD_OUTPUTTYPE_AUTODETECT;
        if (sys->getOutput(&salida) == FMOD_OK)
            m_outputWarning = outputWarningFor((int)salida);
        if (!m_outputWarning.empty())
            std::cerr << m_outputWarning << std::endl;
        return true;
    }
    catch (const std::exception& e)
    {
        // m_system is assigned at the END, so until here it is still nullptr and
        // shutdown() would exit through its guard without releasing anything: the created System
        // stayed unclosed forever if any step after System_Create
        // failed. release() closes and frees.
        if (sys) sys->release();
        // With no output device (or with FMOD badly installed) this BEFORE
        // propagated the exception up to the catch in main and the editor did not
        // start. Now the engine stays alive, muted, and says so once. There is no
        // channel to the Log Console from here (same reason documented in
        // AudioClipComponent::setVolume); hosts that want to warn in their UI
        // have the return bool and available().
        std::cerr << "Audio disabled: " << e.what() << std::endl;
        return false;
    }
#else
    return false;
#endif
}

bool AudioManager::available() const
{
#ifdef DT_FMOD_ENABLED
    return m_system != nullptr;
#else
    return false;
#endif
}

#ifdef DT_FMOD_ENABLED
namespace {
// Speed cap, in world units per second. Above this the data is not
// believed: a teleport, a scene load or a very long frame would give
// an absurd velocity, and the doppler would turn it into a screech that lasts as
// long as the voice. The primitives of this repo measure 50 units, so 2000
// u/s is very fast but still plausible for a projectile.
constexpr float kMaxSourceSpeed = 2000.0f;

// Velocity between two positions, or zero if it is not trustworthy (non-positive dt,
// first frame, or a jump too large to be real movement).
glm::vec3 safeVelocity(const glm::vec3& current, const glm::vec3& last, bool hasLast, float dt)
{
    if (!hasLast || dt <= 0.0f) return glm::vec3(0.0f);
    const glm::vec3 v = (current - last) / dt;
    const float speed = glm::length(v);
    if (!std::isfinite(speed) || speed > kMaxSourceSpeed) return glm::vec3(0.0f);
    return v;
}
} // namespace
#endif

void AudioManager::update(const glm::vec3& pos, const glm::vec3& fwd, const glm::vec3& up, float dt)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system) return;
    // The listener velocity is half of the doppler (the other half is that of each
    // source). Before it was fixed at zero, so there was no effect no matter how much
    // the camera moved.
    const glm::vec3 v = safeVelocity(pos, m_lastListenerPos, m_hasLastListenerPos, dt);
    m_lastListenerPos = pos;
    m_hasLastListenerPos = true;

    FMOD_VECTOR p   = { pos.x, pos.y, pos.z };
    FMOD_VECTOR vel = { v.x, v.y, v.z };
    FMOD_VECTOR f   = { fwd.x, fwd.y, fwd.z };
    FMOD_VECTOR u   = { up.x,  up.y,  up.z  };
    SYS->set3DListenerAttributes(0, &p, &vel, &f, &u);
    SYS->update();
#else
    (void)dt;
#endif
}

void AudioManager::shutdown()
{
#ifdef DT_FMOD_ENABLED
    if (!m_system) return;
    for (auto* s : m_sounds)    if (s) reinterpret_cast<FMOD::Sound*>(s)->release();
    m_sounds.clear(); m_sfxChannels.clear();
    m_soundPaths.clear(); m_soundFailureReported.clear();
    // The cache ones too: a later init() would find the map
    // pointing to slots that no longer exist and would return ids of dead sounds.
    m_soundRefs.clear(); m_soundKeys.clear(); m_freeSlots.clear();
    m_soundByKey.clear(); m_pinnedSounds.clear();
    m_soundLastPos.clear(); m_soundHasLastPos.clear();
    // Parallel to m_sounds: if they stayed, after a later init() the new id 0
    // would inherit the settings of the old sound (and push_back would misalign them).
    m_soundImport.clear(); m_soundVolume.clear();
    // The DSPs BEFORE the groups they hang from: releasing the group first
    // would leave the DSPs hanging from something that no longer exists. They are native resources,
    // not loose pointers.
    for (auto& [key, dsp] : m_busEffects)
        if (dsp) reinterpret_cast<FMOD::DSP*>(dsp)->release();
    m_busEffects.clear();
    // Reverb zones are another native resource with the same treatment.
    clearReverbZones();
    if (SFXG) SFXG->release();
    if (MUSICG) MUSICG->release();
    SYS->close();
    SYS->release();
    m_system = m_sfxGroup = m_musicGroup = nullptr;
#endif
}

#ifdef DT_FMOD_ENABLED
std::string AudioManager::soundKey(const std::string& path, bool is3D, bool loop,
                                    AudioLoadMode loadMode, AudioRolloff rolloff)
{
    // The flags before the path: the path can contain anything, so
    // the separator goes where it cannot collide with its content. The load
    // mode enters the key like is3D and loop: a streamed sound and the
    // same file decompressed in RAM are two different FMOD::Sound.
    // The rolloff is also in the FMOD_MODE, so it enters the key: the
    // same file with a linear curve and with an inverse curve are two sounds.
    return std::string(is3D ? "3" : "2") + (loop ? "L" : "N")
         + (loadMode == AudioLoadMode::Stream ? "S" : "M")
         + audioRolloffToStr(rolloff)[0] + "|" + path;
}
#endif

size_t AudioManager::loadedSoundCount() const
{
#ifdef DT_FMOD_ENABLED
    size_t n = 0;
    for (auto* s : m_sounds) if (s) ++n;
    return n;
#else
    return 0;
#endif
}

size_t AudioManager::soundSlotCount() const
{
#ifdef DT_FMOD_ENABLED
    return m_sounds.size();
#else
    return 0;
#endif
}

bool AudioManager::isSoundStreaming(int id) const
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() || !m_sounds[id]) return false;
    FMOD_MODE mode = 0;
    if (reinterpret_cast<FMOD::Sound*>(m_sounds[id])->getMode(&mode) != FMOD_OK) return false;
    return (mode & FMOD_CREATESTREAM) != 0;
#else
    (void)id;
    return false;
#endif
}

AudioRolloff AudioManager::getSoundRolloff(int id) const
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() || !m_sounds[id])
        return AudioRolloff::Inverse;
    FMOD_MODE mode = 0;
    if (reinterpret_cast<FMOD::Sound*>(m_sounds[id])->getMode(&mode) != FMOD_OK)
        return AudioRolloff::Inverse;
    if (mode & FMOD_3D_LINEARSQUAREROLLOFF) return AudioRolloff::LinearSquare;
    if (mode & FMOD_3D_LINEARROLLOFF)       return AudioRolloff::Linear;
    // Without an explicit flag FMOD uses inverse, which is our default.
    return AudioRolloff::Inverse;
#else
    (void)id;
    return AudioRolloff::Inverse;
#endif
}

int AudioManager::loadSound(const std::string& path, bool is3D, bool loop, AudioLoadMode loadMode,
                             AudioRolloff rolloff)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system) return -1;

    // Is that same file already loaded with the same mode? Then the FMOD::Sound is
    // shared and only the counter goes up. Twenty objects with the same
    // shot were twenty decompressed copies in memory.
    const std::string key = soundKey(path, is3D, loop, loadMode, rolloff);
    if (auto it = m_soundByKey.find(key); it != m_soundByKey.end())
    {
        const int cached = it->second;
        // The map could have a stale entry if something left it uncleaned;
        // the slot is checked before returning it instead of trusting.
        if (cached >= 0 && cached < (int)m_sounds.size() && m_sounds[cached])
        {
            ++m_soundRefs[cached];
            return cached;
        }
        m_soundByKey.erase(it);
    }

    // FMOD starts with FMOD_INIT_NORMAL (line 29), which is the thread-safe API.
    // NONBLOCKING offloads reading and decoding to FMOD's internal thread:
    // createSound returns immediately and we do not write a single line of
    // concurrency code. It is why audio does not go through the JobSystem.
    FMOD_MODE mode = (is3D ? FMOD_3D : FMOD_2D)
                   | (loop ? FMOD_LOOP_NORMAL : FMOD_LOOP_OFF)
                   | FMOD_NONBLOCKING;
    // CREATESTREAM: the file is read and decoded on the fly instead of
    // being fully decompressed into RAM. In exchange, the sound only admits ONE voice at a
    // time (a single decode buffer), which is the reason this
    // mode is for music and not for effects.
    if (loadMode == AudioLoadMode::Stream) mode |= FMOD_CREATESTREAM;
    // Attenuation curve. Without any of these flags FMOD applies the inverse,
    // which is exactly AudioRolloff::Inverse: that is why that case adds nothing.
    if (rolloff == AudioRolloff::Linear)            mode |= FMOD_3D_LINEARROLLOFF;
    else if (rolloff == AudioRolloff::LinearSquare) mode |= FMOD_3D_LINEARSQUAREROLLOFF;
    FMOD::Sound* snd;
    if (SYS->createSound(path.c_str(), mode, nullptr, &snd) != FMOD_OK) return -1;
    // Import settings of the file (sidecar): only on CREATING the sound; a
    // cache hit already returned earlier with its own.
    std::string importWarning;
    const AudioImportSettings importSettings = loadAudioImportSettings(path, &importWarning);
    if (!importWarning.empty())
        std::fprintf(stderr, "[AudioImport] %s: %s\n", path.c_str(), importWarning.c_str());
    // Recycled slot if there is one: ids were never reused and the vectors
    // grew one entry per clip on EVERY Play->Stop cycle (which recreates the
    // whole scene from the snapshot).
    int id;
    if (!m_freeSlots.empty())
    {
        id = m_freeSlots.back();
        m_freeSlots.pop_back();
        m_sounds[id]                = snd;
        m_sfxChannels[id]           = nullptr;
        m_soundPaths[id]            = path;
        m_soundFailureReported[id]  = 0;
        m_soundRefs[id]             = 1;
        m_soundKeys[id]             = key;
        m_soundHasLastPos[id]       = 0;
        m_soundImport[id]           = importSettings;
        m_soundVolume[id]           = 1.0f;
    }
    else
    {
        m_sounds.push_back(snd);
        m_sfxChannels.push_back(nullptr);
        m_soundPaths.push_back(path);
        m_soundFailureReported.push_back(0);
        m_soundRefs.push_back(1);
        m_soundKeys.push_back(key);
        m_soundLastPos.push_back(glm::vec3(0.0f));
        m_soundHasLastPos.push_back(0);
        m_soundImport.push_back(importSettings);
        m_soundVolume.push_back(1.0f);
        id = (int)m_sounds.size() - 1;
    }
    m_soundByKey[key] = id;
    return id;
#else
    (void)loop;
    return -1;
#endif
}

void AudioManager::unloadSound(int id)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() || !m_sounds[id]) return;

    // Reference count: the sound may be in use by several
    // AudioClipComponents (same path and same mode). Releasing it with the first
    // destructor would be a use-after-free for all the others.
    if (--m_soundRefs[id] > 0) return;

    reinterpret_cast<FMOD::Sound*>(m_sounds[id])->release();
    m_sounds[id] = nullptr;
    m_sfxChannels[id] = nullptr;
    m_soundPaths[id].clear();
    m_soundFailureReported[id] = 0;
    m_soundRefs[id] = 0;
    // Out of the map BEFORE marking the slot as free: otherwise, a later loadSound
    // with that same key would find the stale entry pointing to a
    // slot that already belongs to another sound.
    m_soundByKey.erase(m_soundKeys[id]);
    m_soundKeys[id].clear();
    m_soundHasLastPos[id] = 0;
    m_soundImport[id] = AudioImportSettings{};
    m_soundVolume[id] = 1.0f;
    m_freeSlots.push_back(id);
#endif
}

AudioManager::SoundLoadState AudioManager::getSoundState(int id) const
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() || !m_sounds[id])
        return SoundLoadState::Missing;
    auto* snd = reinterpret_cast<FMOD::Sound*>(m_sounds[id]);
    FMOD_OPENSTATE st;
    // When loading fails, FMOD returns the error code as the return value
    // of getOpenState and *st may not have been written, so the return is looked at
    // FIRST. Checked with the two forms of failure that occur in
    // practice —non-existent path and a file that exists but is not audio—: both
    // go out through here (see the two tests in audio_tests.cpp, which were
    // written with a sabotage of this line).
    if (snd->getOpenState(&st, nullptr, nullptr, nullptr) != FMOD_OK)
        return SoundLoadState::Failed;
    if (st == FMOD_OPENSTATE_LOADING) return SoundLoadState::Loading;
    // Defensive and WITHOUT test coverage: it has not been possible to provoke a
    // getOpenState that returns FMOD_OK with state ERROR (the two
    // sabotages of this line passed the tests). It stays because the FMOD docs
    // define the state and removing it would be betting that it never happens; it is
    // not presented as a tested path.
    if (st == FMOD_OPENSTATE_ERROR)   return SoundLoadState::Failed;
    return SoundLoadState::Ready;
#else
    (void)id;
    return SoundLoadState::Missing;
#endif
}

void AudioManager::pollLoadFailures(std::vector<std::string>& out)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system) return;
    for (size_t i = 0; i < m_sounds.size(); ++i)
    {
        if (!m_sounds[i] || m_soundFailureReported[i]) continue;
        if (getSoundState((int)i) != SoundLoadState::Failed) continue;
        m_soundFailureReported[i] = 1;
        out.push_back(m_soundPaths[i]);
    }
#else
    (void)out;
#endif
}

#ifdef DT_FMOD_ENABLED
// The Channel* stored for id, only if it is still playing and is still the channel
// of THAT sound. Returns nullptr in any other case.
static FMOD::Channel* liveChannel(void* raw, void* expectedSound)
{
    auto* ch = reinterpret_cast<FMOD::Channel*>(raw);
    if (!ch) return nullptr;

    bool playing = false;
    if (ch->isPlaying(&playing) != FMOD_OK || !playing) return nullptr;

    FMOD::Sound* current = nullptr;
    if (ch->getCurrentSound(&current) != FMOD_OK) return nullptr;
    if (current != reinterpret_cast<FMOD::Sound*>(expectedSound)) return nullptr;

    return ch;
}
#endif

#ifdef DT_FMOD_ENABLED
// Mixes to mono the voice of a sound marked forceMono: each of the two front
// outputs receives the sum of ALL the inputs at 1/N (the peak does not exceed
// 1 even if the inputs are in phase); the rest of the outputs (surround, LFE)
// are left silent. 2D sounds only: in 3D FMOD's spatial panner takes
// precedence over the matrix and a 3D emitter with spread 0 already sounds like a point. A
// single-channel clip, or any failure reading the matrix, leaves the voice as it is.
// Dimensions of the mix matrix of a voice of `snd`: inputs = channels of the
// sound, outputs = channels of the system output. They are NOT asked of the channel:
// getMixMatrix(nullptr, ...) returns OK with 0 and 0 on a newly created voice that
// does not yet have its own matrix.
static bool mixMatrixDims(FMOD::System* sys, FMOD::Sound* snd, int& out, int& in)
{
    FMOD_SPEAKERMODE speakerMode = FMOD_SPEAKERMODE_DEFAULT;
    if (sys->getSoftwareFormat(nullptr, &speakerMode, nullptr) != FMOD_OK) return false;
    if (sys->getSpeakerModeChannels(speakerMode, &out) != FMOD_OK) return false;
    if (snd->getFormat(nullptr, nullptr, &in, nullptr) != FMOD_OK) return false;
    return true;
}

// `stereoPan` [-1, 1] goes INSIDE the matrix: FMOD's Channel::setPan replaces
// the whole matrix, so a later setPan silently undid the mono. That is
// why this function is called AFTER setPan and applies the panning itself: balance
// law (the side opposite to the pan is attenuated linearly, the center does not touch anything).
static void applyForceMono(FMOD::System* sys, FMOD::Channel* ch, FMOD::Sound* snd, float stereoPan)
{
    FMOD_MODE mode = 0;
    if (snd->getMode(&mode) != FMOD_OK || (mode & FMOD_3D)) return;
    int out = 0, in = 0;
    if (!mixMatrixDims(sys, snd, out, in) || in < 2 || out < 1) return;
    const float pan = std::isfinite(stereoPan) ? std::clamp(stereoPan, -1.0f, 1.0f) : 0.0f;
    const float gainRow[2] = { 1.0f - std::max(0.0f, pan), 1.0f - std::max(0.0f, -pan) };
    std::vector<float> m(static_cast<size_t>(out) * in, 0.0f);
    const float g = 1.0f / static_cast<float>(in);
    for (int row = 0; row < std::min(out, 2); ++row)
        for (int col = 0; col < in; ++col)
            m[static_cast<size_t>(row) * in + col] = g * gainRow[row];
    ch->setMixMatrix(m.data(), out, in, in);
}

float AudioManager::voiceVolume(int id, float volume) const
{
    if (id < 0 || id >= static_cast<int>(m_soundImport.size())) return volume;
    return volume * audioGainLinear(m_soundImport[id].gainDb);
}
#endif

void AudioManager::setChannelVolume(int id, float volume)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return;
    m_soundVolume[id] = volume;                       // the last thing the component asked for
    if (FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]))
        ch->setVolume(voiceVolume(id, volume));
#else
    (void)id; (void)volume;
#endif
}

void AudioManager::setChannelPitch(int id, float pitch)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return;
    if (FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]))
        ch->setPitch(pitch);
#else
    (void)id; (void)pitch;
#endif
}

AudioImportSettings AudioManager::getSoundImportSettings(int id) const
{
#ifdef DT_FMOD_ENABLED
    if (id < 0 || id >= static_cast<int>(m_soundImport.size()) || !m_sounds[id])
        return {};
    return m_soundImport[id];
#else
    (void)id;
    return {};
#endif
}

void AudioManager::refreshImportSettings(const std::string& path)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system) return;
    for (size_t i = 0; i < m_sounds.size(); ++i)
    {
        if (!m_sounds[i] || !sameAssetPath(m_soundPaths[i], path)) continue;
        std::string warning;
        m_soundImport[i] = loadAudioImportSettings(m_soundPaths[i], &warning);
        if (!warning.empty())
            std::fprintf(stderr, "[AudioImport] %s: %s\n", m_soundPaths[i].c_str(), warning.c_str());
        // The live voice is readjusted right away with the COMPONENT's volume, not with
        // the channel's. Mono applies from the next playback.
        if (FMOD::Channel* ch = liveChannel(m_sfxChannels[i], m_sounds[i]))
            ch->setVolume(voiceVolume(static_cast<int>(i), m_soundVolume[i]));
    }
#else
    (void)path;
#endif
}

bool AudioManager::isVoiceForcedMono(int id) const
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return false;
    FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]);
    int out = 0, in = 0;
    if (!ch || !mixMatrixDims(SYS, reinterpret_cast<FMOD::Sound*>(m_sounds[id]), out, in) ||
        in < 2 || out < 1) return false;
    const int wantOut = out, wantIn = in;
    std::vector<float> m(static_cast<size_t>(out) * in, 0.0f);
    if (ch->getMixMatrix(m.data(), &out, &in, in) != FMOD_OK) return false;
    // FMOD may return dimensions other than the ones requested: without re-checking,
    // a 0,0 would give "true" with both loops empty.
    if (out != wantOut || in != wantIn) return false;
    // Mono = each of the two front rows carries the SAME value in all the
    // inputs (with panning the row of the opposite side is attenuated, so it is not
    // compared against 1/N), and one of the two is not zero. A factory matrix
    // (identity) has rows [1,0]: it is not constant.
    bool anyNonZero = false;
    for (int row = 0; row < std::min(out, 2); ++row)
    {
        const float first = m[static_cast<size_t>(row) * in];
        for (int col = 0; col < in; ++col)
            if (std::fabs(m[static_cast<size_t>(row) * in + col] - first) > 1e-4f) return false;
        if (first > 1e-4f) anyNonZero = true;
    }
    return anyNonZero;
#else
    (void)id;
    return false;
#endif
}

float AudioManager::getChannelVolume(int id) const
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return -1.0f;
    FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]);
    float v = -1.0f;
    if (!ch || ch->getVolume(&v) != FMOD_OK) return -1.0f;
    return v;
#else
    (void)id;
    return -1.0f;
#endif
}

void AudioManager::setSound3DMinMaxDistance(int id, float minDistance, float maxDistance)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return;
    auto* snd = reinterpret_cast<FMOD::Sound*>(m_sounds[id]);
    FMOD_MODE mode = 0; snd->getMode(&mode);
    if (!(mode & FMOD_3D)) return;
    // ONLY to the channel, never to the FMOD::Sound. Before it was written to both, and that
    // stopped being correct as soon as the sound is shared among several
    // AudioClipComponents: adjusting the attenuation of one speaker would change the
    // radius of all the other objects that use the same file.
    //
    // The flip side is that a newly created sound no longer inherits these
    // distances: they are applied by AudioClipComponent::play, which calls
    // applyDistances() right after starting the voice.
    if (FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]))
        ch->set3DMinMaxDistance(minDistance, maxDistance);
#else
    (void)id; (void)minDistance; (void)maxDistance;
#endif
}

void AudioManager::playSound(int id, const glm::vec3& worldPos, float volume, float pitch,
                              AudioBus bus, float minDistance, float maxDistance,
                              float spread, float stereoPan, float dopplerLevel)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return;
    FMOD::Channel* ch;
    auto* snd = reinterpret_cast<FMOD::Sound*>(m_sounds[id]);
    // With NONBLOCKING, the Sound exists but may still be loading. A
    // playSound on it returns FMOD_ERR_NOTREADY. It is silently ignored instead
    // of spitting out an error: the user asked to play something that is not
    // there yet, and the normal case is pressing Play right after dropping the file.
    FMOD_OPENSTATE state;
    if (snd->getOpenState(&state, nullptr, nullptr, nullptr) == FMOD_OK
        && state == FMOD_OPENSTATE_LOADING)
        return;
    // paused = true: volume, pitch and position have to be set BEFORE
    // the first sample plays. Starting it playing, a 3D clip is heard for an
    // instant from the world origin and with the volume of the previous channel.
    auto* group = reinterpret_cast<FMOD::ChannelGroup*>(groupForBus(bus));
    if (SYS->playSound(snd, group, true, &ch) != FMOD_OK) return;
    // If the id already had a voice playing from a previous playback, it is stopped
    // before overwriting the reference: otherwise that channel is left orphaned (without
    // a reference) and keeps playing indefinitely if it loops, without
    // stop() or the volume/pitch setters being able to reach it any more. Same
    // semantics as AudioSource.Play() in Unity: a new Play() cuts the
    // previous one.
    if (FMOD::Channel* prev = liveChannel(m_sfxChannels[id], m_sounds[id])) prev->stop();
    m_sfxChannels[id] = ch;

    m_soundVolume[id] = volume;
    ch->setVolume(voiceVolume(id, volume));
    ch->setPitch(pitch);

    FMOD_MODE mode = 0; snd->getMode(&mode);
    if (mode & FMOD_3D) {
        FMOD_VECTOR p = { worldPos.x, worldPos.y, worldPos.z };
        FMOD_VECTOR v = { 0, 0, 0 };
        ch->set3DAttributes(&p, &v);
        // To the VOICE, not to the FMOD::Sound: the sound is shared between clips and
        // writing them there would change the radius for all the others.
        ch->set3DMinMaxDistance(minDistance, maxDistance);
        // Stereo widening and doppler sensitivity, also per voice.
        ch->set3DSpread(spread);
        ch->set3DDopplerLevel(dopplerLevel);
    }
    // Manual panning only makes sense in 2D: in 3D the position decides, and
    // writing it there would fight FMOD's spatial panning. Outside the if, with
    // its own condition, to make clear that it is NOT a 3D property.
    if (!(mode & FMOD_3D) && stereoPan != 0.0f)
        ch->setPan(stereoPan);
    // AFTER setPan: this replaces the whole mix matrix, and mono is
    // a matrix. applyForceMono carries the panning inside.
    if (m_soundImport[id].forceMono) applyForceMono(SYS, ch, snd, stereoPan);

    ch->setPaused(false);
#else
    (void)id; (void)worldPos; (void)volume; (void)pitch;
#endif
}

bool AudioManager::isSoundPlaying(int id) const
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return false;
    return liveChannel(m_sfxChannels[id], m_sounds[id]) != nullptr;
#else
    (void)id;
    return false;
#endif
}

bool AudioManager::isSoundPaused(int id) const
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return false;
    FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]);
    if (!ch) return false;
    bool paused = false;
    if (ch->getPaused(&paused) != FMOD_OK) return false;
    return paused;
#else
    (void)id;
    return false;
#endif
}

void AudioManager::setSoundPaused(int id, bool paused)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return;
    if (FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]))
        ch->setPaused(paused);
#else
    (void)id; (void)paused;
#endif
}

void AudioManager::setSoundMute(int id, bool mute)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return;
    if (FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]))
        ch->setMute(mute);
#else
    (void)id; (void)mute;
#endif
}

float AudioManager::getSoundTime(int id) const
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return -1.0f;
    FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]);
    if (!ch) return -1.0f;
    unsigned int ms = 0;
    if (ch->getPosition(&ms, FMOD_TIMEUNIT_MS) != FMOD_OK) return -1.0f;
    return (float)ms / 1000.0f;
#else
    (void)id;
    return -1.0f;
#endif
}

void AudioManager::setSoundTime(int id, float seconds)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return;
    if (!std::isfinite(seconds) || seconds < 0.0f) return;
    if (FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]))
        ch->setPosition((unsigned int)(seconds * 1000.0f), FMOD_TIMEUNIT_MS);
#else
    (void)id; (void)seconds;
#endif
}

void AudioManager::setAudioPaused(bool paused)
{
#ifdef DT_FMOD_ENABLED
    // On the master, not on each bus: the other two hang from it, so
    // a single one freezes them all —including the loose PlayOneShot voices,
    // which cannot be reached any other way.
    if (auto* g = reinterpret_cast<FMOD::ChannelGroup*>(groupForBus(AudioBus::Master)))
        g->setPaused(paused);
#else
    (void)paused;
#endif
}

bool AudioManager::isAudioPaused() const
{
#ifdef DT_FMOD_ENABLED
    if (auto* g = reinterpret_cast<FMOD::ChannelGroup*>(groupForBus(AudioBus::Master)))
    {
        bool paused = false;
        if (g->getPaused(&paused) == FMOD_OK) return paused;
    }
    return false;
#else
    return false;
#endif
}

void AudioManager::setSoundPosition(int id, const glm::vec3& worldPos, float dt)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() ||
        id >= (int)m_sfxChannels.size() || !m_sounds[id]) return;
    auto* snd = reinterpret_cast<FMOD::Sound*>(m_sounds[id]);
    FMOD_MODE mode = 0; snd->getMode(&mode);
    // In 2D there is nothing to position: set3DAttributes on a 2D voice does
    // nothing useful and this method is called per frame and per clip.
    if (!(mode & FMOD_3D)) return;
    FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id]);
    if (!ch) return;
    // The source velocity comes from its position in the previous frame; it is the
    // other half of the doppler (the listener's is carried by update()). safeVelocity
    // discards the first frame and impossible jumps, which is what avoids the
    // screech of a teleport.
    const glm::vec3 vel = safeVelocity(worldPos, m_soundLastPos[id],
                                        m_soundHasLastPos[id] != 0, dt);
    m_soundLastPos[id]    = worldPos;
    m_soundHasLastPos[id] = 1;

    FMOD_VECTOR p = { worldPos.x, worldPos.y, worldPos.z };
    FMOD_VECTOR v = { vel.x, vel.y, vel.z };
    ch->set3DAttributes(&p, &v);
#else
    (void)id; (void)worldPos;
#endif
}

void AudioManager::playSoundOneShot(int id, const glm::vec3& worldPos, float volume, float pitch,
                                     AudioBus bus, float minDistance, float maxDistance,
                                     float spread, float stereoPan, float dopplerLevel)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system || id < 0 || id >= (int)m_sounds.size() || !m_sounds[id]) return;
    auto* snd = reinterpret_cast<FMOD::Sound*>(m_sounds[id]);
    // Same silence as playSound while the sound is still loading: it is not an
    // error, it is just not there yet.
    FMOD_OPENSTATE state;
    if (snd->getOpenState(&state, nullptr, nullptr, nullptr) == FMOD_OK
        && state == FMOD_OPENSTATE_LOADING)
        return;

    FMOD::Channel* ch;
    // paused = true for the same reason as in playSound: volume, pitch and position
    // have to be set before the first sample.
    auto* group = reinterpret_cast<FMOD::ChannelGroup*>(groupForBus(bus));
    if (SYS->playSound(snd, group, true, &ch) != FMOD_OK) return;

    // And HERE is the difference from playSound: the previous voice is neither stopped nor
    // is this one stored in m_sfxChannels. It is what allows overlapping. The
    // file gain is applied all the same, but m_soundVolume is not touched: this
    // voice cannot be reached afterwards.
    ch->setVolume(voiceVolume(id, volume));
    ch->setPitch(pitch);

    FMOD_MODE mode = 0; snd->getMode(&mode);
    if (mode & FMOD_3D) {
        FMOD_VECTOR p = { worldPos.x, worldPos.y, worldPos.z };
        FMOD_VECTOR v = { 0, 0, 0 };
        ch->set3DAttributes(&p, &v);
        // To the VOICE, not to the FMOD::Sound: the sound is shared between clips and
        // writing them there would change the radius for all the others.
        ch->set3DMinMaxDistance(minDistance, maxDistance);
        // Stereo widening and doppler sensitivity, also per voice.
        ch->set3DSpread(spread);
        ch->set3DDopplerLevel(dopplerLevel);
    }
    // Manual panning only makes sense in 2D: in 3D the position decides, and
    // writing it there would fight FMOD's spatial panning. Outside the if, with
    // its own condition, to make clear that it is NOT a 3D property.
    if (!(mode & FMOD_3D) && stereoPan != 0.0f)
        ch->setPan(stereoPan);
    // AFTER setPan: this replaces the whole mix matrix (see playSound).
    if (m_soundImport[id].forceMono) applyForceMono(SYS, ch, snd, stereoPan);
    // With no stored reference, this voice is not reached by the per-frame 3D
    // tracking (setSoundPosition) either: a one-shot sounds where it was fired. For
    // short clips —which is its use case— the difference is not audible.
    ch->setPaused(false);
#else
    (void)id; (void)worldPos; (void)volume; (void)pitch;
#endif
}

void AudioManager::stopSound(int id)
{
#ifdef DT_FMOD_ENABLED
    if (id < 0 || id >= (int)m_sfxChannels.size() || id >= (int)m_sounds.size()) return;
    // Via liveChannel, like the volume/pitch setters: FMOD recycles the
    // Channel* of voices that finish, so the pointer stored here may already
    // point to the voice of ANOTHER sound. Stopping it blindly, a stopSound(id)
    // on a clip that ended a while ago cuts the one that is playing now.
    if (FMOD::Channel* ch = liveChannel(m_sfxChannels[id], m_sounds[id])) ch->stop();
#else
    (void)id;
#endif
}

#ifdef DT_FMOD_ENABLED
// Loads (or reuses) the sound of a path and leaves it RETAINED in the cache,
// returning its id. It is shared by preloadClip and playClipAtPoint: having a single
// place that loads avoids the double loadSound that was here before, with its
// compensating unloadSound behind — two calls that cancelled each other and only
// served to make the refcount add up.
//
// 3D and without loop: it is the profile of a positional one-shot. The same file in
// 2D is another sound, and it is loaded by the AudioClipComponent that asks for it.
int AudioManager::acquirePinnedSound(const std::string& path)
{
    if (!m_system) return -1;
    const int id = loadSound(path, /*is3D=*/true, /*loop=*/false, AudioLoadMode::Sample);
    if (id < 0) return -1;
    // insert returns false if it was already retained: that loadSound only raised
    // the refcount again, and it is undone so that the pin keeps counting a SINGLE
    // reference. Without this the sound would stay alive anyway (the pin is never
    // released), but the counter would grow without a ceiling and would stop describing
    // reality for anyone who looks at it afterwards.
    if (!m_pinnedSounds.insert(id).second)
        unloadSound(id);
    return id;
}
#endif

void AudioManager::preloadClip(const std::string& path)
{
#ifdef DT_FMOD_ENABLED
    acquirePinnedSound(path);
#else
    (void)path;
#endif
}

void AudioManager::playClipAtPoint(const std::string& path, const glm::vec3& worldPos,
                                    float volume, float pitch, AudioBus bus,
                                    float minDistance, float maxDistance)
{
#ifdef DT_FMOD_ENABLED
    const int id = acquirePinnedSound(path);
    if (id < 0) return;
    playSoundOneShot(id, worldPos, volume, pitch, bus, minDistance, maxDistance);
#else
    (void)path; (void)worldPos; (void)volume; (void)pitch;
    (void)bus; (void)minDistance; (void)maxDistance;
#endif
}

// WITHOUT TEST COVERAGE, and it is worth knowing: that playSound sends the voice to the
// right group cannot be observed from a headless test — FMOD does not expose
// "through which bus is this playing" in a way that does not force inventing a
// getter nobody else would use. A sabotage that ignored the bus and routed everything
// to SFX passes the whole suite. What is covered is everything else on the
// path: the round-trip of the bus through the .scene, the back-compat, the unknown
// name, and that the three volumes are independent.
//
// Manual verification: put a clip on Music, lower Music Volume to 0 and
// check that it goes silent while another clip on SFX is still heard.
#ifdef DT_FMOD_ENABLED
void* AudioManager::groupForBus(AudioBus bus) const
{
    if (!m_system) return nullptr;
    switch (bus)
    {
        case AudioBus::Music: return m_musicGroup;
        case AudioBus::Sfx:   return m_sfxGroup;
        case AudioBus::Master:
        default:
        {
            // The master is not created here: FMOD provides it and it is the parent of the other
            // two, so its volume scales both.
            FMOD::ChannelGroup* master = nullptr;
            if (SYS->getMasterChannelGroup(&master) != FMOD_OK) return nullptr;
            return master;
        }
    }
}
#endif

#ifdef DT_FMOD_ENABLED
namespace {
// FMOD DSP type for each of our effects. All are FMOD Core: none of
// this needs FMOD Studio.
FMOD_DSP_TYPE fmodDspType(AudioEffect e)
{
    switch (e)
    {
        case AudioEffect::HighPass: return FMOD_DSP_TYPE_HIGHPASS;
        case AudioEffect::Echo:     return FMOD_DSP_TYPE_ECHO;
        case AudioEffect::Reverb:   return FMOD_DSP_TYPE_SFXREVERB;
        case AudioEffect::LowPass:
        default:                    return FMOD_DSP_TYPE_LOWPASS;
    }
}

// Translates the [0, 1] of the public API to the units of each DSP. In ONE
// place: if this were spread across the UI and Lua, the two ranges would end up
// out of sync as soon as someone touched one.
void applyEffectAmount(FMOD::DSP* dsp, AudioEffect e, float amount)
{
    const float a = std::clamp(amount, 0.0f, 1.0f);
    switch (e)
    {
        case AudioEffect::LowPass:
            // 0 -> very closed (400 Hz, "underwater"); 1 -> almost
            // transparent (22 kHz). Logarithmic because the ear is: linear
            // left the whole effect bunched up in the last 10% of the slider.
            dsp->setParameterFloat(FMOD_DSP_LOWPASS_CUTOFF,
                                   400.0f * std::pow(55.0f, a));
            break;
        case AudioEffect::HighPass:
            // The other way round: 0 cuts nothing, 1 takes away all the lows.
            dsp->setParameterFloat(FMOD_DSP_HIGHPASS_CUTOFF,
                                   10.0f * std::pow(500.0f, a));
            break;
        case AudioEffect::Echo:
            // Delay between repetitions, from almost back-to-back to almost half a second.
            dsp->setParameterFloat(FMOD_DSP_ECHO_DELAY, 10.0f + a * 490.0f);
            break;
        case AudioEffect::Reverb:
            // Size of the tail, from a small room to a cathedral.
            dsp->setParameterFloat(FMOD_DSP_SFXREVERB_DECAYTIME, 100.0f + a * 9900.0f);
            break;
    }
}
} // namespace
#endif

const std::vector<std::string>& AudioManager::reverbPresetNames()
{
    // A subset of the FMOD_PRESET_*: the ambiences that are actually
    // requested. Extending the list is adding the name here and its case in
    // fmodReverbPreset. The order is that of the inspector combo.
    static const std::vector<std::string> names = {
        "off", "generic", "room", "bathroom", "stoneroom", "auditorium",
        "concerthall", "cave", "arena", "hangar", "hallway", "alley",
        "forest", "city", "mountains", "quarry", "underwater"
    };
    return names;
}

#ifdef DT_FMOD_ENABLED
namespace {
// Name -> FMOD properties. Returns false if the name does not exist, so that
// the caller can warn instead of installing an arbitrary ambience.
bool fmodReverbPreset(const std::string& name, FMOD_REVERB_PROPERTIES& out)
{
    if (name == "off")         { out = FMOD_PRESET_OFF;         return true; }
    if (name == "generic")     { out = FMOD_PRESET_GENERIC;     return true; }
    if (name == "room")        { out = FMOD_PRESET_ROOM;        return true; }
    if (name == "bathroom")    { out = FMOD_PRESET_BATHROOM;    return true; }
    if (name == "stoneroom")   { out = FMOD_PRESET_STONEROOM;   return true; }
    if (name == "auditorium")  { out = FMOD_PRESET_AUDITORIUM;  return true; }
    if (name == "concerthall") { out = FMOD_PRESET_CONCERTHALL; return true; }
    if (name == "cave")        { out = FMOD_PRESET_CAVE;        return true; }
    if (name == "arena")       { out = FMOD_PRESET_ARENA;       return true; }
    if (name == "hangar")      { out = FMOD_PRESET_HANGAR;      return true; }
    if (name == "hallway")     { out = FMOD_PRESET_HALLWAY;     return true; }
    if (name == "alley")       { out = FMOD_PRESET_ALLEY;       return true; }
    if (name == "forest")      { out = FMOD_PRESET_FOREST;      return true; }
    if (name == "city")        { out = FMOD_PRESET_CITY;        return true; }
    if (name == "mountains")   { out = FMOD_PRESET_MOUNTAINS;   return true; }
    if (name == "quarry")      { out = FMOD_PRESET_QUARRY;      return true; }
    if (name == "underwater")  { out = FMOD_PRESET_UNDERWATER;  return true; }
    return false;
}
} // namespace
#endif

bool AudioManager::syncReverbZone(uint64_t ownerId, const glm::vec3& worldPos,
                                   float minDistance, float maxDistance,
                                   const std::string& preset, bool enabled)
{
#ifdef DT_FMOD_ENABLED
    if (!m_system) return false;

    FMOD_REVERB_PROPERTIES props;
    // The preset is validated BEFORE creating anything: with an invented name a
    // zone with some random ambience is not installed, it just says no.
    if (!fmodReverbPreset(preset, props)) return false;

    FMOD::Reverb3D* zone = nullptr;
    if (auto it = m_reverbZones.find(ownerId); it != m_reverbZones.end())
    {
        zone = reinterpret_cast<FMOD::Reverb3D*>(it->second);
    }
    else
    {
        // FMOD limits how many 3D instances there can be at once; if it gives no more,
        // false is returned and the caller decides whether to warn.
        if (SYS->createReverb3D(&zone) != FMOD_OK || !zone) return false;
        m_reverbZones[ownerId] = zone;
    }

    FMOD_VECTOR p = { worldPos.x, worldPos.y, worldPos.z };
    zone->set3DAttributes(&p, minDistance, maxDistance);
    zone->setProperties(&props);
    // Disabled it stays created but without effect: this way toggling the checkbox does not
    // cost creating and destroying the resource every time.
    zone->setActive(enabled);
    return true;
#else
    (void)ownerId; (void)worldPos; (void)minDistance; (void)maxDistance;
    (void)preset; (void)enabled;
    return false;
#endif
}

void AudioManager::removeReverbZone(uint64_t ownerId)
{
#ifdef DT_FMOD_ENABLED
    auto it = m_reverbZones.find(ownerId);
    if (it == m_reverbZones.end()) return;
    reinterpret_cast<FMOD::Reverb3D*>(it->second)->release();
    m_reverbZones.erase(it);
#else
    (void)ownerId;
#endif
}

void AudioManager::retainReverbZones(const std::vector<uint64_t>& aliveOwnerIds)
{
#ifdef DT_FMOD_ENABLED
    if (m_reverbZones.empty()) return;
    for (auto it = m_reverbZones.begin(); it != m_reverbZones.end(); )
    {
        const bool alive = std::find(aliveOwnerIds.begin(), aliveOwnerIds.end(), it->first)
                            != aliveOwnerIds.end();
        if (alive) { ++it; continue; }
        // The GameObject that held it is no longer there: without this its reverb would keep
        // being applied to the rest of the scene forever.
        if (it->second) reinterpret_cast<FMOD::Reverb3D*>(it->second)->release();
        it = m_reverbZones.erase(it);
    }
#else
    (void)aliveOwnerIds;
#endif
}

void AudioManager::clearReverbZones()
{
#ifdef DT_FMOD_ENABLED
    for (auto& [id, zone] : m_reverbZones)
        if (zone) reinterpret_cast<FMOD::Reverb3D*>(zone)->release();
    m_reverbZones.clear();
#endif
}

size_t AudioManager::reverbZoneCount() const
{
#ifdef DT_FMOD_ENABLED
    return m_reverbZones.size();
#else
    return 0;
#endif
}

void AudioManager::setBusEffect(AudioBus bus, AudioEffect effect, float amount)
{
#ifdef DT_FMOD_ENABLED
    auto* group = reinterpret_cast<FMOD::ChannelGroup*>(groupForBus(bus));
    if (!group) return;

    const int key = effectKey(bus, effect);
    // Already hung: only the knob is readjusted. Without this branch, moving a slider
    // would chain a new DSP per frame until choking the mixer.
    if (auto it = m_busEffects.find(key); it != m_busEffects.end())
    {
        applyEffectAmount(reinterpret_cast<FMOD::DSP*>(it->second), effect, amount);
        return;
    }

    FMOD::DSP* dsp = nullptr;
    if (SYS->createDSPByType(fmodDspType(effect), &dsp) != FMOD_OK || !dsp) return;
    applyEffectAmount(dsp, effect, amount);
    // If the DSP cannot be attached it has to be released right here: storing it
    // unconnected would leave a live native resource that nobody would look at again.
    if (group->addDSP(FMOD_CHANNELCONTROL_DSP_HEAD, dsp) != FMOD_OK)
    {
        dsp->release();
        return;
    }
    m_busEffects[key] = dsp;
#else
    (void)bus; (void)effect; (void)amount;
#endif
}

void AudioManager::clearBusEffect(AudioBus bus, AudioEffect effect)
{
#ifdef DT_FMOD_ENABLED
    const int key = effectKey(bus, effect);
    auto it = m_busEffects.find(key);
    if (it == m_busEffects.end()) return;
    auto* dsp = reinterpret_cast<FMOD::DSP*>(it->second);
    // Disconnect BEFORE releasing: releasing a DSP still attached to the group
    // leaves the mixer with a dead pointer in its chain.
    if (auto* group = reinterpret_cast<FMOD::ChannelGroup*>(groupForBus(bus)))
        group->removeDSP(dsp);
    dsp->release();
    m_busEffects.erase(it);
#else
    (void)bus; (void)effect;
#endif
}

void AudioManager::clearBusEffects(AudioBus bus)
{
#ifdef DT_FMOD_ENABLED
    for (AudioEffect e : { AudioEffect::LowPass, AudioEffect::HighPass,
                            AudioEffect::Echo, AudioEffect::Reverb })
        clearBusEffect(bus, e);
#else
    (void)bus;
#endif
}

size_t AudioManager::busDspCount(AudioBus bus) const
{
#ifdef DT_FMOD_ENABLED
    auto* group = reinterpret_cast<FMOD::ChannelGroup*>(groupForBus(bus));
    if (!group) return 0;
    int n = 0;
    if (group->getNumDSPs(&n) != FMOD_OK || n < 0) return 0;
    return (size_t)n;
#else
    (void)bus;
    return 0;
#endif
}

size_t AudioManager::activeEffectCount() const
{
#ifdef DT_FMOD_ENABLED
    return m_busEffects.size();
#else
    return 0;
#endif
}

bool AudioManager::hasBusEffect(AudioBus bus, AudioEffect effect) const
{
#ifdef DT_FMOD_ENABLED
    return m_busEffects.find(effectKey(bus, effect)) != m_busEffects.end();
#else
    (void)bus; (void)effect;
    return false;
#endif
}

void AudioManager::setBusVolume(AudioBus bus, float v)
{
#ifdef DT_FMOD_ENABLED
    if (auto* g = groupForBus(bus))
        reinterpret_cast<FMOD::ChannelGroup*>(g)->setVolume(v);
#else
    (void)bus; (void)v;
#endif
}

float AudioManager::getBusVolume(AudioBus bus) const
{
#ifdef DT_FMOD_ENABLED
    if (auto* g = groupForBus(bus))
    {
        float v = 1.0f;
        if (reinterpret_cast<FMOD::ChannelGroup*>(g)->getVolume(&v) == FMOD_OK) return v;
    }
    // Without a system (or if FMOD fails) the neutral one: it is what the UI should draw
    // so as not to suggest that the audio is turned down when what is happening is that there is no
    // audio.
    return 1.0f;
#else
    (void)bus;
    return 1.0f;
#endif
}


std::shared_ptr<AudioClipComponent> AudioManager::createAudioClipComponent(
    const std::string& path, bool is3D, bool loop, AudioLoadMode loadMode)
{
    int id = loadSound(path, is3D, loop, loadMode);
    if (id < 0) return nullptr;
    return std::make_shared<AudioClipComponent>(this, path, id, is3D, loop, loadMode);
}

} // namespace DonTopo
