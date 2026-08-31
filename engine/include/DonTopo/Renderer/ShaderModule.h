#pragma once
#include <vulkan/vulkan.h>
#include <string>
#include <vector>

namespace DonTopo {

// Carga de shaders SPIR-V, en un solo sitio.
//
// Esto vivia copiado en DIECISEIS ficheros —los doce pases, Gizmos, Skybox,
// SplashScreen y UiSpriteBatch— como un par `loadSpv` + `makeModule` estatico
// (H11). Once copias eran identicas byte a byte y las otras cinco solo
// cambiaban el prefijo del mensaje de error, asi que unificar no cambia el
// comportamiento de ninguna: el mensaje lleva la ruta, que dice lo mismo que
// decia el prefijo y ademas cual de los dos shaders del modulo fallo.
//
// Lo que SI cambia es que ahora se valida el tamano. Con la copia repetida
// nadie lo hacia porque hacerlo obligaba a tocar los dieciseis sitios, que es
// exactamente el sintoma que describia el hallazgo.

// Lee un .spv entero. Lanza std::runtime_error, con la ruta en el mensaje, si
// el fichero no abre o si su tamano no puede ser SPIR-V.
//
// El tamano importa: `VkShaderModuleCreateInfo::pCode` es un `const uint32_t*`
// y Vulkan lee `codeSize` bytes desde ahi. Un fichero truncado —una compilacion
// de shaders interrumpida deja alguno a medias— hacia que la lectura se saliera
// del vector, y el fallo aparecia mas tarde y en otro sitio.
std::vector<char> readSpvFile(const std::string& path);

// Lee y crea el modulo. Los veinte llamantes que habia hacian estas dos cosas
// seguidas y ninguno se quedaba con el blob, asi que la unica forma util del
// helper es esta. El modulo lo destruye el llamante, como antes.
VkShaderModule loadShaderModule(VkDevice device, const std::string& path);

} // namespace DonTopo
