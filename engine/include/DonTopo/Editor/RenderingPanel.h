#pragma once
#include "DonTopo/Editor/RenderSettingControls.h"
#include "DonTopo/Renderer/RenderBackend.h"

#include <functional>

namespace DonTopo {

struct EditorContext;

// Panel de los ajustes de render: ambiente, sombras, bloom, SSAO, SSR, niebla,
// motion blur, anti-aliasing, Forward+, sondas de reflexion y el selector de
// backend.
//
// Los 41 controles vivian dentro del BeginMenu("View") (H58). Un menu de ImGui
// se cierra al soltar el raton, asi que afinar el bloom o la niebla mirando el
// viewport obligaba a reabrirlo en cada retoque; acoplado se queda fijo y el
// efecto se ve mientras se arrastra el slider.
//
// Cada efecto va en su CollapsingHeader, como el panel de Performance. Solo el
// primero abre por defecto: con los once desplegados el panel no entra en una
// columna estrecha. ImGui recuerda en imgui.ini cuales dejo abiertos el
// usuario.
//
// Es SOLO del editor: nada de esto entra en DonTopoCore ni en el runtime
// exportado.
class RenderingPanel {
public:
    RenderingPanel()                                 = default;
    RenderingPanel(const RenderingPanel&)            = delete;
    RenderingPanel& operator=(const RenderingPanel&) = delete;

    // `active` es el backend con el que se creo el device de ESTE proceso y
    // `selected` el que el proyecto pide para el proximo: el panel dibuja los
    // dos y escribe el segundo, pero el dueño de ambos sigue siendo EditorUI,
    // que es quien los serializa. Van por parametro y no por el EditorContext
    // porque no los usa ningun otro panel.
    void draw(EditorContext& ctx, RenderBackend active, RenderBackend& selected);
    bool* GetOpenPtr() { return &m_open; }
    void  open() { m_open = true; }

private:
    bool m_open = false;

    // Los widgets con su undo. Miembro y no local de draw(): guarda el valor de
    // inicio del arrastre en curso, que tiene que sobrevivir entre frames.
    RenderSettingControls m_ctl;

    // Estado propio de dos controles, que estaba suelto entre los miembros de
    // EditorUI: la ventana del editor de ambiente, y el factor de supersampling
    // pendiente mientras se arrastra su slider (no se aplica hasta soltar,
    // porque cada cambio recrea los render targets).
    bool  m_environmentWindowOpen = false;
    float m_ssaaPendingFactor     = 2.0f;
    bool  m_ssaaSliderActive      = false;
};

} // namespace DonTopo
