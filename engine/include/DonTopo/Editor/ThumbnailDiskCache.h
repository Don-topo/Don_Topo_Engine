#pragma once
#include "DonTopo/Editor/Thumbnail.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace DonTopo {

// ALWAYS bump when the look of the thumbnails changes (rasterizer, lights,
// framing, texture reduction): an old cache does not know it is stale.
// 2: .obj files declare their .mtl as a dependency (entries of 1 do not have them).
// 3: .gltf files declare their .bin and images (modelCompanionFiles).
// 4: the preview of a static model draws all its pieces.
// 5: glTF files apply the root transform.
inline constexpr uint32_t kThumbDiskVersion = 5;

// Thumbnails already generated, one per file in <project>/.dt-cache/thumbs/, with
// the list of dependencies and their mtime. Used from the workers: everything const and
// with no shared state except the "cannot write" warning, which is atomic.
class ThumbnailDiskCache
{
public:
    explicit ThumbnailDiskCache(std::filesystem::path dir);

    // The stored cell of asset if the file exists, the magic and the version
    // match, the stored path is that of asset and EVERY dependency is still the same
    // (same mtime, or still nonexistent). Never throws.
    std::optional<ThumbnailResult> load(const std::filesystem::path& asset) const;

    // Writes to a unique temporary and renames over it. false (and a warning on
    // stderr the first time) if it could not, or if r carries no sealed dependencies.
    bool store(const std::filesystem::path& asset, const ThumbnailResult& r) const;

    // <dir>/<fnv1a64 of the normalized absolute path, 16 hex>.bin
    std::filesystem::path        fileFor(const std::filesystem::path& asset) const;
    const std::filesystem::path& directory() const { return m_dir; }

private:
    std::filesystem::path     m_dir;
    mutable std::atomic<bool> m_warned{ false };
};

} // namespace DonTopo
