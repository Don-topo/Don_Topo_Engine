#include "DonTopo/Editor/ModelReimport.h"

#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/ImportSettings.h"
#include "DonTopo/Physics/Colliders/Collider.h"
#include "DonTopo/Renderer/EditorRenderer.h"
#include "DonTopo/Renderer/ModelLoader.h"
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

    // Cola comun a los dos caminos (estatico por pieza y el de siempre), justo
    // despues de applyMaterialOverrides: registro en GPU y contador de
    // recargados. Extraida a lambda porque son mas de 3 lineas y divergir aqui
    // ha sido la fuente de bugs sutiles del reimport en el pasado.
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
        // Estatico: una lectura con todas las piezas y cada objeto recarga la
        // SUYA. La que ya no existe deja el objeto como estaba, con aviso (mismo
        // contrato que una recarga fallida).
        if (!ModelLoader::hasBones(sourcePath))
        {
            StaticModel model;
            try { model = ModelLoader::loadStatic(sourcePath); }
            catch (const std::exception& e)
            {
                r.warnings.push_back("Reimport de '" + fbx.filename().string() + "' fallido: " + e.what() +
                                     " (los objetos se quedan como estaban)");
                r.skipped += static_cast<int>(users.size());
                continue;
            }
            // Hijos de un grupo de piezas (Add Mesh de un modelo de > 1 pieza):
            // la escala del sidecar entro en la traslacion de su localTransform
            // (collectPieces), asi que un reimport con otra escala la corrige
            // por nueva/vieja. Si no, cada pieza encoge sobre su propio origen
            // y se queda en las posiciones de la escala vieja: el modelo se
            // desmonta. Grupo = el fichero tiene > 1 pieza Y el padre del
            // objeto tiene >= 2 hijos con ese mismo sourcePath. Un objeto
            // suelto (pieza 0 anadida antes de esta feature, o un fichero de
            // una pieza) lo coloco el usuario: no se mueve.
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
                    r.warnings.push_back("Reimport: '" + go->name + "' tiene una carga en curso, se salta");
                    ++r.skipped;
                    continue;
                }
                const int piece = go->getMesh()->piece;
                if (piece < 0 || static_cast<size_t>(piece) >= model.meshes.size())
                {
                    r.warnings.push_back("Reimport: '" + go->name + "' usaba la pieza " + std::to_string(piece) +
                                         ", que ya no esta en el fichero: se queda como estaba");
                    ++r.skipped;
                    continue;
                }
                auto& next = byPiece[piece];
                if (!next) next = std::make_shared<const Mesh>(model.meshes[piece]);
                if (renderer) renderer->removeMeshComponent(go);
                else          go->setMesh(nullptr);
                go->setMesh(next);
                // DESPUES de setMesh, que baja los base*Taken: applyMaterialOverrides
                // recaptura como baseline lo que trae la malla NUEVA y reaplica encima
                // los overrides del objeto y su .mat.
                applyMaterialOverrides(*go);
                if (rescale && inPieceGroup(go))
                {
                    go->localTransform[3].x *= scaleRatio;
                    go->localTransform[3].y *= scaleRatio;
                    go->localTransform[3].z *= scaleRatio;
                    // Misma receta que applyLocalTransform (ViewportPanel):
                    // mundo recalculado y teleport, o el actor de PhysX se
                    // queda donde estaba. Con el subarbol entero, que los
                    // nietos tambien se han movido.
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
            finishReload(go);
        }
    }

    // Sin esperar, los objetos recargados aparecerian ~2 frames tarde.
    if (renderer && anyRegistered) renderer->flushUploadsAndWait();
    return r;
}

} // namespace DonTopo
