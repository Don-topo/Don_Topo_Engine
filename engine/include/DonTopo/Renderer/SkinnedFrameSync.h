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
    // Everything that has to be told to the backend per frame about a GameObject with
    // a skinned mesh. It was copied in the THREE hosts (the runtime and the two
    // sandbox paths, Vulkan and D3D12), so every new pose value
    // had to be added three times and the one that got forgotten compiled all the same and
    // failed only in one backend.
    //
    // A template and not `EditorRenderer&` because of the tests: both backends inherit
    // from that interface, but it is 75 pure methods and a double would have to
    // implement all of them to check these five calls. With the
    // template, the double declares the five that are used. Incidentally there is no
    // virtual dispatch.
    //
    // evaluateTransitions distinguishes Edit from Play: in Edit the state's time
    // advances but the graph does not move. It arrives as a parameter because each host
    // knows it from a different place (the runtime always plays, the sandbox
    // asks the editor or the renderer) and the backend interface does not know it.
    // The part of the Animator that does NOT depend on the skeleton mesh: advancing the
    // graph, root motion and property clips. Before, this lived inside
    // applySkinnedFrame, which returned as soon as the object had no skinned
    // index: an object without a skeleton animated nothing.
    //
    // It returns WHAT it wrote, because both hosts propagate the worldTransforms and
    // push the transform to the backend BEFORE getting here: whatever is animated in
    // this frame has to be resent, or it would go one frame behind.
    struct AnimatorFrameResult
    {
        bool transform = false;   // any of position, rotation or scale
        bool material  = false;   // metallic or roughness
    };

    inline AnimatorFrameResult applyAnimatorFrame(GameObject& go, float dt, bool evaluateTransitions)
    {
        AnimatorFrameResult res;
        const auto& anim = go.getAnimator();
        if (!anim) return res;
        // The Animator is the sole owner of animTime: it computes on the CPU and the
        // backend only receives the result.
        anim->update(dt, evaluateTransitions);
        // Root motion before sending the transform: this update's advance already
        // comes out in the position the backend receives. dt is the frame's: the
        // Animator's speed is already in the ticks.
        if (!anim->rootMotionSamples().empty())
            if (const SkinnedMesh* sk = go.getSkinnedMesh())
                applyRootMotion(go, rootMotionDelta(*sk, *anim), dt);

        AnimatorComponent::PropertySampleRef muestras[kMaxLayersPose * kMaxPoseSamplesPerLayer];
        const int n = anim->propertySamples(muestras, (int)(sizeof(muestras) / sizeof(muestras[0])));
        if (n == 0) return res;

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
                    // Tracks with a parameter target (the curves) were already
                    // written by the component inside update(): here only
                    // the object's properties go.
                    if (tr.target != TrackTarget::Property) continue;
                    if (tr.property != id || !tr.resolved || tr.keys.empty()) continue;
                    aporta[m++] = { samplePropertyTrack(tr, muestras[k].time, propertyGet(go, id)),
                                    muestras[k].weight };
                    break;   // one track per property and clip: the first one rules
                }
            }
            // A property that NOBODY animates is not touched: this way a Y-only track
            // does not overwrite the object's X and Z.
            if (m == 0) continue;
            escritas[p] = true;
            valores[p]  = blendPropertyValues(id, aporta, m);
        }
        propertyApply(go, escritas, valores);

        for (int p = 0; p <= (int)PropertyId::ScaleZ; p++) res.transform = res.transform || escritas[p];
        res.material = escritas[(int)PropertyId::MaterialMetallic] || escritas[(int)PropertyId::MaterialRoughness];
        // The world of THIS object and its children's, right here: the hosts already
        // propagated before the traversal, so without this the animated part would reach
        // the backend one frame late and the children two.
        if (res.transform)
            go.updateWorldTransforms(go.parent ? go.parent->worldTransform : glm::mat4(1.0f));
        return res;
    }

    template <typename R>
    void applySkinnedFrame(GameObject& go, R& renderer, float dt, bool evaluateTransitions)
    {
        // The graph ALWAYS runs, whether or not there is a skeleton mesh: it is what
        // allows animating a door or a light with a property clip.
        const AnimatorFrameResult animado = applyAnimatorFrame(go, dt, evaluateTransitions);
        if (go.staticRenderIndex >= 0)
        {
            // Resend what was just animated: the host pushed this object's transform
            // BEFORE calling here, and the material factors were only pushed
            // by the panel and the editor commands.
            if (animado.transform) renderer.setTransform(go.staticRenderIndex, go.worldTransform);
            if (animado.material)
                renderer.setObjectMaterialFactors((size_t)go.staticRenderIndex,
                                                  propertyGet(go, PropertyId::MaterialMetallic),
                                                  propertyGet(go, PropertyId::MaterialRoughness));
        }

        // Without an index it is not registered in the backend: it is not drawn and there is
        // no pose to send.
        if (go.skinnedRenderIndex < 0) return;

        // BEFORE touching the animation. Today where it really matters is the path
        // without an Animator in Vulkan: `Renderer::updateAnimation` freezes the clock
        // of a hidden mesh, so the flag has to be set already or it would go one
        // frame behind. With an Animator the order changes nothing (the clock
        // is kept by the CPU and `setAnimationPose` does not look at visibility), and D3D12 does not
        // freeze on any path; a single order is kept for both
        // so that this difference does not depend on who calls.
        renderer.setSkinnedMeshVisible(go.skinnedRenderIndex, go.meshVisible);

        if (const auto& anim = go.getAnimator())
        {
            // The update and root motion were already done by applyAnimatorFrame.
            // The whole pose always: samples of the current state, of the one that is
            // fading out (with its own blend) and the frozen one if a fade was
            // interrupted. The freeze request is consumed HERE, when
            // delivering it, and not in the next update: a Lua CrossFade
            // between two updates would lose it.
            renderer.setAnimationPose(go.skinnedRenderIndex, anim->pose());
            anim->clearFreezeRequest();
        }
        else
        {
            // Without an Animator: clip 0 in a loop, exactly as before the
            // component existed. The two paths do not step on each other.
            renderer.updateAnimation(go.skinnedRenderIndex, dt);
        }

        // IK: the target and the pole are scene GameObjects, and the shader
        // wants them in MODEL space. They are resolved here, which is where a
        // GameObject exists; the Animator does not know the scene. It is ALWAYS called, also
        // with count 0: otherwise, removing the last constraint would leave the
        // previous frame's one switched on.
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
        // The backend does not know the flag: with SSR off it is sent a 0.
        renderer.setSkinnedSsr(go.skinnedRenderIndex,
                               go.ssrEnabled ? go.ssrIntensity : 0.0f);
    }
}
