#pragma once
#include <cstdint>
#include "DonTopo/Core/AnimationIk.h"
#include "DonTopo/Core/PropertyTracks.h"
#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/RootMotionApply.h"
#include "DonTopo/Renderer/RootMotion.h"
#include "DonTopo/Renderer/SkinnedMesh.h"

namespace DonTopo
{
    // Todo lo que hay que decirle al backend por frame sobre un GameObject con
    // malla skinned. Estaba copiado en los TRES hosts —el runtime y los dos
    // caminos del sandbox, Vulkan y D3D12—, así que cada valor nuevo de la pose
    // había que añadirlo tres veces y el que se olvidara compilaba igual y
    // fallaba solo en un backend.
    //
    // Plantilla y no `EditorRenderer&` por los tests: los dos backends heredan
    // de esa interfaz, pero son 75 métodos puros y un doble tendría que
    // implementarlos todos para comprobar estas cinco llamadas. Con la
    // plantilla, el doble declara los cinco que se usan. De paso no hay
    // despacho virtual.
    //
    // evaluateTransitions distingue Edit de Play: en Edit el tiempo del estado
    // avanza pero el grafo no se mueve. Llega como parámetro porque cada host
    // lo sabe de un sitio distinto (el runtime siempre juega, el sandbox lo
    // pregunta al editor o al renderer) y la interfaz del backend no lo conoce.
    // Lo del Animator que NO depende de la malla con esqueleto: avanzar el
    // grafo, el root motion y los clips de propiedades. Antes esto vivía dentro
    // de applySkinnedFrame, que salía en cuanto el objeto no tenía índice
    // skinned: un objeto sin esqueleto no animaba nada.
    //
    // Devuelve true si escribió alguna propiedad de MATERIAL: esos factores
    // viven en el GameObject y hay que empujarlos al backend (hoy solo los
    // empujaban el panel y los comandos del editor).
    inline bool applyAnimatorFrame(GameObject& go, float dt, bool evaluateTransitions)
    {
        const auto& anim = go.getAnimator();
        if (!anim) return false;
        // El Animator es el único dueño de animTime: calcula en CPU y el
        // backend solo recibe el resultado.
        anim->update(dt, evaluateTransitions);
        // Root motion antes de mandar el transform: el avance de este update ya
        // sale en la posición que recibe el backend. dt es el del frame: la
        // velocidad del Animator ya va en los ticks.
        if (!anim->rootMotionSamples().empty())
            if (const SkinnedMesh* sk = go.getSkinnedMesh())
                applyRootMotion(go, rootMotionDelta(*sk, *anim), dt);

        AnimatorComponent::PropertySampleRef muestras[kMaxLayersPose * kMaxPoseSamplesPerLayer];
        const int n = anim->propertySamples(muestras, (int)(sizeof(muestras) / sizeof(muestras[0])));
        if (n == 0) return false;

        bool  escritas[(int)PropertyId::Count] = {};
        float valores [(int)PropertyId::Count] = {};
        for (int p = 0; p < (int)PropertyId::Count; p++)
        {
            const PropertyId id = (PropertyId)p;
            PropertyContribution aporta[kMaxLayersPose * kMaxPoseSamplesPerLayer];
            int m = 0;
            for (int k = 0; k < n; k++)
            {
                const PropertyClip& clip = anim->propertyClips()[(size_t)muestras[k].clip];
                for (const auto& tr : clip.tracks)
                {
                    if (tr.property != id || !tr.resolved || tr.keys.empty()) continue;
                    aporta[m++] = { samplePropertyTrack(tr, muestras[k].time, propertyGet(go, id)),
                                    muestras[k].weight };
                    break;   // una pista por propiedad y clip: la primera manda
                }
            }
            // Una propiedad que NADIE anima no se toca: así una pista de solo Y
            // no pisa la X y la Z del objeto.
            if (m == 0) continue;
            escritas[p] = true;
            valores[p]  = blendPropertyValues(id, aporta, m);
        }
        propertyApply(go, escritas, valores);
        return escritas[(int)PropertyId::MaterialMetallic] || escritas[(int)PropertyId::MaterialRoughness];
    }

    template <typename R>
    void applySkinnedFrame(GameObject& go, R& renderer, float dt, bool evaluateTransitions)
    {
        // El grafo corre SIEMPRE, tenga o no malla con esqueleto: es lo que
        // permite animar una puerta o una luz con un clip de propiedades.
        const bool tocoMaterial = applyAnimatorFrame(go, dt, evaluateTransitions);
        if (tocoMaterial && go.staticRenderIndex >= 0)
            renderer.setObjectMaterialFactors((size_t)go.staticRenderIndex,
                                              propertyGet(go, PropertyId::MaterialMetallic),
                                              propertyGet(go, PropertyId::MaterialRoughness));

        // Sin índice no está dado de alta en el backend: ni se dibuja ni hay
        // pose que mandar.
        if (go.skinnedRenderIndex < 0) return;

        // ANTES de tocar la animación. Hoy donde de verdad importa es el camino
        // sin Animator en Vulkan: `Renderer::updateAnimation` congela el reloj
        // de un mesh oculto, así que el flag tiene que estar ya puesto o iría un
        // frame por detrás. Con Animator el orden no cambia nada (el reloj lo
        // lleva la CPU y `setAnimationPose` no mira la visibilidad), y D3D12 no
        // congela en ningún camino; se mantiene un único orden para los dos
        // para que esa diferencia no dependa de quién llama.
        renderer.setSkinnedMeshVisible(go.skinnedRenderIndex, go.meshVisible);

        if (const auto& anim = go.getAnimator())
        {
            // El update y el root motion ya los hizo applyAnimatorFrame.
            // La pose entera siempre: muestras del estado actual, del que se
            // apaga (con su propio blend) y la congelada si un fade se
            // interrumpió. La petición de congelar se consume AQUÍ, al
            // entregarla, y no en el siguiente update: un CrossFade de Lua
            // entre dos updates la perdería.
            renderer.setAnimationPose(go.skinnedRenderIndex, anim->pose());
            anim->clearFreezeRequest();
        }
        else
        {
            // Sin Animator: clip 0 en bucle, exactamente como antes de que el
            // componente existiera. Los dos caminos no se pisan.
            renderer.updateAnimation(go.skinnedRenderIndex, dt);
        }

        // IK: el objetivo y el pole son GameObjects de la escena, y el shader
        // los quiere en espacio del MODELO. Se resuelven aquí, que es donde hay
        // GameObject; el Animator no conoce la escena. Se llama SIEMPRE, también
        // con count 0: si no, quitar la última restricción dejaría encendida la
        // del frame anterior.
        AnimationIk ik;
        if (const auto& anim = go.getAnimator())
        {
            const GameObject* raiz = &go;
            while (raiz->parent) raiz = raiz->parent;
            auto porId = [](const GameObject* nodo, uint64_t id) -> const GameObject* {
                if (id == 0) return nullptr;
                std::vector<const GameObject*> pila = { nodo };
                while (!pila.empty())
                {
                    const GameObject* x = pila.back();
                    pila.pop_back();
                    if (x->id == id) return x;
                    for (const auto& h : x->children) pila.push_back(h.get());
                }
                return nullptr;
            };
            const glm::mat4 aModelo = glm::inverse(go.worldTransform);
            for (const auto& c : anim->ikConstraints())
            {
                if (ik.count >= kMaxIkPose) break;
                if (c.boneIndex < 0 || c.weight <= 0.0f) continue;
                const GameObject* objetivo = porId(raiz, c.targetId);
                if (!objetivo) continue;
                IkSolve s;
                s.type        = c.type == AnimatorComponent::IkType::TwoBone ? 1u : 0u;
                s.bone        = c.boneIndex;
                s.parent      = c.parentIndex;
                s.grandParent = c.grandParentIndex;
                s.weight      = c.weight;
                s.target      = glm::vec3(aModelo * glm::vec4(glm::vec3(objetivo->worldTransform[3]), 1.0f));
                s.aimAxis     = c.aimAxis;
                s.maxAngle    = c.maxAngle;
                if (const GameObject* pole = porId(raiz, c.poleId))
                {
                    s.pole    = glm::vec3(aModelo * glm::vec4(glm::vec3(pole->worldTransform[3]), 1.0f));
                    s.hasPole = 1u;
                }
                ik.solves[ik.count++] = s;
            }
        }
        renderer.setAnimationIk(go.skinnedRenderIndex, ik);

        renderer.setSkinnedTransform(go.skinnedRenderIndex, go.worldTransform);
        // El backend no conoce el flag: con el SSR apagado se le manda un 0.
        renderer.setSkinnedSsr(go.skinnedRenderIndex,
                               go.ssrEnabled ? go.ssrIntensity : 0.0f);
    }
}
