#pragma once
#include "DonTopo/Core/AnimationIk.h"
#include "DonTopo/Core/AnimationPose.h"
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace DonTopo {

    // The drawable instance types of the Vulkan backend. They lived inside
    // Renderer and moved here for a concrete reason: ReflectionProbePass
    // receives them in its Context to redraw the scene onto the six faces of
    // a probe, and putting Renderer.h inside a pass would be circular (Renderer.h already
    // includes the pass header to hold it by value).
    //
    // They are in the same namespace as Renderer, so inside the class they are
    // still named unqualified exactly as before. Same
    // move that was made for Frustum when it went out to Culling.

    // A drawable instance. It NO LONGER owns GPU resources: buffers,
    // textures and descriptor set live in the shared entry that
    // sharedIndex points to, and N objects with the same mesh+material
    // all point to the same one. The only per-instance thing is the transform
    // (and the name, which is for debugging).
    struct RenderObject
    {
        std::string     name;
        // -1 = no resources (never built, or already freed from the
        // editor). It is the check that replaces the old
        // "vertexBuffer == VK_NULL_HANDLE".
        int             sharedIndex         = -1;
        glm::mat4       transform{1.0f};
        // 0 = does not reflect. Synchronized by the application loop from
        // the GameObject, same as the transform.
        float           ssrStrength         = 0.0f;
        // PBR factors PER OBJECT. They lived in the shared entry
        // (SharedGpuMesh), which is what forced re-keying the object and
        // rebuilding its GPU resources to move a slider: changing a number
        // changed the dedup key. Here moving them costs nothing (they travel by
        // push constant, like the transform) and incidentally two objects with the same
        // mesh and different finish share the VRAM.
        //
        // With an ORM map both go to 1.0 and the texture rules: the shader
        // multiplies. That decision is made by whoever registers the object, not the draw
        // pass, which no longer has the material in front of it.
        float           metallic            = 0.0f;
        float           roughness           = 0.5f;
        // false = skipped by the scene, shadow and AO passes: the
        // mesh is not sent to the GPU, so it neither casts nor occludes.
        bool            meshVisible         = true;
    };

    struct SkinnedMatGfx {
        VkImage         textureImage  = VK_NULL_HANDLE;
        VkDeviceMemory  textureMem    = VK_NULL_HANDLE;
        VkImageView     textureView   = VK_NULL_HANDLE;
        VkSampler       sampler       = VK_NULL_HANDLE;
        VkImage         normalImage   = VK_NULL_HANDLE;
        VkDeviceMemory  normalMem     = VK_NULL_HANDLE;
        VkImageView     normalView    = VK_NULL_HANDLE;
        VkSampler       normalSampler = VK_NULL_HANDLE;
        VkImage         ormImage      = VK_NULL_HANDLE;
        VkDeviceMemory  ormMem        = VK_NULL_HANDLE;
        VkImageView     ormView       = VK_NULL_HANDLE;
        VkSampler       ormSampler    = VK_NULL_HANDLE;
        float           metallic      = 0.0f;
        float           roughness     = 0.5f;
        VkDescriptorSet descSets[2]   = {};
        // Pool they came from; see SharedGpuMesh::descPool.
        VkDescriptorPool descPool     = VK_NULL_HANDLE;
    };

    struct SubMeshDraw {
        uint32_t indexStart;
        uint32_t indexCount;
        uint32_t materialIndex;
    };

    struct PushData {
        glm::mat4 transform{1.0f};
        float     metallic  = 1.0f;
        float     roughness = 1.0f;
        // flags.x = 1: triangle.vert takes the model matrix from the instance
        // SSBO by gl_InstanceIndex (static, grouped path);
        // 0: it takes it from `transform` (skinned path, which shares this
        // vertex shader and draws one instance with its own matrix).
        // It is the old _pad reused: same type and offset, so
        // pbr.frag keeps declaring the block the same as always.
        glm::vec2 flags{0.0f, 0.0f};
    };
    static_assert(sizeof(PushData) == 80, "PushData must be 80 bytes");

    struct SkinnedRenderObject {
        std::string    name;
        // static SSBOs
        VkBuffer       keyframePosBuffer    = VK_NULL_HANDLE;
        VkDeviceMemory keyframePosMemory    = VK_NULL_HANDLE;
        VkBuffer       keyframeRotBuffer    = VK_NULL_HANDLE;
        VkDeviceMemory keyframeRotMemory    = VK_NULL_HANDLE;
        VkBuffer       keyframeScaleBuffer  = VK_NULL_HANDLE;
        VkDeviceMemory keyframeScaleMemory  = VK_NULL_HANDLE;
        VkBuffer       boneInfoBuffer       = VK_NULL_HANDLE;
        VkDeviceMemory boneInfoMemory       = VK_NULL_HANDLE;
        VkBuffer       inputVertexBuffer    = VK_NULL_HANDLE;
        VkDeviceMemory inputVertexMemory    = VK_NULL_HANDLE;
        // dynamic SSBOs (written by compute)
        VkBuffer       localTransformBuffer = VK_NULL_HANDLE;
        VkDeviceMemory localTransformMemory = VK_NULL_HANDLE;
        VkBuffer       finalBoneBuffer      = VK_NULL_HANDLE;
        VkDeviceMemory finalBoneMemory      = VK_NULL_HANDLE;
        // Output vertex buffer (also used as VB in graphics)
        VkBuffer       outputVertexBuffer   = VK_NULL_HANDLE;
        VkDeviceMemory outputVertexMemory   = VK_NULL_HANDLE;
        // Index buffer
        VkBuffer       indexBuffer          = VK_NULL_HANDLE;
        VkDeviceMemory indexMemory          = VK_NULL_HANDLE;
        uint32_t       indexCount           = 0;
        uint32_t       vertexCount          = 0;
        uint32_t       boneCount            = 0;
        // No. of clips concatenated in the keyframe SSBOs. Only used
        // to clamp in setAnimationState (Task 3): a clipIndex out of
        // range would make clipBase point outside the BoneInfos SSBO and
        // the compute read garbage with nothing warning about it.
        uint32_t       clipCount            = 1;
        // Descriptor set de compute
        VkDescriptorSet computeDescSet      = VK_NULL_HANDLE;
        // And the pool it came from. SkinningPass chains pools as needed,
        // so to free the set you have to remember WHICH one it was:
        // vkFreeDescriptorSets asks for the specific pool, the last one will not do.
        VkDescriptorPool computeDescPool    = VK_NULL_HANDLE;
        // Textures and descriptor sets per material
        std::vector<SkinnedMatGfx>  matGfx;
        std::vector<SubMeshDraw>    subMeshes;
        // SSR strength of the object, synchronized from the GameObject
        // like the transform. The skinned path draws one instance
        // per submesh, so there is no grouping to split here.
        float          ssrStrength          = 0.0f;
        // false = skipped by the scene pass and the shadow pass. The skinning
        // compute does keep running: the selection outline reads
        // its output vertices.
        bool           meshVisible          = true;
        // Animation state
        float     animTime       = 0.0f;
        // Index of the clip evaluated this frame. The others reside in the
        // SSBO and are not read.
        uint32_t  activeClip     = 0;
        // The pose an Animator sends (setAnimationPose): up to 6 samples and
        // the frozen one. Without it (hasPose false), bone_eval evaluates a single
        // sample: activeClip at animTime, which is the updateAnimation path.
        AnimationPose pose;
        bool          hasPose        = false;
        // TRS of the resulting pose (written by bone_eval) and the frozen copy
        // when a fade is interrupted. 3 vec4 per bone.
        VkBuffer       poseTrsBuffer        = VK_NULL_HANDLE;
        VkDeviceMemory poseTrsMemory        = VK_NULL_HANDLE;
        VkBuffer       frozenTrsBuffer      = VK_NULL_HANDLE;
        VkDeviceMemory frozenTrsMemory      = VK_NULL_HANDLE;
        // Pose block (PoseBlock.h), one copy per frame in flight, persistently
        // mapped: written by SkinningPass::record every frame.
        VkBuffer       poseBlockBuffer      = VK_NULL_HANDLE;
        VkDeviceMemory poseBlockMemory      = VK_NULL_HANDLE;
        void*          poseBlockMapped      = nullptr;
        // Copy of the pose masks: the Animator's one is only valid during
        // setAnimationPose.
        std::vector<uint8_t> poseMasks[kMaxLayersPose];
        // IK: what the Animator sends (already in model space) and its block
        // for the GPU, with one copy per frame in flight.
        AnimationIk    ik;
        VkBuffer       ikBlockBuffer  = VK_NULL_HANDLE;
        VkDeviceMemory ikBlockMemory  = VK_NULL_HANDLE;
        void*          ikBlockMapped  = nullptr;
        float     duration       = 0.0f;
        float     ticksPerSecond = 24.0f;
        glm::mat4 transform      {1.0f};
        // Bound for frustum culling: sphere centered on the local
        // origin, valid in every pose (see skinnedBoundRadius).
        // hasBounds false = mesh with nothing to bound it with -> it is never
        // culled, which is the safe side.
        float     boundRadius    = 0.0f;
        bool      hasBounds      = false;
        // Largest side of the AABB of the REST pose, in local space.
        // Only used by the selection outline thickness, which is
        // proportional to the object's size: boundRadius is no good there
        // because it bounds all poses and comes out several times larger than the
        // mesh. 0 = mesh without vertices (the outline falls to its minimum).
        float     restMaxExtent  = 0.0f;
        // 0 = uploaded and visible. >0 = waiting for the fence of the batch
        // with that ticket to signal. Without this, the object would be drawn with
        // its textures still in TRANSFER_DST_OPTIMAL.
        uint64_t  uploadTicket   = 0;
    };

} // namespace DonTopo
