#pragma once
#include <string>

namespace DonTopo {

// Bus a sound goes out through. The three groups already existed inside
// AudioManager (the FMOD master, "BGM" and "SFX") but could not be chosen: every
// clip went out through SFX and nobody touched the volumes, so there was no way to
// lower the music without lowering the effects.
//
// Master is not just another destination: it is the parent group of the other two, and its volume
// scales both. It can be assigned to a clip all the same — sometimes you want something
// that ignores both the music and the effects knob, like a system voice.
//
// In its own header and not inside AudioManager.h because AudioClipComponent
// needs the type and must not drag in the whole manager: the forward
// declaration of AudioManager in that header is deliberate.
enum class AudioBus { Master, Music, Sfx };

// Stable name for the .scene and for the UI. By NAME and never by index:
// reordering the enum cannot change anybody's saved bus (same criterion
// as the ProjectContext::ViewSettings combos).
inline const char* audioBusToStr(AudioBus bus)
{
    switch (bus)
    {
        case AudioBus::Master: return "master";
        case AudioBus::Music:  return "music";
        case AudioBus::Sfx:
        default:               return "sfx";
    }
}

// Returns false if the name does not exist, leaving out untouched: the caller
// decides whether that deserves a warning (scene load) or silence.
inline bool audioBusFromStr(const std::string& name, AudioBus& out)
{
    if (name == "master") { out = AudioBus::Master; return true; }
    if (name == "music")  { out = AudioBus::Music;  return true; }
    if (name == "sfx")    { out = AudioBus::Sfx;    return true; }
    return false;
}

// How the file is brought into memory. It is Unity's "Load Type", and until now
// it could not be chosen: every clip was fully decompressed into RAM, so a three-minute
// mp3 put on an object ate tens of MB without warning. The
// only streaming in the engine was in the BGM API, which nobody used.
enum class AudioLoadMode {
    // Fully decompressed on load. It starts instantly and supports as many
    // simultaneous voices as wanted: it is what you want for effects.
    Sample,
    // Read and decoded from disk on the fly (FMOD_CREATESTREAM). It takes very
    // little RAM, in exchange for a bit of latency on start.
    //
    // LIMITATION, and that is why it is for music and not for effects: a stream has a
    // single decode buffer, so it CANNOT play twice at the same time.
    // Since clips with the same path and the same mode share a sound (the
    // AudioManager cache), two GameObjects with the same file in Stream
    // cannot play simultaneously either — the second Play cuts the first.
    // [verify against the FMOD API] the exact FMOD behavior when
    // asking for the second voice of a stream; what is stated here is the design.
    Stream
};

inline const char* audioLoadModeToStr(AudioLoadMode mode)
{
    return mode == AudioLoadMode::Stream ? "stream" : "sample";
}

inline bool audioLoadModeFromStr(const std::string& name, AudioLoadMode& out)
{
    if (name == "sample") { out = AudioLoadMode::Sample; return true; }
    if (name == "stream") { out = AudioLoadMode::Stream; return true; }
    return false;
}

// Shape of the attenuation curve between minDistance and maxDistance. Until now
// only the RANGE could be chosen, not the curve: FMOD always applied its
// factory one (inverse). It is baked into the sound's FMOD_MODE, like is3D and loop,
// so changing it reloads the clip.
// The three are constants that FMOD Core really has (FMOD_3D_*ROLLOFF); there
// is no "no attenuation" rolloff — for that you raise maxDistance, you do not choose
// a curve. Unity calls the inverse "Logarithmic".
enum class AudioRolloff {
    // FMOD's factory one: the volume drops fast near the source and
    // stretches out far away. The one that best imitates the real world.
    Inverse,
    // Drops at a constant rate and reaches ZERO exactly at maxDistance. The one you
    // want when "outside the radius it cannot be heard" has to hold literally.
    Linear,
    // Linear squared: similar to inverse in the near range, but
    // it also silences completely at maxDistance.
    LinearSquare
};

inline const char* audioRolloffToStr(AudioRolloff r)
{
    switch (r)
    {
        case AudioRolloff::Linear:       return "linear";
        case AudioRolloff::LinearSquare: return "linearSquare";
        case AudioRolloff::Inverse:
        default:                         return "inverse";
    }
}

inline bool audioRolloffFromStr(const std::string& name, AudioRolloff& out)
{
    if (name == "inverse")      { out = AudioRolloff::Inverse;      return true; }
    if (name == "linear")       { out = AudioRolloff::Linear;       return true; }
    if (name == "linearSquare") { out = AudioRolloff::LinearSquare; return true; }
    return false;
}

// Effects that can be hung on a bus. They are the DSP types that FMOD Core
// ships with (System::createDSPByType), so neither FMOD Studio
// nor banks are needed: that rules out snapshots and events, not filters.
//
// They hang from the BUS and not from each clip on purpose: a per-voice filter is paid
// per voice, and the real use case —"everything sounds muffled underwater", "the
// music loses bass in the pause menu"— is a group one.
enum class AudioEffect {
    // Cuts the highs above the cutoff frequency. It is the "I am underwater"
    // or "the sound comes from the next room" effect.
    LowPass,
    // Cuts the lows below the cutoff: radio voice, telephone.
    HighPass,
    // Spaced repetitions of the sound: cave, stadium PA.
    Echo,
    // Reverberant tail: the feeling of being in a large space.
    Reverb
};

inline const char* audioEffectToStr(AudioEffect e)
{
    switch (e)
    {
        case AudioEffect::HighPass: return "highPass";
        case AudioEffect::Echo:     return "echo";
        case AudioEffect::Reverb:   return "reverb";
        case AudioEffect::LowPass:
        default:                    return "lowPass";
    }
}

inline bool audioEffectFromStr(const std::string& name, AudioEffect& out)
{
    if (name == "lowPass")  { out = AudioEffect::LowPass;  return true; }
    if (name == "highPass") { out = AudioEffect::HighPass; return true; }
    if (name == "echo")     { out = AudioEffect::Echo;     return true; }
    if (name == "reverb")   { out = AudioEffect::Reverb;   return true; }
    return false;
}

// Is it an audio extension of those the engine accepts? It lives here, and not in the
// Properties panel, because there are FOUR paths through which audio comes in (the
// inspector dialog, its drop-zone, AddComponent from Lua and scene load)
// and the list only covered the first: through Lua or a hand-edited .scene
// anything got in, and with FMOD's asynchronous loading that ends up in a
// muted clip with no further explanation.
//
// The comparison is lowercase: the caller is in charge of lowering the extension
// beforehand (it is not done here so as not to drag <algorithm> into a header that includes
// half the audio module).
inline bool isSupportedAudioExtension(const std::string& lowercaseExt)
{
    return lowercaseExt == ".wav" || lowercaseExt == ".mp3"
        || lowercaseExt == ".ogg" || lowercaseExt == ".flac";
}

} // namespace DonTopo
