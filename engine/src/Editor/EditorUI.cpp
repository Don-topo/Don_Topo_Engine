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

void EditorUI::draw(VkDescriptorSet viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView)
{
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
        [this](const std::filesystem::path& p) { m_scriptEditor->openFile(p); },
        [this]() { m_animatorPanel.open(); },
        m_assetLoader,
        m_loadingModal.active(),   // veta la edición mientras el modal carga
    };

    m_scenePanel.draw(ctx, sceneRoot);
    // ScenePanel ha borrado el GameObject seleccionado — invalida los caches
    // de edición de Properties pa que no arrastren punteros colgantes
    // (GameObject / BoxCollider ya liberados) hasta la próxima selección real.
    if (m_scenePanel.selectionWasDeletedThisFrame())
        m_propertiesPanel.invalidateCaches();
    m_viewportPanel.draw(ctx, viewportTexture, cameraView);
    m_propertiesPanel.draw(ctx);
    m_logPanel.draw();
    drawSceneDialog();
    drawExportDialog();
    m_contentBrowserPanel.draw(ctx, sceneRoot);
    m_scriptEditor->draw();
    m_animatorPanel.draw(ctx);

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
                cfg.path  = ".";
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
            ImGui::MenuItem("Scene", nullptr, m_scenePanel.GetOpenPtr());
            ImGui::MenuItem("Viewport", nullptr, m_viewportPanel.GetOpenPtr());
            ImGui::MenuItem("Properties", nullptr, m_propertiesPanel.GetOpenPtr());
            ImGui::MenuItem("Log", nullptr, m_logPanel.GetOpenPtr());
            ImGui::MenuItem("Content Browser", nullptr, m_contentBrowserPanel.GetOpenPtr());
            ImGui::MenuItem("Script Editor", nullptr, m_scriptEditor->GetOpenPtr());
            ImGui::MenuItem("Animator", nullptr, m_animatorPanel.GetOpenPtr());
            ImGui::Separator();
            // Peso del ambiente IBL. Ajuste de sesion: no se serializa en la
            // escena, asi que al reabrir el editor vuelve a 1.0.
            if (m_renderer)
            {
                float ambient = m_renderer->ambientIntensity();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("Ambient (IBL)", &ambient, 0.0f, 3.0f, "%.2f"))
                    m_renderer->setAmbientIntensity(ambient);

                // Bloom. Mismo criterio que el ambiente: ajuste de sesion, no se
                // serializa. Intensity 0 deja la imagen como antes del bloom.
                ImGui::Separator();
                float threshold = m_renderer->bloomThreshold();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("Bloom threshold", &threshold, 0.0f, 5.0f, "%.2f"))
                    m_renderer->setBloomThreshold(threshold);

                float knee = m_renderer->bloomKnee();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("Bloom knee", &knee, 0.0f, 1.0f, "%.2f"))
                    m_renderer->setBloomKnee(knee);

                float intensity = m_renderer->bloomIntensity();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("Bloom intensity", &intensity, 0.0f, 1.0f, "%.3f"))
                    m_renderer->setBloomIntensity(intensity);

                ImGui::Text("Bloom GPU: %.3f ms", m_renderer->bloomGpuMs());

                // SSAO. Mismo criterio que el ambiente y el bloom: ajuste de
                // sesion, no se serializa. Apagado deja la imagen exactamente
                // como antes de la feature y el coste GPU a cero.
                ImGui::Separator();
                bool ssao = m_renderer->ssaoEnabled();
                if (ImGui::Checkbox("SSAO", &ssao))
                    m_renderer->setSsaoEnabled(ssao);

                // Los sliders no se ocultan con el efecto apagado: se dejan
                // desactivados para que se vea que existen y con que valores
                // arrancarian.
                ImGui::BeginDisabled(!ssao);
                float ssaoRadius = m_renderer->ssaoRadius();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("SSAO radius", &ssaoRadius, 0.05f, 2.0f, "%.2f"))
                    m_renderer->setSsaoRadius(ssaoRadius);

                float ssaoBias = m_renderer->ssaoBias();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("SSAO bias", &ssaoBias, 0.0f, 0.2f, "%.3f"))
                    m_renderer->setSsaoBias(ssaoBias);

                float ssaoIntensity = m_renderer->ssaoIntensity();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("SSAO intensity", &ssaoIntensity, 0.0f, 3.0f, "%.2f"))
                    m_renderer->setSsaoIntensity(ssaoIntensity);

                float ssaoPower = m_renderer->ssaoPower();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("SSAO power", &ssaoPower, 0.25f, 4.0f, "%.2f"))
                    m_renderer->setSsaoPower(ssaoPower);
                ImGui::EndDisabled();

                ImGui::Text("SSAO GPU: %.3f ms", m_renderer->ssaoGpuMs());

                // SSR: interruptor global. La fuerza es POR GAMEOBJECT (panel
                // Properties), asi que con esto puesto pero ningun objeto marcado
                // tampoco se graba nada.
                ImGui::Separator();
                bool ssr = m_renderer->ssrEnabled();
                if (ImGui::Checkbox("SSR", &ssr))
                    m_renderer->setSsrEnabled(ssr);

                ImGui::BeginDisabled(!ssr);
                float ssrDist = m_renderer->ssrMaxDistance();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("SSR distance", &ssrDist, 0.5f, 50.0f, "%.1f"))
                    m_renderer->setSsrMaxDistance(ssrDist);

                float ssrThick = m_renderer->ssrThickness();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("SSR thickness", &ssrThick, 0.01f, 3.0f, "%.2f"))
                    m_renderer->setSsrThickness(ssrThick);

                int ssrSteps = m_renderer->ssrMaxSteps();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderInt("SSR steps", &ssrSteps, 8, 128))
                    m_renderer->setSsrMaxSteps(ssrSteps);

                float ssrEdge = m_renderer->ssrEdgeFade();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("SSR edge fade", &ssrEdge, 0.0f, 0.5f, "%.3f"))
                    m_renderer->setSsrEdgeFade(ssrEdge);

                float ssrInt = m_renderer->ssrIntensity();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("SSR intensity", &ssrInt, 0.0f, 2.0f, "%.2f"))
                    m_renderer->setSsrIntensity(ssrInt);
                ImGui::EndDisabled();

                ImGui::Text("SSR GPU: %.3f ms", m_renderer->ssrGpuMs());
            }
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
        cfg.path  = "assets";
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
        cfg.path  = "assets";
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
    }
    m_undoHistory.clear();

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
        m_logPanel.push("Operación de escena cancelada: no se puede guardar ni cargar en Play Mode");
        return;
    }

    if (m_sceneFileDialog->IsOk())
    {
        std::string path = m_sceneFileDialog->GetFilePathName();

        if (m_sceneDlgIsSave)
        {
            bool saved   = m_scene && m_scene->save(path);
            m_sceneIOError = saved ? "" : "No se pudo guardar la escena";
            m_logPanel.push(saved ? ("Escena guardada: " + path) : ("Error al guardar escena: " + path));
        }
        else
        {
            // Valida la estructura básica del JSON ANTES de tocar GPU/Scene:
            // rechaza un fichero top-level corrupto sin tocar nada (fast
            // path, evita el churn de GPU de reloadSceneFromJson). No cubre
            // malformación anidada — para eso, Scene::fromJson es atómico y
            // reloadSceneFromJson cubre ambos desenlaces.
            auto parsed = FileManager::readJson(path);
            bool structureOk = parsed.has_value() &&
                                parsed->contains("version") && (*parsed)["version"].is_number_integer() &&
                                (*parsed)["version"].get<int>() == 1 &&
                                parsed->contains("root") && (*parsed)["root"].is_object();

            bool loaded  = structureOk && reloadSceneFromJson(*parsed, /*async=*/true);
            m_sceneIOError = loaded ? "" : "No se pudo cargar la escena";
            m_logPanel.push(loaded ? ("Escena cargada: " + path) : ("Error al cargar escena: " + path));
        }
    }

    m_sceneFileDialog->Close();
    m_sceneDlgOpen = false;
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

    ExportResult result = exportGame(*m_scene, scriptPaths, m_exportDestDir,
                                     m_exportNameBuffer, projectRoot, scriptsDir, runtimeExe);
    for (const std::string& msg : result.messages)
        m_logPanel.push(msg);
    if (!result.ok)
        m_logPanel.push("Export FALLIDO");
}

} // namespace DonTopo
