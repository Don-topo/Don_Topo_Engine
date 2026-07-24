#include "DonTopo/Renderer/SplashScreen.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include <stb_image.h>
#include <stdexcept>
#include <fstream>
#include <vector>
#include <cstring>

namespace DonTopo {

bool loadSplashImage(const std::string& path, std::vector<uint8_t>& outRGBA,
                     int& outW, int& outH)
{
    int w = 0, h = 0, ch = 0;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &ch, STBI_rgb_alpha);
    if (!px) return false;
    outRGBA.assign(px, px + (size_t)w * h * 4);
    outW = w;
    outH = h;
    stbi_image_free(px);
    return true;
}

// ── helpers (molde: Skybox.cpp) ─────────────────────────────────────────────

static std::vector<char> loadSpv(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("SplashScreen: cannot open shader: " + path);
    size_t sz = (size_t)f.tellg();
    std::vector<char> buf(sz);
    f.seekg(0);
    f.read(buf.data(), sz);
    return buf;
}

static VkShaderModule makeModule(VkDevice dev, const std::vector<char>& code)
{
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS)
        throw std::runtime_error("SplashScreen: failed to create shader module");
    return m;
}

// ── public ───────────────────────────────────────────────────────────────────

bool SplashScreen::init(GpuDevice& gpu, VkRenderPass renderPass, VkFormat colorFormat,
                         const std::string& logoPath)
{
    (void)colorFormat; // el renderPass ya tiene el formato correcto (igual que Skybox)

    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    if (!loadSplashImage(logoPath, pixels, w, h))
        return false; // logo ausente: no se crea ningun objeto Vulkan

    m_logoW = w;
    m_logoH = h;

    VkDeviceSize imageSize = (VkDeviceSize)w * h * 4;

    // Staging buffer
    VkBuffer       staging;
    VkDeviceMemory stagingMem;
    {
        VkBufferCreateInfo ci{};
        ci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        ci.size        = imageSize;
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
    vkMapMemory(gpu.device(), stagingMem, 0, imageSize, 0, &mapped);
    memcpy(mapped, pixels.data(), (size_t)imageSize);
    vkUnmapMemory(gpu.device(), stagingMem);

    // Imagen 2D de una sola capa (sin CUBE_COMPATIBLE, arrayLayers=1)
    {
        VkImageCreateInfo ci{};
        ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.format        = VK_FORMAT_R8G8B8A8_SRGB;
        ci.extent        = { (uint32_t)w, (uint32_t)h, 1 };
        ci.mipLevels     = 1;
        ci.arrayLayers   = 1;
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

    VkImageSubresourceRange fullRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // Transition UNDEFINED -> TRANSFER_DST
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

    // Copia unica (una sola capa) — textura subida tal cual, sin flip vertical
    VkBufferImageCopy copy{};
    copy.bufferOffset      = 0;
    copy.bufferRowLength   = 0;
    copy.bufferImageHeight = 0;
    copy.imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageOffset       = { 0, 0, 0 };
    copy.imageExtent       = { (uint32_t)w, (uint32_t)h, 1 };
    vkCmdCopyBufferToImage(cmd, staging, m_image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    // Transition TRANSFER_DST -> SHADER_READ_ONLY
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

    // Image view 2D
    {
        VkImageViewCreateInfo ci{};
        ci.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.image            = m_image;
        ci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
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

    // Todo lo que sigue (descriptores + pipeline) puede lanzar
    // std::runtime_error (shader ausente/corrupto, fallo de creacion de
    // pipeline). El splash es siempre opcional: cualquier fallo aqui debe
    // resultar en init() devolviendo false, nunca en una excepcion que suba
    // hasta main(). Se libera todo lo ya creado y se reporta false.
    try {
        createDescriptors(gpu);
        createPipeline(gpu, renderPass);
    } catch (const std::exception&) {
        destroyResources(gpu);
        return false;
    }
    return true;
}

void SplashScreen::shutdown(GpuDevice& gpu)
{
    if (m_pipeline == VK_NULL_HANDLE) return;
    destroyResources(gpu);
}

void SplashScreen::destroyResources(GpuDevice& gpu)
{
    VkDevice dev = gpu.device();
    vkDestroyPipeline(dev,            m_pipeline,   nullptr);
    vkDestroyPipelineLayout(dev,      m_pipeLayout, nullptr);
    vkDestroyDescriptorPool(dev,      m_descPool,   nullptr);
    vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr);
    vkDestroySampler(dev,             m_sampler,    nullptr);
    vkDestroyImageView(dev,           m_view,       nullptr);
    vkDestroyImage(dev,               m_image,      nullptr);
    vkFreeMemory(dev,                 m_memory,     nullptr);
    m_pipeline   = VK_NULL_HANDLE;
    m_pipeLayout = VK_NULL_HANDLE;
    m_descPool   = VK_NULL_HANDLE;
    m_descLayout = VK_NULL_HANDLE;
    m_sampler    = VK_NULL_HANDLE;
    m_view       = VK_NULL_HANDLE;
    m_image      = VK_NULL_HANDLE;
    m_memory     = VK_NULL_HANDLE;
}

void SplashScreen::recordDraw(VkCommandBuffer cmd, float alpha, float screenAspect)
{
    if (!isInitialized()) return;
    struct Push { float alpha; float imgAR; float screenAR; } push{
        alpha,
        m_logoH > 0 ? (float)m_logoW / (float)m_logoH : 1.0f,
        screenAspect
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_pipeLayout, 0, 1, &m_descSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_pipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(Push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

// ── private ──────────────────────────────────────────────────────────────────

void SplashScreen::createDescriptors(GpuDevice& gpu)
{
    // Layout: set 0, binding 0 = sampler2D
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

void SplashScreen::createPipeline(GpuDevice& gpu, VkRenderPass renderPass)
{
    auto vertCode = loadSpv("shaders/splash.vert.spv");
    auto fragCode = loadSpv("shaders/splash.frag.spv");

    VkShaderModule vertMod = makeModule(gpu.device(), vertCode);
    VkShaderModule fragMod = makeModule(gpu.device(), fragCode);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName  = "main";

    // Sin vertex input — posiciones hardcoded en el vertex shader
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
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Sin depth: el splash no usa depth (render pass de Task 5 sin attachment
    // de depth para este pipeline).
    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    // Blend opaco: el alpha ya se aplica en el shader multiplicando el color.
    blendAtt.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments    = &blendAtt;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    // Push constant: 3 floats sueltos = 12 bytes, solo fragment stage
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset     = 0;
    push.size       = 3 * sizeof(float);

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount         = 1;
    layoutCI.pSetLayouts            = &m_descLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &push;
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
    pCI.pDepthStencilState  = nullptr;
    pCI.pColorBlendState    = &blend;
    pCI.pDynamicState       = &dyn;
    pCI.layout              = m_pipeLayout;
    pCI.renderPass          = renderPass;
    pCI.subpass             = 0;

    VkResult pipeResult = vkCreateGraphicsPipelines(
        gpu.device(), VK_NULL_HANDLE, 1, &pCI, nullptr, &m_pipeline);

    // Los shader modules son locales a esta funcion y solo hacen falta
    // durante la creacion del pipeline: se destruyen aqui SIEMPRE, tanto en
    // el camino de exito como en el de fallo, para que un throw no los deje
    // filtrados (vkCreateGraphicsPipelines no los consume/posee).
    vkDestroyShaderModule(gpu.device(), vertMod, nullptr);
    vkDestroyShaderModule(gpu.device(), fragMod, nullptr);

    if (pipeResult != VK_SUCCESS)
        throw std::runtime_error("SplashScreen: failed to create pipeline");
}

} // namespace DonTopo
