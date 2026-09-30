#include "DonTopo/Renderer/InstanceBuffers.h"
#include "DonTopo/Renderer/GpuDevice.h"

#include <stdexcept>

namespace DonTopo {

void InstanceBuffers::destroyBuffer(const Context& ctx, int frame)
{
    if (m_buffers[frame] == VK_NULL_HANDLE) return;
    // The persistent mapping dies with the memory; no explicit unmap is
    // needed, but the pointer must be forgotten so as not to write through it if something
    // failed between the destroy and the create.
    m_mapped[frame] = nullptr;
    vkDestroyBuffer(ctx.gpu.device(), m_buffers[frame], nullptr);
    vkFreeMemory(ctx.gpu.device(), m_memory[frame], nullptr);
    m_buffers[frame]  = VK_NULL_HANDLE;
    m_memory[frame]   = VK_NULL_HANDLE;
    m_capacity[frame] = 0;
}

void InstanceBuffers::create(const Context& ctx)
{
    // Set 1: un solo storage buffer, solo lo lee el vertex shader
    // (triangle.vert y shadow.vert).
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 1;
    dslInfo.pBindings    = &binding;
    if (vkCreateDescriptorSetLayout(ctx.gpu.device(), &dslInfo, nullptr, &m_descLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create instance descriptor set layout!");

    // Own pool and not the Renderer's chain of shared pools: that one is
    // handed out per object and only has UNIFORM_BUFFER and COMBINED_IMAGE_SAMPLER.
    // Here we need exactly kFrames sets with one STORAGE_BUFFER each,
    // and the sets live for the whole process (they are never freed).
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = kFrames;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    poolInfo.maxSets       = kFrames;
    if (vkCreateDescriptorPool(ctx.gpu.device(), &poolInfo, nullptr, &m_descPool) != VK_SUCCESS)
        throw std::runtime_error("failed to create instance descriptor pool!");

    VkDescriptorSetLayout layouts[kFrames];
    for (int i = 0; i < kFrames; i++) layouts[i] = m_descLayout;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descPool;
    allocInfo.descriptorSetCount = kFrames;
    allocInfo.pSetLayouts        = layouts;
    if (vkAllocateDescriptorSets(ctx.gpu.device(), &allocInfo, m_descSets) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate instance descriptor sets!");

    // The buffers of ALL frames, not just the current one: each frame's descriptor set
    // has to point at something valid from the first draw.
    //
    // This used to save and restore the Renderer's m_currentFrame in order to
    // call ensureInstanceCapacity, which read the frame from there. With the frame
    // as an argument the workaround is unnecessary.
    for (int i = 0; i < kFrames; i++)
        ensureCapacity(ctx, i, kInitialCapacity);
}

void InstanceBuffers::destroy(const Context& ctx)
{
    for (int i = 0; i < kFrames; i++)
        destroyBuffer(ctx, i);

    if (m_descPool != VK_NULL_HANDLE)
    {
        // The sets go away with the pool; vkFreeDescriptorSets does not apply here.
        vkDestroyDescriptorPool(ctx.gpu.device(), m_descPool, nullptr);
        m_descPool = VK_NULL_HANDLE;
    }
    for (int i = 0; i < kFrames; i++) m_descSets[i] = VK_NULL_HANDLE;

    if (m_descLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(ctx.gpu.device(), m_descLayout, nullptr);
        m_descLayout = VK_NULL_HANDLE;
    }
    // The cursor pointed at memory that has just died.
    m_cur.reset(nullptr, 0);
}

void InstanceBuffers::ensureCapacity(const Context& ctx, int frame, uint32_t matrices)
{
    if (matrices <= m_capacity[frame]) return;

    // Double the size instead of fitting it tightly: instancing one object per frame
    // (Lua scripts) must not recreate the buffer every frame.
    uint32_t capacity = m_capacity[frame] ? m_capacity[frame] : kInitialCapacity;
    while (capacity < matrices) capacity *= 2;

    destroyBuffer(ctx, frame);

    VkDeviceSize size = (VkDeviceSize)capacity * sizeof(glm::mat4);

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size        = size;
    bufferInfo.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(ctx.gpu.device(), &bufferInfo, nullptr, &m_buffers[frame]) != VK_SUCCESS)
        throw std::runtime_error("failed to create instance buffer!");

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(ctx.gpu.device(), m_buffers[frame], &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = memReq.size;
    allocInfo.memoryTypeIndex = ctx.gpu.findMemoryType(memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(ctx.gpu.device(), &allocInfo, nullptr, &m_memory[frame]) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate instance buffer memory!");
    vkBindBufferMemory(ctx.gpu.device(), m_buffers[frame], m_memory[frame], 0);

    // Persistent mapping, like the uniform buffers: it is written every frame.
    vkMapMemory(ctx.gpu.device(), m_memory[frame], 0, size, 0, &m_mapped[frame]);
    m_capacity[frame] = capacity;

    VkDescriptorBufferInfo dbi{};
    dbi.buffer = m_buffers[frame];
    dbi.offset = 0;
    dbi.range  = size;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = m_descSets[frame];
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo     = &dbi;
    vkUpdateDescriptorSets(ctx.gpu.device(), 1, &write, 0, nullptr);
}

void InstanceBuffers::beginFrame(const Context& ctx, int frame, uint32_t matrices)
{
    ensureCapacity(ctx, frame, matrices);
    // After ensureCapacity, not before: growing recreates the buffer and the previously
    // mapped pointer is left dangling.
    m_cur.reset((glm::mat4*)m_mapped[frame], m_capacity[frame]);
}

}
