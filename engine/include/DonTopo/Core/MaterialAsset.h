#pragma once
#include <filesystem>
#include <string>

namespace DonTopo {

// A reusable material, referenced by path from an object slot (see
// MaterialOverride::matAsset). An empty/-1 field means "inherit from the model":
// a .mat with only roughness leaves the three textures and the metallic intact.
struct MaterialAsset
{
    std::string albedo, normal, orm;   // absolute in memory; "" = inherit
    float       metallic  = -1.0f;     // -1 = inherit; otherwise [0, 1]
    float       roughness = -1.0f;
};

inline bool operator==(const MaterialAsset& a, const MaterialAsset& b)
{
    return a.albedo == b.albedo && a.normal == b.normal && a.orm == b.orm &&
           a.metallic == b.metallic && a.roughness == b.roughness;
}
inline bool isDefault(const MaterialAsset& m) { return m == MaterialAsset{}; }

// Never throws. Absent = inherit everything, WITHOUT a warning (it is the normal case: a slot may not
// have a .mat). Broken, of unknown version/type, larger than 64 KiB, or a field with
// the wrong type -> that field (or the whole file) inherits, WITH a warning.
MaterialAsset loadMaterialAsset(const std::filesystem::path& mat, std::string* warning = nullptr);

// ALWAYS writes the file, even if `asset` is the default (a .mat is a named
// asset, not an optional setting that disappears). false = it could not, and
// `error` says why.
bool saveMaterialAsset(const std::filesystem::path& mat, const MaterialAsset& asset,
                       std::string* error = nullptr);

} // namespace DonTopo
