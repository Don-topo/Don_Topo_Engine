#include "DonTopo/Editor/ModelReimport.h"

#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/ImportSettings.h"
#include "DonTopo/Physics/Colliders/Collider.h"
#include "DonTopo/Renderer/EditorRenderer.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Renderer/SkinnedMeshAnimations.h"

#include <cmath>
#include <exception>
#include <map>
#include <memory>

namespace DonTopo {

ModelReimportResult reimportModelUsers(GameObject* sceneRoot, const std::filesystem::path& fbx,
                                       EditorRenderer* renderer, float scaleRatio)
{
    ModelReimportResult r;
    if (!sceneRoot) return r;

    // Group by the EXACT sourcePath the mesh carries: one load per group.
    std::map<std::string, std::vector<GameObject*>> groups;
    sceneRoot->traverse([&](GameObject* go)
    {
        if (!go->hasMesh()) return;
        const std::string& sp = go->getMesh()->sourcePath;
        if (sp.empty()) return;
        // User of the FBX: its mesh comes from it, OR it is a character that uses it as
        // an external animation source (the Mixamo flow: its clips are re-read with the
        // sidecar of THAT file). In the second case the whole character is reloaded
        // from its own sourcePath; the sources are re-read in the recipe.
        bool usa = sameAssetPath(sp, fbx);
        if (!usa)
            if (const SkinnedMesh* sk = go->getSkinnedMesh())
                for (const AnimationSource& src : sk->animationSources)
                    if (!src.builtin && sameAssetPath(src.path, fbx)) { usa = true; break; }
        if (usa) groups[sp].push_back(go);
    });

    bool anyRegistered = false;

    // Tail common to both paths (static per piece and the usual one), right
    // after applyMaterialOverrides: GPU registration and the counter of
    // reloaded ones. Extracted to a lambda because it is more than 3 lines and diverging here
    // has been the source of subtle reimport bugs in the past.
    auto finishReload = [&](GameObject* go)
    {
        if (renderer)
        {
            if (const SkinnedMesh* sk = go->getSkinnedMesh())
                go->skinnedRenderIndex = renderer->addSkinnedMesh(*sk, nullptr);
            else
                go->staticRenderIndex = renderer->addStaticMesh(*go->getMesh(), nullptr);
            anyRegistered = true;
        }
        if (go->hasAnimator())
            if (const SkinnedMesh* sk = go->getSkinnedMesh())
                go->getAnimator()->rebindClips(*sk, &r.warnings);

        ++r.reimported;
    };

    for (auto& [sourcePath, users] : groups)
    {
        // Static: one read with all the pieces and each object reloads
        // ITS OWN. The one that no longer exists leaves the object as it was, with a warning (same
        // contract as a failed reload).
        if (!ModelLoader::hasBones(sourcePath))
        {
            StaticModel model;
            try { model = ModelLoader::loadStatic(sourcePath); }
            catch (const std::exception& e)
            {
                r.warnings.push_back("Reimport of '" + fbx.filename().string() + "' failed: " + e.what() +
                                     " (the objects stay as they were)");
                r.skipped += static_cast<int>(users.size());
                continue;
            }
            // Children of a piece group (Add Mesh of a model with > 1 piece):
            // the sidecar scale entered the translation of their localTransform
            // (collectPieces), so a reimport with another scale corrects it
            // by new/old. Otherwise each piece shrinks around its own origin
            // and stays at the positions of the old scale: the model
            // falls apart. Group = the file has > 1 piece AND the object's parent
            // has >= 2 children with that same sourcePath. A loose
            // object (piece 0 added before this feature, or a one-piece
            // file) was placed by the user: it does not move.
            auto inPieceGroup = [&](const GameObject* go)
            {
                if (model.pieces.size() <= 1 || !go->parent) return false;
                int siblings = 0;
                for (const auto& c : go->parent->children)
                    if (c->hasMesh() && c->getMesh()->sourcePath == sourcePath) ++siblings;
                return siblings >= 2;
            };
            const bool rescale = std::isfinite(scaleRatio) && scaleRatio > 0.0f && scaleRatio != 1.0f;
            std::map<int, std::shared_ptr<const Mesh>> byPiece;
            for (GameObject* go : users)
            {
                if (go->pendingMeshJob != 0)
                {
                    r.warnings.push_back("Reimport: '" + go->name + "' has a load in progress, skipped");
                    ++r.skipped;
                    continue;
                }
                const int piece = go->getMesh()->piece;
                if (piece < 0 || static_cast<size_t>(piece) >= model.meshes.size())
                {
                    r.warnings.push_back("Reimport: '" + go->name + "' used piece " + std::to_string(piece) +
                                         ", which is no longer in the file: it stays as it was");
                    ++r.skipped;
                    continue;
                }
                auto& next = byPiece[piece];
                if (!next) next = std::make_shared<const Mesh>(model.meshes[piece]);
                if (renderer) renderer->removeMeshComponent(go);
                else          go->setMesh(nullptr);
                go->setMesh(next);
                // AFTER setMesh, which lowers the base*Taken: applyMaterialOverrides
                // recaptures as baseline what the NEW mesh brings and reapplies on top
                // the object's overrides and its .mat.
                applyMaterialOverrides(*go);
                if (rescale && inPieceGroup(go))
                {
                    go->localTransform[3].x *= scaleRatio;
                    go->localTransform[3].y *= scaleRatio;
                    go->localTransform[3].z *= scaleRatio;
                    // Same recipe as applyLocalTransform (ViewportPanel):
                    // world recomputed and teleport, or the PhysX actor
                    // stays where it was. With the whole subtree, since the
                    // grandchildren have moved too.
                    go->updateWorldTransforms(go->parent ? go->parent->worldTransform : glm::mat4(1.0f));
                    go->traverse([](GameObject* n)
                    {
                        if (auto col = n->anyCollider()) col->teleport(n->worldTransform);
                    });
                }
                finishReload(go);
            }
            continue;
        }

        std::shared_ptr<Mesh> fresh;
        try
        {
            // Here the file has bones (the static branch already exited above):
            // loadSkinned directly, not loadAuto, which would repeat the hasBones
            // check, another full ReadFile per character.
            fresh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned(sourcePath));
        }
        catch (const std::exception& e)
        {
            r.warnings.push_back("Reimport of '" + fbx.filename().string() + "' failed: " + e.what() +
                                 " (the objects stay as they were)");
            r.skipped += static_cast<int>(users.size());
            continue;
        }

        for (GameObject* go : users)
        {
            // Defensive: an object with a mesh AND an asynchronous load in flight is not
            // touched (same criterion as MeshComponentCommand::put).
            if (go->pendingMeshJob != 0)
            {
                r.warnings.push_back("Reimport: '" + go->name + "' has a load in progress, skipped");
                ++r.skipped;
                continue;
            }

            std::shared_ptr<const Mesh> next;
            if (const auto* sk = dynamic_cast<const SkinnedMesh*>(fresh.get()))
            {
                auto copy = std::make_shared<SkinnedMesh>(*sk);
                // Before dropping the old mesh: the renames and external
                // sources the user had come from it.
                if (const SkinnedMesh* old = go->getSkinnedMesh())
                    applyAnimationSourceConfig(*copy, animationSourceConfigOf(*old), r.warnings);
                next = std::move(copy);
            }
            else
            {
                next = fresh;   // static: shared; whoever edits it copies it (editMesh)
            }

            if (renderer) renderer->removeMeshComponent(go);
            else          go->setMesh(nullptr);
            go->setMesh(next);
            // AFTER setMesh, which lowers the base*Taken: applyMaterialOverrides
            // recaptures as baseline what the NEW mesh brings and reapplies on top
            // the object's overrides and its .mat.
            applyMaterialOverrides(*go);
            finishReload(go);
        }
    }

    // Without waiting, the reloaded objects would appear ~2 frames late.
    if (renderer && anyRegistered) renderer->flushUploadsAndWait();
    return r;
}

} // namespace DonTopo
