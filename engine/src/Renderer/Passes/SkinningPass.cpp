#include "DonTopo/Renderer/Passes/SkinningPass.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include <stdexcept>
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>
#include "DonTopo/Renderer/ShaderModule.h"

namespace DonTopo {

// ── helpers ──────────────────────────────────────────────────────────────────

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

    // --- Descriptor pool: el primero de la cadena (ver allocateSet) ---
    if (!addPool(ctx))
    {
        throw std::runtime_error("failed to create compute descriptor pool!");
    }

    // --- Crear los tres pipelines ---
    auto makePipeline = [&](const std::string& spv, VkPipeline& pipeline)
    {
        auto module = loadShaderModule(ctx.gpu.device(), spv);

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

bool SkinningPass::addPool(const Context& ctx)
{
    // 8 SSBOs por set, que son los ocho buffers que ata initSkinnedRenderObject.
    VkDescriptorPoolSize ps{};
    ps.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount = 8 * kSetsPerPool;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &ps;
    poolInfo.maxSets       = kSetsPerPool;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(ctx.gpu.device(), &poolInfo, nullptr, &pool) != VK_SUCCESS)
        return false;

    m_descPools.push_back(pool);
    return true;
}

VkDescriptorPool SkinningPass::allocateSet(const Context& ctx, VkDescriptorSet& outSet)
{
    outSet = VK_NULL_HANDLE;

    VkDescriptorSetAllocateInfo dsAlloc{};
    dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts        = &m_descLayout;

    // Dos intentos: el ultimo pool y, si esta lleno, uno recien creado. No hace
    // falta recorrer los anteriores — al liberar un set su hueco vuelve a SU
    // pool, asi que un pool viejo puede tener sitio; lo que se pierde por no
    // buscarlo es un poco de memoria, no correccion, y a cambio el camino
    // normal es una sola llamada.
    for (int intento = 0; intento < 2; ++intento)
    {
        if (!m_descPools.empty())
        {
            dsAlloc.descriptorPool = m_descPools.back();
            const VkResult r = vkAllocateDescriptorSets(ctx.gpu.device(), &dsAlloc, &outSet);
            if (r == VK_SUCCESS)
                return m_descPools.back();
            // Cualquier cosa que no sea "este pool esta lleno" no la arregla
            // otro pool.
            if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL)
                return VK_NULL_HANDLE;
        }
        if (!addPool(ctx))
            return VK_NULL_HANDLE;
    }
    return VK_NULL_HANDLE;
}

void SkinningPass::destroyPipelines(const Context& ctx)
{
    // Los pools ANTES que nada: los descriptor sets de las mallas salen de aqui
    // y el caller ya ha soltado los suyos.
    for (VkDescriptorPool pool : m_descPools)
    {
        if (pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(ctx.gpu.device(), pool, nullptr);
    }
    m_descPools.clear();
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
