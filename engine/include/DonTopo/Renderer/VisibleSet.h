#pragma once
#include "DonTopo/Renderer/Frustum.h"
#include "DonTopo/Renderer/InstanceBatching.h"
#include "DonTopo/Renderer/RenderObjects.h"
#include "DonTopo/Renderer/SharedGpuMesh.h"

#include <cstdint>
#include <vector>

namespace DonTopo
{
    // Who enters a pass. The decision was written FOUR times inside the Vulkan
    // backend (scene, shadows (per cascade), depth pre-pass and selection
    // outline) and all four had to give the same result: if they diverge, AO
    // darkens against geometry that is not drawn, or a shadow is left floating
    // with no object casting it. Diverging there breaks nothing the
    // validation layer can see.
    //
    // Nothing from any graphics API beyond the handles that SharedGpuMesh
    // already brings, same as Culling (Frustum.h) and Batching
    // (InstanceBatching.h): it can be exercised without a device.
    namespace Visibility
    {
        // The four usual guards, in the same order they had:
        //
        //  - obj.meshVisible: the "Visible" checkbox of the Mesh component. A hidden
        //    mesh is not sent to the GPU in ANY pass, so it does not cast
        //    a shadow or occlude either.
        //  - !gpu: the entry was deleted from the editor.
        //  - uploadTicket ahead of the last completed one: the upload is still in
        //    flight, its textures are still in TRANSFER_DST_OPTIMAL and
        //    sampling them would read garbage. It shows up as soon as its batch's
        //    fence signals.
        //  - Outside the frustum: it does not even spend a slot in the SSBO. Objects
        //    without an AABB (empty mesh, hasBounds = false) cannot be bounded and
        //    always pass.
        //
        // The frustum is chosen by the caller: the camera in the scene pass, that
        // of THIS cascade in the shadow pass. An object the camera does not see can
        // still cast a shadow onto what is seen.
        inline bool objectVisible(const RenderObject& obj, const SharedGpuMesh* gpu,
                                  uint64_t lastCompletedTicket, const Culling::Frustum& frustum)
        {
            if (!obj.meshVisible || !gpu || gpu->uploadTicket > lastCompletedTicket)
                return false;

            if (gpu->hasBounds &&
                !Culling::aabbVisible(frustum, gpu->aabbMin, gpu->aabbMax, obj.transform))
            {
                return false;
            }

            return true;
        }

        // Evaluates ALL objects and leaves the result in `out`, ready for
        // Batching::buildInstanceBatches. Invisible ones also go in, with
        // visible = false: the grouping skips them, but the Performance panel
        // counts them as culled.
        //
        // ssrEnabled = false sets the SSR strength to 0 on all of them. That is what
        // the passes that do not draw color (shadows, depth) want: SSR
        // enters the grouping key, so with a single value fewer draws come out and
        // the resulting map is identical.
        // colorPass governs the PBR factors and CANNOT be merged with
        // ssrEnabled, however much the caller passes `colorPass && ssrEnabled`
        // in the other one: ssrEnabled is the GLOBAL SSR switch, so
        // turning it off would leave the factors at their default value in the pass that
        // does draw color (all objects with metallic 0 and roughness 0.5, the
        // whole scene matte, for touching a setting that has nothing to do with it).
        inline void gatherCandidates(const std::vector<RenderObject>& objects,
                                     const SharedGpuMeshCache& meshes,
                                     uint64_t lastCompletedTicket,
                                     const Culling::Frustum& frustum,
                                     bool ssrEnabled,
                                     bool colorPass,
                                     std::vector<Batching::BatchCandidate>& out)
        {
            out.clear();
            out.reserve(objects.size());
            for (const RenderObject& obj : objects)
            {
                const SharedGpuMesh* gpu = meshes.get(obj.sharedIndex);
                const bool visible = objectVisible(obj, gpu, lastCompletedTicket, frustum);
                const float ssr    = ssrEnabled ? obj.ssrStrength : 0.0f;
                // The defaults of BatchCandidate and not the object's: it is what
                // collapses the groups in shadows and depth, where the material
                // is not read.
                const Batching::BatchCandidate neutro{};
                const float metallic  = colorPass ? obj.metallic  : neutro.metallic;
                const float roughness = colorPass ? obj.roughness : neutro.roughness;
                out.push_back({ obj.sharedIndex, visible, &obj.transform, ssr,
                                metallic, roughness });
            }
        }
    }
}
