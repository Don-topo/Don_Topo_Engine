#include "DonTopo/Editor/PerformancePanel.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Editor/GpuTimeFormat.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/Scene.h"
// Fuera del #ifdef de Windows: draw() habla con el Renderer en TODAS las
// plataformas; lo unico que es de Windows son las lecturas del proceso.
#include "DonTopo/Renderer/EditorRenderer.h"
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
// llamadas al kernel/driver: a 60 fps se notan, a 1 Hz no.
//
// Es tambien la VENTANA del porcentaje de CPU, que se calcula por diferencia
// entre dos muestras: con un segundo la cifra deja de bailar, a cambio de
// tardar un segundo en reaccionar. Para RAM y VRAM da igual, que son valores
// absolutos.
constexpr double kSampleInterval = 1.0;

// El naranja de aviso del panel, en UN solo sitio. Ya lo usaban a mano el
// overflow del SSBO y los slots al 90 %; ahora también el pass más caro y los
// avisos nuevos, para que "naranja" signifique siempre lo mismo.
const ImVec4 kWarn(1.0f, 0.6f, 0.2f, 1.0f);

// Escribe el texto pegado al borde derecho de la celda. Una columna de números
// con anchos distintos no se puede leer en vertical, que es justo para lo que
// sirve una tabla de tiempos.
void rightAligned(const char* text, bool disabled)
{
    const float w     = ImGui::CalcTextSize(text).x;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
    if (disabled) ImGui::TextDisabled("%s", text);
    else          ImGui::TextUnformatted(text);
}

// Una fila de la tabla de tiempos GPU. Un valor <= 0 significa "ese pass no ha
// corrido este frame" (efecto apagado, o la captura aún no tiene dos frames).
// `hottest` marca el pass más caro del frame: de los diez números de la tabla
// es el único sobre el que se puede actuar, así que se señala en vez de
// obligar a compararlos a ojo.
void gpuRow(const char* name, float ms, float totalMs, bool hottest)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    if (hottest) ImGui::TextColored(kWarn, "%s", name);
    else         ImGui::TextUnformatted(name);
    ImGui::TableSetColumnIndex(1);
    // Mismo formato que el menú View, desde GpuTimeFormat.h: la regla de qué se
    // enseña cuando NO hay medida vive en un solo sitio (H57). Aquí además se
    // atenúa, que en una tabla distingue de un vistazo las filas sin dato.
    char buf[kGpuMsTextSize];
    gpuMsText(ms, buf, kGpuMsTextSize);
    rightAligned(buf, ms <= 0.0f);
    ImGui::TableSetColumnIndex(2);
    if (ms > 0.0f && totalMs > 0.0f)
    {
        const float frac = std::clamp(ms / totalMs, 0.0f, 1.0f);
        char pct[16];
        std::snprintf(pct, sizeof(pct), "%.1f %%", 100.0f * frac);
        // Barra proporcional: el reparto se ve antes de leer ningún número.
        if (hottest) ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kWarn);
        ImGui::ProgressBar(frac, ImVec2(-1.0f, ImGui::GetTextLineHeight()), pct);
        if (hottest) ImGui::PopStyleColor();
    }
    else ImGui::TextDisabled("--");
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
    // El reloj lo lleva draw(): esto se llama SOLO en el frame del refresco.
    // Antes decidía aquí dentro, y como la llamada estaba dentro de la sección
    // "Proceso", con esa sección plegada el reloj no avanzaba nunca.
#ifdef _WIN32
    const double now = ImGui::GetTime();
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

        // Y al punto que se está formando para la gráfica, que avanza al ritmo
        // del refresco y no al de los frames.
        m_bucketMaxMs = std::max(m_bucketMaxMs, frameMs);
        m_bucketSumMs += (double)frameMs;
        ++m_bucketFrames;

        // ── Refresco: UN solo reloj para todo el panel ───────────────────────
        // Los dos historiales se alimentan cada frame (arriba), pero TODO lo
        // que se enseña —números, barras y gráficas— avanza en este tick. Antes
        // solo RAM/CPU/VRAM iban cacheados y el resto parpadeaba 60 veces por
        // segundo, que es la diferencia entre un dato y un borrón.
        const double now     = ImGui::GetTime();
        const bool   refresh = now >= m_nextSampleTime;
        if (refresh) m_nextSampleTime = now + kSampleInterval;

        // Se resuelve el Renderer antes de nada: casi todo lo que se congela
        // sale de él.
        if (!ctx.renderer)
        {
            ImGui::TextDisabled("Sin Renderer.");
            ImGui::End();
            return;
        }
        EditorRenderer& r = *ctx.renderer;

        if (refresh)
        {
            m_showFps     = io.Framerate;
            m_showFrameMs = frameMs;

            // Cierra el punto de la gráfica: el PEOR frame del intervalo en la
            // curva de ms, y los FPS medios en el histograma. El peor frame es
            // lo que hay que ver en una curva de tiempo; los FPS se leen como
            // ritmo, y ahí el peor caso engañaría. El 1% low de abajo sigue
            // saliendo del historial por frame, que es lo único que puede
            // dárselo.
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

            // Estadísticos del historial. Salen de los mismos 120 valores que
            // ya pinta la gráfica —ni una medida nueva—, y dicen lo que la
            // gráfica no deja leer: la media esconde los tirones y el pico
            // esconde el caso normal.
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
                // 1% low: el frame del percentil 99 en tiempo, expresado en
                // FPS. Es la cifra que delata el micro-tirón que la media se
                // traga; con 120 muestras equivale al peor frame de los últimos
                // dos segundos.
                const int idx = (int)((float)(m_histFilled - 1) * 0.99f);
                std::nth_element(sorted, sorted + idx, sorted + m_histFilled);
                if (sorted[idx] > 0.0f) m_showLowFps = 1000.0f / sorted[idx];
            }

            // Los tiempos GPU salen del frame N-2 (es el slot cuya fence ya
            // esperó este frame), así que los dos primeros frames tras abrir el
            // panel muestran "--". No se bloquea nada para adelantarlos. La
            // captura sigue corriendo CADA frame: lo que va a 1 Hz es la
            // lectura que se pinta, no la medida.
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

            // Recorrido del árbol. No es una llamada al sistema ni toca la GPU
            // —es pasear punteros que ya están en caché—, pero a 1 Hz da igual
            // lo grande que sea la escena.
            m_sceneObjects = m_sceneMeshes = m_sceneLightNodes = 0;
            if (ctx.scene)
            {
                size_t nodes = 0;
                ctx.scene->getRoot().traverse([&](GameObject* n) {
                    ++nodes;
                    if (n->hasMesh())  ++m_sceneMeshes;
                    if (n->getLight()) ++m_sceneLightNodes;
                });
                // La raíz cuenta como nodo en traverse y no es un objeto de la
                // escena: se descuenta para que el número cuadre con la
                // jerarquía que se ve en el panel Scene.
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
        ImGui::TextDisabled("Todo el panel —numeros, barras y graficas— avanza cada %.1f s.",
                            kSampleInterval);

        // ¿Quién marca el frame? Se compara la MEDIA de CPU (no el frame
        // suelto, que salta) contra el total de GPU. Con Vsync la resta no
        // significa nada: lo que sobra es espera al refresco, no trabajo, y
        // todo sale a 16 ms hagas lo que hagas.
        if (m_passTotal > 0.0f && m_histFilled > 0)
        {
            if (r.presentMode() == PresentMode::Vsync)
                ImGui::TextDisabled("CPU %.2f ms vs GPU %.2f ms — con Vsync la diferencia es "
                                    "espera al refresco: para medir, pon Immediate.",
                                    m_showAvgMs, m_passTotal);
            else if (m_showAvgMs - m_passTotal <= 0.5f)
                ImGui::Text("Limitado por GPU (CPU %.2f ms, GPU %.2f ms)", m_showAvgMs, m_passTotal);
            else
                ImGui::Text("Limitado por CPU (+%.2f ms sobre la GPU: %.2f vs %.2f ms)",
                            m_showAvgMs - m_passTotal, m_showAvgMs, m_passTotal);
        }

        // ── CPU: historial ───────────────────────────────────────────────────
        ImGui::PushID("cpu");
        if (ImGui::CollapsingHeader("CPU (historial)", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // El historial es circular, así que se pasa el offset para que la
            // gráfica avance de izquierda a derecha en vez de saltar.
            // Un punto por refresco, no por frame. El historial es circular,
            // asi que se pasa el offset para que la grafica avance de izquierda
            // a derecha en vez de saltar.
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "CPU %.2f ms", m_showFrameMs);
            ImGui::PlotLines("##frameMs", m_plotMsHistory, m_plotFilled,
                             m_plotFilled == kHistory ? m_plotCursor : 0,
                             overlay, 0.0f, 33.3f, ImVec2(-1.0f, 60.0f));
            std::snprintf(overlay, sizeof(overlay), "%.0f FPS", m_showFps);
            ImGui::PlotHistogram("##fps", m_plotFpsHistory, m_plotFilled,
                                 m_plotFilled == kHistory ? m_plotCursor : 0,
                                 overlay, 0.0f, 165.0f, ImVec2(-1.0f, 60.0f));
            ImGui::TextDisabled("Un punto por refresco: el PEOR frame de cada %.1f s en la\n"
                                "curva de ms, los FPS medios en el histograma. %d puntos = %.0f s.",
                                kSampleInterval, kHistory, kHistory * kSampleInterval);
            ImGui::Text("min %.2f ms   media %.2f ms   max %.2f ms",
                        m_showMinMs, m_showAvgMs, m_showMaxMs);
            ImGui::Text("1%% low: %.1f FPS", m_showLowFps);
            ImGui::TextDisabled("Estos cuatro salen del historial POR FRAME (%d frames, %d\n"
                                "llenos), no de la grafica: el 1%% low es el peor frame suelto.",
                                kHistory, m_histFilled);
        }
        ImGui::PopID();

        ImGui::PushID("gpu");
        if (ImGui::CollapsingHeader("GPU por pass", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // Los nueve pases EN ORDEN DE PIPELINE, que es informacion en si
            // misma: se lee como se graba el frame. La tabla es ordenable, pero
            // el orden de partida es este.
            struct PassRow { const char* name; float ms; };
            // Los nombres, en el MISMO orden en el que el refresco llena
            // m_passMs. Si se añade un pase hay que tocar los dos sitios, y por
            // eso el static_assert de debajo.
            static const char* const kPassNames[] = {
                "Sombras", "Escena", "AO (SSAO)", "Forward+ (cull)", "SSR",
                "Niebla",  "Motion blur", "Bloom", "Anti-aliasing",
            };
            constexpr int kPassCount = (int)(sizeof(kPassNames) / sizeof(kPassNames[0]));
            static_assert(kPassCount == kPasses, "nombres y medidas de pase descuadrados");
            PassRow rows[kPassCount];
            for (int i = 0; i < kPassCount; ++i) rows[i] = {kPassNames[i], m_passMs[i]};

            // El pass más caro del frame. -1 mientras no haya ni una medida:
            // sin datos no hay nada que destacar.
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
                // Ordenar por nombre no dice nada; por % es lo mismo que por ms
                // (comparten el total). Solo la columna de ms ordena.
                ImGui::TableSetupColumn("Pass",    ImGuiTableColumnFlags_NoSort);
                ImGui::TableSetupColumn("ms",      ImGuiTableColumnFlags_DefaultSort);
                ImGui::TableSetupColumn("% total", ImGuiTableColumnFlags_NoSort);
                ImGui::TableHeadersRow();

                // Se reordena CADA frame, no solo cuando SpecsDirty: los tiempos
                // cambian en cada vuelta, asi que un orden calculado una vez se
                // quedaria mintiendo al frame siguiente.
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
                gpuRow("TOTAL (sin UI)", m_passTotal, m_passTotal, false);
                ImGui::EndTable();
            }
            ImGui::TextDisabled("Los pasos apagados y los dos primeros frames salen como '--'.\n"
                                "En naranja, el pass mas caro. Click en 'ms' para ordenar.");
        }
        ImGui::PopID();

        // ── Contadores de dibujo ─────────────────────────────────────────────
        // Scope de IDs propio por seccion: una cabecera y un widget que se
        // llamen igual en dos secciones distintas colisionan, y el sintoma es
        // que uno de los dos deja de responder.
        ImGui::PushID("dibujo");
        if (ImGui::CollapsingHeader("Dibujo", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Draw calls:  %d", m_drawCalls);
            ImGui::Text("Instancias:  %d", m_instances);
            ImGui::Text("Culleados:   %d", m_culled);
            ImGui::TextDisabled("Solo el pass de escena (estaticos instanciados + skinned).");

            // Debe ser siempre 0, asi que solo se pinta cuando NO lo es: una
            // fila permanente a cero es ruido, y este numero solo importa el
            // dia que deja de serlo. Mismo criterio que el aviso de celdas
            // desbordadas de Forward+.
            if (m_instanceOverflow > 0)
                ImGui::TextColored(kWarn,
                                   "%d objetos sin sitio en el SSBO: pierden sombra",
                                   m_instanceOverflow);

            // Ranuras de objeto. Aqui y no en el menu View porque es un
            // diagnostico, no un ajuste: lo que dice es si borrar esta
            // devolviendo los huecos. Si tras varios ciclos Play/Stop el numero
            // sube en vez de volver al de partida, hay una fuga.
            auto slotRow = [](const char* label, size_t used, size_t capacity) {
                if (capacity == 0) {
                    // Backend sin tope duro: el vector crece, asi que el numero
                    // solo es util comparado consigo mismo.
                    ImGui::Text("%s %zu (sin tope)", label, used);
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
            ImGui::TextDisabled("Pasado el tope, el objeto se dibuja con el bloque global\n"
                                "de descriptores: sale plano, pero no se sale del heap.");
        }
        ImGui::PopID();

        // ── Escena: lo que hay que dibujar ───────────────────────────────────
        // El "por que" de los numeros de arriba: cuantos objetos y cuantas
        // luces hay, y que parte de ellos se esta perdiendo por un tope.
        ImGui::PushID("escena");
        if (ImGui::CollapsingHeader("Escena", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ctx.scene)
            {
                ImGui::Text("Objetos:     %zu  (%zu con malla)", m_sceneObjects, m_sceneMeshes);
                ImGui::Text("Con luz:     %zu", m_sceneLightNodes);
            }
            else
            {
                ImGui::TextDisabled("Sin escena.");
            }

            // El total lo pone quien monta el frame, no el backend: es el unico
            // que ve las luces DESCARTADAS. collectLights se queda con las
            // primeras MAX_LIGHTS y tira el resto en silencio.
            if (m_sceneLights > (size_t)MAX_LIGHTS)
                ImGui::TextColored(kWarn, "Luces:       %zu / %d — el resto ni ilumina ni "
                                          "proyecta sombra", m_sceneLights, MAX_LIGHTS);
            else
                ImGui::Text("Luces:       %zu / %d", m_sceneLights, MAX_LIGHTS);

            // Forward+ solo cuenta si esta encendido: apagado no graba ni un
            // dispatch y sus contadores no significan nada.
            if (r.forwardPlusMode() != RendererState::FpMode::Off)
            {
                ImGui::Text("Luces/celda: %.1f  (Forward+ %s)",
                            m_fpAvgPerCell,
                            r.forwardPlusMode() == RendererState::FpMode::Tiled ? "tiled"
                                                                                : "clustered");
                if (m_fpOverflowCells > 0)
                    ImGui::TextColored(kWarn, "%u celdas desbordadas (pierden luces)",
                                       m_fpOverflowCells);
            }
            else
            {
                ImGui::TextDisabled("Forward+: apagado.");
            }

            // Sondas de reflexion: lo caro de una sonda es su VRAM y su bake,
            // no su coste por frame, asi que van aqui y no en la tabla de
            // pases. La cifra por sonda la da el BACKEND ACTIVO: los dos
            // guardan cosas distintas (H51).
            if (m_probes > 0)
            {
                ImGui::Text("Sondas:      %d  (%.2f MB c/u, %.1f MB en total)",
                            m_probes, m_probeMbEach, m_probeMbEach * (double)m_probes);
                // Un "0.00 ms" se leeria como bake instantaneo en vez de como
                // "nunca se ha horneado" (H56), asi que se distingue.
                char b[kGpuMsTextSize];
                if (m_probeBakeMs <= 0.0f)
                    ImGui::TextDisabled("Ultimo bake: sin bakear");
                else
                    ImGui::Text("Ultimo bake: %s ms de GPU",
                                gpuMsText(m_probeBakeMs, b, kGpuMsTextSize));
            }
            else
            {
                ImGui::TextDisabled("Sondas: ninguna en la escena.");
            }
        }
        ImGui::PopID();

        // ── Configuración activa ─────────────────────────────────────────────
        // No es un panel de ajustes (eso es Rendering): es el CONTEXTO de las
        // medidas de arriba. Un tiempo de pass sin saber a que resolucion y con
        // que AA se tomo no se puede comparar con el de ayer.
        ImGui::PushID("config");
        if (ImGui::CollapsingHeader("Configuracion activa"))
        {
            const uint32_t rw = r.renderWidth(), rh = r.renderHeight();
            const uint32_t uw = r.uiWidth(),     uh = r.uiHeight();
            ImGui::Text("Render:      %u x %u  (%.2f Mpx)", rw, rh,
                        (double)rw * (double)rh / 1e6);
            // Con SSAA el render interno es MAYOR que la salida, y ese factor
            // es lo que explica el coste del pass de escena.
            if (rw != uw || rh != uh)
                ImGui::Text("Salida:      %u x %u  (SSAA x%.2f)", uw, uh, r.ssaaFactor());
            else
                ImGui::Text("Salida:      %u x %u", uw, uh);

            const char* aa = "ninguno";
            switch (r.aaMode())
            {
                case RendererState::AaMode::None: aa = "ninguno"; break;
                case RendererState::AaMode::Fxaa: aa = "FXAA";    break;
                case RendererState::AaMode::Ssaa: aa = "SSAA";    break;
                case RendererState::AaMode::Msaa: aa = "MSAA";    break;
                case RendererState::AaMode::Taa:  aa = "TAA";     break;
            }
            if (r.aaMode() == RendererState::AaMode::Msaa)
                ImGui::Text("Anti-alias:  %s x%d", aa, r.msaaSamples());
            else
                ImGui::Text("Anti-alias:  %s", aa);

            ImGui::Text("Sombras:     %d x %d texeles, alcance %.0f",
                        r.shadowResolution(), r.shadowResolution(), r.shadowDistance());

            // Lo PEDIDO, no lo efectivo: el backend cae a Vsync sin avisar si
            // el modo no esta soportado, y eso no se puede leer desde aqui.
            const char* pm = "Vsync";
            switch (r.presentMode())
            {
                case PresentMode::Vsync:     pm = "Vsync";     break;
                case PresentMode::Mailbox:   pm = "Mailbox";   break;
                case PresentMode::Immediate: pm = "Immediate"; break;
            }
            ImGui::Text("Presentacion: %s (pedido)", pm);
            if (r.isWireframeMode())
                ImGui::TextColored(kWarn, "Modo alambre activo: los tiempos no son los del "
                                          "render normal");
            ImGui::TextDisabled("Se cambia en el panel Rendering; aqui solo se lee, para\n"
                                "poder comparar dos medidas sabiendo con que se tomaron.");
        }
        ImGui::PopID();

        // ── Proceso: RAM, CPU, VRAM ──────────────────────────────────────────
        ImGui::PushID("proceso");
        if (ImGui::CollapsingHeader("Proceso", ImGuiTreeNodeFlags_DefaultOpen))
        {
#ifdef _WIN32
            ImGui::Text("RAM (working set): %.1f MB  (pico %.1f MB)", m_workingSetMb, m_peakWorkingMb);
            ImGui::Text("CPU del proceso:   %.1f %%", m_cpuPercent);
            if (m_gpuBudgetMb > 0.0f)
            {
                // Mismo umbral que las ranuras de objeto: al 90 % del
                // presupuesto el driver ya empieza a echar recursos a RAM.
                const float uso = std::clamp(m_gpuUsedMb / m_gpuBudgetMb, 0.0f, 1.0f);
                if (uso >= 0.9f)
                    ImGui::TextColored(kWarn, "VRAM del proceso:  %.1f MB / %.1f MB de presupuesto",
                                       m_gpuUsedMb, m_gpuBudgetMb);
                else
                    ImGui::Text("VRAM del proceso:  %.1f MB / %.1f MB de presupuesto",
                                m_gpuUsedMb, m_gpuBudgetMb);
                if (uso >= 0.9f) ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kWarn);
                ImGui::ProgressBar(uso, ImVec2(-1.0f, 0.0f));
                if (uso >= 0.9f) ImGui::PopStyleColor();
            }
            else
            {
                ImGui::TextDisabled("VRAM: no disponible (sin DXGI).");
            }
            ImGui::TextDisabled("Lecturas del kernel/driver, no por frame.");
#else
            ImGui::TextDisabled("RAM/CPU/VRAM del proceso: solo en Windows.");
#endif
        }
        ImGui::PopID();
    }
    ImGui::End();
}

} // namespace DonTopo
