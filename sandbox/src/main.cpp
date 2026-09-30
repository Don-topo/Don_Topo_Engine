#include "DonTopo/Core/Window.h"
#include "DonTopo/Renderer/Renderer.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/SkinnedFrameSync.h"
#include "DonTopo/Renderer/Cube.h"
#include "DonTopo/Renderer/Sphere.h"
#include "DonTopo/Renderer/Plane.h"
#include "DonTopo/Core/Camera.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/JobSystem.h"
#include "DonTopo/Renderer/AsyncAssetLoader.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Physics/Rigidbody.h"
#include "DonTopo/Scripting/ScriptManager.h"
#include "DonTopo/Renderer/Gizmos.h"
#include "DonTopo/Editor/EditorShortcuts.h"
#include "DonTopo/Editor/EditorUI.h"
#include "DonTopo/Editor/ProjectContext.h"
#include "DonTopo/Renderer/D3D12/D3D12Renderer.h"
#include "DonTopo/Core/Input.h"
#include "DonTopo/UI/UiInputBridge.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#ifdef DT_D3D12_ENABLED
#include <d3d12.h>
#include <imgui_impl_dx12.h>
#include <nlohmann/json.hpp>
#include <fstream>
#endif
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <glm/gtc/matrix_transform.hpp>
#ifdef DT_PHYSX_ENABLED
#include <PxPhysicsAPI.h>
#include "DonTopo/Audio/AudioListenerComponent.h"
#include "DonTopo/Physics/Colliders/BoxCollider.h"
#endif

namespace {

// ─── Project selector ────────────────────────────────────────────────────────
// Used by both backends: it is pure ImGui UI, it does not know what is drawing it.

// Screen state. It lives outside the function because it has to survive
// from one frame to the next: which project is selected and what was typed in
// the create dialog.
struct ProjectSelectorState {
    std::vector<std::filesystem::path> entries = DonTopo::ProjectContext::discover();
    int                                picked  = -1;
    char                               newName[64] = {};
    std::string                        createError;
};

// Draws the screen and returns the chosen project, or an empty path while
// none has been chosen.
std::filesystem::path drawProjectSelector(ProjectSelectorState& st)
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##ProjectSelector", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::TextUnformatted("Don Topo Engine - Projects");
    ImGui::TextDisabled("%s", DonTopo::ProjectContext::workspaceDir().string().c_str());
    ImGui::Separator();

    bool                  confirmed = false;
    std::filesystem::path chosen;

    ImGui::BeginChild("##ProjectList", ImVec2(0.0f, vp->WorkSize.y * 0.55f), true);
    if (st.entries.empty())
        ImGui::TextDisabled("There are no projects yet: create one with 'New project...'.");
    for (int i = 0; i < (int)st.entries.size(); ++i)
    {
        const std::string label = DonTopo::ProjectContext::readProjectName(st.entries[i]) +
                                  "###project" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), st.picked == i,
                              ImGuiSelectableFlags_AllowDoubleClick))
        {
            st.picked = i;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                confirmed = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", st.entries[i].filename().string().c_str());
    }
    ImGui::EndChild();

    ImGui::BeginDisabled(st.picked < 0);
    if (ImGui::Button("Open project", ImVec2(160.0f, 0.0f)))
        confirmed = true;
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("New project...", ImVec2(160.0f, 0.0f)))
    {
        st.newName[0] = '\0';
        st.createError.clear();
        ImGui::OpenPopup("Create project");
    }

    if (ImGui::BeginPopupModal("Create project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Project name:");
        ImGui::SetNextItemWidth(320.0f);
        const bool enter = ImGui::InputText("##NewProjectName", st.newName, sizeof(st.newName),
                                            ImGuiInputTextFlags_EnterReturnsTrue);

        // The error is shown HERE, in the dialog, and nothing is created: repeated
        // name (even if the case changes), empty, `..`, path
        // separators or invalid characters on Windows.
        if (!st.createError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", st.createError.c_str());

        if (ImGui::Button("Create", ImVec2(120.0f, 0.0f)) || enter)
        {
            std::filesystem::path created;
            if (DonTopo::ProjectContext::create(st.newName, created, st.createError))
            {
                // Refreshes the list and leaves the new project selected.
                st.entries = DonTopo::ProjectContext::discover();
                st.picked  = -1;
                for (int i = 0; i < (int)st.entries.size(); ++i)
                    if (st.entries[i] == created)
                        st.picked = i;
                st.createError.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
        {
            st.createError.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (confirmed && st.picked >= 0 && st.picked < (int)st.entries.size())
        chosen = st.entries[st.picked];

    ImGui::End();
    return chosen;
}

// Leaves the editor with that project open. false = the project was not valid and the
// selector stays on screen.
bool openChosenProject(DonTopo::EditorUI& editor, DonTopo::ProjectContext& project,
                       const std::filesystem::path& chosen)
{
    project = DonTopo::ProjectContext(chosen);
    if (!project.valid())
        return false;

    editor.setProject(&project);
    // It is remembered for the NEXT startup: the render backend will come from
    // this project.json. If it fails nothing is aborted —the only effect
    // is starting on Vulkan again.
    DonTopo::ProjectContext::writeLastProject(chosen);
    // And with ITS scene, not the one set up at startup: in a newly created
    // project it is empty, so it is entered seeing only the skybox.
    editor.openProjectScene();
    return true;
}

}  // namespace

int main()
{
    try {
        DonTopo::Window window;
        window.init(1280, 720, "Don Topo Engine", "assets/MainEngineLogo.png");
        DonTopo::Input::init(window.getNativeWindow());

        // Render backend. It is decided HERE, before creating ANYTHING on the GPU, because
        // the device hangs from it. The setting lives in project.json, but the
        // project has not been chosen yet (its selector is drawn with ImGui, that
        // is, with a Renderer already running): that is why it is read from the last
        // opened project, which is what editor.json remembers.
        //
        // resolveRenderBackend never fails: whatever cannot be started falls back to
        // Vulkan with a reason.
        DonTopo::RenderBackend requestedBackend = DonTopo::RenderBackend::Vulkan;
        const std::filesystem::path lastProject = DonTopo::ProjectContext::readLastProject();
        if (!lastProject.empty())
        {
            const DonTopo::ProjectContext::ViewSettings last =
                DonTopo::ProjectContext::readSettings(lastProject, {});
            bool backendOk    = true;
            requestedBackend  = DonTopo::renderBackendFromName(last.renderBackend, backendOk);
            if (!backendOk)
                requestedBackend = DonTopo::RenderBackend::Vulkan;
        }

        const DonTopo::BackendSelection backend = DonTopo::resolveRenderBackend(requestedBackend);

#ifdef DT_D3D12_ENABLED
        // DirectX 12 path. It exits through here BEFORE building the editor, the
        // physics, the audio or the scene: none of that knows how to draw with
        // this backend yet, and bringing it up only to not use it would just
        // drag Vulkan dependencies into a process that is not going to open it.
        //
        // This loop is the skeleton that the following phases keep filling in
        // until it matches the Vulkan one. Today it presents and processes events.
        if (backend.backend == DonTopo::RenderBackend::D3D12)
        {
            std::cout << backend.message << std::endl;

            // The backend is built by main and ownership ends up in the editor;
            // the reference to the concrete type is kept before moving it, because
            // the life cycle (init, drawFrame, shutdown) is not in the
            // interface the editor consumes.
            auto d3d12Owned = std::make_unique<DonTopo::D3D12::D3D12Renderer>();
            DonTopo::D3D12::D3D12Renderer& d3d12 = *d3d12Owned;
            d3d12.init(window);


            std::cout << "D3D12: adapter '" << d3d12.adapterName() << "'" << std::endl;

            // The project scene, loaded with the SAME Scene::load the editor
            // uses. The geometry enters through the backend's public API,
            // just like the editor does with the Vulkan Renderer.
            //
            // The declaration order is copied from the Vulkan path and for
            // the same reasons: colliders release actors on the
            // PxScene and AudioClips release channels, so the scene has
            // to be destroyed BEFORE physics and audio.
            DonTopo::PhysicsManager d3dPhysics;
            d3dPhysics.init();
            DonTopo::AudioManager d3dAudio;
            d3dAudio.init();
            // And the ScriptManager BEFORE the scene, also as in Vulkan:
            // the ScriptComponents in the tree store sol::table whose destructor
            // touches the Lua VM, so the scene has to die before
            // it (destruction goes in reverse of declaration).
            DonTopo::ScriptManager d3dScripts;
            DonTopo::Scene         d3dScene;

            // The project, which is where the scene and the settings come from. It lives
            // here, outside everything, because the panels query it throughout
            // the session.
            DonTopo::ProjectContext d3dProject;
            if (!lastProject.empty())
                d3dProject = DonTopo::ProjectContext(lastProject);

            // JobSystem and asynchronous loading: without this Load Scene falls to the
            // synchronous path and blocks the whole frame while Assimp works.
            DonTopo::JobSystem d3dJobs;
            d3dJobs.start();
            DonTopo::AsyncAssetLoader d3dAssets(d3dJobs);

            // Before, the user pointer was plain &d3d12: the drop of external
            // files needs its own queue besides the renderer, so it
            // gains a small struct instead of a loose pointer — same
            // pattern as the AppCtx of the Vulkan path.
            struct D3D12WindowCtx {
                DonTopo::D3D12::D3D12Renderer* renderer;
                std::vector<DonTopo::DroppedFile> drops;
                // Filled in once they exist (below): the key callback needs
                // them for the F shortcut. Null until then, and checked.
                DonTopo::EditorUI* editor = nullptr;
                DonTopo::Camera*   camera = nullptr;
            };
            D3D12WindowCtx d3dWindowCtx{ &d3d12, {} };
            glfwSetWindowUserPointer(window.getNativeWindow(), &d3dWindowCtx);
            glfwSetFramebufferSizeCallback(
                window.getNativeWindow(), [](GLFWwindow* w, int width, int height) {
                    auto* c = static_cast<D3D12WindowCtx*>(glfwGetWindowUserPointer(w));
                    if (c && c->renderer)
                        c->renderer->resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
                });
            glfwSetDropCallback(window.getNativeWindow(), [](GLFWwindow* w, int count, const char** paths) {
                auto* c = static_cast<D3D12WindowCtx*>(glfwGetWindowUserPointer(w));
                double x, y;
                glfwGetCursorPos(w, &x, &y);
                for (int i = 0; i < count; ++i)
                    c->drops.push_back({ std::filesystem::path(paths[i]), (float)x, (float)y });
            });

            // The editor, with this backend behind it. From here on the path
            // is the same as with Vulkan: the panels talk to the interface.
            DonTopo::EditorUI editor;
            d3dWindowCtx.editor = &editor;

            // Editor shortcuts and the game canvas' text channel, as in the
            // Vulkan path. They MUST be installed before editor.initUi: in this
            // path ImGui installs its own GLFW callbacks (install_callbacks =
            // true, see EditorUI) and chains the ones already set, so ImGui
            // keeps receiving every key and character and these don't forward
            // anything to it. Installed after initUi, they would replace ImGui's.
            glfwSetKeyCallback(window.getNativeWindow(), [](GLFWwindow* w, int key, int, int action, int) {
                auto* c = static_cast<D3D12WindowCtx*>(glfwGetWindowUserPointer(w));
                switch (DonTopo::editorKeyAction(key, action, ImGui::GetIO().WantTextInput))
                {
                    case DonTopo::EditorKeyAction::CloseWindow:
                        glfwSetWindowShouldClose(w, GLFW_TRUE);
                        break;
                    case DonTopo::EditorKeyAction::FocusSelected:
                        if (c && c->editor && c->camera)
                            c->editor->focusSelected(*c->camera);
                        break;
                    case DonTopo::EditorKeyAction::None:
                        break;
                }
            });
            glfwSetCharCallback(window.getNativeWindow(), [](GLFWwindow* w, unsigned int ch) {
                // Same gate as the Vulkan path: only in Play and only while ImGui
                // has no text focus, or renaming a GameObject would also type
                // into the scene's InputField.
                auto* c = static_cast<D3D12WindowCtx*>(glfwGetWindowUserPointer(w));
                if (c && c->editor && c->editor->isPlaying() && !ImGui::GetIO().WantTextInput)
                    DonTopo::pushUiInputChar(ch);
            });

            editor.setRenderer(std::move(d3d12Owned));
            editor.setActiveRenderBackend(backend.backend);
            if (!backend.message.empty())
                editor.pushExternalLog(backend.message);

            DonTopo::UiLayer::InitInfo uiInfo{};
            uiInfo.api            = DonTopo::UiLayer::GraphicsApi::D3D12;
            uiInfo.window         = window.getNativeWindow();
            uiInfo.d3dDevice      = d3d12.nativeDevice();
            uiInfo.d3dQueue       = d3d12.nativeQueue();
            uiInfo.d3dSrvHeap     = d3d12.uiDescriptorHeap();
            uiInfo.d3dSrvCpuStart = d3d12.uiHeapStartCpu();
            uiInfo.d3dSrvGpuStart = d3d12.uiHeapStartGpu();
            uiInfo.d3dSrvCount    = d3d12.uiDescriptorCount();
            uiInfo.d3dSrvStride   = d3d12.descriptorSize();
            uiInfo.d3dRtvFormat   = DXGI_FORMAT_R8G8B8A8_UNORM;
            uiInfo.framesInFlight = static_cast<uint32_t>(d3d12.framesInFlight());
            editor.initUi(uiInfo);

            // The scene goes to a texture and the backbuffer is left for the
            // interface: it is what the viewport inside its panel needs.
            d3d12.setUiLayer(&editor);
            d3d12.setRenderToTexture(true);

            // To the BACKEND too, not just to the editor: it is where it gets the
            // scene reflection probes from. Without this it sees none and bakes
            // nothing, without saying why.
            d3d12.setScene(&d3dScene);
            d3d12.setSceneRoot(&d3dScene.getRoot());

            editor.setScene(&d3dScene);
            editor.setPhysicsManager(&d3dPhysics);
            editor.setAudioManager(&d3dAudio);
            editor.setAssetLoader(&d3dAssets);
            editor.setJobSystem(&d3dJobs);
            editor.setDroppedFilesProvider([&d3dWindowCtx]() {
                std::vector<DonTopo::DroppedFile> out;
                out.swap(d3dWindowCtx.drops);
                return out;
            });

            // Scripting, with the same wiring as the Vulkan path.
            d3dScripts.setScene(&d3dScene);
            d3dScripts.setPhysicsManager(&d3dPhysics);
            d3dScripts.setAudioManager(&d3dAudio);
            d3dScripts.setLogCallback(
                [&editor](const std::string& msg) { editor.pushExternalLog(msg); });
            d3dScripts.setOnInstantiated([&d3d12](DonTopo::GameObject* go) {
                // What a script instantiates has to be uploaded to the GPU: otherwise,
                // it exists in the scene and is not drawn.
                go->traverse([&d3d12](DonTopo::GameObject* n) {
                    if (!n->hasMesh())
                        return;
                    if (n->isSkinned())
                        n->skinnedRenderIndex = d3d12.addSkinnedMesh(*n->getSkinnedMesh());
                    else
                        n->staticRenderIndex = d3d12.addStaticMesh(*n->getMesh());
                });
            });
            // A single place for "this node goes away": it is announced by Scene, so
            // it covers the three paths (Scene panel, Undo of Delete and
            // Lua's Scene.Destroy) instead of only the scripts one.
            d3dScene.setOnNodeRemoved([&d3d12, &editor](DonTopo::GameObject* go) {
                // The selection first: if the editor is left pointing at what
                // is about to be released, it crashes when drawing Properties the next
                // frame.
                editor.onGameObjectDestroyed(go);
                d3d12.removeGameObject(go);
            });
            // Scripts/ lives at the repo root and is not copied next to the exe: the
            // hot reload has to watch the .lua files the user edits.
            std::filesystem::path d3dScriptsDir = "Scripts";
            if (!std::filesystem::is_directory(d3dScriptsDir))
            {
                for (auto dir = std::filesystem::current_path(); dir != dir.parent_path();
                     dir = dir.parent_path())
                {
                    if (std::filesystem::is_directory(dir / "Scripts"))
                    {
                        d3dScriptsDir = dir / "Scripts";
                        break;
                    }
                }
            }
            d3dScripts.init(d3dScriptsDir.string());
            editor.setScriptManager(&d3dScripts);
            editor.pushExternalLog(
                "DirectX 12: the editor runs on this backend. No in-game 2D UI and no "
                "reflection probes yet.");

            // Project selector, the same as with Vulkan: while it is
            // active the loop only presents its frame, so the editor does not
            // appear until one is chosen. It is what opens the scene —through
            // openProjectScene, the same door as the menu's Load Scene— and
            // what sets the ProjectContext the settings come from.
            //
            // The remembered project is NOT opened by itself: the backend we started with
            // came from it, but choosing is up to the user.
            ProjectSelectorState d3dProjectSelector;
            editor.setProjectSelector([&]() -> bool {
                const std::filesystem::path chosen = drawProjectSelector(d3dProjectSelector);
                if (chosen.empty())
                    return false;
                return openChosenProject(editor, d3dProject, chosen);
            });

            // Fly camera, the same one this path already had.
            DonTopo::Camera d3dCamera(glm::vec3(6.0f, 4.5f, 8.0f), -126.87f, -21.8f);
            d3dWindowCtx.camera = &d3dCamera;
            d3dCamera.moveSpeed = 8.0f;
            d3d12.setCamera(d3dCamera);

            double d3dLastX = 0.0, d3dLastY = 0.0;
            bool   d3dLooking   = false;
            auto   d3dLastFrame = std::chrono::high_resolution_clock::now();

            // Lights: the buffers live outside the loop so as not to reallocate per
            // frame. The filler ones are the same ones the Vulkan path uses and
            // only appear if the scene contributes none.
            const std::vector<DonTopo::Light> d3dDefaultLights = {
                { glm::vec4(0.0f, 500.0f, 300.0f, 1.0f),     glm::vec4(1.0f, 0.95f, 0.8f, 1.0f) },
                { glm::vec4(-300.0f, 200.0f, -200.0f, 1.0f), glm::vec4(0.4f, 0.5f, 1.0f, 0.8f) },
            };
            std::vector<DonTopo::Light> d3dLights;
            std::vector<float>          d3dLightRadii;

            // Pairs the scene canvases with their backend slots by
            // ownerId (syncUiCanvases), outside the loop so as not to reassign every
            // frame. The sync cache of each canvas lives INSIDE its slot.
            std::vector<DonTopo::UiCanvasBinding> d3dBindings;
            // And the SCREEN canvases in input priority order, also
            // outside the loop: it is refilled entirely every frame.
            std::vector<DonTopo::UiCanvas*> d3dUiCanvases;

            while (!window.shouldClose())
            {
                window.pollEvents();

                const auto  d3dNow = std::chrono::high_resolution_clock::now();
                const float d3dDelta =
                    std::chrono::duration<float>(d3dNow - d3dLastFrame).count();
                d3dLastFrame = d3dNow;

                // Selector on screen: neither scene, nor scripts, nor camera. Only
                // its frame, which is drawn by the editor itself inside
                // buildUiFrame.
                if (editor.isProjectSelectorActive())
                {
                    editor.buildUiFrame(d3d12.viewportTexture(), &d3dScene.getRoot(),
                                        d3dCamera.getViewMatrix());
                    // With the selector in front no scene is drawn, but the
                    // gizmo singleton is global: discarding it here also
                    // prevents whatever was left from before from carrying over. It is the other
                    // legitimate case of throwing away without consuming, like the frame that
                    // is aborted because of an out-of-date swapchain.
                    DonTopo::Gizmos::discard();
                    d3d12.drawFrame();
                    continue;
                }

                // Scene → backend, per frame and BEFORE the interface: it is what
                // makes moving an object in Properties, hiding a mesh,
                // touching its reflections or moving a light visible. Without this the
                // backend keeps what it was sent at load and the editor
                // can be looked at but not touched. Same traversal as the Vulkan
                // path, except for the Play part —physics and scripts— which this
                // path does not run yet.
                //
                // Live traversal and not a cached list: the editor can
                // delete GameObjects, and a stored pointer would be left dangling.
                d3dScripts.pollChanges();

                // Same criterion as the Vulkan path, and for the same
                // reasons: in Play the scene's Audio Listener rules if there
                // is one and it is enabled, in Edit Mode the editor camera always
                // rules (the inspector preview has to be heard from
                // where the user is looking). With a degenerate basis (scale 0) it
                // falls back to the camera instead of sneaking a NaN into FMOD, from which it
                // does not recover.
                glm::vec3 listenerPos = d3dCamera.getPos();
                glm::vec3 listenerFwd = d3dCamera.getFront();
                glm::vec3 listenerUp  = d3dCamera.getUp();
                if (editor.isPlaying())
                {
                    if (DonTopo::GameObject* lis = d3dScene.findAudioListener())
                    {
                        const glm::vec3 fwdAxis = glm::vec3(lis->worldTransform[2]);
                        const glm::vec3 upAxis  = glm::vec3(lis->worldTransform[1]);
                        // getEnabled() as in Vulkan: without it, this path
                        // respected a disabled listener and the two
                        // backends sounded different with the same scene.
                        if (lis->getAudioListener()->getEnabled() &&
                            glm::length(fwdAxis) > 1e-6f && glm::length(upAxis) > 1e-6f)
                        {
                            listenerPos = glm::vec3(lis->worldTransform[3]);
                            listenerFwd = glm::normalize(-fwdAxis);
                            listenerUp  = glm::normalize(upAxis);
                        }
                    }
                }
                // Outside the Play gate: see the comment of the Vulkan path
                // (it is the only call to System::update() and to
                // set3DListenerAttributes).
                d3dAudio.update(listenerPos, listenerFwd, listenerUp, d3dDelta);

                if (editor.isPlaying())
                {
                    d3dPhysics.stepSimulation(d3dDelta);
                    d3dScene.update(d3dDelta);
                    d3dScene.syncReverbZones(d3dAudio);
                    d3dScripts.update(d3dDelta);
                }
                else
                {
                    // Without physics running, but parent→child transforms
                    // keep being propagated: Properties and the gizmos have to
                    // work in Edit. Scene::update would do this and would also
                    // impose the PhysX pose on every object with a dynamic
                    // collider, which is exactly what prevents editing them.
                    d3dScene.getRoot().updateWorldTransforms();
                    // Same as in the Vulkan path: the inspector preview
                    // follows the object while it is dragged.
                    // Without dt: in Edit Mode there is no doppler (the object is moved
                    // by the gizmo, not by a physical velocity).
                    d3dScene.updateAudioSpatial();
                }

                // tickDeferredDeletes first, which releases what was deleted;
                // onAssetsLoaded after, which applies what just arrived from the
                // loader and closes the batch with a single flush. Both BEFORE the
                // traversal: an object whose mesh just landed has to be
                // already registered when the transforms are pushed.
                d3d12.tickDeferredDeletes();
                editor.onAssetsLoaded(d3dAssets.pumpCompleted(2.0f), d3dScene, d3d12);

                d3dScene.traverse([&](DonTopo::GameObject* go) {
                    if (go->staticRenderIndex >= 0)
                    {
                        d3d12.setTransform(static_cast<size_t>(go->staticRenderIndex),
                                           go->worldTransform);
                        d3d12.setObjectSsr(static_cast<size_t>(go->staticRenderIndex),
                                           go->ssrEnabled ? go->ssrIntensity : 0.0f);
                        d3d12.setObjectMeshVisible(static_cast<size_t>(go->staticRenderIndex),
                                                   go->meshVisible);
                    }

                    // The Play state is carried by the editor; the DirectX 12
                    // backend does not know it.
                    applySkinnedFrame(*go, d3d12, d3dDelta, editor.isPlaying());
                });

                // Lights after the traversal: their worldTransform are already
                // propagated and position and direction come from there. Per frame and not
                // in an event because moving a light's GameObject has to
                // move it right away, just like a mesh's transform.
                // The TOTAL, not the trimmed one: collectLights keeps the
                // first MAX_LIGHTS and silently discards the rest. Without
                // storing it, the editor cannot warn about what is lost.
                const size_t totalLuces = d3dScene.collectLights(d3dLights, d3dLightRadii);
                d3d12.setSceneLightTotal(totalLuces);
                if (totalLuces > 0)
                {
                    d3d12.setLights(d3dLights);
                    d3d12.setLightRadii(d3dLightRadii);
                }
                else
                {
                    // Without a single light in the scene, the filler ones: the repo
                    // scenes predate LightComponent and without this the
                    // editor would open in the dark.
                    d3d12.setLights(d3dDefaultLights);
                    d3d12.setLightRadii({});
                }

                // Game 2D UI: resolution, widgets and hierarchy of EACH
                // canvas of the scene, per frame —for the same reason as the
                // transforms, touching a field in Properties has to be seen
                // right away—. Without any Canvas the list is empty and the sync
                // cleans the tree.
                d3dBindings.clear();
                d3dScene.collectCanvases(d3dBindings);
                d3d12.syncUiCanvases(d3dBindings);

                // UI input: without this the tree does not resolve states and the
                // button colors, the fade and the Click would do nothing. The
                // mouse only enters in Play; time always enters, so that a
                // freshly edited color is seen without pressing Play.
                {
                    DonTopo::UiInputState uiInput;
                    if (editor.isPlaying() && editor.isViewportImageHovered())
                    {
                        const ImVec2    m   = ImGui::GetIO().MousePos;
                        const glm::vec2 org = editor.viewportImagePos();
                        uiInput.mousePos     = glm::vec2(m.x - org.x, m.y - org.y);
                        uiInput.mouseDown[0] = ImGui::IsMouseDown(ImGuiMouseButton_Left);
                        uiInput.mouseDown[1] = ImGui::IsMouseDown(ImGuiMouseButton_Right);
                        uiInput.mouseDown[2] = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
                        // The wheel is already read by ImGui, so it is asked of it
                        // instead of duplicating a GLFW callback.
                        uiInput.scrollDelta = ImGui::GetIO().MouseWheel;
                    }
                    else
                    {
                        uiInput.mousePos = glm::vec2(-1.0f, -1.0f);
                    }
                    // Keyboard and gamepad ONLY in Play, like the mouse: in
                    // editing, Tab and the arrows belong to the editor.
                    if (editor.isPlaying() && !ImGui::GetIO().WantCaptureKeyboard)
                        DonTopo::fillUiInputKeys(uiInput);
                    else
                        // The PUSH gate (WantTextInput) is not the same as this one,
                        // so a character may have come in on a frame that is not
                        // consumed. Without discarding it, it would come out on the next one that is.
                        DonTopo::discardUiInputChars();
                    uiInput.timeSeconds = (float)glfwGetTime();
                    // To ALL the screen canvases, not just the first: see
                    // dispatchUiInput. With uiCanvas() the buttons of a second
                    // canvas were drawn but were inert.
                    d3d12.screenUiCanvases(d3dUiCanvases);
                    DonTopo::dispatchUiInput(d3dUiCanvases, uiInput);
                }

                // The interface afterwards: whatever is touched here is picked up by the
                // next frame's traversal.
                editor.buildUiFrame(d3d12.viewportTexture(), &d3dScene.getRoot(),
                                    d3dCamera.getViewMatrix());

                // Camera after the interface, so that dragging over a
                // panel does not rotate the view.
                {
                    GLFWwindow* native = window.getNativeWindow();

                    // Over the scene panel, not "where ImGui does not capture":
                    // the viewport IS an ImGui window, so
                    // WantCaptureMouse is true precisely where the
                    // view has to be rotated and the camera never moved. Same criterion as
                    // the Vulkan path.
                    const bool rightDown =
                        editor.isViewportHovered() &&
                        glfwGetMouseButton(native, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

                    double mouseX = 0.0, mouseY = 0.0;
                    glfwGetCursorPos(native, &mouseX, &mouseY);
                    if (rightDown) {
                        if (d3dLooking)
                            d3dCamera.processMouse(static_cast<float>(mouseX - d3dLastX),
                                                   static_cast<float>(mouseY - d3dLastY));
                        d3dLooking = true;
                    } else {
                        d3dLooking = false;
                    }
                    d3dLastX = mouseX;
                    d3dLastY = mouseY;

                    // The camera KEYBOARD only with the right button
                    // held, as in Unity: without that, W/E/R —the gizmo
                    // mode shortcuts— would move the camera forward and up instead
                    // of changing mode. The gamepad does not enter the split: it does not
                    // compete with any shortcut and keeps flying always.
                    if (editor.isViewportHovered())
                        d3dCamera.update(native, d3dDelta, /*keyboardEnabled=*/rightDown);

                    d3d12.setCamera(d3dCamera);
                }

                // Gizmos: filled in by the viewport panel inside
                // buildUiFrame (colliders, lights, camera frustum, axes of
                // the selection) and here they are uploaded to the backend. In Vulkan this is
                // done by Renderer::drawFrame.
                //
                // takeVertices takes them AND empties the buffer at once: this
                // path once read them without emptying and the vector grew frame by
                // frame until exceeding kMaxGizmoVertices, with the capacity warning as
                // the only symptom. Now consuming empties, so the
                // next backend cannot repeat it (H16).
                {
                    const std::vector<DonTopo::GizmoVertex> lineas =
                        DonTopo::Gizmos::takeVertices();
                    d3d12.submitDebugLines(
                        lineas.empty() ? nullptr : &lineas[0].pos.x, lineas.size());
                }

                d3d12.drawFrame();
            }

            // CRITICAL ORDER. The GPU is still on the last frame in flight when leaving
            // the loop, and that frame uses the ImGui buffers and texture.
            // Shutting ImGui down without waiting first takes them from the GPU from underneath, and the
            // process dies on close —with a WER dump, but with nothing on
            // screen giving it away, because there is no frame left to draw.
            d3d12.waitIdle();
            editor.shutdownUi();
            // The JobSystem before the backend: a half-done job still touches
            // the loader and the scene, and stopping it afterwards would be using them already
            // released.
            d3dJobs.shutdown();
            d3d12.shutdown();
            return 0;
        }
#endif

        // The backend is built by main —which is the one that knows which applies— and
        // ownership passes to the editor. The reference to the concrete type is kept
        // BEFORE moving it: the life cycle (init, drawFrame, shutdown) is not
        // in the interface, and this path is the Vulkan one.
        DonTopo::EditorUI editor;

        auto               vulkanRenderer = std::make_unique<DonTopo::Renderer>();
        DonTopo::Renderer& renderer       = *vulkanRenderer;
        editor.setRenderer(std::move(vulkanRenderer));

        editor.setActiveRenderBackend(backend.backend);
        if (!backend.message.empty())
            editor.pushExternalLog(backend.message);

        // scene.shutdown() explicitly releases the colliders/
        // audioclips of the scene before destroying physics/audio (see below).
        // physics/audio are still declared before scene as a safety
        // net against an exit by exception prior to that explicit
        // shutdown: in that case, the declaration order still guarantees
        // that scene is destroyed before physics/audio.
        DonTopo::PhysicsManager physics;
        physics.init();

        DonTopo::AudioManager audio;
        audio.init();

        // scriptManager is declared BEFORE scene: the ScriptComponents in the
        // tree store sol::table whose destructor touches the Lua VM, so
        // scene must be destroyed before the sol::state of scriptManager
        // (destruction order = reverse of declaration).
        DonTopo::ScriptManager scriptManager;

        DonTopo::Scene scene;

        // Startup scene: only what startup needs, nothing more.
        // renderer.init() requires a non-empty `meshes` —it takes the auto-fit bbox
        // from it, and with an empty list it would come out as infinities—, so a
        // procedural cube is enough, which does not touch disk. The three FBX the demo
        // loaded (modelTexture, model and modelAnimation) were the whole startup cost
        // and are no longer loaded: as soon as a project is chosen this scene is
        // replaced entirely by the project's (EditorUI::openProjectScene), so
        // loading them was work thrown in the trash.
        // The size of this floor is NOT decorative: renderer.init() takes from it
        // m_cameraDistance (= maxDim * 1.2) and from there come the near and far of the
        // editor projection (near = d*0.001, far = d*3). With only the cube of
        // 50 the far fell to ~180 and, with the camera at z=300, the skybox was clipped.
        // The 1000 is the same the demo floor had, so the
        // framing and the skybox look the same as before.
        auto floorMesh = std::make_shared<DonTopo::Mesh>(DonTopo::Plane::create(1000.0f, 0.0f));
        auto cubeMesh  = std::make_shared<DonTopo::Mesh>(DonTopo::Cube::create(50.0f));

        auto* floorNode = scene.addGameObject("floor");
        floorNode->setMesh(floorMesh);
        floorNode->setBoxCollider(physics.createBoxColliderComponent(
            glm::vec3(500.0f, 0.5f, 500.0f), glm::vec3(0.0f),
            glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.5f, 0.0f)), /*dynamic=*/false));

        auto* cube = scene.addGameObject("cube");
        cube->setMesh(cubeMesh);
        cube->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 50.0f, -200.0f));

        cube->updateWorldTransforms();
        cube->setBoxCollider(physics.createBoxColliderComponent(
            glm::vec3(25.0f, 25.0f, 25.0f), glm::vec3(0.0f), cube->worldTransform, /*dynamic=*/true));
        // Rigidbody: makes the cube a simulated body (it falls with the scene
        // gravity). Without a Rigidbody the collider would be static and would not fall.
        {
            auto cubeRb = std::make_shared<DonTopo::Rigidbody>();
            physics.attachRigidbody(cube->getBoxCollider(), cubeRb);
            cube->setRigidbody(cubeRb);
        }

#ifdef DT_PHYSX_ENABLED
        {
            physx::PxRaycastBuffer hit;
            physx::PxVec3 origin(cube->worldTransform[3].x, cube->worldTransform[3].y + 200.0f, cube->worldTransform[3].z);
            physx::PxVec3 dir(0.0f, -1.0f, 0.0f);
            bool didHit = physics.raycast(origin, dir, 400.0f, hit);
            std::cout << "[PhysX smoke test] raycast at the cube: " << (didHit ? "HIT" : "MISS") << std::endl;
        }
#endif

        std::vector<DonTopo::GameObject*> allNodes;
        scene.traverse([&](DonTopo::GameObject* go) { allNodes.push_back(go); });

        // Pass 1: static meshes -> Renderer::init(meshes)
        std::vector<DonTopo::Mesh> meshes;
        for (auto* go : allNodes)
        {
            if (go->hasMesh() && !go->isSkinned())
            {
                go->staticRenderIndex = (int)meshes.size();
                meshes.push_back(*go->getMesh());
            }
        }

        DonTopo::Camera camera({0.0f, 90.0f, 300.0f});


        renderer.init(window, meshes);
        renderer.setSceneRoot(&scene.getRoot());
        renderer.setScene(&scene);
        editor.setScene(&scene);
        editor.setPhysicsManager(&physics);
        editor.setAudioManager(&audio);

        scriptManager.setScene(&scene);
        scriptManager.setPhysicsManager(&physics);
        scriptManager.setAudioManager(&audio);
        scriptManager.setLogCallback([&editor](const std::string& msg) {
            editor.pushExternalLog(msg);
        });
        scriptManager.setOnInstantiated([&renderer](DonTopo::GameObject* go) {
            go->traverse([&renderer](DonTopo::GameObject* n) {
                if (!n->hasMesh()) return;
                if (n->isSkinned()) n->skinnedRenderIndex = renderer.addSkinnedMesh(*n->getSkinnedMesh());
                else                n->staticRenderIndex  = renderer.addStaticMesh(*n->getMesh());
            });
        });
        // A single place for "this node goes away": it is announced by Scene, so it covers
        // the three paths (Scene panel, Undo of Delete and Lua's Scene.Destroy)
        // instead of only the scripts one.
        scene.setOnNodeRemoved([&renderer, &editor](DonTopo::GameObject* go) {
            // Releases the editor selection if it points to go or its subtree BEFORE
            // releasing anything — otherwise m_selected is left dangling and the editor crashes
            // when drawing Properties/gizmo the next frame.
            editor.onGameObjectDestroyed(go);
            // Releases the GPU of the whole subtree (static + skinned).
            renderer.removeGameObject(go);
        });
        // Scripts/ lives at the repo root and is NOT copied next to the exe
        // (unlike assets/shaders): the hot reload must watch the
        // original .lua files the user edits, not a copy that every build
        // would overwrite. Since the exe runs with CWD in build-ninja/sandbox, the
        // folder is searched upwards from the current directory.
        std::filesystem::path scriptsDir = "Scripts";
        if (!std::filesystem::is_directory(scriptsDir))
        {
            for (auto dir = std::filesystem::current_path();
                 dir != dir.parent_path(); dir = dir.parent_path())
            {
                if (std::filesystem::is_directory(dir / "Scripts"))
                {
                    scriptsDir = dir / "Scripts";
                    break;
                }
            }
        }
        scriptManager.init(scriptsDir.string());
        editor.setScriptManager(&scriptManager);

        editor.setOnAxisSelected([&camera](const glm::vec3& axis) { camera.lookAlongAxis(axis); });

        renderer.initSkybox({
            "assets/skybox/px.png",  // +X
            "assets/skybox/nx.png",  // -X
            "assets/skybox/py.png",  // +Y
            "assets/skybox/ny.png",  // -Y
            "assets/skybox/pz.png",  // +Z
            "assets/skybox/nz.png",  // -Z
        });

        // Pass 2: animated meshes -> addSkinnedMesh (after init, as the Renderer requires)
        for (auto* go : allNodes)
        {
            if (go->hasMesh() && go->isSkinned())
                go->skinnedRenderIndex = renderer.addSkinnedMesh(*go->getSkinnedMesh());
        }

        // Default lights: the ones used when the opened scene has
        // not a single LightComponent. The repo scenes predate the
        // component and without this the editor would open in the dark; as
        // soon as the scene contributes ONE light, these disappear and the scene rules.
        const std::vector<DonTopo::Light> defaultLights = {
            { glm::vec4(0.0f, 500.0f, 300.0f, 1.0f),     glm::vec4(1.0f, 0.95f, 0.8f, 1.0f) },
            { glm::vec4(-300.0f, 200.0f, -200.0f, 1.0f), glm::vec4(0.4f, 0.5f, 1.0f, 0.8f) },
        };
        std::vector<DonTopo::Light> frameLights;
        std::vector<float>          frameLightRadii;
        renderer.setLights(defaultLights);

        struct AppCtx {
            DonTopo::Camera* cam;
            DonTopo::Renderer* rnd;
            DonTopo::EditorUI* ed;
            std::vector<DonTopo::DroppedFile> drops;
        };
        AppCtx ctx{ &camera, &renderer, &editor, {} };
        glfwSetWindowUserPointer(window.getNativeWindow(), &ctx);

        glfwSetFramebufferSizeCallback(window.getNativeWindow(), [](GLFWwindow* w, int, int) {
            static_cast<AppCtx*>(glfwGetWindowUserPointer(w))->rnd->notifyResize();
        });

        // Camera: only rotates with the right button and when ImGui does not capture the mouse
        glfwSetCursorPosCallback(window.getNativeWindow(), [](GLFWwindow* w, double x, double y) {
            ImGui_ImplGlfw_CursorPosCallback(w, x, y);
            static double lastX = x, lastY = y;
            double dx = x - lastX, dy = y - lastY;
            lastX = x; lastY = y;
            auto* ctx = static_cast<AppCtx*>(glfwGetWindowUserPointer(w));
            if (ctx->ed->isViewportHovered() &&
                glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
            {
                ctx->cam->processMouse((float)dx, (float)dy);
            }
        });

        // Forward mouse buttons and scroll to ImGui
        glfwSetMouseButtonCallback(window.getNativeWindow(), [](GLFWwindow* w, int btn, int action, int mods) {
            ImGui_ImplGlfw_MouseButtonCallback(w, btn, action, mods);
        });
        glfwSetScrollCallback(window.getNativeWindow(), [](GLFWwindow* w, double xoff, double yoff) {
            ImGui_ImplGlfw_ScrollCallback(w, xoff, yoff);
        });
        glfwSetCharCallback(window.getNativeWindow(), [](GLFWwindow* w, unsigned int c) {
            ImGui_ImplGlfw_CharCallback(w, c);
            // To the game canvas ONLY in Play and with ImGui without text focus:
            // the same gate already applied to the keyboard a few lines
            // below. Without this, renaming a GameObject in the Hierarchy would also
            // type into the scene's InputField.
            auto* ctx = static_cast<AppCtx*>(glfwGetWindowUserPointer(w));
            if (ctx && ctx->ed && ctx->ed->isPlaying() && !ImGui::GetIO().WantTextInput)
                DonTopo::pushUiInputChar(c);
        });

        glfwSetKeyCallback(window.getNativeWindow(), [](GLFWwindow* w, int key, int scancode, int action, int mods) {
            ImGui_ImplGlfw_KeyCallback(w, key, scancode, action, mods);
            // Same decision as the DirectX 12 path (editorKeyAction).
            switch (DonTopo::editorKeyAction(key, action, ImGui::GetIO().WantTextInput))
            {
                case DonTopo::EditorKeyAction::CloseWindow:
                    glfwSetWindowShouldClose(w, GLFW_TRUE);
                    break;
                case DonTopo::EditorKeyAction::FocusSelected:
                {
                    auto* ctx = static_cast<AppCtx*>(glfwGetWindowUserPointer(w));
                    ctx->ed->focusSelected(*ctx->cam);
                    break;
                }
                case DonTopo::EditorKeyAction::None:
                    break;
            }
        });

        glfwSetDropCallback(window.getNativeWindow(), [](GLFWwindow* w, int count, const char** paths) {
            auto* ctx = static_cast<AppCtx*>(glfwGetWindowUserPointer(w));
            double x, y;
            glfwGetCursorPos(w, &x, &y);
            for (int i = 0; i < count; ++i)
                ctx->drops.push_back({ std::filesystem::path(paths[i]), (float)x, (float)y });
        });

        // JobSystem + asynchronous asset loader. They are created after all the setup and
        // BEFORE the loop: the FBX drop and Load Scene enqueue here, and the per-frame
        // pump (below) drains the results. The shutdown of the JobSystem
        // goes FIRST on leaving the loop (join of the workers before destroying
        // Renderer/Scene) — see the shutdown block.
        DonTopo::JobSystem        jobSystem;
        jobSystem.start();
        DonTopo::AsyncAssetLoader assetLoader(jobSystem);
        editor.setAssetLoader(&assetLoader);
        editor.setDroppedFilesProvider([&ctx]() {
            std::vector<DonTopo::DroppedFile> out;
            out.swap(ctx.drops);
            return out;
        });
        editor.setJobSystem(&jobSystem);

        // ─── Project selector ────────────────────────────────────────────────
        // First state of the ImGui loop that already exists: same window, same
        // Vulkan device and same ImGui session. While it is active, the loop
        // below skips EVERYTHING (scripts, camera, scene, lights, UI) and only
        // presents the selector frame, so the editor does not appear until a
        // project is chosen. `project` lives here, outside the loop: it is what the
        // panels query throughout the session.
        DonTopo::ProjectContext project;
        ProjectSelectorState    projectSelector;

        editor.setProjectSelector([&]() -> bool {
            const std::filesystem::path chosen = drawProjectSelector(projectSelector);
            if (chosen.empty())
                return false;
            return openChosenProject(editor, project, chosen);
        });

        // Pairs the scene canvases with their Renderer slots by
        // ownerId (Renderer::syncUiCanvases), outside the loop so as not to
        // reassign every frame. The sync cache of each canvas lives INSIDE
        // its slot, in the Renderer: none is needed here any more.
        std::vector<DonTopo::UiCanvasBinding> uiBindings;
        // And the SCREEN canvases in input priority order, also
        // outside the loop: it is refilled entirely every frame.
        std::vector<DonTopo::UiCanvas*> uiCanvases;

        while (!window.shouldClose())
        {
            DonTopo::Input::update();

            // Selector active: the frame just presents it. No
            // scripts, no camera, no scene, no lights, no UI — nothing below
            // runs until a project is chosen.
            if (editor.isProjectSelectorActive())
            {
                renderer.drawFrame(window);
                window.pollEvents();
                continue;
            }

            scriptManager.pollChanges();

            auto now = std::chrono::high_resolution_clock::now();
            static auto last = now;
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;

            // Same split as the D3D12 path: the camera KEYBOARD
            // only while the right button is held, which is what leaves
            // W/E/R free for the gizmo mode shortcuts. The gamepad stays
            // outside the split and keeps flying always.
            //
            // Here the button is queried from GLFW instead of reusing a variable:
            // in the Vulkan path the mouse-look lives in the cursor
            // callback, not in the loop, so there is no `rightDown` to
            // borrow.
            if (editor.isViewportHovered())
            {
                const bool rightDown = glfwGetMouseButton(window.getNativeWindow(),
                                                          GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
                camera.update(window.getNativeWindow(), dt, /*keyboardEnabled=*/rightDown);
            }
            renderer.setCamera(camera);

            // 3D listener. In Play, if the scene has an Audio Listener (and it is
            // enabled), 3D audio is heard from IT: position = column 3 of the
            // worldTransform, forward = local -Z, up = local +Y (same
            // convention as the camera and the runtime); with a degenerate basis
            // (scale 0) it falls back to the camera instead of sneaking a NaN into FMOD, from
            // which it does not recover.
            //
            // In Edit Mode the editor camera ALWAYS rules, even if the scene
            // has a listener: the Audio Clip Play button (PropertiesPanel) is
            // an editing tool and has to be heard from where the
            // user is looking. With the scene's listener ruling here,
            // previewing a 3D clip of a distant object would give silence with no
            // explanation.
            glm::vec3 listenerPos = camera.getPos();
            glm::vec3 listenerFwd = camera.getFront();
            glm::vec3 listenerUp  = camera.getUp();
            if (renderer.isPlaying())
            {
                if (DonTopo::GameObject* lis = scene.findAudioListener())
                {
                    const glm::vec3 lisFwdAxis = glm::vec3(lis->worldTransform[2]);
                    const glm::vec3 lisUpAxis  = glm::vec3(lis->worldTransform[1]);
                    if (lis->getAudioListener()->getEnabled() &&
                        glm::length(lisFwdAxis) >= 1e-6f && glm::length(lisUpAxis) >= 1e-6f)
                    {
                        listenerPos = glm::vec3(lis->worldTransform[3]);
                        listenerFwd = glm::normalize(-lisFwdAxis);
                        listenerUp  = glm::normalize(lisUpAxis);
                    }
                }
            }
            // OUTSIDE the Play gate, on purpose: AudioManager::update is the only
            // thing that calls set3DListenerAttributes and System::update(),
            // so leaving it inside the listener stayed where the last
            // Play session left it —or at FMOD's factory (0,0,0) looking at -Z if Play
            // was never entered— and the preview of a 3D clip sounded attenuated
            // or muted for no apparent reason.
            // dt for the doppler: the listener velocity comes from comparing
            // with its position from the previous frame.
            audio.update(listenerPos, listenerFwd, listenerUp, dt);

            if (renderer.isPlaying())
            {
                physics.stepSimulation(dt);
                scene.update(dt);
                scene.syncReverbZones(audio);
                scriptManager.update(dt);
            }
            else
            {
                // Without physics running, but parent→child transforms
                // keep being propagated: gizmo/Properties must keep
                // working in Edit Mode. Scene::update also does this,
                // but it also imposes the PhysX pose on every GameObject
                // with a dynamic collider — exactly what makes editing
                // those objects impossible today (physics fights them every frame). By
                // skipping scene.update() entirely in Edit Mode, that pull no
                // longer happens.
                scene.getRoot().updateWorldTransforms();
                // But 3D audio tracking is needed here: the
                // inspector preview may be playing while the user
                // drags the object with the gizmo. In Play scene.update does it.
                // Without dt on purpose: dragging with the gizmo is not velocity.
                scene.updateAudioSpatial();
            }

            // Per-frame pump of the asynchronous load, BEFORE the traverse: the
            // objects whose mesh just arrived have to be already registered
            // in the Renderer when the scene is traversed to push transforms.
            // tickDeferredDeletes first (releases what was deleted); onAssetsLoaded
            // applies the results and closes the batch with ONE
            // flushPendingUploads (so ~440 vkQueueWaitIdle become one).
            renderer.tickDeferredDeletes();
            editor.onAssetsLoaded(assetLoader.pumpCompleted(2.0f), scene, renderer);

            // Live traversal (not the allNodes list cached at startup): the
            // editor allows deleting GameObjects in real time, so a
            // cached pointer could be left dangling after a delete.
            DonTopo::GameObject* liveCube = nullptr;
            scene.traverse([&](DonTopo::GameObject* go) {
                if (go == cube)
                    liveCube = go;

                if (go->staticRenderIndex >= 0)
                {
                    renderer.setTransform(go->staticRenderIndex, go->worldTransform);
                    // Same place as the transform: it is the only per-frame
                    // synchronization that already covers Play Mode, Undo/Redo and scene
                    // loading without paths of their own.
                    renderer.setObjectSsr(go->staticRenderIndex,
                                          go->ssrEnabled ? go->ssrIntensity : 0.0f);
                    renderer.setObjectMeshVisible(go->staticRenderIndex, go->meshVisible);
                }

                // In Edit the graph does not evaluate transitions (it only advances the time
                // of the entry state); otherwise, the "animation
                // finished" conditions would walk the graph by themselves only in the editor.
                applySkinnedFrame(*go, renderer, dt, renderer.isPlaying());
            });

            // Scene lights, after the traverse: the frame's worldTransform are
            // already propagated and position and direction come from them.
            // It goes per frame and not in an event because moving a light's
            // GameObject has to move it right away, just like a mesh's
            // transform.
            // The TOTAL, not the trimmed one: see the comment of the DX12 path.
            const size_t totalLuces = scene.collectLights(frameLights, frameLightRadii);
            renderer.setSceneLightTotal(totalLuces);
            if (totalLuces > 0)
            {
                renderer.setLights(frameLights);
                renderer.setLightRadii(frameLightRadii);
            }
            else
            {
                renderer.setLights(defaultLights);
                renderer.setLightRadii({});
            }

            // UI canvas: the resolution, widgets and hierarchy of EACH
            // scene canvas come from their components, not from one wired
            // here. It goes per frame and not in an event because touching a field in
            // Properties has to be seen right away — also outside Play,
            // which is when the usable-area gizmo is drawn. Without any Canvas
            // the list is empty and syncUiCanvases leaves clean whatever there was.
            uiBindings.clear();
            scene.collectCanvases(uiBindings);
            renderer.syncUiCanvases(uiBindings);

            // UI input: without this the tree does not resolve states, so the
            // five button colors, the fade and the Click would do nothing.
            // The MOUSE only enters in Play (as in Unity: in editing a button is not
            // highlighted when hovered), but time always enters —
            // so the Normal state applies as soon as its color is edited and is seen
            // without pressing Play. The viewport image is drawn 1:1 with the
            // render, so subtracting its corner from the mouse already gives the canvas
            // pixel, without scaling anything.
            {
                DonTopo::UiInputState uiInput;
                if (editor.isPlaying() && editor.isViewportImageHovered())
                {
                    const ImVec2   m   = ImGui::GetIO().MousePos;
                    const glm::vec2 org = editor.viewportImagePos();
                    uiInput.mousePos     = glm::vec2(m.x - org.x, m.y - org.y);
                    uiInput.mouseDown[0] = ImGui::IsMouseDown(ImGuiMouseButton_Left);
                    uiInput.mouseDown[1] = ImGui::IsMouseDown(ImGuiMouseButton_Right);
                    uiInput.mouseDown[2] = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
                    // The wheel is already read by ImGui: it is asked of it instead of
                    // duplicating a GLFW callback.
                    uiInput.scrollDelta = ImGui::GetIO().MouseWheel;
                }
                else
                {
                    // Outside the whole canvas: no button is left in Hover.
                    uiInput.mousePos = glm::vec2(-1.0f, -1.0f);
                }
                // Keyboard and gamepad ONLY in Play, like the mouse: in editing
                // Tab and the arrows belong to the editor, not to the game.
                if (editor.isPlaying() && !ImGui::GetIO().WantCaptureKeyboard)
                    DonTopo::fillUiInputKeys(uiInput);
                else
                    // Same reason as in the D3D12 path: the two gates do not
                    // coincide, and what is not consumed cannot cross the frame.
                    DonTopo::discardUiInputChars();
                uiInput.timeSeconds = (float)glfwGetTime();
                // To ALL the screen canvases, not just the first: see
                // dispatchUiInput.
                renderer.screenUiCanvases(uiCanvases);
                DonTopo::dispatchUiInput(uiCanvases, uiInput);
            }

            // --- Gizmos: visual debug demo (bbox, ray, frustum) ---
            // The axes are no longer drawn fixed here: ViewportPanel::drawSelectionGizmo()
            // shows them automatically on any selected GameObject.
            // liveCube (captured in the traverse above, not the `cube` pointer
            // cached in the setup) avoids a use-after-free if the user deleted the
            // "cube" GameObject from the editor: scene.traverse() only visits live
            // nodes, so liveCube is nullptr that frame instead of dangling.
            if (liveCube)
            {
                DonTopo::Gizmos::drawRay(
                    glm::vec3(liveCube->worldTransform[3].x, liveCube->worldTransform[3].y + 200.0f, liveCube->worldTransform[3].z),
                    glm::vec3(0.0f, -1.0f, 0.0f), 400.0f, glm::vec3(1.0f, 0.0f, 1.0f));
            }

            {
                glm::mat4 debugView = glm::lookAt(glm::vec3(0.0f, 300.0f, 300.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
                glm::mat4 debugProj = glm::perspective(glm::radians(45.0f), 16.0f / 9.0f, 10.0f, 500.0f);
                debugProj[1][1] *= -1.0f;
                DonTopo::Gizmos::drawFrustum(debugProj * debugView, glm::vec3(1.0f));
            }

            renderer.drawFrame(window);
            window.pollEvents();
        }

        // The drops provider captures `ctx` by reference, and ctx (declared
        // AFTER editor, because it stores pointers to it) dies before the
        // editor. Releasing it here avoids leaving a lambda dangling during the
        // rest of the shutdown.
        editor.setDroppedFilesProvider(nullptr);

        // The JobSystem FIRST: if it were destroyed after the Renderer/Scene, a
        // worker still in flight (a half-done FBX ReadFile) could touch memory
        // already freed. The join in shutdown() guarantees that no thread is alive
        // before starting to destroy the rest.
        jobSystem.shutdown();

        // Explicitly releases colliders/audioclips before destroying
        // physics/audio: without this, ~BoxCollider() would try to release() a
        // PxRigidDynamic on an already released PxScene (or ~AudioClipComponent
        // would call an already destroyed AudioManager).
        scene.shutdown();
        audio.shutdown();
        physics.shutdown();
        renderer.shutdown();
        window.shutdown();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return 0;
}
