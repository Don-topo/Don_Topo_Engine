#include "DonTopo/Core/MaterialAsset.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>

namespace DonTopo {

namespace {
constexpr std::uintmax_t kMaxMaterialAssetBytes = 64 * 1024;

// Path relative to `base` if possible (with ".." if it has to go up), or
// absolute as is if it cannot be expressed relatively (another drive).
std::string toRelativeToFolder(const std::string& path, const std::filesystem::path& base)
{
    if (path.empty()) return path;
    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(path, base, ec);
    if (ec || rel.empty()) return path;
    return rel.generic_string();
}

// The inverse: relative to `base`, or already absolute.
std::string fromRelativeToFolder(const std::string& stored, const std::filesystem::path& base)
{
    if (stored.empty()) return stored;
    std::filesystem::path p(stored);
    if (p.is_absolute()) return stored;
    return std::filesystem::weakly_canonical(base / p).string();
}

float clampFactor(float v)
{
    if (!std::isfinite(v)) return -1.0f;   // not finite: as if it were not set
    return std::clamp(v, 0.0f, 1.0f);
}
} // namespace

MaterialAsset loadMaterialAsset(const std::filesystem::path& mat, std::string* warning)
{
    MaterialAsset out;
    if (warning) warning->clear();
    auto warn = [&](const std::string& m) { if (warning) *warning = m; };

    std::error_code ec;
    if (!std::filesystem::is_regular_file(mat, ec) || ec)
    {
        // Although the spec counts it together with "unreadable", it IS distinguished from the
        // rest: applyMaterialOverrides calls this function without `warning`
        // (nullptr), so `warn` does nothing on the hot path — the
        // cost only exists when collectMaterialOverrideWarnings (which only
        // runs when loading the scene) asks for it.
        warn("missing material; everything is inherited from the model");
        return out;
    }

    const std::uintmax_t size = std::filesystem::file_size(mat, ec);
    if (ec || size > kMaxMaterialAssetBytes)
    {
        warn("material unreadable or too large; everything is inherited from the model");
        return out;
    }
    if (size == 0) return out;

    std::ifstream in(mat, std::ios::binary);
    if (!in) { warn("could not open the material; everything is inherited from the model"); return out; }
    const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };

    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object())
    {
        warn("invalid JSON; everything is inherited from the model");
        return out;
    }
    const auto version = j.find("version");
    if (version == j.end() || !version->is_number_integer() || version->get<long long>() != 1)
    {
        warn("unknown material version; everything is inherited from the model");
        return out;
    }
    const auto type = j.find("type");
    if (type == j.end() || !type->is_string() || type->get<std::string>() != "material")
    {
        warn("the file is not of type material; everything is inherited from the model");
        return out;
    }

    const std::filesystem::path folder = mat.parent_path();
    std::string problems;
    auto readPath = [&](const char* key, std::string& dst)
    {
        const auto it = j.find(key);
        if (it == j.end()) return;
        if (it->is_string()) dst = fromRelativeToFolder(it->get<std::string>(), folder);
        else problems += std::string(key) + " is not a valid path (inherited). ";
    };
    readPath("albedo", out.albedo);
    readPath("normal", out.normal);
    readPath("orm",    out.orm);

    auto readFactor = [&](const char* key, float& dst)
    {
        const auto it = j.find(key);
        if (it == j.end()) return;
        if (!it->is_number()) { problems += std::string(key) + " is not numeric (inherited). "; return; }
        const double v = it->get<double>();
        if (!std::isfinite(v)) { problems += std::string(key) + " is not finite (inherited). "; return; }
        dst = clampFactor(static_cast<float>(v));
        if (static_cast<double>(dst) != v)
            problems += std::string(key) + " out of range (clamped). ";
    };
    readFactor("metallic",  out.metallic);
    readFactor("roughness", out.roughness);

    if (!problems.empty()) warn(problems);
    return out;
}

bool saveMaterialAsset(const std::filesystem::path& mat, const MaterialAsset& asset, std::string* error)
{
    const std::filesystem::path folder = mat.parent_path();
    nlohmann::json j;
    j["version"]  = 1;
    j["type"]     = "material";
    if (!asset.albedo.empty()) j["albedo"] = toRelativeToFolder(asset.albedo, folder);
    if (!asset.normal.empty()) j["normal"] = toRelativeToFolder(asset.normal, folder);
    if (!asset.orm.empty())    j["orm"]    = toRelativeToFolder(asset.orm,    folder);
    if (asset.metallic  >= 0.0f) j["metallic"]  = asset.metallic;
    if (asset.roughness >= 0.0f) j["roughness"] = asset.roughness;

    std::error_code ec;
    std::filesystem::path tmp = mat;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { if (error) *error = "could not write " + mat.string(); return false; }
        out << j.dump(2) << '\n';
        if (!out)
        {
            if (error) *error = "incomplete write of " + mat.string();
            out.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::rename(tmp, mat, ec);
    if (ec)
    {
        if (error) *error = "could not rename to " + mat.string() + ": " + ec.message();
        std::error_code rmEc;
        std::filesystem::remove(tmp, rmEc);
        return false;
    }
    return true;
}

} // namespace DonTopo
