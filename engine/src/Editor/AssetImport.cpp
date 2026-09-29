#include "DonTopo/Editor/AssetImport.h"
#include "DonTopo/Core/ImportSettings.h"
#include "DonTopo/Renderer/ModelLoader.h"

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
        ".wav", ".mp3", ".ogg", ".flac",
        ".png", ".jpg", ".jpeg", ".bmp", ".tga",
        ".ttf", ".otf", ".ttc"};
    return ModelLoader::isSupportedModelExtension(ext) || kImportable.count(toLower(ext)) != 0;
}

std::filesystem::path importedAssetDestDir(const std::filesystem::path& projectRoot,
                                            const std::string& ext)
{
    static const std::set<std::string> kAudio = {".wav", ".mp3", ".ogg", ".flac"};
    static const std::set<std::string> kImage = {".png", ".jpg", ".jpeg", ".bmp", ".tga"};
    static const std::set<std::string> kFont  = {".ttf", ".otf", ".ttc"};

    const std::string lower = toLower(ext);
    const std::filesystem::path imported = projectRoot / "assets" / "Imported";
    if (ModelLoader::isSupportedModelExtension(lower)) return imported / "Meshes";
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
        return { AssetImportResult::RejectedCopyFailed, {}, "The source is not a file", source };

    // "" / "x.png" es un path relativo: copiaria al CWD del proceso.
    if (destDir.empty())
        return { AssetImportResult::RejectedCopyFailed, {}, "The destination folder is empty", source };

    std::filesystem::create_directories(destDir, ec);
    if (ec)
        return { AssetImportResult::RejectedCopyFailed, {},
                 "Could not create the destination folder: " + ec.message(), source };

    const std::filesystem::path dest = destDir / source.filename();
    const bool ok = std::filesystem::copy_file(source, dest, ec);
    if (!ok)
    {
        if (ec == std::errc::file_exists)
            return { AssetImportResult::RejectedNameConflict, {}, "", source };
        return { AssetImportResult::RejectedCopyFailed, {}, ec.message(), source };
    }
    // Si el origen traia ajustes de importacion, viajan con la copia. Un fallo
    // aqui no deshace la importacion: el asset ya esta; se avisa en el mensaje.
    std::string warnings;
    std::string sidecarError;
    if (!copyImportSidecar(source, dest, &sidecarError))
        warnings = "could not copy the .import.json: " + sidecarError;

    // Un modelo lee otros ficheros (.mtl y sus texturas, .bin e imagenes de un
    // .gltf) en rutas relativas a su carpeta: se copian con la misma ruta, o la
    // copia del proyecto no carga o sale sin material. Uno que ya existe en el
    // destino no se pisa (puede ser de otro modelo) y se avisa.
    if (ModelLoader::isSupportedModelExtension(source.extension().string()))
    {
        for (const std::string& rel : ModelLoader::modelCompanionFiles(source.string()))
        {
            const std::filesystem::path from = source.parent_path() / std::filesystem::path(rel);
            const std::filesystem::path to   = dest.parent_path() / std::filesystem::path(rel);
            std::error_code cec;
            if (!std::filesystem::is_regular_file(from, cec)) continue;   // referenciado pero ausente: el loader lo dira
            std::filesystem::create_directories(to.parent_path(), cec);
            if (!std::filesystem::copy_file(from, to, cec))
            {
                if (!warnings.empty()) warnings += "; ";
                warnings += (cec == std::errc::file_exists ? "already existed: " : "could not copy ") + rel;
                continue;
            }
            copyImportSidecar(from, to, nullptr);
        }
    }
    return { AssetImportResult::Copied, dest, warnings, source };
}

std::string describeImportResult(const AssetImportOutcome& outcome)
{
    switch (outcome.result)
    {
        case AssetImportResult::Copied:
            return outcome.errorMessage.empty() ? std::string("imported")
                                                : "imported (" + outcome.errorMessage + ")";
        case AssetImportResult::RejectedExtension:    return "extension not supported for import";
        case AssetImportResult::RejectedNameConflict: return "a file with that name already exists at the destination";
        case AssetImportResult::RejectedCopyFailed:
            return outcome.errorMessage.empty() ? "copy failed" : outcome.errorMessage;
    }
    return "copy failed";
}

} // namespace DonTopo
