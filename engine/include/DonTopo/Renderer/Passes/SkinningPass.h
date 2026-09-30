#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>
#include "DonTopo/Renderer/RenderObjects.h"

namespace DonTopo {

class GpuDevice;

// Compute skinning: the three dispatches that evaluate the animation of each
// mesh with bones (bone_eval -> bone_hierarchy -> skinning) and the resources they
// share. The OUTPUT is each object's outputVertexBuffer, which is then read
// as a vertex buffer by the scene pass, the shadow pass and the outline.
//
// What is NOT its own: the four GRAPHICS pipelines of the skinned meshes. Those
// stay in the Renderer because they depend on the MSAA sample count and on the
// scene render pass, which govern several passes.
//
// Ties with code that is not its own:
//  - descLayout()/allocateSet(): the compute descriptor sets come from there,
//    allocated by initSkinnedRenderObject and freed by destroySkinnedRenderObject, which
//    belong to the Renderer because they go with each mesh's SSBOs.
//  - the objects and the visible list arrive through the Context: they belong to the
//    Renderer, and the visible list is the SAME one consumed by the drawing
//    loop (skipping here an object that is later drawn would leave it with the pose of the
//    last frame in which it was visible).
class SkinningPass {
public:
    // Mirror of the push_constant block of the three skinning .comp files (and of
    // ComputePush in D3D12): same order, same types. The pose goes in the
    // pose block (PoseBlock.h); here only the offset of the frame's copy.
    struct Push
    {
        uint32_t boneCount;
        uint32_t vertexCount;
        // --- Only read by bone_eval.comp ---
        uint32_t rootMotionMode;    // 0 free, 1 root pinned to bind, 2 only X and Z
        uint32_t poseBlockOffset;   // en uints
        // --- bone_ik.comp y bone_hierarchy.comp ---
        uint32_t ikBlockOffset;     // en uints
        uint32_t flags;             // bit 0: the hierarchy writes only world
    };
    static_assert(sizeof(Push) == 24, "Push: the 4 .comp shaders and D3D12's ComputePush declare this layout");

    struct Context {
        GpuDevice& gpu;
        // The meshes with bones and the frame's visible list, both from the
        // Renderer.
        std::vector<SkinnedRenderObject>& skinnedObjects;
        const std::vector<uint8_t>&       skinnedVisible;
        // Frame in flight being recorded: it chooses the copy of the pose block.
        uint32_t                          frameIndex = 0;
    };

    SkinningPass()                               = default;
    SkinningPass(const SkinningPass&)            = delete;
    SkinningPass& operator=(const SkinningPass&) = delete;

    // Set layout, pool, pipeline layout and the three compute pipelines. Only once,
    // in init: none of this depends on the size or on the samples.
    void createPipelines(const Context& ctx);
    void destroyPipelines(const Context& ctx);

    // The three dispatches per visible object, with their barriers. It goes at the start
    // of the frame's command buffer, BEFORE the shadow pass: the last
    // barrier is compute -> VERTEX_INPUT, which is what leaves the
    // outputVertexBuffer ready to be drawn.
    void record(const Context& ctx, VkCommandBuffer cmd);

    // The compute descriptor sets of each mesh come from here, allocated and
    // freed by the Renderer together with the object's SSBOs.
    VkDescriptorSetLayout descLayout() const { return m_descLayout; }

    // Allocates a set and returns the POOL it came from, which is what is needed
    // afterwards to free it (vkFreeDescriptorSets requires the specific pool). If
    // the last pool is full, it creates another and continues: before there was a single one with
    // maxSets = 16 and the 17th character made it throw in the middle of the scene
    // load.
    //
    // It grows by chaining pools instead of recreating a bigger one on purpose:
    // recreating it would invalidate the sets of all the already loaded meshes, and they would
    // all have to be rebuilt with the GPU stopped.
    //
    // Returns VK_NULL_HANDLE if not even the new pool could be created.
    VkDescriptorPool allocateSet(const Context& ctx, VkDescriptorSet& outSet);

private:
    // Creates one more pool in the chain. kSetsPerPool comes from the historical cap: most
    // scenes fit in the first one and nothing extra is paid.
    static constexpr uint32_t kSetsPerPool = 16;
    bool addPool(const Context& ctx);

    VkDescriptorSetLayout m_descLayout       = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> m_descPools;
    VkPipelineLayout      m_pipelineLayout   = VK_NULL_HANDLE;
    VkPipeline            m_boneEval         = VK_NULL_HANDLE;
    VkPipeline            m_boneHierarchy    = VK_NULL_HANDLE;
    VkPipeline            m_boneIk           = VK_NULL_HANDLE;
    VkPipeline            m_skinning         = VK_NULL_HANDLE;
};

} // namespace DonTopo
