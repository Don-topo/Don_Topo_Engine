#pragma once
#include "DonTopo/Renderer/RenderConstants.h"
#include <vulkan/vulkan.h>
#include <cstdint>

namespace DonTopo {

class GpuDevice;

// Global IBL: two cubemaps precomputed ONCE over the skybox cubemap,
// irradiance (diffuse) and environment prefiltered by roughness (mips). The BRDF
// term is not a texture: pbr.frag uses Karis's analytic approximation, so
// there is no LUT or third binding.
//
// The images are ALWAYS created in init, with neutral content, and only
// really filled in if initSkybox() has loaded a cubemap. That way the descriptor
// sets never point at a null handle and a scene without a skybox is lit with a
// flat ambient instead of blowing up.
//
// Ties with code that is not its own:
//  - irradianceView()/prefilterView()/sampler(): written into bindings 5
//    and 6 of their descriptor sets by allocateObjectDescriptorSet and the skinned path,
//    which belong to the Renderer.
//  - the two convolution pipelines, their layout, their pool and their set layout are
//    REUSED by ReflectionProbePass to convolve each probe's capture: that is
//    why they are exposed in the public interface, by handle.
class IblPass {
public:
    // The three come from RenderConstants.h: the D3D12 backend needs them
    // IDENTICAL and had them copied with their value hardcoded. Here they are re-exposed with
    // the name this pass already used, so as not to touch its callers.
    //
    // kPrefilterMips also lives as #define IBL_PREFILTER_MIPS in
    // shaders/pbr.frag, and that third copy CANNOT be shared: a shader does not
    // include a C++ header, and putting it in the UBO block would silently shift it
    // for the six shaders that declare it (std140).
    static constexpr uint32_t kIrradianceSize = IBL_IRRADIANCE_SIZE;
    static constexpr uint32_t kPrefilterSize  = IBL_PREFILTER_SIZE;
    static constexpr uint32_t kPrefilterMips  = IBL_PREFILTER_MIPS;
    // rgba16f: the cubemaps are HDR. With 8 bits the prefiltered specular would
    // band in the smooth gradient areas.
    static constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    // Push of ibl_irradiance.comp and ibl_prefilter.comp.
    // intensity: weight that is baked into the resulting cubemap. 1.0 in the global
    // IBL (result identical to the one before the probes) and the probe's intensity
    // when this convolves its capture.
    struct Push { float roughness; uint32_t faceSize; float intensity; };

    struct Context {
        GpuDevice& gpu;
        // Skybox environment cubemap. VK_NULL_HANDLE = no environment
        // loaded: precompute() does nothing and the two cubemaps stay with
        // the neutral content that createResources() left.
        VkImageView envView;
        VkSampler   envSampler;
    };

    IblPass()                          = default;
    IblPass(const IblPass&)            = delete;
    IblPass& operator=(const IblPass&) = delete;

    // Images, views, sampler, layout, pool and the two compute pipelines, plus
    // the clear to the neutral ambient. Only once, in init.
    void createResources(const Context& ctx);
    void destroyResources(const Context& ctx);

    // Fills the two cubemaps from the skybox cubemap. No-op if there is no
    // environment. Only once, from initSkybox().
    void precompute(const Context& ctx);

    // A standalone write of bindings 5 and 6 on an ALREADY allocated set, like
    // writeSsaoBinding: rewriting them is the only thing needed for an
    // object to go from the global IBL to a probe. No new layout, no new member
    // in the UBO, no index in PushData (which is at exactly 80 bytes).
    void writeBindings(const Context& ctx, VkDescriptorSet set,
                       VkImageView irradiance, VkImageView prefilter) const;

    // The TWO IBL bindings, in a single place.
    //
    // They are written by THREE paths (the static meshes, the characters and the probe
    // pass, which swaps the global cubemap for a probe's) and each one
    // had its own copy of the block. Diverging fails nowhere: the object
    // samples another object's ambient and it is only seen by comparing captures, which is
    // exactly how H3, H65, H75 and H76 slipped through.
    //
    // It FILLS the writes, it does not send them: meshes and characters send them along with
    // their other five bindings in a single call, and splitting that into two
    // vkUpdateDescriptorSets per object would be paying to unify. `infos` is
    // provided by the caller because it has to stay alive until that call.
    static constexpr uint32_t kBindingIrradiance = 5;
    static constexpr uint32_t kBindingPrefilter  = 6;
    static void fillIblWrites(VkDescriptorSet set, VkImageView irradiance,
                              VkImageView prefilter, VkSampler sampler,
                              VkDescriptorImageInfo infos[2],
                              VkWriteDescriptorSet writes[2]);

    // The two CUBE views that go in each object's descriptor sets.
    VkImageView irradianceView() const { return m_irradianceView; }
    VkImageView prefilterView()  const { return m_prefilterView; }
    VkSampler   sampler()        const { return m_sampler; }

    // What the probe bake reuses.
    VkPipeline            irradiancePipeline() const { return m_irradiancePipeline; }
    VkPipeline            prefilterPipeline()  const { return m_prefilterPipeline; }
    VkPipelineLayout      pipelineLayout()     const { return m_pipelineLayout; }
    VkDescriptorPool      descPool()           const { return m_descPool; }
    VkDescriptorSetLayout descLayout()         const { return m_descLayout; }

private:
    VkImage        m_irradianceImage  = VK_NULL_HANDLE;
    VkDeviceMemory m_irradianceMemory = VK_NULL_HANDLE;
    // CUBE view for sampling from pbr.frag; 2D_ARRAY view so that the compute
    // can write it as a storage image (a write imageCube would require
    // capabilities that are not needed).
    VkImageView    m_irradianceView   = VK_NULL_HANDLE;
    VkImageView    m_irradianceStore  = VK_NULL_HANDLE;
    VkImage        m_prefilterImage   = VK_NULL_HANDLE;
    VkDeviceMemory m_prefilterMemory  = VK_NULL_HANDLE;
    VkImageView    m_prefilterView    = VK_NULL_HANDLE;
    VkImageView    m_prefilterStore[kPrefilterMips] {};
    VkSampler      m_sampler          = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_descLayout          = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool            = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout      = VK_NULL_HANDLE;
    VkPipeline            m_irradiancePipeline  = VK_NULL_HANDLE;
    VkPipeline            m_prefilterPipeline   = VK_NULL_HANDLE;
};

} // namespace DonTopo
