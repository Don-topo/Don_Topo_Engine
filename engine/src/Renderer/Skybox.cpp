#include "DonTopo/Renderer/Skybox.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include <stb_image.h>
#include <stdexcept>
#include <fstream>
#include <vector>
#include <cstring>
#include <glm/glm.hpp>
#include "DonTopo/Renderer/ShaderModule.h"

namespace DonTopo {

// ── helpers ──────────────────────────────────────────────────────────────────

// ── public ────────────────────────────────────────────────────────────────────

void Skybox::init(GpuDevice& gpu, VkRenderPass renderPass, VkFormat colorFormat,
                  const std::array<std::string, 6>& facePaths,
                  VkSampleCountFlagBits samples)
{
    // Re-entry: without this, a second call overwrote image, memory, view,
    // sampler, pool, layout and pipeline without destroying anything: a whole cubemap
    // leaked per sky change. Today it does not happen (initSkybox is called only once,
    // from the sandbox and runtime startup), but IblPass is already
    // written so that it can be called again (see the "Reset and not free" in
    // IblPass::precompute) and this was the missing half.
    //
    // The wait goes here and not in the caller because this is the one that cannot know: it is
    // paid only when there is something to destroy, so at startup it costs
    // zero.
    if (isInitialized()) {
        vkDeviceWaitIdle(gpu.device());
        shutdown(gpu);
    }

    loadCubemap(gpu, facePaths);
    createDescriptors(gpu);
    createPipeline(gpu, renderPass, colorFormat, samples);
}

void Skybox::recreatePipeline(GpuDevice& gpu, VkRenderPass renderPass, VkSampleCountFlagBits samples)
{
    // With no cubemap loaded there is nothing to recreate: the skybox is not active.
    if (m_pipeline == VK_NULL_HANDLE) return;
    vkDestroyPipeline(gpu.device(), m_pipeline, nullptr);
    m_pipeline = VK_NULL_HANDLE;
    // The format still comes from the render pass; it is passed for symmetry with init.
    createPipeline(gpu, renderPass, VK_FORMAT_UNDEFINED, samples);
}

void Skybox::shutdown(GpuDevice& gpu)
{
    if (m_pipeline   == VK_NULL_HANDLE) return;
    VkDevice dev = gpu.device();
    vkDestroyPipeline(dev,            m_pipeline,   nullptr);
    vkDestroyPipelineLayout(dev,      m_pipeLayout, nullptr);
    vkDestroyDescriptorPool(dev,      m_descPool,   nullptr);
    vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr);
    vkDestroySampler(dev,             m_sampler,    nullptr);
    vkDestroyImageView(dev,           m_view,       nullptr);
    vkDestroyImage(dev,               m_image,      nullptr);
    vkFreeMemory(dev,                 m_memory,     nullptr);

    // ALL to null, not just the pipeline. While this was only the final shutdown
    // it did not matter, but init() now calls here to reset itself: if a
    // reload failed halfway, the old handles would keep looking
    // valid and the real shutdown would destroy them a second time.
    m_pipeline   = VK_NULL_HANDLE;
    m_pipeLayout = VK_NULL_HANDLE;
    m_descPool   = VK_NULL_HANDLE;
    m_descSet    = VK_NULL_HANDLE;
    m_descLayout = VK_NULL_HANDLE;
    m_sampler    = VK_NULL_HANDLE;
    m_view       = VK_NULL_HANDLE;
    m_image      = VK_NULL_HANDLE;
    m_memory     = VK_NULL_HANDLE;
}

void Skybox::draw(VkCommandBuffer cmd, const glm::mat4& invViewProj)
{
    if (!isInitialized()) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_pipeLayout, 0, 1, &m_descSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_pipeLayout, VK_SHADER_STAGE_VERTEX_BIT,
        0, sizeof(glm::mat4), &invViewProj);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

// ── private ───────────────────────────────────────────────────────────────────

void Skybox::loadCubemap(GpuDevice& gpu, const std::array<std::string, 6>& facePaths)
{
    int w = 0, h = 0;
    std::array<stbi_uc*, 6> pixels{};

    // The loaded faces are released no matter what. The two error exits
    // of this loop are the MOST frequent there are (a misspelled path, or six
    // images the user did not crop to the same size) and both threw
    // leaving everything before them allocated: up to five faces at 2048x2048 are 80
    // MB of leak per attempt, and the editor survives the exception, so it
    // could be repeated (H30).
    struct FreeFaces {
        std::array<stbi_uc*, 6>& p;
        bool armado = true;
        ~FreeFaces()
        {
            if (!armado) return;
            for (stbi_uc*& f : p) { if (f) stbi_image_free(f); f = nullptr; }
        }
    } guard{pixels};

    for (int i = 0; i < 6; i++) {
        int iw, ih, ch;
        pixels[i] = stbi_load(facePaths[i].c_str(), &iw, &ih, &ch, STBI_rgb_alpha);
        if (!pixels[i])
            throw std::runtime_error("Skybox: failed to load face: " + facePaths[i]);
        if (i == 0) { w = iw; h = ih; }
        else if (iw != w || ih != h)
            // With the sizes: "face size mismatch" did not say WHICH one is off or by
            // how much, and the fix is to crop the image.
            throw std::runtime_error("Skybox: face " + std::to_string(i) + " measures " +
                                     std::to_string(iw) + "x" + std::to_string(ih) +
                                     " and the previous ones " + std::to_string(w) + "x" +
                                     std::to_string(h) + ": " + facePaths[i]);
    }

    VkDeviceSize faceSize  = (VkDeviceSize)w * h * 4;
    VkDeviceSize totalSize = faceSize * 6;

    // Staging buffer
    VkBuffer       staging;
    VkDeviceMemory stagingMem;
    {
        VkBufferCreateInfo ci{};
        ci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        ci.size        = totalSize;
        ci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(gpu.device(), &ci, nullptr, &staging);

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(gpu.device(), staging, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = gpu.findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(gpu.device(), &ai, nullptr, &stagingMem);
        vkBindBufferMemory(gpu.device(), staging, stagingMem, 0);
    }

    void* mapped;
    vkMapMemory(gpu.device(), stagingMem, 0, totalSize, 0, &mapped);
    // Good path: they are copied and released here, so the guard is disarmed.
    // From this point on there is nothing of its own left to release.
    for (int i = 0; i < 6; i++) {
        memcpy((uint8_t*)mapped + i * faceSize, pixels[i], (size_t)faceSize);
        stbi_image_free(pixels[i]);
        pixels[i] = nullptr;
    }
    guard.armado = false;
    vkUnmapMemory(gpu.device(), stagingMem);

    // Cubemap image (6 array layers + CUBE_COMPATIBLE flag)
    {
        VkImageCreateInfo ci{};
        ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.format        = VK_FORMAT_R8G8B8A8_SRGB;
        ci.extent        = { (uint32_t)w, (uint32_t)h, 1 };
        ci.mipLevels     = 1;
        ci.arrayLayers   = 6;
        ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ci.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCreateImage(gpu.device(), &ci, nullptr, &m_image);

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(gpu.device(), m_image, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = gpu.findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(gpu.device(), &ai, nullptr, &m_memory);
        vkBindImageMemory(gpu.device(), m_image, m_memory, 0);
    }

    VkImageSubresourceRange fullRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };

    // Transition UNDEFINED → TRANSFER_DST
    VkCommandBuffer cmd = gpu.beginOneTimeCommands();
    {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = m_image;
        b.subresourceRange    = fullRange;
        b.srcAccessMask       = 0;
        b.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // Copy each face to the corresponding array layer
    std::array<VkBufferImageCopy, 6> copies{};
    for (int i = 0; i < 6; i++) {
        copies[i].bufferOffset      = faceSize * (VkDeviceSize)i;
        copies[i].bufferRowLength   = 0;
        copies[i].bufferImageHeight = 0;
        copies[i].imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)i, 1 };
        copies[i].imageOffset       = { 0, 0, 0 };
        copies[i].imageExtent       = { (uint32_t)w, (uint32_t)h, 1 };
    }
    vkCmdCopyBufferToImage(cmd, staging, m_image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, copies.data());

    // Transition TRANSFER_DST → SHADER_READ_ONLY
    {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = m_image;
        b.subresourceRange    = fullRange;
        b.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }
    gpu.endOneTimeCommands(cmd);

    vkDestroyBuffer(gpu.device(), staging, nullptr);
    vkFreeMemory(gpu.device(), stagingMem, nullptr);

    // Image view (CUBE)
    {
        VkImageViewCreateInfo ci{};
        ci.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.image            = m_image;
        ci.viewType         = VK_IMAGE_VIEW_TYPE_CUBE;
        ci.format           = VK_FORMAT_R8G8B8A8_SRGB;
        ci.subresourceRange = fullRange;
        vkCreateImageView(gpu.device(), &ci, nullptr, &m_view);
    }

    // Sampler
    {
        VkSamplerCreateInfo ci{};
        ci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ci.magFilter    = VK_FILTER_LINEAR;
        ci.minFilter    = VK_FILTER_LINEAR;
        ci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.maxLod       = 1.0f;
        vkCreateSampler(gpu.device(), &ci, nullptr, &m_sampler);
    }
}

void Skybox::createDescriptors(GpuDevice& gpu)
{
    // Layout: set 0, binding 0 = samplerCube
    VkDescriptorSetLayoutBinding b{};
    b.binding         = 0;
    b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 1;
    lci.pBindings    = &b;
    vkCreateDescriptorSetLayout(gpu.device(), &lci, nullptr, &m_descLayout);

    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pci{};
    pci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets       = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes    = &ps;
    vkCreateDescriptorPool(gpu.device(), &pci, nullptr, &m_descPool);

    VkDescriptorSetAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = m_descPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &m_descLayout;
    vkAllocateDescriptorSets(gpu.device(), &ai, &m_descSet);

    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler     = m_sampler;
    imgInfo.imageView   = m_view;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet w{};
    w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet          = m_descSet;
    w.dstBinding      = 0;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(gpu.device(), 1, &w, 0, nullptr);
}

void Skybox::createPipeline(GpuDevice& gpu, VkRenderPass renderPass, VkFormat colorFormat,
                            VkSampleCountFlagBits samples)
{
    (void)colorFormat; // the renderPass already has the right format

    VkShaderModule vertMod = loadShaderModule(gpu.device(), "shaders/skybox.vert.spv");
    VkShaderModule fragMod = loadShaderModule(gpu.device(), "shaders/skybox.frag.spv");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName  = "main";

    // No vertex input — positions hardcoded in the vertex shader
    VkPipelineVertexInputStateCreateInfo vtxInput{};
    vtxInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpState{};
    vpState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpState.viewportCount = 1;
    vpState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rast{};
    rast.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.cullMode    = VK_CULL_MODE_NONE;
    rast.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rast.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    // It is set by the Renderer's anti-aliasing mode: the skybox is drawn in the
    // scene pass and has to declare the same samples as it.
    ms.rasterizationSamples = samples;

    // Depth: LEQUAL test (z=1.0 at the far plane), no write
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments    = &blendAtt;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    // Push constant: mat4 = 64 bytes, solo vertex stage
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.offset     = 0;
    push.size       = sizeof(glm::mat4);

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount         = 1;
    layoutCI.pSetLayouts            = &m_descLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &push;
    // When the pipeline is recreated due to a change of samples (MSAA) the layout does not
    // change: creating it again here would orphan the previous one, and that is only
    // seen at shutdown, as one undestroyed VkPipelineLayout per change.
    if (m_pipeLayout == VK_NULL_HANDLE)
        vkCreatePipelineLayout(gpu.device(), &layoutCI, nullptr, &m_pipeLayout);

    VkGraphicsPipelineCreateInfo pCI{};
    pCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pCI.stageCount          = 2;
    pCI.pStages             = stages;
    pCI.pVertexInputState   = &vtxInput;
    pCI.pInputAssemblyState = &ia;
    pCI.pViewportState      = &vpState;
    pCI.pRasterizationState = &rast;
    pCI.pMultisampleState   = &ms;
    pCI.pDepthStencilState  = &ds;
    pCI.pColorBlendState    = &blend;
    pCI.pDynamicState       = &dyn;
    pCI.layout              = m_pipeLayout;
    pCI.renderPass          = renderPass;
    pCI.subpass             = 0;

    if (vkCreateGraphicsPipelines(gpu.device(), VK_NULL_HANDLE, 1, &pCI, nullptr, &m_pipeline)
            != VK_SUCCESS)
        throw std::runtime_error("Skybox: failed to create pipeline");

    vkDestroyShaderModule(gpu.device(), vertMod, nullptr);
    vkDestroyShaderModule(gpu.device(), fragMod, nullptr);
}

} // namespace DonTopo
