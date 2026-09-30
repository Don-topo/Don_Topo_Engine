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
#include <algorithm>
#include <cassert>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include "DonTopo/Renderer/EditorRenderer.h"
#include "DonTopo/Audio/AudioListenerComponent.h"

namespace DonTopo {

namespace {
// Collision layers window open or not. It lives here and not in EditorUI because
// it is pure UI state (which window is visible), not a project setting, and
// the editor header is outside the scope of this feature. There is a single
// EditorUI per process.
bool g_showLayerMatrix = false;

// Layer the user has asked to delete and is awaiting confirmation; -1 = none.
// Deleting RENUMBERS (see PhysicsManager::removeLayer), so it is not a change
// that can be undone with Ctrl+Z: it is asked about first.
int g_layerPendienteDeBorrar = -1;

// Collision layers window. A free function and NOT part of drawMenuBar: if it
// were drawn from there it would come out before the dockspace and ImGui would not let it
// dock with the rest of the panels. Called by draw() right after
// drawDockSpace().
//
// onChanged saves the settings to project.json (EditorUI::saveProjectSettings).
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
            ImGui::TextDisabled("No PhysicsManager: there are no layers to edit.");
            ImGui::End();
            return;
        }

        ImGui::TextWrapped(
            "The project's collision layers. The matrix is SYMMETRIC: checking (a,b) "
            "also checks (b,a), which is why only the upper half is drawn. Everything "
            "checked = no filtering, the default behavior.");
        ImGui::Separator();

        const int total = physics->layerCount();

        // --- Layer list: name + delete --------------------------------------
        for (int i = 0; i < total; ++i)
        {
            ImGui::PushID(i);
            ImGui::Text("%2d", i);
            ImGui::SameLine();

            char buf[64] = {};
            std::snprintf(buf, sizeof(buf), "%s", physics->getLayerName(i).c_str());
            ImGui::SetNextItemWidth(220.0f);
            // It is written to the manager on every key (so that the labels of the
            // matrix and of the collider are seen on the fly) but project.json is only
            // saved on CONFIRM: otherwise, it would be rewritten letter by letter.
            if (ImGui::InputText("##nombreCapa", buf, sizeof(buf)))
                physics->setLayerName(i, buf);
            if (ImGui::IsItemDeactivatedAfterEdit())
                onChanged();

            ImGui::SameLine();
            // Layer 0 is the fallback for deletions: it cannot be removed.
            ImGui::BeginDisabled(i == 0);
            if (ImGui::SmallButton("x"))
                g_layerPendienteDeBorrar = i;
            ImGui::EndDisabled();
            if (i == 0)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(the default layer cannot be deleted)");
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
            ImGui::TextDisabled("at most %d layers", PhysicsManager::kLayerCount);
        }

        // --- Delete confirmation --------------------------------------------
        if (g_layerPendienteDeBorrar > 0)
            ImGui::OpenPopup("Delete layer");
        if (ImGui::BeginPopupModal("Delete layer", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            const int capa = g_layerPendienteDeBorrar;
            ImGui::Text("Delete layer %d (\"%s\")?", capa,
                        capa > 0 ? physics->getLayerName(capa).c_str() : "");
            ImGui::TextWrapped(
                "Colliders using it will move to layer 0, the layers "
                "above will shift down one index and the matrix will lose its row and "
                "column. It CANNOT be undone with Ctrl+Z.");
            ImGui::Separator();
            if (ImGui::Button("Delete"))
            {
                if (physics->removeLayer(capa)) onChanged();
                g_layerPendienteDeBorrar = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                g_layerPendienteDeBorrar = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // --- Matrix ---------------------------------------------------------
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

            // Only b >= a: the symmetric cell is written by setLayerCollision, and
            // drawing both would give two controls for the same datum.
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
    // onDelete used to be wired here to release the subtree's GPU before
    // deleting it. It is no longer needed: Scene::setOnNodeRemoved notifies, which is
    // wired by the host only once and covers the three callers of
    // removeGameObject (this panel, the Undo command and Lua's Scene.Destroy)
    // instead of just one.
}

EditorUI::~EditorUI() = default;

void EditorUI::setRenderer(std::unique_ptr<EditorRenderer> renderer)
{
    m_renderer = std::move(renderer);

    // The backend calls back through here to record the interface pass.
    // It has to be set BEFORE presentation starts, and this is
    // the first moment there is a backend to tell: the editor no longer
    // builds it, it is given to it.
    if (m_renderer)
        m_renderer->setUiLayer(this);
}

void EditorUI::setProject(const ProjectContext* project)
{
    m_project = project;

    // Root against which Scene relativizes/resolves the hand-assigned texture
    // paths (Task 6). It has to be set HERE, before
    // openProjectScene(), and not in applyProjectSettings(): while the project
    // selector is still on screen, draw() yields the whole frame to the callback and
    // returns without reaching applyProjectSettings, so the startup scene
    // (the one openChosenProject() loads synchronously inside that same
    // callback, right after this setProject()) would be loaded with the root
    // still empty and its relative paths would stay unresolved. Setting it
    // here covers the three load paths: the startup scene (through
    // this setProject), the File menu's Load Scene and the Play->Stop restore
    // (the last two already run with the project applied, much later).
    //
    // The SAME assignment repeated in setScene, further down: it is NOT duplication to
    // clean up. main() calls setScene and setProject in two different places
    // of the wiring, and nothing forces a particular order between the two; before
    // this fix there was a safety net (applyProjectSettings ran every frame and set it
    // anyway, though a frame late, which is exactly the bug above). Without
    // that net, if someone inverts the order in main.cpp or a new host calls
    // setScene after setProject, the root would silently stay empty
    // (the "caller's obligation" pattern that in this repo has already
    // cost several bugs, see caller_obligation_is_a_latent_bug.md). Each
    // setter covers the order in which IT arrives second.
    if (m_scene && project && project->valid())
        m_scene->setAssetRoot(project->root().string());
}

void EditorUI::setScene(Scene* scene)
{
    m_scene = scene;

    // See the comment of setProject, right above: this is the SAME
    // assignment, repeated on purpose to cover the inverse wiring
    // order (setScene called after setProject). If m_project
    // is not set yet (today's normal order: setScene runs before
    // setProject in main.cpp), this is a no-op and setProject takes care of it
    // when its turn comes.
    if (m_scene && m_project && m_project->valid())
        m_scene->setAssetRoot(m_project->root().string());
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

    // The handles arrive as opaque integers (UiLayer does not include vulkan.h so
    // that the editor can also be drawn with DirectX 12): here their real types are recovered,
    // which is where they are actually known.
    m_uiDevice = reinterpret_cast<VkDevice>(info.device);

    // Dedicated pool for ImGui (needs FREE_DESCRIPTOR_SET_BIT).
    // The new API (Sept 2025) uses separate SAMPLER + SAMPLED_IMAGE in AddTexture().
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

    // Without installing GLFW callbacks of its own: ImGui polls them in NewFrame
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
        // On DirectX 12 the texture already comes with its descriptor made: the backend
        // created it in the heap the interface shares, and that value IS what
        // ImGui::Image treats as an identifier. There is nothing to register.
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
        // Nothing to release: the descriptor belongs to the backend, not the interface.
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

    // InitForOther and not InitForVulkan: with DirectX 12 the GLFW backend does not
    // have to prepare anything of the graphics API, only the input.
    //
    // install_callbacks = true, unlike the Vulkan path: there main
    // installs them (it needs to intercept the cursor for mouse-look) and forwards
    // buttons, wheel, keys and characters to ImGui by hand. Here nobody
    // forwards them, and with false ImGui only receives the mouse POSITION (it
    // reads it on its own in NewFrame): not a single click arrives, so nothing can be
    // selected or tab switched. The callbacks that were already
    // set (the framebuffer size one) keep being called: ImGui
    // stores them and chains them.
    ImGui_ImplGlfw_InitForOther(info.window, true);

    // The descriptor range the backend reserved for the interface. Since
    // 1.92 its DX12 backend requests descriptors on its own (one per
    // texture, not just the font), so they have to be handed out with these two
    // callbacks instead of giving it a fixed one.
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
            // Running out of room here would be a silent failure that would end up
            // overwriting scene descriptors.
            if (pool->next >= pool->capacity)
                throw std::runtime_error("EditorUI: ImGui requested more descriptors than were reserved");
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

// Names of the modes saved in project.json. They are the SAME
// literals offered by the View menu combos (aaNames/fpNames): the setting
// is persisted by name, so reordering or inserting an option in the array
// does not change what is already saved.
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

// ok = false if the name is none of today's (file from a future
// version, or hand-edited): the caller falls back to the default and leaves it in the Log.
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

    // The backend does NOT come from the Renderer: it is the one the user chose for the
    // next startup, which may not be the one this process runs with.
    s.renderBackend = renderBackendName(m_selectedBackend);
    s.skyboxFolder  = m_skyboxFolder;

    // The volumes come from the AudioManager, which is the source of truth (FMOD
    // stores them in the ChannelGroups). Without audio the struct's neutral ones stay,
    // so opening the editor on a mute machine does not write zeros into
    // anyone's project.json.
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

    // Physics layers: the source of truth is the PhysicsManager. Without it
    // (headless tests, startup before creating it) the ViewSettings defaults
    // stay, which are the same as the manager's.
    static_assert(ProjectContext::ViewSettings::LayerCount == PhysicsManager::kLayerCount,
                  "project.json and the PhysicsManager must have the same number of layers");
    if (m_physics)
    {
        s.layerActive = m_physics->layerCount();
        for (int i = 0; i < PhysicsManager::kLayerCount; ++i)
        {
            s.layerNames[static_cast<size_t>(i)] = m_physics->getLayerName(i);
            s.layerMasks[static_cast<size_t>(i)] = m_physics->layerMask(i);
        }
    }

    // Saving is a COMPLETE snapshot: when writing, every panel of the table ends up
    // with data. "No data" only exists when reading a project.json that did not
    // carry it (one from before that panel was persisted).
    const auto punteros = panelOpenPtrs();
    for (int i = 0; i < ProjectContext::ViewSettings::PanelCount; ++i)
        s.panelOpen[i] = (punteros[static_cast<size_t>(i)] != nullptr) &&
                         *punteros[static_cast<size_t>(i)];
    return s;
}

std::array<bool*, ProjectContext::ViewSettings::PanelCount> EditorUI::panelOpenPtrs()
{
    using VS = ProjectContext::ViewSettings;
    std::array<bool*, VS::PanelCount> t{};
    t[VS::PanelScene]          = m_scenePanel.GetOpenPtr();
    t[VS::PanelViewport]       = m_viewportPanel.GetOpenPtr();
    t[VS::PanelProperties]     = m_propertiesPanel.GetOpenPtr();
    t[VS::PanelLog]            = m_logPanel.GetOpenPtr();
    t[VS::PanelContentBrowser] = m_contentBrowserPanel.GetOpenPtr();
    t[VS::PanelScriptEditor]   = m_scriptEditor ? m_scriptEditor->GetOpenPtr() : nullptr;
    t[VS::PanelAnimator]       = m_animatorPanel.GetOpenPtr();
    t[VS::PanelPerformance]    = m_performancePanel.GetOpenPtr();
    t[VS::PanelRendering]      = m_renderingPanel.GetOpenPtr();
    t[VS::PanelInputActions]   = m_inputActionsPanel.GetOpenPtr();
    return t;
}

void EditorUI::applyProjectSettings()
{
    if (m_project == m_appliedProject)
        return;
    // The Renderer is needed to apply: without it the next frame retries
    // instead of considering the project applied.
    if (!m_renderer)
        return;

    m_appliedProject = m_project;
    if (!m_project || !m_project->valid())
        return; // headless tests / startup before the selector: as always.

    // The base is the Renderer's values from NOW: every parameter that
    // project.json does not carry keeps the Renderer's default. The enables
    // do not: readSettings forces them off when they are missing.
    const ProjectContext::ViewSettings s =
        ProjectContext::readSettings(m_project->root(), currentSettings());

    if (s.loadFailed)
        m_logPanel.push("Unreadable project settings: effects open turned off");

    // Audio first: it does not depend on the Renderer, and putting it here makes clear that it
    // shares the same application moment as the rest of the settings.
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
    // The saved sample count may not exist on THIS GPU (project
    // brought from another machine): it is clamped to what the device supports.
    const int maxSamples = m_renderer->maxMsaaSamples();
    m_renderer->setMsaaSamples(std::clamp(s.msaaSamples, 1, maxSamples > 0 ? maxSamples : 1));
    m_renderer->setTaaFeedback(s.taaFeedback);
    m_renderer->setTaaJitterScale(s.taaJitterScale);

    // The modes, last: changing them recreates targets, and this way it is done only once
    // with the parameters already set.
    bool aaOk = true;
    const EditorRenderer::AaMode aa = aaModeFromName(s.aaMode, aaOk);
    if (!aaOk)
        m_logPanel.push("Unknown anti-aliasing mode in the project ('" + s.aaMode + "'): se usa None");
    m_renderer->setAaMode(aa);

    bool fpOk = true;
    const EditorRenderer::FpMode fp = fpModeFromName(s.fpMode, fpOk);
    if (!fpOk)
        m_logPanel.push("Unknown Forward+ mode in the project ('" + s.fpMode + "'): se usa Off");
    m_renderer->setForwardPlusMode(fp);
    m_renderer->setForwardPlusLightRadius(s.fpLightRadius);
    m_renderer->setShadowDistance(s.shadowDistance);
    m_renderer->setCascadeLambda(s.cascadeLambda);
    m_renderer->setShadowResolution(s.shadowResolution);
    m_renderer->setPresentMode(static_cast<PresentMode>(s.presentMode));

    // Render backend: it is READ but not applied. This process's device is already
    // created (the project selector is drawn on top of it), so the only thing
    // that can be done is leave it chosen for the next startup and warn.
    bool backendOk = true;
    // The project's sky, BEFORE touching the backend: initSkybox reconvolves
    // the global IBL, which is what the scene's ambient feeds on.
    std::snprintf(m_skyboxFolder, sizeof(m_skyboxFolder), "%s", s.skyboxFolder.c_str());
    if (m_renderer)
        m_renderer->initSkybox(s.skyboxFaces());

    m_selectedBackend = renderBackendFromName(s.renderBackend, backendOk);
    if (!backendOk)
        m_logPanel.push("Unknown render backend in the project ('" + s.renderBackend +
                        "'): se usa Vulkan");
    if (m_selectedBackend != m_activeBackend)
        m_logPanel.push(std::string("This project asks for the ") +
                        renderBackendName(m_selectedBackend) + " backend and the editor is running with " +
                        renderBackendName(m_activeBackend) + ": restart to apply it");

    // Physics layers: the names as they are, and the matrix walking only the
    // UPPER half (b >= a). setLayerCollision already writes both halves, so
    // a project.json with an asymmetric matrix (hand-edited) ends up
    // symmetric instead of fighting itself cell by cell.
    if (m_physics)
    {
        // How many layers there are: it is emptied down to 0 and recreated, so the manager ends up
        // with the project's and not those of the previous project.
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

    // Panel visibility: project.json overrides imgui.ini on WHICH panels
    // are open; the layout (docking, sizes) is still kept by imgui.ini.
    // A panel without saved data stays as it is, which is not the same as
    // closed: a project.json from before that panel was persisted must not
    // close it.
    const auto punteros = panelOpenPtrs();
    for (int i = 0; i < ProjectContext::ViewSettings::PanelCount; ++i)
    {
        bool* open = punteros[static_cast<size_t>(i)];
        if (open && s.panelOpen[i].has_value())
            *open = *s.panelOpen[i];

        // An unfilled gap in the table is EXACTLY the failure that cost
        // this: the Rendering panel existed, was drawn, stored its position in
        // imgui.ini, and started closed because nobody had tied it to its
        // index. A nullptr here does nothing visible (the panel simply
        // is not persisted), so it is said.
        //
        // There is no test that catches it: the table lives in EditorUI, which cannot be
        // built without a window or a Renderer. This warning is what there is, and it comes out on
        // opening any project. The only legitimate nullptr is the Script
        // Editor's, which does not exist until there is a project.
        if (!open && i != ProjectContext::ViewSettings::PanelScriptEditor)
            m_logPanel.push("EditorUI: el panel " + std::to_string(i) +
                            " is not in panelOpenPtrs(); its visibility is not saved.");
    }
}

void EditorUI::applySkyboxFolder(const std::string& folder)
{
    if (folder.empty() || !m_renderer)
        return;

    // Relative to the project if it falls inside: it is what is persisted and what the
    // exporter knows how to resolve. A folder from outside is saved as is, and
    // then the export will not find it; a warning goes to the Log.
    std::string guardada = folder;
    if (m_project && m_project->valid())
    {
        std::error_code ec;
        const std::filesystem::path rel =
            std::filesystem::relative(std::filesystem::path(folder), m_project->root(), ec);
        if (!ec && !rel.empty() && *rel.begin() != "..")
            guardada = rel.generic_string();
        else
            m_logPanel.push("Skybox: '" + folder +
                            "' is outside the project; the exported game will not find it.");
    }

    std::snprintf(m_skyboxFolder, sizeof(m_skyboxFolder), "%s", guardada.c_str());

    ProjectContext::ViewSettings tmp;
    tmp.skyboxFolder = guardada;
    m_renderer->initSkybox(tmp.skyboxFaces());
    saveProjectSettings();
    m_logPanel.push("Skybox reloaded from '" + guardada + "'");
}

void EditorUI::drawEnvironmentWindow()
{
    if (!m_environmentWindowOpen)
        return;

    ImGui::SetNextWindowSize(ImVec2(460.0f, 200.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Environment", &m_environmentWindowOpen))
    {
        ImGui::TextWrapped(
            "Sky folder. The six faces are expected inside with these names: "
            "px, nx, py, ny, pz and nz (.png). Changing it also recomputes the "
            "ambient lighting, which comes from convolving this same cubemap.");
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
            // filters = nullptr -> IGFD selects a folder, same as the export.
            m_skyboxDialog->OpenDialog("SkyboxDlg", "Skybox folder", nullptr, cfg);
            m_skyboxDlgOpen = true;
        }
        ImGui::SameLine();
        // Reload without changing folder: useful after overwriting the images.
        if (ImGui::Button("Reload"))
            applySkyboxFolder(m_skyboxFolder);

        // Drag zone. It accepts DT_ASSET_DIR, which is the payload the Content
        // Browser sets ONLY on folders: this way no file lands here.
        ImGui::BeginChild("##SkyboxDrop", ImVec2(0, 44), true);
        ImGui::TextDisabled("...or drag a folder here from the Content Browser");
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
        return; // without an open project this does not run: behavior as before.

    if (!ProjectContext::writeSettings(m_project->root(), currentSettings()))
        m_logPanel.push("Could not save the settings to project.json");
}

void EditorUI::draw(uint64_t viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView)
{
    // Project selector: first state of the loop. It takes the whole frame
    // (no menu, toolbar, dockspace or panels) until the callback
    // returns true; then it is released and the next frame is already the usual
    // editor. Same window, same device and same ImGui session.
    if (m_projectSelector)
    {
        if (m_projectSelector())
            m_projectSelector = nullptr;
        return;
    }

    // Audio load failures that FMOD has detected since the previous frame.
    // They are drained here, outside Play: a broken clip is seen when dropped, not when
    // pressing Play. With FMOD_NONBLOCKING the error does not exist yet when
    // createSound returns, so this per-frame pump is the ONLY place where
    // the failure can be observed. Each sound is reported only once.
    if (m_audio)
    {
        m_audioFailures.clear();
        m_audio->pollLoadFailures(m_audioFailures);
        for (const auto& path : m_audioFailures)
            m_logPanel.push("Could not load the audio '" + path +
                             "': missing file, unsupported format or corrupt data");
        if (!m_audioOutputWarned && !m_audio->outputWarning().empty())
        {
            m_logPanel.push(m_audio->outputWarning());
            m_audioOutputWarned = true;
        }
    }

    // View menu settings of the open project: they are dumped to the Renderer and to the
    // panel visibility on the first frame after choosing a project. No-op on the
    // rest of the frames and without a project.
    applyProjectSettings();

    // Drain of the DonTopo.loadScene mailbox: here, at the start of the UI
    // frame, the script tick has already been left (ScriptManager::update runs earlier in
    // main's loop), so loading does not destroy the GameObject that requested the
    // load. Same path as the File menu's Load Scene.
    if (std::string luaScenePath; ScriptBindings::takePendingSceneLoad(luaScenePath))
    {
        if (!m_isPlaying)
            m_logPanel.push("DonTopo.loadScene ignored: it only works in Play Mode");
        else
        {
            loadSceneFile(luaScenePath);
            // The old scene died: Lua's alive set held its pointers and
            // the new GameObjects may reuse those addresses.
            if (m_scriptManager) m_scriptManager->rebuildAliveSet();
        }
    }

    handleUndoRedoShortcut();
    handleGizmoModeShortcut();
    drawMenuBar();
    drawToolbar();
    drawDockSpace();
    // After the dockspace: a window drawn earlier cannot be docked.
    drawCollisionLayersWindow(m_physics, [this] { saveProjectSettings(); });
    drawEnvironmentWindow();

    // Single Ctx, built once per frame and shared by reference
    // with all the panels (pattern set here for the following tasks).
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
        m_onAxisSelected,
        [this](const std::filesystem::path& p) {
            // The registered .lua files live in the Scripts/ folder that ScriptManager
            // watches (the repo's, shared like the engine's assets):
            // those are still opened the same way. What is rejected is a .lua from ANOTHER
            // project; for that, contains() of an ad-hoc context over the
            // scripts folder works, with the same fail-closed behavior.
            const bool engineScripts =
                m_scriptManager &&
                ProjectContext(m_scriptManager->scriptsDirPath()).contains(p);
            if (!engineScripts && !projectAllows(p, "Script"))
                return;
            m_scriptEditor->openFile(p);
        },
        [this]() { m_animatorPanel.open(); },
        // Deferred: whoever requests it does so WHILE this ctx is being built, and the
        // panel needs the ctx (the renderer) to open the image.
        [this](const std::string& atlasPath) { m_pendingSpriteAtlas = atlasPath; },
        [this]() { m_propertiesPanel.invalidateSpriteNames(); },
        // Save the project settings: requested by RenderingPanel on every
        // control, and also by each one's undo command.
        [this] { saveProjectSettings(); },
        [this] { m_environmentWindowOpen = true; },
        m_assetLoader,
        m_project,                 // sandbox of the open project's paths
        m_loadingModal.active(),   // vetoes editing while the modal loads
        [this](const std::filesystem::path& p) {
            // The Play Mode guard lives here too, not only in the panel:
            // same reason as in drawSceneDialog: whoever actually loads is
            // the one that has to refuse.
            if (m_isPlaying) return;
            loadSceneFile(p.string());
        },
        [this](const std::filesystem::path& thenLoad) {
            if (m_isPlaying) return;
            if (m_currentScenePath.empty())
            {
                // Never-saved scene: same Save Scene dialog of the File
                // menu, with the load chained to its confirmation.
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
            if (!projectAllows(m_currentScenePath, "Scene")) return;
            bool saved = m_scene && m_scene->save(m_currentScenePath);
            if (saved) m_undoHistory.markSceneSaved();
            m_sceneIOError = saved ? "" : "Could not save the scene";
            m_logPanel.push(saved ? ("Scene saved: " + m_currentScenePath)
                                  : ("Error saving scene: " + m_currentScenePath));
            if (saved && !thenLoad.empty())
                loadSceneFile(thenLoad.string());
        },
        m_droppedFilesProvider,
        m_jobSystem,
    };

    m_scenePanel.draw(ctx, sceneRoot);
    // ScenePanel deleted the selected GameObject: invalidates the Properties edit
    // caches so that they do not keep dangling pointers
    // (GameObject / BoxCollider already freed) until the next real selection.
    if (m_scenePanel.selectionWasDeletedThisFrame())
        m_propertiesPanel.invalidateCaches();
    m_viewportPanel.draw(ctx, viewportTexture, cameraView);
    // The render goes at the EXACT size of the panel's image area. Without this it would
    // render at the window's and ImGui would rescale when drawing: that filtering
    // eats the stair-stepping (the anti-aliasing modes stop being distinguishable) and
    // deforms the scene if the panel's aspect does not match the window's.
    // The Renderer ignores null sizes and only recreates when it really changes.
    if (m_renderer)
        m_renderer->setViewportSize(m_viewportPanel.contentWidth(), m_viewportPanel.contentHeight());
    m_propertiesPanel.draw(ctx);
    m_logPanel.draw();
    drawSceneDialog();
    drawExportDialog();
    m_contentBrowserPanel.draw(ctx, sceneRoot);
    m_scriptEditor->draw();
    m_animatorPanel.draw(ctx);
    // The request to open the sprite editor is served HERE, with the ctx already
    // built; the panel needs the renderer to load the image.
    if (!m_pendingSpriteAtlas.empty())
    {
        m_spriteEditor.open(ctx, m_pendingSpriteAtlas);
        m_pendingSpriteAtlas.clear();
    }
    m_spriteEditor.draw(ctx);
    // Always, even closed: its draw() is what turns off the Renderer's metrics capture
    // when the panel stops being visible.
    m_performancePanel.draw(ctx);
    // Both backends go as parameters: the panel draws them and writes the
    // selected one, but the owner is still this class, which is what
    // serializes them in project.json.
    m_renderingPanel.draw(ctx, m_activeBackend, m_selectedBackend);
    m_inputActionsPanel.draw();

    // Progress overlay: updated with what is still left to pump and
    // drawn on top. draw() returns true only on the frame Cancel is pressed
    // -> the live requests are cancelled and the mailbox is emptied; the
    // scene stays with what was already applied (valid, saveable state).
    m_loadingModal.update(m_assetLoader ? m_assetLoader->pending() : 0);
    if (m_loadingModal.draw() && m_assetLoader)
        m_assetLoader->cancelAllPending();
}

void EditorUI::onAssetsLoaded(std::vector<LoadedMesh> results, Scene& scene, EditorRenderer& renderer)
{
    for (auto& r : results)
    {
        // Adding a Mesh enters the undo HERE and not on pressing the button: the
        // load is asynchronous, and until applyLoadedMesh does the setMesh there is no
        // mesh to store. Only the loads the user requested from Properties
        // (the scene load goes through this same pump), and it is queried even if
        // it failed, so that it does not stay noted.
        const bool delUsuario = m_propertiesPanel.consumeUserMeshJob(r.targetId, r.job);

        // Several pieces in a static model: the selected one becomes the parent
        // and each piece a child. A single undo step, with the meshes already loaded in
        // each command so that redoing does not read the file.
        if (delUsuario && r.error.empty() && r.pieces.size() > 1)
        {
            if (GameObject* parent = scene.findById(r.targetId))
            {
                parent->pendingMeshJob = 0;
                std::vector<std::string> warnings;
                const std::vector<GameObject*> kids = insertModelPieces(
                    scene, parent, r.path, r.pieces, r.pieceMeshes, *m_physics, *m_audio, &warnings);
                for (const std::string& w : warnings) m_logPanel.push(w);
                // No piece got in (all out of range or without a mesh): the parent
                // stays without a mesh and without new children. Without this warning, the user
                // sees that "Add Mesh" did nothing and does not know why.
                if (kids.empty())
                    m_logPanel.push("'" + std::filesystem::path(r.path).stem().string() +
                                     "': no piece could be added to '" + parent->name + "'");
                auto group = std::make_unique<CompositeCommand>(
                    "Add model '" + std::filesystem::path(r.path).stem().string() + "' a '" + parent->name + "'");
                for (GameObject* kid : kids)
                {
                    renderer.registerGameObject(kid);
                    // Key by the MESH's sourcePath, not by r.path: it is what
                    // subtreeToJson actually serializes (mesh->sourcePath, see
                    // Scene::nodeToJson) and what CreateGameObjectCommand's redo
                    // looks up in the cache when re-reading that JSON. They coincide in practice,
                    // but depending on r.path here would be an implicit coincidence,
                    // not a contract.
                    PreloadedMeshCache cache;
                    cache[meshCacheKey(kid->getMesh()->sourcePath, kid->getMesh()->piece)] = kid->getMesh();
                    const size_t index = static_cast<size_t>(
                        std::find_if(parent->children.begin(), parent->children.end(),
                                     [&](const auto& c) { return c.get() == kid; }) - parent->children.begin());
                    group->add(std::make_unique<CreateGameObjectCommand>(
                        scene, *m_physics, *m_audio, renderer, group->label(), parent->id, index,
                        scene.subtreeToJson(kid), std::move(cache)));
                }
                renderer.flushUploadsAndWait();
                if (!group->empty()) m_undoHistory.push(std::move(group));   // without execute: they are already created
                m_propertiesPanel.invalidateCaches();
            }
            continue;
        }

        std::string              err;
        std::vector<std::string> avisos;
        // The warnings go to the Log whatever happened with the load: they describe
        // overrides that are not going to be applied, and that holds the same if the GPU
        // registration ended up failing afterwards.
        const bool ok = applyLoadedMesh(r, scene, renderer, &err, &avisos);
        for (const std::string& aviso : avisos)
            m_logPanel.push(aviso);
        if (!ok && !err.empty())
            m_logPanel.push(err);

        // It is pushed WITHOUT execute(): the setMesh is already done.
        if (ok && delUsuario)
        {
            if (GameObject* go = scene.findById(r.targetId))
                m_undoHistory.push(std::make_unique<MeshComponentCommand>(
                    scene, &renderer, "Add Mesh to '" + go->name + "'", *go, /*add=*/true));
        }
    }

    // A single submit for all the uploads of this pump. It is what turns
    // ~440 vkQueueWaitIdle (one per object) into one.
    renderer.flushPendingUploads();

    // The meshes of an async load arrive AFTER reloadSceneFromJson, so
    // the camera range that was recomputed there did not include them: it is redone with
    // the scene already complete. Without this, a big scene loaded from disk would be
    // drawn with the near/far of whatever was there before.
    renderer.refitCameraRange();
}

void EditorUI::onGameObjectDestroyed(GameObject* node)
{
    if (!node || !m_selected) return;
    bool selectionInSubtree = false;
    node->traverse([&](GameObject* n) { if (n == m_selected) selectionInSubtree = true; });
    if (selectionInSubtree)
    {
        m_selected = nullptr;               // the object is going to be freed: do not leave a dangling pointer
        m_propertiesPanel.invalidateCaches(); // the edit caches pointed to components already freed
    }
}

void EditorUI::handleGizmoModeShortcut()
{
    ImGuiIO& io = ImGui::GetIO();
    // Renaming a GameObject in the Hierarchy, or typing in the Script Editor,
    // cannot change the gizmo mode: W is a letter before it is a shortcut.
    if (io.WantTextInput || ImGui::IsAnyItemActive())
        return;
    // Ctrl+W / Ctrl+R are something else (or nothing) in any editor: they are not eaten
    // here. And with the right button held, W/E are the fly camera; it is
    // exactly the split that makes it possible to reuse Unity's keys.
    if (io.KeyCtrl || io.KeyAlt || ImGui::IsMouseDown(ImGuiMouseButton_Right))
        return;

    if (ImGui::IsKeyPressed(ImGuiKey_W))
        m_viewportPanel.setGizmoMode(GizmoMode::Translate);
    else if (ImGui::IsKeyPressed(ImGuiKey_E))
        m_viewportPanel.setGizmoMode(GizmoMode::Rotate);
    else if (ImGui::IsKeyPressed(ImGuiKey_R))
        m_viewportPanel.setGizmoMode(GizmoMode::Scale);
}

void EditorUI::handleUndoRedoShortcut()
{
    // In Play it is ALSO undone (A11). What is undone there is only what was done
    // DURING Play: the history is emptied on pressing Play, and Stop restores the
    // scene from the snapshot and empties it again. So this cannot
    // leave a permanent strange state, and it adds no new capability:
    // creating, deleting and editing objects or the Animator graph is already allowed with
    // the scene running, without going through here.
    //
    // It is also what makes it useful that AnimatorGraphCommand keeps the parameter values
    // and the playhead when applying a graph: that guarantee was
    // written "because it runs in Play" and with the gate it was unreachable.
    if (!m_scene || !ImGui::GetIO().KeyCtrl || ImGui::GetIO().WantTextInput)
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
    if (ImGui::IsKeyPressed(ImGuiKey_D))
        duplicateSelection();
}

void EditorUI::duplicateSelection()
{
    // The scene root (parent == nullptr) is not duplicated: it is the same gate
    // ScenePanel uses for Del/F2. cloneGameObject also rejects it, but
    // this way it is not reached with a selection that has no possible siblings.
    if (!m_selected || !m_selected->parent) return;
    if (!m_scene || !m_physics || !m_audio || !m_renderer) return;

    GameObject* parent = m_selected->parent;
    // The parent decision (sibling, not child) lives in duplicateAsSibling so that
    // it can be tested without a GUI; the copying is still Scene::cloneGameObject.
    GameObject* clone = duplicateAsSibling(*m_scene, m_selected, *m_physics, *m_audio);
    if (!clone)
    {
        m_logPanel.push("Could not duplicate '" + m_selected->name + "'");
        return;
    }

    // cloneGameObject leaves the render indices at -1; this registers them.
    // Synchronous flush for the same reason as CreateGameObjectCommand::execute:
    // without it the duplicate would be invisible for ~2 frames.
    m_renderer->registerGameObject(clone);
    m_renderer->flushUploadsAndWait();

    // The warnings cloneGameObject left in the scene (e.g. the discard of the
    // CameraComponent) are known to Core but not to the Log Console: whoever does
    // know it dumps them here, like the scene load.
    for (const auto& w : m_scene->lastWarnings())
        m_logPanel.push(w);

    // The clone ALREADY exists, so the command is not executed: push() never calls
    // execute(). Same pattern as ScenePanel::createBasicShape: the snapshot is
    // taken with the object already assembled, and undo()/redo() delete it and
    // rebuild it by id from that JSON. That is why a new command is
    // not needed: CreateGameObjectCommand does not assume the object is empty.
    const size_t index = parent->children.size() - 1;
    nlohmann::json snapshot = m_scene->subtreeToJson(clone);
    m_undoHistory.push(std::make_unique<CreateGameObjectCommand>(
        *m_scene, *m_physics, *m_audio, *m_renderer,
        "Duplicate '" + m_selected->name + "'", parent->id, index, std::move(snapshot)));

    m_selected = clone;
    m_propertiesPanel.invalidateCaches();
    m_logPanel.push("GameObject '" + clone->name + "' duplicated");
}

// ── Render settings with undo (P8/H49) ──────────────────────────────────────
//
// The four wrappers read the value BEFORE drawing and that is the one that ends up
// in the command: `SliderFloat` jumps to the click's value in the same frame in
// which it is pressed, so reading it afterwards would return the jump's destination and not
// where it came from. The four menu Combos read afterwards on purpose and that is why they do not
// go through here: their previous value is the one the Combo itself was showing.
//
// The drag is detected by EDGE (the item becomes active and its ID is not the one
// we already had) instead of by a bare `IsItemActivated()`: `ColorEdit3` is a
// group of sub-widgets and its activation flag does not always rise to the group,
// whereas `IsItemActive()` works in both cases.
void EditorUI::drawMenuBar()
{
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            // Outside Play Mode for the same reason as Save/Load: the package
            // is built from the IN-MEMORY scene, so exporting during
            // Play would package the simulation state instead of the author's scene,
            // and the exported game would start mid-play.
            if (ImGui::MenuItem("Export Game...", nullptr, false, m_scene != nullptr && !m_isPlaying))
            {
                IGFD::FileDialogConfig cfg;
                cfg.path  = (m_project && m_project->valid()) ? m_project->root().string() : std::string(".");
                cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_HideColumnDate |
                            ImGuiFileDialogFlags_DisableThumbnailMode |
                            ImGuiFileDialogFlags_DisablePlaceMode;
                // filters = nullptr -> IGFD selects a folder, not a file.
                m_exportDialog->OpenDialog("ExportDlg", "Export destination folder", nullptr, cfg);
                m_exportDlgOpen = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            // The panels' visibility is also a project setting: the
            // MenuItem is a checkbox, so it is saved in the same frame as the
            // click. The LAYOUT (docking, sizes) is still kept by imgui.ini.
            bool panelToggled = false;
            panelToggled |= ImGui::MenuItem("Scene", nullptr, m_scenePanel.GetOpenPtr());
            panelToggled |= ImGui::MenuItem("Viewport", nullptr, m_viewportPanel.GetOpenPtr());
            panelToggled |= ImGui::MenuItem("Properties", nullptr, m_propertiesPanel.GetOpenPtr());
            panelToggled |= ImGui::MenuItem("Log", nullptr, m_logPanel.GetOpenPtr());
            panelToggled |= ImGui::MenuItem("Content Browser", nullptr, m_contentBrowserPanel.GetOpenPtr());
            panelToggled |= ImGui::MenuItem("Script Editor", nullptr, m_scriptEditor->GetOpenPtr());
            panelToggled |= ImGui::MenuItem("Animator", nullptr, m_animatorPanel.GetOpenPtr());
            // Not persisted in the project settings on purpose: it is opened
            // to slice a specific atlas and closed, it is not one of the panels
            // one wants to find open at startup.
            ImGui::MenuItem("Sprite Editor", nullptr, m_spriteEditor.GetOpenPtr());
            // Same reason as the Sprite Editor: it is opened to touch the matrix and
            // closed, it is not a panel one wants open at startup.
            ImGui::MenuItem("Collision Layers", nullptr, &g_showLayerMatrix);
            panelToggled |= ImGui::MenuItem("Performance", nullptr, m_performancePanel.GetOpenPtr());
            panelToggled |= ImGui::MenuItem("Rendering", nullptr, m_renderingPanel.GetOpenPtr());
            panelToggled |= ImGui::MenuItem("Input Actions", nullptr, m_inputActionsPanel.GetOpenPtr());
            if (panelToggled)
                saveProjectSettings();
            ImGui::Separator();

            // Per-bus volume. Here and not in a panel of its own: they are three sliders
            // touched once per project, not something one wants taking up
            // room in the dock. They are saved in project.json (unlike the session
            // settings further down), because it is the knob the player expects to persist.
            //
            // The value is read from the AudioManager every frame, not from a copy:
            // this way a SetMasterVolume from Lua is reflected here instead of
            // leaving the UI lying.
            if (m_audio)
            {
                // Without an audio device the sliders are still drawn but
                // disabled: hiding them would make one think the feature does not
                // exist. It is explained in the tooltip.
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
                    // On release, not on every pixel of the drag: writing
                    // project.json per frame would be one file per millisecond.
                    if (ImGui::IsItemDeactivatedAfterEdit())
                        saveProjectSettings();
                }
                ImGui::EndDisabled();
                if (!m_audio->available() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("No audio device on this machine: the editor "
                                      "starts muted and these controls have no effect");
                ImGui::Separator();
            }
            // Weight of the IBL ambient. Session setting: it is not serialized in the
            // scene, so on reopening the editor it goes back to 1.0.
            // The render settings and the backend selector live in the Rendering
            // panel since H58: a menu closes when the mouse is released, and
            // tuning bloom or fog while watching the viewport forced reopening it on
            // every tweak.
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

}

void EditorUI::drawToolbar()
{
    // vp->WorkPos/WorkSize (not vp->Pos/vp->Size) because BeginMainMenuBar
    // reserves its strip by subtracting from the main viewport's WorkPos/WorkSize
    // so the Toolbar sits right below the MenuBar instead of overlapping it.
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
            // Synchronous restore (async=false): no modal, deterministic. Introducing
            // half-finished states in the Play->Stop transition is not worth it.
            m_sceneIOError = reloadSceneFromJson(m_playSnapshot, /*async=*/false) ? "" : "Could not restore the scene";
            m_isPlaying = false;
            m_logPanel.push("Play Mode stopped");
        }
        ImGui::PopStyleColor();
    }
    else
    {
        if (ImGui::Button("Play"))
        {
            m_playSnapshot = m_scene->toJson();
            m_undoHistory.clear();
            // Warning only once when Play starts (not every frame: the Renderer
            // queries findCamera() in all of them, and logging there would flood the
            // console). Without a camera, Play starts anyway with the editor's, since
            // being able to iterate without a camera matters more than forcing discipline.
            if (!m_scene->findCamera())
                m_logPanel.push("No camera in the scene; using the editor's");
            m_isPlaying = true;
            // Animators start Play from their entry state, with the
            // clock at zero and the parameters clean.
            //
            // In Edit Mode the clock DOES run (only the transitions are skipped,
            // see AnimatorComponent::update), so an entry state with
            // loop=false reaches its end while the user edits and leaves
            // finished as true. Without this reset, Play would start with that flag already
            // set and an "animation finished" transition would fire on the
            // first frame: the entry animation would never be seen.
            // With loop=true it is not noticeable, because a looping clip does not end.
            //
            // Stop does not need the symmetric: it rebuilds the scene from the
            // JSON snapshot and bindClips already ends in reset().
            m_scene->traverse([](GameObject* go) {
                if (go->hasAnimator()) go->getAnimator()->reset();
            });
            // A Save/Load or export dialog open when Play starts is left
            // orphaned: the operation would no longer run, but the
            // dialog would stay on screen until Stop. Both are IGFD
            // and do not block the toolbar, so we get here with them
            // open; the export's modal popups do block and there is no
            // need to touch them.
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
            // Without an Audio Listener in the scene (or with its own disabled) the
            // clips sound ANYWAY: 3D audio is then heard from the camera,
            // the fallback that the three host paths already resolve every frame
            // (sandbox/src/main.cpp, runtime/main.cpp). Here there used to be a
            // gate that skipped this sweep, but it only covered playOnAwake:
            // neither Lua's AudioClip:Play (ScriptBindings.cpp) nor the inspector's
            // Play button (PropertiesPanel.cpp) consulted it, so the
            // warning lied (the clip was heard) and the "invariant" held for one
            // of the four playback paths. Enforcing it for real would require
            // repeating it in all four, because it cannot live inside
            // AudioManager/AudioClipComponent (those two are tested without a scene).
            // The warning stays, now informative and true. One per Play, not
            // one per clip.
            GameObject* listenerGo = m_scene->findAudioListener();
            const bool listenerActive = listenerGo && listenerGo->getAudioListener()->getEnabled();
            if (!listenerActive)
                m_logPanel.push("No Audio Listener in the scene: 3D audio is heard from the camera");
            m_scene->traverse([](GameObject* go) {
                if (go->hasAudioClip() && go->getAudioClip()->getPlayOnAwake())
                    go->getAudioClip()->play(glm::vec3(go->worldTransform[3]));
            });
            m_logPanel.push("Play Mode started");
        }
    }
    ImGui::EndDisabled();

    // Viewport gizmo mode. Three exclusive buttons, the active one with the
    // ImGuiCol_ButtonActive color: the same idiom already used by Stop and
    // Wireframe right next to it, so that "pressed" reads the same across the whole bar.
    //
    // The state lives in ViewportPanel (whoever reads it is its manipulator); here
    // it is only drawn and written. The W/E/R shortcuts do the same from
    // handleGizmoModeShortcut.
    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    const struct { GizmoMode mode; const char* label; const char* tip; } kGizmoBtns[] = {
        { GizmoMode::Translate, "Move",   "Move (W)"   },
        { GizmoMode::Rotate,    "Rotate", "Rotate (E)"   },
        { GizmoMode::Scale,     "Scale",  "Scale (R)" },
    };
    for (const auto& b : kGizmoBtns)
    {
        ImGui::SameLine();
        const bool activo = m_viewportPanel.gizmoMode() == b.mode;
        if (activo)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(b.label))
            m_viewportPanel.setGizmoMode(b.mode);
        if (activo)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", b.tip);
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("|");

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

    // Save/Load are left out of Play Mode: what is in memory during Play
    // is simulation state (positions moved by physics, values that
    // scripts mutated), not the scene the user is creating.
    // Saving it would make it permanent without anyone noticing (a volume at 0 or an
    // accumulated rotation is not visible anywhere) and loading another scene
    // would leave m_playSnapshot describing a scene that no longer exists, so
    // Stop would restore something foreign.
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
        ImGui::SetTooltip("Stop Play Mode to save or load scenes");

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
        ImGui::SetTooltip("Stop Play Mode to save or load scenes");

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
    // local ctx, not a persistent member: avoids ambiguous lifetime of the
    // references (same pattern as EditorContext in draw()).
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
        m_onAxisSelected,
    };
    m_viewportPanel.focusSelected(ctx, camera);
}

bool EditorUI::reloadSceneFromJson(const nlohmann::json& j, bool async)
{
    if (!m_scene || !m_renderer || !m_physics || !m_audio)
        return false;

    // Releases GPU resources of the current scene, and with them its indices: if
    // fromJson fails further down due to nested malformation, m_root is still
    // this same tree (Scene::fromJson is atomic), and with the indices at -1 the
    // re-registration traverse below registers it again just as if it were
    // the new tree. Without that, the old tree would be left with stale indices
    // and unregistered after a failure, leaving the viewport empty even though the
    // Scene data did not change.
    //
    // The reset is already done by removeGameObject in both backends (H14): here
    // it was repeated by hand because the Vulkan one did not do it.
    for (auto& child : m_scene->getRoot().children)
        m_renderer->removeGameObject(child.get());

    // Before starting an async Load Scene, cancel any load still in
    // flight from a previous operation: its results would resolve to targets already
    // deleted and would be discarded anyway, but leaving them alive would inflate the
    // pending() with which the modal below is opened. Only on the async path and
    // only if there is a loader.
    if (async && m_assetLoader)
        m_assetLoader->cancelAllPending();

    // Only Load Scene (async) goes asynchronous. The Play->Stop restore stays
    // synchronous on purpose: introducing half-finished states in that transition is not
    // worth the gain, which layer B already gives on its own. Without a loader (or in
    // synchronous) fromJson loads the meshes in place, as always.
    bool loaded = m_scene->fromJson(j, *m_physics, *m_audio,
                                    async ? m_assetLoader : nullptr);
    // It runs both if loaded is true (new tree, indices already at -1 by
    // construction) and if it is false (old tree intact, indices
    // reset right above); in both cases the meshes have to be
    // uploaded to the GPU again.
    m_renderer->registerGameObject(&m_scene->getRoot());

    // Synchronous path (Play->Stop restore): the meshes are registered via the
    // deferred batch, which without a flush does not become visible until ~2 frames later
    // (the old objects were already removed above => pop-in/flicker). It is uploaded and
    // waited on here so that the restored geometry is visible in this same
    // frame, just like before the asynchronous load. The async path does NOT go through
    // here: it keeps its modal + per-frame pump.
    if (!async)
        m_renderer->flushUploadsAndWait();

    if (loaded)
    {
        m_selected = nullptr; // the previous selection no longer exists
        // Load warnings (e.g. a scene with two cameras, where fromJson
        // keeps the first): Core does not know the Log Console, so whoever
        // does know it dumps them here. Only if loaded: a failed load
        // does not modify the scene and its warnings do not apply.
        for (const auto& w : m_scene->lastWarnings())
            m_logPanel.push(w);

        // In async, fromJson enqueued one request per sourcePath; open the
        // modal with that count. begin() does nothing if they are 0 (scene without
        // file meshes), so an empty modal does not appear.
        if (async && m_assetLoader)
            m_loadingModal.begin(m_assetLoader->pending());

        // The scene that was just assembled overrides the editor's near/far:
        // until here they were still those of the meshes passed to
        // Renderer::init at startup. On the async path this only sees the meshes
        // already present; onAssetsLoaded repeats it when the rest lands.
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
        m_logPanel.push("[Project] The project has no startup scene: " + scene.string());
        return false;
    }
    return loadSceneFile(scene.string());
}

bool EditorUI::projectAllows(const std::filesystem::path& path, const char* what)
{
    if (!m_project || !m_project->valid())
        return true; // without an open project there is no sandbox: as always.
    if (m_project->contains(path))
        return true;

    m_logPanel.push(std::string("[Project] ") + what + " outside the project, rejected: " +
                    path.string());
    return false;
}

bool EditorUI::loadSceneFile(const std::string& path)
{
    // Project sandbox: a scene from another project (or from outside the
    // workspace) is rejected here, which is where ALL loads pass:
    // File menu, double click in the Content Browser and Lua's DonTopo.loadScene.
    if (!projectAllows(path, "Scene"))
        return false;

    // Validates the basic JSON structure BEFORE touching GPU/Scene:
    // rejects a corrupt top-level file without touching anything (fast path, avoids
    // reloadSceneFromJson's GPU churn). It does not cover nested malformation;
    // for that, Scene::fromJson is atomic and reloadSceneFromJson covers
    // both outcomes.
    auto parsed = FileManager::readJson(path);
    bool structureOk = parsed.has_value() &&
                        parsed->contains("version") && (*parsed)["version"].is_number_integer() &&
                        (*parsed)["version"].get<int>() == 1 &&
                        parsed->contains("root") && (*parsed)["root"].is_object();

    bool loaded = structureOk && reloadSceneFromJson(*parsed, /*async=*/true);
    // markSceneSaved only here, not in reloadSceneFromJson: that function also
    // restores the Play->Stop snapshot, which returns the scene to the state
    // before Play (with its unsaved edits) and must not mark it clean.
    if (loaded)
    {
        m_undoHistory.markSceneSaved();
        m_currentScenePath = path;
    }
    m_sceneIOError = loaded ? "" : "Could not load the scene";
    m_logPanel.push(loaded ? ("Scene loaded: " + path) : ("Error loading scene: " + path));
    return loaded;
}

void EditorUI::drawSceneDialog()
{
    // Same reason as PropertiesPanel::drawMeshDialog/drawAudioClipDialog:
    // it runs every frame independently of m_sceneDlgOpen to drain
    // the dialog even if the user closes it without confirming.
    if (!m_sceneDlgOpen || !m_sceneFileDialog->Display("SceneDlg"))
        return;

    // The guard lives here, in the place that really writes and loads, not only
    // in the buttons: the IGFD dialog does not block the toolbar, so one
    // can open Save, press Play and confirm afterwards. The disabled button
    // communicates; this is what prevents.
    if (m_isPlaying)
    {
        m_sceneFileDialog->Close();
        m_sceneDlgOpen = false;
        m_pendingSceneLoadAfterSave.clear();
        m_logPanel.push("Scene operation cancelled: cannot save or load in Play Mode");
        return;
    }

    if (m_sceneFileDialog->IsOk())
    {
        std::string path = m_sceneFileDialog->GetFilePathName();

        if (m_sceneDlgIsSave)
        {
            // Same as in the load: the destination has to fall inside the
            // project. It is rejected before writing, so the file from
            // outside is neither created nor overwritten.
            if (!projectAllows(path, "Scene"))
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
            m_sceneIOError = saved ? "" : "Could not save the scene";
            m_logPanel.push(saved ? ("Scene saved: " + path) : ("Error saving scene: " + path));

            // This Save came from the Content Browser modal's "Save" on
            // a scene without a file: it chains here the load that was left
            // waiting. If the save failed nothing is loaded; losing the
            // changes is exactly what the modal exists to avoid.
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
    // Cancelling the dialog (or a failed save) discards the chained
    // load: the current scene keeps its unsaved changes.
    m_pendingSceneLoadAfterSave.clear();
}

void EditorUI::drawExportDialog()
{
    // It runs every frame because the two BeginPopupModal below (name and
    // confirmation) need to be submitted every frame for ImGui to
    // keep them open after the OpenPopup that triggers them; if this function
    // were not called, the popup would close by itself even if the user did not press
    // Cancel. (The Display("ExportDlg") is conditional on m_exportDlgOpen:
    // the && below short-circuits and does not evaluate it when the folder
    // dialog is closed.)
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
        ImGui::Text("Destination: %s", m_exportDestDir.c_str());
        ImGui::InputText("Name", m_exportNameBuffer, sizeof(m_exportNameBuffer));

        // pkg is what is really going to be created/deleted: it is computed and
        // shown here (not the raw name) so that the user evaluates the
        // real path, not a piece of text that might not match
        // it (see isValidExportGameName in GameExporter.cpp).
        const std::filesystem::path pkg =
            std::filesystem::path(m_exportDestDir) / m_exportNameBuffer;
        std::string nameError;
        const bool nameOk = isValidExportGameName(m_exportNameBuffer, nameError);

        // inspectExportTarget is only queried with a valid name: with an
        // invalid name pkg may not even represent a useful path
        // (stray separators, device name...) and there is nothing to
        // classify yet. Missing is an arbitrary filler value for
        // that case; it is never read because canExport already requires nameOk.
        const ExportTargetState targetState =
            nameOk ? inspectExportTarget(pkg) : ExportTargetState::Missing;
        // Occupied disables the button instead of asking for confirmation: if it
        // were allowed to be confirmed, writeExportPackage would abort anyway (it is
        // authoritative, GameExporter.h:103-107) but after the
        // user had already said "yes, delete" about something that in reality was never
        // going to be deleted: a confirmation that lies about what it does.
        const bool occupied  = nameOk && targetState == ExportTargetState::Occupied;
        const bool canExport = nameOk && !occupied;

        if (!nameOk)
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", nameError.c_str());
        else if (occupied)
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "'%s' already exists and has content that is not from a previous "
                               "export; choose another name or destination folder.",
                               pkg.string().c_str());
        else
            ImGui::Text("Package: %s", pkg.string().c_str());

        // Backend the game will start with. It need not be the editor's:
        // it is exported for the player's machine, not for this one. It is
        // stored in the package's game.cfg, not in project.json.
        ImGui::Separator();
        const char* exportBackendNames[] = { "Vulkan", "DirectX 12" };
        ImGui::SetNextItemWidth(140.0f);
        ImGui::Combo("Render backend", &m_exportBackend, exportBackendNames,
                     IM_ARRAYSIZE(exportBackendNames));
        if (m_exportBackend == (int)RenderBackend::D3D12)
            ImGui::TextDisabled("On a machine without DirectX 12 the game starts with\n"
                                "Vulkan and says so in game.log.");

        ImGui::BeginDisabled(!canExport);
        if (ImGui::Button("Export"))
        {
            // Missing/Empty: nothing to lose, it is exported directly. PriorPackage:
            // there is really a previous export there, it is confirmed before
            // deleting it (Occupied already disabled the button above).
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
        ImGui::OpenPopup("Overwrite export");
        m_openExportConfirmPopup = false;
    }

    if (ImGui::BeginPopupModal("Overwrite export", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        // Same resolved path as the previous popup, not the raw name: it is
        // literally what remove_all() is going to delete if the user
        // confirms, and the name by itself does not represent it (see review
        // finding: "The folder '..' already exists" does not say "I am going to delete
        // C:\Users\ruben").
        const std::filesystem::path pkg =
            std::filesystem::path(m_exportDestDir) / m_exportNameBuffer;
        // It is only reached with targetState == PriorPackage (see the Export
        // button above): pkg really exists and contains a game.scene,
        // so the "could not be checked" nuance this text used to carry
        // is not needed: Occupied (fs::status failure included) never
        // lets this popup open.
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                           "'%s' contains a previous export.", pkg.string().c_str());
        ImGui::Text("All of its content will be deleted before exporting.");
        if (ImGui::Button("Delete and export"))
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

    // exportGame() takes Scene& (not Scene*): the "is there an open
    // scene" check cannot live inside it and stays here, before
    // dereferencing m_scene.
    if (!m_scene)
    {
        m_logPanel.push("Export cancelled: no scene is open");
        return;
    }

    // Defense at the point that builds the package, not only in the menu. The
    // two popups of the flow (name and confirmation) are BeginPopupModal and do
    // block the toolbar, but the folder dialog is IGFD and does not: it can be
    // left open, press Play and carry on. And even if today no gap
    // were left, this is the function that has to be hardened: it is the one that reads the scene.
    if (m_isPlaying)
    {
        m_logPanel.push("Export cancelled: stop Play Mode before exporting");
        return;
    }

    // The package is written inside the open project: exporting over
    // another project's folder is rejected here, before creating or deleting anything.
    // The package FORMAT and what exportGame() does do not change.
    if (!projectAllows(fs::path(m_exportDestDir) / m_exportNameBuffer, "Export"))
        return;

    std::error_code ec;
    fs::path projectRoot = fs::current_path(ec);
    if (ec) projectRoot = ".";
    fs::path canon = fs::canonical(projectRoot, ec);
    if (!ec) projectRoot = canon;

    const fs::path runtimeExe = projectRoot / DT_RUNTIME_FILE_NAME;
    const fs::path scriptsDir = m_scriptManager ? m_scriptManager->scriptsDirPath()
                                                : projectRoot / "Scripts";

    std::map<std::string, fs::path> scriptPaths;
    if (m_scriptManager)
        for (const auto& [name, cls] : m_scriptManager->getRegistry())
            scriptPaths[name] = cls.path;

    // Warning, NOT a block: the package is assembled from what the scene references, and
    // that may fall outside the project. The repo's shared assets are
    // legitimate and travel as always; what is worth flagging is an asset of
    // ANOTHER project, which is a leak. It warns and exports anyway: leaving it
    // out of the package would give a game without that asset, which is worse.
    if (m_project && m_project->valid())
    {
        const ProjectContext workspace(ProjectContext::workspaceDir());
        for (const ExportAsset& a : collectSceneAssets(*m_scene, projectRoot, scriptPaths))
        {
            const fs::path src(a.sourcePath);
            if (m_project->contains(src))
                continue;
            m_logPanel.push(std::string("[Project] Warning: the export includes an asset from ") +
                            (workspace.contains(src) ? "ANOTHER project: " : "outside the project: ") +
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
        m_logPanel.push("Export FAILED");
}

} // namespace DonTopo
