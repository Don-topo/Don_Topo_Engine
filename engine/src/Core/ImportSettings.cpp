#include "DonTopo/Core/ImportSettings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>

namespace DonTopo {

namespace {

// A legitimate sidecar is ~100 bytes. The cap exists so that a file of
// megabytes, or a JSON nested 100000 levels deep, never reaches the parser.
constexpr std::uintmax_t kMaxSidecarBytes = 64 * 1024;

const char* normalsModeName(NormalsMode m)
{
    switch (m)
    {
        case NormalsMode::Smooth: return "smooth";
        case NormalsMode::Flat:   return "flat";
        case NormalsMode::File:   break;
    }
    return "file";
}

const char* colorSpaceName(ColorSpaceOverride c)
{
    switch (c)
    {
        case ColorSpaceOverride::Srgb:   return "srgb";
        case ColorSpaceOverride::Linear: return "linear";
        case ColorSpaceOverride::Auto:   break;
    }
    return "auto";
}

// Reads and validates the common ENVELOPE of a sidecar (existence, size, JSON,
// version and type). nullopt = use the default: `warning` explains why unless
// the file simply does not exist or is empty (the normal case, no warning).
std::optional<nlohmann::json> readSidecar(const std::filesystem::path& asset,
                                          const char* expectedType, std::string* warning)
{
    if (warning) warning->clear();
    auto warn = [&](const std::string& m) { if (warning) *warning = m; };

    const std::filesystem::path sidecar = importSidecarPath(asset);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(sidecar, ec) || ec)
        return std::nullopt;

    const std::uintmax_t size = std::filesystem::file_size(sidecar, ec);
    if (ec || size > kMaxSidecarBytes)
    {
        warn("sidecar unreadable or too large; using the default values");
        return std::nullopt;
    }
    if (size == 0)
        return std::nullopt;

    std::ifstream in(sidecar, std::ios::binary);
    if (!in)
    {
        warn("could not open the sidecar; using the default values");
        return std::nullopt;
    }
    const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };

    nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object())
    {
        warn("invalid JSON; using the default values");
        return std::nullopt;
    }
    const auto version = j.find("version");
    if (version == j.end() || !version->is_number_integer() || version->get<long long>() != 1)
    {
        warn("unknown sidecar version; using the default values");
        return std::nullopt;
    }
    const auto type = j.find("type");
    if (type == j.end() || !type->is_string() || type->get<std::string>() != expectedType)
    {
        warn(std::string("the sidecar is not of type ") + expectedType +
             "; using the default values");
        return std::nullopt;
    }
    return j;
}

// Write through a temporary file + rename: a cut halfway leaves no half-written sidecar.
bool writeSidecar(const std::filesystem::path& asset, const nlohmann::json& j, std::string* error)
{
    const std::filesystem::path sidecar = importSidecarPath(asset);
    std::error_code ec;
    std::filesystem::path tmp = sidecar;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            if (error) *error = "could not write " + sidecar.string();
            return false;
        }
        out << j.dump(2) << '\n';
        if (!out)
        {
            if (error) *error = "incomplete write of " + sidecar.string();
            out.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::rename(tmp, sidecar, ec);
    if (ec)
    {
        if (error) *error = "could not rename to " + sidecar.string() + ": " + ec.message();
        std::error_code rmEc;
        std::filesystem::remove(tmp, rmEc);
        return false;
    }
    return true;
}

// Saving the default = removing the sidecar; absent is not an error either.
bool removeSidecar(const std::filesystem::path& asset, std::string* error)
{
    std::error_code ec;
    const std::filesystem::path sidecar = importSidecarPath(asset);
    std::filesystem::remove(sidecar, ec);
    if (ec)
    {
        if (error) *error = "could not delete " + sidecar.string() + ": " + ec.message();
        return false;
    }
    return true;
}

} // namespace

std::filesystem::path importSidecarPath(const std::filesystem::path& asset)
{
    std::filesystem::path p = asset;
    p += kImportSidecarSuffix;
    return p;
}

bool isImportSidecar(const std::filesystem::path& p)
{
    const auto name = p.filename().native();
    const auto suffix = std::filesystem::path(kImportSidecarSuffix).native();
    if (name.size() <= suffix.size()) return false;
    const size_t offset = name.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i)
    {
        auto c = name[offset + i];
        if (c >= 'A' && c <= 'Z') c = static_cast<decltype(c)>(c + 32);
        if (c != suffix[i]) return false;
    }
    return true;
}

TextureImportSettings loadTextureImportSettings(const std::filesystem::path& asset,
                                                std::string* warning)
{
    TextureImportSettings out;
    const std::optional<nlohmann::json> j = readSidecar(asset, "texture", warning);
    if (!j) return out;

    std::string problems;
    if (const auto it = j->find("colorSpace"); it != j->end())
    {
        const std::string v = it->is_string() ? it->get<std::string>() : std::string();
        if      (v == "auto")   out.colorSpace = ColorSpaceOverride::Auto;
        else if (v == "srgb")   out.colorSpace = ColorSpaceOverride::Srgb;
        else if (v == "linear") out.colorSpace = ColorSpaceOverride::Linear;
        else problems += "unknown colorSpace (using auto). ";
    }
    if (const auto it = j->find("mipmaps"); it != j->end())
    {
        if (it->is_boolean()) out.mipmaps = it->get<bool>();
        else                  problems += "mipmaps is not a boolean (using false). ";
    }
    if (!problems.empty() && warning) *warning = problems;
    return out;
}

bool saveTextureImportSettings(const std::filesystem::path& asset,
                               const TextureImportSettings& settings, std::string* error)
{
    if (isDefault(settings))
        return removeSidecar(asset, error);

    nlohmann::json j;
    j["version"]    = 1;
    j["type"]       = "texture";
    j["colorSpace"] = colorSpaceName(settings.colorSpace);
    j["mipmaps"]    = settings.mipmaps;
    return writeSidecar(asset, j, error);
}

float clampAudioGainDb(float gainDb)
{
    if (std::isnan(gainDb)) return 0.0f;
    return std::clamp(gainDb, kAudioGainMinDb, kAudioGainMaxDb);
}

float audioGainLinear(float gainDb)
{
    const float db = clampAudioGainDb(gainDb);
    if (db == 0.0f) return 1.0f;                       // exact: without adjustment the volume is not touched
    return std::pow(10.0f, db / 20.0f);
}

AudioImportSettings loadAudioImportSettings(const std::filesystem::path& asset, std::string* warning)
{
    AudioImportSettings out;
    const std::optional<nlohmann::json> j = readSidecar(asset, "audio", warning);
    if (!j) return out;

    std::string problems;
    if (const auto it = j->find("gainDb"); it != j->end())
    {
        if (it->is_number())
        {
            const double v = it->get<double>();
            if (!std::isfinite(v))
                problems += "gainDb is not finite (using 0). ";
            else
            {
                out.gainDb = clampAudioGainDb(static_cast<float>(v));
                if (static_cast<double>(out.gainDb) != v)
                    problems += "gainDb out of range (clamped). ";
            }
        }
        else problems += "gainDb is not numeric (using 0). ";
    }
    if (const auto it = j->find("forceMono"); it != j->end())
    {
        if (it->is_boolean()) out.forceMono = it->get<bool>();
        else                  problems += "forceMono is not a boolean (using false). ";
    }
    if (!problems.empty() && warning) *warning = problems;
    return out;
}

bool saveAudioImportSettings(const std::filesystem::path& asset, const AudioImportSettings& settings,
                             std::string* error)
{
    AudioImportSettings s = settings;
    s.gainDb = clampAudioGainDb(s.gainDb);
    if (isDefault(s))
        return removeSidecar(asset, error);

    nlohmann::json j;
    j["version"]   = 1;
    j["type"]      = "audio";
    j["gainDb"]    = s.gainDb;
    j["forceMono"] = s.forceMono;
    return writeSidecar(asset, j, error);
}

float clampModelScale(float scale)
{
    if (std::isnan(scale) || scale <= 0.0f) return 1.0f;
    return std::clamp(scale, kModelScaleMin, kModelScaleMax);
}

ModelImportSettings loadModelImportSettings(const std::filesystem::path& asset, std::string* warning)
{
    ModelImportSettings out;
    const std::optional<nlohmann::json> j = readSidecar(asset, "model", warning);
    if (!j) return out;

    std::string problems;
    if (const auto it = j->find("scale"); it != j->end())
    {
        if (it->is_number())
        {
            // In double and with literals: kModelScaleMin (float) as double is not
            // exactly 0.001, and a hand-written 0.001 would come out "out of range".
            const double v = it->get<double>();
            if (std::isnan(v) || v <= 0.0)
                problems += "scale is not positive (using 1). ";
            else if (v > 1000.0)
            {
                out.scale = kModelScaleMax;
                problems += "scale out of range (clamped). ";
            }
            else if (v < 0.001)
            {
                out.scale = kModelScaleMin;
                problems += "scale out of range (clamped). ";
            }
            else out.scale = static_cast<float>(v);
        }
        else problems += "scale is not numeric (using 1). ";
    }
    if (const auto it = j->find("normals"); it != j->end())
    {
        const std::string v = it->is_string() ? it->get<std::string>() : std::string();
        if      (v == "file")   out.normals = NormalsMode::File;
        else if (v == "smooth") out.normals = NormalsMode::Smooth;
        else if (v == "flat")   out.normals = NormalsMode::Flat;
        else problems += "unknown normals (using file). ";
    }
    auto readBool = [&](const char* key, bool& dst)
    {
        const auto it = j->find(key);
        if (it == j->end()) return;
        if (it->is_boolean()) dst = it->get<bool>();
        else problems += std::string(key) + " is not a boolean (using the default). ";
    };
    readBool("calcTangents",     out.calcTangents);
    readBool("flipUVs",          out.flipUVs);
    readBool("importAnimations", out.importAnimations);

    if (!problems.empty() && warning) *warning = problems;
    return out;
}

bool saveModelImportSettings(const std::filesystem::path& asset, const ModelImportSettings& settings,
                             std::string* error)
{
    ModelImportSettings s = settings;
    s.scale = clampModelScale(s.scale);
    if (isDefault(s))
        return removeSidecar(asset, error);

    nlohmann::json j;
    j["version"]          = 1;
    j["type"]             = "model";
    j["scale"]            = s.scale;
    j["normals"]          = normalsModeName(s.normals);
    j["calcTangents"]     = s.calcTangents;
    j["flipUVs"]          = s.flipUVs;
    j["importAnimations"] = s.importAnimations;
    return writeSidecar(asset, j, error);
}

bool sameAssetPath(const std::filesystem::path& a, const std::filesystem::path& b)
{
    if (a.empty() || b.empty()) return false;
    std::error_code ec;
    if (std::filesystem::equivalent(a, b, ec) && !ec) return true;
    ec.clear();
    const std::filesystem::path ca = std::filesystem::weakly_canonical(a, ec);
    if (ec) return a.lexically_normal() == b.lexically_normal();
    ec.clear();
    const std::filesystem::path cb = std::filesystem::weakly_canonical(b, ec);
    if (ec) return a.lexically_normal() == b.lexically_normal();
    return ca == cb;
}

bool importSidecarConflict(const std::filesystem::path& from, const std::filesystem::path& to)
{
    std::error_code a, b;
    return std::filesystem::exists(importSidecarPath(from), a) && !a &&
           std::filesystem::exists(importSidecarPath(to), b) && !b;
}

bool moveImportSidecar(const std::filesystem::path& oldAsset, const std::filesystem::path& newAsset,
                       std::string* error)
{
    const std::filesystem::path from = importSidecarPath(oldAsset);
    std::error_code ec;
    if (!std::filesystem::exists(from, ec) || ec) return true;        // nothing to move
    std::filesystem::rename(from, importSidecarPath(newAsset), ec);
    if (ec) { if (error) *error = ec.message(); return false; }
    return true;
}

bool copyImportSidecar(const std::filesystem::path& srcAsset, const std::filesystem::path& dstAsset,
                       std::string* error)
{
    const std::filesystem::path from = importSidecarPath(srcAsset);
    std::error_code ec;
    if (!std::filesystem::exists(from, ec) || ec) return true;
    std::filesystem::copy_file(from, importSidecarPath(dstAsset),
                               std::filesystem::copy_options::skip_existing, ec);
    if (ec) { if (error) *error = ec.message(); return false; }
    return true;
}

void removeImportSidecar(const std::filesystem::path& asset)
{
    std::error_code ec;
    std::filesystem::remove(importSidecarPath(asset), ec);
}

} // namespace DonTopo
