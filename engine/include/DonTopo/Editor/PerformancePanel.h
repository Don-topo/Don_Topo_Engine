#pragma once

#include <cstddef>
#include <cstdint>

namespace DonTopo {

struct EditorContext;

// Panel de monitorización en vivo del editor. Mide tres cosas de tres sitios
// distintos: el frame de CPU con el reloj de ImGui, el coste GPU por pass con
// los timestamps que graba el Renderer, y RAM/CPU/VRAM del proceso con API de
// Windows. Alrededor de esas tres pinta el CONTEXTO que las explica —qué hay
// en la escena y con qué ajustes se está dibujando—, que sale entero de
// getters que el Renderer y la Scene ya exponían.
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
    // Pases de GPU medidos, en orden de pipeline. Los nombres viven en el .cpp;
    // aquí solo el hueco donde se congela su medida.
    static constexpr int kPasses = 9;

    // RAM/CPU/VRAM del proceso. Lo llama el refresco del panel, que es quien
    // lleva el reloj: aquí ya no se decide cuándo toca.
    void sampleProcess();

    bool  m_open = false;

    // Historial por FRAME. No se pinta: alimenta los estadísticos (min, media,
    // max y el 1% low), que dejan de significar nada si se calculan sobre algo
    // que no sean frames sueltos — el 1% low ES el peor frame.
    float m_frameMsHistory[kHistory] = {};
    float m_fpsHistory[kHistory]     = {};
    int   m_histCursor               = 0;
    // Frames con datos: hasta llenar el buffer solo se mira lo válido.
    int   m_histFilled               = 0;

    // Historial que se PINTA, un punto por refresco (1 Hz): 120 puntos son dos
    // minutos de tendencia. Una curva que avanza 60 puntos por segundo no se
    // puede seguir con la vista; a este ritmo se ve de dónde viene el frame.
    float m_plotMsHistory[kHistory]  = {};
    float m_plotFpsHistory[kHistory] = {};
    int   m_plotCursor               = 0;
    int   m_plotFilled               = 0;
    // Acumulador del punto en curso. Del intervalo se pinta el PEOR frame, no
    // la media: un tirón que dura tres frames desaparece de una media de
    // sesenta, y es justo lo que se está buscando en esta gráfica.
    float m_bucketMaxMs              = 0.0f;
    double m_bucketSumMs             = 0.0;
    int   m_bucketFrames             = 0;

    // ── Lo que se ENSEÑA, congelado entre refrescos ──────────────────────────
    // El historial se sigue alimentando cada frame —una gráfica muestreada a 1
    // Hz deja de ser una gráfica de tiempo de frame—, pero los NÚMEROS y las
    // barras se quedan quietos un segundo. Un valor que cambia 60 veces por
    // segundo no se puede leer, y una barra que baila esconde justo la
    // comparación para la que está.
    float    m_showFps        = 0.0f;
    float    m_showFrameMs    = 0.0f;
    float    m_showMinMs      = 0.0f;
    float    m_showAvgMs      = 0.0f;
    float    m_showMaxMs      = 0.0f;
    float    m_showLowFps     = 0.0f;
    float    m_passMs[kPasses] = {};
    float    m_passTotal      = 0.0f;
    int      m_drawCalls      = 0;
    int      m_instances      = 0;
    int      m_culled         = 0;
    int      m_instanceOverflow = 0;
    size_t   m_slotObjects    = 0;
    size_t   m_slotObjectCap  = 0;
    size_t   m_slotSkinned    = 0;
    size_t   m_slotSkinnedCap = 0;
    size_t   m_sceneObjects   = 0;
    size_t   m_sceneMeshes    = 0;
    size_t   m_sceneLightNodes = 0;
    size_t   m_sceneLights    = 0;
    float    m_fpAvgPerCell   = 0.0f;
    uint32_t m_fpOverflowCells = 0;
    int      m_probes         = 0;
    double   m_probeMbEach    = 0.0;
    float    m_probeBakeMs    = 0.0f;

    // ── Reloj único del panel (1 Hz) ─────────────────────────────────────────
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
