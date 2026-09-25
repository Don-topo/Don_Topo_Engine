#pragma once
#include "DonTopo/Editor/Thumbnail.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace DonTopo {

// Subir SIEMPRE que cambie el aspecto de las miniaturas (rasterizador, luces,
// encuadre, reduccion de texturas): una cache vieja no sabe que esta obsoleta.
// 2: los .obj declaran sus .mtl como dependencia (las entradas de la 1 no los tienen).
inline constexpr uint32_t kThumbDiskVersion = 2;

// Miniaturas ya generadas, una por fichero en <proyecto>/.dt-cache/thumbs/, con
// la lista de dependencias y sus mtime. Se usa desde los workers: todo const y
// sin estado compartido salvo el aviso de "no se puede escribir", que es atomico.
class ThumbnailDiskCache
{
public:
    explicit ThumbnailDiskCache(std::filesystem::path dir);

    // La casilla guardada de asset si el fichero existe, la magia y la version
    // casan, la ruta guardada es la de asset y CADA dependencia sigue igual
    // (mismo mtime, o sigue sin existir). Nunca lanza.
    std::optional<ThumbnailResult> load(const std::filesystem::path& asset) const;

    // Escribe a un temporal unico y renombra encima. false (y un aviso por
    // stderr la primera vez) si no se pudo, o si r no trae dependencias selladas.
    bool store(const std::filesystem::path& asset, const ThumbnailResult& r) const;

    // <dir>/<fnv1a64 de la ruta absoluta normalizada, 16 hex>.bin
    std::filesystem::path        fileFor(const std::filesystem::path& asset) const;
    const std::filesystem::path& directory() const { return m_dir; }

private:
    std::filesystem::path     m_dir;
    mutable std::atomic<bool> m_warned{ false };
};

} // namespace DonTopo
