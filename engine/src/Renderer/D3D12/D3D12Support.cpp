#include "DonTopo/Renderer/D3D12/D3D12Support.h"

#ifdef DT_D3D12_ENABLED

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace DonTopo::D3D12 {

namespace {

using Microsoft::WRL::ComPtr;



}  // namespace

std::string narrow(const wchar_t* wide) {
    if (wide == nullptr || wide[0] == L'\0') {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

SupportInfo querySupport() {
    SupportInfo info;

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        info.error = "CreateDXGIFactory2 falló: no hay DXGI disponible en este sistema";
        return info;
    }

    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }

        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) {
            continue;
        }
        // WARP renderiza por CPU: arrancaría, pero a una velocidad que no sirve
        // como backend de un editor. Si no hay GPU real, mejor decir que no.
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }

        // Con pDevice a nullptr, D3D12CreateDevice solo comprueba el soporte:
        // no crea nada y no hay nada que liberar.
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        __uuidof(ID3D12Device), nullptr))) {
            info.supported = true;
            info.adapterName = narrow(desc.Description);
            return info;
        }
    }

    info.error = "ningún adaptador hardware soporta D3D_FEATURE_LEVEL_11_0";
    return info;
}

}  // namespace DonTopo::D3D12

#endif  // DT_D3D12_ENABLED
