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
    sel.message  = "DirectX 12: este build se compiló con DTE_ENABLE_D3D12=OFF. Se arranca con Vulkan.";
    return sel;
#else
    const D3D12::SupportInfo support = D3D12::querySupport();
    if (!support.supported)
    {
        sel.fellBack = true;
        sel.message  = "DirectX 12 no está disponible en esta máquina (" + support.error +
                      "). Se arranca con Vulkan.";
        return sel;
    }

    // La máquina lo soporta: se arranca con él. El aviso NO es un fallback, es
    // la advertencia de hasta dónde llega el backend hoy.
    sel.backend  = RenderBackend::D3D12;
    sel.fellBack = false;
    sel.message  = "Backend DirectX 12 activo (" + support.adapterName +
                  "). Corre el editor con la escena del proyecto, pero todavía "
                  "no dibuja la UI 2D del juego ni las sondas de reflexión, y el "
                  "juego exportado sigue arrancando con Vulkan.";
    return sel;
#endif
}

}  // namespace DonTopo
