#pragma once
#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace DonTopo
{
    // Grouping of objects into instanced draws, with nothing from any graphics
    // API: used by the Vulkan backend and the DirectX 12 one. It lived inside
    // Renderer (Vulkan) until there was a second backend that needed it,
    // just as happened with Culling (Renderer/Frustum.h).
    namespace Batching
    {
        // An instanced draw: all the instances in the range
        // [firstInstance, firstInstance + instanceCount) of the transforms
        // buffer share the shared entry sharedIndex, so they
        // are drawn with a single indexed draw.
        struct InstanceBatch {
            int      sharedIndex   = -1;
            uint32_t firstInstance = 0;
            uint32_t instanceCount = 0;
            // The three values PER GROUP that travel by push constant, and that is why
            // all three go into the grouping KEY along with sharedIndex: two
            // objects that do not match on them cannot share a draw.
            //
            // metallic and roughness arrived here when they were taken out of the dedup
            // key (makeSharedMeshKey). While they lived there, "same shared
            // entry" already implied "same factors" and this key did not
            // have to look at them (that is the premise this comment used to document,
            // and it has stopped being true). In exchange, two identical cubes with
            // different metallic now SHARE the mesh in VRAM (before they were two
            // copies) and are only split into two draws, which is much cheaper.
            float    ssrStrength   = 0.0f;
            float    metallic      = 0.0f;
            float    roughness     = 0.5f;
        };

        // An object already evaluated by the pass that will draw it. The guards
        // (deleted entry, upload in flight) and the AABB culling are resolved by the
        // caller (it is the one that has the GPU cache and the frustum) and arrive here
        // summarized in `visible`. The transform goes by pointer: the grouping only
        // copies it into the buffer, it does not keep it.
        struct BatchCandidate {
            int              sharedIndex = -1;
            bool             visible     = false;
            const glm::mat4* transform   = nullptr;
            // The passes that do not draw color (shadows, depth pre-pass) leave the
            // three at their default value: with a single value the grouping comes out
            // identical to what it was before the feature, which is what is wanted there,
            // since those passes do not read the material, so splitting groups by factors
            // would only cost them draws.
            float            ssr         = 0.0f;
            float            metallic    = 0.0f;
            float            roughness   = 0.5f;
        };

        // Groups the VISIBLE candidates by sharedIndex and leaves their transforms
        // contiguous per group in outTransforms (which already points to the slot in the
        // buffer, with room for outCapacity matrices). The order is stable: groups
        // come out in order of first appearance and within each group the
        // order of the candidates is kept, so the result does not dance
        // between frames.
        //
        // firstInstanceBase is the ABSOLUTE index inside the buffer of the
        // first matrix written: the passes share a buffer and the second one
        // writes after the first, so without a base the firstInstance of the
        // main pass would point into the shadow range.
        //
        // Returns how many matrices were written. If they do not all fit it truncates by
        // groups (those that do not fit come out with instanceCount 0 and are discarded)
        // rather than write out of range: the caller sizes the buffer
        // beforehand, this is the safety net.
        uint32_t buildInstanceBatches(const BatchCandidate* candidates,
                                      size_t                count,
                                      glm::mat4*            outTransforms,
                                      uint32_t              outCapacity,
                                      uint32_t              firstInstanceBase,
                                      std::vector<InstanceBatch>& outBatches);
    }  // namespace Batching
}  // namespace DonTopo
