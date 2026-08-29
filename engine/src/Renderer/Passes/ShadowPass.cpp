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

namespace DonTopo {

// ── helpers ──────────────────────────────────────────────────────────────────

static std::vector<char> loadSpv(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("failed to open shader: " + path);
    size_t sz = (size_t)f.tellg();
    std::vector<char> buf(sz);
    f.seekg(0);
    f.read(buf.data(), (std::streamsize)sz);
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
        throw std::runtime_error("failed to create shader module!");
    return m;
}

// El alcance de las sombras y el reparto de los cortes entre cascadas eran
// constantes de aqui (kShadowMaxDistance = 500 y kCascadeLambda = 0.75). Ahora
// los elige el usuario y llegan por parametro a computeCascades; lo que
// significan y por que esos defaults esta documentado en RendererState.h, junto
// a shadowDistance() y cascadeLambda(). Los valores por defecto de alli son
// exactamente los que habia aqui, asi que la imagen no cambia sola.

// Margen por detras del volumen de cada cascada, en la direccion de la luz.
// Sin el, un objeto alto que queda fuera del frustum de la camara pero cuya
// sombra si cae dentro no se dibujaria en el shadow map.
static constexpr float kCasterMargin = 200.0f;

// ── recursos ────────────────────────────────────────────────────────────────

void ShadowPass::createSizedResources(const Context& ctx)
{
    // 1. Imagen depth para shadow map: un texture array con una capa por
    // cascada. No usa m_res.createImage porque esa fija arrayLayers a 1 y
    // la firma la comparten todas las texturas del motor.
    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.format        = VK_FORMAT_D32_SFLOAT;
    imageInfo.extent        = { m_size, m_size, 1 };
    imageInfo.mipLevels     = 1;
    imageInfo.arrayLayers   = SHADOW_CASCADES;
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

    // 2. Image views: una del array entero para muestrear, y una por capa
    // para colgarle un framebuffer.
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                          = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                          = m_image;
    viewInfo.viewType                       = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format                         = VK_FORMAT_D32_SFLOAT;
    viewInfo.subresourceRange.aspectMask    = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.layerCount    = SHADOW_CASCADES;
    viewInfo.subresourceRange.levelCount    = 1;
    if(vkCreateImageView(ctx.gpu.device(), &viewInfo, nullptr, &m_view) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create shadow image view!");
    }

    for (uint32_t c = 0; c < SHADOW_CASCADES; c++)
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

    // 3. Sampler de comparación (PCF listo)
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

     // 5. Framebuffers: uno por cascada, cada uno sobre su capa. Todos
     // comparten el render pass (el formato del attachment es el mismo).
     createFramebuffers(ctx);

    // 6. Pipeline (vertex-only, sin color attachments)
    auto vertCode = loadSpv("shaders/shadow.vert.spv");
    VkShaderModule vertModule = makeModule(ctx.gpu.device(), vertCode);

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
    colorBlend.attachmentCount = 0; // sin color attachments

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynStates;

    // El model matrix NO va por push constant: shadow.vert lo saca del SSBO
    // de instancias (set 1) por gl_InstanceIndex, igual que triangle.vert.
    // El único push constant es el índice de cascada, que dice cuál de las
    // matrices del UBO usar. Este layout es propio del pass de sombras y no
    // lo comparte ningún otro pipeline, así que el rango de PushData que
    // usan triangle/pbr/outline no se toca. El depth pre-pass sí lo toma
    // prestado: declara los mismos dos sets y no usa el push.
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

    // Variante para las mallas skinned. Todo el estado se copia del de
    // arriba (mismo bias, mismo depth, mismas cascadas, mismo layout), así
    // que la sombra de los estáticos no cambia. Lo único distinto es el
    // vertex input.
    //
    // stride 80, no sizeof(SkinnedVertex): ese es el vértice de ENTRADA del
    // compute (7×vec4, con índices y pesos de hueso). Lo que se dibuja aquí
    // es su SALIDA, el OutputVertex de skinning.comp, que son 5×vec4 y lleva
    // la posición en el primero. Es el mismo stride que declara el pipeline
    // skinned del pass principal.
    VkVertexInputBindingDescription skinnedBinding{};
    skinnedBinding.binding   = 0;
    skinnedBinding.stride    = 5 * (uint32_t)sizeof(glm::vec4);  // 80 bytes
    skinnedBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // pos es un vec4 (std430 del compute); shadow.vert solo declara vec3, y
    // leer 3 de los 4 floats es legal.
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

// Los framebuffers cuelgan de las vistas por capa Y del render pass, asi que
// van aparte: el resize rehace los primeros sin tocar el segundo.
void ShadowPass::createFramebuffers(const Context& ctx)
{
    for (uint32_t c = 0; c < SHADOW_CASCADES; c++)
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
    for (int c = 0; c < SHADOW_CASCADES; c++)
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
    // Detras de la imagen y sus vistas: los framebuffers las referencian.
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

// ── cascadas ────────────────────────────────────────────────────────────────

void ShadowPass::computeCascades(const glm::mat4& view, const glm::mat4& proj,
                                 const std::vector<Light>& lights,
                                 float maxDistance, float lambda,
                                 const glm::vec3& sceneCenter)
{
    for (int i = 0; i < SHADOW_CASCADES; i++) m_cascadeMatrices[i] = glm::mat4(1.0f);
    m_cascadeSplits = glm::vec4(0.0f);
    if (lights.empty()) return;

    // Esquinas del frustum, desproyectando el cubo NDC. z va de 0 a 1 y no
    // de -1 a 1 porque ese es el rango que Vulkan clipea: lo que se dibuja
    // de verdad está siempre entre esos dos planos.
    const glm::mat4 invViewProj = glm::inverse(proj * view);
    glm::vec3 cornerNear[4], cornerFar[4];
    const float ndcX[4] = { -1.0f,  1.0f,  1.0f, -1.0f };
    const float ndcY[4] = { -1.0f, -1.0f,  1.0f,  1.0f };
    for (int i = 0; i < 4; i++)
    {
        glm::vec4 pn = invViewProj * glm::vec4(ndcX[i], ndcY[i], 0.0f, 1.0f);
        glm::vec4 pf = invViewProj * glm::vec4(ndcX[i], ndcY[i], 1.0f, 1.0f);
        if (std::abs(pn.w) < 1e-8f || std::abs(pf.w) < 1e-8f) return;   // proyección degenerada
        cornerNear[i] = glm::vec3(pn) / pn.w;
        cornerFar[i]  = glm::vec3(pf) / pf.w;
    }

    // near/far REALES: la profundidad en view space de esos dos planos. No
    // se sacan de los coeficientes de proj a propósito — el editor
    // construye su proyección con glm::perspective (z en [-1,1]) y el
    // CameraComponent con *RH_ZO, así que los mismos coeficientes
    // significan cosas distintas y la fórmula tendría que saber cuál está
    // activa. Los planos z=0 y z=1, en cambio, son los mismos en los dos
    // casos, y las 4 esquinas de cada uno están a profundidad constante.
    const float camNear = -(view * glm::vec4(cornerNear[0], 1.0f)).z;
    const float camFar  = -(view * glm::vec4(cornerFar[0],  1.0f)).z;
    if (!std::isfinite(camNear) || !std::isfinite(camFar) ||
        camNear <= 0.0f || camFar <= camNear)
    {
        return;
    }

    // Las esquinas ya están puestas con el far REAL (es el que define los
    // rayos del frustum); el reparto de cascadas usa el far recortado.
    const float shadowFar = std::min(camFar, maxDistance);
    if (shadowFar <= camNear) return;

    // SOLO la luz 0 proyecta sombra. De donde sale su direccion lo decide
    // keyLightDirection, que es el UNICO sitio donde vive ese criterio: la
    // niebla lo necesita identico para que su in-scattering y este shadow map
    // hablen de la misma luz. Una luz de punto no tiene direccion propia y
    // apunta a sceneCenter, que llega ya calculado desde el Renderer.
    glm::vec3 lightDir;
    if (!keyLightDirection(lights[0].position, lights[0].direction, sceneCenter, lightDir))
        return;
    const glm::vec3 up = std::abs(lightDir.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                      : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::mat4 lightRot    = glm::lookAt(glm::vec3(0.0f), lightDir, up);
    const glm::mat4 invLightRot = glm::inverse(lightRot);

    float prevDist = camNear;
    for (int c = 0; c < SHADOW_CASCADES; c++)
    {
        const float p         = (float)(c + 1) / (float)SHADOW_CASCADES;
        const float logSplit  = camNear * std::pow(shadowFar / camNear, p);
        const float uniSplit  = camNear + (shadowFar - camNear) * p;
        const float dist      = lambda * logSplit + (1.0f - lambda) * uniSplit;
        m_cascadeSplits[c]    = dist;

        // Interpolar entre las esquinas cercana y lejana es exacto: la
        // profundidad en view space varía linealmente a lo largo de ese
        // segmento. Los factores se calculan contra el far REAL porque es el
        // que sitúa cornerFar.
        const float tNear = (prevDist - camNear) / (camFar - camNear);
        const float tFar  = (dist     - camNear) / (camFar - camNear);

        glm::vec3 corners[8];
        for (int i = 0; i < 4; i++)
        {
            const glm::vec3 ray = cornerFar[i] - cornerNear[i];
            corners[i]     = cornerNear[i] + ray * tNear;
            corners[i + 4] = cornerNear[i] + ray * tFar;
        }

        // Esfera envolvente y no AABB: el radio no depende de hacia dónde
        // mire la cámara, así que girar en el sitio no cambia el tamaño del
        // volumen y las sombras no laten.
        glm::vec3 center(0.0f);
        for (const glm::vec3& v : corners) center += v;
        center /= 8.0f;
        float radius = 0.0f;
        for (const glm::vec3& v : corners) radius = std::max(radius, glm::length(v - center));
        // Cuantizar el radio evita que un cambio mínimo de la cámara mueva
        // el borde del volumen y con él todos los téxeles.
        radius = std::ceil(radius * 16.0f) / 16.0f;
        if (radius < 1e-4f) radius = 1e-4f;

        // Snap del centro a téxeles del shadow map, en el espacio de la luz.
        // Sin esto, avanzar la cámara arrastra el volumen de forma continua
        // y los bordes de sombra hierven.
        const float unitsPerTexel = (2.0f * radius) / (float)m_size;
        glm::vec3 centerLS = glm::vec3(lightRot * glm::vec4(center, 1.0f));
        centerLS.x = std::floor(centerLS.x / unitsPerTexel) * unitsPerTexel;
        centerLS.y = std::floor(centerLS.y / unitsPerTexel) * unitsPerTexel;
        center = glm::vec3(invLightRot * glm::vec4(centerLS, 1.0f));

        const glm::mat4 lightView = glm::lookAt(center - lightDir * (radius + kCasterMargin),
                                                center, up);
        glm::mat4 lightProj = glm::orthoRH_ZO(-radius, radius, -radius, radius,
                                              0.0f, 2.0f * radius + kCasterMargin);
        lightProj[1][1] *= -1.0f;
        m_cascadeMatrices[c] = lightProj * lightView;

        prevDist = dist;
    }
}

// ── grabacion ───────────────────────────────────────────────────────────────

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
