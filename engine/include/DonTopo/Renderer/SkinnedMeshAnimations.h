#pragma once
#include <string>
#include <vector>
#include "DonTopo/Renderer/SkinnedMesh.h"

namespace DonTopo
{
    // Clip merge layer: no Vulkan on purpose, same as
    // SkinnedMeshPacking. Inside the Renderer it could only be tested with a
    // live VkDevice, that is, it could not be tested.

    // true if the clip animates something: some channel in which some value changes
    // over time. A clip that does not meet this leaves the character stuck in its
    // bind pose however long its duration is and however many keys it carries.
    //
    // VALUES are compared, not key counts. Mixamo character FBX files
    // carry a one-tick "mixamo.com" take whose Hips channel has two keys
    // per track with the SAME value at t=0 and t=1, and the rest of the channels a single
    // key: counting keys would report it as animated. That take ended up occupying clip
    // 0, precisely the index that all the engine's degraded paths fall to
    // (unresolved state, clipIndex out of range, orphan graph), so
    // any mismatch showed up as a character in T-pose.
    //
    // Absolute tolerance of 1e-4: below that there is exporter jitter,
    // not intent to animate. In Mixamo units (a character ~236 tall)
    // that is microns, and in a quaternion, thousandths of a degree.
    bool clipHasMotion(const AnimationClip& clip);

    // Returns base if no clip in existing uses it; otherwise, base + " (N)" with
    // the first free N. Empty base -> "Animation": the Animator resolves
    // clips by name, so an empty or repeated name leaves clips
    // unreachable.
    std::string uniqueClipName(const std::vector<AnimationClip>& existing,
                               const std::string& base);

    // Imports the animations of path and adds them to mesh.animationClips,
    // registering the source in mesh.animationSources.
    //
    // Clips are named by the file's basename (walk.fbx -> "walk",
    // "walk (1)"...): the internal name of a Mixamo FBX is "mixamo.com"
    // for all of them, and with that the clip list cannot be read.
    //
    // forcedNames, if it is not nullptr, overrides those names in order until exhausted.
    // It is used by scene loading and by the undo of a remove: without it, a renamed
    // clip would come back with the file's name and the graph states that
    // reference it would be left orphaned.
    //
    // A forcedName already in use (by an existing clip or by another earlier
    // forcedName of this same call) is NOT duplicated: it falls to uniqueClipName and a
    // warning is added. Duplicating the name would leave one of the two clips
    // unreachable, because the Animator resolves by name.
    //
    // Returns false and leaves mesh UNTOUCHED if the file contributes nothing (unreadable,
    // without animations, or no bone in common with mesh.skeleton).
    bool addAnimationSource(SkinnedMesh& mesh, const std::string& path,
                            std::vector<std::string>& warnings,
                            const std::vector<std::string>* forcedNames = nullptr);

    // Removes the source and the clips it contributed. false if the index is out of
    // range or points to the builtin source (that is the model, it cannot be removed).
    //
    // The indices of the surviving clips are shifted; nothing needs to be
    // fixed in the graph because the states reference by name and
    // AnimatorComponent::bindClips resolves them again.
    bool removeAnimationSource(SkinnedMesh& mesh, size_t sourceIndex);

    // Renames a clip and updates the clipNames of its source. false if oldName
    // does not exist, newName is empty or newName is already in use.
    //
    // Does NOT touch the Animator states: that is done by
    // AnimatorComponent::renameClipReferences, which lives in Core (this module is
    // Renderer and must not depend on Core).
    bool renameClip(SkinnedMesh& mesh, const std::string& oldName,
                    const std::string& newName);

    // Applies savedNames POSITIONALLY, all at once, to the first
    // min(savedNames.size(), source.clipNames.size()) clips of source,
    // meant to restore the saved renames of a scene onto the builtin
    // source just rebuilt by loadSkinned.
    //
    // Unlike chaining renameClip clip by clip, this resolves
    // ANY permutation correctly: a swap of two names with
    // sequential renameClip collides with itself (the second rename hits the
    // name the first one just freed, in the wrong slot)
    // and applies nothing, so the clips keep the name that
    // the FBX brings and an Animator that references the saved name binds to the
    // WRONG clip silently, which is worse than an orphan.
    //
    // It guards the unique-names invariant: if savedNames carries duplicates
    // among themselves, or a saved name already belongs to a clip that is NOT part
    // of this same batch (another source, or another clip outside the first n),
    // THAT index is not applied (the clip is left with the name it already had)
    // and a warning is issued naming the clip. A saved name equal to the one the clip already
    // has is a valid no-op, not a collision.
    void applyClipNamesPositionally(SkinnedMesh& mesh, AnimationSource& source,
                                    const std::vector<std::string>& savedNames,
                                    std::vector<std::string>& warnings);

    // Configuration of ONE animation source of a skinned mesh, as the
    // scene saves it. It is what has to be remembered from the old mesh to
    // rebuild it on a freshly loaded one (model reimport) and what
    // Scene::fromJson reads from its JSON: a single way to apply it.
    struct AnimationSourceConfig
    {
        std::string              path;
        bool                     builtin = false;
        std::vector<std::string> clipNames;   // final names, in order
    };

    // The sources of mesh, in the same order.
    std::vector<AnimationSourceConfig> animationSourceConfigOf(const SkinnedMesh& mesh);

    // Reapplies `sources` onto a mesh freshly loaded by loadSkinned: the BUILTIN
    // source already exists and only recovers the NAMES (positionally, all at
    // once: chaining renameClip collides with itself on a swap of two
    // names); the external ones are re-added with those names. An external source that
    // no longer loads (moved, deleted, another rig) is WARNED about and skipped: losing the
    // whole scene for that would be much worse, and the states that used its clips
    // are marked orphaned by bindClips. Never throws.
    void applyAnimationSourceConfig(SkinnedMesh& mesh,
                                    const std::vector<AnimationSourceConfig>& sources,
                                    std::vector<std::string>& warnings);
}
