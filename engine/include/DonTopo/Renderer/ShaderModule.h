#pragma once
#include <vulkan/vulkan.h>
#include <string>
#include <vector>

namespace DonTopo {

// Loading of SPIR-V shaders, in a single place.
//
// This lived copied in SIXTEEN files (the twelve passes, Gizmos, Skybox,
// SplashScreen and UiSpriteBatch) as a static `loadSpv` + `makeModule` pair
// (H11). Eleven copies were byte-for-byte identical and the other five only
// changed the error message prefix, so unifying does not change the
// behavior of any: the message carries the path, which says the same thing the
// prefix said and also which of the module's two shaders failed.
//
// What DOES change is that the size is now validated. With the repeated copy
// nobody did it because doing so meant touching the sixteen places, which is
// exactly the symptom the finding described.

// Reads a whole .spv. Throws std::runtime_error, with the path in the message, if
// the file does not open or if its size cannot be SPIR-V.
//
// The size matters: `VkShaderModuleCreateInfo::pCode` is a `const uint32_t*`
// and Vulkan reads `codeSize` bytes from there. A truncated file (an interrupted
// shader build leaves some half done) made the read run past the end
// of the vector, and the failure showed up later and elsewhere.
std::vector<char> readSpvFile(const std::string& path);

// Reads and creates the module. The twenty callers that existed did these two things
// back to back and none kept the blob, so the only useful form of the
// helper is this one. The module is destroyed by the caller, as before.
VkShaderModule loadShaderModule(VkDevice device, const std::string& path);

} // namespace DonTopo
