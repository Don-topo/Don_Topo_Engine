#include "DonTopo/Renderer/InstanceBuffers.h"
#include "DonTopo/Renderer/GpuDevice.h"

#include <stdexcept>

namespace DonTopo {

void InstanceBuffers::destroyBuffer(const Context& ctx, int frame)
{
    if (m_buffers[frame] == VK_NULL_HANDLE) return;
    // El mapeo persistente muere con la memoria; no hace falta unmap
    // explícito, pero sí olvidar el puntero para no escribir en él si algo
    // fallara entre el destroy y el create.
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

    // Pool propio y no la cadena de pools compartidos del Renderer: esa se
    // reparte por objeto y solo tiene UNIFORM_BUFFER y COMBINED_IMAGE_SAMPLER.
    // Aquí hacen falta exactamente kFrames sets con un STORAGE_BUFFER cada uno,
    // y los sets viven todo el proceso (no se liberan nunca).
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

    // Los buffers de TODOS los frames, no solo el actual: el descriptor set de
    // cada frame tiene que apuntar a algo válido desde el primer draw.
    //
    // Antes esto guardaba y restauraba el m_currentFrame del Renderer para poder
    // llamar a ensureInstanceCapacity, que leía el frame de ahí. Con el frame
    // como argumento el apaño sobra.
    for (int i = 0; i < kFrames; i++)
        ensureCapacity(ctx, i, kInitialCapacity);
}

void InstanceBuffers::destroy(const Context& ctx)
{
    for (int i = 0; i < kFrames; i++)
        destroyBuffer(ctx, i);

    if (m_descPool != VK_NULL_HANDLE)
    {
        // Los sets se van con el pool; no hay vkFreeDescriptorSets que valga.
        vkDestroyDescriptorPool(ctx.gpu.device(), m_descPool, nullptr);
        m_descPool = VK_NULL_HANDLE;
    }
    for (int i = 0; i < kFrames; i++) m_descSets[i] = VK_NULL_HANDLE;

    if (m_descLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(ctx.gpu.device(), m_descLayout, nullptr);
        m_descLayout = VK_NULL_HANDLE;
    }
    // El cursor apuntaba a memoria que acaba de morir.
    m_cur.reset(nullptr, 0);
}

void InstanceBuffers::ensureCapacity(const Context& ctx, int frame, uint32_t matrices)
{
    if (matrices <= m_capacity[frame]) return;

    // Duplicar en vez de ajustar al pelo: instanciar un objeto por frame
    // (scripts Lua) no debe recrear el buffer en cada uno.
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

    // Mapeo persistente, como los uniform buffers: se escribe cada frame.
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
    // Despues de ensureCapacity, no antes: crecer recrea el buffer y el puntero
    // mapeado de antes queda colgando.
    m_cur.reset((glm::mat4*)m_mapped[frame], m_capacity[frame]);
}

}
