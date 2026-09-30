#pragma once
#include "DonTopo/Editor/RenderSettingControls.h"
#include "DonTopo/Renderer/RenderBackend.h"

#include <functional>

namespace DonTopo {

struct EditorContext;

// Panel for the render settings: ambient, shadows, bloom, SSAO, SSR, fog,
// motion blur, anti-aliasing, Forward+, reflection probes and the backend
// selector.
//
// The 41 controls used to live inside the BeginMenu("View") (H58). An ImGui menu
// closes when the mouse is released, so tuning bloom or fog while watching the
// viewport forced reopening it on every tweak; docked, it stays fixed and the
// effect is visible while the slider is dragged.
//
// Each effect goes in its own CollapsingHeader, like the Performance panel. Only the
// first one opens by default: with all eleven expanded the panel does not fit in a
// narrow column. ImGui remembers in imgui.ini which ones the user left open.
//
// It is ONLY for the editor: none of this enters DonTopoCore or the exported
// runtime.
class RenderingPanel {
public:
    RenderingPanel()                                 = default;
    RenderingPanel(const RenderingPanel&)            = delete;
    RenderingPanel& operator=(const RenderingPanel&) = delete;

    // `active` is the backend with which THIS process's device was created and
    // `selected` the one the project requests for the next one: the panel draws both
    // and writes the second, but the owner of both is still EditorUI,
    // which is what serializes them. They go as parameters and not through the EditorContext
    // because no other panel uses them.
    void draw(EditorContext& ctx, RenderBackend active, RenderBackend& selected);
    bool* GetOpenPtr() { return &m_open; }
    void  open() { m_open = true; }

private:
    bool m_open = false;

    // The widgets with their undo. A member and not a local of draw(): it stores the drag's
    // starting value, which has to survive between frames.
    RenderSettingControls m_ctl;

    // State of two controls of its own, which used to be loose among the members of
    // EditorUI: the ambient editor window, and the pending supersampling factor
    // while its slider is dragged (it is not applied until release, because each
    // change recreates the render targets).
    bool  m_environmentWindowOpen = false;
    float m_ssaaPendingFactor     = 2.0f;
    bool  m_ssaaSliderActive      = false;
};

} // namespace DonTopo
