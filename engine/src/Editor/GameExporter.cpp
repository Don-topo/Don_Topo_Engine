#include "DonTopo/Editor/GameExporter.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/ImportSettings.h"
#include "DonTopo/Core/MaterialAsset.h"
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Scripting/ScriptComponent.h"
#include "DonTopo/Files/FileManager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <system_error>
#include "DonTopo/UI/ButtonComponent.h"
#include "DonTopo/UI/ProgressBarComponent.h"
#include "DonTopo/UI/TextComponent.h"

namespace fs = std::filesystem;

namespace {

// true if p is inside dir (both already canonicalized and lowercase, that
// is, coming out of exportPathKey). The module's only containment predicate:
// the editor had another (isPathWithinOrEqual) with different semantics and no
// tests, and two implementations of the same predicate diverge sooner or later.
bool keyUnderDir(const std::string& p, const std::string& dir)
{
    if (dir.empty() || p.size() <= dir.size()) return false;
    if (p.compare(0, dir.size(), dir) != 0)    return false;
    return p[dir.size()] == '/';
}

// isspace from <cctype> with a signed char (e.g. a Latin-1 accent) is
// UB; it is always passed through unsigned char first.
bool isBlankChar(char c)
{
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

// All the materials of a GameObject, whether the mesh is static or skinned.
// loadSkinned never populates the Material inherited from Mesh: it hands out one per
// submesh in SkinnedMesh::materials. Same criterion as materialsOf() in
// ContentBrowserPanel.cpp: looking only at `material` would leave out the textures
// of any rigged character.
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

} // namespace

namespace DonTopo {

bool isValidExportGameName(const std::string& name, std::string& reason)
{
    // find_first_not_of(' ') only discarded the space U+0020: a name of
    // pure tabs ("\t\t\t") got past it and blew up later when creating the
    // folder. std::all_of + isBlankChar covers any real whitespace
    // (tab, CR, LF, form feed...).
    if (name.empty() || std::all_of(name.begin(), name.end(), isBlankChar))
    {
        reason = "The name cannot be empty";
        return false;
    }
    // Covers "." and ".." as well as any name with trailing dots/spaces
    // (e.g. "...", "Game. "): Win32 drops them when creating the
    // folder, so the real destination stops being the one shown to the
    // user in the popup.
    if (name.back() == '.' || isBlankChar(name.back()))
    {
        reason = "The name cannot end in '.' or a space";
        return false;
    }
    // Same set of Windows reserved characters as
    // ContentBrowserPanel.cpp::isValidFileName (kReserved there): the comment
    // that said "same pattern" only covered ':' and the separators, so
    // "My*Game", "a?b", "x|y" or "<z>" got through here and failed later with a
    // generic "could not be created" instead of this specific reason.
    static const std::string kReserved = "\\/:*?\"<>|";
    for (char c : name)
    {
        if (kReserved.find(c) != std::string::npos)
        {
            reason = "The name cannot contain any of these characters: \\ / : * ? \" < > |";
            return false;
        }
    }
    // filename() different from the full name == it contains path
    // separators ('/' or '\') or is an absolute path; in both cases destDir / name
    // stops pointing inside the folder the user chose in the
    // dialog. Redundant with kReserved above (both separators are already
    // in the set) but left as an extra safety net over operator/.
    if (fs::path(name).filename().string() != name)
    {
        reason = "The name cannot contain path separators";
        return false;
    }
    // Device names reserved by Windows (CON, NUL, COM1..9,
    // LPT1..9): "<destination>\NUL" does not create a folder, it resolves to the
    // NUL device. exists() on that gives true, so the confirmation popup
    // would claim "the folder already exists and its contents will be deleted" about something
    // that is not a folder and has no contents. Windows' rule looks at the
    // name WITHOUT extension (everything before the first '.'), case-insensitive,
    // so "NUL.txt" is also reserved.
    static const std::array<std::string, 22> kReservedDeviceNames = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };
    std::string baseUpper = name.substr(0, name.find('.'));
    std::transform(baseUpper.begin(), baseUpper.end(), baseUpper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    for (const std::string& reserved : kReservedDeviceNames)
    {
        if (baseUpper == reserved)
        {
            reason = "'" + name + "' is a device name reserved by Windows";
            return false;
        }
    }
    return true;
}

ExportTargetState inspectExportTarget(const fs::path& pkg)
{
    // An empty pkg means destDir and gameName came in empty: there is
    // no folder to inspect and certainly none to delete.
    if (pkg.empty()) return ExportTargetState::Occupied;

    std::error_code ec;
    const fs::file_status st = fs::status(pkg, ec);
    // The type() is looked at BEFORE ec on purpose: MSVC's STL leaves ec
    // set to ERROR_FILE_NOT_FOUND (value() == 2) for the "does not exist" case,
    // even though type() itself is already not_found, contrary to what
    // cppreference suggests ("not treated as an error"). Looking at ec first
    // classified EVERY missing destination as Occupied and the Export button
    // stayed disabled for any new folder: it was detected because
    // test_inspect_export_target_states (exporter_tests.cpp) failed even
    // in the Missing case. A real error (permissions, disconnected drive, malformed
    // path) does not give not_found; for those ec does matter, and that is why the
    // check below is still there.
    if (st.type() == fs::file_type::not_found) return ExportTargetState::Missing;
    if (ec) return ExportTargetState::Occupied;
    // A file, a link or a device with that name: it is not a package of
    // ours and remove_all() would take it away anyway.
    if (!fs::is_directory(st)) return ExportTargetState::Occupied;

    fs::directory_iterator it(pkg, ec), end;
    if (ec) return ExportTargetState::Occupied;
    if (it == end) return ExportTargetState::Empty;

    // game.scene is only written by writeExportPackage, and always: its presence
    // at the root is the signature of an exported package. It is the only mark that
    // distinguishes "a folder I generated myself and can regenerate" from "a user's
    // folder". If it cannot even be queried, it is assumed occupied.
    std::error_code sceneEc;
    const bool hasSceneFile = fs::is_regular_file(pkg / "game.scene", sceneEc);
    if (sceneEc) return ExportTargetState::Occupied;
    return hasSceneFile ? ExportTargetState::PriorPackage : ExportTargetState::Occupied;
}

std::string exportPathKey(const std::string& path)
{
    if (path.empty()) return {};
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(fs::path(path), ec);
    std::string s = (ec ? fs::path(path) : canon).generic_string();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::vector<ExportAsset> collectSceneAssets(
    Scene& scene,
    const fs::path& projectRoot,
    const std::map<std::string, fs::path>& scriptPaths)
{
    std::vector<ExportAsset> out;
    std::map<std::string, size_t> seen;              // key -> index in out
    std::map<std::string, int> externalDirIndex;     // key of the source directory -> subfolder
    const std::string rootKey = exportPathKey(projectRoot.string());

    // One subfolder per external source directory, numbered in order of
    // first appearance: assets/_external/0/prop.fbx, assets/_external/1/prop.fbx.
    //
    // The previous scheme flattened everything to assets/_external/<name> with a numeric
    // suffix on collision, and that silently broke the sibling relationship
    // that ModelLoader takes for granted: it derives an FBX's texture
    // as dirname(fbx)/basename (ModelLoader.cpp:156). With two external folders
    // that both have prop.fbx + prop.png, the second pair became
    // prop_1.fbx + prop_1.png and ModelLoader looked for dirname(prop_1.fbx)/prop.png
    // (the FIRST model's texture), with no error or warning, just an incorrect
    // render. With one subfolder per directory the sibling relationship is preserved
    // by construction and the collisions disappear: two files with the same
    // name in the same directory do not exist.
    auto externalPackagePath = [&](const fs::path& abs) -> std::string
    {
        const std::string dirKey = exportPathKey(abs.parent_path().string());
        auto [it, inserted] = externalDirIndex.emplace(dirKey, (int)externalDirIndex.size());
        (void)inserted;
        return "assets/_external/" + std::to_string(it->second) + "/" + abs.filename().string();
    };

    // forcedPackagePath: the package path already decided (an associated file of
    // a model, see addModel). Empty = the usual rule.
    auto add = [&](const std::string& raw, const std::string& forcedPackagePath = {})
    {
        if (raw.empty()) return;
        const std::string key = exportPathKey(raw);
        if (key.empty() || seen.count(key)) return;

        std::error_code ec;
        fs::path abs = fs::weakly_canonical(fs::path(raw), ec);
        if (ec) abs = fs::path(raw);

        std::string packagePath = forcedPackagePath;
        if (!packagePath.empty())
        {
            // Associated file of a model: its place is fixed by the model.
        }
        else if (keyUnderDir(key, rootKey))
        {
            // Inside the project: the hierarchy is kept as is. It is what
            // makes the textures find each other again in the runtime:
            // ModelLoader derives them as dirname(fbx)/filename.
            packagePath = fs::relative(abs, projectRoot, ec).generic_string();
            if (ec || packagePath.empty()) packagePath = externalPackagePath(abs);
        }
        else
        {
            packagePath = externalPackagePath(abs);
        }

        ExportAsset a;
        a.sourcePath   = abs.string();
        a.packagePath  = packagePath;
        a.existsOnDisk = fs::exists(abs, ec) && !ec;
        seen[key] = out.size();
        out.push_back(std::move(a));
    };

    // An asset with import settings (material texture, audio clip,
    // model) carries its sidecar: the runtime looks for it next to the asset. It is one more ExportAsset:
    // it shares the source folder, so the assets/_external/N numbering and
    // the hierarchy inside the project come out the same as the asset's.
    // The sidecar goes NEXT TO the asset's real place in the package (which may be
    // forced, see addModel), not where the general rule would put it.
    auto addWithSidecar = [&](const std::string& raw, const std::string& forcedPackagePath = {})
    {
        if (raw.empty()) return;
        add(raw, forcedPackagePath);
        const fs::path sidecar = importSidecarPath(fs::path(raw));
        std::error_code sec;
        if (!fs::exists(sidecar, sec) || sec) return;
        const auto it = seen.find(exportPathKey(raw));
        if (it == seen.end()) { add(sidecar.string()); return; }
        add(sidecar.string(), importSidecarPath(fs::path(out[it->second].packagePath)).generic_string());
    };

    // A model carries its sidecar and the files it reads besides itself (.mtl
    // and its textures, .bin and images of a .gltf), placed RELATIVE to the
    // model's folder in the package: the runtime looks for them at the same relative
    // path, and outside the project another assets/_external/N would break that
    // relationship. All the models are walked in a first pass, before
    // any material, so that the dedup keeps this placement.
    auto addModel = [&](const std::string& raw)
    {
        if (raw.empty() || seen.count(exportPathKey(raw))) return;   // already there, with its associated files
        addWithSidecar(raw);
        const auto it = seen.find(exportPathKey(raw));
        if (it == seen.end()) return;
        const fs::path modelPkgDir = fs::path(out[it->second].packagePath).parent_path();
        const fs::path modelDir    = fs::path(raw).parent_path();
        for (const std::string& rel : ModelLoader::modelCompanionFiles(raw))
            addWithSidecar((modelDir / fs::path(rel)).string(), (modelPkgDir / fs::path(rel)).generic_string());
    };

    // Material texture: if it hangs from its model's folder, the runtime
    // derives it from there (game.scene does not store the base texture), so it is placed
    // relative to the model in the package, like an associated file.
    auto addMaterialTexture = [&](const std::string& modelPath, const std::string& tex)
    {
        if (tex.empty()) return;
        const auto model = seen.find(exportPathKey(modelPath));
        if (!modelPath.empty() && model != seen.end())
        {
            const fs::path rel = fs::path(tex).lexically_relative(fs::path(modelPath).parent_path());
            const bool inside = !rel.empty() && !rel.has_root_path() &&
                                std::none_of(rel.begin(), rel.end(), [](const fs::path& p) { return p == ".."; });
            if (inside)
            {
                const fs::path modelPkgDir = fs::path(out[model->second].packagePath).parent_path();
                addWithSidecar(tex, (modelPkgDir / rel).generic_string());
                return;
            }
        }
        addWithSidecar(tex);
    };

    // First pass: the models and what they read.
    scene.traverse([&](GameObject* go)
    {
        if (!go->hasMesh()) return;
        addModel(go->getMesh()->sourcePath);
        if (const SkinnedMesh* sm = go->getSkinnedMesh())
            for (const AnimationSource& src : sm->animationSources)
                addModel(src.path);             // the builtin repeats sourcePath; addModel deduplicates
    });

    scene.traverse([&](GameObject* go)
    {
        if (go->hasMesh())
        {
            // empty sourcePath = procedural mesh: its geometry already travels
            // inside the .scene, there is no file to copy. The models already
            // went in during the first pass, with their sidecar and their associated files.
            const std::string& modelPath = go->getMesh()->sourcePath;
            for (const Material* m : materialsOf(go))
            {
                // The embedded* contribute no path: they travel inside the FBX.
                addMaterialTexture(modelPath, m->texturePath);
                addMaterialTexture(modelPath, m->normalMapPath);
                addMaterialTexture(modelPath, m->metallicRoughnessPath);
            }
            for (const MaterialOverride& ov : go->materialOverrides)
            {
                if (ov.matAsset.empty()) continue;
                add(ov.matAsset);   // the .mat carries no sidecar of its own
                // Its own textures too: the in-memory material only carries the
                // ones that won (a per-slot override hides the .mat's), and the
                // packaged .mat is repointed at them (writeExportPackage).
                const MaterialAsset mat = loadMaterialAsset(ov.matAsset);
                addMaterialTexture(modelPath, mat.albedo);
                addMaterialTexture(modelPath, mat.normal);
                addMaterialTexture(modelPath, mat.orm);
            }
        }

        if (go->hasAudioClip())
            addWithSidecar(go->getAudioClip()->getPath());

        if (go->hasButton())
        {
            const auto& b = go->getButton();
            // The UI paths are the only ones that can come RELATIVE (they are
            // typed by hand in the panel, and the default one always is):
            // resolving them against the project root is what keeps them inside
            // the package instead of in assets/_external, or outright
            // marked as nonexistent depending on where the editor is launched from.
            auto addUiPath = [&](const std::string& raw)
            {
                if (raw.empty()) return;
                const fs::path p(raw);
                add(p.is_absolute() ? raw : (projectRoot / p).string());
            };
            addUiPath(b->atlasPath);
            // A button without its own font draws its text with the default one,
            // so THAT one is a game asset just like any other: without
            // this the package comes out without it and the text does not appear anywhere
            // except on the machine that exported.
            if (!b->fontPath.empty())  addUiPath(b->fontPath);
            else if (!b->text.empty()) addUiPath(kDefaultUiFontPath);
        }

        if (go->hasProgressBar())
        {
            const auto& p = go->getProgressBar();
            // Same relative path resolution as the Button, and ALL THREE: the
            // background and the fill can carry their own image and only fall back to the
            // atlas if they do not. No fallback to any default asset: a
            // bar without images is drawn with flat color quads.
            auto addUiPath = [&](const std::string& raw)
            {
                if (raw.empty()) return;
                const fs::path candidata(raw);
                add(candidata.is_absolute() ? raw : (projectRoot / candidata).string());
            };
            addUiPath(p->atlasPath);
            addUiPath(p->backgroundPath);
            addUiPath(p->fillPath);
        }

        if (go->hasText())
        {
            const auto& t = go->getText();
            // Same relative path resolution and same fallback as the
            // Button: a Text without its own font draws with the default one, and
            // without packaging it the text does not appear outside the machine that
            // exported.
            auto addUiPath = [&](const std::string& raw)
            {
                if (raw.empty()) return;
                const fs::path p(raw);
                add(p.is_absolute() ? raw : (projectRoot / p).string());
            };
            if (!t->fontPath.empty())  addUiPath(t->fontPath);
            else if (!t->text.empty()) addUiPath(kDefaultUiFontPath);
        }

        for (const auto& s : go->getScripts())
        {
            auto it = scriptPaths.find(s->scriptName);
            if (it != scriptPaths.end())
                add(it->second.string());
        }
    });

    std::sort(out.begin(), out.end(),
              [](const ExportAsset& a, const ExportAsset& b) { return a.packagePath < b.packagePath; });
    return out;
}

namespace {

// Rewrites a path field if the map knows it. Returns 1 if it touched anything.
// storedBase: the folder a relative stored value is relative to (the Scene's
// assetRoot for what toStoredPath wrote). Empty = look the value up as is.
int rewriteField(nlohmann::json& holder, const char* field,
                 const std::map<std::string, std::string>& sourceToPackage,
                 const std::string& storedBase = {})
{
    if (!holder.contains(field) || !holder[field].is_string()) return 0;
    const std::string current = holder[field].get<std::string>();
    if (current.empty()) return 0;
    std::string lookup = current;
    if (!storedBase.empty() && !fs::path(current).is_absolute())
        lookup = (fs::path(storedBase) / fs::path(current)).string();
    auto it = sourceToPackage.find(DonTopo::exportPathKey(lookup));
    if (it == sourceToPackage.end()) return 0;
    holder[field] = it->second;
    return 1;
}

int rewriteNode(nlohmann::json& node, const std::map<std::string, std::string>& sourceToPackage,
                const std::string& assetRoot)
{
    int n = 0;
    if (node.contains("mesh") && node["mesh"].is_object())
    {
        nlohmann::json& mesh = node["mesh"];
        n += rewriteField(mesh, "sourcePath", sourceToPackage);
        if (mesh.contains("animationSources") && mesh["animationSources"].is_array())
            for (nlohmann::json& src : mesh["animationSources"])
                n += rewriteField(src, "path", sourceToPackage);
        // Texture overrides set by hand (Scene::nodeToJson, "materials"
        // block). Without this the file IS packaged (collectSceneAssets
        // already knows these paths) but the one left written in the package's game.scene
        // is still the editor's disk one: an override that points
        // outside the project root travels absolute and the exported game
        // looks for the texture on the machine that generated it. baseAlbedo/baseNormal/
        // baseOrm are not touched: nodeToJson only writes them with
        // carryOverrideBaseline=true (cloning, in-memory undo/redo), and
        // exportGame calls scene.toJson() with the default false.
        // These four are the only fields toStoredPath writes relative to the
        // scene's assetRoot (the project), which is not the export root.
        if (mesh.contains("materials") && mesh["materials"].is_array())
            for (nlohmann::json& mat : mesh["materials"])
            {
                n += rewriteField(mat, "albedo", sourceToPackage, assetRoot);
                n += rewriteField(mat, "normal", sourceToPackage, assetRoot);
                n += rewriteField(mat, "orm", sourceToPackage, assetRoot);
                n += rewriteField(mat, "matAsset", sourceToPackage, assetRoot);
            }
    }
    if (node.contains("audioClip") && node["audioClip"].is_object())
        n += rewriteField(node["audioClip"], "path", sourceToPackage);

    if (node.contains("button") && node["button"].is_object())
    {
        nlohmann::json& button = node["button"];
        n += rewriteField(button, "atlasPath", sourceToPackage);
        // An empty fontPath stays empty (rewriteField does not touch an empty
        // string): the runtime will fall back to kDefaultUiFontPath again, which inside
        // the package has the SAME relative path as in the project.
        n += rewriteField(button, "fontPath", sourceToPackage);
    }

    if (node.contains("text") && node["text"].is_object())
    {
        // Same rule as the Button's: an empty fontPath stays empty and the
        // runtime falls back to kDefaultUiFontPath again.
        n += rewriteField(node["text"], "fontPath", sourceToPackage);
    }

    if (node.contains("progressBar") && node["progressBar"].is_object())
    {
        // The three image paths. An empty one stays empty (rewriteField does not
        // touch an empty string): the runtime falls back to the atlas or to the
        // color quad again, just like in the editor.
        nlohmann::json& bar = node["progressBar"];
        n += rewriteField(bar, "atlasPath", sourceToPackage);
        n += rewriteField(bar, "backgroundPath", sourceToPackage);
        n += rewriteField(bar, "fillPath", sourceToPackage);
    }

    if (node.contains("children") && node["children"].is_array())
        for (nlohmann::json& child : node["children"])
            n += rewriteNode(child, sourceToPackage, assetRoot);
    return n;
}

} // namespace

int rewriteScenePaths(nlohmann::json& sceneJson,
                      const std::map<std::string, std::string>& sourceToPackage,
                      const std::string& assetRoot)
{
    // Accepts both the complete Scene::toJson() document ({version, root})
    // and a loose node, so that the tests can assemble the JSON by hand.
    if (sceneJson.contains("root") && sceneJson["root"].is_object())
        return rewriteNode(sceneJson["root"], sourceToPackage, assetRoot);
    return rewriteNode(sceneJson, sourceToPackage, assetRoot);
}

ExportPlatform exportPlatformFor(platform::Os os)
{
    if (os == platform::Os::Windows)
        return { ".exe", false, { "fmod.dll" }, true, true, false };
    return { "", true, { "libfmod.so." }, false, false, true };
}

bool isAudioLibFile(const std::string& n, const ExportPlatform& plat)
{
    for (const std::string& p : plat.audioLibPrefixes)
    {
        if (!p.empty() && p.back() == '.')
        {
            if (n.size() > p.size() && n.rfind(p, 0) == 0 &&
                std::all_of(n.begin() + (std::ptrdiff_t)p.size(), n.end(),
                            [](unsigned char c) { return std::isdigit(c) != 0; }))
                return true;
        }
        else if (n == p)
            return true;
    }
    return false;
}

ExportResult writeExportPackage(const std::vector<ExportAsset>& assets,
                                const nlohmann::json& rewrittenScene,
                                const fs::path& destDir,
                                const std::string& gameName,
                                const fs::path& projectRoot,
                                const fs::path& scriptsDir,
                                const fs::path& runtimeExe,
                                RenderBackend backend,
                                const std::string& skyboxFolder,
                                const ExportPlatform& plat)
{
    ExportResult r;
    std::error_code ec;

    if (!fs::exists(runtimeExe, ec) || ec)
    {
        r.messages.push_back("Export cancelled: cannot find " + runtimeExe.string() +
                             ". Build the DonTopoRuntime target.");
        return r;
    }

    const fs::path pkg = destDir / gameName;

    // The destination's state is queried HERE, not only in the UI: this function
    // is the one that deletes, so it is the one that has to answer for the deletion.
    // Before, it trusted that the editor had validated, and the list of forbidden
    // places that the editor enumerated left out <repo>/assets: remove_all
    // took the source assets tree away and the export reported success, because the
    // ones it copies are read from the executable's directory.
    const ExportTargetState targetState = inspectExportTarget(pkg);
    if (targetState == ExportTargetState::Occupied)
    {
        r.messages.push_back("Export cancelled: '" + pkg.string() +
                             "' already exists and has content that is not from a previous export "
                             "(there is no game.scene inside). Nothing was deleted: "
                             "choose another name or destination folder.");
        return r;
    }

    // Delete + recreate: if it were copied over, the package would carry orphan
    // assets from a previous export and would stop fulfilling "only the
    // referenced ones". This point is only reached with the destination Missing, Empty or
    // PriorPackage, so the only thing that can disappear is a package that
    // this same code generated. The confirmation to the user, in the
    // PriorPackage case, is asked by the UI before calling here.
    //
    // Two separate error_codes on purpose: create_directories() on a
    // folder that already exists is NOT an error, so if it shared the ec of the
    // earlier remove_all(), a half-failed deletion (e.g. a
    // MyGame.exe from the previous export still running and locked)
    // would be masked as soon as create_directories "succeeded" on
    // that same half-deleted folder. The function would go on believing it
    // has a clean package and orphan files would survive.
    std::error_code removeEc;
    fs::remove_all(pkg, removeEc);
    if (removeEc)
    {
        r.messages.push_back("Export failed: could not clean the previous package at " +
                             pkg.string() + " (" + removeEc.message() +
                             "). Is some process using files in that folder?");
        return r;
    }

    std::error_code createEc;
    fs::create_directories(pkg, createEc);
    if (createEc)
    {
        r.messages.push_back("Export failed: could not create " + pkg.string());
        return r;
    }

    auto copyOne = [&](const fs::path& from, const fs::path& to) -> bool
    {
        std::error_code cec;
        fs::create_directories(to.parent_path(), cec);
        if (!fs::copy_file(from, to, fs::copy_options::overwrite_existing, cec))
        {
            r.messages.push_back("Could not copy " + from.string());
            return false;
        }
        std::error_code sec;
        std::uintmax_t size = fs::file_size(to, sec);
        if (!sec) r.totalBytes += size;
        ++r.fileCount;
        return true;
    };

    const fs::path exeDst = pkg / (gameName + plat.executableSuffix);
    bool ok = copyOne(runtimeExe, exeDst);
    // Linux: without the execute bit the game does not start on double click or from
    // the terminal, even if the file is the right binary.
    if (ok && plat.setExecutableBit)
    {
        std::error_code pec;
        fs::permissions(exeDst, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add, pec);
        if (pec)
        {
            r.messages.push_back("Could not mark " + exeDst.string() + " as executable");
            ok = false;
        }
    }

    for (const ExportAsset& a : assets)
        ok = copyOne(fs::path(a.sourcePath), pkg / fs::path(a.packagePath)) && ok;

    // A .mat names its textures relative to its own folder (or absolute when
    // that is impossible). Copied verbatim, one outside the project still
    // pointed at the exporting machine, since the texture itself went to
    // assets/_external/N. Rewrite each packaged .mat against the packaged
    // copies; this runs after the loop so the textures already exist and
    // saveMaterialAsset can relativize against them.
    std::map<std::string, std::string> sourceToPackage;
    for (const ExportAsset& a : assets)
        sourceToPackage[exportPathKey(a.sourcePath)] = a.packagePath;
    for (const ExportAsset& a : assets)
    {
        if (fs::path(a.packagePath).extension() != ".mat") continue;
        MaterialAsset mat = loadMaterialAsset(fs::path(a.sourcePath));
        for (std::string* tex : { &mat.albedo, &mat.normal, &mat.orm })
        {
            if (tex->empty()) continue;
            const auto it = sourceToPackage.find(exportPathKey(*tex));
            if (it != sourceToPackage.end()) *tex = (pkg / fs::path(it->second)).string();
        }
        std::string err;
        if (!saveMaterialAsset(pkg / fs::path(a.packagePath), mat, &err))
        {
            r.messages.push_back("Could not rewrite the texture paths of " + a.packagePath + ": " + err);
            ok = false;
        }
    }

    // Skybox: the SOURCE is the folder the project has chosen, but the
    // DESTINATION is always assets/skybox, because it is where the runtime looks for it.
    // This way choosing another sky in the editor does not force touching the runtime.
    //
    // It always goes even if the scene does not "reference" it.
    //
    // The copied faces are counted and missing one is an ERROR, not a silent
    // skip: Skybox.cpp:84 throws std::runtime_error("Skybox: failed to
    // load face") and the game dies on startup. Before, the if(exists) swallowed
    // the case, the Log said "Export completed" and the user found out
    // when they handed the .exe to someone else.
    std::vector<std::string> missingFaces;
    for (const char* face : { "px", "nx", "py", "ny", "pz", "nz" })
    {
        const fs::path from =
            projectRoot / fs::path(skyboxFolder.empty() ? "assets/skybox" : skyboxFolder) /
            (std::string(face) + ".png");
        std::error_code fec;
        if (fs::exists(from, fec) && !fec)
            ok = copyOne(from, pkg / "assets" / "skybox" / from.filename()) && ok;
        else
            missingFaces.push_back(from.string());
    }
    if (!missingFaces.empty())
    {
        std::string list;
        for (const std::string& f : missingFaces) list += (list.empty() ? "" : ", ") + f;
        r.messages.push_back("Export incomplete: missing " + std::to_string(missingFaces.size()) +
                             " of the 6 skybox faces (" + list +
                             "); the exported game would abort when loading it.");
        ok = false;
    }

    // Splash logo: the runtime looks for it as splash.png next to the .exe. It always goes
    // (the scene does not reference it), but missing is a WARNING, not an error: without
    // it, the runtime starts directly without a splash (SplashScreen::init returns
    // false and the runtime respects it). Same criterion as fmod.dll.
    {
        const fs::path logo = projectRoot / "assets" / "MainEngineLogo.png";
        std::error_code lec;
        if (fs::exists(logo, lec) && !lec)
            ok = copyOne(logo, pkg / "splash.png") && ok;
        else
            r.messages.push_back("Warning: could not find " + logo.string() +
                                 "; the exported game will start without a splash screen.");
    }

    // Action map of the Input Actions panel: Input reads it as
    // "input_actions.json" relative to the CWD, so it goes next to the .exe. It is taken
    // from the editor's CWD (it is where the panel writes it, next to imgui.ini) and
    // not from projectRoot. Missing is a WARNING: without it the actions are empty and
    // Input.IsActionDown returns false, but the game starts anyway.
    {
        const fs::path actions = fs::current_path() / "input_actions.json";
        std::error_code aec;
        if (fs::exists(actions, aec) && !aec)
            ok = copyOne(actions, pkg / "input_actions.json") && ok;
        else
            r.messages.push_back("Warning: could not find " + actions.string() +
                                 "; the exported game will have no input actions defined.");
    }

    // shaders/*.spv to the package root: Renderer::createPipeline opens them
    // as "shaders/<name>.spv" relative to the CWD.
    //
    // Zero shaders copied is also an error: without any .spv the runtime dies
    // in createPipeline. The iterator's error_code used to be dropped on the floor, and
    // an inaccessible or empty shaders/ folder produced a package that does not
    // start with a Log that said "completed".
    {
        int spvCopied = 0;
        std::error_code dec;
        for (fs::directory_iterator it(projectRoot / "shaders", dec), end; !dec && it != end; it.increment(dec))
        {
            if (it->path().extension() != ".spv") continue;
            if (copyOne(it->path(), pkg / "shaders" / it->path().filename())) ++spvCopied;
            else                                                             ok = false;
        }
        if (spvCopied == 0)
        {
            r.messages.push_back("Export incomplete: no .spv shader was copied from " +
                                 (projectRoot / "shaders").string() +
                                 (dec ? " (" + dec.message() + ")" : " (folder empty or without .spv)") +
                                 "; the exported game would die when creating the pipeline.");
            ok = false;
        }
    }

    // shaders/*.dxil next to the .spv files. They are ALWAYS copied, not only when the
    // chosen backend is DirectX 12: there are 35 small files and this way the package
    // still starts if someone changes renderBackend in game.cfg by hand.
    //
    // Their absence is NOT an error, unlike with the .spv files: a build made with
    // DTE_ENABLE_D3D12=OFF does not generate them, and that package is perfectly valid
    // as long as it stays on Vulkan.
    {
        int dxilCopied = 0;
        std::error_code dec;
        for (fs::directory_iterator it(projectRoot / "shaders", dec), end; !dec && it != end; it.increment(dec))
        {
            if (it->path().extension() != ".dxil") continue;
            if (copyOne(it->path(), pkg / "shaders" / it->path().filename())) ++dxilCopied;
            else                                                             ok = false;
        }
        if (dxilCopied == 0 && backend == RenderBackend::D3D12)
        {
            r.messages.push_back("Warning: DirectX 12 was chosen but there is no .dxil shader in " +
                                 (projectRoot / "shaders").string() +
                                 "; the exported game will not be able to use that backend.");
        }
    }

    // game.cfg: STARTUP configuration of the game, separate from game.scene on
    // purpose. Putting the backend in the scene would force touching the scene format
    // and its versioning for a datum that does not describe the scene.
    {
        nlohmann::json cfg;
        cfg["renderBackend"] = renderBackendName(backend);

        const fs::path cfgPath = pkg / "game.cfg";
        std::ofstream  out(cfgPath, std::ios::binary | std::ios::trunc);
        if (out.is_open())
        {
            out << cfg.dump(4);
            out.flush();
            if (out.good())
            {
                ++r.fileCount;
                // And its bytes: the Log summary says "N files, K KB", and
                // counting the file without counting its size throws off the total.
                out.close();
                std::error_code sizeEc;
                const auto      cfgSize = fs::file_size(cfgPath, sizeEc);
                if (!sizeEc)
                    r.totalBytes += cfgSize;
            }
            else
            {
                r.messages.push_back("Could not write " + cfgPath.string() +
                                     "; the game will start with Vulkan.");
                ok = false;
            }
        }
        else
        {
            r.messages.push_back("Could not create " + cfgPath.string() +
                                 "; the game will start with Vulkan.");
            ok = false;
        }
    }

    // The whole Scripts/: the .lua files are referenced by name and may
    // require each other, so filtering by references would break them.
    if (fs::exists(scriptsDir, ec))
    {
        // Package key so as not to walk the output itself: if pkg falls
        // inside scriptsDir (Missing destination under Scripts/, which the
        // inspectExportTarget criterion allows because it deletes nothing), this recursive
        // walk would keep creating files inside the tree it is
        // walking and might never end. Excluding them here solves the
        // problem where it is (the copy must not consume its own output) instead
        // of forbidding destination folders from the outside.
        const std::string pkgKey = exportPathKey(pkg.string());
        std::error_code rec;
        for (fs::recursive_directory_iterator it(scriptsDir, rec), end; !rec && it != end; it.increment(rec))
        {
            const std::string entryKey = exportPathKey(it->path().string());
            if (entryKey == pkgKey || keyUnderDir(entryKey, pkgKey))
            {
                if (it->is_directory()) it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file()) continue;
            std::error_code relEc;
            fs::path rel = fs::relative(it->path(), scriptsDir, relEc);
            if (relEc) continue;
            ok = copyOne(it->path(), pkg / "Scripts" / rel) && ok;
        }
    }

    // fmod.dll is only a real dependency if the engine was built with FMOD
    // (DT_FMOD_ENABLED, engine/CMakeLists.txt:71-73); without it the runtime does not
    // even link it and its absence means nothing. With it, a missing
    // DLL makes the .exe die with STATUS_ENTRYPOINT_NOT_FOUND before
    // main. Even so it is a warning and not an error, unlike the skybox and the
    // shaders: the rest of the package is correct and it is fixed by copying one
    // file next to the .exe, without re-exporting.
#ifdef DT_FMOD_ENABLED
    {
        // fmod.dll on Windows; libfmod.so.N (the name of its soname, which is
        // the one the binary asks for) on Linux. The table says which.
        int audioCopied = 0;
        std::error_code aec;
        for (fs::directory_iterator it(projectRoot, aec), end; !aec && it != end; it.increment(aec))
        {
            if (!it->is_regular_file()) continue;
            if (!isAudioLibFile(it->path().filename().string(), plat)) continue;
            if (copyOne(it->path(), pkg / it->path().filename())) ++audioCopied;
            else                                                  ok = false;
        }
        if (audioCopied == 0)
        {
            std::string lib = plat.audioLibPrefixes[0];
            if (!lib.empty() && lib.back() == '.') lib += "N";
            r.messages.push_back("Warning: could not find " + (projectRoot / lib).string() +
                                 "; the engine was built with FMOD, so the exported game will not "
                                 "start until you copy that library next to the executable.");
        }
    }
#endif

    // The MSVC CRT (VCRUNTIME140.dll, MSVCP140.dll and companions) is not on
    // a machine without Visual Studio or the VC++ Redistributable installed: there
    // the .exe dies before main with "VCRUNTIME140.dll is missing". Same failure
    // that is invisible locally as the debug CRT below, and for the same
    // reason: the dev-box always meets the precondition that the
    // player lacks.
    //
    // The DLLs travel INSIDE the package (app-local) because the package is a
    // folder that is simply copied: without an installer there is nobody to register the
    // redist on the destination machine. Whoever leaves them next to the editor is the
    // POST_BUILD of sandbox/CMakeLists.txt (via
    // InstallRequiredSystemLibraries), so here they only need to be picked up from
    // next to it, just like fmod.dll.
    //
    // They are filtered by prefix and not by a literal list of names: the DLL set
    // depends on the toolchain version (VCRUNTIME140_1.dll did not exist
    // in VS2015, MSVCP140_atomic_wait.dll arrived with VS2019) and a fixed list would
    // silently fall short with the next MSVC update.
    //
    // Warning and not error, same criterion as fmod.dll: the rest of the package is
    // correct and it is fixed by copying files next to the .exe, without re-exporting.
#ifdef NDEBUG
    if (plat.copyMsvcCrt)
    {
        int crtCopied = 0;
        std::error_code cdec;
        for (fs::directory_iterator it(projectRoot, cdec), end; !cdec && it != end; it.increment(cdec))
        {
            if (!it->is_regular_file()) continue;
            std::string name = it->path().filename().string();
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (name.size() <= 4 || name.compare(name.size() - 4, 4, ".dll") != 0) continue;
            if (name.rfind("msvcp140",    0) != 0 &&
                name.rfind("vcruntime140", 0) != 0 &&
                name.rfind("concrt140",    0) != 0) continue;
            if (copyOne(it->path(), pkg / it->path().filename())) ++crtCopied;
            else                                                  ok = false;
        }
        if (crtCopied == 0)
            r.messages.push_back("Warning: no MSVC CRT DLL was found "
                                 "(VCRUNTIME140.dll, MSVCP140.dll...) next to the editor in " +
                                 projectRoot.string() +
                                 "; the exported game will only start on machines that already "
                                 "have the Visual C++ Redistributable installed. Re-run "
                                 "configure-release.bat so that the build leaves them "
                                 "there.");
    }
#endif

    // A Debug MSVC binary links the debug CRT (ucrtbased.dll,
    // MSVCP140D.dll, VCRUNTIME140D.dll and companions). Microsoft does NOT allow
    // redistributing those DLLs and they are only installed where Visual Studio is:
    // the package starts on the machine that exported it and dies with "ucrtbased.dll
    // is missing" on anyone else's. It is precisely the failure that no local
    // test can see, because the developer's machine always meets the precondition
    // that the player lacks.
    //
    // It is enough to look at how THIS binary was compiled: the runtime copied to the
    // package is always of the same configuration as the exporting editor
    // (runtime/CMakeLists.txt's POST_BUILD leaves it next to Sandbox.exe).
    //
    // Warning and not error, same criterion as fmod.dll: exporting in Debug to
    // test locally is legitimate and the rest of the package is correct.
#ifndef NDEBUG
    if (plat.warnDebugCrt)
    r.messages.push_back("Warning: exported in the Debug configuration. This package only starts on "
                         "machines with Visual Studio installed, because it links MSVC's debug "
                         "CRT (ucrtbased.dll and friends), which is not "
                         "redistributable. To ship the game: configure-release.bat, "
                         "build-release.bat, and export from build-ninja-release.");
#endif

    // glibc cannot be packaged: the game needs the one from this machine or a
    // newer one. libstdc++ does go inside the runtime (runtime/CMakeLists.txt).
    if (plat.warnGlibc && !platform::libcVersion().empty())
        r.messages.push_back("Warning: the game needs glibc " + platform::libcVersion() +
                             " or newer; it will not start on distros older than this one.");

    if (!FileManager::writeJson((pkg / "game.scene").string(), rewrittenScene))
    {
        r.messages.push_back("Could not write game.scene");
        ok = false;
    }
    else
    {
        ++r.fileCount;
        // Same error-tolerant pattern as copyOne: game.scene also
        // weighs something and the summary ("N files, M KB") was leaving it out.
        std::error_code sec;
        std::uintmax_t size = fs::file_size(pkg / "game.scene", sec);
        if (!sec) r.totalBytes += size;
    }

    r.ok = ok;
    if (ok)
        r.messages.push_back("Export completed in " + pkg.string() + ": " +
                             std::to_string(r.fileCount) + " files, " +
                             std::to_string(r.totalBytes / 1024) + " KB");
    return r;
}

ExportResult exportGame(Scene& scene,
                        const std::map<std::string, fs::path>& scriptPaths,
                        const fs::path& destDir,
                        const std::string& gameName,
                        const fs::path& projectRoot,
                        const fs::path& scriptsDir,
                        const fs::path& runtimeExe,
                        RenderBackend backend,
                        const std::string& skyboxFolder)
{
    ExportResult r;

    // runExport (the original name of this in EditorUI) is the last function
    // before the destructive remove_all() inside writeExportPackage, and must
    // not trust that the UI already validated: the guardian of an
    // irreversible deletion cannot depend on someone else's flag. It is revalidated here.
    std::string nameError;
    if (!isValidExportGameName(gameName, nameError))
    {
        r.messages.push_back("Export cancelled: invalid name (" + nameError + ")");
        return r;
    }

    // Without a camera the game could not render: it fails here, where the
    // user can fix it, and not in an .exe that opens a black window.
    if (!scene.findCamera())
    {
        r.messages.push_back("Export cancelled: the scene has no camera (Add > Camera in Properties)");
        return r;
    }

    std::error_code ec;
    if (!fs::exists(runtimeExe, ec))
    {
        r.messages.push_back("Export cancelled: missing " + runtimeExe.string() +
                             ". Build the DonTopoRuntime target.");
        return r;
    }

    std::vector<ExportAsset> assets = collectSceneAssets(scene, projectRoot, scriptPaths);

    std::vector<std::string> missing;
    for (const ExportAsset& a : assets)
        if (!a.existsOnDisk) missing.push_back(a.sourcePath);
    if (!missing.empty())
    {
        r.messages.push_back("Export cancelled: missing on disk " +
                             std::to_string(missing.size()) + " referenced assets:");
        for (const std::string& m : missing)
            r.messages.push_back("  " + m);
        return r;
    }

    std::map<std::string, std::string> sourceToPackage;
    for (const ExportAsset& a : assets)
        sourceToPackage[exportPathKey(a.sourcePath)] = a.packagePath;

    nlohmann::json sceneJson = scene.toJson();
    rewriteScenePaths(sceneJson, sourceToPackage, scene.assetRoot());

    return writeExportPackage(assets, sceneJson, destDir, gameName, projectRoot, scriptsDir,
                              runtimeExe, backend, skyboxFolder);
}

} // namespace DonTopo
