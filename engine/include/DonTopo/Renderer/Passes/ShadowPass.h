#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include "DonTopo/Renderer/UniformBufferObject.h"

namespace DonTopo {

class GpuDevice;
struct Light;

// Cascaded shadow map: the texture array (one layer per cascade), its comparison
// sampler, the depth-only render pass, the per-layer framebuffers, the two
// pipelines (static and skinned) and the cascade split.
//
// Same split as DepthPrepassPass: this class owns the TARGET and the
// PIPELINES; the DRAWS stay in the Renderer, between beginCascade() and
// endCascade(), because they come from its object lists and its instance
// SSBO — and that SSBO's cursor is shared by the shadow pass, the
// depth pre-pass and the scene pass.
//
// Ties with code that is not its own:
//  - view()/sampler(): go to binding 3 of each object descriptor set, which
//    are written by allocateObjectDescriptorSet and the skinned path, and to the fog's
//    Context.
//  - pipelineLayout(): it is LENT to the depth pre-pass, which declares the same two
//    sets and does not use the push constant.
//  - cascadeMatrices()/cascadeSplits(): copied by updateUniformBuffer into the UBO,
//    which is where pbr.frag reads them.
class ShadowPass {
public:
    // Default side of the map, in texels. The number of cascades is
    // SHADOW_CASCADES and lives in UniformBufferObject.h: it has to have the same value
    // there, in the shaders' UBO block array and in the layers of this
    // texture array.
    static constexpr uint32_t kShadowSize = 2048;

    // The side being used right now. Changed by resizeResources.
    uint32_t size() const { return m_size; }

    struct Context {
        GpuDevice& gpu;
        // The two sets that shadow.vert declares: the object's (UBO + textures,
        // although this shader only reads the UBO) and the instance SSBO. Both
        // belong to the Renderer; the pipeline layout that comes out of them is what
        // the depth pre-pass then borrows.
        VkDescriptorSetLayout objectSetLayout;
        VkDescriptorSetLayout instanceSetLayout;
    };

    ShadowPass()                             = default;
    ShadowPass(const ShadowPass&)            = delete;
    ShadowPass& operator=(const ShadowPass&) = delete;

    // Image, views, sampler, render pass, framebuffers and the two pipelines.
    // Only once, in init. None of this depends on the window size.
    void createResources(const Context& ctx);
    void destroyResources(const Context& ctx);

    // Rebuilds ONLY what depends on the map's side: image, memory, the views
    // that hang from it and the framebuffers. It leaves standing the sampler, the render
    // pass, the pipelines and the PIPELINE LAYOUT, and the latter is not a detail:
    // the depth pre-pass borrows that layout (see Renderer::init), so
    // destroying it would force rebuilding that pass too.
    //
    // No pipeline bakes in the size: viewport and scissor are dynamic
    // state.
    //
    // It must be called with the GPU IDLE, and whoever calls it has to
    // afterwards rewrite binding 3 of ALL the descriptor sets: it points to the view that
    // is destroyed here.
    void resizeResources(const Context& ctx, uint32_t size);

    // Cascade split and matrix of each one, from the frame camera's
    // frustum and the first light. Once per frame, BEFORE
    // updateUniformBuffer and recording: both consumers read the cache.
    // Without lights (or with a degenerate camera) it leaves identity and splits at 0, which
    // is what turns off the sampling in the shader.
    // maxDistance and lambda come from RendererState (shadowDistance() and
    // cascadeLambda()): they were compile-time constants and now the
    // user picks them. They go as parameters and not through the Context because this pass does not
    // know the Renderer, and the split is the only thing of its own that depends on them.
    // sceneCenter is where a POINT light aims, since it has no direction
    // of its own. It arrives as a parameter for the same reason: the pass does not see the
    // scene's objects. The Renderer computes it and passes it identically to the
    // fog, which needs the SAME light direction as this shadow map.
    void computeCascades(const glm::mat4& view, const glm::mat4& proj,
                         const std::vector<Light>& lights,
                         float maxDistance, float lambda,
                         const glm::vec3& sceneCenter);

    // Opens the render pass of one cascade with the viewport, the scissor, the
    // static pipeline and the index push already set. Between this and
    // endCascade() the Renderer records its draws.
    //
    // It is ALWAYS called, also without lights: it is this render pass that clears the
    // layer and leaves it in DEPTH_STENCIL_READ_ONLY_OPTIMAL, which is the layout that
    // the descriptor sets declare. What the Renderer skips in that case is
    // the draws.
    void beginCascade(VkCommandBuffer cmd, uint32_t cascade);
    // Switches to the skinned meshes' pipeline within the same cascade. The
    // layout is the same, so the index push survives the switch; it is
    // rewritten so as not to depend on that compatibility.
    void bindSkinnedPipeline(VkCommandBuffer cmd, uint32_t cascade);
    void endCascade(VkCommandBuffer cmd);

    // View of the whole array: the one sampled by pbr.frag (sampler2DArrayShadow)
    // and the one that goes in the descriptor sets.
    VkImageView      view()           const { return m_view; }
    VkSampler        sampler()        const { return m_sampler; }
    VkPipelineLayout pipelineLayout() const { return m_pipelineLayout; }

    const glm::mat4* cascadeMatrices() const { return m_cascadeMatrices; }  // [SHADOW_MATRICES]
    const glm::mat4& cascadeMatrix(uint32_t c) const { return m_cascadeMatrices[c]; }
    const glm::vec4& cascadeSplits()   const { return m_cascadeSplits; }

    // Layers that the KEY light leaves valid in this frame, starting from 0: 4 with the
    // cascades of a directional, 6 with the cubemap of a point light or a very wide
    // spot, 1 with the face of a normal spot, 0 without lights.
    //
    // It only bounds the DRAWS. The render passes have to be opened anyway on ALL the
    // layers, because they are the ones that clear them and leave them in the layout that
    // the descriptor sets declare; an unopened layer stays in UNDEFINED and
    // the validation layer complains every frame.
    uint32_t activeLayers() const { return m_activeLayers; }

    // Secondary spots that got a slot. They occupy the layers
    // [SHADOW_KEY_MATRICES, SHADOW_KEY_MATRICES + extraLayers).
    uint32_t extraLayers() const { return m_extraLayers; }

    // Slot of each light, or -1 if it casts no shadow. Slot 0 is always -1: the key uses
    // the first SHADOW_KEY_MATRICES and does not go through here. The Renderer reads it
    // to tell the shader through position.w.
    const int* shadowSlots() const { return m_shadowSlot; }
    // Layers that each light took: 1 one face, SHADOW_KEY_MATRICES a cubemap,
    // 0 if it casts no shadow. The UBO needs it to tell the shader which path to
    // sample through, without the shader deducing it on its own.
    const int* shadowFaceCounts() const { return m_shadowFaces; }

private:
    VkImage        m_image                          = VK_NULL_HANDLE;
    VkDeviceMemory m_memory                         = VK_NULL_HANDLE;
    VkImageView    m_view                           = VK_NULL_HANDLE;
    // A view of ONE layer per cascade. They only exist to be able to hang
    // a framebuffer from each layer; nobody samples them.
    // What depends on the size, kept apart so it can be rebuilt on its own (see
    // resizeResources).
    void createSizedResources(const Context& ctx);
    void destroySizedResources(const Context& ctx);
    void createFramebuffers(const Context& ctx);

    uint32_t       m_size                          = kShadowSize;
    VkImageView    m_layerViews[SHADOW_MATRICES]    {};
    VkSampler      m_sampler                        = VK_NULL_HANDLE;
    VkRenderPass   m_renderPass                     = VK_NULL_HANDLE;
    VkFramebuffer  m_framebuffers[SHADOW_MATRICES]  {};
    VkPipeline     m_pipeline                       = VK_NULL_HANDLE;
    // Sibling of the previous one for the skinned meshes: same shadow.vert, same
    // layout, same render pass and same bias. Only the vertex input changes,
    // because what is drawn is the output of the skinning compute (5xvec4,
    // stride 80) and the stride is pipeline state.
    VkPipeline       m_skinnedPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout  = VK_NULL_HANDLE;

    // Per-frame cache filled by computeCascades(). Identity and 0 if the scene
    // has no lights: in that case the pass only clears the layers.
    glm::mat4 m_cascadeMatrices[SHADOW_MATRICES] { glm::mat4(1.0f), glm::mat4(1.0f),
                                                   glm::mat4(1.0f), glm::mat4(1.0f),
                                                   glm::mat4(1.0f), glm::mat4(1.0f) };
    glm::vec4 m_cascadeSplits { 0.0f };
    uint32_t  m_activeLayers = 0;
    uint32_t  m_extraLayers  = 0;
    int       m_shadowSlot[MAX_LIGHTS] = {};
    int       m_shadowFaces[MAX_LIGHTS] = {};
};

} // namespace DonTopo
