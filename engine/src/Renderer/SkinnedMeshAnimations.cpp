#include "DonTopo/Renderer/SkinnedMeshAnimations.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include <cmath>
#include <filesystem>

namespace DonTopo
{
    bool clipHasMotion(const AnimationClip& clip)
    {
        constexpr float kEps = 1e-4f;

        // It is enough to compare against the FIRST key of the track: if all are
        // equal to the first, all are equal to each other, and if any differs
        // there is already motion. There is no need to compare every pair.
        auto varia = [](const auto& keys, auto component) {
            for (size_t k = 1; k < keys.size(); k++)
                for (int c = 0; c < component(keys[0]).length(); c++)
                    if (std::fabs(component(keys[k])[c] - component(keys[0])[c]) > kEps)
                        return true;
            return false;
        };

        for (const auto& ch : clip.channels)
        {
            if (varia(ch.posKeys,   [](const BoneKeyframe& k)  { return k.value; })) return true;
            if (varia(ch.scaleKeys, [](const BoneKeyframe& k)  { return k.value; })) return true;
            if (varia(ch.rotKeys,   [](const BoneKeyframeQ& k) { return k.value; })) return true;
        }
        return false;
    }

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

        if (loaded.clips.empty()) return false;   // the loader already emitted the warning

        const std::string base = std::filesystem::path(path).stem().string();
        const std::string file = std::filesystem::path(path).filename().string();

        AnimationSource src;
        src.path    = path;
        src.builtin = false;

        for (size_t i = 0; i < loaded.clips.size(); i++)
        {
            AnimationClip clip = std::move(loaded.clips[i]);
            // forcedNames wins; if it runs out (the FBX has more clips than the
            // last time), the rest fall under the normal basename rule.
            const bool forced = forcedNames && i < forcedNames->size();
            if (forced)
            {
                // The forced name wins IF it is free: uniqueClipName returns it
                // as is and a rename survives a save/load
                // intact. If it is already in use (by a previous clip, or by an earlier
                // forcedName of this same import) it returns a
                // suffixed variant, so comparing the result with what was
                // requested detects the collision without duplicating the "taken" logic.
                const std::string& wanted = (*forcedNames)[i];
                clip.name = uniqueClipName(mesh.animationClips, wanted);
                if (clip.name != wanted)
                    warnings.push_back(file + ": the saved clip name '" + wanted
                                        + "' was already in use, renamed to '" + clip.name + "'");
            }
            else
            {
                clip.name = uniqueClipName(mesh.animationClips, base);
            }
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
                break;   // names are unique: one and only one per name
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
            if (c.name == newName) return false;      // duplicate

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

    void applyClipNamesPositionally(SkinnedMesh& mesh, AnimationSource& source,
                                    const std::vector<std::string>& savedNames,
                                    std::vector<std::string>& warnings)
    {
        const size_t n = savedNames.size() < source.clipNames.size()
                        ? savedNames.size() : source.clipNames.size();
        if (n == 0) return;

        // Snapshot BEFORE touching anything: source.clipNames[i] is overwritten
        // further down, and locating each clip in mesh.animationClips by its
        // OLD name has to be done against the state prior to
        // any mutation; otherwise, an already applied rename could temporarily leave two
        // clips with the same name and a later name lookup
        // in the loop would find the wrong one.
        std::vector<std::string> oldNames(source.clipNames.begin(), source.clipNames.begin() + (long)n);

        // Index into animationClips of each clip in the batch, resolved only once
        // against oldNames (still intact at this point).
        std::vector<long> clipIdx(n, -1);
        for (size_t i = 0; i < n; i++)
            for (size_t k = 0; k < mesh.animationClips.size(); k++)
                if (mesh.animationClips[k].name == oldNames[i]) { clipIdx[i] = (long)k; break; }

        std::vector<bool> skip(n, false);

        // 1) Duplicates among the savedNames themselves: two indices pointing
        //    at the same target name would leave a clip unreachable. Both
        //    indices are discarded, neither of the two is applied.
        for (size_t i = 0; i < n; i++)
        {
            for (size_t j = i + 1; j < n; j++)
            {
                if (savedNames[i] != savedNames[j]) continue;
                if (!skip[i] && !skip[j])
                    warnings.push_back("Duplicate clip name when restoring the builtin source: '"
                                        + savedNames[i] + "', the original names are kept");
                skip[i] = true;
                skip[j] = true;
            }
        }

        // batchSet: ORIGINAL names of the clips that make up this batch,
        // "the clips being renamed in this same operation", exactly
        // as the finding asks. A target name that matches one of
        // these is not an external collision (it is, for example, the other side of
        // a swap).
        auto inBatch = [&](const std::string& name) {
            for (size_t k = 0; k < n; k++)
                if (oldNames[k] == name) return true;
            return false;
        };

        // 2) Collision with a clip OUTSIDE the batch (another source, or another clip of the
        //    mesh itself outside these n). A name equal to the one the clip already
        //    has is a valid no-op, never a collision.
        for (size_t i = 0; i < n; i++)
        {
            if (skip[i] || clipIdx[i] < 0) continue;
            if (savedNames[i] == oldNames[i]) continue;   // no-op

            for (const auto& c : mesh.animationClips)
            {
                if (c.name != savedNames[i] || inBatch(c.name)) continue;
                warnings.push_back("Saved clip name '" + savedNames[i]
                                    + "' already used by another clip, not applied when restoring the builtin source");
                skip[i] = true;
                break;
            }
        }

        // 3) Apply what survived: all at once, so a swap
        //    (A->B and B->A at the same time) converges instead of colliding with itself
        //    as it would by chaining renameClip.
        for (size_t i = 0; i < n; i++)
        {
            if (skip[i] || clipIdx[i] < 0) continue;
            if (savedNames[i] == oldNames[i]) continue;   // no-op

            mesh.animationClips[(size_t)clipIdx[i]].name = savedNames[i];
            source.clipNames[i] = savedNames[i];
        }
    }

    std::vector<AnimationSourceConfig> animationSourceConfigOf(const SkinnedMesh& mesh)
    {
        std::vector<AnimationSourceConfig> out;
        out.reserve(mesh.animationSources.size());
        for (const AnimationSource& s : mesh.animationSources)
            out.push_back({ s.path, s.builtin, s.clipNames });
        return out;
    }

    void applyAnimationSourceConfig(SkinnedMesh& mesh,
                                    const std::vector<AnimationSourceConfig>& sources,
                                    std::vector<std::string>& warnings)
    {
        for (const AnimationSourceConfig& src : sources)
        {
            if (src.builtin)
            {
                // Up to the smaller of the two sizes: an FBX re-exported with more or
                // fewer clips must not break loading.
                if (mesh.animationSources.empty()) continue;
                applyClipNamesPositionally(mesh, mesh.animationSources[0], src.clipNames, warnings);
                continue;
            }

            std::vector<std::string> sourceWarnings;
            const std::vector<std::string> names = src.clipNames;
            if (!addAnimationSource(mesh, src.path, sourceWarnings, &names))
                for (const std::string& w : sourceWarnings)
                    warnings.push_back(w);
        }
    }
}
