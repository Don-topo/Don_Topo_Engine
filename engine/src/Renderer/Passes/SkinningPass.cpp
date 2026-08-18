#include "DonTopo/Renderer/Passes/SkinningPass.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include <stdexcept>
#include <fstream>
#include <string>
#include <vector>
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

// ── recursos ────────────────────────────────────────────────────────────────

void SkinningPass::createPipelines(const Context& ctx)
{
    // --- Descriptor set layout: 8 storage buffers ---
    VkDescriptorSetLayoutBinding bindings[8]{};
    for (uint32_t i = 0; i < 8; i++)
    {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 8;
    dslInfo.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(ctx.gpu.device(), &dslInfo, nullptr, &m_descLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create compute descriptor set layout!");

    // --- Pipeline layout (1 set + push constant) ---
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset     = 0;
    pcr.size       = sizeof(Push);

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount         = 1;
    plInfo.pSetLayouts            = &m_descLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(ctx.gpu.device(), &plInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create compute pipeline layout!");
    }

    // --- Descriptor pool: 8 SSBOs * 16 objetos max ---
    VkDescriptorPoolSize ps{};
    ps.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount = 8 * 16;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &ps;
    poolInfo.maxSets       = 16;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    if (vkCreateDescriptorPool(ctx.gpu.device(), &poolInfo, nullptr, &m_descPool) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create compute descriptor pool!");
    }

    // --- Crear los tres pipelines ---
    auto makePipeline = [&](const std::string& spv, VkPipeline& pipeline)
    {
        auto code   = loadSpv(spv);
        auto module = makeModule(ctx.gpu.device(), code);

        VkComputePipelineCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = module;
        info.stage.pName  = "main";
        info.layout       = m_pipelineLayout;

        if (vkCreateComputePipelines(ctx.gpu.device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
            throw std::runtime_error("failed to create compute pipeline: " + spv);

        vkDestroyShaderModule(ctx.gpu.device(), module, nullptr);
    };

    makePipeline("shaders/bone_eval.comp.spv",      m_boneEval);
    makePipeline("shaders/bone_hierarchy.comp.spv", m_boneHierarchy);
    makePipeline("shaders/skinning.comp.spv",       m_skinning);
}

void SkinningPass::destroyPipelines(const Context& ctx)
{
    // El pool ANTES que nada: los descriptor sets de las mallas salen de aqui y
    // el caller ya ha soltado los suyos.
    if (m_descPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(ctx.gpu.device(), m_descPool, nullptr);
        m_descPool = VK_NULL_HANDLE;
    }
    vkDestroyPipeline(ctx.gpu.device(), m_boneEval,      nullptr);
    vkDestroyPipeline(ctx.gpu.device(), m_boneHierarchy, nullptr);
    vkDestroyPipeline(ctx.gpu.device(), m_skinning,      nullptr);
    vkDestroyPipelineLayout(ctx.gpu.device(), m_pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.gpu.device(), m_descLayout, nullptr);
}

// ── grabacion ───────────────────────────────────────────────────────────────

void SkinningPass::record(const Context& ctx, VkCommandBuffer cmd)
{
    if (ctx.skinnedObjects.empty()) return;

    auto ssboBarrier = [](VkBuffer buf) {
        VkBufferMemoryBarrier b{};
        b.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.buffer        = buf;
        b.offset        = 0;
        b.size          = VK_WHOLE_SIZE;
        return b;
    };

    for (size_t i = 0; i < ctx.skinnedObjects.size(); i++)
    {
        // Borrado desde el editor, aún en vuelo (despachar skinning sobre un
        // SSBO cuyo batch no ha señalado sería un read-after-write que la
        // validación de sync marca) o fuera de cámara: los tres casos los
        // resolvió el culling del principio del frame, y el bucle de dibujo
        // de más abajo lee ESA misma lista. Saltar aquí un objeto que sí se
        // dibujara le dejaría la pose del último frame en que fue visible.
        if (i >= ctx.skinnedVisible.size() || !ctx.skinnedVisible[i]) continue;
        SkinnedRenderObject& obj = ctx.skinnedObjects[i];
        // Checkbox "Visible" apagado: no se dibuja en ningún pass, así que
        // skinearlo sería trabajo de GPU que nadie lee. La pose se queda
        // congelada en la del último frame visible, igual que hace el culling
        // con un personaje fuera de cámara.
        if (!obj.meshVisible) continue;
        Push push{};
        push.animTime     = obj.animTime;
        push.boneCount    = obj.boneCount;
        push.vertexCount  = obj.vertexCount;
        push.clipBase     = obj.activeClip * obj.boneCount;
        push.prevAnimTime = obj.prevAnimTime;
        push.prevClipBase = obj.prevClip * obj.boneCount;
        push.blendWeight  = obj.blendWeight;
        push.lockRootMotion = obj.lockRootMotion ? 1u : 0u;

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            m_pipelineLayout, 0, 1, &obj.computeDescSet, 0, nullptr);

        // --- Pass 1: bone_eval (local transforms) ---
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_boneEval);
        vkCmdPushConstants(cmd, m_pipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &push);
        vkCmdDispatch(cmd, (obj.boneCount + 63) / 64, 1, 1);

        // Barrier: localTransform escrito → leído por bone_hierarchy
        VkBufferMemoryBarrier b1 = ssboBarrier(obj.localTransformBuffer);
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 1, &b1, 0, nullptr);

        // --- Pass 2: bone_hierarchy (world transforms + inverse bind pose) ---
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_boneHierarchy);
        vkCmdPushConstants(cmd, m_pipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &push);
        vkCmdDispatch(cmd, 1, 1, 1);

        // Barrier: finalBone escrito → leído por skinning
        VkBufferMemoryBarrier b2 = ssboBarrier(obj.finalBoneBuffer);
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 1, &b2, 0, nullptr);

        // --- Pass 3: skinning (output vertex buffer) ---
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_skinning);
        vkCmdPushConstants(cmd, m_pipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &push);
        vkCmdDispatch(cmd, (obj.vertexCount + 63) / 64, 1, 1);

        // Barrier: outputVertexBuffer escrito por compute → leído como VB en vertex shader
        VkBufferMemoryBarrier b3{};
        b3.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        b3.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b3.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        b3.buffer        = obj.outputVertexBuffer;
        b3.offset        = 0;
        b3.size          = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
            0, 0, nullptr, 1, &b3, 0, nullptr);
    }
}

} // namespace DonTopo
