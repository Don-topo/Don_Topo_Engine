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
        throw std::runtime_error("shader vacio: " + path);
    // SPIR-V son palabras de 32 bits. Un tamano que no es multiplo de 4 no es
    // que sea sospechoso: es que `pCode` leeria una palabra a medias fuera del
    // buffer. Mejor decirlo aqui, con la ruta, que dejar que reviente dentro
    // del driver.
    if (size % 4 != 0)
        throw std::runtime_error("shader truncado (" + std::to_string(size) +
                                 " bytes, no es multiplo de 4): " + path);

    std::vector<char> buf(static_cast<size_t>(size));
    f.seekg(0);
    f.read(buf.data(), size);
    if (!f)
        throw std::runtime_error("lectura incompleta del shader: " + path);
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
