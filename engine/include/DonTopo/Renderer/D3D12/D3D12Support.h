#pragma once

// Only exists when the DirectX 12 backend is compiled (DTE_ENABLE_D3D12=ON).
// Consumers must guard their calls with the same #ifdef.
#ifdef DT_D3D12_ENABLED

#include <string>

namespace DonTopo::D3D12 {

// Result of asking the system whether there is a DX12-capable adapter, without
// creating the device. Used by the editor's backend selector: offering
// DirectX 12 on a machine that does not support it would lead to a failure at
// startup, which is exactly the moment when it is hardest to warn the user.
struct SupportInfo {
    bool supported = false;
    std::string adapterName;  // Description of the chosen adapter; empty if there is none
    std::string error;        // Reason; only has content when supported == false
};

// Enumerates the adapters through DXGI and returns the first one that accepts
// D3D_FEATURE_LEVEL_11_0. Discards software adapters (WARP). Creates no
// device and leaves no live object behind after returning.
SupportInfo querySupport();

// UTF-16 -> UTF-8. An adapter description and the D3D12MA statistics JSON
// arrive as wchar_t, and the rest of the engine speaks UTF-8 std::string
// (the log, project.json, ImGui). Empty string if `wide` is nullptr or empty.
//
// It lives here because it was written TWICE (this one and another in D3D12Renderer.cpp,
// byte for byte the same except for the brace style), and both convert what
// ends up in the same log (H48).
std::string narrow(const wchar_t* wide);

}  // namespace DonTopo::D3D12

#endif  // DT_D3D12_ENABLED
