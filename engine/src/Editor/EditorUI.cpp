#include "DonTopo/Editor/EditorUI.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Renderer/Renderer.h"
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
#include <stdexcept>

namespace DonTopo {

EditorUI::EditorUI()
    : m_sceneFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_exportDialog(std::make_unique<IGFD::FileDialog>())
    , m_renderer(std::make_unique<Renderer>())
    , m_scriptEditor(std::make_unique<ScriptEditorPanel>())
{
    m_scriptEditor->setLogCallback([this](const std::string& msg) { m_logPanel.push(msg); });
    // El Renderer nos llamará de vuelta por aquí para el pass de UI. Se
    // registra en el constructor porque tiene que estar puesto antes de
    // initPresentation(), que es quien arranca la UI.
    m_renderer->setUiLayer(this);
    // Liberar los recursos GPU del subárbol justo antes de desengancharlo:
    // lo hacía el Renderer al fijar la raíz de escena, y ahora que el dueño
    // es el editor lo cablea él.
    setOnDelete([this](GameObject* node) { m_renderer->removeGameObject(node); });
}

EditorUI::~EditorUI() = default;

Renderer& EditorUI::renderer() { return *m_renderer; }

// ─── UiLayer: backend de ImGui ───────────────────────────────────────────────

void EditorUI::initUi(const InitInfo& info)
{
    m_uiDevice = info.device;

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
    initInfo.Instance                         = info.instance;
    initInfo.PhysicalDevice                   = info.physicalDevice;
    initInfo.Device                           = info.device;
    initInfo.QueueFamily                      = info.queueFamily;
    initInfo.Queue                            = info.queue;
    initInfo.DescriptorPool                   = m_uiDescPool;
    initInfo.MinImageCount                    = 2;
    initInfo.ImageCount                       = info.imageCount;
    initInfo.PipelineInfoMain.RenderPass      = info.renderPass;
    initInfo.PipelineInfoMain.MSAASamples     = VK_SAMPLE_COUNT_1_BIT;
    ImGui_ImplVulkan_Init(&initInfo);

    printf("ImGui init OK\n"); fflush(stdout);
}

void EditorUI::shutdownUi()
{
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    if (m_uiDescPool)
    {
        vkDestroyDescriptorPool(m_uiDevice, m_uiDescPool, nullptr);
        m_uiDescPool = VK_NULL_HANDLE;
    }
}

VkDescriptorSet EditorUI::registerUiTexture(VkSampler sampler, VkImageView view)
{
    return ImGui_ImplVulkan_AddTexture(sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void EditorUI::unregisterUiTexture(VkDescriptorSet set)
{
    ImGui_ImplVulkan_RemoveTexture(set);
}

void EditorUI::buildUiFrame(VkDescriptorSet viewportTexture, GameObject* sceneRoot,
                            const glm::mat4& cameraView)
{
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    draw(viewportTexture, sceneRoot, cameraView);

    ImGui::Render();
}

void EditorUI::recordUi(VkCommandBuffer cmd)
{
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

namespace {

// Nombres de los modos que se guardan en el project.json. Son los MISMOS
// literales que ofrecen los combos del menú View (aaNames/fpNames): el ajuste
// se persiste por nombre, así que reordenar o insertar una opción en el array
// no cambia lo que ya hay guardado.
const char* aaModeName(Renderer::AaMode mode)
{
    switch (mode)
    {
        case Renderer::AaMode::Fxaa: return "FXAA";
        case Renderer::AaMode::Ssaa: return "SSAA";
        case Renderer::AaMode::Msaa: return "MSAA";
        case Renderer::AaMode::Taa:  return "TAA";
        default:                     return "None";
    }
}

// ok = false si el nombre no es ninguno de los de hoy (fichero de una versión
// futura, o editado a mano): el caller se cae al default y lo deja en el Log.
Renderer::AaMode aaModeFromName(const std::string& name, bool& ok)
{
    ok = true;
    if (name == "None") return Renderer::AaMode::None;
    if (name == "FXAA") return Renderer::AaMode::Fxaa;
    if (name == "SSAA") return Renderer::AaMode::Ssaa;
    if (name == "MSAA") return Renderer::AaMode::Msaa;
    if (name == "TAA")  return Renderer::AaMode::Taa;
    ok = false;
    return Renderer::AaMode::None;
}

const char* fpModeName(Renderer::FpMode mode)
{
    switch (mode)
    {
        case Renderer::FpMode::Tiled:     return "Tiled";
        case Renderer::FpMode::Clustered: return "Clustered";
        default:                          return "Off";
    }
}

Renderer::FpMode fpModeFromName(const std::string& name, bool& ok)
{
    ok = true;
    if (name == "Off")       return Renderer::FpMode::Off;
    if (name == "Tiled")     return Renderer::FpMode::Tiled;
    if (name == "Clustered") return Renderer::FpMode::Clustered;
    ok = false;
    return Renderer::FpMode::Off;
}

} // namespace

ProjectContext::ViewSettings EditorUI::currentSettings()
{
    ProjectContext::ViewSettings s;

    // El backend NO sale del Renderer: es el que el usuario ha elegido para el
    // próximo arranque, que puede no ser con el que corre este proceso.
    s.renderBackend = renderBackendName(m_selectedBackend);

    if (m_renderer)
    {
        s.ambient = m_renderer->ambientEnabled();
        s.bloom   = m_renderer->bloomEnabled();
        s.ssao    = m_renderer->ssaoEnabled();
        s.ssr     = m_renderer->ssrEnabled();
        s.fog     = m_renderer->fogEnabled();
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
        s.fxaaSubpix           = m_renderer->fxaaSubpix();
        s.fxaaEdgeThreshold    = m_renderer->fxaaEdgeThreshold();
        s.fxaaEdgeThresholdMin = m_renderer->fxaaEdgeThresholdMin();
        s.ssaaFactor           = m_renderer->ssaaFactor();
        s.msaaSamples          = m_renderer->msaaSamples();
        s.taaFeedback          = m_renderer->taaFeedback();
        s.taaJitterScale       = m_renderer->taaJitterScale();
        s.fpLightRadius        = m_renderer->forwardPlusLightRadius();
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

    m_renderer->setAmbientEnabled(s.ambient);
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
    const Renderer::AaMode aa = aaModeFromName(s.aaMode, aaOk);
    if (!aaOk)
        m_logPanel.push("Modo de anti-aliasing desconocido en el proyecto ('" + s.aaMode + "'): se usa None");
    m_renderer->setAaMode(aa);

    bool fpOk = true;
    const Renderer::FpMode fp = fpModeFromName(s.fpMode, fpOk);
    if (!fpOk)
        m_logPanel.push("Modo de Forward+ desconocido en el proyecto ('" + s.fpMode + "'): se usa Off");
    m_renderer->setForwardPlusMode(fp);
    m_renderer->setForwardPlusLightRadius(s.fpLightRadius);

    // Backend de render: se LEE pero no se aplica. El device de este proceso ya
    // está creado —el selector de proyecto se dibuja sobre él—, así que lo único
    // que se puede hacer es dejarlo elegido para el próximo arranque y avisar.
    bool backendOk = true;
    m_selectedBackend = renderBackendFromName(s.renderBackend, backendOk);
    if (!backendOk)
        m_logPanel.push("Backend de render desconocido en el proyecto ('" + s.renderBackend +
                        "'): se usa Vulkan");
    if (m_selectedBackend != m_activeBackend)
        m_logPanel.push(std::string("Este proyecto pide el backend ") +
                        renderBackendName(m_selectedBackend) + " y el editor está corriendo con " +
                        renderBackendName(m_activeBackend) + ": reinicia para aplicarlo");

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

void EditorUI::saveProjectSettings()
{
    if (!m_project || !m_project->valid())
        return; // sin proyecto abierto esto no corre: comportamiento de antes.

    if (!ProjectContext::writeSettings(m_project->root(), currentSettings()))
        m_logPanel.push("No se pudieron guardar los ajustes en el project.json");
}

void EditorUI::draw(VkDescriptorSet viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView)
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

void EditorUI::onAssetsLoaded(std::vector<LoadedMesh> results, Scene& scene, Renderer& renderer)
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
            panelToggled |= ImGui::MenuItem("Performance", nullptr, m_performancePanel.GetOpenPtr());
            panelToggled |= ImGui::MenuItem("Input Actions", nullptr, m_inputActionsPanel.GetOpenPtr());
            if (panelToggled)
                saveProjectSettings();
            ImGui::Separator();
            // Peso del ambiente IBL. Ajuste de sesion: no se serializa en la
            // escena, asi que al reabrir el editor vuelve a 1.0.
            if (m_renderer)
            {
                bool ambientOn = m_renderer->ambientEnabled();
                if (ImGui::Checkbox("Ambient (IBL)", &ambientOn))
                {
                    m_renderer->setAmbientEnabled(ambientOn);
                    saveProjectSettings();
                }

                // Igual que en el bloom: el slider no se oculta con el ambiente
                // apagado, se deja desactivado.
                ImGui::BeginDisabled(!ambientOn);
                float ambient = m_renderer->ambientIntensity();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("Ambient intensity", &ambient, 0.0f, 3.0f, "%.2f"))
                    m_renderer->setAmbientIntensity(ambient);
                // Se guarda al SOLTAR: arrastrar de punta a punta escribe una
                // vez, no una por frame. Mismo criterio en todos los sliders.
                if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();
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
                ImGui::Text("Sondas: %d  (%.2f MB c/u)", probes,
                            (double)Renderer::probeMemoryBytes() / (1024.0 * 1024.0));
                ImGui::Text("Ultimo bake: %.2f ms de GPU", m_renderer->lastProbeBakeMs());

                // Bloom. Mismo criterio que el ambiente: ajuste de sesion, no se
                // serializa. Intensity 0 deja la imagen como antes del bloom.
                ImGui::Separator();
                bool bloom = m_renderer->bloomEnabled();
                if (ImGui::Checkbox("Bloom", &bloom))
                {
                    m_renderer->setBloomEnabled(bloom);
                    saveProjectSettings();
                }

                // Igual que en el SSAO y el SSR: los sliders no se ocultan con el
                // efecto apagado, se dejan desactivados.
                ImGui::BeginDisabled(!bloom);
                float threshold = m_renderer->bloomThreshold();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("Bloom threshold", &threshold, 0.0f, 5.0f, "%.2f"))
                    m_renderer->setBloomThreshold(threshold);
                if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();

                float knee = m_renderer->bloomKnee();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("Bloom knee", &knee, 0.0f, 1.0f, "%.2f"))
                    m_renderer->setBloomKnee(knee);
                if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();

                float intensity = m_renderer->bloomIntensity();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("Bloom intensity", &intensity, 0.0f, 1.0f, "%.3f"))
                    m_renderer->setBloomIntensity(intensity);
                if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();
                ImGui::EndDisabled();

                ImGui::Text("Bloom GPU: %.3f ms", m_renderer->bloomGpuMs());

                // SSAO. Mismo criterio que el ambiente y el bloom: ajuste de
                // sesion, no se serializa. Apagado deja la imagen exactamente
                // como antes de la feature y el coste GPU a cero.
                ImGui::Separator();
                bool ssao = m_renderer->ssaoEnabled();
                if (ImGui::Checkbox("SSAO", &ssao))
                {
                    m_renderer->setSsaoEnabled(ssao);
                    saveProjectSettings();
                }

                // Los sliders no se ocultan con el efecto apagado: se dejan
                // desactivados para que se vea que existen y con que valores
                // arrancarian.
                ImGui::BeginDisabled(!ssao);
                float ssaoRadius = m_renderer->ssaoRadius();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("SSAO radius", &ssaoRadius, 0.05f, 2.0f, "%.2f"))
                    m_renderer->setSsaoRadius(ssaoRadius);
                if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();

                float ssaoBias = m_renderer->ssaoBias();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("SSAO bias", &ssaoBias, 0.0f, 0.2f, "%.3f"))
                    m_renderer->setSsaoBias(ssaoBias);
                if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();

                float ssaoIntensity = m_renderer->ssaoIntensity();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("SSAO intensity", &ssaoIntensity, 0.0f, 3.0f, "%.2f"))
                    m_renderer->setSsaoIntensity(ssaoIntensity);
                if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();

                float ssaoPower = m_renderer->ssaoPower();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("SSAO power", &ssaoPower, 0.25f, 4.0f, "%.2f"))
                    m_renderer->setSsaoPower(ssaoPower);
                if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();
                ImGui::EndDisabled();

                ImGui::Text("SSAO GPU: %.3f ms", m_renderer->ssaoGpuMs());

                // SSR: interruptor global. La fuerza es POR GAMEOBJECT (panel
                // Properties), asi que con esto puesto pero ningun objeto marcado
                // tampoco se graba nada.
                ImGui::Separator();
                bool ssr = m_renderer->ssrEnabled();
                if (ImGui::Checkbox("SSR", &ssr))
                {
                    m_renderer->setSsrEnabled(ssr);
                    saveProjectSettings();
                }

                ImGui::BeginDisabled(!ssr);
                float ssrDist = m_renderer->ssrMaxDistance();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("SSR distance", &ssrDist, 0.5f, 50.0f, "%.1f"))
                    m_renderer->setSsrMaxDistance(ssrDist);
                if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();

                float ssrThick = m_renderer->ssrThickness();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("SSR thickness", &ssrThick, 0.01f, 3.0f, "%.2f"))
                    m_renderer->setSsrThickness(ssrThick);
                if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();

                int ssrSteps = m_renderer->ssrMaxSteps();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderInt("SSR steps", &ssrSteps, 8, 128))
                    m_renderer->setSsrMaxSteps(ssrSteps);
                if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();

                float ssrEdge = m_renderer->ssrEdgeFade();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("SSR edge fade", &ssrEdge, 0.0f, 0.5f, "%.3f"))
                    m_renderer->setSsrEdgeFade(ssrEdge);
                if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();

                float ssrInt = m_renderer->ssrIntensity();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("SSR intensity", &ssrInt, 0.0f, 2.0f, "%.2f"))
                    m_renderer->setSsrIntensity(ssrInt);
                if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();
                ImGui::EndDisabled();

                ImGui::Text("SSR GPU: %.3f ms", m_renderer->ssrGpuMs());

                // Niebla volumetrica: interruptor global, ajuste de sesion (no
                // se serializa) igual que el bloom, el SSAO y el SSR. Apagada
                // deja la imagen exactamente como antes de la feature y el coste
                // GPU a cero.
                ImGui::Separator();
                bool fog = m_renderer->fogEnabled();
                if (ImGui::Checkbox("Volumetric Fog", &fog))
                {
                    m_renderer->setFogEnabled(fog);
                    saveProjectSettings();
                }

                // Como en el SSAO y el SSR: los sliders no se ocultan con el
                // efecto apagado, se dejan desactivados.
                ImGui::BeginDisabled(!fog);
                float fogDensity = m_renderer->fogDensity();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("Fog density", &fogDensity, 0.0f, 0.5f, "%.3f"))
                    m_renderer->setFogDensity(fogDensity);
                if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();

                float fogFalloff = m_renderer->fogHeightFalloff();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("Fog height falloff", &fogFalloff, 0.0f, 0.5f, "%.3f"))
                    m_renderer->setFogHeightFalloff(fogFalloff);
                if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();

                float fogBase = m_renderer->fogBaseHeight();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("Fog base height", &fogBase, -50.0f, 50.0f, "%.1f"))
                    m_renderer->setFogBaseHeight(fogBase);
                if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();

                float fogG = m_renderer->fogAnisotropy();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("Fog anisotropy", &fogG, -0.95f, 0.95f, "%.2f"))
                    m_renderer->setFogAnisotropy(fogG);
                if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();

                int fogSteps = m_renderer->fogSteps();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderInt("Fog steps", &fogSteps, 8, 128))
                    m_renderer->setFogSteps(fogSteps);
                if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();

                glm::vec3 scatter = m_renderer->fogScatter();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::ColorEdit3("Fog scattering", &scatter.x))
                    m_renderer->setFogScatter(scatter);
                if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();
                ImGui::EndDisabled();

                ImGui::Text("Fog GPU: %.3f ms", m_renderer->fogGpuMs());

                // Anti-aliasing. Modos EXCLUYENTES, cada uno con sus propios
                // parametros. Mismo criterio que el resto: ajuste de sesion, no
                // se serializa. En None no se graba ni un comando de mas y la
                // imagen es identica a la de antes de la feature.
                ImGui::Separator();
                using AaMode = Renderer::AaMode;
                const char* aaNames[] = { "None", "FXAA", "SSAA", "MSAA", "TAA" };
                int aaCurrent = (int)m_renderer->aaMode();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::Combo("Anti-aliasing", &aaCurrent, aaNames, IM_ARRAYSIZE(aaNames)))
                {
                    m_renderer->setAaMode((AaMode)aaCurrent);
                    // Se guarda el NOMBRE del modo, no este indice: ver
                    // aaModeName() al principio del fichero.
                    saveProjectSettings();
                }

                const AaMode aaMode = m_renderer->aaMode();

                if (aaMode == AaMode::Fxaa)
                {
                    float fxaaSubpix = m_renderer->fxaaSubpix();
                    ImGui::SetNextItemWidth(140.0f);
                    if (ImGui::SliderFloat("FXAA subpixel", &fxaaSubpix, 0.0f, 1.0f, "%.2f"))
                        m_renderer->setFxaaSubpix(fxaaSubpix);
                    if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();

                    float fxaaEdge = m_renderer->fxaaEdgeThreshold();
                    ImGui::SetNextItemWidth(140.0f);
                    if (ImGui::SliderFloat("FXAA edge threshold", &fxaaEdge, 0.063f, 0.333f, "%.3f"))
                        m_renderer->setFxaaEdgeThreshold(fxaaEdge);
                    if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();

                    float fxaaEdgeMin = m_renderer->fxaaEdgeThresholdMin();
                    ImGui::SetNextItemWidth(140.0f);
                    if (ImGui::SliderFloat("FXAA edge min", &fxaaEdgeMin, 0.0312f, 0.0833f, "%.4f"))
                        m_renderer->setFxaaEdgeThresholdMin(fxaaEdgeMin);
                    if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();
                }
                else if (aaMode == AaMode::Ssaa)
                {
                    // Cambiar el factor recrea TODOS los targets internos, asi
                    // que se aplica al soltar el slider y no a cada pixel
                    // arrastrado: reconstruir el render entero 60 veces por
                    // segundo mientras se arrastra congelaria el editor.
                    static float pendingFactor = m_renderer->ssaaFactor();
                    if (!ImGui::IsAnyItemActive()) pendingFactor = m_renderer->ssaaFactor();
                    ImGui::SetNextItemWidth(140.0f);
                    ImGui::SliderFloat("SSAA factor", &pendingFactor, 1.25f, 2.0f, "%.2fx");
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        m_renderer->setSsaaFactor(pendingFactor);
                        saveProjectSettings();
                    }
                    ImGui::TextDisabled("%.2fx pixeles por frame", pendingFactor * pendingFactor);
                }
                else if (aaMode == AaMode::Msaa)
                {
                    const int maxSamples = m_renderer->maxMsaaSamples();
                    int samples = m_renderer->msaaSamples();
                    // Solo se ofrecen las cuentas que soporta el device para
                    // color Y profundidad a la vez: el pass de escena usa las dos.
                    for (int s = 2; s <= 8; s *= 2)
                    {
                        if (s > maxSamples) break;
                        if (s > 2) ImGui::SameLine();
                        char label[8];
                        snprintf(label, sizeof(label), "%dx", s);
                        if (ImGui::RadioButton(label, samples == s))
                        {
                            m_renderer->setMsaaSamples(s);
                            saveProjectSettings();
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(max %dx)", maxSamples);
                }
                else if (aaMode == AaMode::Taa)
                {
                    float feedback = m_renderer->taaFeedback();
                    ImGui::SetNextItemWidth(140.0f);
                    if (ImGui::SliderFloat("TAA feedback", &feedback, 0.0f, 0.98f, "%.2f"))
                        m_renderer->setTaaFeedback(feedback);
                    if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();

                    float jitter = m_renderer->taaJitterScale();
                    ImGui::SetNextItemWidth(140.0f);
                    if (ImGui::SliderFloat("TAA jitter", &jitter, 0.0f, 2.0f, "%.2f"))
                        m_renderer->setTaaJitterScale(jitter);
                    if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();
                }

                // El pass propio solo existe en FXAA, SSAA y TAA. El coste del
                // MSAA y el del supersampling estan repartidos en el render, y
                // por eso se muestra tambien el total: comparandolo con el de
                // None sale el sobrecoste real del modo.
                ImGui::Text("AA GPU: %.3f ms", m_renderer->aaGpuMs());
                ImGui::Text("Render GPU: %.3f ms", m_renderer->renderGpuMs());

                // Forward+. Modos EXCLUYENTES, igual que el AA: en Off no se
                // graba ni un dispatch y pbr.frag recorre las luces del UBO como
                // siempre. Ajuste de sesion, no se serializa: asi el runtime y el
                // editor arrancan en el mismo modo y renderizan igual.
                ImGui::Separator();
                using FpMode = Renderer::FpMode;
                const char* fpNames[] = { "Off", "Tiled", "Clustered" };
                int fpCurrent = (int)m_renderer->forwardPlusMode();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::Combo("Forward+", &fpCurrent, fpNames, IM_ARRAYSIZE(fpNames)))
                {
                    m_renderer->setForwardPlusMode((FpMode)fpCurrent);
                    saveProjectSettings();
                }

                if (m_renderer->forwardPlusMode() != FpMode::Off)
                {
                    // El radio es lo que hace que el culling sirva de algo: con
                    // uno enorme toda luz cae en toda celda y la lista se llena.
                    float radius = m_renderer->forwardPlusLightRadius();
                    ImGui::SetNextItemWidth(140.0f);
                    if (ImGui::SliderFloat("Light radius", &radius, 50.0f, 5000.0f, "%.0f"))
                        m_renderer->setForwardPlusLightRadius(radius);
                    if (ImGui::IsItemDeactivatedAfterEdit()) saveProjectSettings();

                    ImGui::Text("Forward+ GPU: %.3f ms", m_renderer->forwardPlusGpuMs());
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
            // Gate de reproducción: sin Audio Listener en la escena (o con el
            // suyo deshabilitado) no suena ningún clip. Mismo gate que el
            // runtime (runtime/main.cpp), y por la misma razón fuera de
            // AudioManager/AudioClipComponent. Un solo aviso, no uno por clip.
            GameObject* listenerGo = m_scene->findAudioListener();
            const bool listenerActive = listenerGo && listenerGo->getAudioListener()->getEnabled();
            if (!listenerActive)
                m_logPanel.push("Sin Audio Listener en la escena: los AudioClip no se reproduciran");
            else
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
        m_renderer->setWireframeMode(!wireframe);
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

    // Libera recursos GPU de la escena actual y resetea sus índices a -1:
    // si fromJson falla más abajo por malformación anidada, m_root sigue
    // siendo este mismo árbol (Scene::fromJson es atómico), y resetear los
    // índices aquí permite que el traverse de re-registro de abajo lo
    // vuelva a registrar igual que si fuera el árbol nuevo — sin esto, el
    // árbol viejo quedaría con índices obsoletos (Renderer::removeGameObject
    // no los resetea) y sin re-registrar tras un fallo, dejando el viewport
    // vacío pese a que los datos de Scene no cambiaron.
    for (auto& child : m_scene->getRoot().children)
    {
        m_renderer->removeGameObject(child.get());
        child->traverse([](GameObject* go) {
            go->staticRenderIndex = -1;
            go->skinnedRenderIndex = -1;
        });
    }

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
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                               "El backend DirectX 12 todavia no dibuja escenas: el juego\n"
                               "arrancara con Vulkan y lo dejara dicho en game.log.");

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
                                     exportBackend);
    for (const std::string& msg : result.messages)
        m_logPanel.push(msg);
    if (!result.ok)
        m_logPanel.push("Export FALLIDO");
}

} // namespace DonTopo
