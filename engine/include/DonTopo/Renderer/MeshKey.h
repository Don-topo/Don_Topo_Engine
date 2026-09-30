#pragma once
#include <string>

namespace DonTopo
{
    struct Mesh;

    // Content key: two Mesh that produce the same key generate exactly the same
    // GPU resources. It mixes exact discriminants (sizes, texture paths,
    // metallic/roughness) with an FNV-1a of the bytes of vertices, indices and
    // embedded textures. The mesh name and its sourcePath do NOT go in: they do
    // not affect a single byte of what is uploaded to the GPU.
    //
    // It lives in its own header and not in SharedGpuMesh.h because it does not
    // depend on any backend: the Vulkan Renderer and the DirectX 12 one both use
    // it, and dragging vulkan.h into the second one for a hashing function makes
    // no sense.
    std::string makeSharedMeshKey(const Mesh& mesh);
}
