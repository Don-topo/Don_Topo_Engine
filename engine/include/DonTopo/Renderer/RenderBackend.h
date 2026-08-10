#pragma once

#include <string>

namespace DonTopo {

// Backend de render con el que arranca el proceso. NO se puede cambiar en
// caliente: el device, la swapchain y todos los recursos de GPU cuelgan de él,
// así que cambiarlo obliga a reiniciar la aplicación.
enum class RenderBackend {
    Vulkan,
    D3D12,
};

// Nombres que se persisten en el project.json. Igual que aaMode/fpMode: el
// ajuste se guarda por NOMBRE, nunca por índice, para que reordenar el combo no
// cambie lo que ya hay guardado.
const char* renderBackendName(RenderBackend backend);

// ok = false si el nombre no es ninguno de los de hoy (fichero de una versión
// futura, o editado a mano): el caller se cae a Vulkan y lo deja en el Log.
RenderBackend renderBackendFromName(const std::string& name, bool& ok);

// Qué backend se va a usar de verdad, frente al que se pidió.
struct BackendSelection {
    RenderBackend backend  = RenderBackend::Vulkan;  // el que se puede arrancar
    bool          fellBack = false;                  // true si no es el pedido
    std::string   message;                           // motivo; vacío si no hubo fallback
};

// Resuelve el backend de arranque. NUNCA falla ni lanza: si el pedido no se
// puede usar —build sin DX12, máquina sin adaptador capaz, backend todavía
// incompleto— devuelve Vulkan y explica el motivo en `message`, que el llamante
// debe enseñar al usuario. Esta es la única puerta por la que se elige backend.
BackendSelection resolveRenderBackend(RenderBackend requested);

}  // namespace DonTopo
