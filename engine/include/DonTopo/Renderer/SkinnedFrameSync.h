#pragma once
#include <cstdint>
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
    template <typename R>
    void applySkinnedFrame(GameObject& go, R& renderer, float dt, bool evaluateTransitions)
    {
        // Sin índice no está dado de alta en el backend: ni se dibuja ni se le
        // avanza el reloj.
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
            // El Animator es el único dueño de animTime: calcula en CPU y el
            // backend solo recibe el resultado.
            anim->update(dt, evaluateTransitions);
            // Root motion antes de mandar el transform: el avance de este
            // update ya sale en la posición que recibe el backend. dt es el
            // del frame: la velocidad del Animator ya va en los ticks.
            if (!anim->rootMotionSamples().empty())
                if (const SkinnedMesh* sk = go.getSkinnedMesh())
                    applyRootMotion(go, rootMotionDelta(*sk, *anim), dt);
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

        renderer.setSkinnedTransform(go.skinnedRenderIndex, go.worldTransform);
        // El backend no conoce el flag: con el SSR apagado se le manda un 0.
        renderer.setSkinnedSsr(go.skinnedRenderIndex,
                               go.ssrEnabled ? go.ssrIntensity : 0.0f);
    }
}
