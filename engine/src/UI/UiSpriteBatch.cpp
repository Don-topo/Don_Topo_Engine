#include "DonTopo/UI/UiSpriteBatch.h"

#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/GpuResources.h"

#include <glm/ext/matrix_clip_space.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace DonTopo
{
    namespace
    {
        // uint16 para los índices: 4 vértices por quad, así que el techo son
        // 16383 quads por frame. Pasado ese punto se deja de emitir en vez de
        // desbordar el índice en silencio.
        constexpr size_t kMaxVertices = 65532;

        constexpr uint32_t kInitialVertexCapacity = 1024;   // 256 quads
        constexpr uint32_t kInitialIndexCapacity  = 1536;

        UiScissor scissorFromRect(const glm::vec2& pos, const glm::vec2& size)
        {
            if (size.x <= 0.0f || size.y <= 0.0f) return {};

            // Hacia fuera (floor/ceil): recortar de menos deja el borde del
            // sprite; recortar de más se come una fila de píxeles.
            const float fx0 = std::floor(pos.x);
            const float fy0 = std::floor(pos.y);
            const float fx1 = std::ceil(pos.x + size.x);
            const float fy1 = std::ceil(pos.y + size.y);

            UiScissor s{};
            s.x = (int32_t)fx0;
            s.y = (int32_t)fy0;
            s.width  = (uint32_t)std::max(0.0f, fx1 - fx0);
            s.height = (uint32_t)std::max(0.0f, fy1 - fy0);
            return s;
        }

        // Intersección, NO reemplazo: un hijo nunca puede pintar fuera de lo que
        // su padre ya había recortado.
        UiScissor intersectScissor(const UiScissor& a, const UiScissor& b)
        {
            if (a.empty() || b.empty()) return {};

            const int64_t x0 = std::max((int64_t)a.x, (int64_t)b.x);
            const int64_t y0 = std::max((int64_t)a.y, (int64_t)b.y);
            const int64_t x1 = std::min((int64_t)a.x + a.width,  (int64_t)b.x + b.width);
            const int64_t y1 = std::min((int64_t)a.y + a.height, (int64_t)b.y + b.height);

            if (x1 <= x0 || y1 <= y0) return {};

            UiScissor s{};
            s.x = (int32_t)x0;
            s.y = (int32_t)y0;
            s.width  = (uint32_t)(x1 - x0);
            s.height = (uint32_t)(y1 - y0);
            return s;
        }

        void emitQuad(const UiElement& node, const glm::vec2& pos, const glm::vec2& size,
                      const UiScissor& scissor, float opacity, UiDrawData& out)
        {
            if (out.vertices.size() + 4 > kMaxVertices) return;

            // Un lote solo puede llevar UN atlas y UN scissor: cualquiera de los
            // dos que cambie obliga a cerrar el actual y abrir otro.
            if (out.batches.empty() ||
                out.batches.back().atlas != node.atlas ||
                out.batches.back().scissor != scissor)
            {
                UiBatch batch{};
                batch.atlas      = node.atlas;
                batch.scissor    = scissor;
                batch.firstIndex = (uint32_t)out.indices.size();
                batch.indexCount = 0;
                out.batches.push_back(batch);
            }

            UiUvRect uv{};
            if (node.atlas) uv = node.atlas->uvRect(node.sprite);

            const uint16_t base = (uint16_t)out.vertices.size();

            // La opacidad acumulada del árbol viaja POR VÉRTICE: así no parte el
            // lote, que solo puede cambiar por atlas o por scissor.
            glm::vec4 color = node.color;
            color.a *= opacity;

            // Sentido horario en pantalla empezando arriba a la izquierda. El
            // vértice inferior tiene la Y MAYOR: +Y va hacia abajo.
            out.vertices.push_back({{pos.x,          pos.y         }, {uv.u0, uv.v0}, color});
            out.vertices.push_back({{pos.x + size.x, pos.y         }, {uv.u1, uv.v0}, color});
            out.vertices.push_back({{pos.x + size.x, pos.y + size.y}, {uv.u1, uv.v1}, color});
            out.vertices.push_back({{pos.x,          pos.y + size.y}, {uv.u0, uv.v1}, color});

            const uint16_t quad[6] = { (uint16_t)(base + 0), (uint16_t)(base + 1), (uint16_t)(base + 2),
                                       (uint16_t)(base + 2), (uint16_t)(base + 3), (uint16_t)(base + 0) };
            out.indices.insert(out.indices.end(), quad, quad + 6);
            out.batches.back().indexCount += 6;
        }

        void emitNode(const UiElement& node, const glm::vec2& parentPos, const glm::vec2& parentScale,
                      const glm::vec2& parentSize, UiScissor scissor, float parentOpacity,
                      UiDrawData& out)
        {
            // enabled NO se mira aquí: es para el input, no para el dibujado.
            if (!node.visible) return;

            const glm::vec2 worldScale = parentScale * node.scale;
            const glm::vec2 worldSize  = node.size * worldScale;

            // anchor cuenta sobre el rect DEL PADRE y pivot sobre el PROPIO: con
            // ambos a {0,0} sale exactamente parentPos + position * parentScale,
            // que es lo que hacía antes. node.rotation se ignora a propósito.
            const glm::vec2 worldPos = parentPos
                                     + node.anchor * parentSize
                                     + node.position * parentScale
                                     - node.pivot * worldSize;

            const float opacity = parentOpacity * node.opacity;

            // Un contenedor sin tamaño (la raíz, o un grupo que solo agrupa) no
            // define área de anclaje: sus hijos siguen anclando contra la del
            // padre en vez de colapsar todos contra su esquina.
            const glm::vec2 childArea = (worldSize.x > 0.0f && worldSize.y > 0.0f) ? worldSize : parentSize;

            if (node.clipChildren)
            {
                scissor = intersectScissor(scissor, scissorFromRect(worldPos, worldSize));
                // Intersección vacía: ni este nodo ni ninguno de sus hijos puede
                // verse, así que no se emite ni un draw con width/height 0.
                if (scissor.empty()) return;
            }

            if (node.drawable && worldSize.x > 0.0f && worldSize.y > 0.0f)
                emitQuad(node, worldPos, worldSize, scissor, opacity, out);

            for (const auto& child : node.children())
                emitNode(*child, worldPos, worldScale, childArea, scissor, opacity, out);
        }

        std::vector<char> readSpv(const std::string& path)
        {
            std::ifstream file(path, std::ios::ate | std::ios::binary);
            if (!file.is_open()) throw std::runtime_error("failed to open shader file: " + path);
            const size_t size = (size_t)file.tellg();
            std::vector<char> buffer(size);
            file.seekg(0);
            file.read(buffer.data(), (std::streamsize)size);
            return buffer;
        }

        VkShaderModule makeModule(VkDevice device, const std::vector<char>& code)
        {
            VkShaderModuleCreateInfo info{};
            info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            info.codeSize = code.size();
            info.pCode    = reinterpret_cast<const uint32_t*>(code.data());
            VkShaderModule module = VK_NULL_HANDLE;
            if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS)
                throw std::runtime_error("failed to create ui shader module!");
            return module;
        }
    }

    // ── CPU ─────────────────────────────────────────────────────────────────

    void UiSpriteBatch::build(const UiCanvas& canvas, uint32_t width, uint32_t height, UiDrawData& out)
    {
        UiScissor full{};
        full.x = 0;
        full.y = 0;
        full.width  = width;
        full.height = height;

        // El "padre" de la raíz es el render entero: es contra ese rect contra
        // el que anclan los elementos de primer nivel.
        const glm::vec2 screen{(float)width, (float)height};

        emitNode(canvas.root(), glm::vec2(0.0f), glm::vec2(1.0f), screen, full, 1.0f, out);
    }

    // ── GPU ─────────────────────────────────────────────────────────────────

    void UiSpriteBatch::init(GpuDevice& gpu, GpuResources& res, VkRenderPass renderPass,
                             VkSampleCountFlagBits samples)
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo dslInfo{};
        dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslInfo.bindingCount = 1;
        dslInfo.pBindings    = &binding;
        if (vkCreateDescriptorSetLayout(gpu.device(), &dslInfo, nullptr, &m_descLayout) != VK_SUCCESS)
            throw std::runtime_error("failed to create ui descriptor set layout!");

        // Pool propio: un set por atlas (más el blanco). 32 cubre de sobra la
        // UI de un juego y los sets viven todo el proceso.
        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = 32;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;
        poolInfo.maxSets       = 32;
        if (vkCreateDescriptorPool(gpu.device(), &poolInfo, nullptr, &m_descPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create ui descriptor pool!");

        res.createTextureSampler(m_sampler);

        const uint8_t white[4] = { 255, 255, 255, 255 };
        res.createSolidColorImage(white, m_whiteImage, m_whiteMemory);
        // UNORM y no el SRGB por defecto de createTextureImageView: la imagen la
        // crea createSolidColorImage como R8G8B8A8_UNORM, y la vista tiene que
        // declarar EXACTAMENTE ese formato (la imagen no es MUTABLE_FORMAT).
        // Da igual visualmente — 255 es 1.0 en los dos — pero es un error de
        // validacion y comportamiento indefinido.
        res.createTextureImageView(m_whiteImage, m_whiteView, VK_FORMAT_R8G8B8A8_UNORM);

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool     = m_descPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts        = &m_descLayout;
        if (vkAllocateDescriptorSets(gpu.device(), &allocInfo, &m_whiteSet) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate ui white descriptor set!");

        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView   = m_whiteView;
        imageInfo.sampler     = m_sampler;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = m_whiteSet;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &imageInfo;
        vkUpdateDescriptorSets(gpu.device(), 1, &write, 0, nullptr);

        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pcr.offset     = 0;
        pcr.size       = sizeof(glm::mat4);

        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &m_descLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &pcr;
        if (vkCreatePipelineLayout(gpu.device(), &pli, nullptr, &m_layout) != VK_SUCCESS)
            throw std::runtime_error("failed to create ui pipeline layout!");

        createPipeline(gpu, renderPass, samples);
    }

    void UiSpriteBatch::recreatePipeline(GpuDevice& gpu, VkRenderPass renderPass,
                                         VkSampleCountFlagBits samples)
    {
        if (m_layout == VK_NULL_HANDLE) return;   // sin init (headless sin UI)
        if (m_pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(gpu.device(), m_pipeline, nullptr);
            m_pipeline = VK_NULL_HANDLE;
        }
        createPipeline(gpu, renderPass, samples);
    }

    void UiSpriteBatch::createPipeline(GpuDevice& gpu, VkRenderPass renderPass,
                                       VkSampleCountFlagBits samples)
    {
        VkShaderModule vert = makeModule(gpu.device(), readSpv("shaders/ui.vert.spv"));
        VkShaderModule frag = makeModule(gpu.device(), readSpv("shaders/ui.frag.spv"));

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName  = "main";

        VkVertexInputBindingDescription bindingDesc{};
        bindingDesc.binding   = 0;
        bindingDesc.stride    = sizeof(UiVertex);
        bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attrs[3]{};
        attrs[0].location = 0;
        attrs[0].binding  = 0;
        attrs[0].format   = VK_FORMAT_R32G32_SFLOAT;
        attrs[0].offset   = offsetof(UiVertex, pos);
        attrs[1].location = 1;
        attrs[1].binding  = 0;
        attrs[1].format   = VK_FORMAT_R32G32_SFLOAT;
        attrs[1].offset   = offsetof(UiVertex, uv);
        attrs[2].location = 2;
        attrs[2].binding  = 0;
        attrs[2].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[2].offset   = offsetof(UiVertex, color);

        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount   = 1;
        vi.pVertexBindingDescriptions      = &bindingDesc;
        vi.vertexAttributeDescriptionCount = 3;
        vi.pVertexAttributeDescriptions    = attrs;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{};
        vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        // NONE: los quads salen en el orden en que los emite el batcher y su
        // orientación no depende del frontFace del resto del motor.
        rs.cullMode    = VK_CULL_MODE_NONE;
        rs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        // Las mismas muestras que sus compañeros del pass de composición: con
        // MSAA ese pass es multisample y un pipeline que declare 1 sample no
        // sería compatible.
        ms.rasterizationSamples = samples;

        // El pass de composición trae la profundidad de la escena cargada. La UI
        // ni la lee ni la escribe: va siempre encima.
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable  = VK_FALSE;
        ds.depthWriteEnable = VK_FALSE;
        ds.depthCompareOp   = VK_COMPARE_OP_ALWAYS;

        // Alpha recto (SRC_ALPHA / ONE_MINUS_SRC_ALPHA): el color del sprite NO
        // viene premultiplicado.
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable         = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp        = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp        = VK_BLEND_OP_ADD;
        blend.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments    = &blend;

        VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dyn{};
        dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates    = dynStates;

        VkGraphicsPipelineCreateInfo pci{};
        pci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pci.stageCount          = 2;
        pci.pStages             = stages;
        pci.pVertexInputState   = &vi;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState      = &vp;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState   = &ms;
        pci.pDepthStencilState  = &ds;
        pci.pColorBlendState    = &cb;
        pci.pDynamicState       = &dyn;
        pci.layout              = m_layout;
        pci.renderPass          = renderPass;
        pci.subpass             = 0;

        if (vkCreateGraphicsPipelines(gpu.device(), VK_NULL_HANDLE, 1, &pci, nullptr, &m_pipeline) != VK_SUCCESS)
            throw std::runtime_error("failed to create ui pipeline!");

        vkDestroyShaderModule(gpu.device(), vert, nullptr);
        vkDestroyShaderModule(gpu.device(), frag, nullptr);
    }

    bool UiSpriteBatch::registerAtlas(GpuDevice& gpu, UiTextureAtlas& atlas)
    {
        if (m_descPool == VK_NULL_HANDLE || !atlas.loaded()) return false;

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool     = m_descPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts        = &m_descLayout;

        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(gpu.device(), &allocInfo, &set) != VK_SUCCESS) return false;

        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView   = atlas.view();
        imageInfo.sampler     = m_sampler;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &imageInfo;
        vkUpdateDescriptorSets(gpu.device(), 1, &write, 0, nullptr);

        atlas.setDescriptorSet(set);
        return true;
    }

    void UiSpriteBatch::ensureBuffers(GpuDevice& gpu, int frame, uint32_t vertexCount, uint32_t indexCount)
    {
        auto grow = [&](VkBuffer& buffer, VkDeviceMemory& memory, void*& mapped, uint32_t& capacity,
                        uint32_t needed, uint32_t initial, size_t elementSize, VkBufferUsageFlags usage)
        {
            if (needed <= capacity) return;

            uint32_t next = capacity ? capacity : initial;
            while (next < needed) next *= 2;

            if (buffer != VK_NULL_HANDLE)
            {
                mapped = nullptr;
                vkDestroyBuffer(gpu.device(), buffer, nullptr);
                vkFreeMemory(gpu.device(), memory, nullptr);
                buffer = VK_NULL_HANDLE;
                memory = VK_NULL_HANDLE;
            }

            const VkDeviceSize size = (VkDeviceSize)next * elementSize;

            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size        = size;
            bufferInfo.usage       = usage;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(gpu.device(), &bufferInfo, nullptr, &buffer) != VK_SUCCESS)
                throw std::runtime_error("failed to create ui buffer!");

            VkMemoryRequirements memReq;
            vkGetBufferMemoryRequirements(gpu.device(), buffer, &memReq);

            VkMemoryAllocateInfo allocInfo{};
            allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize  = memReq.size;
            allocInfo.memoryTypeIndex = gpu.findMemoryType(memReq.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (vkAllocateMemory(gpu.device(), &allocInfo, nullptr, &memory) != VK_SUCCESS)
                throw std::runtime_error("failed to allocate ui buffer memory!");
            vkBindBufferMemory(gpu.device(), buffer, memory, 0);

            // Mapeo persistente: se reescribe entero cada frame.
            vkMapMemory(gpu.device(), memory, 0, size, 0, &mapped);
            capacity = next;
        };

        grow(m_vertexBuffers[frame], m_vertexMemory[frame], m_vertexMapped[frame], m_vertexCapacity[frame],
             vertexCount, kInitialVertexCapacity, sizeof(UiVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        grow(m_indexBuffers[frame], m_indexMemory[frame], m_indexMapped[frame], m_indexCapacity[frame],
             indexCount, kInitialIndexCapacity, sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    }

    void UiSpriteBatch::destroyBuffers(GpuDevice& gpu, int frame)
    {
        if (m_vertexBuffers[frame] != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(gpu.device(), m_vertexBuffers[frame], nullptr);
            vkFreeMemory(gpu.device(), m_vertexMemory[frame], nullptr);
        }
        if (m_indexBuffers[frame] != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(gpu.device(), m_indexBuffers[frame], nullptr);
            vkFreeMemory(gpu.device(), m_indexMemory[frame], nullptr);
        }
        m_vertexBuffers[frame]  = VK_NULL_HANDLE;
        m_vertexMemory[frame]   = VK_NULL_HANDLE;
        m_vertexMapped[frame]   = nullptr;
        m_vertexCapacity[frame] = 0;
        m_indexBuffers[frame]   = VK_NULL_HANDLE;
        m_indexMemory[frame]    = VK_NULL_HANDLE;
        m_indexMapped[frame]    = nullptr;
        m_indexCapacity[frame]  = 0;
    }

    void UiSpriteBatch::record(GpuDevice& gpu, VkCommandBuffer cmd, const UiDrawData& data,
                               VkExtent2D extent, int frame)
    {
        // Canvas vacío = ni un comando, ni un buffer creado, ni un mapeo. Es la
        // condición que hace que la escena 3D salga EXACTAMENTE igual que antes.
        if (data.empty() || m_pipeline == VK_NULL_HANDLE) return;
        if (extent.width == 0 || extent.height == 0) return;

        ensureBuffers(gpu, frame, (uint32_t)data.vertices.size(), (uint32_t)data.indices.size());

        std::memcpy(m_vertexMapped[frame], data.vertices.data(), data.vertices.size() * sizeof(UiVertex));
        std::memcpy(m_indexMapped[frame],  data.indices.data(),  data.indices.size()  * sizeof(uint16_t));

        // top=0 y bottom=alto: (0,0) cae ARRIBA a la izquierda. RH_ZO porque
        // Vulkan clipea z fuera de [0,1] y glm::ortho a secas da [-1,1].
        const glm::mat4 proj = glm::orthoRH_ZO(0.0f, (float)extent.width,
                                               (float)extent.height, 0.0f,
                                               0.0f, 1.0f);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
        vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &proj);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertexBuffers[frame], &offset);
        vkCmdBindIndexBuffer(cmd, m_indexBuffers[frame], 0, VK_INDEX_TYPE_UINT16);

        for (const UiBatch& batch : data.batches)
        {
            if (batch.indexCount == 0 || batch.scissor.empty()) continue;

            VkRect2D rect{};
            rect.offset.x      = batch.scissor.x;
            rect.offset.y      = batch.scissor.y;
            rect.extent.width  = batch.scissor.width;
            rect.extent.height = batch.scissor.height;
            vkCmdSetScissor(cmd, 0, 1, &rect);

            VkDescriptorSet set = (batch.atlas && batch.atlas->descriptorSet() != VK_NULL_HANDLE)
                                ? batch.atlas->descriptorSet() : m_whiteSet;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &set, 0, nullptr);
            vkCmdDrawIndexed(cmd, batch.indexCount, 1, batch.firstIndex, 0, 0);
        }

        // El scissor es estado dinámico del command buffer: dejarlo recortado
        // afectaría a lo que se grabe después en este mismo buffer.
        VkRect2D full{};
        full.offset = {0, 0};
        full.extent = extent;
        vkCmdSetScissor(cmd, 0, 1, &full);
    }

    void UiSpriteBatch::shutdown(GpuDevice& gpu)
    {
        for (int i = 0; i < kFrames; ++i) destroyBuffers(gpu, i);

        if (m_pipeline != VK_NULL_HANDLE)   vkDestroyPipeline(gpu.device(), m_pipeline, nullptr);
        if (m_layout != VK_NULL_HANDLE)     vkDestroyPipelineLayout(gpu.device(), m_layout, nullptr);
        if (m_whiteView != VK_NULL_HANDLE)  vkDestroyImageView(gpu.device(), m_whiteView, nullptr);
        if (m_whiteImage != VK_NULL_HANDLE) vkDestroyImage(gpu.device(), m_whiteImage, nullptr);
        if (m_whiteMemory != VK_NULL_HANDLE)vkFreeMemory(gpu.device(), m_whiteMemory, nullptr);
        if (m_sampler != VK_NULL_HANDLE)    vkDestroySampler(gpu.device(), m_sampler, nullptr);
        // El pool se lleva por delante todos los sets (el blanco y los de los
        // atlas), así que no hay que liberarlos uno a uno.
        if (m_descPool != VK_NULL_HANDLE)   vkDestroyDescriptorPool(gpu.device(), m_descPool, nullptr);
        if (m_descLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(gpu.device(), m_descLayout, nullptr);

        m_pipeline    = VK_NULL_HANDLE;
        m_layout      = VK_NULL_HANDLE;
        m_whiteView   = VK_NULL_HANDLE;
        m_whiteImage  = VK_NULL_HANDLE;
        m_whiteMemory = VK_NULL_HANDLE;
        m_sampler     = VK_NULL_HANDLE;
        m_descPool    = VK_NULL_HANDLE;
        m_descLayout  = VK_NULL_HANDLE;
        m_whiteSet    = VK_NULL_HANDLE;
    }
}
