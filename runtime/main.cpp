// Game runtime: loads a .scene and runs it. It is the wiring of
// sandbox/src/main.cpp minus everything from the editor — no ImGui, no debug
// gizmos, no hot reload and in Play from frame 0.
#include "DonTopo/Core/Window.h"
#include "DonTopo/Core/Input.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/JobSystem.h"
#include "DonTopo/Renderer/Renderer.h"
#include "DonTopo/Renderer/AsyncAssetLoader.h"
#include "DonTopo/Renderer/RenderBackend.h"
#include "DonTopo/Renderer/EditorRenderer.h"
#include "DonTopo/Renderer/SkinnedFrameSync.h"
#include "DonTopo/UI/UiInputBridge.h"
#ifdef DT_D3D12_ENABLED
#include "DonTopo/Renderer/D3D12/D3D12Renderer.h"
#endif
#include <memory>
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Scripting/ScriptManager.h"
#include "DonTopo/Scripting/ScriptBindings.h"
#include "DonTopo/Files/FileManager.h"
#include "SplashDriver.h"

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Core/Platform.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Audio/AudioListenerComponent.h"

namespace {

// Mouse wheel accumulated between frames. GLFW only gives it through a callback (there is no
// "current state" to query), so it is summed here and the loop consumes it:
// reading it without emptying it would leave the scroll stuck forever.
float g_uiScroll = 0.0f;

void onScroll(GLFWwindow*, double, double yoffset)
{
    g_uiScroll += (float)yoffset;
}

// The same with characters: GLFW only gives them through a callback. In the runtime there is
// no editor to compete with, so they always go to the canvas.
void onChar(GLFWwindow*, unsigned int codepoint)
{
    DonTopo::pushUiInputChar(codepoint);
}

// Executable directory. The exported package uses relative paths
// (assets/, shaders/, Scripts/), so the runtime sets its CWD here: without
// this, launching the game from another folder would find nothing. Note: this
// happens BEFORE reading argv[1], so a relative argv[1] (e.g.
// "..\niveles\l2.scene") is also resolved against the executable
// directory, not against the cwd of whoever launched it — deliberate, same reason.
std::filesystem::path executableDir()
{
    return DonTopo::platform::executableDir();
}

// Sends std::cout and std::cerr to game.log, next to the executable. The runtime is
// linked as the WINDOWS subsystem (runtime/CMakeLists.txt) so as not to open a
// console behind the game, and without a console those streams go nowhere:
// the engine messages and Lua print() calls would be lost silently.
//
// The rdbuf is swapped instead of reopening stdout with freopen: without a console the MSVC CRT
// has no stream to reopen (_fileno(stdout) == -2) and freopen_s fails
// returning an error without creating the file — tested, it is not theory.
// Swapping the rdbuf does not touch the system descriptors, so it works the same
// with or without a console. What a third-party library writes through printf
// (Assimp, PhysX) is still not captured; the engine only uses cout/cerr.
//
// The ofstream is leaked on purpose: cout/cerr keep their rdbuf, and destroying it
// on exit would leave those pointers dangling during the destruction of static
// objects, where there may still be logs. unitbuf makes every << reach the
// disk, so a crash does not take the last lines with it — which are
// precisely the ones of interest.
void redirectStdioToLogFile()
{
    auto* logStream = new std::ofstream("game.log", std::ios::out | std::ios::trunc);
    if (!logStream->is_open())
    {
        delete logStream;
        return;
    }
    std::cout.rdbuf(logStream->rdbuf());
    std::cerr.rdbuf(logStream->rdbuf());
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);
}

// Errors that prevent playing: without a console, a message on stderr only ends up in
// game.log and the user sees the window close without explanation. It also goes to the
// log, which is where the trail remains after the dialog is closed.
void reportFatal(const std::string& msg)
{
    std::cerr << msg << std::endl;
    DonTopo::platform::showFatalError("Don Topo Engine", msg);
}

// What Scene::load repaired or ignored (a missing .mat, a bad collider...).
// The editor shows these in its Log; without this the exported game just
// renders without the asset and game.log says nothing about why.
void logSceneWarnings(const DonTopo::Scene& scene, const std::string& scenePath)
{
    for (const std::string& w : scene.lastWarnings())
        std::cerr << "Warning (" << scenePath << "): " << w << std::endl;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const std::filesystem::path exeDir = executableDir();
        std::error_code ec;
        std::filesystem::current_path(exeDir, ec);
        // After current_path: game.log is created next to the executable, not in
        // the directory from which the game was launched.
        redirectStdioToLogFile();

        const std::string scenePath = (argc > 1) ? argv[1] : "game.scene";

        // Render backend chosen at export. It lives in game.cfg and not in
        // game.scene: it is startup configuration, not part of the scene.
        // A package without game.cfg —exported before it existed— starts
        // with Vulkan, which is what it always did.
        //
        // The result leaves the block: it is what decides which backend is
        // built further below.
        DonTopo::RenderBackend requestedBackend = DonTopo::RenderBackend::Vulkan;
        {
            DonTopo::RenderBackend requested = DonTopo::RenderBackend::Vulkan;
            std::ifstream cfgIn(exeDir / "game.cfg");
            if (cfgIn.is_open())
            {
                try
                {
                    nlohmann::json cfg;
                    cfgIn >> cfg;
                    if (cfg.is_object())
                    {
                        const auto it = cfg.find("renderBackend");
                        if (it != cfg.end() && it->is_string())
                        {
                            bool ok  = true;
                            requested = DonTopo::renderBackendFromName(it->get<std::string>(), ok);
                            if (!ok)
                            {
                                std::cout << "game.cfg: unknown render backend, using Vulkan"
                                          << std::endl;
                                requested = DonTopo::RenderBackend::Vulkan;
                            }
                        }
                    }
                }
                catch (const std::exception&)
                {
                    std::cout << "game.cfg unreadable: using Vulkan" << std::endl;
                }
            }

            const DonTopo::BackendSelection sel = DonTopo::resolveRenderBackend(requested);
            // std::cout and not printf: the runtime runs without a console and what
            // is redirected to game.log are the C++ streams, so a printf would be
            // lost without a trace.
            if (!sel.message.empty())
                std::cout << sel.message << std::endl;

            // What can really be started, not what was requested:
            // resolveRenderBackend already fell back to Vulkan if DirectX 12 was not there.
            requestedBackend = sel.backend;
        }

        DonTopo::Window window;
        // Hidden from the start: it is shown after presenting the first frame (the splash's).
        // Without this, the window became visible here and Windows painted
        // the client area WHITE during the ~520ms that initPresentation takes
        // to bring up Vulkan — a white flash before the logo.
        window.init(1280, 720, exeDir.stem().string().c_str(), nullptr, /*showOnInit=*/false);
        DonTopo::Input::init(window.getNativeWindow());
        // The mouse wheel for the UI. Registered AFTER Input::init in
        // case someday it registers its own: the last one wins, and here the owner
        // of the wheel is the canvas.
        glfwSetScrollCallback(window.getNativeWindow(), onScroll);
        glfwSetCharCallback(window.getNativeWindow(), onChar);
        // The backend, built according to what the project asked for. From
        // here on the whole runtime talks to the interface: who decides which one it is
        // is this line and nobody else.
        std::unique_ptr<DonTopo::EditorRenderer> rendererOwned;
#ifdef DT_D3D12_ENABLED
        if (requestedBackend == DonTopo::RenderBackend::D3D12)
            rendererOwned = std::make_unique<DonTopo::D3D12::D3D12Renderer>();
#endif
        if (!rendererOwned)
            rendererOwned = std::make_unique<DonTopo::Renderer>();
        DonTopo::EditorRenderer& renderer = *rendererOwned;

        // Declaration order copied from sandbox/src/main.cpp:38-55, and for
        // the same reasons: ScriptComponents hold sol::table whose
        // destructor touches the Lua VM, and colliders release actors on the
        // PxScene. Destroying in another order blows up on exit.
        DonTopo::PhysicsManager physics;
        physics.init();

        DonTopo::AudioManager audio;
        audio.init();

        DonTopo::ScriptManager scriptManager;

        DonTopo::Scene scene;

        // The spec says "the editor sets it when opening a project and the runtime to its
        // working directory" (see EditorUI::setProject/setScene for the
        // editor side). Until now the latter did not happen: it worked by
        // chance because the paths inside the package are stored relative
        // and two independent relativizations (the editor's on export, the
        // one of toStoredPath on loading without a root) produce the same string. Setting it
        // here turns it into an explicit guarantee, not a coincidence of
        // path format. exeDir is the real working directory of the
        // runtime since line 146 (current_path already points there, even
        // before reading argv[1]) and it is where exportGame places assets/,
        // shaders/, Scripts/, game.scene and game.cfg — the same root that
        // toStoredPath would resolve if this were left unset.
        scene.setAssetRoot(exeDir.string());

        // Before initPresentation(): initImGui and createOffscreenImages read
        // the flag during that initialization. Moved earlier relative to the original order
        // because now initPresentation runs BEFORE scene.load (see
        // below), and setHeadless must precede it all the same.
        renderer.setHeadless(true);

        // Thread pool + asynchronous loader. The JobSystem is declared here, BEFORE
        // anything a job could capture by reference, and it is shut down by hand
        // before the scene teardown (see the end): a live worker touching
        // a half-destroyed scene would be a crash on exit.
        DonTopo::JobSystem jobSystem;
        jobSystem.start();
        DonTopo::AsyncAssetLoader assetLoader(jobSystem);

        // Resolves the logo: in an exported package it is next to the .exe as
        // splash.png; in dev (not exported) it falls back to assets/MainEngineLogo.png.
        std::string logoPath = "splash.png";
        {
            std::error_code lec;
            if (!std::filesystem::exists(logoPath, lec) || lec)
                logoPath = "assets/MainEngineLogo.png";
        }

        // initPresentation + splash BEFORE scene.load: neither depends on the
        // scene (initPresentation is phase 1 "be able to present"; the auto-fit and
        // the resources that need meshes live in initSceneResources, phase 2,
        // which still goes AFTER loading). This way the splash is already on screen
        // while the assets load and can show real progress.
        renderer.initPresentation(window);

        const auto splashStart = std::chrono::high_resolution_clock::now();
        const bool haveSplash = renderer.beginSplash(logoPath);
        const SplashTimings splashT;
        auto sinceSplash = [&]() {
            return std::chrono::duration<float>(
                std::chrono::high_resolution_clock::now() - splashStart).count();
        };
        auto pumpSplash = [&](bool loadingDone, float loadingDoneAt) {
            if (!haveSplash) return;
            window.pollEvents();
            SplashState s = splashStateAt(splashT, sinceSplash(), loadingDone, loadingDoneAt);
            renderer.drawSplashFrame(s.alpha);
        };

        // One splash frame before the heavy load (alpha of the initial fade-in).
        pumpSplash(false, 0.0f);

        // The window is shown HERE, with the first splash frame already
        // presented: the first thing the user sees is the logo over the dark
        // shader background, never the default white of the window. This
        // timing (show AFTER the first present of the splash) is what avoids the
        // white flash and is kept intact despite the reordering.
        // Without a splash (logo absent) it stays hidden until right before the game
        // loop — see the show() further below—, which also avoids the white.
        bool windowShown = false;
        if (haveSplash)
        {
            window.show();
            windowShown = true;
        }

        // --- Parallel preload (progress on the splash) ---
        // The heavy cost of scene.load is the Assimp::ReadFile of each mesh,
        // synchronous. The scene JSON is parsed by hand to collect the unique
        // sourcePaths, they are loaded in the workers, and then scene.load
        // consumes them from an in-RAM cache instead of reading disk — pumping the splash
        // all the time so that the window responds and shows progress.
        DonTopo::PreloadedMeshCache preloaded;
        {
            auto sceneJson = DonTopo::FileManager::readJson(scenePath);
            if (!sceneJson)
            {
                reportFatal("Error: could not load the scene '" + scenePath + "'");
                jobSystem.shutdown();
                return EXIT_FAILURE;
            }

            // Set of unique (sourcePath, piece): several nodes that share
            // file AND piece generate a single ReadFile (the loader also dedups
            // by path internally); two pieces of the same static model
            // request the same path request but a different piece, so the
            // piece enters the set key.
            std::set<std::pair<std::string, int>> uniquePaths;
            std::function<void(const nlohmann::json&)> collect = [&](const nlohmann::json& node) {
                if (node.contains("mesh") && node["mesh"].is_object())
                {
                    const std::string sp = node["mesh"].value("sourcePath", std::string());
                    if (!sp.empty())
                    {
                        // Same reading as nodeFromJson: absent or invalid = 0.
                        int piece = 0;
                        if (const auto it = node["mesh"].find("piece");
                            it != node["mesh"].end() && it->is_number_integer())
                            piece = std::max(0, it->get<int>());
                        uniquePaths.insert({ sp, piece });
                    }
                }
                if (auto it = node.find("children"); it != node.end() && it->is_array())
                    for (const auto& child : *it)
                        collect(child);
            };
            if (sceneJson->contains("root") && (*sceneJson)["root"].is_object())
                collect((*sceneJson)["root"]);

            // Enqueues a request per (path, piece). targetId is not used here (the
            // cache is indexed by meshCacheKey, not by GameObject: there is no
            // scene yet), so any index other than 0 goes in.
            uint64_t reqId = 1;
            for (const auto& p : uniquePaths)
                assetLoader.requestMesh(p.first, reqId++, p.second);

            // Pumps the splash while the workers load, storing each
            // result in the cache by path. An error (moved/broken file)
            // leaves the path out of the cache: scene.load will fall back to a disk ReadFile
            // for that one, exactly like the usual path.
            while (assetLoader.pending() > 0)
            {
                for (auto& r : assetLoader.pumpCompleted(1000.0f))
                {
                    if (!r.error.empty())
                    {
                        std::cerr << "Preload failed '" << r.path << "': " << r.error
                                  << " (it will be retried from disk when the scene loads)" << std::endl;
                        continue;
                    }
                    if (r.mesh)
                        preloaded[DonTopo::meshCacheKey(r.path, r.piece)] = r.mesh;
                }
                pumpSplash(false, 0.0f);
            }
        }

        // Scene load from the cache. loader == nullptr: the per-GameObject
        // async path of Task 8 is NOT used (it would lose the Animator clip config
        // and would not auto-fit in time). Instead, preloaded provides
        // the meshes already in RAM and loading runs through the usual synchronous
        // path, only without ReadFile — same registration model, same
        // animation config.
        if (!scene.load(scenePath, physics, audio, /*loader=*/nullptr, /*preloaded=*/&preloaded))
        {
            reportFatal("Error: could not load the scene '" + scenePath + "'");
            jobSystem.shutdown();
            return EXIT_FAILURE;
        }
        logSceneWarnings(scene, scenePath);

        // Without a CameraComponent, Renderer::currentFrameCamera() falls back to the
        // editor fallback (m_camera/m_viewMatrix), and if in addition the scene has no
        // static meshes the auto-fit of Renderer::init leaves m_cameraDistance at
        // -inf: a projection with NaN and a black window with no hint. The editor
        // warns on pressing Play (EditorUI.cpp); here there is no Play to press, so
        // the warning goes right after loading the scene.
        if (!scene.findCamera())
            std::cerr << "Warning: the scene has no camera (CameraComponent); "
                          "the game will not be able to render correctly." << std::endl;

        std::vector<DonTopo::GameObject*> allNodes;
        scene.traverse([&](DonTopo::GameObject* go) { allNodes.push_back(go); });

        // Pass 1: static meshes -> Renderer::init(meshes). The meshes are already
        // in the GameObjects (they came from the cache), so the auto-fit of
        // initSceneResources works the same as with synchronous loading.
        std::vector<DonTopo::Mesh> meshes;
        for (auto* go : allNodes)
        {
            if (go->hasMesh() && !go->isSkinned())
            {
                go->staticRenderIndex = (int)meshes.size();
                meshes.push_back(*go->getMesh());
            }
        }

        renderer.initSceneResources(meshes);
        pumpSplash(false, 0.0f);
        // Only what the Renderer really uses: setSceneRoot (the tree it
        // traverses to draw) and setScene (currentFrameCamera() calls
        // findCamera() in Play). The physics/audio/scripts passthroughs that
        // were here belonged to the editor, which does not exist at runtime.
        renderer.setSceneRoot(&scene.getRoot());
        renderer.setScene(&scene);

        renderer.initSkybox({
            "assets/skybox/px.png",
            "assets/skybox/nx.png",
            "assets/skybox/py.png",
            "assets/skybox/ny.png",
            "assets/skybox/pz.png",
            "assets/skybox/nz.png",
        });
        pumpSplash(false, 0.0f);

        // Pass 2: animated meshes, after init as the Renderer requires.
        for (auto* go : allNodes)
        {
            if (go->hasMesh() && go->isSkinned())
                go->skinnedRenderIndex = renderer.addSkinnedMesh(*go->getSkinnedMesh());
            pumpSplash(false, 0.0f);
        }

        // --- Wait for uploads before the first game frame (correctness) ---
        // addSkinnedMesh does NOT upload instantly: it puts the upload in m_pendingBatch and
        // marks the object with an uploadTicket > 0, so the mesh stays
        // INVISIBLE until the batch is sent (flushPendingUploads) and its fence
        // signals (detected by tickDeferredDeletes, which advances m_lastCompletedTicket).
        // Without this the exported .exe showed rigged characters half uploaded
        // —that is, invisible— on the first frame. The send is forced and it waits
        // for ALL the tickets to signal, with the splash still on screen so
        // that the window keeps responding and there is no pop-in. Static meshes
        // do not go through the batch (uploadTicket == 0), so they were already visible;
        // this is only needed for the skinned ones.
        renderer.flushPendingUploads();
        while (renderer.hasPendingUploads())
        {
            renderer.tickDeferredDeletes();   // recovers completed batches, advances m_lastCompletedTicket
            pumpSplash(false, 0.0f);          // splash up / window alive
        }

        // Same lights as the editor: the scene does not serialize them.
        renderer.setLights({
            { glm::vec4(0.0f, 500.0f, 300.0f, 1.0f),     glm::vec4(1.0f, 0.95f, 0.8f, 1.0f) },
            { glm::vec4(-300.0f, 200.0f, -200.0f, 1.0f), glm::vec4(0.4f, 0.5f, 1.0f, 0.8f) },
        });

        scriptManager.setScene(&scene);
        scriptManager.setPhysicsManager(&physics);
        scriptManager.setAudioManager(&audio);
        scriptManager.setLogCallback([](const std::string& msg) {
            std::cout << msg << std::endl;
        });
        scriptManager.setOnInstantiated([&renderer](DonTopo::GameObject* go) {
            go->traverse([&renderer](DonTopo::GameObject* n) {
                if (!n->hasMesh()) return;
                if (n->isSkinned()) n->skinnedRenderIndex = renderer.addSkinnedMesh(*n->getSkinnedMesh());
                else                n->staticRenderIndex  = renderer.addStaticMesh(*n->getMesh());
            });
        });
        // It is announced by Scene and not by the ScriptManager: at runtime it only deletes Lua,
        // but the contract is the same as in the editor and lives in one place.
        scene.setOnNodeRemoved([&renderer](DonTopo::GameObject* go) {
            renderer.removeGameObject(go);
        });
        // Scripts/ goes inside the package, next to the executable — unlike
        // the editor, which looks for it by going up directories toward the repo.
        scriptManager.init("Scripts");
        pumpSplash(false, 0.0f);

        glfwSetWindowUserPointer(window.getNativeWindow(), &renderer);
        glfwSetFramebufferSizeCallback(window.getNativeWindow(), [](GLFWwindow* w, int, int) {
            static_cast<DonTopo::EditorRenderer*>(glfwGetWindowUserPointer(w))->notifyResize();
        });
        glfwSetKeyCallback(window.getNativeWindow(), [](GLFWwindow* w, int key, int, int action, int) {
            if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
                glfwSetWindowShouldClose(w, GLFW_TRUE);
        });

        // Loading finished: mark the instant and drain the rest of the splash
        // (hold until minTotal + fade-out). Simple fallback (no crossfade
        // with the scene): the logo fades to its background color and is cut at the
        // first game frame. The real crossfade is a later improvement.
        if (haveSplash)
        {
            const float loadingDoneAt = sinceSplash();
            for (;;)
            {
                window.pollEvents();
                if (window.shouldClose()) break;
                SplashState s = splashStateAt(splashT, sinceSplash(), true, loadingDoneAt);
                // s.crossfading is ignored on purpose: this fallback only
                // draws the splash (faded to its background color), never the
                // scene underneath. The crossfade with the scene is out of
                // scope, it is not an oversight.
                renderer.drawSplashFrame(s.alpha);
                if (s.done) break;
            }
        }

        // Without a splash the window is still hidden: it is shown here, with everything loaded,
        // so that the first frame seen is the scene's. This way the "logo absent"
        // path does not show the white of the empty window either.
        if (!windowShown)
        {
            window.show();
            windowShown = true;
        }

        scriptManager.onPlayStart();

        // Exact replica of the editor's Play button (EditorUI.cpp:167-170): without
        // this, an AudioClipComponent with playOnAwake enabled plays on pressing
        // Play in the editor but goes silent in the exported .exe — the designer
        // enabled it trusting what they heard, and here there is no log to warn.
        //
        // A scene without an Audio Listener (or with its own disabled) plays
        // its clips ALL THE SAME: 3D audio is heard from the camera, the fallback
        // resolved by the loop below every frame. There used to be a gate here that
        // skipped this sweep, and it was removed because it only covered playOnAwake
        // —Lua's AudioClip:Play skipped it— and it cannot live inside
        // AudioManager or AudioClipComponent: those two classes are tested
        // directly and have to keep sounding without a scene. The warning
        // stays, now informative and true. One for the whole scene, not one per
        // clip.
        {
            DonTopo::GameObject* listenerGo = scene.findAudioListener();
            const bool listenerActive = listenerGo && listenerGo->getAudioListener()->getEnabled();
            if (!listenerActive)
                std::cerr << "No Audio Listener in the scene: 3D audio is heard from the camera"
                          << std::endl;
            scene.traverse([](DonTopo::GameObject* go) {
                if (go->hasAudioClip() && go->getAudioClip()->getPlayOnAwake())
                    go->getAudioClip()->play(glm::vec3(go->worldTransform[3]));
            });
        }

        // Pairs the scene canvases with their Renderer slots by
        // ownerId (Renderer::syncUiCanvases), outside the loop so as not to
        // reassign every frame. The sync cache of each canvas lives INSIDE
        // its slot, in the Renderer: none is needed here any more.
        std::vector<DonTopo::UiCanvasBinding> uiBindings;
        // And the SCREEN canvases in input priority order, also
        // outside the loop for the same reason: it is refilled entirely every frame.
        std::vector<DonTopo::UiCanvas*> uiCanvases;
        // Buffer of the audio load failure pump, outside the loop for the same
        // reason as the two above.
        std::vector<std::string> audioFailures;

        while (!window.shouldClose())
        {
            DonTopo::Input::update();

            // Draining the DonTopo.loadScene mailbox, at the start of the frame:
            // the scripts of the previous frame have already finished their tick, so
            // destroying the scene here does not kill the GameObject that requested the load.
            // Same sanitation as EditorUI::reloadSceneFromJson: release the GPU
            // resources of the old tree, reset indices, load, and register the
            // whole tree again in the Renderer.
            if (std::string luaScenePath; DonTopo::ScriptBindings::takePendingSceneLoad(luaScenePath))
            {
                for (auto& child : scene.getRoot().children)
                {
                    renderer.removeGameObject(child.get());
                    child->traverse([](DonTopo::GameObject* go) {
                        go->staticRenderIndex  = -1;
                        go->skinnedRenderIndex = -1;
                    });
                }
                bool luaLoaded = scene.load(luaScenePath, physics, audio);
                renderer.registerGameObject(&scene.getRoot());
                // Synchronous like the editor restore: without a flush, the meshes of the
                // deferred batch would not be seen until ~2 frames later and the
                // old tree is gone (flicker).
                renderer.flushUploadsAndWait();
                renderer.setSceneRoot(&scene.getRoot());
                // The near/far came from the meshes of the startup scene
                // (initSceneResources): without recomputing it, a larger scene loaded by
                // script looks clipped — the skybox first.
                // Same reason EditorUI calls it on reload.
                renderer.refitCameraRange();
                // The Lua alive set stored pointers of the old scene and the new
                // GameObjects may reuse those addresses.
                scriptManager.rebuildAliveSet();
                std::cout << (luaLoaded ? "Scene loaded: " : "Error loading scene: ")
                          << luaScenePath << std::endl;
                if (luaLoaded) logSceneWarnings(scene, luaScenePath);
            }

            auto now = std::chrono::high_resolution_clock::now();
            static auto last = now;
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;

            // 3D listener: FMOD is initialized with FMOD_INIT_3D_RIGHTHANDED
            // (AudioManager::init), so attenuation and panning depend
            // on where the listener points, not just where it is. It is
            // resolved by findCamera() on each iteration -not once before
            // the loop- because a Lua script can destroy GameObjects on
            // any frame; caching the pointer would leave it dangling. Without a
            // camera in the scene it falls back to the origin looking at -Z (same
            // values this code had before the fix), not to a deref of
            // nullptr.
            glm::vec3 listenerPos(0.0f);
            glm::vec3 listenerFwd(0.0f, 0.0f, -1.0f);
            glm::vec3 listenerUp(0.0f, 1.0f, 0.0f);
            if (DonTopo::GameObject* cam = scene.findCamera())
            {
                // Same axis convention the Renderer uses to build
                // the image seen on screen (Renderer.cpp:296-304,
                // Renderer::currentFrameCamera in Play) and which camera_tests.cpp
                // confirms: the camera looks at LOCAL -Z (world[2] is the
                // local +Z axis taken to world, so the real "forward" is
                // its negation) and local +Y is "up". If +Z were used here instead
                // of -Z, 3D audio would be mirrored relative to what
                // is seen on screen: sounds on the left would play on
                // the right and vice versa.
                // Degenerate basis (some Transform axis with scale 0, something
                // the editor lets you set from the Scale fields): here
                // glm::normalize would give NaN and that NaN would reach
                // set3DListenerAttributes, where FMOD has no way to
                // recover — 3D audio stays broken for the rest of the game.
                // Same epsilon criterion as CameraComponent::viewFromWorld
                // (CameraComponent.cpp:71-74), which resolves the mirror case for
                // the view matrix; if the basis is no good, it falls back to the default values
                // above (origin, -Z, +Y) instead of propagating NaN.
                const glm::vec3 camFwdAxis = glm::vec3(cam->worldTransform[2]);
                const glm::vec3 camUpAxis  = glm::vec3(cam->worldTransform[1]);
                if (glm::length(camFwdAxis) >= 1e-6f && glm::length(camUpAxis) >= 1e-6f)
                {
                    listenerPos = glm::vec3(cam->worldTransform[3]);
                    listenerFwd = glm::normalize(-camFwdAxis);
                    listenerUp  = glm::normalize(camUpAxis);
                }
            }
            // Audio Listener: if the scene has one (and it is enabled), it rules
            // and not the camera — same axis convention and same degenerate-basis guard
            // as the block above. Without a listener, what the camera
            // resolved stays, which is the fallback.
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
            audio.update(listenerPos, listenerFwd, listenerUp, dt);
            // Load failures that FMOD has detected since the previous frame.
            // With FMOD_NONBLOCKING the error does not exist when createSound
            // returns, so without this pump an asset that did not travel in the bundle
            // is absolute silence: neither the .exe nor game.log say anything. Each
            // sound is reported once.
            audioFailures.clear();
            audio.pollLoadFailures(audioFailures);
            for (const auto& failed : audioFailures)
                std::cerr << "Could not load the audio '" << failed
                          << "': missing file, unsupported format or corrupt data"
                          << std::endl;
            physics.stepSimulation(dt);
            scene.update(dt);
            scene.syncReverbZones(audio);
            scriptManager.update(dt);

            scene.traverse([&](DonTopo::GameObject* go) {
                if (go->staticRenderIndex >= 0)
                {
                    renderer.setTransform(go->staticRenderIndex, go->worldTransform);
                    // The runtime has to render the same as the editor: same
                    // sink, same place.
                    renderer.setObjectSsr(go->staticRenderIndex,
                                          go->ssrEnabled ? go->ssrIntensity : 0.0f);
                    renderer.setObjectMeshVisible(go->staticRenderIndex, go->meshVisible);
                }

                // The runtime always plays: the graph evaluates transitions.
                applySkinnedFrame(*go, renderer, dt, /*evaluateTransitions=*/true);
            });

            // Before drawFrame: Lua scripts can instantiate/delete
            // GameObjects on any frame. tickDeferredDeletes reclaims the
            // batches already signaled (advances visibility) and drains the deferred
            // deletes; flushPendingUploads sends the batch of what was instantiated
            // THIS frame (addStaticMesh/addSkinnedMesh via setOnInstantiated leave it
            // in m_pendingBatch: without a flush it is never uploaded and the object
            // stays invisible). Same pair as the editor loop
            // (sandbox/src/main.cpp: tickDeferredDeletes + onAssetsLoaded, which
            // ends in flushPendingUploads). In steady state, without instantiating
            // anything, the flush is a cheap no-op.
            renderer.tickDeferredDeletes();
            renderer.flushPendingUploads();

            // UI canvas: resolution, widgets and hierarchy of EACH canvas of
            // the scene, per frame — same rule as in the editor. Without any
            // Canvas, collectCanvases returns the empty list and syncUiCanvases
            // leaves clean whatever there was.
            uiBindings.clear();
            scene.collectCanvases(uiBindings);
            renderer.syncUiCanvases(uiBindings);

            // UI input: without this the tree does not resolve states and the five
            // button colors, the fade and the Click do not exist. The mouse is
            // in WINDOW pixels and the canvas works in OUTPUT pixels,
            // which need not match (window scaling).
            {
                DonTopo::UiInputState uiInput;
                double mx = 0.0, my = 0.0;
                glfwGetCursorPos(window.getNativeWindow(), &mx, &my);
                int ww = 0, wh = 0;
                glfwGetWindowSize(window.getNativeWindow(), &ww, &wh);
                // uiWidth/uiHeight and NOT renderWidth/renderHeight: the canvas is
                // resolved in OUTPUT pixels, which with SSAA are not those of the
                // render (the mouse landed twice as far from the cursor).
                const float sx = (ww > 0) ? (float)renderer.uiWidth()  / (float)ww : 1.0f;
                const float sy = (wh > 0) ? (float)renderer.uiHeight() / (float)wh : 1.0f;
                uiInput.mousePos = glm::vec2((float)mx * sx, (float)my * sy);
                uiInput.mouseDown[0] =
                    glfwGetMouseButton(window.getNativeWindow(), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                uiInput.mouseDown[1] =
                    glfwGetMouseButton(window.getNativeWindow(), GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
                uiInput.mouseDown[2] =
                    glfwGetMouseButton(window.getNativeWindow(), GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
                // Keyboard and gamepad: Tab, the arrows, accept and cancel.
                // Without this focus and navigation existed but there was no way
                // to move them, so a gamepad game could not use a menu.
                DonTopo::fillUiInputKeys(uiInput);
                // The wheel is accumulated by the callback and consumed here: if it
                // were read without emptying, a single wheel tick would scroll
                // forever.
                uiInput.scrollDelta = g_uiScroll;
                g_uiScroll = 0.0f;
                uiInput.timeSeconds = (float)glfwGetTime();
                // To ALL the screen canvases, not just the first: with
                // uiCanvas() the buttons of a second canvas (a pause menu
                // on top of the HUD) were drawn but had neither hover, nor
                // state colors, nor Click. dispatchUiInput distributes: the mouse
                // goes to the topmost one that has it underneath and the others are cleared.
                renderer.screenUiCanvases(uiCanvases);
                DonTopo::dispatchUiInput(uiCanvases, uiInput);
            }

            renderer.drawFrame(window);
            window.pollEvents();
        }

        scriptManager.onPlayStop();
        // jobSystem.shutdown() BEFORE the scene teardown: it stops and joins all
        // the workers, so none can touch the scene while it is destroyed.
        // (At this point nothing should be pending —the preload was
        // drained entirely before the loop— but the order is respected anyway.)
        jobSystem.shutdown();
        scene.shutdown();
        audio.shutdown();
        physics.shutdown();
        renderer.shutdown();
        window.shutdown();
    } catch (const std::exception& e) {
        reportFatal(std::string("Error: ") + e.what());
        return EXIT_FAILURE;
    }
    return 0;
}
