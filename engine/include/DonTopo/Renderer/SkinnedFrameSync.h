#pragma once
#include <cstdint>
#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Core/GameObject.h"

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
    template <typename R>
    void applySkinnedFrame(GameObject& go, R& renderer, float dt, bool evaluateTransitions)
    {
        // Sin índice no está dado de alta en el backend: ni se dibuja ni se le
        // avanza el reloj.
        if (go.skinnedRenderIndex < 0) return;

        // ANTES de tocar la animación: el backend congela el reloj de un mesh
        // oculto, así que el flag tiene que estar ya puesto o iría un frame por
        // detrás.
        renderer.setSkinnedMeshVisible(go.skinnedRenderIndex, go.meshVisible);

        if (const auto& anim = go.getAnimator())
        {
            // El Animator es el único dueño de animTime: calcula en CPU y el
            // backend solo recibe el resultado.
            anim->update(dt, evaluateTransitions);
            // setAnimationBlend siempre: los pose* resuelven ya el cross-fade y
            // el blend por parámetro, y sin ninguno de los dos el peso vale 1 y
            // el backend ni mira el segundo clip. Primero el clip actual (B),
            // después el que se apaga (A).
            renderer.setAnimationBlend(go.skinnedRenderIndex,
                                       (uint32_t)anim->poseClipB(),
                                       anim->poseTimeB(),
                                       (uint32_t)anim->poseClipA(),
                                       anim->poseTimeA(),
                                       anim->poseWeight(),
                                       anim->poseLockRootMotion());
        }
        else
        {
            // Sin Animator: clip 0 en bucle, exactamente como antes de que el
            // componente existiera. Los dos caminos no se pisan.
            renderer.updateAnimation(go.skinnedRenderIndex, dt);
        }

        renderer.setSkinnedTransform(go.skinnedRenderIndex, go.worldTransform);
        // El backend no conoce el flag: con el SSR apagado se le manda un 0.
        renderer.setSkinnedSsr(go.skinnedRenderIndex,
                               go.ssrEnabled ? go.ssrIntensity : 0.0f);
    }
}
