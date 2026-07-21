#include "DonTopo/Editor/GameExporter.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Scripting/ScriptComponent.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace fs = std::filesystem;

namespace {

// true si p está dentro de dir (ambos ya canonicalizados y en minúsculas).
bool keyUnderDir(const std::string& p, const std::string& dir)
{
    if (dir.empty() || p.size() <= dir.size()) return false;
    if (p.compare(0, dir.size(), dir) != 0)    return false;
    return p[dir.size()] == '/';
}

// Todos los materiales de un GameObject, sea la malla estática o skinned.
// loadSkinned nunca puebla el Material heredado de Mesh: reparte uno por
// submalla en SkinnedMesh::materials. Mismo criterio que materialsOf() en
// ContentBrowserPanel.cpp — mirar solo `material` dejaría fuera las texturas
// de cualquier personaje con rig.
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
    std::map<std::string, size_t> seen;                 // key -> índice en out
    std::map<std::string, std::string> externalTaken;   // packagePath -> key dueño
    const std::string rootKey = exportPathKey(projectRoot.string());

    auto add = [&](const std::string& raw)
    {
        if (raw.empty()) return;
        const std::string key = exportPathKey(raw);
        if (key.empty() || seen.count(key)) return;

        std::error_code ec;
        fs::path abs = fs::weakly_canonical(fs::path(raw), ec);
        if (ec) abs = fs::path(raw);

        std::string packagePath;
        if (keyUnderDir(key, rootKey))
        {
            // Dentro del proyecto: se conserva la jerarquía tal cual. Es lo
            // que hace que las texturas se reencuentren solas en el runtime:
            // ModelLoader las deriva como dirname(fbx)/filename.
            packagePath = fs::relative(abs, projectRoot, ec).generic_string();
            if (ec || packagePath.empty()) packagePath = "assets/_external/" + abs.filename().string();
        }
        else
        {
            packagePath = "assets/_external/" + abs.filename().string();
        }

        // Colisión de nombres entre dos assets externos distintos: sufijo.
        if (auto it = externalTaken.find(packagePath); it != externalTaken.end() && it->second != key)
        {
            const fs::path p = abs;
            for (int n = 1; ; ++n)
            {
                std::string candidate = "assets/_external/" + p.stem().string() + "_" +
                                        std::to_string(n) + p.extension().string();
                if (!externalTaken.count(candidate)) { packagePath = candidate; break; }
            }
        }
        externalTaken[packagePath] = key;

        ExportAsset a;
        a.sourcePath   = abs.string();
        a.packagePath  = packagePath;
        a.existsOnDisk = fs::exists(abs, ec) && !ec;
        seen[key] = out.size();
        out.push_back(std::move(a));
    };

    scene.traverse([&](GameObject* go)
    {
        if (go->hasMesh())
        {
            // sourcePath vacío = mesh procedural: su geometría ya viaja
            // dentro del .scene, no hay fichero que copiar.
            add(go->getMesh()->sourcePath);

            if (const SkinnedMesh* sm = go->getSkinnedMesh())
                for (const AnimationSource& src : sm->animationSources)
                    add(src.path);   // la fuente builtin repite sourcePath; add() deduplica

            for (const Material* m : materialsOf(go))
            {
                // Los embedded* no aportan path: viajan dentro del FBX.
                add(m->texturePath);
                add(m->normalMapPath);
                add(m->metallicRoughnessPath);
            }
        }

        if (go->hasAudioClip())
            add(go->getAudioClip()->getPath());

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

} // namespace DonTopo
