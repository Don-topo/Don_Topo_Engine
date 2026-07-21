#include "DonTopo/Editor/GameExporter.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Scripting/ScriptComponent.h"
#include "DonTopo/Files/FileManager.h"

#include <nlohmann/json.hpp>

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

namespace {

// Reescribe un campo de path si el mapa lo conoce. Devuelve 1 si tocó algo.
int rewriteField(nlohmann::json& holder, const char* field,
                 const std::map<std::string, std::string>& sourceToPackage)
{
    if (!holder.contains(field) || !holder[field].is_string()) return 0;
    const std::string current = holder[field].get<std::string>();
    if (current.empty()) return 0;
    auto it = sourceToPackage.find(DonTopo::exportPathKey(current));
    if (it == sourceToPackage.end()) return 0;
    holder[field] = it->second;
    return 1;
}

int rewriteNode(nlohmann::json& node, const std::map<std::string, std::string>& sourceToPackage)
{
    int n = 0;
    if (node.contains("mesh") && node["mesh"].is_object())
    {
        nlohmann::json& mesh = node["mesh"];
        n += rewriteField(mesh, "sourcePath", sourceToPackage);
        if (mesh.contains("animationSources") && mesh["animationSources"].is_array())
            for (nlohmann::json& src : mesh["animationSources"])
                n += rewriteField(src, "path", sourceToPackage);
    }
    if (node.contains("audioClip") && node["audioClip"].is_object())
        n += rewriteField(node["audioClip"], "path", sourceToPackage);

    if (node.contains("children") && node["children"].is_array())
        for (nlohmann::json& child : node["children"])
            n += rewriteNode(child, sourceToPackage);
    return n;
}

} // namespace

int rewriteScenePaths(nlohmann::json& sceneJson,
                      const std::map<std::string, std::string>& sourceToPackage)
{
    // Acepta tanto el documento completo de Scene::toJson() ({version, root})
    // como un nodo suelto, para que los tests puedan armar el JSON a mano.
    if (sceneJson.contains("root") && sceneJson["root"].is_object())
        return rewriteNode(sceneJson["root"], sourceToPackage);
    return rewriteNode(sceneJson, sourceToPackage);
}

ExportResult writeExportPackage(const std::vector<ExportAsset>& assets,
                                const nlohmann::json& rewrittenScene,
                                const fs::path& destDir,
                                const std::string& gameName,
                                const fs::path& projectRoot,
                                const fs::path& scriptsDir,
                                const fs::path& runtimeExe)
{
    ExportResult r;
    std::error_code ec;

    if (!fs::exists(runtimeExe, ec) || ec)
    {
        r.messages.push_back("Export cancelado: no se encuentra " + runtimeExe.string() +
                             ". Compila el target DonTopoRuntime.");
        return r;
    }

    const fs::path pkg = destDir / gameName;

    // Borrado + recreado: si se copiara encima, el paquete arrastraría assets
    // huérfanos de un export anterior y dejaría de cumplir "solo los
    // referenciados". La confirmación al usuario la pide la UI antes de
    // llamar aquí.
    fs::remove_all(pkg, ec);
    fs::create_directories(pkg, ec);
    if (ec)
    {
        r.messages.push_back("Export fallido: no se pudo crear " + pkg.string());
        return r;
    }

    auto copyOne = [&](const fs::path& from, const fs::path& to) -> bool
    {
        std::error_code cec;
        fs::create_directories(to.parent_path(), cec);
        if (!fs::copy_file(from, to, fs::copy_options::overwrite_existing, cec))
        {
            r.messages.push_back("No se pudo copiar " + from.string());
            return false;
        }
        std::error_code sec;
        std::uintmax_t size = fs::file_size(to, sec);
        if (!sec) r.totalBytes += size;
        ++r.fileCount;
        return true;
    };

    bool ok = copyOne(runtimeExe, pkg / (gameName + ".exe"));

    for (const ExportAsset& a : assets)
        ok = copyOne(fs::path(a.sourcePath), pkg / fs::path(a.packagePath)) && ok;

    // Skybox: el runtime lo tiene hardcoded (initSkybox con assets/skybox/*),
    // así que va siempre aunque la escena no lo "referencie".
    for (const char* face : { "px", "nx", "py", "ny", "pz", "nz" })
    {
        const fs::path from = projectRoot / "assets" / "skybox" / (std::string(face) + ".png");
        if (fs::exists(from, ec))
            ok = copyOne(from, pkg / "assets" / "skybox" / from.filename()) && ok;
    }

    // shaders/*.spv a la raíz del paquete: Renderer::createPipeline los abre
    // como "shaders/<nombre>.spv" relativo al CWD.
    {
        std::error_code dec;
        for (fs::directory_iterator it(projectRoot / "shaders", dec), end; !dec && it != end; it.increment(dec))
        {
            if (it->path().extension() != ".spv") continue;
            ok = copyOne(it->path(), pkg / "shaders" / it->path().filename()) && ok;
        }
    }

    // Scripts/ entera: los .lua se referencian por nombre y pueden hacer
    // require entre ellos, así que filtrar por referencias los rompería.
    if (fs::exists(scriptsDir, ec))
    {
        std::error_code rec;
        for (fs::recursive_directory_iterator it(scriptsDir, rec), end; !rec && it != end; it.increment(rec))
        {
            if (!it->is_regular_file()) continue;
            std::error_code relEc;
            fs::path rel = fs::relative(it->path(), scriptsDir, relEc);
            if (relEc) continue;
            ok = copyOne(it->path(), pkg / "Scripts" / rel) && ok;
        }
    }

    if (fs::exists(projectRoot / "fmod.dll", ec))
        ok = copyOne(projectRoot / "fmod.dll", pkg / "fmod.dll") && ok;

    if (!FileManager::writeJson((pkg / "game.scene").string(), rewrittenScene))
    {
        r.messages.push_back("No se pudo escribir game.scene");
        ok = false;
    }
    else
    {
        ++r.fileCount;
    }

    r.ok = ok;
    if (ok)
        r.messages.push_back("Export completado en " + pkg.string() + ": " +
                             std::to_string(r.fileCount) + " ficheros, " +
                             std::to_string(r.totalBytes / 1024) + " KB");
    return r;
}

} // namespace DonTopo
