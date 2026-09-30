#include "DonTopo/Renderer/ShaderModule.h"

#include <fstream>
#include <stdexcept>

namespace DonTopo {

std::vector<char> readSpvFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error("failed to open shader: " + path);

    const std::streamoff size = f.tellg();
    if (size <= 0)
        throw std::runtime_error("empty shader: " + path);
    // SPIR-V is made of 32-bit words. A size that is not a multiple of 4 is not
    // merely suspicious: `pCode` would read half a word past the end of the
    // buffer. Better to say so here, with the path, than to let it blow up inside
    // the driver.
    if (size % 4 != 0)
        throw std::runtime_error("truncated shader (" + std::to_string(size) +
                                 " bytes, not a multiple of 4): " + path);

    std::vector<char> buf(static_cast<size_t>(size));
    f.seekg(0);
    f.read(buf.data(), size);
    if (!f)
        throw std::runtime_error("incomplete shader read: " + path);
    return buf;
}

VkShaderModule loadShaderModule(VkDevice device, const std::string& path)
{
    const std::vector<char> code = readSpvFile(path);

    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &module) != VK_SUCCESS)
        throw std::runtime_error("failed to create shader module: " + path);
    return module;
}

} // namespace DonTopo
