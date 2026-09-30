#include "DonTopo/Renderer/IkBlock.h"
#include "DonTopo/Renderer/PoseBlock.h"
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

// ── resources ────────────────────────────────────────────────────────────────

void SkinningPass::createPipelines(const Context& ctx)
{
    // --- Descriptor set layout: 10 storage buffers (8 y 9: poseTrs y
    // frozenTrs, solo de bone_eval) ---
    VkDescriptorSetLayoutBinding bindings[12]{};
    for (uint32_t i = 0; i < 12; i++)
    {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 12;
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

    // --- Descriptor pool: the first of the chain (see allocateSet) ---
    if (!addPool(ctx))
    {
        throw std::runtime_error("failed to create compute descriptor pool!");
    }

    // --- Create the three pipelines ---
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
    makePipeline("shaders/bone_ik.comp.spv",        m_boneIk);
    makePipeline("shaders/skinning.comp.spv",       m_skinning);
}

bool SkinningPass::addPool(const Context& ctx)
{
    // 12 SSBOs per set, which are the twelve buffers that initSkinnedRenderObject binds.
    VkDescriptorPoolSize ps{};
    ps.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount = 12 * kSetsPerPool;

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

    // Two attempts: the last pool and, if it is full, a freshly created one. There
    // is no need to go through the previous ones: when a set is freed its slot goes back to ITS
    // pool, so an old pool may have room; what is lost by not
    // looking for it is a bit of memory, not correctness, and in exchange the normal
    // path is a single call.
    for (int intento = 0; intento < 2; ++intento)
    {
        if (!m_descPools.empty())
        {
            dsAlloc.descriptorPool = m_descPools.back();
            const VkResult r = vkAllocateDescriptorSets(ctx.gpu.device(), &dsAlloc, &outSet);
            if (r == VK_SUCCESS)
                return m_descPools.back();
            // Anything that is not "this pool is full" is not fixed by
            // another pool.
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
    // The pools BEFORE anything else: the meshes' descriptor sets come from here
    // and the caller has already released its own.
    for (VkDescriptorPool pool : m_descPools)
    {
        if (pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(ctx.gpu.device(), pool, nullptr);
    }
    m_descPools.clear();
    vkDestroyPipeline(ctx.gpu.device(), m_boneEval,      nullptr);
    vkDestroyPipeline(ctx.gpu.device(), m_boneHierarchy, nullptr);
    vkDestroyPipeline(ctx.gpu.device(), m_boneIk,        nullptr);
    vkDestroyPipeline(ctx.gpu.device(), m_skinning,      nullptr);
    vkDestroyPipelineLayout(ctx.gpu.device(), m_pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.gpu.device(), m_descLayout, nullptr);
}

// ── recording ───────────────────────────────────────────────────────────────

void SkinningPass::record(const Context& ctx, VkCommandBuffer cmd)
{
    if (ctx.skinnedObjects.empty()) return;

    // Characters to skin this frame. Deleted from the editor, still in flight
    // (dispatching skinning over an SSBO whose batch has not signaled would be a
    // read-after-write that the sync validation flags) or off camera: the
    // three cases were resolved by the culling at the start of the frame, and the drawing
    // loop further down reads THAT same list. Skipping here an object that does
    // get drawn would leave it with the pose of the last frame in which it was visible. And with
    // the "Visible" checkbox off it is not drawn in any pass: skinning it
    // would be GPU work that nobody reads, and the pose stays frozen just
    // as the culling does with a character off camera.
    std::vector<size_t> activos;
    activos.reserve(ctx.skinnedObjects.size());
    for (size_t i = 0; i < ctx.skinnedObjects.size(); i++)
    {
        if (i >= ctx.skinnedVisible.size() || !ctx.skinnedVisible[i]) continue;
        if (!ctx.skinnedObjects[i].meshVisible) continue;
        activos.push_back(i);
    }
    if (activos.empty()) return;

    // This frame's pose block, in its copy: the Animator's pose or,
    // without it, a sample (active clip, weight 1) with updateAnimation's clock.
    for (size_t i : activos)
    {
        SkinnedRenderObject& obj = ctx.skinnedObjects[i];
        if (!obj.poseBlockMapped) continue;
        AnimationPose unica;
        unica.count = 1;
        unica.samples[0] = { (int)obj.activeClip, obj.animTime, 1.0f, 0 };
        AnimationPose& pose = obj.hasPose ? obj.pose : unica;
        // The masks point to the object's copy: the objects vector
        // may have been reallocated since setAnimationPose.
        for (int L = 0; L < kMaxLayersPose; L++)
            pose.layers[L].mask = obj.poseMasks[L].empty() ? nullptr : &obj.poseMasks[L];
        writePoseBlock(pose, obj.boneCount,
                       static_cast<uint32_t*>(obj.poseBlockMapped) + (size_t)ctx.frameIndex * poseBlockUints(obj.boneCount));
        if (obj.ikBlockMapped)
            writeIkBlock(obj.ik, static_cast<uint32_t*>(obj.ikBlockMapped) + (size_t)ctx.frameIndex * ikBlockUints());
    }

    auto pushDe = [&ctx](const SkinnedRenderObject& obj) {
        Push push{};
        push.boneCount       = obj.boneCount;
        push.vertexCount     = obj.vertexCount;
        push.rootMotionMode  = obj.hasPose ? obj.pose.rootMotionMode : 0u;
        push.poseBlockOffset = ctx.frameIndex * poseBlockUints(obj.boneCount);
        push.ikBlockOffset   = ctx.frameIndex * ikBlockUints();
        return push;
    };

    // Freeze the on-screen pose of each layer with an interrupted fade
    // BEFORE evaluating: poseTrs holds the previous frame's. Once per
    // request.
    auto pideCongelar = [](const SkinnedRenderObject& o) {
        if (!o.hasPose) return false;
        for (int L = 0; L < o.pose.layerCount; L++) if (o.pose.layers[L].freezeNow) return true;
        return false;
    };
    bool hayCongelacion = false;
    for (size_t i : activos)
        if (pideCongelar(ctx.skinnedObjects[i])) hayCongelacion = true;
    if (hayCongelacion)
    {
        VkMemoryBarrier antes{};
        antes.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        antes.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        antes.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &antes, 0, nullptr, 0, nullptr);
        for (size_t i : activos)
        {
            SkinnedRenderObject& obj = ctx.skinnedObjects[i];
            if (!pideCongelar(obj)) continue;
            const VkDeviceSize capa = (VkDeviceSize)obj.boneCount * 3 * sizeof(float) * 4;
            for (int L = 0; L < obj.pose.layerCount; L++)
            {
                if (!obj.pose.layers[L].freezeNow) continue;
                VkBufferCopy region{};
                region.srcOffset = region.dstOffset = capa * (VkDeviceSize)L;
                region.size      = capa;
                vkCmdCopyBuffer(cmd, obj.poseTrsBuffer, obj.frozenTrsBuffer, 1, &region);
                obj.pose.layers[L].freezeNow = false;
            }
        }
        VkMemoryBarrier despues{};
        despues.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        despues.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        despues.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &despues, 0, nullptr, 0, nullptr);
    }

    // Three PHASES for all the characters, not three passes per character: each
    // one's buffers are its own, so within a phase they do not depend on
    // each other and ONE barrier between phases is enough. Before there were two barriers per
    // character, which serialized all of them: 1.41 ms of 11.16 with 30 characters
    // (docs/animation-audit.md, row 9). Incidentally each pipeline is bound once
    // per frame and not once per character.
    auto fase = [&](VkPipeline pipeline, const std::vector<size_t>& lista, uint32_t flags, auto grupos) {
        if (lista.empty()) return;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        for (size_t i : lista)
        {
            SkinnedRenderObject& obj = ctx.skinnedObjects[i];
            Push push = pushDe(obj);
            push.flags = flags;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                m_pipelineLayout, 0, 1, &obj.computeDescSet, 0, nullptr);
            vkCmdPushConstants(cmd, m_pipelineLayout,
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &push);
            vkCmdDispatch(cmd, grupos(obj), 1, 1);
        }
    };
    // What compute wrote in one phase is read by compute in the next.
    auto barreraEntreFases = [&]() {
        VkMemoryBarrier mb{};
        mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &mb, 0, nullptr, 0, nullptr);
    };

    // Characters with IK need the WORLD transforms between the
    // hierarchy and the skinning, so their hierarchy runs twice: the first time
    // without pass 2 (flags bit 0), bone_ik corrects the locals and the second
    // leaves the skinning matrices ready. The others pay nothing.
    std::vector<size_t> conIk;
    for (size_t i : activos)
        if (ctx.skinnedObjects[i].ik.count > 0) conIk.push_back(i);

    // 1) bone_eval: keys -> local transformations. One thread per bone.
    fase(m_boneEval, activos, 0u, [](const SkinnedRenderObject& o) { return (o.boneCount + 63) / 64; });
    barreraEntreFases();
    if (!conIk.empty())
    {
        // 2a) world-only hierarchy and 2b) IK over the locals.
        fase(m_boneHierarchy, conIk, 1u, [](const SkinnedRenderObject&) { return 1u; });
        barreraEntreFases();
        fase(m_boneIk, conIk, 1u, [](const SkinnedRenderObject&) { return 1u; });
        barreraEntreFases();
    }
    // 2) bone_hierarchy: locals -> finals (world x inverse bind pose). One
    //    workgroup per character, by depth levels.
    fase(m_boneHierarchy, activos, 0u, [](const SkinnedRenderObject&) { return 1u; });
    barreraEntreFases();
    // 3) skinning: deformed vertices. One thread per vertex.
    fase(m_skinning, activos, 0u, [](const SkinnedRenderObject& o) { return (o.vertexCount + 63) / 64; });

    // The vertices written by compute are read by the vertex assembler of
    // the drawing passes: a single barrier for all the characters.
    VkMemoryBarrier alDibujo{};
    alDibujo.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    alDibujo.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    alDibujo.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
        0, 1, &alDibujo, 0, nullptr, 0, nullptr);
}

} // namespace DonTopo
