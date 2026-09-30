#pragma once

namespace DonTopo
{
    struct SkinnedMesh;

    // Culling bound for bone meshes, with nothing from any graphics API:
    // it lives next to Frustum.h for the same reason as that one, and by
    // the same path (it came out of Renderer when that file was split into passes).
    namespace Culling
    {
        // Radius of a sphere centered on the model's LOCAL ORIGIN that
        // contains the mesh in ANY pose of ANY of its clips.
        //
        // The rest-pose AABB is not enough: the compute deforms the vertices and a raised
        // arm sticks out of the box, so culling with it would make
        // the character disappear, the worst possible failure here.
        //
        // It does not evaluate any pose: it bounds bone by bone with the extreme values
        // of the keys and propagates through the hierarchy, so the bound also holds
        // between keyframes. It is conservative (it may be too large), never too small.
        //
        // Returns 0 if there is nothing to bound with (no bones, no vertices, or with
        // a NaN leaked in from the model); the caller treats it as "no bound" and
        // does not cull.
        float skinnedBoundRadius(const SkinnedMesh& mesh);
    }  // namespace Culling
}  // namespace DonTopo
