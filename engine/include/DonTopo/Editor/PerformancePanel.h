#pragma once

#include <cstdint>

namespace DonTopo {

struct EditorContext;

// Panel de monitorización en vivo del editor. Mide tres cosas de tres sitios
// distintos: el frame de CPU con el reloj de ImGui, el coste GPU por pass con
// los timestamps que graba el Renderer, y RAM/CPU/VRAM del proceso con API de
// Windows.
//
// Coste cero con el panel cerrado: draw() apaga la captura del Renderer (no se
// graba ni una query, ni se leen los contadores) y no consulta nada del
// sistema. Las lecturas del proceso son las caras y van cacheadas a ~2 Hz, no
// una por frame.
//
// Es SOLO del editor: nada de esto entra en DonTopoCore ni en el runtime
// exportado.
class PerformancePanel {
public:
    PerformancePanel() = default;
    ~PerformancePanel();
    PerformancePanel(const PerformancePanel&)            = delete;
    PerformancePanel& operator=(const PerformancePanel&) = delete;

    void draw(EditorContext& ctx);
    bool* GetOpenPtr() { return &m_open; }
    void open() { m_open = true; }

private:
    // Muestras del historial de las gráficas. 120 a 60 fps son 2 segundos.
    static constexpr int kHistory = 120;

    // RAM/CPU/VRAM del proceso. Solo hace trabajo si ha pasado el intervalo de
    // refresco; el resto de frames devuelve lo cacheado.
    void sampleProcess();

    bool  m_open = false;

    float m_frameMsHistory[kHistory] = {};
    float m_fpsHistory[kHistory]     = {};
    int   m_histCursor               = 0;
    // Frames con datos: hasta llenar el buffer la gráfica solo pinta lo válido.
    int   m_histFilled               = 0;

    // ── Muestreo cacheado del proceso (~2 Hz) ────────────────────────────────
    double   m_nextSampleTime = 0.0;
    float    m_workingSetMb   = 0.0f;
    float    m_peakWorkingMb  = 0.0f;
    float    m_cpuPercent     = 0.0f;
    float    m_gpuUsedMb      = 0.0f;
    float    m_gpuBudgetMb    = 0.0f;
    // Contadores previos de GetProcessTimes, en unidades de 100 ns.
    uint64_t m_lastCpuTicks   = 0;
    double   m_lastCpuWall     = 0.0;
    // IDXGIAdapter3 cacheado (void* para no arrastrar dxgi.h al header). Lo
    // libera el destructor.
    void*    m_dxgiAdapter    = nullptr;
    bool     m_dxgiTried      = false;
};

} // namespace DonTopo
