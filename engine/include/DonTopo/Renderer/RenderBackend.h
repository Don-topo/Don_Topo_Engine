#pragma once

#include <string>

namespace DonTopo {

// Render backend the process starts with. It CANNOT be changed at
// runtime: the device, the swapchain and all the GPU resources hang from it,
// so changing it forces an application restart.
enum class RenderBackend {
    Vulkan,
    D3D12,
};

// Names persisted in project.json. Same as aaMode/fpMode: the
// setting is saved by NAME, never by index, so that reordering the combo does not
// change what is already saved.
const char* renderBackendName(RenderBackend backend);

// ok = false if the name is none of today's (file from a future version,
// or hand-edited): the caller falls back to Vulkan and leaves it in the Log.
RenderBackend renderBackendFromName(const std::string& name, bool& ok);

// Which backend will actually be used, as opposed to the one that was requested.
struct BackendSelection {
    RenderBackend backend  = RenderBackend::Vulkan;  // the one that can be started
    bool          fellBack = false;                  // true if it is not the requested one
    // What to tell the user. It is filled in TWO different cases: when there was a
    // fallback (fellBack, with the reason) and when the chosen backend starts
    // but with limited scope. The caller shows it whenever it is not
    // empty, without looking at fellBack.
    std::string   message;
};

// Resolves the startup backend. NEVER fails or throws: if the requested one
// cannot be used (build without DX12, machine without a capable adapter) it returns Vulkan and
// explains the reason in `message`. This is the only door through which the
// backend is chosen.
BackendSelection resolveRenderBackend(RenderBackend requested);

}  // namespace DonTopo
