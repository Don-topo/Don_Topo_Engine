#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace DonTopo {

// Espacio de color con el que se interpreta una textura de material. Auto = lo
// decide el slot del material (color base sRGB, normal/ORM lineal), que es lo
// que pasaba antes de que existiera el ajuste.
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

// El ajuste vive en "<asset>.import.json", junto al asset, y solo existe si
// difiere del defecto.
inline constexpr const char* kImportSidecarSuffix = ".import.json";

std::filesystem::path importSidecarPath(const std::filesystem::path& asset);
bool                  isImportSidecar(const std::filesystem::path& p);

// Nunca lanza. Ausente = defecto y SIN aviso. Roto, de version o tipo
// desconocido o mayor de 64 KiB = defecto y `warning` explica por que. Un valor
// desconocido en un campo suelto deja los demas campos leidos.
TextureImportSettings loadTextureImportSettings(const std::filesystem::path& asset,
                                                std::string* warning = nullptr);

// Guardar el defecto BORRA el sidecar. false = no se pudo (y `error` lo dice).
bool saveTextureImportSettings(const std::filesystem::path& asset,
                               const TextureImportSettings& settings,
                               std::string* error = nullptr);

// ── Audio ────────────────────────────────────────────────────────────────────

// Propiedades del FICHERO de audio, no del componente: la ganancia se suma al
// volumen de toda voz de ese clip y el mono mezcla sus canales a uno.
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

// Acota a [-30, +12] dB; NaN -> 0. Una ganancia sin techo, o NaN metida en un
// canal de FMOD, ensordece o silencia sin decir nada.
float clampAudioGainDb(float gainDb);
// 10^(dB/20), con el dB acotado antes. 0 dB da EXACTAMENTE 1.0f.
float audioGainLinear(float gainDb);

// Misma tolerancia que las texturas (ver loadTextureImportSettings): nunca lanza.
AudioImportSettings loadAudioImportSettings(const std::filesystem::path& asset,
                                            std::string* warning = nullptr);
// Guardar el defecto BORRA el sidecar. El dB se escribe ya acotado.
bool saveAudioImportSettings(const std::filesystem::path& asset,
                             const AudioImportSettings& settings,
                             std::string* error = nullptr);

// ── Modelos ──────────────────────────────────────────────────────────────────

// De donde salen las normales de la malla. File = las del fichero (y planas si
// faltan), que es lo que pasaba antes de que existiera el ajuste; Smooth y Flat
// las REGENERAN siempre, aunque el fichero las traiga.
enum class NormalsMode : uint8_t { File, Smooth, Flat };

// Propiedades del FICHERO del modelo, no del objeto: el Transform.scale del
// GameObject va aparte y se multiplica encima. La escala se hornea en la
// geometria al importar (FBX en cm frente a m).
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

// NaN o <= 0 -> 1 (una escala 0 colapsaria la malla); el resto se acota a
// [0.001, 1000] (+inf da 1000).
float clampModelScale(float scale);

// Misma tolerancia que texturas y audio: nunca lanza.
ModelImportSettings loadModelImportSettings(const std::filesystem::path& asset,
                                            std::string* warning = nullptr);
// Guardar el defecto BORRA el sidecar. La escala se escribe ya acotada.
bool saveModelImportSettings(const std::filesystem::path& asset,
                             const ModelImportSettings& settings,
                             std::string* error = nullptr);

// Dos rutas que nombran el mismo fichero (exista o no). Existentes: equivalent;
// si no, la forma canonica debil de cada una, y en ultimo caso la lexica.
bool sameAssetPath(const std::filesystem::path& a, const std::filesystem::path& b);

// ── Ciclo de vida del sidecar: viaja con el asset ────────────────────────────

// true si el asset de origen Y el de destino ya tienen sidecar: mover uno encima
// del otro pisaria los ajustes del destino, asi que quien mueve o renombra lo
// rechaza antes de tocar nada.
bool importSidecarConflict(const std::filesystem::path& from, const std::filesystem::path& to);

// Mueve el sidecar de oldAsset a newAsset. true si no habia nada que mover.
bool moveImportSidecar(const std::filesystem::path& oldAsset, const std::filesystem::path& newAsset,
                       std::string* error = nullptr);
// Copia el sidecar de srcAsset a dstAsset (sin pisar uno existente). true si no habia.
bool copyImportSidecar(const std::filesystem::path& srcAsset, const std::filesystem::path& dstAsset,
                       std::string* error = nullptr);
// Borra el sidecar de asset, si existe. Silencioso.
void removeImportSidecar(const std::filesystem::path& asset);

} // namespace DonTopo
