#include "DonTopo/Editor/ModelReimport.h"

#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/ImportSettings.h"
#include "DonTopo/Renderer/EditorRenderer.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/SkinnedMeshAnimations.h"

#include <exception>
#include <map>
#include <memory>

namespace DonTopo {

ModelReimportResult reimportModelUsers(GameObject* sceneRoot, const std::filesystem::path& fbx,
                                       EditorRenderer* renderer)
{
    ModelReimportResult r;
    if (!sceneRoot) return r;

    // Agrupa por el sourcePath EXACTO que lleva la malla: una carga por grupo.
    std::map<std::string, std::vector<GameObject*>> groups;
    sceneRoot->traverse([&](GameObject* go)
    {
        if (!go->hasMesh()) return;
        const std::string& sp = go->getMesh()->sourcePath;
        if (sp.empty()) return;
        // Usuario del FBX: su malla viene de el, O es un personaje que lo usa como
        // fuente de animacion externa (el flujo Mixamo: sus clips se releen con el
        // sidecar de ESE fichero). En el segundo caso se recarga el personaje
        // entero desde su propio sourcePath; las fuentes se releen en la receta.
        bool usa = sameAssetPath(sp, fbx);
        if (!usa)
            if (const SkinnedMesh* sk = go->getSkinnedMesh())
                for (const AnimationSource& src : sk->animationSources)
                    if (!src.builtin && sameAssetPath(src.path, fbx)) { usa = true; break; }
        if (usa) groups[sp].push_back(go);
    });

    bool anyRegistered = false;
    for (auto& [sourcePath, users] : groups)
    {
        std::shared_ptr<Mesh> fresh;
        try
        {
            fresh = ModelLoader::loadAuto(sourcePath);
        }
        catch (const std::exception& e)
        {
            r.warnings.push_back("Reimport de '" + fbx.filename().string() + "' fallido: " + e.what() +
                                 " (los objetos se quedan como estaban)");
            r.skipped += static_cast<int>(users.size());
            continue;
        }

        for (GameObject* go : users)
        {
            // Defensivo: un objeto con malla Y una carga asincrona en vuelo no se
            // toca (mismo criterio que MeshComponentCommand::put).
            if (go->pendingMeshJob != 0)
            {
                r.warnings.push_back("Reimport: '" + go->name + "' tiene una carga en curso, se salta");
                ++r.skipped;
                continue;
            }

            std::shared_ptr<const Mesh> next;
            if (const auto* sk = dynamic_cast<const SkinnedMesh*>(fresh.get()))
            {
                auto copy = std::make_shared<SkinnedMesh>(*sk);
                // Antes de soltar la malla vieja: de ella salen los renames y las
                // fuentes externas que el usuario tenia.
                if (const SkinnedMesh* old = go->getSkinnedMesh())
                    applyAnimationSourceConfig(*copy, animationSourceConfigOf(*old), r.warnings);
                next = std::move(copy);
            }
            else
            {
                next = fresh;   // estatica: compartida; quien la edite la copia (editMesh)
            }

            if (renderer) renderer->removeMeshComponent(go);
            else          go->setMesh(nullptr);
            go->setMesh(next);
            // DESPUES de setMesh, que baja los base*Taken: applyMaterialOverrides
            // recaptura como baseline lo que trae la malla NUEVA y reaplica encima
            // los overrides del objeto y su .mat.
            applyMaterialOverrides(*go);

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
        }
    }

    // Sin esperar, los objetos recargados aparecerian ~2 frames tarde.
    if (renderer && anyRegistered) renderer->flushUploadsAndWait();
    return r;
}

} // namespace DonTopo
