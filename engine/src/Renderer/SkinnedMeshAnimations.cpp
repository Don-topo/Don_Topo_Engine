#include "DonTopo/Renderer/SkinnedMeshAnimations.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include <filesystem>

namespace DonTopo
{
    std::string uniqueClipName(const std::vector<AnimationClip>& existing,
                               const std::string& base)
    {
        const std::string root = base.empty() ? std::string("Animation") : base;

        auto taken = [&](const std::string& n) {
            for (const auto& c : existing)
                if (c.name == n) return true;
            return false;
        };

        if (!taken(root)) return root;

        int suffix = 1;
        std::string candidate = root + " (" + std::to_string(suffix) + ")";
        while (taken(candidate))
            candidate = root + " (" + std::to_string(++suffix) + ")";
        return candidate;
    }

    bool addAnimationSource(SkinnedMesh& mesh, const std::string& path,
                            std::vector<std::string>& warnings,
                            const std::vector<std::string>* forcedNames)
    {
        LoadedClips loaded = ModelLoader::loadAnimationClips(path, mesh.skeleton);
        for (auto& w : loaded.warnings) warnings.push_back(w);

        if (loaded.clips.empty()) return false;   // el warning ya lo puso el loader

        const std::string base = std::filesystem::path(path).stem().string();

        AnimationSource src;
        src.path    = path;
        src.builtin = false;

        for (size_t i = 0; i < loaded.clips.size(); i++)
        {
            AnimationClip clip = std::move(loaded.clips[i]);
            // forcedNames manda; si se agota (el FBX trae más clips que la
            // última vez), el resto cae en la regla normal de basename.
            const bool forced = forcedNames && i < forcedNames->size();
            clip.name = forced ? (*forcedNames)[i]
                                : uniqueClipName(mesh.animationClips, base);
            src.clipNames.push_back(clip.name);
            mesh.animationClips.push_back(std::move(clip));
        }

        mesh.animationSources.push_back(std::move(src));
        return true;
    }

    bool removeAnimationSource(SkinnedMesh& mesh, size_t sourceIndex)
    {
        if (sourceIndex >= mesh.animationSources.size())  return false;
        if (mesh.animationSources[sourceIndex].builtin)   return false;

        const std::vector<std::string>& names = mesh.animationSources[sourceIndex].clipNames;

        for (const std::string& n : names)
        {
            for (size_t i = 0; i < mesh.animationClips.size(); i++)
            {
                if (mesh.animationClips[i].name != n) continue;
                mesh.animationClips.erase(mesh.animationClips.begin() + (long)i);
                break;   // los nombres son únicos: uno y solo uno por nombre
            }
        }

        mesh.animationSources.erase(mesh.animationSources.begin() + (long)sourceIndex);
        return true;
    }

    bool renameClip(SkinnedMesh& mesh, const std::string& oldName,
                    const std::string& newName)
    {
        if (newName.empty() || oldName == newName) return false;

        for (const auto& c : mesh.animationClips)
            if (c.name == newName) return false;      // duplicado

        AnimationClip* target = nullptr;
        for (auto& c : mesh.animationClips)
            if (c.name == oldName) { target = &c; break; }
        if (!target) return false;

        target->name = newName;

        for (auto& src : mesh.animationSources)
            for (auto& n : src.clipNames)
                if (n == oldName) n = newName;

        return true;
    }
}
