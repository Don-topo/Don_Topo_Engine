#include "DonTopo/Gizmos.h"
#include "DonTopo/GpuDevice.h"
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <cstdio>
#include <cstddef>

namespace DonTopo {

namespace {

std::vector<char> loadSpv(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Gizmos: cannot open shader: " + path);
    size_t sz = (size_t)f.tellg();
    std::vector<char> buf(sz);
    f.seekg(0);
    f.read(buf.data(), sz);
    return buf;
}

VkShaderModule makeModule(VkDevice dev, const std::vector<char>& code)
{
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS)
        throw std::runtime_error("Gizmos: failed to create shader module");
    return m;
}

} // namespace

Gizmos& Gizmos::get()
{
    static Gizmos instance;
    return instance;
}

void Gizmos::setEnabled(bool enabled) { get().m_enabled = enabled; }
bool Gizmos::isEnabled() { return get().m_enabled; }

void Gizmos::addLine(const glm::vec3& a, const glm::vec3& b, const glm::vec3& color)
{
    if (!m_enabled) return;
    if (m_vertices.size() + 2 > kMaxGizmoVertices) {
        if (!m_capacityWarned) {
            fprintf(stderr,
                "Gizmos: capacidad de %u vertices excedida, se descartan lineas adicionales\n",
                kMaxGizmoVertices);
            m_capacityWarned = true;
        }
        return;
    }
    m_vertices.push_back({a, color});
    m_vertices.push_back({b, color});
}

void Gizmos::drawLine(const glm::vec3& a, const glm::vec3& b, const glm::vec3& color)
{
    get().addLine(a, b, color);
}

void Gizmos::createBuffer(GpuDevice& gpu)
{
    VkDeviceSize size = sizeof(GizmoVertex) * (VkDeviceSize)kMaxGizmoVertices;

    for (int i = 0; i < kFramesInFlight; ++i) {
        VkBufferCreateInfo ci{};
        ci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        ci.size        = size;
        ci.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(gpu.device(), &ci, nullptr, &m_vertexBuffer[i]) != VK_SUCCESS)
            throw std::runtime_error("Gizmos: failed to create vertex buffer");

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(gpu.device(), m_vertexBuffer[i], &req);
        VkMemoryAllocateInfo ai{};
        ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = gpu.findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(gpu.device(), &ai, nullptr, &m_vertexMemory[i]) != VK_SUCCESS)
            throw std::runtime_error("Gizmos: failed to allocate vertex buffer memory");
        vkBindBufferMemory(gpu.device(), m_vertexBuffer[i], m_vertexMemory[i], 0);

        vkMapMemory(gpu.device(), m_vertexMemory[i], 0, size, 0, &m_mapped[i]);
    }
}

void Gizmos::createPipeline(GpuDevice& gpu, VkRenderPass renderPass)
{
    auto vertCode = loadSpv("shaders/gizmo.vert.spv");
    auto fragCode = loadSpv("shaders/gizmo.frag.spv");

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

    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = sizeof(GizmoVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0;
    attrs[0].binding  = 0;
    attrs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset   = offsetof(GizmoVertex, pos);
    attrs[1].location = 1;
    attrs[1].binding  = 0;
    attrs[1].format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset   = offsetof(GizmoVertex, color);

    VkPipelineVertexInputStateCreateInfo vtxInput{};
    vtxInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vtxInput.vertexBindingDescriptionCount   = 1;
    vtxInput.pVertexBindingDescriptions      = &binding;
    vtxInput.vertexAttributeDescriptionCount = 2;
    vtxInput.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

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

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS;

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

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.offset     = 0;
    push.size       = sizeof(glm::mat4);

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &push;
    if (vkCreatePipelineLayout(gpu.device(), &layoutCI, nullptr, &m_pipeLayout) != VK_SUCCESS)
        throw std::runtime_error("Gizmos: failed to create pipeline layout");

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

    if (vkCreateGraphicsPipelines(gpu.device(), VK_NULL_HANDLE, 1, &pCI, nullptr, &m_pipeline) != VK_SUCCESS)
        throw std::runtime_error("Gizmos: failed to create pipeline");

    vkDestroyShaderModule(gpu.device(), vertMod, nullptr);
    vkDestroyShaderModule(gpu.device(), fragMod, nullptr);
}

void Gizmos::init(GpuDevice& gpu, VkRenderPass renderPass, VkFormat colorFormat)
{
    (void)colorFormat; // el renderPass ya tiene el formato correcto
    get().createBuffer(gpu);
    get().createPipeline(gpu, renderPass);
}

void Gizmos::shutdown(GpuDevice& gpu)
{
    Gizmos& g = get();
    if (g.m_pipeline == VK_NULL_HANDLE) return;
    VkDevice dev = gpu.device();
    vkDestroyPipeline(dev, g.m_pipeline, nullptr);
    vkDestroyPipelineLayout(dev, g.m_pipeLayout, nullptr);
    for (int i = 0; i < kFramesInFlight; ++i) {
        vkUnmapMemory(dev, g.m_vertexMemory[i]);
        vkDestroyBuffer(dev, g.m_vertexBuffer[i], nullptr);
        vkFreeMemory(dev, g.m_vertexMemory[i], nullptr);
    }
    g.m_pipeline = VK_NULL_HANDLE;
}

void Gizmos::draw(VkCommandBuffer cmd, const glm::mat4& viewProj, int frameIndex)
{
    Gizmos& g = get();
    if (!g.m_enabled || g.m_vertices.empty() || g.m_pipeline == VK_NULL_HANDLE) return;

    VkDeviceSize copySize = sizeof(GizmoVertex) * (VkDeviceSize)g.m_vertices.size();
    memcpy(g.m_mapped[frameIndex], g.m_vertices.data(), (size_t)copySize);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.m_pipeline);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &g.m_vertexBuffer[frameIndex], &offset);
    vkCmdPushConstants(cmd, g.m_pipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &viewProj);
    vkCmdDraw(cmd, (uint32_t)g.m_vertices.size(), 1, 0, 0);
}

void Gizmos::clear()
{
    get().m_vertices.clear();
}

} // namespace DonTopo
