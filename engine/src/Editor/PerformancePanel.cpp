#include "DonTopo/Editor/PerformancePanel.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Renderer/Renderer.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>
// Se enlazan aquí y no desde CMake: son librerías de importación del SDK de
// Windows y este es el único traductor que las usa.
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "dxgi.lib")
#endif

namespace DonTopo {

namespace {

// Refresco de RAM/CPU/VRAM. GetProcessMemoryInfo y QueryVideoMemoryInfo son
// llamadas al kernel/driver: a 60 fps se notan, a 2 Hz no.
constexpr double kSampleInterval = 0.5;

// Una fila de la tabla de tiempos GPU. Un valor <= 0 significa "ese pass no ha
// corrido este frame" (efecto apagado, o la captura aún no tiene dos frames).
void gpuRow(const char* name, float ms, float totalMs)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(name);
    ImGui::TableSetColumnIndex(1);
    if (ms > 0.0f) ImGui::Text("%.3f", ms);
    else           ImGui::TextDisabled("--");
    ImGui::TableSetColumnIndex(2);
    if (ms > 0.0f && totalMs > 0.0f) ImGui::Text("%.1f %%", 100.0f * ms / totalMs);
    else                             ImGui::TextDisabled("--");
}

} // namespace

PerformancePanel::~PerformancePanel()
{
#ifdef _WIN32
    if (m_dxgiAdapter)
    {
        ((IDXGIAdapter3*)m_dxgiAdapter)->Release();
        m_dxgiAdapter = nullptr;
    }
#endif
}

void PerformancePanel::sampleProcess()
{
    const double now = ImGui::GetTime();
    if (now < m_nextSampleTime) return;
    m_nextSampleTime = now + kSampleInterval;

#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
    {
        m_workingSetMb  = (float)((double)pmc.WorkingSetSize / (1024.0 * 1024.0));
        m_peakWorkingMb = (float)((double)pmc.PeakWorkingSetSize / (1024.0 * 1024.0));
    }

    // CPU del proceso: tiempo de kernel + usuario consumido desde la muestra
    // anterior, repartido entre el tiempo de pared y los núcleos. Sin dividir
    // por los núcleos, un proceso con 8 hilos saturados marcaría 800 %.
    FILETIME ftCreate{}, ftExit{}, ftKernel{}, ftUser{};
    if (GetProcessTimes(GetCurrentProcess(), &ftCreate, &ftExit, &ftKernel, &ftUser))
    {
        ULARGE_INTEGER k{}, u{};
        k.LowPart  = ftKernel.dwLowDateTime;  k.HighPart  = ftKernel.dwHighDateTime;
        u.LowPart  = ftUser.dwLowDateTime;    u.HighPart  = ftUser.dwHighDateTime;
        const uint64_t ticks = (uint64_t)k.QuadPart + (uint64_t)u.QuadPart;
        if (m_lastCpuTicks != 0)
        {
            const double wall = now - m_lastCpuWall;
            SYSTEM_INFO si{};
            GetSystemInfo(&si);
            const double cores = si.dwNumberOfProcessors > 0 ? (double)si.dwNumberOfProcessors : 1.0;
            if (wall > 0.0)
            {
                // Los FILETIME van en unidades de 100 ns: 1e7 por segundo.
                const double busy = (double)(ticks - m_lastCpuTicks) / 1e7;
                m_cpuPercent = (float)std::clamp(100.0 * busy / (wall * cores), 0.0, 100.0);
            }
        }
        m_lastCpuTicks = ticks;
        m_lastCpuWall  = now;
    }

    // VRAM: DXGI da el uso REAL del proceso y el presupuesto que le concede el
    // sistema, que es justo lo que interesa vigilar. Vulkan por sí solo no lo
    // expone sin VK_EXT_memory_budget, y esa extensión habría que habilitarla
    // en la creación del device (Core), así que se mide desde aquí.
    if (!m_dxgiTried)
    {
        m_dxgiTried = true;
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) && factory)
        {
            IDXGIAdapter1* adapter1 = nullptr;
            // Adaptador 0: el motor no expone el LUID del device de Vulkan, y en
            // una máquina de un solo GPU dedicado es el mismo.
            if (SUCCEEDED(factory->EnumAdapters1(0, &adapter1)) && adapter1)
            {
                IDXGIAdapter3* adapter3 = nullptr;
                if (SUCCEEDED(adapter1->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&adapter3)))
                {
                    m_dxgiAdapter = adapter3;
                }
                adapter1->Release();
            }
            factory->Release();
        }
    }
    if (m_dxgiAdapter)
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (SUCCEEDED(((IDXGIAdapter3*)m_dxgiAdapter)->QueryVideoMemoryInfo(
                0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        {
            m_gpuUsedMb   = (float)((double)info.CurrentUsage / (1024.0 * 1024.0));
            m_gpuBudgetMb = (float)((double)info.Budget / (1024.0 * 1024.0));
        }
    }
#endif
}

void PerformancePanel::draw(EditorContext& ctx)
{
    // Lo PRIMERO: sincronizar la captura del Renderer con el estado del panel.
    // Con el panel cerrado esto deja el frame exactamente como si la feature no
    // existiera (ni resets, ni timestamps, ni contadores), y de paso cubre el
    // caso de cerrar con la X de la ventana, que Begin escribe en m_open.
    if (ctx.renderer) ctx.renderer->setPerfCaptureEnabled(m_open);
    if (!m_open) return;

    if (ImGui::Begin("Performance", &m_open))
    {
        const ImGuiIO& io = ImGui::GetIO();

        // ── CPU: framerate y tiempo de frame ─────────────────────────────────
        const float frameMs = io.DeltaTime * 1000.0f;
        m_frameMsHistory[m_histCursor] = frameMs;
        m_fpsHistory[m_histCursor]     = io.Framerate;
        m_histCursor = (m_histCursor + 1) % kHistory;
        if (m_histFilled < kHistory) m_histFilled++;

        ImGui::Text("%.1f FPS   %.2f ms/frame (CPU)", io.Framerate, frameMs);

        // El historial es circular, así que se pasa el offset para que la
        // gráfica avance de izquierda a derecha en vez de saltar.
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "CPU %.2f ms", frameMs);
        ImGui::PlotLines("##frameMs", m_frameMsHistory, m_histFilled,
                         m_histFilled == kHistory ? m_histCursor : 0,
                         overlay, 0.0f, 33.3f, ImVec2(-1.0f, 60.0f));
        std::snprintf(overlay, sizeof(overlay), "%.0f FPS", io.Framerate);
        ImGui::PlotHistogram("##fps", m_fpsHistory, m_histFilled,
                             m_histFilled == kHistory ? m_histCursor : 0,
                             overlay, 0.0f, 165.0f, ImVec2(-1.0f, 60.0f));

        // ── GPU por pass ─────────────────────────────────────────────────────
        ImGui::Separator();
        if (!ctx.renderer)
        {
            ImGui::TextDisabled("Sin Renderer.");
            ImGui::End();
            return;
        }
        EditorRenderer& r = *ctx.renderer;

        // Los tiempos GPU salen del frame N-2 (es el slot cuya fence ya esperó
        // este frame), así que los dos primeros frames tras abrir el panel
        // muestran "--". No se bloquea nada para adelantarlos.
        const float total = r.renderGpuMs();
        if (ImGui::CollapsingHeader("GPU por pass", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::BeginTable("gpuPasses", 3,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Pass");
                ImGui::TableSetupColumn("ms");
                ImGui::TableSetupColumn("% total");
                ImGui::TableHeadersRow();

                gpuRow("Sombras",          r.shadowGpuMs(),      total);
                gpuRow("Escena",           r.sceneGpuMs(),       total);
                gpuRow("AO (SSAO)",        r.ssaoGpuMs(),        total);
                gpuRow("Forward+ (cull)",  r.forwardPlusGpuMs(), total);
                gpuRow("SSR",              r.ssrGpuMs(),         total);
                gpuRow("Niebla",           r.fogGpuMs(),         total);
                gpuRow("Bloom",            r.bloomGpuMs(),       total);
                gpuRow("Anti-aliasing",    r.aaGpuMs(),          total);
                gpuRow("TOTAL (sin UI)",   total,                total);
                ImGui::EndTable();
            }
            ImGui::TextDisabled("Los pasos apagados y los dos primeros frames salen como '--'.");
        }

        // ── Contadores de dibujo ─────────────────────────────────────────────
        if (ImGui::CollapsingHeader("Dibujo", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Draw calls:  %d", r.statDrawCalls());
            ImGui::Text("Instancias:  %d", r.statInstances());
            ImGui::Text("Culleados:   %d", r.statCulled());
            ImGui::TextDisabled("Solo el pass de escena (estaticos instanciados + skinned).");

            // Debe ser siempre 0, asi que solo se pinta cuando NO lo es: una
            // fila permanente a cero es ruido, y este numero solo importa el
            // dia que deja de serlo. Mismo criterio que el aviso de celdas
            // desbordadas de Forward+.
            if (r.statInstanceOverflow() > 0)
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                   "%d objetos sin sitio en el SSBO: pierden sombra",
                                   r.statInstanceOverflow());

            // Ranuras de objeto. Aqui y no en el menu View porque es un
            // diagnostico, no un ajuste: lo que dice es si borrar esta
            // devolviendo los huecos. Si tras varios ciclos Play/Stop el numero
            // sube en vez de volver al de partida, hay una fuga.
            const EditorRenderer::SlotUsage slots = r.slotUsage();
            auto slotRow = [](const char* label, size_t used, size_t capacity) {
                if (capacity == 0) {
                    // Backend sin tope duro: el vector crece, asi que el numero
                    // solo es util comparado consigo mismo.
                    ImGui::Text("%s %zu (sin tope)", label, used);
                    return;
                }
                const float uso = (float)used / (float)capacity;
                if (uso >= 0.9f)
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s %zu / %zu", label,
                                       used, capacity);
                else
                    ImGui::Text("%s %zu / %zu", label, used, capacity);
            };
            slotRow("Slots GPU:   ", slots.objects, slots.objectCapacity);
            slotRow("Slots skinned:", slots.skinned, slots.skinnedCapacity);
            ImGui::TextDisabled("Pasado el tope, el objeto se dibuja con el bloque global\n"
                                "de descriptores: sale plano, pero no se sale del heap.");
        }

        // ── Proceso: RAM, CPU, VRAM ──────────────────────────────────────────
        if (ImGui::CollapsingHeader("Proceso", ImGuiTreeNodeFlags_DefaultOpen))
        {
            sampleProcess();
#ifdef _WIN32
            ImGui::Text("RAM (working set): %.1f MB  (pico %.1f MB)", m_workingSetMb, m_peakWorkingMb);
            ImGui::Text("CPU del proceso:   %.1f %%", m_cpuPercent);
            if (m_gpuBudgetMb > 0.0f)
            {
                ImGui::Text("VRAM del proceso:  %.1f MB / %.1f MB de presupuesto",
                            m_gpuUsedMb, m_gpuBudgetMb);
                ImGui::ProgressBar(std::clamp(m_gpuUsedMb / m_gpuBudgetMb, 0.0f, 1.0f),
                                   ImVec2(-1.0f, 0.0f));
            }
            else
            {
                ImGui::TextDisabled("VRAM: no disponible (sin DXGI).");
            }
            ImGui::TextDisabled("Refresco cada %.1f s, no por frame.", kSampleInterval);
#else
            ImGui::TextDisabled("RAM/CPU/VRAM del proceso: solo en Windows.");
#endif
        }
    }
    ImGui::End();
}

} // namespace DonTopo
