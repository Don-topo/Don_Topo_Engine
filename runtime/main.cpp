// Runtime del juego: carga un .scene y lo ejecuta. Es el wiring de
// sandbox/src/main.cpp menos todo lo del editor — sin ImGui, sin gizmos de
// depuración, sin hot reload y en Play desde el frame 0.
#include "DonTopo/Core/Engine.h"
#include "DonTopo/Core/Window.h"
#include "DonTopo/Core/Input.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Renderer/Renderer.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Scripting/ScriptManager.h"

#include <GLFW/glfw3.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// Directorio del ejecutable. El paquete exportado usa rutas relativas
// (assets/, shaders/, Scripts/), así que el runtime fija su CWD aquí: sin
// esto, lanzar el juego desde otra carpeta no encontraría nada.
std::filesystem::path executableDir()
{
#ifdef _WIN32
    wchar_t buffer[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (n == 0 || n == MAX_PATH)
        return std::filesystem::current_path();
    return std::filesystem::path(buffer).parent_path();
#else
    std::error_code ec;
    std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return std::filesystem::current_path();
    return self.parent_path();
#endif
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const std::filesystem::path exeDir = executableDir();
        std::error_code ec;
        std::filesystem::current_path(exeDir, ec);

        const std::string scenePath = (argc > 1) ? argv[1] : "game.scene";

        DonTopo::Engine engine;
        DonTopo::Window window;
        window.init(1280, 720, exeDir.stem().string().c_str(), nullptr);
        DonTopo::Input::init(window.getNativeWindow());
        DonTopo::Renderer renderer;

        // Orden de declaración calcado de sandbox/src/main.cpp:38-55, y por
        // los mismos motivos: los ScriptComponent guardan sol::table cuyo
        // destructor toca la VM Lua, y los colliders liberan actores sobre la
        // PxScene. Destruir en otro orden revienta al salir.
        DonTopo::PhysicsManager physics;
        physics.init();

        DonTopo::AudioManager audio;
        audio.init();

        DonTopo::ScriptManager scriptManager;

        DonTopo::Scene scene;

        if (!scene.load(scenePath, physics, audio))
        {
            std::cerr << "Error: no se pudo cargar la escena '" << scenePath << "'" << std::endl;
            return EXIT_FAILURE;
        }

        // Antes de init(): initImGui y createOffscreenImages leen el flag.
        renderer.setHeadless(true);

        std::vector<DonTopo::GameObject*> allNodes;
        scene.traverse([&](DonTopo::GameObject* go) { allNodes.push_back(go); });

        // Pasada 1: meshes estáticos -> Renderer::init(meshes)
        std::vector<DonTopo::Mesh> meshes;
        for (auto* go : allNodes)
        {
            if (go->hasMesh() && !go->isSkinned())
            {
                go->staticRenderIndex = (int)meshes.size();
                meshes.push_back(*go->getMesh());
            }
        }

        renderer.init(window, meshes);
        renderer.setSceneRoot(&scene.getRoot());
        renderer.setScene(&scene);
        renderer.setPhysicsManager(&physics);
        renderer.setAudioManager(&audio);

        renderer.initSkybox({
            "assets/skybox/px.png",
            "assets/skybox/nx.png",
            "assets/skybox/py.png",
            "assets/skybox/ny.png",
            "assets/skybox/pz.png",
            "assets/skybox/nz.png",
        });

        // Pasada 2: meshes animados, después de init como exige el Renderer.
        for (auto* go : allNodes)
        {
            if (go->hasMesh() && go->isSkinned())
                go->skinnedRenderIndex = renderer.addSkinnedMesh(*go->getSkinnedMesh());
        }

        // Mismas luces que el editor: la escena no las serializa.
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
        scriptManager.setOnDestroying([&renderer](DonTopo::GameObject* go) {
            renderer.removeGameObject(go);
        });
        // Scripts/ va dentro del paquete, junto al ejecutable — a diferencia
        // del editor, que la busca subiendo directorios hacia el repo.
        scriptManager.init("Scripts");
        renderer.setScriptManager(&scriptManager);

        glfwSetWindowUserPointer(window.getNativeWindow(), &renderer);
        glfwSetFramebufferSizeCallback(window.getNativeWindow(), [](GLFWwindow* w, int, int) {
            static_cast<DonTopo::Renderer*>(glfwGetWindowUserPointer(w))->notifyResize();
        });
        glfwSetKeyCallback(window.getNativeWindow(), [](GLFWwindow* w, int key, int, int action, int) {
            if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
                glfwSetWindowShouldClose(w, GLFW_TRUE);
        });

        scriptManager.onPlayStart();

        while (!window.shouldClose())
        {
            DonTopo::Input::update();

            auto now = std::chrono::high_resolution_clock::now();
            static auto last = now;
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;

            audio.update(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            physics.stepSimulation(dt);
            scene.update(dt, physics);
            scriptManager.update(dt);

            scene.traverse([&](DonTopo::GameObject* go) {
                if (go->staticRenderIndex >= 0)
                    renderer.setTransform(go->staticRenderIndex, go->worldTransform);

                if (go->skinnedRenderIndex >= 0)
                {
                    if (const auto& anim = go->getAnimator())
                    {
                        anim->update(dt, /*playing=*/true);
                        renderer.setAnimationState(go->skinnedRenderIndex,
                                                    (uint32_t)anim->currentClipIndex(),
                                                    anim->animTime());
                    }
                    else
                    {
                        renderer.updateAnimation(go->skinnedRenderIndex, dt);
                    }
                    renderer.setSkinnedTransform(go->skinnedRenderIndex, go->worldTransform);
                }
            });

            renderer.drawFrame(window);
            window.pollEvents();
        }

        scriptManager.onPlayStop();
        scene.shutdown(physics, audio);
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
