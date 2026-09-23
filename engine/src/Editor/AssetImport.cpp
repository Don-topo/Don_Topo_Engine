#include "DonTopo/Editor/AssetImport.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <system_error>

namespace DonTopo {

namespace {
std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
} // namespace

bool isImportableExtension(const std::string& ext)
{
    static const std::set<std::string> kImportable = {
        ".fbx",
        ".wav", ".mp3", ".ogg", ".flac",
        ".png", ".jpg", ".jpeg", ".bmp", ".tga",
        ".ttf", ".otf", ".ttc"};
    return kImportable.count(toLower(ext)) != 0;
}

std::filesystem::path importedAssetDestDir(const std::filesystem::path& projectRoot,
                                            const std::string& ext)
{
    static const std::set<std::string> kAudio = {".wav", ".mp3", ".ogg", ".flac"};
    static const std::set<std::string> kImage = {".png", ".jpg", ".jpeg", ".bmp", ".tga"};
    static const std::set<std::string> kFont  = {".ttf", ".otf", ".ttc"};

    const std::string lower = toLower(ext);
    const std::filesystem::path imported = projectRoot / "assets" / "Imported";
    if (lower == ".fbx")     return imported / "Meshes";
    if (kAudio.count(lower)) return imported / "Audio";
    if (kImage.count(lower)) return imported / "Textures";
    if (kFont.count(lower))  return imported / "Fonts";
    return {};
}

AssetImportOutcome importExternalAsset(const std::filesystem::path& source,
                                        const std::filesystem::path& destDir)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec) || ec)
        return { AssetImportResult::RejectedCopyFailed, {}, "El origen no es un fichero", source };

    std::filesystem::create_directories(destDir, ec);
    ec.clear();

    const std::filesystem::path dest = destDir / source.filename();
    const bool ok = std::filesystem::copy_file(source, dest, ec);
    if (!ok)
    {
        if (ec == std::errc::file_exists)
            return { AssetImportResult::RejectedNameConflict, {}, "", source };
        return { AssetImportResult::RejectedCopyFailed, {}, ec.message(), source };
    }
    return { AssetImportResult::Copied, dest, "", source };
}

std::string describeImportResult(const AssetImportOutcome& outcome)
{
    switch (outcome.result)
    {
        case AssetImportResult::Copied:              return "importado";
        case AssetImportResult::RejectedExtension:    return "extension no soportada para importar";
        case AssetImportResult::RejectedNameConflict: return "ya existe un fichero con ese nombre en el destino";
        case AssetImportResult::RejectedCopyFailed:
            return outcome.errorMessage.empty() ? "fallo de copia" : outcome.errorMessage;
    }
    return "fallo de copia";
}

} // namespace DonTopo
