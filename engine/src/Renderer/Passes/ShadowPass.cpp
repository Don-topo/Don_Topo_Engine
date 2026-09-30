#include "DonTopo/Renderer/Passes/ShadowPass.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/Mesh.h"
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>
#include <fstream>
#include <string>
#include <vector>
#include <cmath>
#include <cstdint>
#include "DonTopo/Renderer/ShaderModule.h"

namespace DonTopo {

// ── helpers ──────────────────────────────────────────────────────────────────

// The shadow reach and the split of the cuts between cascades were
// constants here (kShadowMaxDistance = 500 and kCascadeLambda = 0.75). Now
// the user picks them and they arrive as a parameter to computeCascades; what they
// mean and why those defaults is documented in RendererState.h, next
// to shadowDistance() and cascadeLambda(). The default values there are
// exactly the ones that were here, so the image does not change on its own.


// ── resources ────────────────────────────────────────────────────────────────

void ShadowPass::createSizedResources(const Context& ctx)
{
    // 1. Depth image for the shadow map: a texture array with one layer per
    // cascade. It does not use m_res.createImage because that one fixes arrayLayers to 1 and
    // the signature is shared by all the engine's textures.
    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.format        = VK_FORMAT_D32_SFLOAT;
    imageInfo.extent        = { m_size, m_size, 1 };
    imageInfo.mipLevels     = 1;
    imageInfo.arrayLayers   = SHADOW_MATRICES;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(ctx.gpu.device(), &imageInfo, nullptr, &m_image) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create shadow image!");
    }

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(ctx.gpu.device(), m_image, &memReq);
    VkMemoryAllocateInfo memAlloc{};
    memAlloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memAlloc.allocationSize  = memReq.size;
    memAlloc.memoryTypeIndex = ctx.gpu.findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(ctx.gpu.device(), &memAlloc, nullptr, &m_memory) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to allocate shadow image memory!");
    }
    vkBindImageMemory(ctx.gpu.device(), m_image, m_memory, 0);

    // 2. Image views: one of the whole array for sampling, and one per layer
    // to hang a framebuffer from.
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                          = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                          = m_image;
    viewInfo.viewType                       = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format                         = VK_FORMAT_D32_SFLOAT;
    viewInfo.subresourceRange.aspectMask    = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.layerCount    = SHADOW_MATRICES;
    viewInfo.subresourceRange.levelCount    = 1;
    if(vkCreateImageView(ctx.gpu.device(), &viewInfo, nullptr, &m_view) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create shadow image view!");
    }

    for (uint32_t c = 0; c < SHADOW_MATRICES; c++)
    {
        VkImageViewCreateInfo layerInfo = viewInfo;
        layerInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        layerInfo.subresourceRange.baseArrayLayer = c;
        layerInfo.subresourceRange.layerCount     = 1;
        if (vkCreateImageView(ctx.gpu.device(), &layerInfo, nullptr, &m_layerViews[c]) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create shadow layer view!");
        }
    }
}

void ShadowPass::createResources(const Context& ctx)
{
    createSizedResources(ctx);

    // 3. Comparison sampler (PCF ready)
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter               = VK_FILTER_LINEAR;
    samplerInfo.minFilter               = VK_FILTER_LINEAR;
    samplerInfo.addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.borderColor             = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.compareEnable           = VK_TRUE;
    samplerInfo.compareOp               = VK_COMPARE_OP_LESS_OR_EQUAL;
    samplerInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    if(vkCreateSampler(ctx.gpu.device(), &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create shadow sampler!");
    }

    // 4. Render pass depth-only
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format         = VK_FORMAT_D32_SFLOAT;
    depthAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthAttachmentRef{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthAttachmentRef;

    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass      = 0;
    dependencies[0].srcStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask    = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask   = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags  = VK_DEPENDENCY_BY_REGION_BIT;
    dependencies[1].srcSubpass      = 0;
    dependencies[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask    = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask   = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dependencyFlags  = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType            = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount   = 1;
    renderPassInfo.pAttachments      = &depthAttachment;
    renderPassInfo.subpassCount      = 1;
    renderPassInfo.pSubpasses        = &subpass;
    renderPassInfo.dependencyCount   = 2;
    renderPassInfo.pDependencies     = dependencies;
    if(vkCreateRenderPass(ctx.gpu.device(), &renderPassInfo, nullptr, &m_renderPass) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create shadow render pass!");
    }

     // 5. Framebuffers: one per cascade, each over its layer. All of them
     // share the render pass (the attachment format is the same).
     createFramebuffers(ctx);

    // 6. Pipeline (vertex-only, no color attachments)
    VkShaderModule vertModule = loadShaderModule(ctx.gpu.device(), "shaders/shadow.vert.spv");

    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    vertStage.pName  = "main";

    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding   = 0;
    bindingDesc.stride    = sizeof(Vertex);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrDesc{};
    attrDesc.binding  = 0;
    attrDesc.location = 0;
    attrDesc.format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrDesc.offset   = offsetof(Vertex, pos);

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions    = &attrDesc;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode             = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode                = VK_CULL_MODE_NONE;
    rasterizer.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth               = 1.0f;
    rasterizer.depthBiasEnable         = VK_TRUE;
    rasterizer.depthBiasConstantFactor = 1.25f;
    rasterizer.depthBiasSlopeFactor    = 1.75f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 0; // no color attachments

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynStates;

    // The model matrix does NOT go through a push constant: shadow.vert takes it from the instance
    // SSBO (set 1) by gl_InstanceIndex, like triangle.vert.
    // The only push constant is the cascade index, which says which of the UBO's
    // matrices to use. This layout is the shadow pass's own and no other
    // pipeline shares it, so the PushData range that
    // triangle/pbr/outline use is not touched. The depth pre-pass does borrow it:
    // it declares the same two sets and does not use the push.
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.offset     = 0;
    pcr.size       = sizeof(uint32_t);

    VkDescriptorSetLayout setLayouts[] = { ctx.objectSetLayout, ctx.instanceSetLayout };

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 2;
    layoutInfo.pSetLayouts            = setLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(ctx.gpu.device(), &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create shadow pipeline layout!");
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount          = 1;
    pipelineInfo.pStages             = &vertStage;
    pipelineInfo.pVertexInputState   = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState   = &multisampling;
    pipelineInfo.pDepthStencilState  = &depthStencil;
    pipelineInfo.pColorBlendState    = &colorBlend;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = m_pipelineLayout;
    pipelineInfo.renderPass          = m_renderPass;
    if (vkCreateGraphicsPipelines(ctx.gpu.device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create shadow pipeline!");
    }

    // Variant for the skinned meshes. All the state is copied from the one
    // above (same bias, same depth, same cascades, same layout), so
    // the static meshes' shadow does not change. The only difference is the
    // vertex input.
    //
    // stride 80, not sizeof(SkinnedVertex): that is the compute's INPUT vertex
    // (7×vec4, with bone indices and weights). What is drawn here
    // is its OUTPUT, the OutputVertex of skinning.comp, which is 5×vec4 and carries
    // the position in the first one. It is the same stride that the main pass's
    // skinned pipeline declares.
    VkVertexInputBindingDescription skinnedBinding{};
    skinnedBinding.binding   = 0;
    skinnedBinding.stride    = 5 * (uint32_t)sizeof(glm::vec4);  // 80 bytes
    skinnedBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // pos is a vec4 (the compute's std430); shadow.vert only declares vec3, and
    // reading 3 of the 4 floats is legal.
    VkVertexInputAttributeDescription skinnedAttr{};
    skinnedAttr.binding  = 0;
    skinnedAttr.location = 0;
    skinnedAttr.format   = VK_FORMAT_R32G32B32_SFLOAT;
    skinnedAttr.offset   = 0;

    VkPipelineVertexInputStateCreateInfo skinnedVertexInput = vertexInput;
    skinnedVertexInput.pVertexBindingDescriptions   = &skinnedBinding;
    skinnedVertexInput.pVertexAttributeDescriptions = &skinnedAttr;

    VkGraphicsPipelineCreateInfo skinnedPipelineInfo = pipelineInfo;
    skinnedPipelineInfo.pVertexInputState = &skinnedVertexInput;
    if (vkCreateGraphicsPipelines(ctx.gpu.device(), VK_NULL_HANDLE, 1, &skinnedPipelineInfo, nullptr, &m_skinnedPipeline) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create skinned shadow pipeline!");
    }

    vkDestroyShaderModule(ctx.gpu.device(), vertModule, nullptr);
}

// The framebuffers hang from the per-layer views AND from the render pass, so
// they go apart: the resize rebuilds the former without touching the latter.
void ShadowPass::createFramebuffers(const Context& ctx)
{
    for (uint32_t c = 0; c < SHADOW_MATRICES; c++)
    {
        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass      = m_renderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments    = &m_layerViews[c];
        fbInfo.width           = m_size;
        fbInfo.height          = m_size;
        fbInfo.layers          = 1;
        if (vkCreateFramebuffer(ctx.gpu.device(), &fbInfo, nullptr, &m_framebuffers[c]) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create shadow framebuffer!");
        }
    }
}

void ShadowPass::destroySizedResources(const Context& ctx)
{
    vkDestroyImageView(ctx.gpu.device(), m_view, nullptr);
    m_view = VK_NULL_HANDLE;
    for (int c = 0; c < SHADOW_MATRICES; c++)
    {
        vkDestroyImageView(ctx.gpu.device(), m_layerViews[c], nullptr);
        vkDestroyFramebuffer(ctx.gpu.device(), m_framebuffers[c], nullptr);
        m_layerViews[c]   = VK_NULL_HANDLE;
        m_framebuffers[c] = VK_NULL_HANDLE;
    }
    vkDestroyImage(ctx.gpu.device(), m_image, nullptr);
    vkFreeMemory(ctx.gpu.device(), m_memory, nullptr);
    m_image  = VK_NULL_HANDLE;
    m_memory = VK_NULL_HANDLE;
}

void ShadowPass::resizeResources(const Context& ctx, uint32_t size)
{
    if (size == m_size || size == 0) return;

    destroySizedResources(ctx);
    m_size = size;
    createSizedResources(ctx);
    // Behind the image and its views: the framebuffers reference them.
    createFramebuffers(ctx);
}

void ShadowPass::destroyResources(const Context& ctx)
{
    vkDestroySampler(ctx.gpu.device(), m_sampler, nullptr);
    destroySizedResources(ctx);
    vkDestroyPipeline(ctx.gpu.device(), m_pipeline, nullptr);
    vkDestroyPipeline(ctx.gpu.device(), m_skinnedPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.gpu.device(), m_pipelineLayout, nullptr);
    vkDestroyRenderPass(ctx.gpu.device(), m_renderPass, nullptr);
}

// ── cascades ────────────────────────────────────────────────────────────────

void ShadowPass::computeCascades(const glm::mat4& view, const glm::mat4& proj,
                                 const std::vector<Light>& lights,
                                 float maxDistance, float lambda,
                                 const glm::vec3& sceneCenter)
{
    for (int i = 0; i < SHADOW_MATRICES; i++) m_cascadeMatrices[i] = glm::mat4(1.0f);
    m_cascadeSplits = glm::vec4(0.0f);
    m_activeLayers  = 0;
    m_extraLayers   = 0;
    for (int& r : m_shadowSlot) r = -1;
    if (lights.empty()) return;

    // The secondary spots FIRST, because the three branches of the key light further
    // down exit with return. They occupy the slots from SHADOW_KEY_MATRICES
    // onwards, which the key never touches.
    {
        const int n = std::min((int)lights.size(), MAX_LIGHTS);
        m_extraLayers = repartirSombrasExtra(
            lights.data(), n,
            [](const Light& l) { return (int)(l.direction.w + 0.5f); },
            [](const Light& l) { return l.params; },
            m_shadowSlot, m_shadowFaces);

        for (int i = 1; i < n; i++)
        {
            if (m_shadowSlot[i] < 0) continue;

            // Which technique applies to THIS light. It is the same criterion that decides
            // how many slots the allocation gave it, and that is why it is asked the same way:
            // if it were answered differently here, fewer faces than the reserved
            // ones would be recorded and the shader would sample another light's layers.
            const int  tipo = static_cast<int>(lights[i].direction.w + 0.5f);
            const bool cubemap = tipo == static_cast<int>(LightType::Point) ||
                                 (tipo == static_cast<int>(LightType::Spot) &&
                                  spotNecesitaCubemap(lights[i].params));

            // flipY = true, like everything else in Vulkan.
            const bool ok = cubemap
                ? pointShadowMatrices(lights[i].position, lights[i].params, /*flipY=*/true,
                                      &m_cascadeMatrices[m_shadowSlot[i]])
                : spotShadowMatrix(lights[i].position, lights[i].direction, lights[i].params,
                                   /*flipY=*/true, m_cascadeMatrices[m_shadowSlot[i]]);
            if (!ok)
            {
                // No direction or no usable range: its slot is withdrawn
                // instead of leaving an identity matrix that would shade anything.
                // The reserved layers are left undrawn, which is
                // correct: the shader is not going to look at them.
                m_shadowSlot[i]  = -1;
                m_shadowFaces[i] = 0;
            }
        }
    }

    // POINT: six-face cubemap. It does not use the camera's frustum either (the
    // volume is set by the light's range), so it exits through here just like the
    // spot. The cuts are left at 0: the point branch of shadow_lookup.glsl does not
    // look at them.
    // A SPOT that is too wide also enters here: above a 90 degree
    // cone, a single face spreads the same texels over so much world that the
    // shadow's edge comes out stepped, and it gets worse with each degree because it goes with
    // tan(FOV/2). Six 90 degree faces are strictly better.
    const int tipoKey = static_cast<int>(lights[0].direction.w + 0.5f);
    if (tipoKey == static_cast<int>(LightType::Point) ||
        (tipoKey == static_cast<int>(LightType::Spot) && spotNecesitaCubemap(lights[0].params)))
    {
        // flipY = true, like the spot and the cascades' orthographic:
        // in Vulkan the Y convention is absorbed by the matrix, not the viewport.
        if (pointShadowMatrices(lights[0].position, lights[0].params,
                                /*flipY=*/true, m_cascadeMatrices))
        {
            m_activeLayers = SHADOW_KEY_MATRICES;
        }
        return;
    }

    // SPOT: a single perspective face, in layer 0. It does not use the camera's
    // frustum at all (the volume is set by the light's cone), so it exits through
    // here before computing it. The cuts are left at 0: the spot branch of
    // shadow_lookup.glsl does not look at them.
    if (static_cast<int>(lights[0].direction.w + 0.5f) == static_cast<int>(LightType::Spot))
    {
        // flipY = true, the SAME inversion applied to the cascades'
        // orthographic a few lines below and for the same reason: in Vulkan the Y
        // convention is absorbed by the matrix, and in D3D12 by the negative-height
        // viewport of the shadow pass. See the comment on spotShadowMatrix.
        if (spotShadowMatrix(lights[0].position, lights[0].direction, lights[0].params,
                             /*flipY=*/true, m_cascadeMatrices[0]))
        {
            m_activeLayers = 1;
        }
        return;
    }

    // The cascade split lives in cascadeShadowMatrices, shared with
    // D3D12: it was the same 60 lines of math written twice, and a
    // divergence between them would go undetected (H3).
    //
    // The direction is decided by keyLightDirection, which is the ONLY place where
    // that criterion lives: the fog needs it identical so that its
    // in-scattering and this shadow map talk about the same light. A point light
    // has no direction of its own and aims at sceneCenter, which arrives already computed
    // from the Renderer.
    glm::vec3 lightDir;
    if (!keyLightDirection(lights[0].position, lights[0].direction, sceneCenter, lightDir))
        return;

    // flipY = true: in Vulkan the Y convention is absorbed by the matrix.
    if (!cascadeShadowMatrices(view, proj, lightDir, maxDistance, lambda, m_size,
                               /*flipY=*/true, m_cascadeMatrices, m_cascadeSplits))
    {
        return;
    }
    m_activeLayers = SHADOW_CASCADES;
}

// ── recording ───────────────────────────────────────────────────────────────

void ShadowPass::beginCascade(VkCommandBuffer cmd, uint32_t cascade)
{
    VkClearValue clearDepth{};
    clearDepth.depthStencil = { 1.0f, 0 };

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass        = m_renderPass;
    renderPassInfo.framebuffer       = m_framebuffers[cascade];
    renderPassInfo.renderArea.extent = { m_size, m_size };
    renderPassInfo.clearValueCount   = 1;
    renderPassInfo.pClearValues      = &clearDepth;

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp {0.0f, 0.0f, (float)m_size, (float)m_size, 0.0f, 1.0f};
    VkRect2D   sc {{0,0}, {m_size, m_size}};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(uint32_t), &cascade);
}

void ShadowPass::bindSkinnedPipeline(VkCommandBuffer cmd, uint32_t cascade)
{
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinnedPipeline);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(uint32_t), &cascade);
}

void ShadowPass::endCascade(VkCommandBuffer cmd)
{
    vkCmdEndRenderPass(cmd);
}

} // namespace DonTopo
