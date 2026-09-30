#pragma once
#include <filesystem>
#include <string>

namespace DonTopo {

// Result of trying to copy an external file (outside the project or the
// engine's shared workspace) into a project folder. There is no
// overwriting: a name already taken at the destination is a rejection, not a
// replacement; the user deletes or renames by hand first.
enum class AssetImportResult {
    Copied,
    RejectedExtension,
    RejectedNameConflict,
    RejectedCopyFailed,
};

struct AssetImportOutcome {
    AssetImportResult result = AssetImportResult::RejectedCopyFailed;
    // Valid only if result == Copied.
    std::filesystem::path destPath;
    // Empty except for RejectedCopyFailed (std::error_code message).
    std::string errorMessage;
    // The source file that was attempted to be imported. Filled in by the caller
    // that knows which one it was (importExternalAsset and importDroppedFilesInto), so
    // that a rejection log in a batch of several files can say WHICH one
    // failed and not only why.
    std::filesystem::path sourcePath;
};

// true if ext (with the dot, any combination of upper/lower case) is one
// of the types the editor knows how to import today: the same set already used by the
// internal drag&drop of the Content Browser grid. Single source of truth:
// ContentBrowserPanel and PropertiesPanel share it instead of each
// maintaining its own list.
bool isImportableExtension(const std::string& ext);

// Destination folder by extension type, under projectRoot/assets/Imported/:
// Meshes, Audio, Textures or Fonts. Empty if the extension is not importable
// (isImportableExtension(ext) == false); the caller decides what to do with
// that, this function rejects nothing on its own.
std::filesystem::path importedAssetDestDir(const std::filesystem::path& projectRoot,
                                            const std::string& ext);

// Copies source to destDir/source.filename(). Does not overwrite: if the destination
// already exists, it returns RejectedNameConflict without touching disk. Creates destDir if it
// does not exist. Never throws: std::filesystem with the std::error_code overload
// in every disk call.
AssetImportOutcome importExternalAsset(const std::filesystem::path& source,
                                        const std::filesystem::path& destDir);

// Readable log message for a result that is NOT Copied (for Copied the
// caller already has destPath and composes its own success message).
std::string describeImportResult(const AssetImportOutcome& outcome);

} // namespace DonTopo
