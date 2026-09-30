#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace DonTopo {

// Color space with which a material texture is interpreted. Auto = decided
// by the material slot (base color sRGB, normal/ORM linear), which is what
// happened before the setting existed.
enum class ColorSpaceOverride : uint8_t { Auto, Srgb, Linear };

struct TextureImportSettings
{
    ColorSpaceOverride colorSpace = ColorSpaceOverride::Auto;
    bool               mipmaps    = false;
};

inline bool operator==(const TextureImportSettings& a, const TextureImportSettings& b)
{
    return a.colorSpace == b.colorSpace && a.mipmaps == b.mipmaps;
}
inline bool isDefault(const TextureImportSettings& s) { return s == TextureImportSettings{}; }

// The setting lives in "<asset>.import.json", next to the asset, and only exists if
// it differs from the default.
inline constexpr const char* kImportSidecarSuffix = ".import.json";

std::filesystem::path importSidecarPath(const std::filesystem::path& asset);
bool                  isImportSidecar(const std::filesystem::path& p);

// Never throws. Absent = default and NO warning. Broken, of unknown version or type
// or larger than 64 KiB = default and `warning` explains why. An unknown value
// in a single field leaves the other fields read.
TextureImportSettings loadTextureImportSettings(const std::filesystem::path& asset,
                                                std::string* warning = nullptr);

// Saving the default DELETES the sidecar. false = it could not (and `error` says so).
bool saveTextureImportSettings(const std::filesystem::path& asset,
                               const TextureImportSettings& settings,
                               std::string* error = nullptr);

// ── Audio ────────────────────────────────────────────────────────────────────

// Properties of the audio FILE, not of the component: the gain is added to the
// volume of every voice of that clip and mono mixes its channels down to one.
struct AudioImportSettings
{
    float gainDb    = 0.0f;    // [kAudioGainMinDb, kAudioGainMaxDb]
    bool  forceMono = false;
};
inline constexpr float kAudioGainMinDb = -30.0f;
inline constexpr float kAudioGainMaxDb = 12.0f;

inline bool operator==(const AudioImportSettings& a, const AudioImportSettings& b)
{
    return a.gainDb == b.gainDb && a.forceMono == b.forceMono;
}
inline bool isDefault(const AudioImportSettings& s) { return s == AudioImportSettings{}; }

// Bounds to [-30, +12] dB; NaN -> 0. A gain without a ceiling, or a NaN put into an
// FMOD channel, deafens or silences without saying anything.
float clampAudioGainDb(float gainDb);
// 10^(dB/20), with the dB bounded first. 0 dB gives EXACTLY 1.0f.
float audioGainLinear(float gainDb);

// Same tolerance as the textures (see loadTextureImportSettings): never throws.
AudioImportSettings loadAudioImportSettings(const std::filesystem::path& asset,
                                            std::string* warning = nullptr);
// Saving the default DELETES the sidecar. The dB is written already bounded.
bool saveAudioImportSettings(const std::filesystem::path& asset,
                             const AudioImportSettings& settings,
                             std::string* error = nullptr);

// ── Models ───────────────────────────────────────────────────────────────────

// Where the mesh normals come from. File = those of the file (and flat if
// missing), which is what happened before the setting existed; Smooth and Flat
// ALWAYS REGENERATE them, even if the file brings them.
enum class NormalsMode : uint8_t { File, Smooth, Flat };

// Properties of the model FILE, not of the object: the GameObject's Transform.scale
// is separate and multiplies on top. The scale is baked into the
// geometry on import (FBX in cm versus m).
struct ModelImportSettings
{
    float       scale            = 1.0f;               // [kModelScaleMin, kModelScaleMax]
    NormalsMode normals          = NormalsMode::File;
    bool        calcTangents     = true;
    bool        flipUVs          = true;
    bool        importAnimations = true;
};
inline constexpr float kModelScaleMin = 0.001f;
inline constexpr float kModelScaleMax = 1000.0f;

inline bool operator==(const ModelImportSettings& a, const ModelImportSettings& b)
{
    return a.scale == b.scale && a.normals == b.normals && a.calcTangents == b.calcTangents &&
           a.flipUVs == b.flipUVs && a.importAnimations == b.importAnimations;
}
inline bool isDefault(const ModelImportSettings& s) { return s == ModelImportSettings{}; }

// NaN or <= 0 -> 1 (a scale of 0 would collapse the mesh); the rest is bounded to
// [0.001, 1000] (+inf gives 1000).
float clampModelScale(float scale);

// Same tolerance as textures and audio: never throws.
ModelImportSettings loadModelImportSettings(const std::filesystem::path& asset,
                                            std::string* warning = nullptr);
// Saving the default DELETES the sidecar. The scale is written already bounded.
bool saveModelImportSettings(const std::filesystem::path& asset,
                             const ModelImportSettings& settings,
                             std::string* error = nullptr);

// Two paths that name the same file (whether or not it exists). Existing ones: equivalent;
// otherwise, the weakly canonical form of each, and lastly the lexical one.
bool sameAssetPath(const std::filesystem::path& a, const std::filesystem::path& b);

// ── Sidecar life cycle: it travels with the asset ────────────────────────────

// true if the source asset AND the destination one already have a sidecar: moving one over
// the other would overwrite the destination's settings, so whoever moves or renames
// rejects it before touching anything.
bool importSidecarConflict(const std::filesystem::path& from, const std::filesystem::path& to);

// Moves the sidecar from oldAsset to newAsset. true if there was nothing to move.
bool moveImportSidecar(const std::filesystem::path& oldAsset, const std::filesystem::path& newAsset,
                       std::string* error = nullptr);
// Copies the sidecar from srcAsset to dstAsset (without overwriting an existing one). true if there was none.
bool copyImportSidecar(const std::filesystem::path& srcAsset, const std::filesystem::path& dstAsset,
                       std::string* error = nullptr);
// Deletes the sidecar of asset, if it exists. Silent.
void removeImportSidecar(const std::filesystem::path& asset);

} // namespace DonTopo
