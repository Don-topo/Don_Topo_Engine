#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>
#include "DonTopo/Core/ImportSettings.h"
#include "DonTopo/Core/MaterialAsset.h"
#include "DonTopo/Editor/Command.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Editor/AssetImport.h"
#include "DonTopo/Editor/Thumbnail.h"

namespace IGFD { class FileDialog; }

namespace DonTopo {

class GameObject;

// Label of the grid's icon button. The ImGui id comes from it, so it
// cannot change when the thumbnail appears.
std::string assetIconButtonLabel(const char* text, bool hasThumbnail);

// Direct subfolders of dir, sorted by path, filtering out the noise that
// is not interesting to see in the Content Browser tree: hidden entries (name
// that starts with '.') and the build directory. Returns empty (without
// throwing) if dir does not exist, is not a directory or cannot be read.
// Declared here (and not in the .cpp's anonymous namespace) so the
// headless test can link it.
std::vector<std::filesystem::path> listVisibleSubdirs(const std::filesystem::path& dir);

// Imports each DroppedFile whose (screenX, screenY) falls inside the rect
// (rectX, rectY, rectW, rectH) into targetDir; those that fall outside generate
// no entry in the result (they are silently ignored). An individual failure
// (non-importable extension, name conflict) does not abort the
// rest of the batch. Declared here, not in the .cpp's anonymous namespace,
// so the headless test can link it.
std::vector<AssetImportOutcome> importDroppedFilesInto(
    const std::vector<DroppedFile>& dropped,
    float rectX, float rectY, float rectW, float rectH,
    const std::filesystem::path& targetDir);

// Type of an asset for the Content Browser: shared by the grid icon and the
// type filter, so that they cannot disagree.
enum class AssetKind { Folder, Model3D, Audio, Image, Font, Scene, Script, Shader, Material, Other };

// ext with the dot and in any combination of case ("" if it has none). A
// folder is always Folder, even if it is called "a.png".
AssetKind classifyAsset(const std::string& ext, bool isDir);

// The grid types that have a thumbnail: images, models and materials.
bool wantsThumbnail(AssetKind kind);

// true if the asset passes the grid filter: text = case-insensitive substring
// (empty lets everything through) and kindFilter = exact type equality
// (nullopt = all). Both are combined with AND, so with a type chosen the
// folders are left out unless the type is Folder.
bool assetMatchesFilter(const std::string& name, AssetKind kind,
                        const std::string& text, std::optional<AssetKind> kindFilter);

// true if the grid must start a drag for this asset: folders, what already
// supports import from outside the project (isImportableExtension), a .mat
// (a project asset, never imported from outside, so isImportableExtension
// does not cover it) or any that is part of a multiple selection (moving
// does not depend on which drop zones know how to accept the type). Declared here so
// the headless test can link it without instantiating ImGui.
bool isAssetDraggable(const std::string& ext, bool isDir, bool inMultiSelection);

// Finding from the final reviewer: the texture Browse of the .mat edit
// modal accepted any absolute path from outside the project as is (unlike
// the drop, which can only drop a DT_ASSET_PATH already inside
// it). That absolute path survives in the saved .mat and in the exported
// game it stops existing on another machine. A `path` from outside the project is
// imported into assets/Imported/Textures (same destination as the rest of the editor);
// nullopt if the extension is not an image one or the copy fails. Without `project`
// (headless tests) it is accepted as is, like the rest of the editor with no project
// open.
std::optional<std::filesystem::path> acceptOrImportMatTexture(const ProjectContext* project,
                                                               const std::filesystem::path& path);

// Visible files and folders of ONE folder (non-recursive), sorted by path.
// Hidden and build folders are left out with the same predicate as the
// tree; files are not filtered. Empty (without throwing) if dir does not exist, is a
// file or cannot be read. It is what the grid draws and what the polling compares
// between passes to detect changes made outside the editor.
std::vector<std::filesystem::path> listVisibleEntries(const std::filesystem::path& dir);

// Nearest existing ancestor of dir without leaving root: dir itself if it exists,
// and root if dir is outside root or no existing level remains up to it.
// With this the panel keeps working if the current folder is deleted from outside.
std::filesystem::path nearestExistingDir(const std::filesystem::path& dir,
                                         const std::filesystem::path& root);

// One segment of the breadcrumb: what is drawn and where it jumps to when pressed.
struct BreadcrumbSegment {
    std::string           name;
    std::filesystem::path path;
};

// Segments from the project root to current, with cumulative paths. If
// current is the root itself or does not hang from it, it returns only the root segment.
std::vector<BreadcrumbSegment> breadcrumbSegments(const std::filesystem::path& root,
                                                  const std::filesystem::path& current);

// Grid selection: the marked paths (always in the order they are seen) and the
// anchor from which Shift+click computes the range.
struct AssetSelection {
    std::vector<std::filesystem::path>   items;
    std::optional<std::filesystem::path> anchor;

    bool contains(const std::filesystem::path& p) const;
    void clear() { items.clear(); anchor.reset(); }
};

// Applies a click on `clicked` to the selection. visible is the grid's current
// order (with the filters already applied). Click = only that one; Ctrl = toggles that one; Shift
// = range from the anchor to that one (without a visible anchor, like a normal click).
void applyAssetClick(AssetSelection& sel, const std::vector<std::filesystem::path>& visible,
                     const std::filesystem::path& clicked, bool ctrl, bool shift);

// Removes from the selection what is no longer in existing; if the anchor disappears it is
// forgotten. Called after changing folder or rescanning.
void pruneSelection(AssetSelection& sel, const std::vector<std::filesystem::path>& existing);

enum class MoveResult {
    Moved,
    RejectedSameFolder,    // destDir is already src's parent folder
    RejectedIntoSelf,      // a folder inside itself or a descendant
    RejectedNameConflict,  // something with that name already exists in destDir
    RejectedFailed,        // nonexistent source or system error
};

struct MoveOutcome {
    MoveResult            result = MoveResult::RejectedFailed;
    std::filesystem::path newPath;       // valid only if result == Moved
    // RejectedFailed: the cause. Moved: a WARNING if the asset was moved but not its
    // .import.json (empty if everything went well).
    std::string           errorMessage;
};

// Renames a file or folder taking the file's .import.json along. If the
// destination already has a sidecar, it is rejected without touching anything. Never throws.
struct RenameFileOutcome {
    bool        ok = false;
    std::string error;    // cause if !ok
    std::string warning;  // ok but the sidecar could not be moved
};
RenameFileOutcome renameAssetFile(const std::filesystem::path& from, const std::filesystem::path& to,
                                  bool isDir);

struct TextureImportApplyResult {
    bool        ok = false;
    std::string error;       // cause if !ok (the modal shows it and does not close)
    int         refreshed = 0;   // objects whose material was rebuilt
};

// Writes the import settings of `asset` (the default deletes the sidecar) and
// calls `rebuild` with each scene object whose material uses that texture,
// so that the change is visible without restarting. If the sidecar cannot be written
// nothing is rebuilt. A null `rebuild` = only written (without a renderer).
TextureImportApplyResult applyTextureImportSettings(GameObject* sceneRoot,
                                                    const std::filesystem::path& asset,
                                                    const TextureImportSettings& settings,
                                                    const std::function<void(GameObject&)>& rebuild);

struct MaterialAssetApplyResult {
    bool        ok = false;
    std::string error;
    int         refreshed = 0;
};
// Writes the .mat and rebuilds (rebuild) each scene object that
// references it from any slot. Without a write, it rebuilds nothing.
MaterialAssetApplyResult applyMaterialAssetSettings(GameObject* sceneRoot, const std::filesystem::path& mat,
                                                    const MaterialAsset& asset,
                                                    const std::function<void(GameObject&)>& rebuild);
// Free name for a new material inside dir, same pattern as
// uniqueFolderName: "New material.mat", "New material 2.mat"...
std::string uniqueMaterialName(const std::filesystem::path& dir);

// Which import settings an asset offers. The context menu and the modal are
// decided by this, not by loose checks: a new type (models) is
// added here.
enum class ImportSettingsKind { None, Texture, Audio, Model };
ImportSettingsKind importSettingsKindFor(const std::string& ext, bool isDir);

struct AudioImportApplyResult {
    bool        ok = false;
    std::string error;       // cause if !ok (the modal shows it and does not close)
};

// Writes the clip's import settings (the default deletes the sidecar) and, if
// that worked, calls `refresh` with the path so that the voices loaded from that
// file receive them without restarting. If the sidecar cannot be written `refresh` is not
// called. A null `refresh` = only written (without audio).
AudioImportApplyResult applyAudioImportSettings(const std::filesystem::path& asset,
                                                const AudioImportSettings& settings,
                                                const std::function<void(const std::string&)>& refresh);

struct ModelImportApplyResult {
    bool        ok = false;
    std::string error;       // cause if !ok (the modal shows it and does not close)
    int         refreshed = 0;   // objects that reimport says it reloaded
};
// Writes the model's sidecar (the default deletes it) and, ONLY if that worked, calls
// `reimport(asset)` once: in the panel it live-reloads the scene objects
// that use that FBX (reimportModelUsers) and returns how many. Without `reimport` it only
// writes. A write failure reloads nothing.
ModelImportApplyResult applyModelImportSettings(
    const std::filesystem::path& asset,
    const ModelImportSettings& settings,
    const std::function<int(const std::filesystem::path&, float scaleRatio)>& reimport);

// Deletes a file (with its .import.json) or a whole folder. The system
// error, if any; empty = deleted.
std::error_code removeAssetPath(const std::filesystem::path& path, bool isDir);

// Moves src (file or folder) into destDir with the same name. Never
// overwrites: a name already taken is a rejection, not a replacement. It does not touch the
// scene: the caller updates the references with updateSceneReferencesForRename.
// Never throws (overloads with std::error_code).
MoveOutcome moveAsset(const std::filesystem::path& src, const std::filesystem::path& destDir);

// Free name for a new folder inside dir: "New folder", and if something
// (folder or file) already has that name, "New folder 2", "3"...
std::string uniqueFolderName(const std::filesystem::path& dir);

// The next three functions do not touch ContentBrowserPanel's private state
// (only their parameters), so they are declared here as free functions (like
// listVisibleSubdirs) so that the headless test can link them without
// instantiating the whole panel (which would drag in ImGui/Vulkan).

// Counts how many GameObjects of sceneRoot reference path (mesh or audio;
// exact if !isDir, by prefix if isDir).
int countSceneReferences(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir);
// Walks sceneRoot updating Mesh::sourcePath, the 3 paths of
// Material and AudioClipComponent::getPath() that match oldPath (exact
// if !isDir, by prefix if isDir) to the new value after a rename on
// disk that has already been done.
void updateSceneReferencesForRename(EditorContext& ctx, GameObject* sceneRoot,
                                     const std::filesystem::path& oldPath,
                                     const std::filesystem::path& newPath, bool isDir);
// Detaches from the scene any reference to path before
// deleting it from disk: mesh in use -> Renderer::removeMeshComponent;
// audio in use -> setAudioClip(nullptr); Material texture in use ->
// the path field is ALWAYS cleared (it prevents a re-register from trying
// stbi_load on an already deleted file), but the GPU hot-swap to the
// "missing" texture only fires if ctx.renderer && staticRenderIndex
// >= 0: replaceStaticTextureWithMissing indexes the list of static
// objects, so on skinned it never happens: the GPU keeps showing
// the old texture until the scene is reloaded.
void detachSceneReferencesForDelete(EditorContext& ctx, GameObject* sceneRoot,
                                     const std::filesystem::path& path, bool isDir);

// "Content Browser" window: project asset explorer (mesh,
// audio, scripts), with rename/delete and detection of references in the
// scene to detach them before deleting/renaming on disk.
class ContentBrowserPanel {
public:
    // Out of line on purpose: the destructor needs the complete type of
    // IGFD::FileDialog (unique_ptr<T> incomplete), and this header only
    // forward-declares it. Same pattern as PropertiesPanel.
    ContentBrowserPanel();
    ~ContentBrowserPanel();

    void draw(EditorContext& ctx, GameObject* sceneRoot);
    bool* GetOpenPtr() { return &m_open; }

private:
    // Sets up the "Rename Asset" modal popup preloaded with the current name of
    // path (stem if it is a file, full name if it is a folder).
    void beginAssetRename(const std::filesystem::path& path, bool isDir);
    // Sets up the "Delete Asset" modal popup, precomputing how many GameObjects
    // reference path (mesh or audio) to show it in the warning text.
    // targets = (path, isFolder) of everything selected; a single modal for all of them.
    void beginAssetDelete(GameObject* sceneRoot,
                          std::vector<std::pair<std::filesystem::path, bool>> targets);
    // Recursively draws dir and its visible subfolders as TreeNodes.
    // Click on the label selects the folder (m_currentDir); click on the
    // arrow only expands. It scans disk every frame for the open
    // nodes: no cache to invalidate and changes made outside the editor
    // show up by themselves.
    void drawFolderTree(const std::filesystem::path& dir);
    // Turns the last drawn item into a drop target for an asset (file or
    // folder of the grid). It does NOT move anything: it notes it in m_pendingMove, which draw() applies
    // outside the tree/grid walk (moving midway would invalidate what is being drawn).
    void acceptAssetDropOnFolder(const std::filesystem::path& destDir);
    // Applies and empties m_pendingMove: moves on disk, rewrites the scene's
    // references and relocates m_currentDir if the moved folder contained it.
    void applyPendingMove(EditorContext& ctx, GameObject* sceneRoot);

    struct PendingMove {
        std::vector<std::filesystem::path> srcs;   // one, or the whole dragged selection
        std::filesystem::path              destDir;
    };
    std::optional<PendingMove> m_pendingMove;

    // Grid selection (see AssetSelection). It is pruned against what is visible every
    // frame, so changing folder, filtering or rescanning leaves no ghosts.
    AssetSelection m_selection;

    // Texture thumbnails. Created lazily when there is a renderer with a
    // thumbnail atlas AND a JobSystem; without them it stays nullptr and the grid draws
    // the usual colored icon.
    std::unique_ptr<ThumbnailCache> m_thumbs;
    uint64_t                        m_thumbAtlasId = 0;
    std::string                     m_thumbDir;    // folder of the current generation

    bool m_open = true;
    bool m_scanned = false;
    // How often the current folder is re-read to detect changes made outside
    // the editor (same approach as ScriptManager::pollChanges: compare instead of
    // watching). Maximum latency of an external refresh ≈ this interval.
    static constexpr double kDirPollIntervalSeconds = 0.5;
    double m_lastPollTime = 0.0;
    std::string m_currentDir;
    // One-frame reveal: only a double click on a folder of the right
    // grid sets it to true (that folder may not be visible yet in the
    // tree). drawFolderTree queries it to force open the ancestor
    // branch of m_currentDir, and draw() clears it right after that
    // call, so the user regains control to collapse that
    // branch by hand again on the next frame.
    bool m_revealCurrentDir = false;
    // Project root (canonicalized once); it is the root of the folder tree,
    // and therefore the panel's natural navigation limit.
    std::filesystem::path m_projectRoot;
    std::vector<std::filesystem::path> m_assets;

    // Grid filters (current folder only). m_filterKindIndex indexes the
    // table of combo options in draw(); m_filterKind is its translation and
    // is recomputed every frame.
    char                    m_filterText[64] = {};
    int                     m_filterKindIndex = 0;
    std::optional<AssetKind> m_filterKind;

    // Asset rename: modal popup triggered by right-click > Rename in the
    // Content Browser's right grid.
    std::filesystem::path m_assetRenameTarget;
    bool                   m_assetRenameIsDir = false;
    char                   m_assetRenameBuffer[128] = {};
    std::string            m_assetRenameError;
    bool                   m_openAssetRenamePopup = false;

    // Import Settings: modal triggered by right-click > Import Settings... on
    // ONE texture. m_importEdit is the copy being edited; Apply writes it.
    std::filesystem::path  m_importTarget;
    TextureImportSettings  m_importEdit;
    ImportSettingsKind     m_importKind = ImportSettingsKind::None;
    AudioImportSettings    m_importAudioEdit;
    ModelImportSettings    m_importModelEdit;
    std::string            m_importError;
    bool                   m_openImportPopup = false;

    // Editing of a .mat, triggered by double click in the grid.
    std::filesystem::path m_matAssetTarget;
    MaterialAsset          m_matAssetEdit;
    std::string            m_matAssetError;
    // NON-modal floating window (ImGui::Begin, not a popup): see the comment
    // next to its use in the .cpp. A modal popup blocked the rest of the
    // editor and made it impossible to drag a texture from the grid to
    // "Drop image here".
    bool                   m_matAssetWindowOpen = false;
    bool                   m_matAssetDlgOpen     = false;
    DonTopo::MaterialTextureSlot m_matAssetDlgSlot = DonTopo::MaterialTextureSlot::Albedo;
    std::unique_ptr<IGFD::FileDialog> m_matAssetFileDialog;

    // Asset delete: modal popup triggered by right-click > Delete.
    std::vector<std::pair<std::filesystem::path, bool>> m_assetDeleteTargets;
    int                    m_assetDeleteAffectedCount = 0;
    bool                   m_openAssetDeletePopup = false;
    std::string            m_assetDeleteError;

    // Double click on a .json in the grid: load that scene (double and not single,
    // like folders and .lua files: a single click happens while passing over
    // selecting and would load scenes by accident). If the current one has
    // unsaved changes, a three-option modal asks first.
    enum class ScenePromptChoice { None, Save, Discard };
    // Scene that will be loaded (empty = none pending). Cancel in the modal
    // clears it and nothing is loaded.
    std::filesystem::path  m_sceneLoadTarget;
    bool                   m_openScenePromptPopup = false;
    // Decision made inside the popup. It is consumed at the start of the NEXT
    // frame, outside any Begin/BeginPopupModal: loading a scene destroys
    // the GameObject tree and touches the GPU, and doing it from inside the popup
    // would be re-entering in the middle of the panel's own drawing.
    ScenePromptChoice      m_scenePromptChoice = ScenePromptChoice::None;
};

} // namespace DonTopo
