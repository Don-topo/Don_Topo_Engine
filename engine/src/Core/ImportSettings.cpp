#include "DonTopo/Core/ImportSettings.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>
#include <system_error>

namespace DonTopo {

namespace {

// Un sidecar legitimo son ~100 bytes. El tope existe para que un fichero de
// megas, o un JSON anidado a 100000 niveles, no llegue nunca al parser.
constexpr std::uintmax_t kMaxSidecarBytes = 64 * 1024;

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
    if (warning) warning->clear();
    auto warn = [&](const std::string& m) { if (warning) *warning = m; };

    const std::filesystem::path sidecar = importSidecarPath(asset);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(sidecar, ec) || ec)
        return out;                                   // ausente: lo normal, sin aviso

    const std::uintmax_t size = std::filesystem::file_size(sidecar, ec);
    if (ec || size > kMaxSidecarBytes)
    {
        warn("sidecar ilegible o demasiado grande; se usan los valores por defecto");
        return out;
    }
    if (size == 0)
        return out;

    std::ifstream in(sidecar, std::ios::binary);
    if (!in)
    {
        warn("no se pudo abrir el sidecar; se usan los valores por defecto");
        return out;
    }
    const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };

    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object())
    {
        warn("JSON invalido; se usan los valores por defecto");
        return out;
    }

    const auto version = j.find("version");
    if (version == j.end() || !version->is_number_integer() || version->get<long long>() != 1)
    {
        warn("version de sidecar desconocida; se usan los valores por defecto");
        return out;
    }
    const auto type = j.find("type");
    if (type == j.end() || !type->is_string() || type->get<std::string>() != "texture")
    {
        warn("el sidecar no es de tipo texture; se usan los valores por defecto");
        return out;
    }

    std::string problems;
    if (const auto it = j.find("colorSpace"); it != j.end())
    {
        const std::string v = it->is_string() ? it->get<std::string>() : std::string();
        if      (v == "auto")   out.colorSpace = ColorSpaceOverride::Auto;
        else if (v == "srgb")   out.colorSpace = ColorSpaceOverride::Srgb;
        else if (v == "linear") out.colorSpace = ColorSpaceOverride::Linear;
        else problems += "colorSpace desconocido (se usa auto). ";
    }
    if (const auto it = j.find("mipmaps"); it != j.end())
    {
        if (it->is_boolean()) out.mipmaps = it->get<bool>();
        else                  problems += "mipmaps no es booleano (se usa false). ";
    }
    if (!problems.empty()) warn(problems);
    return out;
}

bool saveTextureImportSettings(const std::filesystem::path& asset,
                               const TextureImportSettings& settings, std::string* error)
{
    const std::filesystem::path sidecar = importSidecarPath(asset);
    std::error_code ec;

    if (isDefault(settings))
    {
        std::filesystem::remove(sidecar, ec);         // ausente tampoco es error
        if (ec)
        {
            if (error) *error = "no se pudo borrar " + sidecar.string() + ": " + ec.message();
            return false;
        }
        return true;
    }

    nlohmann::json j;
    j["version"]    = 1;
    j["type"]       = "texture";
    j["colorSpace"] = colorSpaceName(settings.colorSpace);
    j["mipmaps"]    = settings.mipmaps;

    // Fichero temporal + rename: un corte a mitad no deja un sidecar a medias.
    std::filesystem::path tmp = sidecar;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            if (error) *error = "no se pudo escribir " + sidecar.string();
            return false;
        }
        out << j.dump(2) << '\n';
        if (!out)
        {
            if (error) *error = "escritura incompleta de " + sidecar.string();
            out.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::rename(tmp, sidecar, ec);
    if (ec)
    {
        if (error) *error = "no se pudo renombrar a " + sidecar.string() + ": " + ec.message();
        std::error_code rmEc;
        std::filesystem::remove(tmp, rmEc);
        return false;
    }
    return true;
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
    if (!std::filesystem::exists(from, ec) || ec) return true;        // nada que mover
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
