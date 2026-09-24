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
