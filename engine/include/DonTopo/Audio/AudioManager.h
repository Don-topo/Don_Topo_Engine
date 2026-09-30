#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>
#include <glm/glm.hpp>

#include "DonTopo/Audio/AudioBus.h"
#include "DonTopo/Core/ImportSettings.h"

namespace DonTopo {

class AudioClipComponent;

class AudioManager {
public:
    AudioManager() = default;
    ~AudioManager();
    AudioManager(const AudioManager&)            = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    // true if audio is operational. It does NOT throw: a machine with no output
    // device (or without FMOD compiled) returns false, writes a line to
    // cerr and leaves the engine running muted — before, the exception went up to the
    // editor main and the editor did not start. Calling it twice is a no-op.
    bool init();
    // Is there a live FMOD system? It is what tells "no audio on this machine"
    // apart from any other failure; the tests use it to truly skip the
    // cases that need FMOD.
    bool available() const;

    // FMOD started but WITHOUT an output device (FMOD_OUTPUTTYPE_NOSOUND): the
    // engine works and nothing sounds, without any error. It happens on Linux without
    // libpulse or libasound (the minimal WSL image). Empty if there is output or if
    // there is no FMOD. The host shows it (Log Console); init() already writes it to
    // stderr, which in the runtime ends up in game.log.
    const std::string& outputWarning() const { return m_outputWarning; }

    // The text of that warning for an FMOD output type (FMOD_OUTPUTTYPE as
    // int, so as not to drag fmod.hpp into this header). Separate so it can be tested
    // without taking the machine's sound card away.
    static std::string outputWarningFor(int fmodOutputType);
    // dt in seconds. It is used ONLY to derive the listener velocity, which is
    // what gives the doppler effect; with dt <= 0 the velocity stays at zero and the
    // doppler does not act (which is how this behaved before having it). A large
    // position jump —a teleport, or loading another scene— would produce
    // an absurd velocity and a screech: that is why anything exceeding
    // kMaxListenerSpeed is discarded instead of believed.
    void update(const glm::vec3& listenerPos,
                const glm::vec3& listenerForward,
                const glm::vec3& listenerUp,
                float dt = 0.0f);
    void shutdown();

    // Load state of a sound. With FMOD_NONBLOCKING, createSound returns
    // FMOD_OK even if the file does not exist or is corrupt: the real failure only
    // shows up AFTER, here. Without querying it, a broken asset was total silence
    // — no error in the UI when dropping it, no warning when loading the scene, not a
    // line in the Log when pressing Play.
    enum class SoundLoadState {
        Missing,  // id out of range, slot released, or no FMOD system
        Loading,  // the internal FMOD thread is still reading
        Ready,    // utilizable
        Failed    // file absent, unsupported format, corrupt data
    };
    SoundLoadState getSoundState(int soundId) const;

    // Empties into out the paths of the sounds that have moved to Failed and had not yet been
    // reported. Meant to be called once per frame from the host,
    // which is the one with a channel to the user (Log Console in the editor, cerr in the
    // runtime): AudioManager does not have one. Each sound is reported ONCE, not
    // once per frame.
    void pollLoadFailures(std::vector<std::string>& out);

    // Returns immediately: FMOD loads on its internal thread (FMOD_NONBLOCKING). The
    // id is valid from now on, but a playSound before the load finishes does not
    // play anything — it is not an error, it is just not ready yet. Returning an
    // id >= 0 does NOT mean the file is valid: getSoundState says that.
    int  loadSound(const std::string& path, bool is3D = true, bool loop = false,
                   AudioLoadMode loadMode = AudioLoadMode::Sample,
                   AudioRolloff rolloff = AudioRolloff::Inverse);
    void unloadSound(int soundId);

    // FMOD sounds alive right now (occupied slots, not handed-out ids).
    // Diagnostic: it is the only way to see from outside that the cache really shares
    // — two clips of the same file with the same mode have to
    // leave this at 1, not 2.
    size_t loadedSoundCount() const;

    // Reserved slots, occupied or not. Together with loadedSoundCount it is what
    // makes recycling observable: creating and releasing a thousand sounds has to leave
    // this flat, not at a thousand. Without this getter, "ids are not recycled" cannot
    // be told apart from "they are recycled" from outside — both leave the same
    // number of live sounds.
    size_t soundSlotCount() const;

    // Is the sound really streaming? It reads the real FMOD_MODE, not what the
    // component believes. It exists because without this the line that applies
    // FMOD_CREATESTREAM has no coverage at all: removing it left everything
    // "working" —the mode is stored, serialized and read back— with the
    // whole feature without effect (sabotage verified). false if the id does not exist.
    bool isSoundStreaming(int soundId) const;

    // REAL attenuation curve of the sound, read from its FMOD_MODE. It exists for the
    // same reason as isSoundStreaming: without it, the line that applies the rolloff
    // flag has no coverage at all — removing it left the enum traveling through
    // the scene, the UI and Lua without the curve ever changing (sabotage
    // verified). Note: like streaming, the mode is not reliable until
    // loading finishes (getSoundState == Ready).
    AudioRolloff getSoundRolloff(int soundId) const;

    // minDistance/maxDistance are applied to the newly started voice. They go here and
    // not in the FMOD::Sound because the sound is SHARED between clips (cache by
    // path+mode): writing them into the sound would change the attenuation radius of
    // all the objects that use the same file.
    // spread: stereo widening of the 3D source in degrees [0, 360]. 0 = a
    // point (as it always was); 360 = enveloping. pan: manual pan [-1, 1], only
    // effective on 2D clips — in 3D the position decides. doppler: how much the
    // relative velocity affects the pitch, [0, 5]; 0 turns it off.
    void playSound(int soundId, const glm::vec3& worldPos = {},
                   float volume = 1.0f, float pitch = 1.0f,
                   AudioBus bus = AudioBus::Sfx,
                   float minDistance = 1.0f, float maxDistance = 100.0f,
                   float spread = 0.0f, float stereoPan = 0.0f,
                   float dopplerLevel = 0.0f);
    void stopSound(int soundId);

    // Fires a LOOSE voice of the sound: it is not stored in m_sfxChannels, so
    // it does not cut the previous playback nor is it cut by the next, and several
    // shots overlap. It is Unity's PlayOneShot, and it is what was missing so
    // that two consecutive footsteps, or two bullets, did not step on each other — playSound has the
    // opposite semantics on purpose (a new Play cuts the previous one).
    //
    // Consequence of not storing the channel: that voice can no longer be reached.
    // stopSound, the volume/pitch setters and isSoundPlaying do NOT see it; it plays
    // until it finishes. That is why a looping one-shot would be an immortal voice:
    // it is used with shot clips, not with loops.
    void playSoundOneShot(int soundId, const glm::vec3& worldPos = {},
                          float volume = 1.0f, float pitch = 1.0f,
                          AudioBus bus = AudioBus::Sfx,
                          float minDistance = 1.0f, float maxDistance = 100.0f,
                          float spread = 0.0f, float stereoPan = 0.0f,
                          float dopplerLevel = 0.0f);

    // Fires path at a world position, WITHOUT a GameObject in between: Unity's
    // PlayClipAtPoint. It is one-shot (overlaps whatever is playing) and 3D.
    //
    // The sound stays RETAINED in the cache forever — just like in Unity,
    // where the AudioClip is a loaded asset. Without that it would have to be unloaded at
    // the end of the voice, and that voice is not stored anywhere precisely
    // so that it overlaps. Retaining does not leak: it is one FMOD::Sound per distinct
    // path, and the second call with the same path does not count again.
    //
    // NOTE on the first shot: FMOD loads lazily (FMOD_NONBLOCKING), so
    // the first call of a new path is almost certainly not heard — the
    // sound is not ready yet. That is what preloadClip is for.
    void playClipAtPoint(const std::string& path, const glm::vec3& worldPos,
                         float volume = 1.0f, float pitch = 1.0f,
                         AudioBus bus = AudioBus::Sfx,
                         float minDistance = 1.0f, float maxDistance = 100.0f);

    // Loads and retains the sound without playing it, so that the first
    // playClipAtPoint of that path is actually heard. Idempotent.
    void preloadClip(const std::string& path);

    // They push the value to the channel of the last playback of soundId, if
    // it is still theirs. FMOD recycles the Channel*: a channel that already finished
    // may have been reassigned to another sound, and writing the volume to it would
    // change it on someone else's sound. That is why isPlaying() is checked and that
    // getCurrentSound() is the sound of that id.
    //
    // Nothing happens if there is no channel: the value lives in AudioClipComponent and
    // will be applied on the next playSound.
    void setChannelVolume(int soundId, float volume);
    void setChannelPitch (int soundId, float pitch);

    // Import settings (sidecar <clip>.import.json) of the sound: gain and
    // mono. The default if the id does not exist.
    AudioImportSettings getSoundImportSettings(int soundId) const;

    // Rereads the sidecar of `path` and applies it to ALL the live sounds of that path
    // (2D and 3D, with or without loop: they are different sounds of the same file) and readjusts
    // the volume of its live voice. Those of other paths are not touched. Mono applies
    // from the next playback.
    void refreshImportSettings(const std::string& path);

    // REAL volume of the live voice of soundId (the one followed by setChannelVolume and
    // the 3D tracking), or -1 if there is no voice. It exists so that the gain is
    // observable from a test: removing it left the whole feature without effect and the
    // suite green.
    float getChannelVolume(int soundId) const;

    // Does the live voice of soundId carry the mono matrix (each entry at 1/N on the
    // two front outputs)? It exists so that forcing mono is observable from
    // a test: removing the call to applyForceMono left the whole feature without
    // effect and the suite green.
    bool isVoiceForcedMono(int soundId) const;

    // 3D attenuation of the sound: below minDistance it plays at full
    // volume, and from there to maxDistance it falls off. It is written into the FMOD::Sound
    // (valid for future playbacks) AND into the live channel if there is one, with the
    // same check as the two setters above. No-op if the sound was not
    // loaded with FMOD_3D: in 2D there is no attenuation to adjust.
    void setSound3DMinMaxDistance(int soundId, float minDistance, float maxDistance);

    // Repositions the playing voice of soundId. It is what makes a 3D
    // sound FOLLOW its GameObject: playSound only writes the position once,
    // on start, so without this a moving object left the sound nailed where it
    // was when Play was pressed (neither attenuation nor panning
    // changed). No-op if the sound is not 3D or if there is no live voice, with the
    // same liveChannel check as the volume/pitch setters.
    // dt serves to derive the source velocity (doppler), as in
    // update() for the listener: with dt <= 0 the velocity goes to zero.
    void setSoundPosition(int soundId, const glm::vec3& worldPos, float dt = 0.0f);

    // Is there a live voice of this sound? The logic already existed inside the .cpp
    // (liveChannel, the guard against FMOD voice recycling) but it
    // was not exposed, so a script could not wait for a sound to
    // finish. NOTE: a PAUSED voice still counts as "playing" for FMOD,
    // just like in Unity; isSoundPaused is there to tell it apart.
    bool isSoundPlaying(int soundId) const;
    bool isSoundPaused (int soundId) const;

    // Pauses/resumes the live voice of the sound, keeping the playback
    // position — unlike stopSound, which throws it away. No-op if there is no
    // voice: it is not persistent component state, it is voice state.
    void setSoundPaused(int soundId, bool paused);

    // Silences the voice without touching its volume: on unmuting the value it
    // had comes back, without the script having to remember what it was. It is the
    // difference from setting the volume to 0, which is how it was done until now.
    void setSoundMute(int soundId, bool mute);

    // Playback position in SECONDS. It allows starting a clip halfway
    // and knowing where it is. -1 if there is no live voice: 0 would be a lie (that
    // is the start of the clip, not "nothing is playing").
    float getSoundTime(int soundId) const;
    void  setSoundTime(int soundId, float seconds);

    // --- Global pause ------------------------------------------------------
    //
    // Freezes EVERYTHING that plays, keeping the position: it is Unity's
    // AudioListener.pause, what a pause menu wants. It acts on
    // the master group, from which the other two buses hang.
    //
    // Do not confuse it with a timeScale: the engine has no simulation pause,
    // so this silences the audio but the scene keeps running if nobody else
    // stops it.
    void setAudioPaused(bool paused);
    bool isAudioPaused() const;

    // Volume per bus, [0, 1]. It is the knob the player expects to find in
    // the options: Master scales the other two because Music and Sfx hang from
    // it in FMOD. The getter exists so that the UI draws the real value and not
    // its own copy that could get out of sync.
    void  setBusVolume(AudioBus bus, float v);
    float getBusVolume(AudioBus bus) const;

    // --- Reverb zones ------------------------------------------------------
    //
    // A zone is an FMOD::Reverb3D: a sphere with a preset inside which
    // everything that plays gets that reverb. The mixing between overlapping zones and the
    // fade between min and max are done by FMOD, not by us.
    //
    // They are identified by the id of the owner GameObject, not by index: this way the
    // component stays pure data and the native resource has a single
    // owner. syncReverbZone creates the zone the first time and updates it afterwards,
    // so it can be called per frame without fear.
    //
    // preset: name of an FMOD_PRESET_* in lowercase (see reverbPresetNames).
    // An unknown one leaves the zone with the previous preset and returns false.
    bool syncReverbZone(uint64_t ownerId, const glm::vec3& worldPos,
                        float minDistance, float maxDistance,
                        const std::string& preset, bool enabled);

    // Destroys the zone of that GameObject. No-op if it had none.
    void removeReverbZone(uint64_t ownerId);

    // Destroys every zone whose id is NOT in the list. It is how the
    // zones of deleted GameObjects are collected: the manager does not see the scene, so it is the
    // scene that tells it who is still alive.
    void retainReverbZones(const std::vector<uint64_t>& aliveOwnerIds);

    // Destroys ALL of them. Called by scene loading: the zones of the
    // previous scene cannot survive into the new one.
    void clearReverbZones();

    // Live zones. Diagnostics and tests: without this, "the zone was created" and "the zone
    // was created and lost" are the same thing from outside.
    size_t reverbZoneCount() const;

    // Available presets, in the order in which the UI shows them. Static: the
    // list is the same with or without FMOD compiled, so that the editor can
    // draw the combo even if there is no audio.
    static const std::vector<std::string>& reverbPresetNames();

    // --- Per-bus effects ---------------------------------------------------
    //
    // They hang an FMOD DSP from the bus ChannelGroup, so they affect EVERYTHING
    // that goes out through it. Idempotent: requesting the same effect twice on the same
    // bus does not chain two copies.
    //
    // The parameter is the single knob of each effect, normalized to [0, 1] so
    // that the UI and Lua do not have to know the FMOD units:
    //   LowPass / HighPass -> cutoff frequency (0 = more closed, 1 = open)
    //   Echo               -> delay between repetitions
    //   Reverb             -> size of the tail
    // The exact mapping to FMOD units lives in the .cpp, in a single place.
    void setBusEffect(AudioBus bus, AudioEffect effect, float amount);

    // Removes the effect from the bus and releases its DSP. No-op if it was not set.
    void clearBusEffect(AudioBus bus, AudioEffect effect);

    // Removes ALL the effects of a bus. Used by the editor when changing scene.
    void clearBusEffects(AudioBus bus);

    // Diagnostics and tests: how many DSPs are hanging right now. Without this, "the
    // effect was applied" and "the effect was created and lost" are indistinguishable
    // from outside, and a DSP leak is not seen until the mixer chokes.
    size_t activeEffectCount() const;

    // DSPs really connected to a bus group, asking FMOD. It is NOT
    // the same as activeEffectCount, and the difference is exactly where the
    // leak lives: if setBusEffect chained a new DSP on every call instead of
    // readjusting the existing one, the map would still have ONE entry (the new one
    // overwrites the old one) while the group accumulates a hundred lost DSPs. I found out
    // by sabotaging idempotence and seeing that activeEffectCount did not notice.
    //
    // It includes the DSP that FMOD puts by default in each group (the fader), so the
    // absolute value means nothing: it is used by difference.
    size_t busDspCount(AudioBus bus) const;
    bool   hasBusEffect(AudioBus bus, AudioEffect effect) const;

    // Loads path with the given mode (is3D/loop baked into the FMOD_MODE) and
    // wraps the resulting soundId in an AudioClipComponent ready to
    // hang from a GameObject (GameObject::setAudioClip). nullptr if
    // loadSound fails (invalid file / not supported by FMOD).
    std::shared_ptr<AudioClipComponent> createAudioClipComponent(
        const std::string& path, bool is3D, bool loop,
        AudioLoadMode loadMode = AudioLoadMode::Sample);

private:
    // OUTSIDE the #ifdef on purpose: outputWarning() is public and is NOT
    // guarded, so the member has to exist in the build without
    // FMOD too (the Linux CI compiles without audio). Without FMOD nobody writes it and it
    // stays empty, which is exactly what the getter documents.
    std::string m_outputWarning;

#ifdef DT_FMOD_ENABLED
    // FMOD::ChannelGroup* of the bus, or the system master. nullptr without a system.
    void* groupForBus(AudioBus bus) const;

    // Key of the sound cache. It is NOT just the path: is3D and loop are baked
    // into the FMOD_MODE, so the same file loaded as 3D and as
    // 2D are two different FMOD::Sound and cannot be shared. Putting only the
    // path here would make checking "Is 3D?" on a clip silently change that of
    // another GameObject.
    static std::string soundKey(const std::string& path, bool is3D, bool loop,
                                 AudioLoadMode loadMode, AudioRolloff rolloff);

    // Loads and retains the sound of a path, returning its id. Single load
    // point of playClipAtPoint and preloadClip.
    int acquirePinnedSound(const std::string& path);

    void* m_system   = nullptr;  // FMOD::System*
    void* m_sfxGroup = nullptr;  // FMOD::ChannelGroup*
    void* m_musicGroup = nullptr; // FMOD::ChannelGroup* of the Music bus
    std::vector<void*> m_sounds;      // FMOD::Sound* SFX clips
    std::vector<void*> m_sfxChannels; // FMOD::Channel* of the last playback of each id (parallel to m_sounds)
    // Parallel to m_sounds. The path is the only thing useful to the user of the
    // warning (the id is internal), and the flag avoids repeating the same failure on every
    // frame — pollLoadFailures is called per frame.
    std::vector<std::string> m_soundPaths;
    std::vector<char>        m_soundFailureReported;
    // How many AudioClipComponents use each slot. The FMOD::Sound is only released
    // when it reaches zero: before, twenty objects with the same .wav loaded
    // twenty decompressed copies in RAM, and the first one to be destroyed
    // took away... nothing, because each had its own. Sharing,
    // releasing without counting would be a use-after-free for the other nineteen.
    std::vector<int>         m_soundRefs;
    // Key of each slot, to be able to erase the map entry when releasing it.
    std::vector<std::string> m_soundKeys;
    // Import settings of each sound's file (sidecar), and the LAST
    // volume the component requested for its voice (playSound / setChannelVolume).
    // Parallel to m_sounds and recycled with the slot. m_soundVolume exists so that
    // refreshImportSettings reapplies voiceVolume(id, componentVolume) instead of
    // multiplying the volume the channel already carries (it would compound the gain twice).
    // One-shots do NOT touch it: their voice cannot be reached.
    std::vector<AudioImportSettings> m_soundImport;
    std::vector<float>               m_soundVolume;

    // Effective volume of a voice of `id`: the one requested by the component times
    // the file gain. The ONLY place where the gain is multiplied.
    float voiceVolume(int id, float volume) const;

    // Free slots, to reuse them instead of growing without a ceiling: each
    // Play->Stop cycle recreates the whole scene and asked for new ids.
    std::vector<int>         m_freeSlots;
    std::unordered_map<std::string, int> m_soundByKey;
    // Sounds retained by playClipAtPoint/preloadClip. Without this set,
    // each shot would raise the refcount of the same sound again and the counter
    // would grow without a ceiling: the sound would never be released even if
    // unloadSound were called as many times as needed.
    std::unordered_set<int>  m_pinnedSounds;
    // Live DSPs, indexed by (bus, effect). The value is an FMOD::DSP* that has
    // to be disconnected AND released: they are native resources, not loose pointers, and
    // forgetting them is the classic leak of this kind of API.
    std::unordered_map<int, void*> m_busEffects;
    // FMOD::Reverb3D* per GameObject id. Native resources: they have to be
    // released, and that is why the component does not store them.
    std::unordered_map<uint64_t, void*> m_reverbZones;
    // Map key. bus and effect are small enums, so they fit with room to spare.
    static int effectKey(AudioBus bus, AudioEffect effect)
    {
        return (int)bus * 16 + (int)effect;
    }
    // Last known position of the listener and of each source, to derive the
    // velocity the doppler needs. m_hasLastListenerPos prevents the
    // first frame from inventing a huge velocity from the origin.
    glm::vec3                m_lastListenerPos{0.0f};
    bool                     m_hasLastListenerPos = false;
    std::vector<glm::vec3>   m_soundLastPos;   // parallel to m_sounds
    std::vector<char>        m_soundHasLastPos;
#endif
};

} // namespace DonTopo
