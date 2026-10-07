#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>

#include "DonTopo/Renderer/RenderBackend.h"
#include "DonTopo/Core/Platform.h"

namespace DonTopo {

class Scene;
class PhysicsManager;
class AudioManager;

// An asset that the exported package must contain.
struct ExportAsset {
    // Absolute path on disk, resolved with weakly_canonical (not canonical:
    // it tolerates the last component not existing, needed for the cases with
    // existsOnDisk == false). Source of the copy.
    std::string sourcePath;
    // Path relative to the package root, with '/' as separator:
    // "assets/model.fbx". Destination of the copy and the value that ends up in the
    // exported .scene.
    std::string packagePath;
    // false if sourcePath does not exist on disk: it triggers the export abort
    // before copying anything.
    bool        existsOnDisk = false;
};

// Result of writing the package, to dump into the Log Console.
struct ExportResult {
    bool                     ok         = false;
    int                      fileCount  = 0;
    std::uintmax_t           totalBytes = 0;
    std::vector<std::string> messages;
    // exportGame: every asset it collected for the package -- the scene's AND the
    // ones reached only through packaged prefabs (the .prefab files included).
    // Filled once collection succeeds, even if writing fails later. The editor's
    // "asset outside the project" warning reads this list instead of re-scanning
    // the scene, which cannot see what only a prefab references.
    std::vector<ExportAsset> packagedAssets;
};

// Canonical key of a path to compare and deduplicate: weakly_canonical +
// lowercase + '/' as separator. Windows is case-insensitive but
// std::filesystem::path::operator== is not, and the paths arrive mixed
// (absolute from IGFD, relative from a hand-edited .scene). Same criterion
// as samePath() in ContentBrowserPanel.cpp:25, exposed here because
// rewriteScenePaths and its tests need to generate exactly the same key.
std::string exportPathKey(const std::string& path);

// All the assets the scene references: source meshes, animation
// sources, material textures, audio clips and the .lua files of the
// ScriptComponents. Deduplicated and sorted by packagePath.
//
// scriptPaths maps ScriptComponent::scriptName -> .lua file (built by
// the caller from ScriptManager::getRegistry()). A missing name is
// silently ignored: the .lua reaches the package anyway because Scripts/ is copied
// whole.
//
// It does NOT include the skybox, the shaders, Scripts/ or the executable: that is added by
// writeExportPackage. This function answers only "what does the scene reference".
std::vector<ExportAsset> collectSceneAssets(
    Scene& scene,
    const std::filesystem::path& projectRoot,
    const std::map<std::string, std::filesystem::path>& scriptPaths);

// The same over several scenes at once (the game scene plus one temporary
// Scene per packaged prefab), plus extraFiles: plain files that travel as they
// are (the .prefab files themselves). ONE pass with one dedup and one
// assets/_external/N numbering: collecting each scene separately and merging
// would hand the same _external/N to two different source folders.
std::vector<ExportAsset> collectSceneAssets(
    const std::vector<Scene*>& scenes,
    const std::filesystem::path& projectRoot,
    const std::map<std::string, std::filesystem::path>& scriptPaths,
    const std::vector<std::string>& extraFiles);

// Rewrites in place mesh.sourcePath, mesh.animationSources[].path and
// audioClip.path of the whole tree to their packagePath. sourceToPackage is
// keyed by exportPathKey(sourcePath). Returns how many paths were
// rewritten. Textures do not appear here because the .scene does not
// serialize them: ModelLoader derives them as dirname(fbx)/filename.
//
// assetRoot is the Scene's (Scene::assetRoot): the paths toJson stored relative
// to it (matAsset, texture overrides) are resolved against it before the lookup,
// not against the working directory. Empty = look them up as stored. No
// default on purpose: a caller that forgets it silently breaks the package.
//
// It also rewrites every serialized PrefabRef ({"prefab": "<path>"}, the values
// PrefabLibrary::collectReferences finds) stored relative to assetRoot. Used
// for game.scene AND for every packaged .prefab file (same rules).
int rewriteScenePaths(nlohmann::json& sceneJson,
                      const std::map<std::string, std::string>& sourceToPackage,
                      const std::string& assetRoot);

// Erases the prefab link ("prefab" object) and "prefabNode" of every node of a
// scene/prefab document or loose node. The exported runtime never propagates (the
// scene already stores each instance's full subtree), and the link's path and
// override values are editor data: dev-machine paths and references to prefabs
// that are not packaged. game.scene and every packaged .prefab go through it.
void stripPrefabLinks(nlohmann::json& sceneOrNode);

// Validates that 'name' is a safe path component to build
// destDir / name. Fills 'reason' with the cause when it returns false.
//
// It lives here and not in the UI because whoever needs it is the code that is going to
// delete: writeExportPackage() builds destDir/gameName and does remove_all()
// on that path. ".." goes up one level, an absolute name like "C:\Windows"
// makes operator/ IGNORE destDir entirely (that is how operator/ treats an
// absolute path), and Win32 drops trailing spaces/dots from the last
// component when creating the folder, collapsing the real destination onto the
// parent folder even if the string on screen looks harmless.
bool isValidExportGameName(const std::string& name, std::string& reason);

// State of an export's destination directory, to decide whether it is safe
// to delete it. The criterion is the inverse of the earlier one (manually enumerating the
// forbidden places: inside the project, inside Scripts...): that list
// always left one case out, the last one, <repo>/assets, which deleted the source
// assets and on top of that reported success. Here the question is not "where is the
// destination?" but "what is inside?", which is the only thing that determines whether a
// remove_all() destroys someone else's work.
enum class ExportTargetState {
    Missing,        // does not exist: it is created, nothing to delete
    Empty,          // exists and is empty: safe
    PriorPackage,   // exists and contains game.scene: package from a previous export, safe
    Occupied        // exists with foreign content: NEVER deleted
};

// Classifies the package directory 'pkg' (== destDir/gameName). If the
// state cannot be determined (permissions, invalid path, any filesystem
// error) it returns Occupied: it fails closed, because the cost of
// being wrong in the other direction is deleting user data.
ExportTargetState inspectExportTarget(const std::filesystem::path& pkg);

// Creates <destDir>/<gameName>/, copies the runtime, the assets, the skybox,
// shaders/*.spv, Scripts/ and fmod.dll, and writes game.scene.
//
// It calls inspectExportTarget() on its own and aborts without touching anything if the
// destination is Occupied: it is authoritative, it does not assume the UI has
// looked. With Missing/Empty/PriorPackage it does remove_all() + recreate, so
// that the package does not carry orphan assets from a previous export (asking for
// confirmation in the PriorPackage case is still the UI's job).
//
// 'backend' only decides what is written to game.cfg and whether the absence of
// .dxil is warned about: the shaders of both backends are always copied, so that
// the package still starts if someone edits that field by hand.
// What the exported package carries on each platform. writeExportPackage walks
// this instead of having per-system branches: macOS will be another row.
struct ExportPlatform {
    std::string              executableSuffix;   // ".exe" | ""
    bool                     setExecutableBit;   // chmod +x on the executable
    std::vector<std::string> audioLibPrefixes;   // exact file; if it ends in ".", + a number
    bool                     copyMsvcCrt;        // msvcp140* / vcruntime140* next to the editor
    bool                     warnDebugCrt;       // warning about the non-redistributable debug CRT
    bool                     warnGlibc;          // warning about the minimum glibc version
};

ExportPlatform exportPlatformFor(platform::Os os);

// Is `fileName` an audio library that the package must carry? An entry
// that ends in "." admits only that prefix plus a number: "libfmod.so." accepts
// libfmod.so.14 (the soname, what the binary asks for) and neither libfmod.so nor
// libfmod.so.14.14, which are the same file repeated. FMOD's logging
// variant (libfmodL, fmodL.dll) is not included.
bool isAudioLibFile(const std::string& fileName, const ExportPlatform& plat);

ExportResult writeExportPackage(const std::vector<ExportAsset>& assets,
                                const nlohmann::json& rewrittenScene,
                                const std::filesystem::path& destDir,
                                const std::string& gameName,
                                const std::filesystem::path& projectRoot,
                                const std::filesystem::path& scriptsDir,
                                const std::filesystem::path& runtimeExe,
                                RenderBackend backend = RenderBackend::Vulkan,
                                // Folder the 6 sky faces come from,
                                // relative to the project. The destination inside the package
                                // is ALWAYS assets/skybox, which is where the runtime
                                // looks for them, so changing the sky in the editor does not
                                // force touching the runtime.
                                const std::string& skyboxFolder = "assets/skybox",
                                // Which platform the package is for. By default, that of
                                // the exporting editor; the tests write that of
                                // another to cover both rows from any OS.
                                const ExportPlatform& plat = exportPlatformFor(platform::currentOs()),
                                // packagePath -> content of each .prefab in `assets`,
                                // already rewritten (stripPrefabLinks + rewriteScenePaths).
                                // A .prefab asset is written from here, never copied
                                // verbatim: one missing from this map fails the export
                                // instead of leaking the editor's paths into the package.
                                const std::map<std::string, nlohmann::json>& rewrittenPrefabs = {});

// Full export: validates, collects, rewrites and writes the package.
// User messages go in ExportResult::messages; the caller
// decides where to show them (the editor dumps them into the Log Console).
//
// It is here and not in EditorUI because it draws nothing: it is export orchestration
// and path arithmetic. Living in the module is what lets
// exporter_tests exercise the destructive paths without opening a window.
//
// Prefabs: every .prefab the scene's PrefabRefs name is packaged, and the ones
// THEY name, transitively (cycles end). Each one is loaded into a temporary
// Scene (hence physics/audio, which must be alive and are left as found) so
// collectSceneAssets sees its models, materials, clips and scripts exactly as
// it sees the scene's. A referenced prefab that is missing or not a prefab
// cancels the export, like any missing asset. Instance links are stripped
// (stripPrefabLinks): a prefab referenced ONLY by a link is not packaged.
ExportResult exportGame(Scene& scene,
                        PhysicsManager& physics,
                        AudioManager& audio,
                        const std::map<std::string, std::filesystem::path>& scriptPaths,
                        const std::filesystem::path& destDir,
                        const std::string& gameName,
                        const std::filesystem::path& projectRoot,
                        const std::filesystem::path& scriptsDir,
                        const std::filesystem::path& runtimeExe,
                        RenderBackend backend = RenderBackend::Vulkan,
                        // Ver writeExportPackage.
                        const std::string& skyboxFolder = "assets/skybox");

} // namespace DonTopo
