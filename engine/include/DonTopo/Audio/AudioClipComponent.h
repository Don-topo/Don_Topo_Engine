#pragma once
#include <string>
#include <glm/glm.hpp>

#include "DonTopo/Audio/AudioBus.h"

namespace DonTopo {

class AudioManager;

// Unique audio component per GameObject. It wraps an AudioManager soundId;
// loop/is3D are baked into the sound's FMOD_MODE, so
// changing them reloads the clip (unloadSound + loadSound) instead of
// mutating the existing sound.
class AudioClipComponent {
public:
    AudioClipComponent(AudioManager* audio, std::string path, int soundId, bool is3D, bool loop,
                        AudioLoadMode loadMode = AudioLoadMode::Sample);
    ~AudioClipComponent();

    AudioClipComponent(const AudioClipComponent&)            = delete;
    AudioClipComponent& operator=(const AudioClipComponent&) = delete;

    void play(const glm::vec3& worldPos);
    void stop();

    // Fires a loose voice that overlaps whatever is already playing, instead of
    // cutting it as play() does. It uses the component's volume and pitch, but
    // the resulting voice ends up out of its reach: stop(), setVolume/setPitch and
    // isPlaying() do not see it, and neither does the per-frame 3D tracking. For short
    // clips (footsteps, shots, impacts), not for loops.
    void playOneShot(const glm::vec3& worldPos);

    // State of the voice, not of the component: it is not serialized and does not survive a
    // stop. isPlaying() follows the FMOD and Unity criterion — a paused voice
    // counts as playing; isPaused() is what tells them apart.
    bool isPlaying() const;
    bool isPaused()  const;
    // They keep the playback position, unlike stop().
    void pause();
    void resume();

    // Silence WITHOUT losing the volume: when unmuting, the previous one comes back, without
    // anybody having to remember it. It is what was done by hand until now, setting
    // the volume to 0 and storing it aside.
    //
    // Unlike pause/isPlaying, this IS component state and it is
    // serialized: an object can be born muted.
    bool getMute() const { return m_mute; }
    void setMute(bool mute);

    // Playback position in seconds. -1 if nothing is playing: 0 would be
    // a lie, because that is the start of the clip. setTime on something that is not
    // playing is a no-op — it does not start playback, it only moves the one there is.
    float getTime() const;
    void  setTime(float seconds);

    // Pushes the owner's position to the voice that is playing, so that a 3D clip
    // follows the GameObject instead of staying where it was when play() was called.
    // Meant to be called once per frame (Scene::update does it); cheap no-op
    // if the clip is 2D or if nothing is playing.
    void updateSpatial(const glm::vec3& worldPos, float dt = 0.0f);

    // No-op if the value does not change (avoids reloading the sound every frame).
    void setLoop(bool loop);
    void setIs3D(bool is3D);

    // Volume and pitch are properties of the CHANNEL, not of the sound's FMOD_MODE:
    // unlike setLoop/setIs3D, they reload nothing and can be moved
    // while it plays. The clamp lives here so that neither the UI nor Lua can
    // sneak in an out-of-range value.
    void setVolume(float volume);   // [0, 1]
    void setPitch (float pitch);    // [0.5, 2]

    // Attenuation of the 3D clip: closer than minDistance to the listener it sounds at
    // full volume, and from there to maxDistance it falls off. Like volume and
    // pitch, they do not reload the sound. They do nothing in FMOD if the clip is 2D
    // (the value is still stored: when is3D is checked it is applied without losing what was edited).
    void setMinDistance(float d);   // [0.1, 50]
    void setMaxDistance(float d);   // [1, 1000], never below min

    // The file could not be opened (absent, unsupported format, corrupt
    // data). It is not known when the component is built: FMOD loads on its thread
    // and the failure shows up frames later, so this is queried, not
    // cached. Defined in the .cpp so as not to drag AudioManager.h into the header.
    bool hasLoadError() const;

    // Output bus. It does not touch the FMOD_MODE, so changing it does NOT reload the
    // sound — but it is a channel property: it only takes effect on the next
    // playback, because the group is chosen when the voice starts.
    AudioBus getBus() const { return m_bus; }
    void setBus(AudioBus bus) { m_bus = bus; }

    // Load mode. Like loop and is3D, it is baked into the sound's FMOD_MODE:
    // changing it RELOADS the clip and cuts whatever was playing.
    AudioLoadMode getLoadMode() const { return m_loadMode; }
    void setLoadMode(AudioLoadMode mode);

    // Attenuation curve. It is also in the FMOD_MODE: it reloads the clip.
    AudioRolloff getRolloff() const { return m_rolloff; }
    void setRolloff(AudioRolloff rolloff);

    // The three below are properties of the VOICE, not of the sound: they reload
    // nothing, but they are applied when playback starts, so changing them with
    // something already playing has no audible effect until the next Play.
    //
    // spread: stereo widening of a 3D source, in degrees [0, 360]. 0 leaves it
    // as a point (as it always was).
    float getSpread() const { return m_spread; }
    void setSpread(float degrees);

    // stereoPan: manual pan [-1, 1] (left to right). It only has an effect
    // on 2D clips — in 3D the panning is decided by the position.
    float getStereoPan() const { return m_stereoPan; }
    void setStereoPan(float pan);

    // dopplerLevel: how much the relative velocity alters the pitch, [0, 5]. 0 turns it
    // off, which is the default value — doppler is surprising if it shows up
    // without anybody having asked for it.
    float getDopplerLevel() const { return m_dopplerLevel; }
    void setDopplerLevel(float level);

    bool getLoop() const  { return m_loop; }
    bool getIs3D() const  { return m_is3D; }
    const std::string& getPath() const { return m_path; }
    float getVolume() const { return m_volume; }
    float getPitch()  const { return m_pitch;  }
    float getMinDistance() const { return m_minDistance; }
    float getMaxDistance() const { return m_maxDistance; }

    // If active, Play Mode calls play() automatically on entering
    // (see EditorUI::drawToolbar). It does not affect the FMOD_MODE, no reload needed.
    bool getPlayOnAwake() const { return m_playOnAwake; }
    void setPlayOnAwake(bool playOnAwake) { m_playOnAwake = playOnAwake; }
    // Updates only the path bookkeeping (e.g. after a rename on disk);
    // the FMOD sound already loaded does not change contents, no reload needed.
    void setPath(const std::string& path) { m_path = path; }

private:
    void reload();
    // Pushes min/max to the FMOD sound. Called by both setters, the constructor
    // and reload(): a newly created sound starts with FMOD's default min/max
    // (1 / 10000), not with this component's.
    void applyDistances();

    AudioManager* m_audio;
    std::string   m_path;
    int           m_soundId;
    bool          m_is3D;
    bool          m_loop;
    bool          m_playOnAwake = false;
    // Sfx by default: it is where EVERYTHING went out before the buses existed,
    // so an old scene sounds the same.
    AudioBus      m_bus = AudioBus::Sfx;
    // Sample by default: it is how EVERYTHING was loaded before it could be
    // chosen, so an old scene behaves the same.
    AudioLoadMode m_loadMode = AudioLoadMode::Sample;
    // Inverse is FMOD's factory curve: an old scene attenuates the same.
    AudioRolloff  m_rolloff = AudioRolloff::Inverse;
    float         m_spread = 0.0f;
    float         m_stereoPan = 0.0f;
    // Zero, not one as in Unity: turning doppler on by default would change the
    // pitch of everything already playing in existing scenes.
    float         m_dopplerLevel = 0.0f;
    // Serialized, unlike pause: an object can be born muted.
    bool          m_mute = false;
    float         m_volume = 1.0f;
    float         m_pitch  = 1.0f;
    // Defaults to this repo's scale (50-unit primitives), not to
    // FMOD's.
    float         m_minDistance = 1.0f;
    float         m_maxDistance = 100.0f;
};

} // namespace DonTopo
