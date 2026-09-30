#include "DonTopo/Editor/PerformancePanel.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Editor/GpuTimeFormat.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Renderer/EditorRenderer.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>

#include "DonTopo/Core/Platform.h"
#include <thread>

namespace DonTopo {

namespace {

// RAM/CPU/VRAM refresh. GetProcessMemoryInfo and QueryVideoMemoryInfo are
// kernel/driver calls: at 60 fps they are noticeable, at 1 Hz they are not.
//
// It is also the WINDOW of the CPU percentage, which is computed as the difference
// between two samples: with one second the figure stops jumping around, at the cost of
// taking a second to react. For RAM and VRAM it does not matter, since they are
// absolute values.
constexpr double kSampleInterval = 1.0;

// The panel's warning orange, in ONE place. The SSBO overflow and the slots at 90 %
// already used it by hand; now also the most expensive pass and the new
// warnings, so that "orange" always means the same thing.
const ImVec4 kWarn(1.0f, 0.6f, 0.2f, 1.0f);

// Writes the text flush against the right edge of the cell. A column of numbers
// with different widths cannot be read vertically, which is exactly what
// a table of times is for.
void rightAligned(const char* text, bool disabled)
{
    const float w     = ImGui::CalcTextSize(text).x;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
    if (disabled) ImGui::TextDisabled("%s", text);
    else          ImGui::TextUnformatted(text);
}

// One row of the GPU times table. A value <= 0 means "that pass has not
// run this frame" (effect off, or the capture does not yet have two frames).
// `hottest` marks the most expensive pass of the frame: of the ten numbers in the table
// it is the only one that can be acted on, so it is pointed out instead of
// forcing them to be compared by eye.
void gpuRow(const char* name, float ms, float totalMs, bool hottest)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    if (hottest) ImGui::TextColored(kWarn, "%s", name);
    else         ImGui::TextUnformatted(name);
    ImGui::TableSetColumnIndex(1);
    // Same format as the View menu, from GpuTimeFormat.h: the rule for what is
    // shown when there is NO measurement lives in one place (H57). Here it is also
    // dimmed, which in a table distinguishes rows without data at a glance.
    char buf[kGpuMsTextSize];
    gpuMsText(ms, buf, kGpuMsTextSize);
    rightAligned(buf, ms <= 0.0f);
    ImGui::TableSetColumnIndex(2);
    if (ms > 0.0f && totalMs > 0.0f)
    {
        const float frac = std::clamp(ms / totalMs, 0.0f, 1.0f);
        char pct[16];
        std::snprintf(pct, sizeof(pct), "%.1f %%", 100.0f * frac);
        // Proportional bar: the split is visible before reading any number.
        if (hottest) ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kWarn);
        ImGui::ProgressBar(frac, ImVec2(-1.0f, ImGui::GetTextLineHeight()), pct);
        if (hottest) ImGui::PopStyleColor();
    }
    else ImGui::TextDisabled("--");
}

} // namespace

PerformancePanel::~PerformancePanel() = default;

void PerformancePanel::sampleProcess()
{
    // The clock is kept by draw(): this is called ONLY on the refresh frame.
    // Before, it decided in here, and since the call was inside the "Process"
    // section, with that section collapsed the clock never advanced.
    const double now = ImGui::GetTime();
    const platform::ProcessStats st = platform::processStats();
    if (st.valid)
    {
        m_workingSetMb  = (float)st.workingSetMb;
        m_peakWorkingMb = (float)st.peakWorkingSetMb;
    }

    // Process CPU: kernel + user time consumed since the previous sample,
    // divided between wall time and cores. Without dividing by the cores, a process
    // with 8 saturated threads would show 800 %.
    if (m_lastCpuSeconds >= 0.0)
    {
        const double wall  = now - m_lastCpuWall;
        const double cores = (double)std::max(1u, std::thread::hardware_concurrency());
        if (wall > 0.0)
            m_cpuPercent = (float)std::clamp(100.0 * (st.cpuSeconds - m_lastCpuSeconds) / (wall * cores),
                                             0.0, 100.0);
    }
    m_lastCpuSeconds = st.cpuSeconds;
    m_lastCpuWall    = now;

    // Process VRAM and budget: only where the system provides it (DXGI).
    if (const auto vram = platform::gpuMemoryBudget())
    {
        m_gpuUsedMb   = (float)vram->usedMb;
        m_gpuBudgetMb = (float)vram->budgetMb;
    }
}

void PerformancePanel::draw(EditorContext& ctx)
{
    // FIRST: synchronize the Renderer's capture with the panel's state.
    // With the panel closed this leaves the frame exactly as if the feature did not
    // exist (no resets, no timestamps, no counters), and incidentally it covers the
    // case of closing with the window's X, which Begin writes into m_open.
    if (ctx.renderer) ctx.renderer->setPerfCaptureEnabled(m_open);
    if (!m_open) return;

    if (ImGui::Begin("Performance", &m_open))
    {
        const ImGuiIO& io = ImGui::GetIO();

        // ── CPU: framerate and frame time ────────────────────────────────────
        const float frameMs = io.DeltaTime * 1000.0f;
        m_frameMsHistory[m_histCursor] = frameMs;
        m_fpsHistory[m_histCursor]     = io.Framerate;
        m_histCursor = (m_histCursor + 1) % kHistory;
        if (m_histFilled < kHistory) m_histFilled++;

        // And to the point being formed for the graph, which advances at the pace
        // of the refresh and not of the frames.
        m_bucketMaxMs = std::max(m_bucketMaxMs, frameMs);
        m_bucketSumMs += (double)frameMs;
        ++m_bucketFrames;

        // ── Refresh: ONE clock for the whole panel ───────────────────────────
        // The two histories are fed every frame (above), but EVERYTHING
        // that is shown (numbers, bars and graphs) advances on this tick. Before,
        // only RAM/CPU/VRAM were cached and the rest flickered 60 times per
        // second, which is the difference between a datum and a blur.
        const double now     = ImGui::GetTime();
        const bool   refresh = now >= m_nextSampleTime;
        if (refresh) m_nextSampleTime = now + kSampleInterval;

        // The Renderer is resolved first of all: almost everything that is frozen
        // comes from it.
        if (!ctx.renderer)
        {
            ImGui::TextDisabled("No Renderer.");
            ImGui::End();
            return;
        }
        EditorRenderer& r = *ctx.renderer;

        if (refresh)
        {
            m_showFps     = io.Framerate;
            m_showFrameMs = frameMs;

            // Closes the graph point: the WORST frame of the interval on the
            // ms curve, and the mean FPS on the histogram. The worst frame is
            // what has to be seen on a time curve; FPS is read as a
            // rhythm, and there the worst case would mislead. The 1% low below still
            // comes from the per-frame history, which is the only thing that can
            // provide it.
            if (m_bucketFrames > 0)
            {
                const float avg = (float)(m_bucketSumMs / (double)m_bucketFrames);
                m_plotMsHistory[m_plotCursor]  = m_bucketMaxMs;
                m_plotFpsHistory[m_plotCursor] = avg > 0.0f ? 1000.0f / avg : 0.0f;
                m_plotCursor = (m_plotCursor + 1) % kHistory;
                if (m_plotFilled < kHistory) m_plotFilled++;
                m_bucketMaxMs  = 0.0f;
                m_bucketSumMs  = 0.0;
                m_bucketFrames = 0;
            }

            // History statistics. They come from the same 120 values the graph
            // already draws (not a single new measurement), and they say what the
            // graph does not let you read: the mean hides the hitches and the peak
            // hides the normal case.
            m_showMinMs = m_showMaxMs = m_showAvgMs = m_showLowFps = 0.0f;
            if (m_histFilled > 0)
            {
                m_showMinMs = m_frameMsHistory[0];
                m_showMaxMs = m_frameMsHistory[0];
                double sum = 0.0;
                float  sorted[kHistory];
                for (int i = 0; i < m_histFilled; ++i)
                {
                    const float v = m_frameMsHistory[i];
                    sorted[i] = v;
                    sum += v;
                    m_showMinMs = std::min(m_showMinMs, v);
                    m_showMaxMs = std::max(m_showMaxMs, v);
                }
                m_showAvgMs = (float)(sum / (double)m_histFilled);
                // 1% low: the frame at the 99th percentile in time, expressed in
                // FPS. It is the figure that betrays the micro-hitch the mean
                // swallows; with 120 samples it equals the worst frame of the last
                // two seconds.
                const int idx = (int)((float)(m_histFilled - 1) * 0.99f);
                std::nth_element(sorted, sorted + idx, sorted + m_histFilled);
                if (sorted[idx] > 0.0f) m_showLowFps = 1000.0f / sorted[idx];
            }

            // The GPU times come from frame N-2 (it is the slot whose fence was already
            // waited on this frame), so the first two frames after opening the
            // panel show "--". Nothing is blocked to bring them forward. The
            // capture keeps running EVERY frame: what goes at 1 Hz is the
            // reading that is drawn, not the measurement.
            m_passMs[0] = r.shadowGpuMs();
            m_passMs[1] = r.sceneGpuMs();
            m_passMs[2] = r.ssaoGpuMs();
            m_passMs[3] = r.forwardPlusGpuMs();
            m_passMs[4] = r.ssrGpuMs();
            m_passMs[5] = r.fogGpuMs();
            m_passMs[6] = r.motionBlurGpuMs();
            m_passMs[7] = r.bloomGpuMs();
            m_passMs[8] = r.aaGpuMs();
            m_passTotal = r.renderGpuMs();

            m_drawCalls        = r.statDrawCalls();
            m_instances        = r.statInstances();
            m_culled           = r.statCulled();
            m_instanceOverflow = r.statInstanceOverflow();
            const EditorRenderer::SlotUsage slots = r.slotUsage();
            m_slotObjects    = slots.objects;
            m_slotObjectCap  = slots.objectCapacity;
            m_slotSkinned    = slots.skinned;
            m_slotSkinnedCap = slots.skinnedCapacity;

            // Tree walk. It is not a system call and does not touch the GPU
            // (it is walking pointers that are already in cache), but at 1 Hz it does not matter
            // how big the scene is.
            m_sceneObjects = m_sceneMeshes = m_sceneLightNodes = 0;
            if (ctx.scene)
            {
                size_t nodes = 0;
                ctx.scene->getRoot().traverse([&](GameObject* n) {
                    ++nodes;
                    if (n->hasMesh())  ++m_sceneMeshes;
                    if (n->getLight()) ++m_sceneLightNodes;
                });
                // The root counts as a node in traverse and is not an object of the
                // scene: it is subtracted so that the number matches the
                // hierarchy seen in the Scene panel.
                m_sceneObjects = nodes > 0 ? nodes - 1 : 0;
            }
            m_sceneLights     = r.sceneLightTotal();
            m_fpAvgPerCell    = r.forwardPlusAvgPerCell();
            m_fpOverflowCells = r.forwardPlusOverflowCells();
            m_probes          = r.probeCount();
            m_probeMbEach     = (double)r.probeMemoryBytes() / (1024.0 * 1024.0);
            m_probeBakeMs     = r.lastProbeBakeMs();

            sampleProcess();
        }

        ImGui::Text("%.1f FPS   %.2f ms/frame (CPU)", m_showFps, m_showFrameMs);
        ImGui::TextDisabled("The whole panel (numbers, bars and graphs) advances every %.1f s.",
                            kSampleInterval);

        // Who sets the frame? The CPU MEAN (not the single frame,
        // which jumps) is compared against the GPU total. With Vsync the subtraction means
        // nothing: what is left over is waiting for the refresh, not work, and
        // everything comes out at 16 ms whatever you do.
        if (m_passTotal > 0.0f && m_histFilled > 0)
        {
            if (r.presentMode() == PresentMode::Vsync)
                ImGui::TextDisabled("CPU %.2f ms vs GPU %.2f ms: with Vsync the difference is "
                                    "waiting for the refresh; to measure, use Immediate.",
                                    m_showAvgMs, m_passTotal);
            else if (m_showAvgMs - m_passTotal <= 0.5f)
                ImGui::Text("GPU bound (CPU %.2f ms, GPU %.2f ms)", m_showAvgMs, m_passTotal);
            else
                ImGui::Text("CPU bound (+%.2f ms over the GPU: %.2f vs %.2f ms)",
                            m_showAvgMs - m_passTotal, m_showAvgMs, m_passTotal);
        }

        // ── CPU: history ─────────────────────────────────────────────────────
        ImGui::PushID("cpu");
        if (ImGui::CollapsingHeader("CPU (history)", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // The history is circular, so the offset is passed so that the
            // graph advances from left to right instead of jumping.
            // One point per refresh, not per frame. The history is circular,
            // so the offset is passed so that the graph advances from left
            // to right instead of jumping.
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "CPU %.2f ms", m_showFrameMs);
            ImGui::PlotLines("##frameMs", m_plotMsHistory, m_plotFilled,
                             m_plotFilled == kHistory ? m_plotCursor : 0,
                             overlay, 0.0f, 33.3f, ImVec2(-1.0f, 60.0f));
            std::snprintf(overlay, sizeof(overlay), "%.0f FPS", m_showFps);
            ImGui::PlotHistogram("##fps", m_plotFpsHistory, m_plotFilled,
                                 m_plotFilled == kHistory ? m_plotCursor : 0,
                                 overlay, 0.0f, 165.0f, ImVec2(-1.0f, 60.0f));
            ImGui::TextDisabled("One point per refresh: the WORST frame of every %.1f s in the\n"
                                "ms curve, the average FPS in the histogram. %d points = %.0f s.",
                                kSampleInterval, kHistory, kHistory * kSampleInterval);
            ImGui::Text("min %.2f ms   media %.2f ms   max %.2f ms",
                        m_showMinMs, m_showAvgMs, m_showMaxMs);
            ImGui::Text("1%% low: %.1f FPS", m_showLowFps);
            ImGui::TextDisabled("These four come from the PER-FRAME history (%d frames, %d\n"
                                "filled), not from the graph: the 1%% low is the single worst frame.",
                                kHistory, m_histFilled);
        }
        ImGui::PopID();

        ImGui::PushID("gpu");
        if (ImGui::CollapsingHeader("GPU per pass", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // The nine passes IN PIPELINE ORDER, which is information in itself:
            // it reads the way the frame is recorded. The table is sortable, but
            // the starting order is this one.
            struct PassRow { const char* name; float ms; };
            // The names, in the SAME order in which the refresh fills
            // m_passMs. If a pass is added both places have to be touched, and
            // that is why the static_assert below.
            static const char* const kPassNames[] = {
                "Shadows", "Scene", "AO (SSAO)", "Forward+ (cull)", "SSR",
                "Fog",  "Motion blur", "Bloom", "Anti-aliasing",
            };
            constexpr int kPassCount = (int)(sizeof(kPassNames) / sizeof(kPassNames[0]));
            static_assert(kPassCount == kPasses, "pass names and timings out of step");
            PassRow rows[kPassCount];
            for (int i = 0; i < kPassCount; ++i) rows[i] = {kPassNames[i], m_passMs[i]};

            // The most expensive pass of the frame. -1 while there is not a single measurement:
            // with no data there is nothing to highlight.
            int hottest = -1;
            for (int i = 0; i < kPassCount; ++i)
                if (rows[i].ms > 0.0f && (hottest < 0 || rows[i].ms > rows[hottest].ms))
                    hottest = i;
            const char* hottestName = hottest >= 0 ? rows[hottest].name : nullptr;

            if (ImGui::BeginTable("gpuPasses", 3,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Sortable |
                    ImGuiTableFlags_SortTristate))
            {
                // Sorting by name says nothing; by % it is the same as by ms
                // (they share the total). Only the ms column sorts.
                ImGui::TableSetupColumn("Pass",    ImGuiTableColumnFlags_NoSort);
                ImGui::TableSetupColumn("ms",      ImGuiTableColumnFlags_DefaultSort);
                ImGui::TableSetupColumn("% total", ImGuiTableColumnFlags_NoSort);
                ImGui::TableHeadersRow();

                // It is reordered EVERY frame, not only when SpecsDirty: the times
                // change on every pass, so an order computed once would
                // end up lying on the next frame.
                if (const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs())
                {
                    if (specs->SpecsCount > 0)
                    {
                        const bool asc = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                        std::stable_sort(rows, rows + kPassCount,
                                         [asc](const PassRow& a, const PassRow& b) {
                                             return asc ? a.ms < b.ms : a.ms > b.ms;
                                         });
                    }
                }

                for (int i = 0; i < kPassCount; ++i)
                    gpuRow(rows[i].name, rows[i].ms, m_passTotal, rows[i].name == hottestName);
                gpuRow("TOTAL (without UI)", m_passTotal, m_passTotal, false);
                ImGui::EndTable();
            }
            ImGui::TextDisabled("Disabled passes and the first two frames show as '--'.\n"
                                "In orange, the most expensive pass. Click 'ms' to sort.");
        }
        ImGui::PopID();

        // ── Draw counters ────────────────────────────────────────────────────
        // Own ID scope per section: a header and a widget that are
        // named the same in two different sections collide, and the symptom is
        // that one of the two stops responding.
        ImGui::PushID("dibujo");
        if (ImGui::CollapsingHeader("Drawing", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Draw calls:  %d", m_drawCalls);
            ImGui::Text("Instances:  %d", m_instances);
            ImGui::Text("Culled:     %d", m_culled);
            ImGui::TextDisabled("Scene pass only (instanced statics + skinned).");

            // It must always be 0, so it is only drawn when it is NOT: a
            // permanent row at zero is noise, and this number only matters on the
            // day it stops being so. Same criterion as the Forward+ overflowed
            // cells warning.
            if (m_instanceOverflow > 0)
                ImGui::TextColored(kWarn,
                                   "%d objects without room in the SSBO: they lose their shadow",
                                   m_instanceOverflow);

            // Object slots. Here and not in the View menu because it is a
            // diagnostic, not a setting: what it says is whether deleting is
            // returning the gaps. If after several Play/Stop cycles the number
            // goes up instead of returning to the starting one, there is a leak.
            auto slotRow = [](const char* label, size_t used, size_t capacity) {
                if (capacity == 0) {
                    // Backend without a hard cap: the vector grows, so the number
                    // is only useful compared with itself.
                    ImGui::Text("%s %zu (no cap)", label, used);
                    return;
                }
                const float uso = (float)used / (float)capacity;
                if (uso >= 0.9f)
                    ImGui::TextColored(kWarn, "%s %zu / %zu", label, used, capacity);
                else
                    ImGui::Text("%s %zu / %zu", label, used, capacity);
            };
            slotRow("Slots GPU:   ", m_slotObjects, m_slotObjectCap);
            slotRow("Slots skinned:", m_slotSkinned, m_slotSkinnedCap);
            ImGui::TextDisabled("Past the cap, the object is drawn with the global descriptor\n"
                                "block: it looks flat, but it does not overflow the heap.");
        }
        ImGui::PopID();

        // ── Scene: what has to be drawn ──────────────────────────────────────
        // The "why" of the numbers above: how many objects and how many
        // lights there are, and what part of them is being lost to a cap.
        ImGui::PushID("escena");
        if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ctx.scene)
            {
                ImGui::Text("Objects:     %zu  (%zu with a mesh)", m_sceneObjects, m_sceneMeshes);
                ImGui::Text("With light:  %zu", m_sceneLightNodes);
            }
            else
            {
                ImGui::TextDisabled("No scene.");
            }

            // The total is set by whoever assembles the frame, not the backend: it is the only one
            // that sees the DISCARDED lights. collectLights keeps the
            // first MAX_LIGHTS and silently throws away the rest.
            if (m_sceneLights > (size_t)MAX_LIGHTS)
                ImGui::TextColored(kWarn, "Lights:      %zu / %d: the rest neither light nor "
                                          "cast shadows", m_sceneLights, MAX_LIGHTS);
            else
                ImGui::Text("Lights:      %zu / %d", m_sceneLights, MAX_LIGHTS);

            // Forward+ only counts if it is on: when off it records not a single
            // dispatch and its counters mean nothing.
            if (r.forwardPlusMode() != RendererState::FpMode::Off)
            {
                ImGui::Text("Lights/cell: %.1f  (Forward+ %s)",
                            m_fpAvgPerCell,
                            r.forwardPlusMode() == RendererState::FpMode::Tiled ? "tiled"
                                                                                : "clustered");
                if (m_fpOverflowCells > 0)
                    ImGui::TextColored(kWarn, "%u overflowing cells (they lose lights)",
                                       m_fpOverflowCells);
            }
            else
            {
                ImGui::TextDisabled("Forward+: off.");
            }

            // Reflection probes: what is expensive about a probe is its VRAM and its bake,
            // not its per-frame cost, so they go here and not in the table of
            // passes. The per-probe figure is given by the ACTIVE BACKEND: the two
            // store different things (H51).
            if (m_probes > 0)
            {
                ImGui::Text("Probes:      %d  (%.2f MB each, %.1f MB in total)",
                            m_probes, m_probeMbEach, m_probeMbEach * (double)m_probes);
                // A "0.00 ms" would be read as an instant bake instead of as
                // "never baked" (H56), so they are distinguished.
                char b[kGpuMsTextSize];
                if (m_probeBakeMs <= 0.0f)
                    ImGui::TextDisabled("Last bake: not baked");
                else
                    ImGui::Text("Last bake: %s ms of GPU",
                                gpuMsText(m_probeBakeMs, b, kGpuMsTextSize));
            }
            else
            {
                ImGui::TextDisabled("Probes: none in the scene.");
            }
        }
        ImGui::PopID();

        // ── Active configuration ─────────────────────────────────────────────
        // It is not a settings panel (that is Rendering): it is the CONTEXT of the
        // measurements above. A pass time without knowing at what resolution and
        // with what AA it was taken cannot be compared with yesterday's.
        ImGui::PushID("config");
        if (ImGui::CollapsingHeader("Active configuration"))
        {
            const uint32_t rw = r.renderWidth(), rh = r.renderHeight();
            const uint32_t uw = r.uiWidth(),     uh = r.uiHeight();
            ImGui::Text("Render:      %u x %u  (%.2f Mpx)", rw, rh,
                        (double)rw * (double)rh / 1e6);
            // With SSAA the internal render is LARGER than the output, and that factor
            // is what explains the cost of the scene pass.
            if (rw != uw || rh != uh)
                ImGui::Text("Output:      %u x %u  (SSAA x%.2f)", uw, uh, r.ssaaFactor());
            else
                ImGui::Text("Output:      %u x %u", uw, uh);

            const char* aa = "none";
            switch (r.aaMode())
            {
                case RendererState::AaMode::None: aa = "none"; break;
                case RendererState::AaMode::Fxaa: aa = "FXAA";    break;
                case RendererState::AaMode::Ssaa: aa = "SSAA";    break;
                case RendererState::AaMode::Msaa: aa = "MSAA";    break;
                case RendererState::AaMode::Taa:  aa = "TAA";     break;
            }
            if (r.aaMode() == RendererState::AaMode::Msaa)
                ImGui::Text("Anti-alias:  %s x%d", aa, r.msaaSamples());
            else
                ImGui::Text("Anti-alias:  %s", aa);

            ImGui::Text("Shadows:     %d x %d texels, range %.0f",
                        r.shadowResolution(), r.shadowResolution(), r.shadowDistance());

            // What was REQUESTED, not what is effective: the backend falls back to Vsync without warning if
            // the mode is not supported, and that cannot be read from here.
            const char* pm = "Vsync";
            switch (r.presentMode())
            {
                case PresentMode::Vsync:     pm = "Vsync";     break;
                case PresentMode::Mailbox:   pm = "Mailbox";   break;
                case PresentMode::Immediate: pm = "Immediate"; break;
            }
            ImGui::Text("Presentation: %s (requested)", pm);
            if (r.isWireframeMode())
                ImGui::TextColored(kWarn, "Wireframe mode on: the timings are not those of the "
                                          "normal render");
            ImGui::TextDisabled("It is changed in the Rendering panel; here it is only read, to\n"
                                "compare two measurements knowing what they were taken with.");
        }
        ImGui::PopID();

        // ── Process: RAM, CPU, VRAM ──────────────────────────────────────────
        ImGui::PushID("proceso");
        if (ImGui::CollapsingHeader("Process", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("RAM (working set): %.1f MB  (peak %.1f MB)", m_workingSetMb, m_peakWorkingMb);
            ImGui::Text("Process CPU:       %.1f %%", m_cpuPercent);
            if (m_gpuBudgetMb > 0.0f)
            {
                // Same threshold as the object slots: at 90 % of the
                // budget the driver already starts pushing resources out to RAM.
                const float uso = std::clamp(m_gpuUsedMb / m_gpuBudgetMb, 0.0f, 1.0f);
                if (uso >= 0.9f)
                    ImGui::TextColored(kWarn, "Process VRAM:      %.1f MB / %.1f MB of budget",
                                       m_gpuUsedMb, m_gpuBudgetMb);
                else
                    ImGui::Text("Process VRAM:      %.1f MB / %.1f MB of budget",
                                m_gpuUsedMb, m_gpuBudgetMb);
                if (uso >= 0.9f) ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kWarn);
                ImGui::ProgressBar(uso, ImVec2(-1.0f, 0.0f));
                if (uso >= 0.9f) ImGui::PopStyleColor();
            }
            else
            {
                ImGui::TextDisabled("VRAM: not available on this platform.");
            }
            ImGui::TextDisabled("Kernel/driver readings, not per frame.");
        }
        ImGui::PopID();
    }
    ImGui::End();
}

} // namespace DonTopo
