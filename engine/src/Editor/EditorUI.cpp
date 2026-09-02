#include "DonTopo/Editor/EditorUI.h"

#ifdef DT_D3D12_ENABLED
#include <d3d12.h>
#include <imgui_impl_dx12.h>
#endif
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Editor/GpuTimeFormat.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Files/FileManager.h"
#include "DonTopo/Scripting/ScriptManager.h"
#include "DonTopo/Scripting/ScriptBindings.h"
#include "DonTopo/Editor/ScriptEditorPanel.h"
#include "DonTopo/Editor/GameExporter.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <ImGuiFileDialog.h>
#include <cassert>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include "DonTopo/Renderer/EditorRenderer.h"

namespace DonTopo {

namespace {
// Ventana de capas de colisión abierta o no. Vive aquí y no en EditorUI porque
// es estado de UI puro —qué ventana está visible—, no un ajuste del proyecto, y
// el header del editor queda fuera del alcance de esta feature. Hay un solo
// EditorUI por proceso.
bool g_showLayerMatrix = false;

// Capa que el usuario ha pedido borrar y espera confirmación; -1 = ninguna.
// Borrar RENUMERA (ver PhysicsManager::removeLayer), así que no es un cambio
// que se pueda deshacer con Ctrl+Z: se pregunta antes.
int g_layerPendienteDeBorrar = -1;

// Ventana de capas de colisión. Función libre y NO parte de drawMenuBar: si se
// dibujara desde ahí saldría antes que el dockspace y ImGui no la dejaría
// acoplar con el resto de paneles. La llama draw() justo después de
// drawDockSpace().
//
// onChanged guarda los ajustes en el project.json (EditorUI::saveProjectSettings).
void drawCollisionLayersWindow(DonTopo::PhysicsManager* physics,
                               const std::function<void()>& onChanged)
{
    using DonTopo::PhysicsManager;
    if (!g_showLayerMatrix) return;

    ImGui::SetNextWindowSize(ImVec2(760.0f, 540.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Collision Layers", &g_showLayerMatrix))
    {
        if (!physics)
        {
            ImGui::TextDisabled("Sin PhysicsManager: no hay capas que editar.");
            ImGui::End();
            return;
        }

        ImGui::TextWrapped(
            "Capas de colision del proyecto. La matriz es SIMETRICA: marcar (a,b) "
            "marca tambien (b,a), por eso solo se dibuja la mitad superior. Todo "
            "marcado = sin filtros, el comportamiento por defecto.");
        ImGui::Separator();

        const int total = physics->layerCount();

        // --- Lista de capas: nombre + borrar --------------------------------
        for (int i = 0; i < total; ++i)
        {
            ImGui::PushID(i);
            ImGui::Text("%2d", i);
            ImGui::SameLine();

            char buf[64] = {};
            std::snprintf(buf, sizeof(buf), "%s", physics->getLayerName(i).c_str());
            ImGui::SetNextItemWidth(220.0f);
            // Se escribe al manager en cada tecla (para que las etiquetas de la
            // matriz y del collider se vean al vuelo) pero el project.json solo
            // se guarda al CONFIRMAR: si no, se reescribiria letra a letra.
            if (ImGui::InputText("##nombreCapa", buf, sizeof(buf)))
                physics->setLayerName(i, buf);
            if (ImGui::IsItemDeactivatedAfterEdit())
                onChanged();

            ImGui::SameLine();
            // La capa 0 es la de respaldo de los borrados: no se puede quitar.
            ImGui::BeginDisabled(i == 0);
            if (ImGui::SmallButton("x"))
                g_layerPendienteDeBorrar = i;
            ImGui::EndDisabled();
            if (i == 0)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(la capa por defecto no se borra)");
            }
            ImGui::PopID();
        }

        ImGui::BeginDisabled(total >= PhysicsManager::kLayerCount);
        if (ImGui::Button("Add Layer"))
        {
            const int nueva = physics->addLayer("Layer " + std::to_string(total));
            if (nueva >= 0) onChanged();
        }
        ImGui::EndDisabled();
        if (total >= PhysicsManager::kLayerCount)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("maximo %d capas", PhysicsManager::kLayerCount);
        }

        // --- Confirmacion de borrado ----------------------------------------
        if (g_layerPendienteDeBorrar > 0)
            ImGui::OpenPopup("Borrar capa");
        if (ImGui::BeginPopupModal("Borrar capa", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            const int capa = g_layerPendienteDeBorrar;
            ImGui::Text("Borrar la capa %d (\"%s\")?", capa,
                        capa > 0 ? physics->getLayerName(capa).c_str() : "");
            ImGui::TextWrapped(
                "Los colliders que la usaban pasaran a la capa 0, las capas de "
                "encima bajaran un indice y la matriz perdera su fila y su "
                "columna. NO se puede deshacer con Ctrl+Z.");
            ImGui::Separator();
            if (ImGui::Button("Borrar"))
            {
                if (physics->removeLayer(capa)) onChanged();
                g_layerPendienteDeBorrar = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancelar"))
            {
                g_layerPendienteDeBorrar = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // --- Matriz ---------------------------------------------------------
        ImGui::Separator();
        ImGui::BeginChild("matrizCapas", ImVec2(0.0f, 0.0f), false,
                          ImGuiWindowFlags_HorizontalScrollbar);

        const float anchoEtiqueta = 200.0f;
        const float anchoCelda    = ImGui::GetFrameHeight() + 6.0f;

        ImGui::Dummy(ImVec2(1.0f, 1.0f));
        for (int b = 0; b < total; ++b)
        {
            ImGui::SameLine(anchoEtiqueta + b * anchoCelda);
            ImGui::Text("%d", b);
        }

        for (int a = 0; a < total; ++a)
        {
            const std::string nombre = physics->getLayerName(a);
            const std::string fila =
                std::to_string(a) + (nombre.empty() ? std::string() : ": " + nombre);
            ImGui::TextUnformatted(fila.c_str());

            // Solo b >= a: la celda simetrica la escribe setLayerCollision, y
            // dibujar las dos daria dos controles para el mismo dato.
            for (int b = a; b < total; ++b)
            {
                ImGui::SameLine(anchoEtiqueta + b * anchoCelda);
                bool activo = physics->getLayerCollision(a, b);
                ImGui::PushID(a * PhysicsManager::kLayerCount + b);
                if (ImGui::Checkbox("##celda", &activo))
                {
                    physics->setLayerCollision(a, b, activo);
                    onChanged();
                }
                ImGui::PopID();
            }
        }

        ImGui::EndChild();
    }
    ImGui::End();
}
} // namespace

EditorUI::EditorUI()
    : m_sceneFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_exportDialog(std::make_unique<IGFD::FileDialog>())
    , m_skyboxDialog(std::make_unique<IGFD::FileDialog>())
    , m_scriptEditor(std::make_unique<ScriptEditorPanel>())
{
    m_scriptEditor->setLogCallback([this](const std::string& msg) { m_logPanel.push(msg); });
    // Liberar los recursos GPU del subárbol justo antes de desengancharlo: lo
    // hacía el Renderer al fijar la raíz de escena, y ahora lo cablea el
    // editor. El lambda se ejecuta mucho después, con el backend ya puesto.
    setOnDelete([this](GameObject* node) {
        if (m_renderer)
            m_renderer->removeGameObject(node);
    });
}

EditorUI::~EditorUI() = default;

void EditorUI::setRenderer(std::unique_ptr<EditorRenderer> renderer)
{
    m_renderer = std::move(renderer);

    // El backend llama de vuelta por aquí para grabar el pase de interfaz.
    // Tiene que quedar puesto ANTES de que arranque la presentación, y este es
    // el primer momento en que hay backend al que decírselo: el editor ya no lo
    // construye, se lo dan.
    if (m_renderer)
        m_renderer->setUiLayer(this);
}

EditorRenderer& EditorUI::renderer() { return *m_renderer; }

// ─── UiLayer: backend de ImGui ───────────────────────────────────────────────

void EditorUI::initUi(const InitInfo& info)
{
    m_api = info.api;

#ifdef DT_D3D12_ENABLED
    if (info.api == GraphicsApi::D3D12) {
        initUiD3D12(info);
        return;
    }
#endif

    // Los handles llegan como enteros opacos (UiLayer no incluye vulkan.h para
    // que el editor pueda dibujarse también con DirectX 12): aquí se recuperan
    // sus tipos reales, que es donde de verdad se conocen.
    m_uiDevice = reinterpret_cast<VkDevice>(info.device);

    // Pool dedicado para ImGui (necesita FREE_DESCRIPTOR_SET_BIT).
    // La API nueva (sept 2025) usa SAMPLER + SAMPLED_IMAGE separados en AddTexture().
    VkDescriptorPoolSize poolSizes[3]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 16;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_SAMPLER;
    poolSizes[1].descriptorCount = 16;
    poolSizes[2].type            = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    poolSizes[2].descriptorCount = 16;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets       = 48;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes    = poolSizes;
    if (vkCreateDescriptorPool(m_uiDevice, &poolInfo, nullptr, &m_uiDescPool) != VK_SUCCESS)
        throw std::runtime_error("failed to create ImGui descriptor pool!");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();

    // Sin instalar callbacks GLFW propios: ImGui los sondea en NewFrame
    ImGui_ImplGlfw_InitForVulkan(info.window, false);

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion                       = VK_API_VERSION_1_0;
    initInfo.Instance                         = reinterpret_cast<VkInstance>(info.instance);
    initInfo.PhysicalDevice                   = reinterpret_cast<VkPhysicalDevice>(info.physicalDevice);
    initInfo.Device                           = m_uiDevice;
    initInfo.QueueFamily                      = info.queueFamily;
    initInfo.Queue                            = reinterpret_cast<VkQueue>(info.queue);
    initInfo.DescriptorPool                   = m_uiDescPool;
    initInfo.MinImageCount                    = 2;
    initInfo.ImageCount                       = info.imageCount;
    initInfo.PipelineInfoMain.RenderPass      = reinterpret_cast<VkRenderPass>(info.renderPass);
    initInfo.PipelineInfoMain.MSAASamples     = VK_SAMPLE_COUNT_1_BIT;
    ImGui_ImplVulkan_Init(&initInfo);

    printf("ImGui init OK\n"); fflush(stdout);
}

void EditorUI::shutdownUi()
{
#ifdef DT_D3D12_ENABLED
    if (m_api == GraphicsApi::D3D12) {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        return;
    }
#endif

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    if (m_uiDescPool)
    {
        vkDestroyDescriptorPool(m_uiDevice, m_uiDescPool, nullptr);
        m_uiDescPool = VK_NULL_HANDLE;
    }
}

uint64_t EditorUI::registerUiTexture(uint64_t sampler, uint64_t view)
{
#ifdef DT_D3D12_ENABLED
    if (m_api == GraphicsApi::D3D12) {
        // En DirectX 12 la textura ya viene con su descriptor hecho: el backend
        // lo creó en el heap que la interfaz comparte, y ese valor ES lo que
        // ImGui::Image trata como identificador. No hay nada que registrar.
        (void)view;
        return sampler;
    }
#endif

    const VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
        reinterpret_cast<VkSampler>(sampler), reinterpret_cast<VkImageView>(view),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return reinterpret_cast<uint64_t>(set);
}

void EditorUI::unregisterUiTexture(uint64_t handle)
{
#ifdef DT_D3D12_ENABLED
    if (m_api == GraphicsApi::D3D12) {
        // Nada que soltar: el descriptor es del backend, no de la interfaz.
        (void)handle;
        return;
    }
#endif

    ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(handle));
}

void EditorUI::buildUiFrame(uint64_t viewportTexture, GameObject* sceneRoot,
                            const glm::mat4& cameraView)
{
#ifdef DT_D3D12_ENABLED
    if (m_api == GraphicsApi::D3D12)
        ImGui_ImplDX12_NewFrame();
    else
#endif
        ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    draw(viewportTexture, sceneRoot, cameraView);

    ImGui::Render();
}

void EditorUI::recordUi(void* commandList)
{
#ifdef DT_D3D12_ENABLED
    if (m_api == GraphicsApi::D3D12) {
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(),
                                      static_cast<ID3D12GraphicsCommandList*>(commandList));
        return;
    }
#endif

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),
                                    static_cast<VkCommandBuffer>(commandList));
}

#ifdef DT_D3D12_ENABLED
void EditorUI::initUiD3D12(const InitInfo& info)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();

    // InitForOther y no InitForVulkan: con DirectX 12 el backend de GLFW no
    // tiene que preparar nada de la API gráfica, solo el input.
    //
    // install_callbacks = true, al revés que en el camino de Vulkan: allí los
    // instala main —necesita interceptar el cursor para el mouse-look— y le
    // reenvía a ImGui botones, rueda, teclas y caracteres a mano. Aquí no los
    // reenvía nadie, y con false ImGui solo recibe la POSICIÓN del ratón (la
    // lee por su cuenta en NewFrame): ni un clic llega, así que no se puede
    // seleccionar nada ni cambiar de pestaña. Los callbacks que ya hubiera
    // puestos —el de tamaño del framebuffer— siguen llamándose: ImGui los
    // guarda y encadena.
    ImGui_ImplGlfw_InitForOther(info.window, true);

    // El rango de descriptores que el backend reservó para la interfaz. Desde
    // la 1.92 su backend de DX12 pide descriptores por su cuenta —uno por
    // textura, no solo la fuente—, así que hay que repartírselos con estos dos
    // callbacks en vez de darle uno fijo.
    m_d3dSrvPool.cpuStart = info.d3dSrvCpuStart;
    m_d3dSrvPool.gpuStart = info.d3dSrvGpuStart;
    m_d3dSrvPool.stride   = info.d3dSrvStride;
    m_d3dSrvPool.capacity = info.d3dSrvCount;
    m_d3dSrvPool.next     = 0;
    m_d3dSrvPool.released.clear();

    ImGui_ImplDX12_InitInfo initInfo{};
    initInfo.Device            = static_cast<ID3D12Device*>(info.d3dDevice);
    initInfo.CommandQueue      = static_cast<ID3D12CommandQueue*>(info.d3dQueue);
    initInfo.NumFramesInFlight = static_cast<int>(info.framesInFlight);
    initInfo.RTVFormat         = static_cast<DXGI_FORMAT>(info.d3dRtvFormat);
    initInfo.SrvDescriptorHeap = static_cast<ID3D12DescriptorHeap*>(info.d3dSrvHeap);
    initInfo.UserData          = &m_d3dSrvPool;
    initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* init,
                                       D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
                                       D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
        auto*    pool  = static_cast<D3D12SrvPool*>(init->UserData);
        unsigned index = 0;
        if (!pool->released.empty()) {
            index = pool->released.back();
            pool->released.pop_back();
        } else {
            // Quedarse sin sitio aquí sería un fallo silencioso que acabaría
            // pisando descriptores de la escena.
            if (pool->next >= pool->capacity)
                throw std::runtime_error("EditorUI: ImGui pidio mas descriptores de los reservados");
            index = pool->next++;
        }
        outCpu->ptr = pool->cpuStart + static_cast<uint64_t>(index) * pool->stride;
        outGpu->ptr = pool->gpuStart + static_cast<uint64_t>(index) * pool->stride;
    };
    initInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* init,
                                      D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                      D3D12_GPU_DESCRIPTOR_HANDLE) {
        auto* pool = static_cast<D3D12SrvPool*>(init->UserData);
        if (pool->stride == 0 || cpu.ptr < pool->cpuStart)
            return;
        pool->released.push_back(
            static_cast<unsigned>((cpu.ptr - pool->cpuStart) / pool->stride));
    };
    ImGui_ImplDX12_Init(&initInfo);
}
#endif

namespace {

// Nombres de los modos que se guardan en el project.json. Son los MISMOS
// literales que ofrecen los combos del menú View (aaNames/fpNames): el ajuste
// se persiste por nombre, así que reordenar o insertar una opción en el array
// no cambia lo que ya hay guardado.
const char* aaModeName(EditorRenderer::AaMode mode)
{
    switch (mode)
    {
        case EditorRenderer::AaMode::Fxaa: return "FXAA";
        case EditorRenderer::AaMode::Ssaa: return "SSAA";
        case EditorRenderer::AaMode::Msaa: return "MSAA";
        case EditorRenderer::AaMode::Taa:  return "TAA";
        default:                     return "None";
    }
}

// ok = false si el nombre no es ninguno de los de hoy (fichero de una versión
// futura, o editado a mano): el caller se cae al default y lo deja en el Log.
EditorRenderer::AaMode aaModeFromName(const std::string& name, bool& ok)
{
    ok = true;
    if (name == "None") return EditorRenderer::AaMode::None;
    if (name == "FXAA") return EditorRenderer::AaMode::Fxaa;
    if (name == "SSAA") return EditorRenderer::AaMode::Ssaa;
    if (name == "MSAA") return EditorRenderer::AaMode::Msaa;
    if (name == "TAA")  return EditorRenderer::AaMode::Taa;
    ok = false;
    return EditorRenderer::AaMode::None;
}

const char* fpModeName(EditorRenderer::FpMode mode)
{
    switch (mode)
    {
        case EditorRenderer::FpMode::Tiled:     return "Tiled";
        case EditorRenderer::FpMode::Clustered: return "Clustered";
        default:                          return "Off";
    }
}

EditorRenderer::FpMode fpModeFromName(const std::string& name, bool& ok)
{
    ok = true;
    if (name == "Off")       return EditorRenderer::FpMode::Off;
    if (name == "Tiled")     return EditorRenderer::FpMode::Tiled;
    if (name == "Clustered") return EditorRenderer::FpMode::Clustered;
    ok = false;
    return EditorRenderer::FpMode::Off;
}

} // namespace

ProjectContext::ViewSettings EditorUI::currentSettings()
{
    ProjectContext::ViewSettings s;

    // El backend NO sale del Renderer: es el que el usuario ha elegido para el
    // próximo arranque, que puede no ser con el que corre este proceso.
    s.renderBackend = renderBackendName(m_selectedBackend);
    s.skyboxFolder  = m_skyboxFolder;

    // Los volúmenes salen del AudioManager, que es la fuente de verdad (los
    // guarda FMOD en los ChannelGroup). Sin audio se quedan los neutros del
    // struct, así que abrir el editor en una máquina muda no escribe ceros en
    // el project.json de nadie.
    if (m_audio)
    {
        s.masterVolume = m_audio->getBusVolume(AudioBus::Master);
        s.musicVolume  = m_audio->getBusVolume(AudioBus::Music);
        s.sfxVolume    = m_audio->getBusVolume(AudioBus::Sfx);
    }

    if (m_renderer)
    {
        s.ambient   = m_renderer->ambientEnabled();
        s.wireframe = m_renderer->isWireframeMode();
        s.bloom   = m_renderer->bloomEnabled();
        s.ssao    = m_renderer->ssaoEnabled();
        s.ssr     = m_renderer->ssrEnabled();
        s.fog     = m_renderer->fogEnabled();
        s.motionBlur = m_renderer->motionBlurEnabled();
        s.aaMode  = aaModeName(m_renderer->aaMode());
        s.fpMode  = fpModeName(m_renderer->forwardPlusMode());

        s.ambientIntensity = m_renderer->ambientIntensity();
        s.bloomThreshold   = m_renderer->bloomThreshold();
        s.bloomKnee        = m_renderer->bloomKnee();
        s.bloomIntensity   = m_renderer->bloomIntensity();
        s.ssaoRadius       = m_renderer->ssaoRadius();
        s.ssaoBias         = m_renderer->ssaoBias();
        s.ssaoIntensity    = m_renderer->ssaoIntensity();
        s.ssaoPower        = m_renderer->ssaoPower();
        s.ssrMaxDistance   = m_renderer->ssrMaxDistance();
        s.ssrThickness     = m_renderer->ssrThickness();
        s.ssrMaxSteps      = m_renderer->ssrMaxSteps();
        s.ssrEdgeFade      = m_renderer->ssrEdgeFade();
        s.ssrIntensity     = m_renderer->ssrIntensity();
        s.fogDensity       = m_renderer->fogDensity();
        s.fogHeightFalloff = m_renderer->fogHeightFalloff();
        s.fogBaseHeight    = m_renderer->fogBaseHeight();
        s.fogAnisotropy    = m_renderer->fogAnisotropy();
        s.fogSteps         = m_renderer->fogSteps();
        const glm::vec3 scatter = m_renderer->fogScatter();
        s.fogScatter[0] = scatter.x;
        s.fogScatter[1] = scatter.y;
        s.fogScatter[2] = scatter.z;
        s.motionBlurIntensity = m_renderer->motionBlurIntensity();
        s.motionBlurMaxRadius = m_renderer->motionBlurMaxRadius();
        s.motionBlurSamples   = m_renderer->motionBlurSamples();
        s.fxaaSubpix           = m_renderer->fxaaSubpix();
        s.fxaaEdgeThreshold    = m_renderer->fxaaEdgeThreshold();
        s.fxaaEdgeThresholdMin = m_renderer->fxaaEdgeThresholdMin();
        s.ssaaFactor           = m_renderer->ssaaFactor();
        s.msaaSamples          = m_renderer->msaaSamples();
        s.taaFeedback          = m_renderer->taaFeedback();
        s.taaJitterScale       = m_renderer->taaJitterScale();
        s.fpLightRadius        = m_renderer->forwardPlusLightRadius();
        s.shadowDistance       = m_renderer->shadowDistance();
        s.cascadeLambda        = m_renderer->cascadeLambda();
        s.shadowResolution     = m_renderer->shadowResolution();
        s.presentMode          = static_cast<int>(m_renderer->presentMode());
    }

    // Capas de física: la fuente de la verdad es el PhysicsManager. Sin él
    // (tests headless, arranque antes de crearlo) se quedan los defaults de
    // ViewSettings, que son los mismos que los del manager.
    static_assert(ProjectContext::ViewSettings::LayerCount == PhysicsManager::kLayerCount,
                  "El project.json y el PhysicsManager tienen que contar las mismas capas");
    if (m_physics)
    {
        s.layerActive = m_physics->layerCount();
        for (int i = 0; i < PhysicsManager::kLayerCount; ++i)
        {
            s.layerNames[static_cast<size_t>(i)] = m_physics->getLayerName(i);
            s.layerMasks[static_cast<size_t>(i)] = m_physics->layerMask(i);
        }
    }

    using VS = ProjectContext::ViewSettings;
    auto flag = [](bool* open) { return (open && *open) ? 1 : 0; };
    s.panelOpen[VS::PanelScene]          = flag(m_scenePanel.GetOpenPtr());
    s.panelOpen[VS::PanelViewport]       = flag(m_viewportPanel.GetOpenPtr());
    s.panelOpen[VS::PanelProperties]     = flag(m_propertiesPanel.GetOpenPtr());
    s.panelOpen[VS::PanelLog]            = flag(m_logPanel.GetOpenPtr());
    s.panelOpen[VS::PanelContentBrowser] = flag(m_contentBrowserPanel.GetOpenPtr());
    s.panelOpen[VS::PanelScriptEditor]   = flag(m_scriptEditor ? m_scriptEditor->GetOpenPtr() : nullptr);
    s.panelOpen[VS::PanelAnimator]       = flag(m_animatorPanel.GetOpenPtr());
    s.panelOpen[VS::PanelPerformance]    = flag(m_performancePanel.GetOpenPtr());
    s.panelOpen[VS::PanelInputActions]   = flag(m_inputActionsPanel.GetOpenPtr());
    return s;
}

void EditorUI::applyProjectSettings()
{
    if (m_project == m_appliedProject)
        return;
    // El Renderer hace falta para aplicar: sin él se reintenta el frame
    // siguiente en vez de dar el proyecto por aplicado.
    if (!m_renderer)
        return;

    m_appliedProject = m_project;
    if (!m_project || !m_project->valid())
        return; // tests headless / arranque previo al selector: como siempre.

    // La base son los valores de AHORA del Renderer: cada parámetro que el
    // project.json no traiga se queda con el default del Renderer. Los enables
    // no: readSettings los fuerza a apagado cuando faltan.
    const ProjectContext::ViewSettings s =
        ProjectContext::readSettings(m_project->root(), currentSettings());

    if (s.loadFailed)
        m_logPanel.push("Ajustes del proyecto ilegibles: se abren los efectos apagados");

    // Audio primero: no depende del Renderer, y ponerlo aquí deja claro que
    // comparte el mismo momento de aplicación que el resto de ajustes.
    if (m_audio)
    {
        m_audio->setBusVolume(AudioBus::Master, s.masterVolume);
        m_audio->setBusVolume(AudioBus::Music,  s.musicVolume);
        m_audio->setBusVolume(AudioBus::Sfx,    s.sfxVolume);
    }

    m_renderer->setAmbientEnabled(s.ambient);
    m_renderer->setWireframeMode(s.wireframe);
    m_renderer->setAmbientIntensity(s.ambientIntensity);

    m_renderer->setBloomEnabled(s.bloom);
    m_renderer->setBloomThreshold(s.bloomThreshold);
    m_renderer->setBloomKnee(s.bloomKnee);
    m_renderer->setBloomIntensity(s.bloomIntensity);

    m_renderer->setSsaoEnabled(s.ssao);
    m_renderer->setSsaoRadius(s.ssaoRadius);
    m_renderer->setSsaoBias(s.ssaoBias);
    m_renderer->setSsaoIntensity(s.ssaoIntensity);
    m_renderer->setSsaoPower(s.ssaoPower);

    m_renderer->setSsrEnabled(s.ssr);
    m_renderer->setSsrMaxDistance(s.ssrMaxDistance);
    m_renderer->setSsrThickness(s.ssrThickness);
    m_renderer->setSsrMaxSteps(s.ssrMaxSteps);
    m_renderer->setSsrEdgeFade(s.ssrEdgeFade);
    m_renderer->setSsrIntensity(s.ssrIntensity);

    m_renderer->setFogEnabled(s.fog);
    m_renderer->setFogDensity(s.fogDensity);
    m_renderer->setFogHeightFalloff(s.fogHeightFalloff);
    m_renderer->setFogBaseHeight(s.fogBaseHeight);
    m_renderer->setFogAnisotropy(s.fogAnisotropy);
    m_renderer->setFogSteps(s.fogSteps);
    m_renderer->setFogScatter(glm::vec3(s.fogScatter[0], s.fogScatter[1], s.fogScatter[2]));

    m_renderer->setMotionBlurEnabled(s.motionBlur);
    m_renderer->setMotionBlurIntensity(s.motionBlurIntensity);
    m_renderer->setMotionBlurMaxRadius(s.motionBlurMaxRadius);
    m_renderer->setMotionBlurSamples(s.motionBlurSamples);

    m_renderer->setFxaaSubpix(s.fxaaSubpix);
    m_renderer->setFxaaEdgeThreshold(s.fxaaEdgeThreshold);
    m_renderer->setFxaaEdgeThresholdMin(s.fxaaEdgeThresholdMin);
    m_renderer->setSsaaFactor(s.ssaaFactor);
    // El número de muestras guardado puede no existir en ESTA GPU (proyecto
    // traído de otra máquina): se recorta a lo que soporta el device.
    const int maxSamples = m_renderer->maxMsaaSamples();
    m_renderer->setMsaaSamples(std::clamp(s.msaaSamples, 1, maxSamples > 0 ? maxSamples : 1));
    m_renderer->setTaaFeedback(s.taaFeedback);
    m_renderer->setTaaJitterScale(s.taaJitterScale);

    // Los modos, los últimos: cambiarlos recrea targets, y así se hace una sola
    // vez con los parámetros ya puestos.
    bool aaOk = true;
    const EditorRenderer::AaMode aa = aaModeFromName(s.aaMode, aaOk);
    if (!aaOk)
        m_logPanel.push("Modo de anti-aliasing desconocido en el proyecto ('" + s.aaMode + "'): se usa None");
    m_renderer->setAaMode(aa);

    bool fpOk = true;
    const EditorRenderer::FpMode fp = fpModeFromName(s.fpMode, fpOk);
    if (!fpOk)
        m_logPanel.push("Modo de Forward+ desconocido en el proyecto ('" + s.fpMode + "'): se usa Off");
    m_renderer->setForwardPlusMode(fp);
    m_renderer->setForwardPlusLightRadius(s.fpLightRadius);
    m_renderer->setShadowDistance(s.shadowDistance);
    m_renderer->setCascadeLambda(s.cascadeLambda);
    m_renderer->setShadowResolution(s.shadowResolution);
    m_renderer->setPresentMode(static_cast<PresentMode>(s.presentMode));

    // Backend de render: se LEE pero no se aplica. El device de este proceso ya
    // está creado —el selector de proyecto se dibuja sobre él—, así que lo único
    // que se puede hacer es dejarlo elegido para el próximo arranque y avisar.
    bool backendOk = true;
    // El cielo del proyecto, ANTES de tocar el backend: initSkybox reconvoluciona
    // el IBL global, que es de lo que come el ambiente de la escena.
    std::snprintf(m_skyboxFolder, sizeof(m_skyboxFolder), "%s", s.skyboxFolder.c_str());
    if (m_renderer)
        m_renderer->initSkybox(s.skyboxFaces());

    m_selectedBackend = renderBackendFromName(s.renderBackend, backendOk);
    if (!backendOk)
        m_logPanel.push("Backend de render desconocido en el proyecto ('" + s.renderBackend +
                        "'): se usa Vulkan");
    if (m_selectedBackend != m_activeBackend)
        m_logPanel.push(std::string("Este proyecto pide el backend ") +
                        renderBackendName(m_selectedBackend) + " y el editor está corriendo con " +
                        renderBackendName(m_activeBackend) + ": reinicia para aplicarlo");

    // Capas de física: los nombres tal cual, y la matriz recorriendo sólo la
    // mitad SUPERIOR (b >= a). setLayerCollision escribe ya las dos mitades, así
    // que un project.json con la matriz asimétrica —editado a mano— queda
    // simétrico en vez de pelearse consigo mismo celda a celda.
    if (m_physics)
    {
        // Cuántas capas hay: se vacía a la 0 y se recrean, así el manager queda
        // con las del proyecto y no con las del proyecto anterior.
        while (m_physics->layerCount() > 1)
            m_physics->removeLayer(m_physics->layerCount() - 1);
        for (int i = 1; i < s.layerActive; ++i)
            m_physics->addLayer(s.layerNames[static_cast<size_t>(i)]);

        for (int i = 0; i < PhysicsManager::kLayerCount; ++i)
            m_physics->setLayerName(i, s.layerNames[static_cast<size_t>(i)]);

        for (int a = 0; a < PhysicsManager::kLayerCount; ++a)
            for (int b = a; b < PhysicsManager::kLayerCount; ++b)
                m_physics->setLayerCollision(
                    a, b, (s.layerMasks[static_cast<size_t>(a)] & (1u << static_cast<uint32_t>(b))) != 0);
    }

    // Visibilidad de panel: el project.json manda sobre imgui.ini en QUÉ paneles
    // están abiertos; el layout (docking, tamaños) lo sigue llevando imgui.ini.
    // Un panel sin dato guardado (-1) se queda como esté.
    using VS = ProjectContext::ViewSettings;
    auto applyPanel = [&](int index, bool* open) {
        if (open && s.panelOpen[index] >= 0)
            *open = (s.panelOpen[index] != 0);
    };
    applyPanel(VS::PanelScene,          m_scenePanel.GetOpenPtr());
    applyPanel(VS::PanelViewport,       m_viewportPanel.GetOpenPtr());
    applyPanel(VS::PanelProperties,     m_propertiesPanel.GetOpenPtr());
    applyPanel(VS::PanelLog,            m_logPanel.GetOpenPtr());
    applyPanel(VS::PanelContentBrowser, m_contentBrowserPanel.GetOpenPtr());
    applyPanel(VS::PanelScriptEditor,   m_scriptEditor ? m_scriptEditor->GetOpenPtr() : nullptr);
    applyPanel(VS::PanelAnimator,       m_animatorPanel.GetOpenPtr());
    applyPanel(VS::PanelPerformance,    m_performancePanel.GetOpenPtr());
    applyPanel(VS::PanelInputActions,   m_inputActionsPanel.GetOpenPtr());
}

void EditorUI::applySkyboxFolder(const std::string& folder)
{
    if (folder.empty() || !m_renderer)
        return;

    // Relativa al proyecto si cae dentro: es lo que se persiste y lo que el
    // exportador sabe resolver. Una carpeta de fuera se guarda tal cual, y
    // entonces el export no la encontrara — se avisa en el Log.
    std::string guardada = folder;
    if (m_project && m_project->valid())
    {
        std::error_code ec;
        const std::filesystem::path rel =
            std::filesystem::relative(std::filesystem::path(folder), m_project->root(), ec);
        if (!ec && !rel.empty() && rel.native().rfind(L"..", 0) != 0)
            guardada = rel.generic_string();
        else
            m_logPanel.push("Skybox: '" + folder +
                            "' esta fuera del proyecto; el juego exportado no la encontrara.");
    }

    std::snprintf(m_skyboxFolder, sizeof(m_skyboxFolder), "%s", guardada.c_str());

    ProjectContext::ViewSettings tmp;
    tmp.skyboxFolder = guardada;
    m_renderer->initSkybox(tmp.skyboxFaces());
    saveProjectSettings();
    m_logPanel.push("Skybox recargado desde '" + guardada + "'");
}

void EditorUI::drawEnvironmentWindow()
{
    if (!m_environmentWindowOpen)
        return;

    ImGui::SetNextWindowSize(ImVec2(460.0f, 200.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Environment", &m_environmentWindowOpen))
    {
        ImGui::TextWrapped(
            "Carpeta del cielo. Dentro se esperan las seis caras con estos nombres: "
            "px, nx, py, ny, pz y nz (.png). Cambiarla tambien recalcula la "
            "iluminacion ambiental, que sale de convolucionar este mismo cubemap.");
        ImGui::Separator();

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##SkyboxFolder", m_skyboxFolder, sizeof(m_skyboxFolder));

        if (ImGui::Button("Browse..."))
        {
            IGFD::FileDialogConfig cfg;
            cfg.path  = (m_project && m_project->valid()) ? m_project->root().string()
                                                          : std::string(".");
            cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                        ImGuiFileDialogFlags_HideColumnDate |
                        ImGuiFileDialogFlags_DisableThumbnailMode |
                        ImGuiFileDialogFlags_DisablePlaceMode;
            // filters = nullptr -> IGFD selecciona carpeta, igual que el export.
            m_skyboxDialog->OpenDialog("SkyboxDlg", "Carpeta del skybox", nullptr, cfg);
            m_skyboxDlgOpen = true;
        }
        ImGui::SameLine();
        // Recargar sin cambiar de carpeta: util tras sobrescribir las imagenes.
        if (ImGui::Button("Reload"))
            applySkyboxFolder(m_skyboxFolder);

        // Zona de arrastre. Acepta DT_ASSET_DIR, que es el payload que el Content
        // Browser pone SOLO a las carpetas: asi ningun fichero cae aqui.
        ImGui::BeginChild("##SkyboxDrop", ImVec2(0, 44), true);
        ImGui::TextDisabled("...o arrastra aqui una carpeta desde el Content Browser");
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DT_ASSET_DIR"))
                applySkyboxFolder(std::string(static_cast<const char*>(payload->Data)));
            ImGui::EndDragDropTarget();
        }
        ImGui::EndChild();
    }
    ImGui::End();

    if (m_skyboxDlgOpen && m_skyboxDialog->Display("SkyboxDlg"))
    {
        if (m_skyboxDialog->IsOk())
            applySkyboxFolder(m_skyboxDialog->GetCurrentPath());
        m_skyboxDialog->Close();
        m_skyboxDlgOpen = false;
    }
}

void EditorUI::saveProjectSettings()
{
    if (!m_project || !m_project->valid())
        return; // sin proyecto abierto esto no corre: comportamiento de antes.

    if (!ProjectContext::writeSettings(m_project->root(), currentSettings()))
        m_logPanel.push("No se pudieron guardar los ajustes en el project.json");
}

void EditorUI::draw(uint64_t viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView)
{
    // Selector de proyecto: primer estado del bucle. Se lleva el frame entero —
    // ni menú, ni toolbar, ni dockspace, ni paneles— hasta que el callback
    // devuelve true; entonces se suelta y el frame siguiente ya es el editor de
    // siempre. Misma ventana, mismo device y misma sesión de ImGui.
    if (m_projectSelector)
    {
        if (m_projectSelector())
            m_projectSelector = nullptr;
        return;
    }

    // Fallos de carga de audio que FMOD ha detectado desde el frame anterior.
    // Se drenan aquí, fuera de Play: un clip roto se ve al soltarlo, no al
    // darle a Play. Con FMOD_NONBLOCKING el error no existe todavía cuando
    // createSound retorna, así que este pump por frame es el ÚNICO sitio donde
    // el fallo se puede observar. Cada sonido se reporta una sola vez.
    if (m_audio)
    {
        m_audioFailures.clear();
        m_audio->pollLoadFailures(m_audioFailures);
        for (const auto& path : m_audioFailures)
            m_logPanel.push("No se pudo cargar el audio '" + path +
                             "': fichero ausente, formato no soportado o datos corruptos");
    }

    // Ajustes del menú View del proyecto abierto: se vuelcan al Renderer y a la
    // visibilidad de paneles en el primer frame tras elegir proyecto. No-op el
    // resto de frames y sin proyecto.
    applyProjectSettings();

    // Drenaje del buzón de DonTopo.loadScene: aquí, al principio del frame de
    // UI, ya se salió del tick de scripts (ScriptManager::update corre antes en
    // el bucle de main), así que cargar no destruye el GameObject que pidió la
    // carga. Misma ruta que el Load Scene del menú File.
    if (std::string luaScenePath; ScriptBindings::takePendingSceneLoad(luaScenePath))
    {
        if (!m_isPlaying)
            m_logPanel.push("DonTopo.loadScene ignorado: solo funciona en Play Mode");
        else
        {
            loadSceneFile(luaScenePath);
            // La escena vieja murió: el alive set de Lua guardaba sus punteros y
            // los nuevos GameObject pueden reusar esas direcciones.
            if (m_scriptManager) m_scriptManager->rebuildAliveSet();
        }
    }

    handleUndoRedoShortcut();
    drawMenuBar();
    drawToolbar();
    drawDockSpace();
    // Después del dockspace: una ventana dibujada antes no se puede acoplar.
    drawCollisionLayersWindow(m_physics, [this] { saveProjectSettings(); });
    drawEnvironmentWindow();

    // Ctx único, construido una vez por frame y compartido por referencia
    // con todos los paneles (patrón fijado aquí para las tareas siguientes).
    EditorContext ctx{
        m_selected,
        m_isPlaying,
        m_physics,
        m_renderer.get(),
        m_audio,
        m_scene,
        m_scriptManager,
        &m_undoHistory,
        [this](const std::string& msg) { m_logPanel.push(msg); },
        m_onDelete,
        m_onAxisSelected,
        [this](const std::filesystem::path& p) {
            // Los .lua registrados viven en la carpeta Scripts/ que vigila
            // ScriptManager (la del repo, compartida como los assets del motor):
            // esos se siguen abriendo igual. Lo que se rechaza es un .lua de OTRO
            // proyecto — para eso vale contains() de un contexto ad-hoc sobre la
            // carpeta de scripts, con el mismo fallo en cerrado.
            const bool engineScripts =
                m_scriptManager &&
                ProjectContext(m_scriptManager->scriptsDirPath()).contains(p);
            if (!engineScripts && !projectAllows(p, "Script"))
                return;
            m_scriptEditor->openFile(p);
        },
        [this]() { m_animatorPanel.open(); },
        // Diferida: quien la pide lo hace MIENTRAS se construye este ctx, y el
        // panel necesita el ctx (el renderer) para abrir la imagen.
        [this](const std::string& atlasPath) { m_pendingSpriteAtlas = atlasPath; },
        [this]() { m_propertiesPanel.invalidateSpriteNames(); },
        m_assetLoader,
        m_project,                 // sandbox de rutas del proyecto abierto
        m_loadingModal.active(),   // veta la edición mientras el modal carga
        [this](const std::filesystem::path& p) {
            // La guarda de Play Mode vive también aquí, no sólo en el panel:
            // mismo motivo que en drawSceneDialog — quien de verdad carga es
            // quien tiene que negarse.
            if (m_isPlaying) return;
            loadSceneFile(p.string());
        },
        [this](const std::filesystem::path& thenLoad) {
            if (m_isPlaying) return;
            if (m_currentScenePath.empty())
            {
                // Escena nunca guardada: mismo diálogo Save Scene del menú
                // File, con la carga encadenada a su confirmación.
                m_pendingSceneLoadAfterSave = thenLoad.string();
                m_sceneDlgOpen   = true;
                m_sceneDlgIsSave = true;
                IGFD::FileDialogConfig cfg;
                cfg.path  = (m_project && m_project->valid()) ? m_project->root().string() : std::string("assets");
                cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_HideColumnDate |
                            ImGuiFileDialogFlags_DisableThumbnailMode |
                            ImGuiFileDialogFlags_DisablePlaceMode |
                            ImGuiFileDialogFlags_ConfirmOverwrite;
                m_sceneFileDialog->OpenDialog("SceneDlg", "Save Scene", ".json", cfg);
                return;
            }
            if (!projectAllows(m_currentScenePath, "Escena")) return;
            bool saved = m_scene && m_scene->save(m_currentScenePath);
            if (saved) m_undoHistory.markSceneSaved();
            m_sceneIOError = saved ? "" : "No se pudo guardar la escena";
            m_logPanel.push(saved ? ("Escena guardada: " + m_currentScenePath)
                                  : ("Error al guardar escena: " + m_currentScenePath));
            if (saved && !thenLoad.empty())
                loadSceneFile(thenLoad.string());
        },
    };

    m_scenePanel.draw(ctx, sceneRoot);
    // ScenePanel ha borrado el GameObject seleccionado — invalida los caches
    // de edición de Properties pa que no arrastren punteros colgantes
    // (GameObject / BoxCollider ya liberados) hasta la próxima selección real.
    if (m_scenePanel.selectionWasDeletedThisFrame())
        m_propertiesPanel.invalidateCaches();
    m_viewportPanel.draw(ctx, viewportTexture, cameraView);
    // El render va al tamaño EXACTO del área de imagen del panel. Sin esto se
    // renderizaría al de la ventana y ImGui reescalaría al dibujar: ese filtrado
    // se come el escalonado (los modos de anti-aliasing dejan de distinguirse) y
    // deforma la escena si el aspect del panel no coincide con el de la ventana.
    // El Renderer ignora los tamaños nulos y solo recrea cuando cambia de verdad.
    if (m_renderer)
        m_renderer->setViewportSize(m_viewportPanel.contentWidth(), m_viewportPanel.contentHeight());
    m_propertiesPanel.draw(ctx);
    m_logPanel.draw();
    drawSceneDialog();
    drawExportDialog();
    m_contentBrowserPanel.draw(ctx, sceneRoot);
    m_scriptEditor->draw();
    m_animatorPanel.draw(ctx);
    // La petición de abrir el editor de sprites se atiende AQUÍ, con el ctx ya
    // montado; el panel necesita el renderer para cargar la imagen.
    if (!m_pendingSpriteAtlas.empty())
    {
        m_spriteEditor.open(ctx, m_pendingSpriteAtlas);
        m_pendingSpriteAtlas.clear();
    }
    m_spriteEditor.draw(ctx);
    // Siempre, tambien cerrado: su draw() es quien apaga la captura de metricas
    // del Renderer cuando el panel deja de estar visible.
    m_performancePanel.draw(ctx);
    m_inputActionsPanel.draw();

    // Overlay de progreso: se actualiza con lo que aún queda por bombear y se
    // pinta por encima. draw() devuelve true solo el frame en que se pulsa
    // Cancelar -> se cancelan las peticiones vivas y el buzón se vacía; la
    // escena se queda con lo ya aplicado (estado válido y guardable).
    m_loadingModal.update(m_assetLoader ? m_assetLoader->pending() : 0);
    if (m_loadingModal.draw() && m_assetLoader)
        m_assetLoader->cancelAllPending();
}

void EditorUI::onAssetsLoaded(std::vector<LoadedMesh> results, Scene& scene, EditorRenderer& renderer)
{
    for (auto& r : results)
    {
        std::string err;
        if (!applyLoadedMesh(r, scene, renderer, &err) && !err.empty())
            m_logPanel.push(err);
    }

    // Un solo submit para todos los uploads de este pump. Es lo que convierte
    // ~440 vkQueueWaitIdle (uno por objeto) en uno.
    renderer.flushPendingUploads();

    // Las mallas de una carga async llegan DESPUÉS de reloadSceneFromJson, así
    // que el rango de cámara que se recalculó allí no las incluía: se rehace con
    // la escena ya completa. Sin esto, una escena grande cargada de disco se
    // dibujaría con el near/far de lo que hubiera antes.
    renderer.refitCameraRange();
}

void EditorUI::onGameObjectDestroyed(GameObject* node)
{
    if (!node || !m_selected) return;
    bool selectionInSubtree = false;
    node->traverse([&](GameObject* n) { if (n == m_selected) selectionInSubtree = true; });
    if (selectionInSubtree)
    {
        m_selected = nullptr;               // el objeto va a liberarse: no dejar puntero colgante
        m_propertiesPanel.invalidateCaches(); // los caches de edición apuntaban a componentes ya liberados
    }
}

void EditorUI::handleUndoRedoShortcut()
{
    if (!m_scene || m_isPlaying || !ImGui::GetIO().KeyCtrl || ImGui::GetIO().WantTextInput)
        return;

    if (ImGui::IsKeyPressed(ImGuiKey_Z) && m_undoHistory.canUndo())
    {
        uint64_t prevSelId = m_selected ? m_selected->id : 0;
        m_undoHistory.undo();
        m_selected = prevSelId ? m_scene->findById(prevSelId) : nullptr;
        m_propertiesPanel.invalidateCaches();
        m_logPanel.push("Undo: " + m_undoHistory.lastLabel());
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Y) && m_undoHistory.canRedo())
    {
        uint64_t prevSelId = m_selected ? m_selected->id : 0;
        m_undoHistory.redo();
        m_selected = prevSelId ? m_scene->findById(prevSelId) : nullptr;
        m_propertiesPanel.invalidateCaches();
        m_logPanel.push("Redo: " + m_undoHistory.lastLabel());
    }
}

// ── Ajustes de render con undo (P8/H49) ─────────────────────────────────────
//
// Los cuatro envoltorios leen el valor ANTES de dibujar y ese es el que acaba
// en el comando: `SliderFloat` salta al valor del click en el mismo frame en
// que se pulsa, así que leerlo después devolvería el destino del salto y no de
// dónde venía. Los cuatro Combo del menú leen después a propósito y por eso no
// pasan por aquí: su valor previo es el que el propio Combo estaba mostrando.
//
// El arrastre se detecta por FLANCO (el item pasa a activo y su ID no es el que
// ya teníamos) en vez de por `IsItemActivated()` a secas: `ColorEdit3` es un
// grupo de sub-widgets y su flag de activación no siempre sube al grupo,
// mientras que `IsItemActive()` sí funciona en los dos casos.
void EditorUI::renderSliderFloat(const char* label, float lo, float hi, const char* fmt,
                                  const std::function<float()>& get,
                                  const std::function<void(float)>& set)
{
    const float prev = get();
    float v = prev;
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::SliderFloat(label, &v, lo, hi, fmt))
        set(v);

    const unsigned int id = ImGui::GetItemID();
    if (ImGui::IsItemActive() && m_editActiveId != id)
    {
        m_editActiveId    = id;
        m_editBeginScalar = prev;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        pushRenderUndo<float>(label, m_editBeginScalar, get(), set);
    if (ImGui::IsItemDeactivated())
        m_editActiveId = 0;
}

void EditorUI::renderSliderInt(const char* label, int lo, int hi,
                                const std::function<int()>& get,
                                const std::function<void(int)>& set)
{
    const int prev = get();
    int v = prev;
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::SliderInt(label, &v, lo, hi))
        set(v);

    const unsigned int id = ImGui::GetItemID();
    if (ImGui::IsItemActive() && m_editActiveId != id)
    {
        m_editActiveId = id;
        m_editBeginInt = prev;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        pushRenderUndo<int>(label, m_editBeginInt, get(), set);
    if (ImGui::IsItemDeactivated())
        m_editActiveId = 0;
}

// Sin arrastre que esperar: el click ya es el cambio entero, así que el comando
// se empuja en ese mismo frame. Devuelve el valor vigente porque los llamantes
// lo usan acto seguido para el BeginDisabled de los sliders de su efecto.
bool EditorUI::renderCheckbox(const char* label, const std::function<bool()>& get,
                               const std::function<void(bool)>& set)
{
    bool v = get();
    if (ImGui::Checkbox(label, &v))
    {
        set(v);
        pushRenderUndo<bool>(label, !v, v, set);
    }
    return v;
}

void EditorUI::renderColorEdit3(const char* label, const std::function<glm::vec3()>& get,
                                 const std::function<void(const glm::vec3&)>& set)
{
    const glm::vec3 prev = get();
    glm::vec3 v = prev;
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::ColorEdit3(label, &v.x))
        set(v);

    const unsigned int id = ImGui::GetItemID();
    if (ImGui::IsItemActive() && m_editActiveId != id)
    {
        m_editActiveId   = id;
        m_editBeginColor = prev;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        pushRenderUndo<glm::vec3>(label, m_editBeginColor, get(), set);
    if (ImGui::IsItemDeactivated())
        m_editActiveId = 0;
}

void EditorUI::drawMenuBar()
{
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            // Fuera de Play Mode por el mismo motivo que Save/Load: el paquete
            // se construye desde la escena EN MEMORIA, así que exportar durante
            // Play empaquetaría el estado de simulación en vez de la escena de
            // autor, y el juego exportado arrancaría a media partida.
            if (ImGui::MenuItem("Export Game...", nullptr, false, m_scene != nullptr && !m_isPlaying))
            {
                IGFD::FileDialogConfig cfg;
                cfg.path  = (m_project && m_project->valid()) ? m_project->root().string() : std::string(".");
                cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_HideColumnDate |
                            ImGuiFileDialogFlags_DisableThumbnailMode |
                            ImGuiFileDialogFlags_DisablePlaceMode;
                // filters = nullptr -> IGFD selecciona carpeta, no fichero.
                m_exportDialog->OpenDialog("ExportDlg", "Carpeta destino del export", nullptr, cfg);
                m_exportDlgOpen = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            // La visibilidad de los paneles tambien es ajuste del proyecto: el
            // MenuItem es un checkbox, asi que se guarda en el mismo frame del
            // click. El LAYOUT (docking, tamanos) lo sigue llevando imgui.ini.
            bool panelToggled = false;
            panelToggled |= ImGui::MenuItem("Scene", nullptr, m_scenePanel.GetOpenPtr());
            panelToggled |= ImGui::MenuItem("Viewport", nullptr, m_viewportPanel.GetOpenPtr());
            panelToggled |= ImGui::MenuItem("Properties", nullptr, m_propertiesPanel.GetOpenPtr());
            panelToggled |= ImGui::MenuItem("Log", nullptr, m_logPanel.GetOpenPtr());
            panelToggled |= ImGui::MenuItem("Content Browser", nullptr, m_contentBrowserPanel.GetOpenPtr());
            panelToggled |= ImGui::MenuItem("Script Editor", nullptr, m_scriptEditor->GetOpenPtr());
            panelToggled |= ImGui::MenuItem("Animator", nullptr, m_animatorPanel.GetOpenPtr());
            // Sin persistir en los ajustes del proyecto a propósito: se abre
            // para trocear un atlas concreto y se cierra, no es un panel de los
            // que uno quiere encontrarse abiertos al arrancar.
            ImGui::MenuItem("Sprite Editor", nullptr, m_spriteEditor.GetOpenPtr());
            // Misma razón que el Sprite Editor: se abre para tocar la matriz y
            // se cierra, no es un panel que uno quiera abierto al arrancar.
            ImGui::MenuItem("Collision Layers", nullptr, &g_showLayerMatrix);
            panelToggled |= ImGui::MenuItem("Performance", nullptr, m_performancePanel.GetOpenPtr());
            panelToggled |= ImGui::MenuItem("Input Actions", nullptr, m_inputActionsPanel.GetOpenPtr());
            if (panelToggled)
                saveProjectSettings();
            ImGui::Separator();

            // Volumen por bus. Aquí y no en un panel propio: son tres sliders
            // que se tocan una vez por proyecto, no algo que se quiera tener
            // ocupando sitio en el dock. Se guardan en el project.json (a
            // diferencia de los ajustes de sesión de más abajo), porque es el
            // mando que el jugador espera que persista.
            //
            // El valor se lee del AudioManager en cada frame, no de una copia:
            // así un SetMasterVolume desde Lua se ve reflejado aquí en vez de
            // dejar la UI mintiendo.
            if (m_audio)
            {
                // Sin dispositivo de audio los sliders se dibujan igual pero
                // desactivados: esconderlos haría pensar que la feature no
                // existe. Se explica en el tooltip.
                ImGui::BeginDisabled(!m_audio->available());
                struct BusRow { const char* label; AudioBus bus; };
                const BusRow kRows[] = { { "Master Volume", AudioBus::Master },
                                          { "Music Volume",  AudioBus::Music  },
                                          { "SFX Volume",    AudioBus::Sfx    } };
                for (const BusRow& row : kRows)
                {
                    float v = m_audio->getBusVolume(row.bus);
                    if (ImGui::SliderFloat(row.label, &v, 0.0f, 1.0f, "%.2f"))
                        m_audio->setBusVolume(row.bus, v);
                    // Al soltar, no en cada píxel del arrastre: escribir el
                    // project.json por frame sería un fichero por milisegundo.
                    if (ImGui::IsItemDeactivatedAfterEdit())
                        saveProjectSettings();
                }
                ImGui::EndDisabled();
                if (!m_audio->available() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Sin dispositivo de audio en esta maquina: el editor "
                                      "arranca mudo y estos mandos no tienen efecto");
                ImGui::Separator();
            }
            // Peso del ambiente IBL. Ajuste de sesion: no se serializa en la
            // escena, asi que al reabrir el editor vuelve a 1.0.
            if (m_renderer)
            {
                const bool ambientOn = renderCheckbox("Ambient (IBL)",
                    [this] { return m_renderer->ambientEnabled(); },
                    [this](bool v) { m_renderer->setAmbientEnabled(v); });

                // Igual que en el bloom: el slider no se oculta con el ambiente
                // apagado, se deja desactivado.
                ImGui::BeginDisabled(!ambientOn);
                // Se guarda al SOLTAR —y en ese mismo momento entra en el
                // historial—: arrastrar de punta a punta escribe una vez, no
                // una por frame. Mismo criterio en todos los sliders.
                renderSliderFloat("Ambient intensity", 0.0f, 3.0f, "%.2f",
                    [this] { return m_renderer->ambientIntensity(); },
                    [this](float v) { m_renderer->setAmbientIntensity(v); });
                ImGui::EndDisabled();

                // Reflection probes: control GLOBAL (rehornear la escena entera).
                // El radio y la intensidad de cada sonda van en su Properties,
                // que es donde se edita lo que es de un objeto. El bake solo se
                // encola: lo ejecuta el Renderer al principio del frame
                // siguiente, nunca como un pass del frame.
                ImGui::Separator();
                const int probes = m_renderer->probeCount();
                ImGui::BeginDisabled(probes == 0);
                if (ImGui::MenuItem("Bake All Reflection Probes"))
                    m_renderer->requestProbeBakeAll();
                ImGui::EndDisabled();
                // La cifra la da el BACKEND ACTIVO: los dos guardan cosas
                // distintas por sonda y antes se enseñaba siempre la de Vulkan
                // (H51).
                ImGui::Text("Sondas: %d  (%.2f MB c/u)", probes,
                            (double)m_renderer->probeMemoryBytes() / (1024.0 * 1024.0));
                // Sin sondas no hay bake que contar, y un "0.00 ms" se lee como
                // horneado instantáneo en vez de como "nunca" (H56). Es la
                // misma distinción que ya hacía la sección Reflection Probe del
                // panel Properties con su "sin bakear".
                if (probes == 0)
                    ImGui::TextDisabled("Ultimo bake: sin sondas en la escena");
                else if (m_renderer->lastProbeBakeMs() <= 0.0f)
                    ImGui::TextDisabled("Ultimo bake: sin bakear");
                else
                    ImGui::Text("Ultimo bake: %.2f ms de GPU", m_renderer->lastProbeBakeMs());

                // El cielo NO se edita desde aqui: sobre un menu de ImGui no se
                // puede soltar un arrastre —el popup se cierra al soltar fuera—,
                // asi que vive en su propia ventana. Aqui solo la entrada que la
                // abre, junto al ambiente porque el IBL global sale de
                // convolucionar ese mismo cubemap.
                ImGui::Separator();
                if (ImGui::MenuItem("Environment (skybox)..."))
                    m_environmentWindowOpen = true;

                // Sombras en cascada. Los dos eran constantes de compilacion
                // hasta ahora, y son de lo que mas se nota: las 4 cascadas se
                // reparten "Shadow distance", asi que bajarla concentra los
                // mismos texeles en menos mundo y afila la sombra de cerca.
                ImGui::Separator();

                // Modo de presentación. Los que el device no da salen
                // DESHABILITADOS con su motivo, no escondidos: si el core
                // soporta N opciones la UI ofrece N, y el matiz se documenta.
                {
                    const PresentMode kModos[] = { PresentMode::Vsync,
                                                   PresentMode::Mailbox,
                                                   PresentMode::Immediate };
                    const char* kNombres[] = { "Vsync", "Mailbox", "Immediate" };
                    // Dos lineas por modo: que hace, y que se paga por ello.
                    const char* kQueHace[] = {
                        "Espera al refresco.",
                        "Triple buffer: ni espera ni rompe la imagen.",
                        "No espera al refresco.",
                    };
                    const char* kQueCuesta[] = {
                        "Sin tearing, pero clava los FPS a los del monitor.",
                        "Dibuja frames que se descartan. Solo lo da Vulkan.",
                        "Aparece tearing, y es el UNICO modo con el que se puede medir"
                        " el coste real de un frame: con Vsync todo sale a 16 ms.",
                    };

                    const int actual = static_cast<int>(m_renderer->presentMode());
                    ImGui::SetNextItemWidth(140.0f);
                    if (ImGui::BeginCombo("Present mode", kNombres[actual]))
                    {
                        for (int i = 0; i < IM_ARRAYSIZE(kModos); ++i)
                        {
                            const bool soportado = m_renderer->presentModeSupported(kModos[i]);
                            ImGui::BeginDisabled(!soportado);
                            if (ImGui::Selectable(kNombres[i], i == actual))
                            {
                                const PresentMode antes = m_renderer->presentMode();
                                m_renderer->setPresentMode(kModos[i]);
                                // El modo CONCEDIDO puede no ser el pedido (un
                                // device sin Mailbox cae a Vsync), así que el
                                // "after" se relee en vez de darlo por hecho:
                                // un undo debe volver a lo que de verdad hubo.
                                pushRenderUndo<PresentMode>("Present mode", antes,
                                    m_renderer->presentMode(),
                                    [this](const PresentMode& v) { m_renderer->setPresentMode(v); });
                            }
                            ImGui::EndDisabled();
                            // El tooltip va FUERA del BeginDisabled: un item
                            // deshabilitado no recibe hover, y es justo el que
                            // mas necesita explicar por que no se puede elegir.
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            {
                                ImGui::BeginTooltip();
                                ImGui::TextUnformatted(kQueHace[i]);
                                ImGui::TextUnformatted(kQueCuesta[i]);
                                if (!soportado)
                                {
                                    ImGui::Separator();
                                    ImGui::TextColored(
                                        ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                        "No disponible en este equipo con el backend activo.");
                                }
                                ImGui::EndTooltip();
                            }
                        }
                        ImGui::EndCombo();
                    }
                }

                // Resolución del mapa. Es la que da el salto más bruto (4096
                // cuadruplica los texeles de 2048), y la única de las tres que
                // mueve recursos: por eso va por el backend y no por el estado.
                {
                    const int  kSizes[]  = {1024, 2048, 4096, 8192};
                    const char* kLabels[] = {"1024", "2048", "4096", "8192"};
                    int current = 1;
                    for (int i = 0; i < IM_ARRAYSIZE(kSizes); ++i)
                        if (kSizes[i] == m_renderer->shadowResolution()) current = i;
                    ImGui::SetNextItemWidth(140.0f);
                    if (ImGui::Combo("Shadow resolution", &current, kLabels, IM_ARRAYSIZE(kLabels)))
                    {
                        const int antes = m_renderer->shadowResolution();
                        m_renderer->setShadowResolution(kSizes[current]);
                        // Deshacer esto recrea el texture array otra vez. Es
                        // caro y es lo correcto: el usuario pidió volver.
                        pushRenderUndo<int>("Shadow resolution", antes, kSizes[current],
                            [this](const int& v) { m_renderer->setShadowResolution(v); });
                    }
                }

                renderSliderFloat("Shadow distance", 20.0f, 2000.0f, "%.0f",
                    [this] { return m_renderer->shadowDistance(); },
                    [this](float v) { m_renderer->setShadowDistance(v); });

                renderSliderFloat("Cascade blend", 0.0f, 1.0f, "%.2f",
                    [this] { return m_renderer->cascadeLambda(); },
                    [this](float v) { m_renderer->setCascadeLambda(v); });
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("0 = cortes uniformes, 1 = logaritmicos.\n"
                                      "Alto da resolucion cerca; bajo reparte mas parejo.");

                { char b[kGpuMsTextSize];
                    ImGui::Text("Sombras GPU: %s ms", gpuMsText(m_renderer->shadowGpuMs(), b, kGpuMsTextSize)); }

                // Bloom. Mismo criterio que el ambiente: ajuste de sesion, no se
                // serializa. Intensity 0 deja la imagen como antes del bloom.
                ImGui::Separator();
                const bool bloom = renderCheckbox("Bloom",
                    [this] { return m_renderer->bloomEnabled(); },
                    [this](bool v) { m_renderer->setBloomEnabled(v); });

                // Igual que en el SSAO y el SSR: los sliders no se ocultan con el
                // efecto apagado, se dejan desactivados.
                ImGui::BeginDisabled(!bloom);
                renderSliderFloat("Bloom threshold", 0.0f, 5.0f, "%.2f",
                    [this] { return m_renderer->bloomThreshold(); },
                    [this](float v) { m_renderer->setBloomThreshold(v); });

                renderSliderFloat("Bloom knee", 0.0f, 1.0f, "%.2f",
                    [this] { return m_renderer->bloomKnee(); },
                    [this](float v) { m_renderer->setBloomKnee(v); });

                renderSliderFloat("Bloom intensity", 0.0f, 1.0f, "%.3f",
                    [this] { return m_renderer->bloomIntensity(); },
                    [this](float v) { m_renderer->setBloomIntensity(v); });
                ImGui::EndDisabled();

                { char b[kGpuMsTextSize];
                    ImGui::Text("Bloom GPU: %s ms", gpuMsText(m_renderer->bloomGpuMs(), b, kGpuMsTextSize)); }

                // SSAO. Mismo criterio que el ambiente y el bloom: ajuste de
                // sesion, no se serializa. Apagado deja la imagen exactamente
                // como antes de la feature y el coste GPU a cero.
                ImGui::Separator();
                const bool ssao = renderCheckbox("SSAO",
                    [this] { return m_renderer->ssaoEnabled(); },
                    [this](bool v) { m_renderer->setSsaoEnabled(v); });

                // Los sliders no se ocultan con el efecto apagado: se dejan
                // desactivados para que se vea que existen y con que valores
                // arrancarian.
                ImGui::BeginDisabled(!ssao);
                renderSliderFloat("SSAO radius", 0.05f, 2.0f, "%.2f",
                    [this] { return m_renderer->ssaoRadius(); },
                    [this](float v) { m_renderer->setSsaoRadius(v); });

                renderSliderFloat("SSAO bias", 0.0f, 0.2f, "%.3f",
                    [this] { return m_renderer->ssaoBias(); },
                    [this](float v) { m_renderer->setSsaoBias(v); });

                renderSliderFloat("SSAO intensity", 0.0f, 3.0f, "%.2f",
                    [this] { return m_renderer->ssaoIntensity(); },
                    [this](float v) { m_renderer->setSsaoIntensity(v); });

                renderSliderFloat("SSAO power", 0.25f, 4.0f, "%.2f",
                    [this] { return m_renderer->ssaoPower(); },
                    [this](float v) { m_renderer->setSsaoPower(v); });
                ImGui::EndDisabled();

                { char b[kGpuMsTextSize];
                    ImGui::Text("SSAO GPU: %s ms", gpuMsText(m_renderer->ssaoGpuMs(), b, kGpuMsTextSize)); }

                // SSR: interruptor global. La fuerza es POR GAMEOBJECT (panel
                // Properties), asi que con esto puesto pero ningun objeto marcado
                // tampoco se graba nada.
                ImGui::Separator();
                const bool ssr = renderCheckbox("SSR",
                    [this] { return m_renderer->ssrEnabled(); },
                    [this](bool v) { m_renderer->setSsrEnabled(v); });

                ImGui::BeginDisabled(!ssr);
                renderSliderFloat("SSR distance", 0.5f, 50.0f, "%.1f",
                    [this] { return m_renderer->ssrMaxDistance(); },
                    [this](float v) { m_renderer->setSsrMaxDistance(v); });

                renderSliderFloat("SSR thickness", 0.01f, 3.0f, "%.2f",
                    [this] { return m_renderer->ssrThickness(); },
                    [this](float v) { m_renderer->setSsrThickness(v); });

                renderSliderInt("SSR steps", 8, 128,
                    [this] { return m_renderer->ssrMaxSteps(); },
                    [this](int v) { m_renderer->setSsrMaxSteps(v); });

                renderSliderFloat("SSR edge fade", 0.0f, 0.5f, "%.3f",
                    [this] { return m_renderer->ssrEdgeFade(); },
                    [this](float v) { m_renderer->setSsrEdgeFade(v); });

                renderSliderFloat("SSR intensity", 0.0f, 2.0f, "%.2f",
                    [this] { return m_renderer->ssrIntensity(); },
                    [this](float v) { m_renderer->setSsrIntensity(v); });
                ImGui::EndDisabled();

                { char b[kGpuMsTextSize];
                    ImGui::Text("SSR GPU: %s ms", gpuMsText(m_renderer->ssrGpuMs(), b, kGpuMsTextSize)); }

                // Niebla volumetrica: interruptor global, ajuste de sesion (no
                // se serializa) igual que el bloom, el SSAO y el SSR. Apagada
                // deja la imagen exactamente como antes de la feature y el coste
                // GPU a cero.
                ImGui::Separator();
                const bool fog = renderCheckbox("Volumetric Fog",
                    [this] { return m_renderer->fogEnabled(); },
                    [this](bool v) { m_renderer->setFogEnabled(v); });

                // Como en el SSAO y el SSR: los sliders no se ocultan con el
                // efecto apagado, se dejan desactivados.
                ImGui::BeginDisabled(!fog);
                renderSliderFloat("Fog density", 0.0f, 0.5f, "%.3f",
                    [this] { return m_renderer->fogDensity(); },
                    [this](float v) { m_renderer->setFogDensity(v); });

                renderSliderFloat("Fog height falloff", 0.0f, 0.5f, "%.3f",
                    [this] { return m_renderer->fogHeightFalloff(); },
                    [this](float v) { m_renderer->setFogHeightFalloff(v); });

                renderSliderFloat("Fog base height", -50.0f, 50.0f, "%.1f",
                    [this] { return m_renderer->fogBaseHeight(); },
                    [this](float v) { m_renderer->setFogBaseHeight(v); });

                renderSliderFloat("Fog anisotropy", -0.95f, 0.95f, "%.2f",
                    [this] { return m_renderer->fogAnisotropy(); },
                    [this](float v) { m_renderer->setFogAnisotropy(v); });

                renderSliderInt("Fog steps", 8, 128,
                    [this] { return m_renderer->fogSteps(); },
                    [this](int v) { m_renderer->setFogSteps(v); });

                renderColorEdit3("Fog scattering",
                    [this] { return m_renderer->fogScatter(); },
                    [this](const glm::vec3& v) { m_renderer->setFogScatter(v); });
                ImGui::EndDisabled();

                { char b[kGpuMsTextSize];
                    ImGui::Text("Fog GPU: %s ms", gpuMsText(m_renderer->fogGpuMs(), b, kGpuMsTextSize)); }

                // Motion blur de camara. Apagado por defecto: sin el la imagen
                // es exactamente la de antes de la feature y no se graba ni un
                // dispatch. La velocidad sale de reproyectar la profundidad al
                // frame anterior, asi que emborrona lo que mueve la CAMARA; un
                // objeto que se mueve solo con la camara quieta no deja estela.
                ImGui::Separator();
                const bool motionBlur = renderCheckbox("Motion Blur",
                    [this] { return m_renderer->motionBlurEnabled(); },
                    [this](bool v) { m_renderer->setMotionBlurEnabled(v); });

                // Como en el SSAO, el SSR y la niebla: los sliders no se ocultan
                // con el efecto apagado, se dejan desactivados.
                ImGui::BeginDisabled(!motionBlur);
                renderSliderFloat("Motion blur intensity", 0.0f, 4.0f, "%.2f",
                    [this] { return m_renderer->motionBlurIntensity(); },
                    [this](float v) { m_renderer->setMotionBlurIntensity(v); });

                renderSliderFloat("Motion blur max radius", 1.0f, 128.0f, "%.0f px",
                    [this] { return m_renderer->motionBlurMaxRadius(); },
                    [this](float v) { m_renderer->setMotionBlurMaxRadius(v); });

                renderSliderInt("Motion blur samples", 2, 32,
                    [this] { return m_renderer->motionBlurSamples(); },
                    [this](int v) { m_renderer->setMotionBlurSamples(v); });
                ImGui::EndDisabled();

                // Anti-aliasing. Modos EXCLUYENTES, cada uno con sus propios
                // parametros. Mismo criterio que el resto: ajuste de sesion, no
                // se serializa. En None no se graba ni un comando de mas y la
                // imagen es identica a la de antes de la feature.
                ImGui::Separator();
                using AaMode = EditorRenderer::AaMode;
                const char* aaNames[] = { "None", "FXAA", "SSAA", "MSAA", "TAA" };
                int aaCurrent = (int)m_renderer->aaMode();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::Combo("Anti-aliasing", &aaCurrent, aaNames, IM_ARRAYSIZE(aaNames)))
                {
                    // El previo lo da el propio Combo: aaCurrent se leyo ANTES
                    // de dibujarlo y el widget lo acaba de sobrescribir, asi que
                    // el valor de antes hay que releerlo del renderer, que
                    // todavia no ha cambiado.
                    const AaMode antes = m_renderer->aaMode();
                    m_renderer->setAaMode((AaMode)aaCurrent);
                    // Se guarda el NOMBRE del modo, no este indice: ver
                    // aaModeName() al principio del fichero.
                    pushRenderUndo<AaMode>("Anti-aliasing", antes, (AaMode)aaCurrent,
                        [this](const AaMode& v) { m_renderer->setAaMode(v); });
                }

                const AaMode aaMode = m_renderer->aaMode();

                if (aaMode == AaMode::Fxaa)
                {
                    renderSliderFloat("FXAA subpixel", 0.0f, 1.0f, "%.2f",
                        [this] { return m_renderer->fxaaSubpix(); },
                        [this](float v) { m_renderer->setFxaaSubpix(v); });

                    renderSliderFloat("FXAA edge threshold", 0.063f, 0.333f, "%.3f",
                        [this] { return m_renderer->fxaaEdgeThreshold(); },
                        [this](float v) { m_renderer->setFxaaEdgeThreshold(v); });

                    renderSliderFloat("FXAA edge min", 0.0312f, 0.0833f, "%.4f",
                        [this] { return m_renderer->fxaaEdgeThresholdMin(); },
                        [this](float v) { m_renderer->setFxaaEdgeThresholdMin(v); });
                }
                else if (aaMode == AaMode::Ssaa)
                {
                    // Cambiar el factor recrea TODOS los targets internos, asi
                    // que se aplica al soltar el slider y no a cada pixel
                    // arrastrado: reconstruir el render entero 60 veces por
                    // segundo mientras se arrastra congelaria el editor.
                    // Miembro y no `static`: el static sobrevivia al cambio de
                    // proyecto, y su refresco estaba guardado por IsAnyItemActive(),
                    // que es GLOBAL — cualquier otro widget en uso congelaba el
                    // valor mostrado. Ahora solo se congela mientras se arrastra
                    // ESTE slider.
                    if (!m_ssaaSliderActive) m_ssaaPendingFactor = m_renderer->ssaaFactor();
                    ImGui::SetNextItemWidth(140.0f);
                    // Rango COMPLETO: el core no clampea y su default es 2.0, asi que
                    // capar a [1.25, 2.0] dejaba 1.0 (SSAA efectivamente apagado) y
                    // todo lo que pasa de 2 fuera del alcance del panel.
                    ImGui::SliderFloat("SSAA factor", &m_ssaaPendingFactor, 1.0f, 4.0f, "%.2fx");
                    m_ssaaSliderActive = ImGui::IsItemActive();
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        // Este es el unico slider del menu que NO necesita
                        // recordar donde empezo el arrastre: el valor no se
                        // aplica hasta soltar, asi que el renderer todavia
                        // tiene el de antes justo aqui.
                        const float antes = m_renderer->ssaaFactor();
                        m_renderer->setSsaaFactor(m_ssaaPendingFactor);
                        pushRenderUndo<float>("SSAA factor", antes, m_ssaaPendingFactor,
                            [this](const float& v) { m_renderer->setSsaaFactor(v); });
                    }
                    ImGui::TextDisabled("%.2fx pixeles por frame",
                                        m_ssaaPendingFactor * m_ssaaPendingFactor);
                }
                else if (aaMode == AaMode::Msaa)
                {
                    const int maxSamples = m_renderer->maxMsaaSamples();
                    int samples = m_renderer->msaaSamples();
                    // Solo se ofrecen las cuentas que soporta el device para
                    // color Y profundidad a la vez: el pass de escena usa las dos.
                    // Desde 1x: es lo que el core acepta como "sin multimuestra"
                    // y hasta ahora no habia forma de elegirlo desde aqui.
                    for (int s = 1; s <= 8; s *= 2)
                    {
                        if (s > maxSamples) break;
                        if (s > 1) ImGui::SameLine();
                        char label[8];
                        snprintf(label, sizeof(label), "%dx", s);
                        if (ImGui::RadioButton(label, samples == s))
                        {
                            // `samples` se leyo antes del bucle, o sea que es el
                            // valor de antes del click.
                            m_renderer->setMsaaSamples(s);
                            pushRenderUndo<int>("MSAA samples", samples, s,
                                [this](const int& v) { m_renderer->setMsaaSamples(v); });
                        }
                    }
                    // Con maxSamples == 1 el bucle no dibuja nada mas que el 1x, y
                    // ademas conviene decir por que: el modo se puede elegir igual
                    // pero el device no lo va a aplicar.
                    ImGui::SameLine();
                    ImGui::TextDisabled("(max %dx)", maxSamples);
                    if (maxSamples <= 1)
                        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                           "Esta GPU no soporta multimuestra: MSAA no hara nada.");
                }
                else if (aaMode == AaMode::Taa)
                {
                    renderSliderFloat("TAA feedback", 0.0f, 0.98f, "%.2f",
                        [this] { return m_renderer->taaFeedback(); },
                        [this](float v) { m_renderer->setTaaFeedback(v); });

                    renderSliderFloat("TAA jitter", 0.0f, 2.0f, "%.2f",
                        [this] { return m_renderer->taaJitterScale(); },
                        [this](float v) { m_renderer->setTaaJitterScale(v); });
                }

                // El pass propio solo existe en FXAA, SSAA y TAA. El coste del
                // MSAA y el del supersampling estan repartidos en el render, y
                // por eso se muestra tambien el total: comparandolo con el de
                // None sale el sobrecoste real del modo.
                { char b[kGpuMsTextSize];
                    ImGui::Text("AA GPU: %s ms", gpuMsText(m_renderer->aaGpuMs(), b, kGpuMsTextSize)); }
                { char b[kGpuMsTextSize];
                    ImGui::Text("Render GPU: %s ms", gpuMsText(m_renderer->renderGpuMs(), b, kGpuMsTextSize)); }

                // Forward+. Modos EXCLUYENTES, igual que el AA: en Off no se
                // graba ni un dispatch y pbr.frag recorre las luces del UBO como
                // siempre. Ajuste de sesion, no se serializa: asi el runtime y el
                // editor arrancan en el mismo modo y renderizan igual.
                ImGui::Separator();
                using FpMode = EditorRenderer::FpMode;
                const char* fpNames[] = { "Off", "Tiled", "Clustered" };
                int fpCurrent = (int)m_renderer->forwardPlusMode();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::Combo("Forward+", &fpCurrent, fpNames, IM_ARRAYSIZE(fpNames)))
                {
                    // Igual que en el Combo del AA: el previo se relee del
                    // renderer, que aun no ha cambiado.
                    const FpMode antes = m_renderer->forwardPlusMode();
                    m_renderer->setForwardPlusMode((FpMode)fpCurrent);
                    pushRenderUndo<FpMode>("Forward+", antes, (FpMode)fpCurrent,
                        [this](const FpMode& v) { m_renderer->setForwardPlusMode(v); });
                }

                // El recorte de luces, dicho. La escena puede tener las que
                // quiera, pero solo las primeras MAX_LIGHTS llegan al shader y
                // el resto se descartaba EN SILENCIO: la escena se veía peor
                // iluminada sin que nada lo explicara.
                //
                // Va aquí, bajo Forward+, porque es justo la feature que
                // promete escalar el número de luces y que este tope capa.
                {
                    const size_t total = m_renderer->sceneLightTotal();
                    if (total > (size_t)MAX_LIGHTS)
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                           "%zu luces en escena, solo %d iluminan.",
                                           total, MAX_LIGHTS);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(
                                "El bloque UBO tiene sitio para MAX_LIGHTS luces y se queda "
                                "con\nlas primeras en orden de escena. Las demas ni iluminan "
                                "ni\nproyectan sombra.\n\nSubir ese tope obliga a recompilar "
                                "los shaders que declaran el\nbloque, asi que no es un ajuste "
                                "de la UI.");
                    }
                }

                if (m_renderer->forwardPlusMode() != FpMode::Off)
                {
                    // El radio es lo que hace que el culling sirva de algo: con
                    // uno enorme toda luz cae en toda celda y la lista se llena.
                    renderSliderFloat("Light radius", 50.0f, 5000.0f, "%.0f",
                        [this] { return m_renderer->forwardPlusLightRadius(); },
                        [this](float v) { m_renderer->setForwardPlusLightRadius(v); });

                    { char b[kGpuMsTextSize];
                    ImGui::Text("Forward+ GPU: %s ms", gpuMsText(m_renderer->forwardPlusGpuMs(), b, kGpuMsTextSize)); }
                    ImGui::Text("Luces/celda: %.1f", m_renderer->forwardPlusAvgPerCell());
                    const uint32_t overflow = m_renderer->forwardPlusOverflowCells();
                    if (overflow > 0)
                        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                           "%u celdas desbordadas (pierden luces)", overflow);
                }
            }

            // Backend de render. El único ajuste de este menú que NO se aplica
            // al tocarlo: device, swapchain y todos los recursos de GPU cuelgan
            // del backend, así que solo puede cambiar en el arranque. Se guarda
            // en el project.json y el editor arranca con el del último proyecto
            // abierto (ProjectContext::readLastProject).
            //
            // Se ofrecen SIEMPRE las dos opciones, aunque este build no traiga
            // DX12 o la máquina no lo soporte: el aviso explica el motivo y el
            // arranque se cae a Vulkan. Esconder la opción solo dejaría al
            // usuario sin saber por qué no está.
            ImGui::Separator();
            const char* backendNames[] = { "Vulkan", "DirectX 12" };
            int backendCurrent = (int)m_selectedBackend;
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::Combo("Render backend", &backendCurrent, backendNames,
                             IM_ARRAYSIZE(backendNames)))
            {
                // Se guarda el NOMBRE, no este índice: ver renderBackendName().
                m_selectedBackend = (RenderBackend)backendCurrent;
                saveProjectSettings();
                if (m_selectedBackend != m_activeBackend)
                    m_logPanel.push(std::string("Backend de render cambiado a ") +
                                    renderBackendName(m_selectedBackend) +
                                    ": reinicia el editor para aplicarlo");
            }

            if (m_selectedBackend != m_activeBackend)
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                   "Requiere reiniciar (ahora: %s)",
                                   renderBackendName(m_activeBackend));
            else
                ImGui::TextDisabled("En uso: %s", renderBackendName(m_activeBackend));

            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

}

void EditorUI::drawToolbar()
{
    // vp->WorkPos/WorkSize (no vp->Pos/vp->Size) porque BeginMainMenuBar
    // reserva su franja restando de WorkPos/WorkSize del viewport principal
    // — así la Toolbar queda justo debajo del MenuBar en vez de solaparlo.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, kToolbarHeight));
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse;
    ImGui::Begin("##Toolbar", nullptr, flags);

    bool canPlay = m_scene && m_physics && m_audio && m_renderer;
    ImGui::BeginDisabled(!canPlay);
    if (m_isPlaying)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button("Stop"))
        {
            if (m_scriptManager) m_scriptManager->onPlayStop();
            // Restore síncrono (async=false): sin modal, determinista. Meter
            // estados a medias en la transición Play->Stop no compensa.
            m_sceneIOError = reloadSceneFromJson(m_playSnapshot, /*async=*/false) ? "" : "No se pudo restaurar la escena";
            m_isPlaying = false;
            m_logPanel.push("Play Mode detenido");
        }
        ImGui::PopStyleColor();
    }
    else
    {
        if (ImGui::Button("Play"))
        {
            m_playSnapshot = m_scene->toJson();
            m_undoHistory.clear();
            // Aviso una sola vez al arrancar Play (no cada frame: el Renderer
            // consulta findCamera() en todos, y loguear ahí inundaría la
            // consola). Sin cámara, Play arranca igual con la del editor — que
            // se pueda iterar sin cámara importa más que forzar disciplina.
            if (!m_scene->findCamera())
                m_logPanel.push("No hay cámara en la escena; usando la del editor");
            m_isPlaying = true;
            // Los Animator arrancan Play desde su estado de entrada, con el
            // reloj a cero y los parámetros limpios.
            //
            // En Edit Mode el reloj SÍ corre (solo se saltan las transiciones,
            // ver AnimatorComponent::update), así que un estado de entrada con
            // loop=false llega a su final mientras el usuario edita y deja
            // finished a true. Sin este reset, Play empezaría con esa marca ya
            // puesta y una transición "animation finished" dispararía en el
            // primer frame: la animación de entrada no llegaría a verse nunca.
            // Con loop=true no se nota, porque un clip en bucle no termina.
            //
            // El Stop no necesita el simétrico: reconstruye la escena desde el
            // snapshot JSON y bindClips ya termina en reset().
            m_scene->traverse([](GameObject* go) {
                if (go->hasAnimator()) go->getAnimator()->reset();
            });
            // Un diálogo de Save/Load o de export abierto al arrancar Play se
            // queda huérfano: la operación ya no se ejecutaría, pero el
            // diálogo seguiría en pantalla hasta el Stop. Los dos son de IGFD
            // y no bloquean la toolbar, así que se llega aquí con ellos
            // abiertos; los popups modales del export sí bloquean y no hace
            // falta tocarlos.
            if (m_sceneDlgOpen)
            {
                m_sceneFileDialog->Close();
                m_sceneDlgOpen = false;
            }
            if (m_exportDlgOpen)
            {
                m_exportDialog->Close();
                m_exportDlgOpen = false;
            }
            if (m_scriptManager) m_scriptManager->onPlayStart();
            // Sin Audio Listener en la escena (o con el suyo deshabilitado) los
            // clips suenan IGUAL: el audio 3D se oye entonces desde la cámara,
            // el fallback que ya resuelven las tres rutas de host cada frame
            // (sandbox/src/main.cpp, runtime/main.cpp). Aquí antes había un
            // gate que se saltaba este barrido, pero solo cubría playOnAwake:
            // ni AudioClip:Play de Lua (ScriptBindings.cpp) ni el botón Play
            // del inspector (PropertiesPanel.cpp) lo consultaban, así que el
            // aviso mentía —el clip se oía— y el "invariante" valía para una
            // de las cuatro rutas de reproducción. Imponerlo de verdad exigía
            // repetirlo en las cuatro, porque no puede vivir dentro de
            // AudioManager/AudioClipComponent (esas dos se prueban sin escena).
            // El aviso se queda, ahora informativo y cierto. Uno por Play, no
            // uno por clip.
            GameObject* listenerGo = m_scene->findAudioListener();
            const bool listenerActive = listenerGo && listenerGo->getAudioListener()->getEnabled();
            if (!listenerActive)
                m_logPanel.push("Sin Audio Listener en la escena: el audio 3D se oye desde la camara");
            m_scene->traverse([](GameObject* go) {
                if (go->hasAudioClip() && go->getAudioClip()->getPlayOnAwake())
                    go->getAudioClip()->play(glm::vec3(go->worldTransform[3]));
            });
            m_logPanel.push("Play Mode iniciado");
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    bool wireframe = m_renderer && m_renderer->isWireframeMode();
    if (wireframe)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button("Wireframe") && m_renderer)
    {
        m_renderer->setWireframeMode(!wireframe);
        pushRenderUndo<bool>("Wireframe", wireframe, !wireframe,
            [this](const bool& v) { m_renderer->setWireframeMode(v); });
    }
    if (wireframe)
        ImGui::PopStyleColor();

    // Save/Load quedan fuera de Play Mode: lo que hay en memoria durante Play
    // es estado de simulación (posiciones movidas por la física, valores que
    // mutaron los scripts), no la escena que el usuario está creando.
    // Guardarlo lo haría permanente sin que se note —un volumen a 0 o una
    // rotación acumulada no se ven en ninguna parte— y cargar otra escena
    // dejaría a m_playSnapshot describiendo una escena que ya no existe, así
    // que el Stop restauraría algo ajeno.
    ImGui::SameLine();
    ImGui::BeginDisabled(m_isPlaying);
    if (ImGui::Button("Save Scene") && m_scene)
    {
        m_sceneDlgOpen   = true;
        m_sceneDlgIsSave = true;
        IGFD::FileDialogConfig cfg;
        cfg.path  = (m_project && m_project->valid()) ? m_project->root().string() : std::string("assets");
        cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                    ImGuiFileDialogFlags_HideColumnDate |
                    ImGuiFileDialogFlags_DisableThumbnailMode |
                    ImGuiFileDialogFlags_DisablePlaceMode |
                    ImGuiFileDialogFlags_ConfirmOverwrite;
        m_sceneFileDialog->OpenDialog("SceneDlg", "Save Scene", ".json", cfg);
    }
    if (m_isPlaying && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Para el Play Mode para guardar o cargar escenas");

    ImGui::SameLine();
    if (ImGui::Button("Load Scene") && m_scene)
    {
        m_sceneDlgOpen   = true;
        m_sceneDlgIsSave = false;
        IGFD::FileDialogConfig cfg;
        cfg.path  = (m_project && m_project->valid()) ? m_project->root().string() : std::string("assets");
        cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                    ImGuiFileDialogFlags_HideColumnDate |
                    ImGuiFileDialogFlags_DisableThumbnailMode |
                    ImGuiFileDialogFlags_DisablePlaceMode;
        m_sceneFileDialog->OpenDialog("SceneDlg", "Load Scene", ".json", cfg);
    }
    ImGui::EndDisabled();
    if (m_isPlaying && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Para el Play Mode para guardar o cargar escenas");

    if (!m_sceneIOError.empty())
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_sceneIOError.c_str());
    }

    ImGui::End();
}

void EditorUI::drawDockSpace()
{
    ImGuiWindowFlags dockFlags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + kToolbarHeight));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - kToolbarHeight));
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##DockSpace", nullptr, dockFlags);
    ImGui::PopStyleVar(3);
    ImGui::DockSpace(ImGui::GetID("MainDockSpace"), ImVec2(0, 0), ImGuiDockNodeFlags_None);
    ImGui::End();
}

void EditorUI::focusSelected(Camera& camera)
{
    // ctx local, no miembro persistente: evita vida útil ambigua de las
    // referencias (mismo patrón que EditorContext en draw()).
    EditorContext ctx{
        m_selected,
        m_isPlaying,
        m_physics,
        m_renderer.get(),
        m_audio,
        m_scene,
        m_scriptManager,
        &m_undoHistory,
        [this](const std::string& msg) { m_logPanel.push(msg); },
        m_onDelete,
        m_onAxisSelected,
    };
    m_viewportPanel.focusSelected(ctx, camera);
}

bool EditorUI::reloadSceneFromJson(const nlohmann::json& j, bool async)
{
    if (!m_scene || !m_renderer || !m_physics || !m_audio)
        return false;

    // Libera recursos GPU de la escena actual, y con ellos sus índices: si
    // fromJson falla más abajo por malformación anidada, m_root sigue siendo
    // este mismo árbol (Scene::fromJson es atómico), y con los índices a -1 el
    // traverse de re-registro de abajo lo vuelve a registrar igual que si fuera
    // el árbol nuevo. Sin eso, el árbol viejo se quedaría con índices obsoletos
    // y sin re-registrar tras un fallo, dejando el viewport vacío pese a que los
    // datos de Scene no cambiaron.
    //
    // El reseteo lo hace ya removeGameObject en los dos backends (H14): aquí
    // estaba repetido a mano porque el de Vulkan no lo hacía.
    for (auto& child : m_scene->getRoot().children)
        m_renderer->removeGameObject(child.get());

    // Antes de arrancar una Load Scene async, cancela cualquier carga aún en
    // vuelo de una operación anterior: sus resultados resolverían a targets ya
    // borrados y se descartarían igual, pero dejarlos vivos inflaría el
    // pending() con el que se abre el modal de abajo. Solo en el camino async y
    // solo si hay loader.
    if (async && m_assetLoader)
        m_assetLoader->cancelAllPending();

    // Solo Load Scene (async) va asíncrono. El restore de Play->Stop se queda
    // síncrono a propósito: meter estados a medias en esa transición no
    // compensa la ganancia, que la capa B ya da sola. Sin loader (o en
    // síncrono) fromJson carga los meshes en el sitio, como siempre.
    bool loaded = m_scene->fromJson(j, *m_physics, *m_audio,
                                    async ? m_assetLoader : nullptr);
    // Se ejecuta tanto si loaded es true (árbol nuevo, índices ya en -1 por
    // construcción) como si es false (árbol viejo intacto, índices
    // reseteados justo arriba) — en ambos casos hay que volver a subir los
    // meshes a GPU.
    m_renderer->registerGameObject(&m_scene->getRoot());

    // Camino síncrono (restore de Play->Stop): los meshes se registran vía el
    // batch diferido, que sin flush no se hace visible hasta ~2 frames después
    // (los objetos viejos ya se quitaron arriba => pop-in/parpadeo). Se sube y
    // se espera aquí para que la geometría restaurada esté visible en este mismo
    // frame, igual que antes de la carga asíncrona. El camino async NO pasa por
    // aquí: mantiene su modal + pump por frame.
    if (!async)
        m_renderer->flushUploadsAndWait();

    if (loaded)
    {
        m_selected = nullptr; // la selección anterior ya no existe
        // Avisos de la carga (p.ej. escena con dos cámaras, donde fromJson se
        // queda con la primera): Core no conoce el Log Console, así que los
        // vuelca aquí quien sí lo conoce. Solo si loaded — una carga fallida
        // no modifica la escena y sus avisos no aplican.
        for (const auto& w : m_scene->lastWarnings())
            m_logPanel.push(w);

        // En async, fromJson encoló una petición por cada sourcePath; abrir el
        // modal con ese conteo. begin() no hace nada si son 0 (escena sin
        // meshes de fichero), así que no aparece un modal vacío.
        if (async && m_assetLoader)
            m_loadingModal.begin(m_assetLoader->pending());

        // La escena que se acaba de montar manda sobre el near/far del editor:
        // hasta aquí seguían siendo los de las mallas que se le pasaron a
        // Renderer::init en el arranque. En la ruta async esto solo ve los meshes
        // ya presentes; onAssetsLoaded lo repite cuando aterriza el resto.
        if (m_renderer)
            m_renderer->refitCameraRange();
    }
    m_undoHistory.clear();

    return loaded;
}

bool EditorUI::openProjectScene()
{
    if (!m_project || !m_project->valid())
        return false;

    const std::filesystem::path scene = m_project->resolve(ProjectContext::kStartupScene);

    std::error_code ec;
    if (!std::filesystem::exists(scene, ec) || ec)
    {
        m_logPanel.push("[Project] El proyecto no tiene escena de arranque: " + scene.string());
        return false;
    }
    return loadSceneFile(scene.string());
}

bool EditorUI::projectAllows(const std::filesystem::path& path, const char* what)
{
    if (!m_project || !m_project->valid())
        return true; // sin proyecto abierto no hay sandbox: como siempre.
    if (m_project->contains(path))
        return true;

    m_logPanel.push(std::string("[Project] ") + what + " fuera del proyecto, rechazado: " +
                    path.string());
    return false;
}

bool EditorUI::loadSceneFile(const std::string& path)
{
    // Sandbox del proyecto: una escena de otro proyecto (o de fuera del
    // workspace) se rechaza aquí, que es por donde pasan TODAS las cargas —
    // menú File, doble click en el Content Browser y DonTopo.loadScene de Lua.
    if (!projectAllows(path, "Escena"))
        return false;

    // Valida la estructura básica del JSON ANTES de tocar GPU/Scene:
    // rechaza un fichero top-level corrupto sin tocar nada (fast path, evita
    // el churn de GPU de reloadSceneFromJson). No cubre malformación anidada
    // — para eso, Scene::fromJson es atómico y reloadSceneFromJson cubre
    // ambos desenlaces.
    auto parsed = FileManager::readJson(path);
    bool structureOk = parsed.has_value() &&
                        parsed->contains("version") && (*parsed)["version"].is_number_integer() &&
                        (*parsed)["version"].get<int>() == 1 &&
                        parsed->contains("root") && (*parsed)["root"].is_object();

    bool loaded = structureOk && reloadSceneFromJson(*parsed, /*async=*/true);
    // markSceneSaved sólo aquí, no en reloadSceneFromJson: esa función también
    // restaura el snapshot de Play->Stop, que devuelve la escena al estado
    // previo al Play —con sus ediciones sin guardar— y no debe marcarla limpia.
    if (loaded)
    {
        m_undoHistory.markSceneSaved();
        m_currentScenePath = path;
    }
    m_sceneIOError = loaded ? "" : "No se pudo cargar la escena";
    m_logPanel.push(loaded ? ("Escena cargada: " + path) : ("Error al cargar escena: " + path));
    return loaded;
}

void EditorUI::drawSceneDialog()
{
    // Mismo motivo que PropertiesPanel::drawMeshDialog/drawAudioClipDialog:
    // se ejecuta cada frame independientemente de m_sceneDlgOpen para drenar
    // el diálogo aunque el usuario lo cierre sin confirmar.
    if (!m_sceneDlgOpen || !m_sceneFileDialog->Display("SceneDlg"))
        return;

    // La guarda vive aquí, en el sitio que de verdad escribe y carga, no sólo
    // en los botones: el diálogo de IGFD no bloquea la toolbar, así que se
    // puede abrir Save, pulsar Play y confirmar después. El botón deshabilitado
    // comunica; esto es lo que impide.
    if (m_isPlaying)
    {
        m_sceneFileDialog->Close();
        m_sceneDlgOpen = false;
        m_pendingSceneLoadAfterSave.clear();
        m_logPanel.push("Operación de escena cancelada: no se puede guardar ni cargar en Play Mode");
        return;
    }

    if (m_sceneFileDialog->IsOk())
    {
        std::string path = m_sceneFileDialog->GetFilePathName();

        if (m_sceneDlgIsSave)
        {
            // Igual que en la carga: el destino tiene que caer dentro del
            // proyecto. Se rechaza antes de escribir, así que el fichero de
            // fuera ni se crea ni se pisa.
            if (!projectAllows(path, "Escena"))
            {
                m_sceneFileDialog->Close();
                m_sceneDlgOpen = false;
                m_pendingSceneLoadAfterSave.clear();
                return;
            }
            bool saved   = m_scene && m_scene->save(path);
            if (saved)
            {
                m_undoHistory.markSceneSaved();
                m_currentScenePath = path;
            }
            m_sceneIOError = saved ? "" : "No se pudo guardar la escena";
            m_logPanel.push(saved ? ("Escena guardada: " + path) : ("Error al guardar escena: " + path));

            // Este Save venía del "Guardar" del modal del Content Browser sobre
            // una escena sin fichero: encadena aquí la carga que quedó
            // esperando. Si el guardado falló no se carga nada — perder los
            // cambios es justo lo que el modal existe para evitar.
            if (saved && !m_pendingSceneLoadAfterSave.empty())
                loadSceneFile(m_pendingSceneLoadAfterSave);
        }
        else
        {
            loadSceneFile(path);
        }
    }

    m_sceneFileDialog->Close();
    m_sceneDlgOpen = false;
    // Cancelar el diálogo (o un guardado fallido) descarta la carga
    // encadenada: la escena actual sigue con sus cambios sin guardar.
    m_pendingSceneLoadAfterSave.clear();
}

void EditorUI::drawExportDialog()
{
    // Corre cada frame porque los dos BeginPopupModal de abajo (nombre y
    // confirmación) necesitan submitirse en todo frame para que ImGui los
    // mantenga abiertos tras el OpenPopup que los dispara — si esta función
    // no se llamara, el popup se cerraría solo aunque el usuario no pulsara
    // Cancel. (El Display("ExportDlg") sí es condicional a m_exportDlgOpen:
    // el && de abajo cortocircuita y no lo evalúa cuando el diálogo de
    // carpeta está cerrado.)
    if (m_exportDlgOpen && m_exportDialog->Display("ExportDlg"))
    {
        if (m_exportDialog->IsOk())
        {
            m_exportDestDir = m_exportDialog->GetCurrentPath();
            m_openExportNamePopup = true;
        }
        m_exportDialog->Close();
        m_exportDlgOpen = false;
    }

    if (m_openExportNamePopup)
    {
        ImGui::OpenPopup("Export Game");
        m_openExportNamePopup = false;
    }

    if (ImGui::BeginPopupModal("Export Game", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Destino: %s", m_exportDestDir.c_str());
        ImGui::InputText("Nombre", m_exportNameBuffer, sizeof(m_exportNameBuffer));

        // pkg es lo que realmente se va a crear/borrar: se calcula y se
        // enseña aquí (no el nombre crudo) para que el usuario evalúe la
        // ruta real, no un fragmento de texto que podría no coincidir con
        // ella (ver isValidExportGameName en GameExporter.cpp).
        const std::filesystem::path pkg =
            std::filesystem::path(m_exportDestDir) / m_exportNameBuffer;
        std::string nameError;
        const bool nameOk = isValidExportGameName(m_exportNameBuffer, nameError);

        // inspectExportTarget solo se consulta con un nombre válido: con un
        // nombre inválido pkg puede no representar siquiera una ruta útil
        // (separadores sueltos, nombre de dispositivo...) y no hay nada que
        // clasificar todavía. Missing es un valor cualquiera de relleno para
        // ese caso — nunca se lee porque canExport ya exige nameOk.
        const ExportTargetState targetState =
            nameOk ? inspectExportTarget(pkg) : ExportTargetState::Missing;
        // Occupied deshabilita el botón en vez de pedir confirmación: si se
        // dejara confirmar, writeExportPackage abortaría igualmente (es
        // autoritativo, GameExporter.h:103-107) pero después de que el
        // usuario ya haya dicho "sí, borra" sobre algo que en realidad nunca
        // se iba a borrar — una confirmación que miente sobre lo que hace.
        const bool occupied  = nameOk && targetState == ExportTargetState::Occupied;
        const bool canExport = nameOk && !occupied;

        if (!nameOk)
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", nameError.c_str());
        else if (occupied)
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "'%s' ya existe y tiene contenido que no es de un export "
                               "anterior; elige otro nombre u otra carpeta destino.",
                               pkg.string().c_str());
        else
            ImGui::Text("Paquete: %s", pkg.string().c_str());

        // Backend con el que arrancará el juego. No tiene por qué ser el del
        // editor: se exporta para la máquina del jugador, no para esta. Se
        // guarda en el game.cfg del paquete, no en el project.json.
        ImGui::Separator();
        const char* exportBackendNames[] = { "Vulkan", "DirectX 12" };
        ImGui::SetNextItemWidth(140.0f);
        ImGui::Combo("Render backend", &m_exportBackend, exportBackendNames,
                     IM_ARRAYSIZE(exportBackendNames));
        if (m_exportBackend == (int)RenderBackend::D3D12)
            ImGui::TextDisabled("En una maquina sin DirectX 12 el juego arranca con\n"
                                "Vulkan y lo deja dicho en game.log.");

        ImGui::BeginDisabled(!canExport);
        if (ImGui::Button("Export"))
        {
            // Missing/Empty: nada que perder, se exporta directo. PriorPackage:
            // hay un export anterior de verdad ahí, se confirma antes de
            // borrarlo (Occupied ya deshabilitó el botón más arriba).
            if (targetState == ExportTargetState::PriorPackage)
                m_openExportConfirmPopup = true;
            else
                runExport();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (m_openExportConfirmPopup)
    {
        ImGui::OpenPopup("Sobrescribir export");
        m_openExportConfirmPopup = false;
    }

    if (ImGui::BeginPopupModal("Sobrescribir export", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        // Misma ruta resuelta que el popup anterior, no el nombre crudo: es
        // literalmente lo que remove_all() va a borrar si el usuario
        // confirma, y el nombre por sí solo no lo representa (ver hallazgo
        // de review: "La carpeta '..' ya existe" no dice "voy a borrar
        // C:\Users\ruben").
        const std::filesystem::path pkg =
            std::filesystem::path(m_exportDestDir) / m_exportNameBuffer;
        // Solo se llega aquí con targetState == PriorPackage (ver botón
        // Export de arriba): pkg existe de verdad y contiene un game.scene,
        // así que no hace falta el matiz "no se pudo comprobar" que llevaba
        // antes este texto — Occupied (fallo de fs::status incluido) nunca
        // deja abrir este popup.
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                           "'%s' contiene un export anterior.", pkg.string().c_str());
        ImGui::Text("Se borrara todo su contenido antes de exportar.");
        if (ImGui::Button("Borrar y exportar"))
        {
            runExport();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void EditorUI::runExport()
{
    namespace fs = std::filesystem;

    // exportGame() toma Scene& (no Scene*): el chequeo de "hay escena
    // abierta" no puede vivir dentro de ella y se queda aquí, antes de
    // dereferenciar m_scene.
    if (!m_scene)
    {
        m_logPanel.push("Export cancelado: no hay escena abierta");
        return;
    }

    // Defensa en el punto que construye el paquete, no sólo en el menú. Los
    // dos popups del flujo (nombre y confirmación) son BeginPopupModal y sí
    // bloquean la toolbar, pero el diálogo de carpeta es IGFD y no: se puede
    // dejar abierto, pulsar Play y seguir. Y aunque hoy no quedara ningún
    // hueco, esta es la función que hay que blindar: es la que lee la escena.
    if (m_isPlaying)
    {
        m_logPanel.push("Export cancelado: para el Play Mode antes de exportar");
        return;
    }

    // El paquete se escribe dentro del proyecto abierto: exportar sobre la
    // carpeta de otro proyecto se rechaza aquí, antes de crear ni borrar nada.
    // El FORMATO del paquete y lo que hace exportGame() no cambian.
    if (!projectAllows(fs::path(m_exportDestDir) / m_exportNameBuffer, "Export"))
        return;

    std::error_code ec;
    fs::path projectRoot = fs::current_path(ec);
    if (ec) projectRoot = ".";
    fs::path canon = fs::canonical(projectRoot, ec);
    if (!ec) projectRoot = canon;

    const fs::path runtimeExe = projectRoot / "DonTopoRuntime.exe";
    const fs::path scriptsDir = m_scriptManager ? m_scriptManager->scriptsDirPath()
                                                : projectRoot / "Scripts";

    std::map<std::string, fs::path> scriptPaths;
    if (m_scriptManager)
        for (const auto& [name, cls] : m_scriptManager->getRegistry())
            scriptPaths[name] = cls.path;

    // Aviso, NO bloqueo: el paquete se arma con lo que referencia la escena, y
    // eso puede caer fuera del proyecto. Los assets compartidos del repo son
    // legítimos y viajan como siempre; lo que interesa cantar es un asset de
    // OTRO proyecto, que sí es una fuga. Se avisa y se exporta igual —dejarlo
    // fuera del paquete daría un juego sin ese asset, que es peor.
    if (m_project && m_project->valid())
    {
        const ProjectContext workspace(ProjectContext::workspaceDir());
        for (const ExportAsset& a : collectSceneAssets(*m_scene, projectRoot, scriptPaths))
        {
            const fs::path src(a.sourcePath);
            if (m_project->contains(src))
                continue;
            m_logPanel.push(std::string("[Project] Aviso: el export incluye un asset de ") +
                            (workspace.contains(src) ? "OTRO proyecto: " : "fuera del proyecto: ") +
                            a.sourcePath);
        }
    }

    const RenderBackend exportBackend = (m_exportBackend == (int)RenderBackend::D3D12)
                                            ? RenderBackend::D3D12
                                            : RenderBackend::Vulkan;
    ExportResult result = exportGame(*m_scene, scriptPaths, m_exportDestDir,
                                     m_exportNameBuffer, projectRoot, scriptsDir, runtimeExe,
                                     exportBackend, m_skyboxFolder);
    for (const std::string& msg : result.messages)
        m_logPanel.push(msg);
    if (!result.ok)
        m_logPanel.push("Export FALLIDO");
}

} // namespace DonTopo
