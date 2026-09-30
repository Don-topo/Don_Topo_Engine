#pragma once

#include <cstddef>
#include <cstdint>

namespace DonTopo {

struct EditorContext;

// Live monitoring panel of the editor. It measures three things from three different
// places: the CPU frame with the ImGui clock, the GPU cost per pass with
// the timestamps the Renderer records, and the process RAM/CPU/VRAM with the Windows
// API. Around those three it draws the CONTEXT that explains them (what is
// in the scene and with which settings it is being drawn), which comes entirely from
// getters that the Renderer and the Scene already exposed.
//
// Zero cost with the panel closed: draw() turns off the Renderer's capture (not
// one query is recorded, no counters are read) and it queries nothing from the
// system. The process reads are the expensive ones and are cached at ~2 Hz, not
// once per frame.
//
// It is ONLY for the editor: none of this enters DonTopoCore or the
// exported runtime.
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
    // History samples of the graphs. 120 at 60 fps is 2 seconds.
    static constexpr int kHistory = 120;
    // Measured GPU passes, in pipeline order. The names live in the .cpp;
    // here only the slot where their measurement is frozen.
    static constexpr int kPasses = 9;

    // Process RAM/CPU/VRAM. Called by the panel's refresh, which is what
    // keeps the clock: it no longer decides here when it is due.
    void sampleProcess();

    bool  m_open = false;

    // History per FRAME. It is not drawn: it feeds the statistics (min, mean,
    // max and the 1% low), which stop meaning anything if they are computed over anything
    // other than individual frames (the 1% low IS the worst frame).
    float m_frameMsHistory[kHistory] = {};
    float m_fpsHistory[kHistory]     = {};
    int   m_histCursor               = 0;
    // Frames with data: until the buffer fills, only the valid part is looked at.
    int   m_histFilled               = 0;

    // History that is DRAWN, one point per refresh (1 Hz): 120 points are two
    // minutes of trend. A curve that advances 60 points per second cannot be
    // followed by eye; at this rate you can see where the frame comes from.
    float m_plotMsHistory[kHistory]  = {};
    float m_plotFpsHistory[kHistory] = {};
    int   m_plotCursor               = 0;
    int   m_plotFilled               = 0;
    // Accumulator of the current point. For the interval the WORST frame is drawn, not
    // the mean: a hitch lasting three frames vanishes in an average of
    // sixty, and it is exactly what is being looked for in this graph.
    float m_bucketMaxMs              = 0.0f;
    double m_bucketSumMs             = 0.0;
    int   m_bucketFrames             = 0;

    // ── What is SHOWN, frozen between refreshes ──────────────────────────────
    // The history keeps being fed every frame (a graph sampled at 1
    // Hz stops being a frame-time graph), but the NUMBERS and the
    // bars stay still for a second. A value that changes 60 times per
    // second cannot be read, and a bouncing bar hides exactly the
    // comparison it is there for.
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

    // ── Single panel clock (1 Hz) ────────────────────────────────────────────
    double   m_nextSampleTime = 0.0;
    float    m_workingSetMb   = 0.0f;
    float    m_peakWorkingMb  = 0.0f;
    float    m_cpuPercent     = 0.0f;
    float    m_gpuUsedMb      = 0.0f;
    float    m_gpuBudgetMb    = 0.0f;
    // Previous sample of platform::processStats().cpuSeconds; < 0 = there is no
    // sample yet (the first one gives no percentage).
    double   m_lastCpuSeconds = -1.0;
    double   m_lastCpuWall    = 0.0;
};

} // namespace DonTopo
