#include "DonTopo/Renderer/RenderBackend.h"

#ifdef DT_D3D12_ENABLED
#include "DonTopo/Renderer/D3D12/D3D12Support.h"
#endif

namespace DonTopo {

const char* renderBackendName(RenderBackend backend)
{
    switch (backend)
    {
        case RenderBackend::D3D12: return "DirectX 12";
        default:                   return "Vulkan";
    }
}

RenderBackend renderBackendFromName(const std::string& name, bool& ok)
{
    ok = true;
    if (name == "Vulkan")     return RenderBackend::Vulkan;
    if (name == "DirectX 12") return RenderBackend::D3D12;
    ok = false;
    return RenderBackend::Vulkan;
}

BackendSelection resolveRenderBackend(RenderBackend requested)
{
    BackendSelection sel;
    sel.backend = RenderBackend::Vulkan;

    if (requested == RenderBackend::Vulkan)
        return sel;

#ifndef DT_D3D12_ENABLED
    sel.fellBack = true;
    sel.message  = "DirectX 12: this build was compiled with DTE_ENABLE_D3D12=OFF. Starting with Vulkan.";
    return sel;
#else
    const D3D12::SupportInfo support = D3D12::querySupport();
    if (!support.supported)
    {
        sel.fellBack = true;
        sel.message  = "DirectX 12 is not available on this machine (" + support.error +
                      "). Starting with Vulkan.";
        return sel;
    }

    // La máquina lo soporta: se arranca con él. El aviso NO es un fallback, es
    // la advertencia de hasta dónde llega el backend hoy.
    sel.backend  = RenderBackend::D3D12;
    sel.fellBack = false;
    sel.message  = "DirectX 12 backend active (" + support.adapterName +
                  "). It runs the editor and the exported game.";
    return sel;
#endif
}

}  // namespace DonTopo
