#include "DonTopo/Editor/ContentBrowserPanel.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Editor/AssetImport.h"
#include "DonTopo/Editor/ModelReimport.h"
#include "DonTopo/Editor/ThumbnailDiskCache.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Core/JobSystem.h"
#include "DonTopo/Core/ImportSettings.h"
#include "DonTopo/Editor/ProjectContext.h"
#include "DonTopo/Editor/UndoManager.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include <imgui.h>
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <set>
#include "DonTopo/Renderer/EditorRenderer.h"

namespace {

std::string trim(const std::string& name)
{
    size_t begin = name.find_first_not_of(" \t");
    size_t end   = name.find_last_not_of(" \t");
    return name.substr(begin, end - begin + 1);
}

// Compares two paths robustly against case (Windows is
// case-insensitive but std::filesystem::path::operator== is not) and against
// relative/absolute format (weakly_canonical before comparing).
bool samePath(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::error_code ecA, ecB;
    std::filesystem::path ca = std::filesystem::weakly_canonical(a, ecA);
    std::filesystem::path cb = std::filesystem::weakly_canonical(b, ecB);
    std::string sa = (ecA ? a : ca).string();
    std::string sb = (ecB ? b : cb).string();
    std::transform(sa.begin(), sa.end(), sa.begin(), ::tolower);
    std::transform(sb.begin(), sb.end(), sb.begin(), ::tolower);
    return sa == sb;
}

// true if p is strictly inside dir (p == dir counts as false).
bool pathUnderDir(const std::filesystem::path& p, const std::filesystem::path& dir)
{
    std::error_code ecP, ecD;
    std::filesystem::path cp = std::filesystem::weakly_canonical(p, ecP);
    std::filesystem::path cd = std::filesystem::weakly_canonical(dir, ecD);
    std::string sp = (ecP ? p : cp).string();
    std::string sd = (ecD ? dir : cd).string();
    std::transform(sp.begin(), sp.end(), sp.begin(), ::tolower);
    std::transform(sd.begin(), sd.end(), sd.begin(), ::tolower);
    if (sp.size() <= sd.size() || sp.compare(0, sd.size(), sd) != 0)
        return false;
    char sep = sp[sd.size()];
    return sep == '\\' || sep == '/';
}

// Replaces the oldDir prefix with newDir in original. Assumes
// pathUnderDir(original, oldDir) already returned true.
std::string replacePathPrefix(const std::string& original,
                               const std::filesystem::path& oldDir,
                               const std::filesystem::path& newDir)
{
    std::error_code ecO, ecD;
    std::filesystem::path canonOriginal = std::filesystem::weakly_canonical(std::filesystem::path(original), ecO);
    std::filesystem::path canonOldDir   = std::filesystem::weakly_canonical(oldDir, ecD);
    std::string canonOriginalStr = ecO ? original      : canonOriginal.string();
    std::string canonOldDirStr   = ecD ? oldDir.string() : canonOldDir.string();
    if (canonOriginalStr.size() <= canonOldDirStr.size())
        return newDir.string(); // defensive: pathUnderDir should already guarantee original is strictly under oldDir
    return newDir.string() + canonOriginalStr.substr(canonOldDirStr.size());
}

// All the materials of a GameObject in a single list, whether the mesh is
// static or skinned. ModelLoader::loadSkinned NEVER populates the inherited
// Mesh::material: it hands out one Material per submesh in SkinnedMesh::materials, so
// looking only at material left any rigged character out of the
// texture tracking: deleting one of its textures said "0 objects affected"
// in a destructive dialog. It returns pointers to the real material so that
// callers can clear fields, not copies.
std::vector<const DonTopo::Material*> materialsOf(const DonTopo::GameObject* go)
{
    std::vector<const DonTopo::Material*> out;
    if (!go->hasMesh()) return out;
    if (const DonTopo::SkinnedMesh* sm = go->getSkinnedMesh())
        for (const DonTopo::Material& m : sm->materials)
            out.push_back(&m);
    else
        out.push_back(&go->getMesh()->material);
    return out;
}

// Same criterion, writable. It goes through editMesh: it copies the mesh if it is
// shared, so it is only called when something in the material WILL change
// (see tocaAlgunMaterial), never to walk just in case.
std::vector<DonTopo::Material*> editMaterialsOf(DonTopo::GameObject* go)
{
    std::vector<DonTopo::Material*> out;
    if (!go->hasMesh()) return out;
    if (DonTopo::SkinnedMesh* sm = go->editSkinnedMesh())
        for (DonTopo::Material& m : sm->materials)
            out.push_back(&m);
    else
        out.push_back(&go->editMesh()->material);
    return out;
}

// true if any path of any material of the object satisfies `coincide`.
template <typename Pred>
bool tocaAlgunMaterial(const DonTopo::GameObject* go, Pred coincide)
{
    for (const DonTopo::Material* m : materialsOf(go))
        if (coincide(m->texturePath) || coincide(m->normalMapPath) || coincide(m->metallicRoughnessPath))
            return true;
    return false;
}

// Valid file/folder name: not empty after trim, without path separators or
// Windows reserved characters.
bool isValidFileName(const std::string& name)
{
    if (name.empty())
        return false;
    static const std::string kReserved = "\\/:*?\"<>|";
    for (char c : name)
        if (kReserved.find(c) != std::string::npos)
            return false;
    return true;
}

// Single predicate for "folder hidden from the Content Browser". Used both by
// listVisibleSubdirs (left tree) and by the right grid's scan
// (##AssetPane) so that both panels see the same set of folders; otherwise
// a double click in the grid could select a folder that the tree
// never shows.
//
// The criterion that rules is the CONTENT, not the name. Enumerating build
// folder names does not scale: here it is build-ninja, in CLion cmake-build-debug,
// in Visual Studio x64-Debug, and the next one that appears sneaks into the
// panel again. What really distinguishes a build tree is what it has INSIDE:
// a CMakeCache.txt.
//
// That is why the generic names ("build", "out") are NOT here: the only case
// they would cover is that of a freshly created, still unconfigured build folder
// (empty, that is harmless in the panel) and in exchange they would hide a
// user assets folder that happens to be called that. The two that remain are
// unambiguous: build-ninja is this repo's and cmake-build-* is CLion's.
//
// The cost is one stat per subfolder per frame (listVisibleSubdirs runs in
// the render loop), over the subfolders of ONE directory, not recursive.
bool isHiddenDir(const std::filesystem::path& dir)
{
    const std::string name = dir.filename().string();
    if (name.empty() || name[0] == '.') return true;

    if (name == "build-ninja") return true;
    if (name.rfind("cmake-build-", 0) == 0) return true; // cmake-build-debug, -release...

    std::error_code ec;
    return std::filesystem::exists(dir / "CMakeCache.txt", ec) && !ec;
}

// Simple rect containment: top/left edge inclusive, bottom/
// right exclusive, the standard for on-screen rect hit-testing.
bool pointInsideRect(float px, float py, float rectX, float rectY, float rectW, float rectH)
{
    return px >= rectX && px < rectX + rectW && py >= rectY && py < rectY + rectH;
}

} // namespace

namespace DonTopo {

ContentBrowserPanel::ContentBrowserPanel()  = default;
// Out of line on purpose: the unique_ptr<IGFD::FileDialog> only needs the
// complete type HERE, where ImGuiFileDialog.h is already included; same pattern
// as PropertiesPanel::~PropertiesPanel().
ContentBrowserPanel::~ContentBrowserPanel() = default;

std::string assetIconButtonLabel(const char* text, bool hasThumbnail)
{
    // "###" fixes the id: ImGui hashes the whole label, and without this the button
    // would change id (and lose the click in progress) when the thumbnail arrives.
    return std::string(hasThumbnail ? "" : text) + "###icon";
}

bool wantsThumbnail(AssetKind kind)
{
    return kind == AssetKind::Image || kind == AssetKind::Model3D || kind == AssetKind::Material;
}

std::vector<std::filesystem::path> listVisibleSubdirs(const std::filesystem::path& dir)
{
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) return out; // does not exist, is a file or there are no permissions

    // Manual advance with the overload that does not throw: the range-based for's
    // operator++ throws filesystem_error if the enumeration fails
    // midway (folder deleted, permissions, etc.), and this function is
    // called every frame from the editor's render loop.
    static const std::filesystem::directory_iterator kEnd;
    for (; it != kEnd; it.increment(ec))
    {
        if (ec) break;
        const auto& entry = *it;
        std::error_code isDirEc;
        if (!entry.is_directory(isDirEc) || isDirEc) continue;
        if (isHiddenDir(entry.path())) continue;
        out.push_back(entry.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

namespace {
std::string lowerAscii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
} // namespace

AssetKind classifyAsset(const std::string& ext, bool isDir)
{
    if (isDir) return AssetKind::Folder;
    const std::string e = lowerAscii(ext);
    if (ModelLoader::isSupportedModelExtension(e))                           return AssetKind::Model3D;
    if (e == ".mp3" || e == ".wav" || e == ".ogg" || e == ".flac")            return AssetKind::Audio;
    if (e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp") return AssetKind::Image;
    if (e == ".ttf" || e == ".otf" || e == ".ttc")                            return AssetKind::Font;
    if (e == ".json")                                                         return AssetKind::Scene;
    if (e == ".lua")                                                          return AssetKind::Script;
    if (e == ".spv")                                                          return AssetKind::Shader;
    if (e == ".mat")                                                          return AssetKind::Material;
    return AssetKind::Other;
}

bool isAssetDraggable(const std::string& ext, bool isDir, bool inMultiSelection)
{
    if (isDir || inMultiSelection || isImportableExtension(ext)) return true;
    return classifyAsset(ext, false) == AssetKind::Material;
}

std::optional<std::filesystem::path> acceptOrImportMatTexture(const ProjectContext* project,
                                                               const std::filesystem::path& path)
{
    if (!project || !project->valid() || project->contains(path))
        return path;
    const std::string ext = path.extension().string();
    if (!isImportableExtension(ext)) return std::nullopt;
    const std::filesystem::path destDir = importedAssetDestDir(project->root(), ext);
    const AssetImportOutcome outcome = importExternalAsset(path, destDir);
    if (outcome.result != AssetImportResult::Copied) return std::nullopt;
    return outcome.destPath;
}

bool assetMatchesFilter(const std::string& name, AssetKind kind,
                        const std::string& text, std::optional<AssetKind> kindFilter)
{
    if (kindFilter && *kindFilter != kind)
        return false;
    if (text.empty())
        return true;
    return lowerAscii(name).find(lowerAscii(text)) != std::string::npos;
}

std::vector<std::filesystem::path> listVisibleEntries(const std::filesystem::path& dir)
{
    std::vector<std::filesystem::path> out;
    std::error_code existsEc;
    if (!std::filesystem::exists(dir, existsEc) || existsEc)
        return out;

    std::error_code iterEc;
    std::filesystem::directory_iterator it(dir, iterEc);
    // Manual advance with the overload that does not throw. exists() and the iteration are
    // two separate disk calls (TOCTOU): the folder can disappear in between
    // (checkout, build, external tool), and this runs from the render
    // loop, so a failure must leave the list empty for that frame instead of bringing down the
    // editor.
    static const std::filesystem::directory_iterator kEnd;
    for (; !iterEc && it != kEnd; it.increment(iterEc))
    {
        const auto& entry = *it;
        std::error_code fileEc, dirEc;
        const bool isFile     = entry.is_regular_file(fileEc);
        const bool isDirEntry = entry.is_directory(dirEc);
        // Hidden/noise folders filtered the same as the left tree (same
        // predicate); files are not filtered, they are all listed.
        if (isDirEntry && isHiddenDir(entry.path()))
            continue;
        // The .import.json files belong to their assets, they are not assets: they are not listed
        // (and without this the grid would classify them as scenes because of their .json
        // extension).
        if (isFile && isImportSidecar(entry.path()))
            continue;
        if (isFile || isDirEntry)
            out.push_back(entry.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::filesystem::path nearestExistingDir(const std::filesystem::path& dir,
                                         const std::filesystem::path& root)
{
    if (!pathUnderDir(dir, root))
        return root; // dir == root or outside the root
    std::filesystem::path cur = dir;
    while (pathUnderDir(cur, root))
    {
        std::error_code ec;
        if (std::filesystem::is_directory(cur, ec) && !ec)
            return cur;
        cur = cur.parent_path();
    }
    return root;
}

bool AssetSelection::contains(const std::filesystem::path& p) const
{
    return std::find(items.begin(), items.end(), p) != items.end();
}

namespace {
// Index of p in visible, or -1.
int indexIn(const std::vector<std::filesystem::path>& visible, const std::filesystem::path& p)
{
    const auto it = std::find(visible.begin(), visible.end(), p);
    return it == visible.end() ? -1 : static_cast<int>(it - visible.begin());
}

// Reorders items according to their position in visible (those no longer there, at the end).
void sortByVisibleOrder(std::vector<std::filesystem::path>& items,
                        const std::vector<std::filesystem::path>& visible)
{
    std::stable_sort(items.begin(), items.end(),
        [&](const std::filesystem::path& a, const std::filesystem::path& b) {
            int ia = indexIn(visible, a), ib = indexIn(visible, b);
            if (ia < 0) ia = static_cast<int>(visible.size());
            if (ib < 0) ib = static_cast<int>(visible.size());
            return ia < ib;
        });
}
} // namespace

void applyAssetClick(AssetSelection& sel, const std::vector<std::filesystem::path>& visible,
                     const std::filesystem::path& clicked, bool ctrl, bool shift)
{
    if (shift)
    {
        const int a = sel.anchor ? indexIn(visible, *sel.anchor) : -1;
        const int b = indexIn(visible, clicked);
        if (a >= 0 && b >= 0)
        {
            sel.items.assign(visible.begin() + std::min(a, b), visible.begin() + std::max(a, b) + 1);
            return; // Shift does not move the anchor
        }
        // Without a visible anchor: falls through to the normal click below.
    }
    else if (ctrl)
    {
        const auto it = std::find(sel.items.begin(), sel.items.end(), clicked);
        if (it != sel.items.end()) sel.items.erase(it);
        else                       sel.items.push_back(clicked);
        sortByVisibleOrder(sel.items, visible);
        sel.anchor = clicked;
        return;
    }
    sel.items  = { clicked };
    sel.anchor = clicked;
}

void pruneSelection(AssetSelection& sel, const std::vector<std::filesystem::path>& existing)
{
    sel.items.erase(std::remove_if(sel.items.begin(), sel.items.end(),
        [&](const std::filesystem::path& p) { return indexIn(existing, p) < 0; }),
        sel.items.end());
    if (sel.anchor && indexIn(existing, *sel.anchor) < 0)
        sel.anchor.reset();
}

MoveOutcome moveAsset(const std::filesystem::path& src, const std::filesystem::path& destDir)
{
    std::error_code ec;
    if (!std::filesystem::exists(src, ec) || ec)
        return { MoveResult::RejectedFailed, {}, "The source does not exist" };

    if (samePath(src.parent_path(), destDir))
        return { MoveResult::RejectedSameFolder, {}, "" };

    const bool isDir = std::filesystem::is_directory(src, ec);
    if (isDir && (samePath(src, destDir) || pathUnderDir(destDir, src)))
        return { MoveResult::RejectedIntoSelf, {}, "" };

    const std::filesystem::path dest = destDir / src.filename();
    if (std::filesystem::exists(dest, ec))
        return { MoveResult::RejectedNameConflict, {}, "" };
    // A sidecar at the destination (even an orphan one) is also a conflict: moving
    // over it would overwrite its settings.
    if (!isDir && importSidecarConflict(src, dest))
        return { MoveResult::RejectedNameConflict, {}, "" };

    // A .mat's texture paths are stored RELATIVE TO ITS OWN
    // FOLDER (MaterialAsset.cpp): moving the file (or a folder that
    // contains some) changes that folder, and without rewriting them they would be
    // resolved against the new folder instead of the old one. The
    // content (already resolved to absolute in memory) is read BEFORE moving, while
    // the relative path still resolves against the correct folder.
    std::vector<std::pair<std::filesystem::path, MaterialAsset>> matContents;
    if (!isDir)
    {
        if (lowerAscii(src.extension().string()) == ".mat")
            matContents.push_back({ dest, loadMaterialAsset(src) });
    }
    else
    {
        std::error_code walkEc;
        for (auto it = std::filesystem::recursive_directory_iterator(src, walkEc);
             !walkEc && it != std::filesystem::recursive_directory_iterator(); it.increment(walkEc))
        {
            if (walkEc || !it->is_regular_file(walkEc)) continue;
            if (lowerAscii(it->path().extension().string()) != ".mat") continue;
            std::error_code relEc;
            const std::filesystem::path rel = std::filesystem::relative(it->path(), src, relEc);
            if (relEc) continue;
            matContents.push_back({ dest / rel, loadMaterialAsset(it->path()) });
        }
    }

    ec.clear();
    std::filesystem::rename(src, dest, ec);
    if (ec)
        return { MoveResult::RejectedFailed, {}, ec.message() };

    // saveMaterialAsset rewrites each path relative to the .mat's NEW folder,
    // with the same absolute value as before: the texture is still
    // found even though the .mat changed place.
    for (const auto& [matDest, asset] : matContents)
        saveMaterialAsset(matDest, asset);

    if (!isDir)
    {
        std::string sidecarError;
        if (!moveImportSidecar(src, dest, &sidecarError))
            return { MoveResult::Moved, dest,
                     "the asset was moved but not its .import.json: " + sidecarError };
    }
    return { MoveResult::Moved, dest, "" };
}

RenameFileOutcome renameAssetFile(const std::filesystem::path& from, const std::filesystem::path& to,
                                  bool isDir)
{
    RenameFileOutcome out;
    // If source and destination are the SAME sidecar (renaming only by changing
    // case on a system that does not distinguish it) there is no conflict: it is the
    // same file.
    if (!isDir && !samePath(importSidecarPath(from), importSidecarPath(to)) &&
        importSidecarConflict(from, to))
    {
        out.error = "An .import.json with that name already exists";
        return out;
    }
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (ec)
    {
        out.error = ec.message();
        return out;
    }
    out.ok = true;
    if (!isDir)
    {
        std::string sidecarError;
        if (!moveImportSidecar(from, to, &sidecarError))
            out.warning = "The asset was renamed but not its .import.json: " + sidecarError;
    }
    return out;
}

std::error_code removeAssetPath(const std::filesystem::path& path, bool isDir)
{
    std::error_code ec;
    if (isDir) std::filesystem::remove_all(path, ec);
    else       std::filesystem::remove(path, ec);
    if (!ec && !isDir)
        removeImportSidecar(path);
    return ec;
}

std::vector<BreadcrumbSegment> breadcrumbSegments(const std::filesystem::path& root,
                                                  const std::filesystem::path& current)
{
    std::vector<BreadcrumbSegment> out;
    out.push_back({ root.filename().string(), root });
    if (!pathUnderDir(current, root))
        return out;

    // Both canonicalized the same as pathUnderDir, so that the rest is
    // a clean relative even if they come with different casing or separators.
    std::error_code ecR, ecC;
    const std::filesystem::path canonRoot = std::filesystem::weakly_canonical(root, ecR);
    const std::filesystem::path canonCur  = std::filesystem::weakly_canonical(current, ecC);
    if (ecR || ecC)
        return out;

    std::filesystem::path accumulated = root;
    for (const auto& part : canonCur.lexically_relative(canonRoot))
    {
        if (part.empty() || part == ".") continue;
        accumulated /= part;
        out.push_back({ part.string(), accumulated });
    }
    return out;
}

std::string uniqueFolderName(const std::filesystem::path& dir)
{
    const std::string base = "New Folder";
    std::error_code ec;
    if (!std::filesystem::exists(dir / base, ec))
        return base;
    for (int n = 2; ; ++n)
    {
        const std::string candidate = base + " " + std::to_string(n);
        if (!std::filesystem::exists(dir / candidate, ec))
            return candidate;
    }
}

std::string uniqueMaterialName(const std::filesystem::path& dir)
{
    const std::string base = "New Material";
    std::error_code ec;
    if (!std::filesystem::exists(dir / (base + ".mat"), ec))
        return base + ".mat";
    for (int n = 2; ; ++n)
    {
        const std::string candidate = base + " " + std::to_string(n) + ".mat";
        if (!std::filesystem::exists(dir / candidate, ec))
            return candidate;
    }
}

std::vector<AssetImportOutcome> importDroppedFilesInto(
    const std::vector<DroppedFile>& dropped,
    float rectX, float rectY, float rectW, float rectH,
    const std::filesystem::path& targetDir)
{
    std::vector<AssetImportOutcome> out;
    for (const DroppedFile& f : dropped)
    {
        if (!pointInsideRect(f.screenX, f.screenY, rectX, rectY, rectW, rectH))
            continue;
        if (!isImportableExtension(f.path.extension().string()))
        {
            out.push_back({ AssetImportResult::RejectedExtension, {}, "", f.path });
            continue;
        }
        out.push_back(importExternalAsset(f.path, targetDir));
    }
    return out;
}

void ContentBrowserPanel::beginAssetRename(const std::filesystem::path& path, bool isDir)
{
    m_assetRenameTarget = path;
    m_assetRenameIsDir  = isDir;
    m_assetRenameError.clear();
    std::string prefill = isDir ? path.filename().string() : path.stem().string();
    std::strncpy(m_assetRenameBuffer, prefill.c_str(), sizeof(m_assetRenameBuffer) - 1);
    m_assetRenameBuffer[sizeof(m_assetRenameBuffer) - 1] = '\0';
    m_openAssetRenamePopup = true;
}

void updateSceneReferencesForRename(EditorContext& ctx, GameObject* sceneRoot,
                                     const std::filesystem::path& oldPath,
                                     const std::filesystem::path& newPath,
                                     bool isDir)
{
    (void)ctx;
    if (!sceneRoot) return;

    sceneRoot->traverse([&](GameObject* go)
    {
        auto coincide = [&](const std::string& field)
        {
            return !field.empty() && (isDir ? pathUnderDir(field, oldPath) : samePath(field, oldPath));
        };
        auto updateField = [&](std::string& field)
        {
            if (coincide(field))
                field = isDir ? replacePathPrefix(field, oldPath, newPath) : newPath.string();
        };

        if (go->hasMesh())
        {
            // Write to the mesh's copy if it is shared: only when
            // something matches, not for every object that is walked.
            if (coincide(go->getMesh()->sourcePath))
                updateField(go->editMesh()->sourcePath);
            // Same blind spot as in count/detach: on skinned the materials
            // live in SkinnedMesh::materials, never in the inherited
            // Mesh::material. Without this, renaming a texture left all the
            // rigged characters pointing in memory to the old name for
            // the rest of the session, silently, until the next attempt
            // to touch that path. For a slot WITHOUT a user override this
            // is also enough between sessions: the material is re-derived from the FBX
            // on every load. For a slot WITH an override
            // (GameObject::materialOverrides, which since Task 6 is indeed
            // serialized in mesh.materials) this fix only corrects the Material
            // in memory; the saved override keeps pointing to the old name
            // until the user touches that slot again from
            // Properties.
            for (Material* mat : tocaAlgunMaterial(go, coincide) ? editMaterialsOf(go) : std::vector<Material*>{})
            {
                updateField(mat->texturePath);
                updateField(mat->normalMapPath);
                updateField(mat->metallicRoughnessPath);
            }
            // GameObject::materialOverrides is what really survives a
            // save (mesh.materials, Task 6): the loop above only
            // corrects the Material IN MEMORY for the rest of the session. Without
            // this, the saved override keeps pointing to the old name, the
            // next Save writes that dead path as is, and on reopening the
            // texture does not load, precisely the data loss this task
            // is meant to close. Same updateField (same comparison
            // criterion: pathUnderDir/samePath) as the rest of the function.
            //
            // baseAlbedo/baseNormal/baseOrm go in the same loop. They are not
            // serialized outside the IN-MEMORY paths of clone/undo (see
            // the big comment next to "baseAlbedo" in
            // Scene.cpp::nodeToJson), so they are not the data loss BETWEEN
            // sessions that the loop above closes; what they close is the
            // gap within the SAME session: if the renamed file is the one that
            // gave the baseline and not the active override, a later Clear()
            // returned the old name, and the file with that name no longer
            // exists. Renaming is exactly the case where the baseline CAN
            // be corrected instead of thrown away: the asset is still there, it only
            // changed name.
            for (MaterialOverride& ov : go->materialOverrides)
            {
                updateField(ov.albedo);
                updateField(ov.normal);
                updateField(ov.orm);
                updateField(ov.baseAlbedo);
                updateField(ov.baseNormal);
                updateField(ov.baseOrm);
                updateField(ov.matAsset);
            }
        }
        if (go->hasAudioClip())
        {
            std::string audioPath = go->getAudioClip()->getPath();
            bool matches = isDir ? pathUnderDir(audioPath, oldPath) : samePath(audioPath, oldPath);
            if (matches)
            {
                std::string newAudioPath = isDir ? replacePathPrefix(audioPath, oldPath, newPath) : newPath.string();
                go->getAudioClip()->setPath(newAudioPath);
            }
        }
    });
}

TextureImportApplyResult applyTextureImportSettings(GameObject* sceneRoot,
                                                    const std::filesystem::path& asset,
                                                    const TextureImportSettings& settings,
                                                    const std::function<void(GameObject&)>& rebuild)
{
    TextureImportApplyResult r;
    if (!saveTextureImportSettings(asset, settings, &r.error))
        return r;                                    // nothing rebuilt if it could not be written
    r.ok = true;
    if (!sceneRoot || !rebuild) return r;

    sceneRoot->traverse([&](GameObject* go)
    {
        auto coincide = [&](const std::string& field)
        {
            return !field.empty() && samePath(field, asset);
        };
        if (!go->hasMesh() || !tocaAlgunMaterial(go, coincide)) return;
        rebuild(*go);
        ++r.refreshed;
    });
    return r;
}

MaterialAssetApplyResult applyMaterialAssetSettings(GameObject* sceneRoot, const std::filesystem::path& mat,
                                                    const MaterialAsset& asset,
                                                    const std::function<void(GameObject&)>& rebuild)
{
    MaterialAssetApplyResult r;
    if (!saveMaterialAsset(mat, asset, &r.error))
        return r;                                    // nothing rebuilt if it could not be written
    r.ok = true;
    if (!sceneRoot || !rebuild) return r;

    sceneRoot->traverse([&](GameObject* go)
    {
        if (!go->hasMesh()) return;
        bool usaEsteMat = false;
        for (const MaterialOverride& ov : go->materialOverrides)
            if (!ov.matAsset.empty() && samePath(ov.matAsset, mat)) { usaEsteMat = true; break; }
        if (!usaEsteMat) return;
        applyMaterialOverrides(*go);
        rebuild(*go);
        ++r.refreshed;
    });
    return r;
}

ImportSettingsKind importSettingsKindFor(const std::string& ext, bool isDir)
{
    if (isDir) return ImportSettingsKind::None;
    switch (classifyAsset(ext, false))
    {
        case AssetKind::Image:   return ImportSettingsKind::Texture;
        case AssetKind::Audio:   return ImportSettingsKind::Audio;
        case AssetKind::Model3D: return ImportSettingsKind::Model;
        default:                 return ImportSettingsKind::None;
    }
}

AudioImportApplyResult applyAudioImportSettings(const std::filesystem::path& asset,
                                                const AudioImportSettings& settings,
                                                const std::function<void(const std::string&)>& refresh)
{
    AudioImportApplyResult r;
    if (!saveAudioImportSettings(asset, settings, &r.error))
        return r;                                    // nothing refreshed if it could not be written
    r.ok = true;
    if (refresh) refresh(asset.string());
    return r;
}

ModelImportApplyResult applyModelImportSettings(
    const std::filesystem::path& asset,
    const ModelImportSettings& settings,
    const std::function<int(const std::filesystem::path&, float scaleRatio)>& reimport)
{
    ModelImportApplyResult r;
    // The OLD scale is read before writing: the translation of each piece of
    // a model split into children already carries that scale inside the localTransform
    // (collectPieces multiplied it in when doing Add Mesh), and the reimport has to
    // correct it by new/old. The new one is re-read AFTER, already clamped the way
    // ModelLoader will see it.
    const float oldScale = loadModelImportSettings(asset).scale;
    if (!saveModelImportSettings(asset, settings, &r.error))
        return r;                                    // nothing reloaded if it could not be written
    r.ok = true;
    const float newScale = loadModelImportSettings(asset).scale;
    const float ratio    = oldScale > 0.0f ? newScale / oldScale : 1.0f;
    if (reimport) r.refreshed = reimport(asset, std::isfinite(ratio) ? ratio : 1.0f);
    return r;
}

int countSceneReferences(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir)
{
    if (!sceneRoot) return 0;

    int count = 0;
    sceneRoot->traverse([&](GameObject* go)
    {
        auto matches = [&](const std::string& field)
        {
            if (field.empty()) return false;
            return isDir ? pathUnderDir(field, path) : samePath(field, path);
        };

        bool textureMatches = false;
        for (const Material* mat : materialsOf(go))
            if (matches(mat->texturePath) || matches(mat->normalMapPath) ||
                matches(mat->metallicRoughnessPath))
                textureMatches = true;
        for (const MaterialOverride& ov : go->materialOverrides)
            if (matches(ov.matAsset)) textureMatches = true;

        bool meshMatches = go->hasMesh() &&
            (matches(go->getMesh()->sourcePath) || textureMatches);
        bool audioMatches = go->hasAudioClip() && matches(go->getAudioClip()->getPath());
        if (meshMatches || audioMatches)
            ++count;
    });
    return count;
}

void detachSceneReferencesForDelete(EditorContext& ctx, GameObject* sceneRoot,
                                     const std::filesystem::path& path, bool isDir)
{
    if (!sceneRoot) return;

    sceneRoot->traverse([&](GameObject* go)
    {
        auto matches = [&](const std::string& field)
        {
            if (field.empty()) return false;
            return isDir ? pathUnderDir(field, path) : samePath(field, path);
        };

        if (go->hasMesh())
        {
            const Mesh* mesh = go->getMesh().get();
            if (matches(mesh->sourcePath))
            {
                if (ctx.renderer)
                    ctx.renderer->removeMeshComponent(go);
            }
            else
            {
                // The path is ALWAYS cleared when it matches, even if there is no hot-swap:
                // the file leaves the disk, and leaving the path pointing to it
                // would make a later registerGameObject/re-register
                // try stbi_load on a path that no longer exists. For a
                // slot WITHOUT a user override this is also enough between
                // sessions: the material is re-derived from the FBX on every load and
                // nothing is left pointing to the deleted file. The hot-swap to the
                // "missing" texture, on the other hand, only exists for the static
                // pipeline (replaceStaticTextureWithMissing indexes
                // m_objects), so on skinned the path is left empty but the
                // GPU keeps showing the old texture until the next
                // load of the mesh.
                const bool canSwap = ctx.renderer && go->staticRenderIndex >= 0;
                // The writable version (which copies a shared
                // mesh) is only requested if there really is something to clear.
                const bool hayQueLimpiar = tocaAlgunMaterial(go, matches);
                for (Material* mat : hayQueLimpiar ? editMaterialsOf(go) : std::vector<Material*>{})
                {
                    if (matches(mat->texturePath))
                    {
                        mat->texturePath.clear();
                        if (canSwap)
                            ctx.renderer->replaceStaticTextureWithMissing(go->staticRenderIndex, EditorRenderer::TextureSlot::Diffuse);
                    }
                    if (matches(mat->normalMapPath))
                    {
                        mat->normalMapPath.clear();
                        if (canSwap)
                            ctx.renderer->replaceStaticTextureWithMissing(go->staticRenderIndex, EditorRenderer::TextureSlot::Normal);
                    }
                    if (matches(mat->metallicRoughnessPath))
                    {
                        mat->metallicRoughnessPath.clear();
                        if (canSwap)
                            ctx.renderer->replaceStaticTextureWithMissing(go->staticRenderIndex, EditorRenderer::TextureSlot::MetallicRoughness);
                    }
                }
                // GameObject::materialOverrides is what gets serialized
                // (mesh.materials, Task 6): without clearing it here too, the
                // saved override keeps pointing to the file that was just
                // deleted, and the next Save writes that dead path as
                // is; on reopening, applyMaterialOverrides reapplies it over
                // the material freshly derived from the FBX and the texture does not load.
                // Same comparison criterion (matches) as the rest of the
                // function.
                //
                // The base* too, and here NOT because of what is saved to disk
                // (they are not serialized outside the in-memory paths of
                // clone/undo, see the "baseAlbedo" comment in
                // Scene.cpp::nodeToJson) but because of what happens within this
                // same session: a baseline pointing to the just-deleted
                // file is REWRITTEN into the Material on the next
                // applyMaterialOverrides (the "no override: back to
                // baseline" branch), and that next one can be editing ANY OTHER
                // slot. That is, it undid the
                // replaceStaticTextureWithMissing that had just been done, without
                // anything saying so. The trigger is not "doing Clear", which is
                // how it was described, but any reapplication.
                //
                // It is emptied instead of corrected (unlike the rename,
                // where the asset still exists under another name): the file
                // is no longer there, and the *Taken flag is left HIGH on purpose, which
                // is what makes a later Clear return the slot to
                // empty instead of resurrecting the dead path.
                // Unlike albedo/normal/orm, which end up empty and the
                // material only returns to the baseline on the NEXT
                // reapplication (any other edit), matAsset resolved via
                // applyMaterialOverrides already wrote the .mat's value into the
                // Material: if it is left as is, the object looks (and is exported,
                // until the next Save/Load) with the last texture of the deleted
                // .mat. That is why here it IS reapplied and rebuilt
                // immediately, instead of waiting for the next edit.
                bool matAssetTaken = false;
                for (MaterialOverride& ov : go->materialOverrides)
                {
                    if (matches(ov.albedo)) ov.albedo.clear();
                    if (matches(ov.normal)) ov.normal.clear();
                    if (matches(ov.orm))    ov.orm.clear();
                    if (matches(ov.baseAlbedo)) ov.baseAlbedo.clear();
                    if (matches(ov.baseNormal)) ov.baseNormal.clear();
                    if (matches(ov.baseOrm))    ov.baseOrm.clear();
                    if (matches(ov.matAsset))   { ov.matAsset.clear(); matAssetTaken = true; }
                }
                if (matAssetTaken)
                {
                    applyMaterialOverrides(*go);
                    if (ctx.renderer)
                    {
                        if (const SkinnedMesh* sm = go->getSkinnedMesh(); sm && go->skinnedRenderIndex >= 0)
                            ctx.renderer->rebuildSkinnedMesh(go->skinnedRenderIndex, *sm);
                        else if (go->staticRenderIndex >= 0)
                            ctx.renderer->rebuildStaticMesh(go->staticRenderIndex, *go->getMesh());
                    }
                }
            }
        }
        if (go->hasAudioClip() && matches(go->getAudioClip()->getPath()))
        {
            go->setAudioClip(nullptr);
        }
    });
}

void ContentBrowserPanel::beginAssetDelete(GameObject* sceneRoot,
                                           std::vector<std::pair<std::filesystem::path, bool>> targets)
{
    m_assetDeleteTargets        = std::move(targets);
    // With several, it is the per-element sum: an object that uses two of them counts
    // twice. Enough for the warning ("how much is going to break"), it is not an exact count.
    m_assetDeleteAffectedCount  = 0;
    for (const auto& [path, isDir] : m_assetDeleteTargets)
        m_assetDeleteAffectedCount += countSceneReferences(sceneRoot, path, isDir);
    m_assetDeleteError.clear();
    m_openAssetDeletePopup      = true;
}

void ContentBrowserPanel::acceptAssetDropOnFolder(const std::filesystem::path& destDir)
{
    if (!ImGui::BeginDragDropTarget())
        return;
    // The payloads the grid emits: loose files and folders go with different
    // types on purpose (see the comment at the drag source), and a
    // multi-selection goes as DT_ASSET_MULTI, one path per line. Only folders
    // accept the multiple: the Properties zones expect ONE asset.
    for (const char* type : { "DT_ASSET_PATH", "DT_ASSET_DIR", "DT_ASSET_MULTI" })
    {
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(type);
        if (!payload)
            continue;

        std::vector<std::filesystem::path> srcs;
        const std::string data(static_cast<const char*>(payload->Data));
        if (std::string_view(type) == "DT_ASSET_MULTI")
        {
            size_t start = 0;
            while (start < data.size())
            {
                size_t end = data.find('\n', start);
                if (end == std::string::npos) end = data.size();
                if (end > start) srcs.emplace_back(data.substr(start, end - start));
                start = end + 1;
            }
        }
        else
        {
            srcs.emplace_back(data);
        }
        // Dropping a folder onto itself is not an error worth logging.
        srcs.erase(std::remove_if(srcs.begin(), srcs.end(),
                       [&](const std::filesystem::path& s) { return samePath(s, destDir); }),
                   srcs.end());
        if (!srcs.empty())
            m_pendingMove = PendingMove{ std::move(srcs), destDir };
    }
    ImGui::EndDragDropTarget();
}

void ContentBrowserPanel::applyPendingMove(EditorContext& ctx, GameObject* sceneRoot)
{
    if (!m_pendingMove)
        return;
    const PendingMove mv = *m_pendingMove;
    m_pendingMove.reset();

    // Same veto as the asset drops of Properties: with Load Scene in flight
    // the scene is being replaced and rewriting its references makes no sense.
    if (ctx.editingLocked)
    {
        ctx.pushLog("Scene load in progress: the asset move is discarded");
        return;
    }

    // The payload is emitted by this same panel, but it is checked anyway: source and
    // destination have to be inside the project root.
    const bool destInside = samePath(mv.destDir, m_projectRoot) || pathUnderDir(mv.destDir, m_projectRoot);

    int moved = 0;
    for (const std::filesystem::path& src : mv.srcs)
    {
        // Each element of the batch is resolved on its own: one rejection does not abort
        // the others (same criterion as the import of several files).
        if (!pathUnderDir(src, m_projectRoot) || !destInside)
        {
            ctx.pushLog("Move rejected: source or destination outside the project");
            continue;
        }

        std::error_code dirEc;
        const bool isDir = std::filesystem::is_directory(src, dirEc);
        const MoveOutcome outcome = moveAsset(src, mv.destDir);
        switch (outcome.result)
        {
        case MoveResult::Moved:
        {
            ++moved;
            if (!outcome.errorMessage.empty())
                ctx.pushLog("Warning while moving '" + src.filename().string() + "': " + outcome.errorMessage);
            updateSceneReferencesForRename(ctx, sceneRoot, src, outcome.newPath, isDir);
            // If the current folder was the moved one (or hung from it) it no longer exists
            // at that path: follow it to its new place instead of leaving the grid
            // pointing to a folder that disappeared.
            const std::filesystem::path current(m_currentDir);
            if (isDir && samePath(current, src))
                m_currentDir = outcome.newPath.string();
            else if (isDir && pathUnderDir(current, src))
                m_currentDir = replacePathPrefix(m_currentDir, src, outcome.newPath);
            m_scanned = false;
            break;
        }
        case MoveResult::RejectedSameFolder:
            break; // it was already there: nothing to say
        case MoveResult::RejectedIntoSelf:
            ctx.pushLog("A folder cannot be moved into itself");
            break;
        case MoveResult::RejectedNameConflict:
            ctx.pushLog("Move rejected: there is already a '" + src.filename().string() +
                        "' en '" + mv.destDir.filename().string() + "'");
            break;
        case MoveResult::RejectedFailed:
            ctx.pushLog("Could not move '" + src.filename().string() + "': " + outcome.errorMessage);
            break;
        }
    }

    if (moved == 1 && mv.srcs.size() == 1)
        ctx.pushLog("Asset moved: '" + mv.srcs[0].filename().string() + "' -> '" +
                    mv.destDir.filename().string() + "'");
    else if (moved == 1)
        ctx.pushLog("1 asset moved to '" + mv.destDir.filename().string() + "'");
    else if (moved > 1)
        ctx.pushLog(std::to_string(moved) + " assets moved to '" + mv.destDir.filename().string() + "'");
}

void ContentBrowserPanel::drawFolderTree(const std::filesystem::path& dir)
{
    std::vector<std::filesystem::path> subdirs = listVisibleSubdirs(dir);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (subdirs.empty())
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (samePath(dir, std::filesystem::path(m_currentDir)))
        flags |= ImGuiTreeNodeFlags_Selected;
    if (samePath(dir, m_projectRoot))
        flags |= ImGuiTreeNodeFlags_DefaultOpen;

    // If the selected folder hangs from this one, force the branch open so
    // it is visible (e.g. after a double click on a folder of the right grid).
    // Only when m_revealCurrentDir is active (a single frame): if it
    // were done on every frame, the user could never collapse by hand
    // an ancestor of the selected folder.
    if (m_revealCurrentDir && pathUnderDir(std::filesystem::path(m_currentDir), dir))
        ImGui::SetNextItemOpen(true);

    ImGui::PushID(dir.string().c_str());
    bool open = ImGui::TreeNodeEx("##node", flags, "%s", dir.filename().string().c_str());

    // IsItemToggledOpen: pressing the arrow expands, but does not change the selection.
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
    {
        m_currentDir = dir.string();
        m_scanned    = false;
    }

    // Right after the node: BeginDragDropTarget operates on the last item.
    acceptAssetDropOnFolder(dir);

    if (open)
    {
        for (const auto& sub : subdirs)
            drawFolderTree(sub);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void ContentBrowserPanel::draw(EditorContext& ctx, GameObject* sceneRoot)
{
    if (!m_open)
    {
        // Drain anyway even if the window is closed: otherwise, the queue in
        // sandbox/main.cpp grows without limit while the panel is not in
        // view, and on reopening a whole batch would be imported at once against
        // already obsolete screen coordinates. Without a visible panel there is no rect
        // to compare against, so these drops are silently discarded;
        // the user could not drop onto something they could not see.
        if (ctx.takeDroppedFiles)
            ctx.takeDroppedFiles();
        return;
    }

    // Decision from the previous frame's modal (or direct click): the load happens
    // here, outside any popup and before opening the window.
    if (m_scenePromptChoice != ScenePromptChoice::None)
    {
        const std::filesystem::path target = m_sceneLoadTarget;
        const ScenePromptChoice     choice = m_scenePromptChoice;
        m_sceneLoadTarget.clear();
        m_scenePromptChoice = ScenePromptChoice::None;
        if (choice == ScenePromptChoice::Save)
        {
            if (ctx.requestSaveScene) ctx.requestSaveScene(target);
        }
        else if (ctx.requestLoadScene)
        {
            ctx.requestLoadScene(target);
        }
    }

    // ImGui::Begin returns false for a docked tab without focus (or a collapsed
    // window): in that case the panel is not really in view, even though
    // m_open is still true, and dropping onto what IS seen (another tab of the same
    // dock, e.g.) must not import here; visible gates the drop block.
    const bool visible = ImGui::Begin("Content Browser", &m_open);
    float totalWidth  = ImGui::GetContentRegionAvail().x;
    float totalHeight = ImGui::GetContentRegionAvail().y;
    float leftWidth   = totalWidth * 0.38f;

    // Browser root: the open project's folder. The tree and the grid
    // only list children of m_projectRoot and there is no up button, so fixing
    // the root here is what prevents seeing (and dragging) assets from another project.
    // Without a project (headless tests) it falls back to the cwd, as before the concept
    // existed. If the root changes, the current directory goes back to it.
    if (ctx.project && ctx.project->valid() && m_projectRoot != ctx.project->root())
    {
        m_projectRoot = ctx.project->root();
        m_currentDir  = m_projectRoot.string();
    }

    if (m_projectRoot.empty())
    {
        std::error_code cwdEc, canonEc;
        std::filesystem::path cwd = std::filesystem::current_path(cwdEc);
        // current_path returns an empty path on failure, and canonical("") always
        // fails: without this fallback m_projectRoot would end up empty.
        if (cwdEc)
            cwd = ".";
        std::filesystem::path canon = std::filesystem::canonical(cwd, canonEc);
        // If canonical or current_path fail (permissions, cwd deleted from under
        // our feet), do not leave m_projectRoot empty: that would retry this block
        // every frame. Fall back to the best available value.
        m_projectRoot = canonEc ? cwd : canon;
    }
    if (m_currentDir.empty())
        m_currentDir = m_projectRoot.string();

    // OS-level drop (Explorer -> window): consumed once per frame, and
    // only what lands inside THIS window's rect matters; dropping onto
    // another docked panel does nothing here (nobody else claims it). If the
    // panel is not visible (background tab, collapsed) the queue is still drained
    // so as not to accumulate it, but it is simply discarded: without a real rect there is
    // nothing to compare the drop position against.
    if (ctx.takeDroppedFiles)
    {
        if (!visible)
        {
            ctx.takeDroppedFiles();
        }
        else
        {
            const ImVec2 winPos  = ImGui::GetWindowPos();
            const ImVec2 winSize = ImGui::GetWindowSize();
            std::vector<AssetImportOutcome> outcomes = importDroppedFilesInto(
                ctx.takeDroppedFiles(), winPos.x, winPos.y, winSize.x, winSize.y, m_currentDir);
            for (const AssetImportOutcome& o : outcomes)
            {
                if (o.result == AssetImportResult::Copied)
                {
                    ctx.pushLog("Asset imported: " + o.destPath.filename().string());
                    m_scanned = false;
                }
                else
                {
                    ctx.pushLog("Import rejected (" + o.sourcePath.filename().string() + "): " +
                                describeImportResult(o));
                }
            }
        }
    }

    // Left: folder tree
    ImGui::BeginChild("##FolderTreePane", ImVec2(leftWidth, totalHeight), false);
    drawFolderTree(m_projectRoot);
    ImGui::EndChild();
    // One-frame reveal (see the comment next to the declaration in the
    // header): once the tree has been drawn with the branch forced open, it is
    // cleared so that the user can collapse it by hand again.
    m_revealCurrentDir = false;

    // The drop targets (tree and grid folders) only note the
    // move; it is applied here, with the tree already drawn and BEFORE scanning the
    // grid, so that this same frame already sees the result.
    applyPendingMove(ctx, sceneRoot);

    ImGui::SameLine();

    // Right: Asset browser with type icons
    ImGui::BeginChild("##AssetPane", ImVec2(0, totalHeight), false);
    {
        // Thumbnails: the cache is created the first time the backend provides an atlas and there is a
        // JobSystem; changing folder opens a new generation (what was pending from
        // the previous one no longer matters) and every frame starts with beginFrame.
        if (!m_thumbs && ctx.renderer && ctx.jobs)
        {
            const uint64_t atlasId = ctx.renderer->uiThumbnailAtlasId();
            if (atlasId != 0)
            {
                m_thumbAtlasId = atlasId;
                EditorRenderer* renderer = ctx.renderer;
                JobSystem*      jobs     = ctx.jobs;
                // Next to the sources one, in a folder with a dot: .gitignore already
                // excludes it and isHiddenDir hides it from the Content Browser itself.
                std::shared_ptr<const ThumbnailDiskCache> disk;
                if (!m_projectRoot.empty())
                    disk = std::make_shared<ThumbnailDiskCache>(m_projectRoot / ".dt-cache" / "thumbs");
                m_thumbs = std::make_unique<ThumbnailCache>(
                    [jobs](std::function<void()> job) { return jobs->submit(std::move(job)) != 0; },
                    [renderer](const ThumbnailTile* tiles, size_t count) {
                        return renderer->uploadUiThumbnails(tiles, count);
                    },
                    4, kThumbSlotCount, ThumbnailCache::Decoder{}, std::move(disk));
                m_thumbDir = m_currentDir;
            }
        }
        if (m_thumbs)
        {
            if (m_thumbDir != m_currentDir)
            {
                m_thumbs->newGeneration();
                m_thumbDir = m_currentDir;
            }
            m_thumbs->beginFrame();
        }

        const double now = ImGui::GetTime();
        if (!m_scanned)
        {
            m_assets      = listVisibleEntries(m_currentDir);
            m_scanned     = true;
            m_lastPollTime = now;
        }
        else if (visible && now - m_lastPollTime >= kDirPollIntervalSeconds)
        {
            // Polling: someone may have created, deleted or renamed things outside the
            // editor. The current folder is re-read and the list is only replaced if it
            // differs, so at rest there is no change at all. The left tree
            // does not need this: it already rescans every frame.
            m_lastPollTime = now;
            // Regenerate the thumbnails of whatever changed content under the same name.
            if (m_thumbs) m_thumbs->refreshStamps();
            const std::filesystem::path stillThere =
                nearestExistingDir(std::filesystem::path(m_currentDir), m_projectRoot);
            if (!samePath(stillThere, std::filesystem::path(m_currentDir)))
            {
                // The current folder was deleted from outside: go up to the ancestor that
                // remains (without leaving the root) and have the tree show that branch.
                m_currentDir       = stillThere.string();
                m_scanned          = false;
                m_revealCurrentDir = true;
            }
            else
            {
                std::vector<std::filesystem::path> fresh = listVisibleEntries(m_currentDir);
                if (fresh != m_assets)
                    m_assets = std::move(fresh);
            }
        }

        constexpr float ICON_SIZE = 56.0f;
        constexpr float CELL_PAD  = 12.0f;
        float cellW = ICON_SIZE + CELL_PAD;
        float paneW = ImGui::GetContentRegionAvail().x;
        int   cols  = std::max(1, (int)(paneW / cellW));

        // Breadcrumb: root > ... > current folder. Each segment jumps to its folder;
        // the last one (the current) is disabled because pressing it would do nothing.
        // A tree reveal is requested in case the user had collapsed that branch.
        {
            const std::vector<BreadcrumbSegment> segments =
                breadcrumbSegments(m_projectRoot, std::filesystem::path(m_currentDir));
            for (size_t i = 0; i < segments.size(); ++i)
            {
                if (i > 0)
                {
                    ImGui::SameLine(0.0f, 4.0f);
                    ImGui::TextDisabled(">");
                    ImGui::SameLine(0.0f, 4.0f);
                }
                const bool isLast = (i + 1 == segments.size());
                ImGui::PushID(static_cast<int>(i));
                ImGui::BeginDisabled(isLast);
                if (ImGui::SmallButton(segments[i].name.c_str()))
                {
                    m_currentDir       = segments[i].path.string();
                    m_scanned          = false;
                    m_revealCurrentDir = true;
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }

        // Grid filters (current folder only): text by name and combo by
        // type. They are drawn BEFORE Columns: inside a column the text
        // field would shrink to the width of one cell.
        {
            struct KindOption { const char* label; std::optional<AssetKind> kind; };
            static const KindOption kOptions[] = {
                {"Todos", std::nullopt},           {"Folders", AssetKind::Folder},
                {"3D", AssetKind::Model3D},        {"Audio", AssetKind::Audio},
                {"Image", AssetKind::Image},      {"Font", AssetKind::Font},
                {"Scene", AssetKind::Scene},      {"Script", AssetKind::Script},
                {"Shader", AssetKind::Shader},     {"Material", AssetKind::Material},
                {"Other", AssetKind::Other},
            };
            ImGui::SetNextItemWidth(std::max(80.0f, paneW - 140.0f));
            ImGui::InputTextWithHint("##AssetFilterText", "Search by name...",
                                     m_filterText, sizeof(m_filterText));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##AssetFilterKind", kOptions[m_filterKindIndex].label))
            {
                for (int i = 0; i < (int)(sizeof(kOptions) / sizeof(kOptions[0])); ++i)
                    if (ImGui::Selectable(kOptions[i].label, i == m_filterKindIndex))
                        m_filterKindIndex = i;
                ImGui::EndCombo();
            }
            m_filterKind = kOptions[m_filterKindIndex].kind;
        }

        // What passes the filter, resolved ONCE: the range selection
        // (Shift+click) needs the complete visible order before drawing, and this way
        // the loop below does not repeat the stat per element.
        struct GridItem { std::filesystem::path path; bool isDir; std::string ext; AssetKind kind; };
        std::vector<GridItem>              items;
        std::vector<std::filesystem::path> visible;
        for (const auto& p : m_assets)
        {
            std::error_code isDirEc;
            const bool isDir = std::filesystem::is_directory(p, isDirEc);
            std::string ext = isDir ? "" : p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            const AssetKind kind = classifyAsset(ext, isDir);
            if (!assetMatchesFilter(p.filename().string(), kind, m_filterText, m_filterKind))
                continue;
            items.push_back({ p, isDir, std::move(ext), kind });
            visible.push_back(p);
        }
        // Changing folder, filtering or rescanning leaves no ghost selection.
        pruneSelection(m_selection, visible);

        ImGui::Columns(cols, "##AssetGrid", false);

        for (const GridItem& item : items) {
            const std::filesystem::path& path = item.path;
            const bool                   isDir = item.isDir;
            const std::string&           ext   = item.ext;
            const AssetKind              kind  = item.kind;

            ImVec4      btnColor;
            const char* label;
            switch (kind) {
            case AssetKind::Folder:  btnColor = ImVec4(0.55f, 0.55f, 0.60f, 1.0f); label = "DIR"; break;
            case AssetKind::Model3D: btnColor = ImVec4(0.15f, 0.55f, 0.85f, 1.0f); label = "3D";  break;
            case AssetKind::Audio:   btnColor = ImVec4(0.20f, 0.72f, 0.35f, 1.0f); label = "SFX"; break;
            case AssetKind::Image:   btnColor = ImVec4(0.85f, 0.72f, 0.10f, 1.0f); label = "IMG"; break;
            case AssetKind::Font:    btnColor = ImVec4(0.65f, 0.40f, 0.80f, 1.0f); label = "FNT"; break;
            case AssetKind::Scene:   btnColor = ImVec4(0.20f, 0.65f, 0.65f, 1.0f); label = "SCN"; break;
            case AssetKind::Script:  btnColor = ImVec4(0.30f, 0.40f, 0.85f, 1.0f); label = "LUA"; break;
            case AssetKind::Shader:  btnColor = ImVec4(0.80f, 0.35f, 0.10f, 1.0f); label = "SPV"; break;
            case AssetKind::Material: btnColor = ImVec4(0.75f, 0.55f, 0.20f, 1.0f); label = "MAT"; break;
            default:                 btnColor = ImVec4(0.40f, 0.40f, 0.40f, 1.0f); label = "..."; break;
            }

            ImGui::PushID(path.string().c_str());
            // Only what is in view: a folder of thousands of assets must not
            // launch thousands of decodes.
            std::optional<UvRect> thumb;
            if (m_thumbs && wantsThumbnail(kind) &&
                ImGui::IsRectVisible(ImVec2(ICON_SIZE, ICON_SIZE)))
            {
                thumb = m_thumbs->request(path);
                // An FBX without a mesh (only clips) is not a failure: its own icon.
                if (!thumb && kind == AssetKind::Model3D &&
                    m_thumbs->status(path) == ThumbnailStatus::AnimationOnly)
                {
                    btnColor = ImVec4(0.10f, 0.40f, 0.60f, 1.0f);
                    label    = "ANI";
                }
            }
            // Selected: light border and more vivid color. The border is pushed BEFORE
            // the button colors so that it can be popped last.
            const bool selected = m_selection.contains(path);
            if (selected)
            {
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.9f));
                btnColor = ImVec4(btnColor.x + 0.10f, btnColor.y + 0.10f, btnColor.z + 0.10f, 1.0f);
            }
            ImGui::PushStyleColor(ImGuiCol_Button, btnColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(btnColor.x + 0.15f, btnColor.y + 0.15f, btnColor.z + 0.15f, 1.0f));
            // With a thumbnail the button goes without a label and the image is drawn on top,
            // inside the selection border.
            const bool clicked = ImGui::Button(assetIconButtonLabel(label, thumb.has_value()).c_str(),
                                               ImVec2(ICON_SIZE, ICON_SIZE));
            ImGui::PopStyleColor(2);
            if (selected)
            {
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
            }
            if (thumb)
            {
                const ImVec2 mn  = ImGui::GetItemRectMin();
                const ImVec2 mx  = ImGui::GetItemRectMax();
                const float  pad = 3.0f;
                ImGui::GetWindowDrawList()->AddImage(
                    (ImTextureID)m_thumbAtlasId,
                    ImVec2(mn.x + pad, mn.y + pad), ImVec2(mx.x - pad, mx.y - pad),
                    ImVec2(thumb->u0, thumb->v0), ImVec2(thumb->u1, thumb->v1));
            }
            if (clicked)
            {
                const ImGuiIO& io = ImGui::GetIO();
                applyAssetClick(m_selection, visible, path, io.KeyCtrl, io.KeyShift);
            }

            // Dropping an asset onto a grid folder moves it inside. It has
            // to go here, before any other widget: it operates on the button.
            if (isDir)
                acceptAssetDropOnFolder(path);

            if (isDir && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                m_currentDir        = path.string();
                m_scanned           = false;
                // This folder may not be visible yet in the tree (it was
                // selected from the grid, not clicked in the tree), so
                // we request a one-frame reveal to force open
                // its ancestor branch. A click in the tree does not need
                // this: the folder clicked there is already visible by
                // construction.
                m_revealCurrentDir  = true;
            }

            if (!isDir && ext == ".lua" && ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                ctx.openScript(path);
            }

            if (!isDir && ext == ".json" && ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                if (ctx.isPlaying)
                {
                    // Same reason as the toolbar's Save/Load: what is in
                    // memory during Play is simulation state, and loading
                    // another scene would leave the Stop snapshot describing a
                    // scene that no longer exists.
                    if (ctx.pushLog)
                        ctx.pushLog("Stop Play Mode to load a scene");
                }
                else
                {
                    m_sceneLoadTarget = path;
                    // With no pending changes it loads directly (no modal), but
                    // still on the next frame: reloading the scene in the
                    // middle of the loop that walks m_assets is not allowed.
                    if (ctx.undo && ctx.undo->isSceneDirty())
                        m_openScenePromptPopup = true;
                    else
                        m_scenePromptChoice = ScenePromptChoice::Discard;
                }
            }

            if (!isDir && ext == ".mat" && ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                m_matAssetTarget = path;
                m_matAssetEdit   = loadMaterialAsset(path);
                m_matAssetError.clear();
                m_matAssetWindowOpen = true;
            }

            // Files and FOLDERS are dragged with DIFFERENT payloads on
            // purpose: the 14 drop zones that already exist expect a file
            // of a specific extension, and with a separate type none accepts a
            // folder by accident.
            // Dragging several selected items (starting from one of them) goes out with
            // its own payload even if one is not "draggable" on its own: moving
            // does not depend on which drop zones know how to accept the type.
            const bool inMultiSelection = m_selection.items.size() > 1 && m_selection.contains(path);
            const bool arrastrable = isAssetDraggable(ext, isDir, inMultiSelection);
            if (arrastrable && ImGui::BeginDragDropSource())
            {
                // Dragging something that was not selected makes it the
                // selection (as in Explorer).
                if (!m_selection.contains(path))
                    applyAssetClick(m_selection, visible, path, false, false);

                if (m_selection.items.size() > 1)
                {
                    std::string joined;
                    for (const auto& p : m_selection.items)
                        joined += p.string() + "\n";
                    ImGui::SetDragDropPayload("DT_ASSET_MULTI", joined.c_str(), joined.size() + 1);
                    ImGui::Text("%zu items", m_selection.items.size());
                }
                else
                {
                    std::string fullPath = path.string();
                    ImGui::SetDragDropPayload(isDir ? "DT_ASSET_DIR" : "DT_ASSET_PATH",
                                              fullPath.c_str(), fullPath.size() + 1);
                    ImGui::Text("%s", fullPath.c_str());
                }
                ImGui::EndDragDropSource();
            }

            // Right click on something not selected selects it by itself, like
            // Explorer; on the selection, the menu acts on all of it.
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !m_selection.contains(path))
                applyAssetClick(m_selection, visible, path, false, false);
            if (ImGui::BeginPopupContextItem())
            {
                const size_t selCount = m_selection.items.size();
                // Renaming is one at a time.
                if (ImGui::MenuItem("Rename", nullptr, false, selCount <= 1))
                    beginAssetRename(path, isDir);
                // Import settings: only for ONE asset with settings (texture, audio or model).
                const ImportSettingsKind importKind = importSettingsKindFor(path.extension().string(), isDir);
                if (selCount <= 1 && importKind != ImportSettingsKind::None &&
                    ImGui::MenuItem("Import Settings..."))
                {
                    m_importTarget = path;
                    m_importKind   = importKind;
                    if (importKind == ImportSettingsKind::Audio)
                        m_importAudioEdit = loadAudioImportSettings(path);
                    else if (importKind == ImportSettingsKind::Model)
                        m_importModelEdit = loadModelImportSettings(path);
                    else
                        m_importEdit = loadTextureImportSettings(path);
                    m_importError.clear();
                    m_openImportPopup = true;
                }
                const std::string deleteLabel =
                    selCount > 1 ? "Delete (" + std::to_string(selCount) + ")" : std::string("Delete");
                if (ImGui::MenuItem(deleteLabel.c_str()))
                {
                    std::vector<std::pair<std::filesystem::path, bool>> targets;
                    for (const auto& p : m_selection.items)
                    {
                        std::error_code tEc;
                        targets.emplace_back(p, std::filesystem::is_directory(p, tEc));
                    }
                    if (targets.empty())
                        targets.emplace_back(path, isDir);
                    beginAssetDelete(sceneRoot, std::move(targets));
                }
                ImGui::EndPopup();
            }

            std::string fname = path.filename().string();
            if (fname.size() > 11) fname = fname.substr(0, 10) + "..";
            ImGui::TextUnformatted(fname.c_str());

            ImGui::NextColumn();
            ImGui::PopID();
        }
        // Collects what was decoded and uploads it to the atlas: visible from the next frame.
        if (m_thumbs) m_thumbs->pump();

        ImGui::Columns(1);

        // Left click on empty grid space deselects. IsAnyItemHovered leaves out
        // the widgets above (breadcrumb, filters) and the icons.
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !ImGui::IsAnyItemHovered())
            m_selection.clear();

        // Right click on empty grid space (on an asset its own Rename/Delete menu
        // comes up): same pattern as the ScenePanel's Create menu. The
        // folder is created on disk right away with a free name and Rename is opened,
        // as Unity does, so that the user names it without extra steps.
        if (ImGui::BeginPopupContextWindow("##AssetPaneContext",
                ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
        {
            if (ImGui::BeginMenu("Create"))
            {
                if (ImGui::MenuItem("Folder"))
                {
                    const std::filesystem::path parent(m_currentDir);
                    const std::filesystem::path created = parent / uniqueFolderName(parent);
                    std::error_code mkEc;
                    std::filesystem::create_directory(created, mkEc);
                    if (mkEc)
                    {
                        ctx.pushLog("Could not create the folder: " + mkEc.message());
                    }
                    else
                    {
                        ctx.pushLog("Folder created: " + created.filename().string());
                        m_scanned = false;
                        beginAssetRename(created, /*isDir=*/true);
                    }
                }
                if (ImGui::MenuItem("Material"))
                {
                    const std::filesystem::path parent(m_currentDir);
                    const std::filesystem::path created = parent / uniqueMaterialName(parent);
                    std::string saveErr;
                    if (!saveMaterialAsset(created, MaterialAsset{}, &saveErr))
                    {
                        ctx.pushLog("Could not create the material: " + saveErr);
                    }
                    else
                    {
                        ctx.pushLog("Material created: " + created.filename().string());
                        m_scanned = false;
                        beginAssetRename(created, /*isDir=*/false);
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }

        if (m_openScenePromptPopup)
        {
            ImGui::OpenPopup("Save scene changes");
            m_openScenePromptPopup = false;
        }
        if (ImGui::BeginPopupModal("Save scene changes", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("The current scene has unsaved changes.");
            ImGui::TextUnformatted("Save the changes to the current scene?");
            ImGui::Text("Will load: %s", m_sceneLoadTarget.filename().string().c_str());
            ImGui::Separator();
            if (ImGui::Button("Save"))
            {
                m_scenePromptChoice = ScenePromptChoice::Save;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Don't save"))
            {
                m_scenePromptChoice = ScenePromptChoice::Discard;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                m_sceneLoadTarget.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (m_openAssetRenamePopup)
        {
            ImGui::OpenPopup("Rename Asset");
            m_openAssetRenamePopup = false;
        }
        if (ImGui::BeginPopupModal("Rename Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();

            bool enterPressed = ImGui::InputText("##assetRenameInput", m_assetRenameBuffer,
                                                  sizeof(m_assetRenameBuffer),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
            if (!m_assetRenameIsDir)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", m_assetRenameTarget.extension().string().c_str());
            }
            if (!m_assetRenameError.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_assetRenameError.c_str());
            ImGui::Separator();
            bool accept = ImGui::Button("Accept") || enterPressed;
            ImGui::SameLine();
            bool cancel = ImGui::Button("Cancel");

            if (accept)
            {
                std::string newStem = trim(m_assetRenameBuffer);
                if (!isValidFileName(newStem))
                {
                    m_assetRenameError = "Invalid name";
                }
                else
                {
                    std::string newName = m_assetRenameIsDir
                        ? newStem
                        : (newStem + m_assetRenameTarget.extension().string());
                    std::filesystem::path newPath = m_assetRenameTarget.parent_path() / newName;
                    std::error_code existsEc;
                    if (!samePath(newPath, m_assetRenameTarget) && std::filesystem::exists(newPath, existsEc))
                    {
                        m_assetRenameError = "A file/folder with that name already exists";
                    }
                    else
                    {
                        const RenameFileOutcome renamed =
                            renameAssetFile(m_assetRenameTarget, newPath, m_assetRenameIsDir);
                        if (!renamed.ok)
                        {
                            m_assetRenameError = renamed.error;
                        }
                        else
                        {
                            if (!renamed.warning.empty())
                                ctx.pushLog(renamed.warning);
                            ctx.pushLog("Asset renamed: '" + m_assetRenameTarget.filename().string() +
                                    "' -> '" + newPath.filename().string() + "'");
                            updateSceneReferencesForRename(ctx, sceneRoot, m_assetRenameTarget, newPath, m_assetRenameIsDir);
                            m_scanned = false;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
            }
            else if (cancel)
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (m_openAssetDeletePopup)
        {
            ImGui::OpenPopup("Delete Asset");
            m_openAssetDeletePopup = false;
        }
        if (ImGui::BeginPopupModal("Delete Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (m_assetDeleteTargets.size() == 1)
                ImGui::Text("Delete '%s'?", m_assetDeleteTargets[0].first.filename().string().c_str());
            else
                ImGui::Text("Delete %zu items?", m_assetDeleteTargets.size());
            if (m_assetDeleteAffectedCount > 0)
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                    "%d object(s) use it and will lose the reference.", m_assetDeleteAffectedCount);
            if (!m_assetDeleteError.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_assetDeleteError.c_str());
            ImGui::Separator();
            bool confirm = ImGui::Button("Delete");
            ImGui::SameLine();
            bool cancel = ImGui::Button("Cancel");

            if (confirm)
            {
                // Each element on its own: one that fails does not prevent deleting the
                // others. Those that fail stay in the modal, with the first error.
                std::vector<std::pair<std::filesystem::path, bool>> failed;
                std::string firstError;
                for (const auto& [target, isDir] : m_assetDeleteTargets)
                {
                    const std::error_code removeEc = removeAssetPath(target, isDir);

                    if (removeEc)
                    {
                        if (firstError.empty()) firstError = removeEc.message();
                        failed.emplace_back(target, isDir);
                    }
                    else
                    {
                        ctx.pushLog("Asset deleted: " + target.string());
                        detachSceneReferencesForDelete(ctx, sceneRoot, target, isDir);
                    }
                }
                m_scanned = false;
                if (failed.empty())
                {
                    m_assetDeleteTargets.clear();
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    m_assetDeleteTargets = std::move(failed);
                    m_assetDeleteError = firstError;
                }
            }
            else if (cancel)
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Import settings of ONE asset (texture, audio or model; context
        // menu). Apply writes the sidecar and refreshes the asset's users
        // (materials, voices or meshes); if it cannot be written, the modal
        // stays open with the error.
        if (m_openImportPopup)
        {
            ImGui::OpenPopup("Import Settings");
            m_openImportPopup = false;
        }
        if (ImGui::BeginPopupModal("Import Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("%s", m_importTarget.filename().string().c_str());
            ImGui::Separator();

            if (m_importKind == ImportSettingsKind::Audio)
            {
                // It edits a copy and nothing is applied until "Apply": a
                // normal SliderFloat is enough (there is no per-frame write to defer).
                ImGui::SliderFloat("Gain (dB)", &m_importAudioEdit.gainDb,
                                   kAudioGainMinDb, kAudioGainMaxDb, "%.1f dB");
                ImGui::Checkbox("Force mono", &m_importAudioEdit.forceMono);
                ImGui::TextDisabled("The gain is added to the component's volume.");
                ImGui::TextDisabled("Mono: 2D clips only, from the next playback on.");
            }
            else if (m_importKind == ImportSettingsKind::Model)
            {
                ImGui::DragFloat("Scale", &m_importModelEdit.scale, 0.01f,
                                 kModelScaleMin, kModelScaleMax, "%.4f");
                int normals = static_cast<int>(m_importModelEdit.normals);
                if (ImGui::Combo("Normals", &normals,
                                 "From file (flat if missing)\0Smooth (regenerate)\0Flat (regenerate)\0"))
                    m_importModelEdit.normals = static_cast<NormalsMode>(normals);
                ImGui::Checkbox("Recompute tangents", &m_importModelEdit.calcTangents);
                ImGui::Checkbox("Flip UVs", &m_importModelEdit.flipUVs);
                ImGui::Checkbox("Import animations", &m_importModelEdit.importAnimations);
                ImGui::TextDisabled("Applies to every object that uses this model.");
                ImGui::TextDisabled("Colliders are not resized.");
            }
            else
            {
                int colorSpace = static_cast<int>(m_importEdit.colorSpace);
                if (ImGui::Combo("Color space", &colorSpace, "Auto (per slot)\0sRGB\0Linear\0"))
                    m_importEdit.colorSpace = static_cast<ColorSpaceOverride>(colorSpace);
                ImGui::Checkbox("Mipmaps", &m_importEdit.mipmaps);
                ImGui::TextDisabled("Auto: base color sRGB, normal and ORM linear.");
            }

            if (!m_importError.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_importError.c_str());
            ImGui::Separator();

            const bool apply  = ImGui::Button("Apply");
            ImGui::SameLine();
            const bool cancel = ImGui::Button("Cancel");

            if (apply && m_importKind == ImportSettingsKind::Audio)
            {
                const AudioImportApplyResult r = applyAudioImportSettings(
                    m_importTarget, m_importAudioEdit,
                    [&ctx](const std::string& p) { if (ctx.audio) ctx.audio->refreshImportSettings(p); });
                if (r.ok)
                {
                    ctx.pushLog("Import settings applied: " + m_importTarget.filename().string());
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    m_importError = r.error;   // the modal does NOT close
                }
            }
            else if (apply && m_importKind == ImportSettingsKind::Model)
            {
                const ModelImportApplyResult r = applyModelImportSettings(
                    m_importTarget, m_importModelEdit,
                    [&](const std::filesystem::path& p, float scaleRatio)
                    {
                        const ModelReimportResult mr = reimportModelUsers(sceneRoot, p, ctx.renderer, scaleRatio);
                        for (const std::string& w : mr.warnings) ctx.pushLog(w);
                        return mr.reimported;
                    });
                if (r.ok)
                {
                    ctx.pushLog("Import settings applied: " + m_importTarget.filename().string() +
                                " (" + std::to_string(r.refreshed) + " object(s) reloaded)");
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    m_importError = r.error;   // the modal does NOT close
                }
            }
            else if (apply)
            {
                // The same pair of calls as MaterialTextureCommand::apply.
                const TextureImportApplyResult r = applyTextureImportSettings(
                    sceneRoot, m_importTarget, m_importEdit,
                    [&ctx](GameObject& go)
                    {
                        if (!ctx.renderer) return;
                        if (const SkinnedMesh* sm = go.getSkinnedMesh(); sm && go.skinnedRenderIndex >= 0)
                            ctx.renderer->rebuildSkinnedMesh(go.skinnedRenderIndex, *sm);
                        else if (go.staticRenderIndex >= 0)
                            ctx.renderer->rebuildStaticMesh(go.staticRenderIndex, *go.getMesh());
                    });
                if (r.ok)
                {
                    ctx.pushLog("Import settings applied: " + m_importTarget.filename().string() +
                                " (" + std::to_string(r.refreshed) + " object(s) updated)");
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    m_importError = r.error;   // the modal does NOT close
                }
            }
            else if (cancel)
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // NON-modal floating window, not a popup: dragging a texture from
        // this same grid to the "Drop image here" below requires the grid
        // to stay interactable while the window is open, and a modal
        // popup blocks the rest of the editor by definition (finding by the
        // user in manual verification: with BeginPopupModal neither the grid nor
        // any other panel could be touched, so the drag&drop of this
        // same section was impossible to trigger). Same pattern as
        // ScriptEditorPanel::draw() ("Script Editor", &m_open).
        if (m_matAssetWindowOpen)
        {
            if (!m_matAssetFileDialog) m_matAssetFileDialog = std::make_unique<IGFD::FileDialog>();
            ImGui::Begin("Material", &m_matAssetWindowOpen, ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::Text("%s", m_matAssetTarget.filename().string().c_str());
            ImGui::Separator();

            struct MatSlot { const char* nombre; DonTopo::MaterialTextureSlot slot; std::string* dest; };
            const MatSlot slots[3] = {
                { "Albedo",             DonTopo::MaterialTextureSlot::Albedo, &m_matAssetEdit.albedo },
                { "Normal Map",         DonTopo::MaterialTextureSlot::Normal, &m_matAssetEdit.normal },
                { "Metallic/Roughness", DonTopo::MaterialTextureSlot::Orm,    &m_matAssetEdit.orm    },
            };
            for (const MatSlot& s : slots)
            {
                ImGui::PushID(s.nombre);
                ImGui::Text("%s: %s", s.nombre,
                            s.dest->empty() ? "Inherit from model"
                                            : std::filesystem::path(*s.dest).filename().string().c_str());
                if (ImGui::Button("Browse..."))
                {
                    m_matAssetDlgSlot = s.slot;
                    m_matAssetDlgOpen = true;
                    IGFD::FileDialogConfig cfg;
                    cfg.path = "assets";
                    m_matAssetFileDialog->OpenDialog("PickMatTextureDlg", "Choose image",
                                                     ".png,.jpg,.jpeg,.bmp,.tga", cfg);
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear")) s.dest->clear();
                ImGui::BeginChild((std::string("##MatDrop") + s.nombre).c_str(), ImVec2(0, 30), true);
                ImGui::TextDisabled("Drop image here");
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DT_ASSET_PATH"))
                    {
                        // Same veto as the rest of the editor: the grid's drop
                        // only brings paths ALREADY in the project, but nothing
                        // prevents dropping a .fbx or a .wav here without this
                        // filter (finding by the final reviewer).
                        const std::string dropped = static_cast<const char*>(payload->Data);
                        if (classifyAsset(std::filesystem::path(dropped).extension().string(), false)
                            == AssetKind::Image)
                            *s.dest = dropped;
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::EndChild();
                ImGui::PopID();
            }

            bool heredaMetallic = m_matAssetEdit.metallic < 0.0f;
            if (ImGui::Checkbox("Inherit Metallic", &heredaMetallic))
                m_matAssetEdit.metallic = heredaMetallic ? -1.0f : 0.5f;
            if (!heredaMetallic)
                ImGui::SliderFloat("Metallic", &m_matAssetEdit.metallic, 0.0f, 1.0f, "%.2f");

            bool heredaRoughness = m_matAssetEdit.roughness < 0.0f;
            if (ImGui::Checkbox("Inherit Roughness", &heredaRoughness))
                m_matAssetEdit.roughness = heredaRoughness ? -1.0f : 0.5f;
            if (!heredaRoughness)
                ImGui::SliderFloat("Roughness", &m_matAssetEdit.roughness, 0.0f, 1.0f, "%.2f");

            if (!m_matAssetError.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_matAssetError.c_str());
            ImGui::Separator();

            const bool matApply  = ImGui::Button("Apply");
            ImGui::SameLine();
            const bool matCancel = ImGui::Button("Cancel");
            if (matApply)
            {
                const MaterialAssetApplyResult r = applyMaterialAssetSettings(
                    sceneRoot, m_matAssetTarget, m_matAssetEdit,
                    [&ctx](GameObject& go)
                    {
                        if (!ctx.renderer) return;
                        if (const SkinnedMesh* sm = go.getSkinnedMesh(); sm && go.skinnedRenderIndex >= 0)
                            ctx.renderer->rebuildSkinnedMesh(go.skinnedRenderIndex, *sm);
                        else if (go.staticRenderIndex >= 0)
                            ctx.renderer->rebuildStaticMesh(go.staticRenderIndex, *go.getMesh());
                    });
                if (r.ok)
                {
                    ctx.pushLog("Material applied: " + m_matAssetTarget.filename().string() +
                                " (" + std::to_string(r.refreshed) + " object(s) updated)");
                    m_matAssetWindowOpen = false;
                }
                else
                {
                    m_matAssetError = r.error;
                }
            }
            else if (matCancel)
            {
                m_matAssetWindowOpen = false;
            }
            ImGui::End();
        }

        if (m_matAssetDlgOpen && m_matAssetFileDialog->Display("PickMatTextureDlg"))
        {
            if (m_matAssetFileDialog->IsOk())
            {
                // Same veto as the 18 Properties dialogs: a path from
                // outside the project is imported (or rejected if the
                // extension is not an image one), never saved absolute as
                // is (finding by the final reviewer).
                if (const auto picked = acceptOrImportMatTexture(
                        ctx.project, m_matAssetFileDialog->GetFilePathName()))
                {
                    switch (m_matAssetDlgSlot)
                    {
                        case DonTopo::MaterialTextureSlot::Albedo: m_matAssetEdit.albedo = picked->string(); break;
                        case DonTopo::MaterialTextureSlot::Normal: m_matAssetEdit.normal = picked->string(); break;
                        case DonTopo::MaterialTextureSlot::Orm:    m_matAssetEdit.orm    = picked->string(); break;
                    }
                }
            }
            m_matAssetFileDialog->Close();
            m_matAssetDlgOpen = false;
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace DonTopo
