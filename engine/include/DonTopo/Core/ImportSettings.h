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
