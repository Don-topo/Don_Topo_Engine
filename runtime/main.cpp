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
#include "SplashDriver.h"

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <chrono>
#include <cstdlib>
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
// esto, lanzar el juego desde otra carpeta no encontraría nada. Ojo: esto
// pasa ANTES de leer argv[1], así que un argv[1] relativo (p.ej.
// "..\niveles\l2.scene") también se resuelve contra el directorio del
// ejecutable, no contra el cwd de quien lo lanzó — deliberado, mismo motivo.
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
        // Oculta de entrada: se enseña tras presentar el primer frame (el del
        // splash). Sin esto, la ventana se hacia visible aqui y Windows pintaba
        // el area de cliente en BLANCO durante los ~520ms que tarda
        // initPresentation en levantar Vulkan — un flash blanco antes del logo.
        window.init(1280, 720, exeDir.stem().string().c_str(), nullptr, /*showOnInit=*/false);
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

        // Sin CameraComponent, Renderer::currentFrameCamera() cae al repliegue
        // del editor (m_camera/m_viewMatrix), y si además la escena no tiene
        // meshes estáticos el auto-fit de Renderer::init deja m_cameraDistance en
        // -inf: proyección con NaN y ventana negra sin ninguna pista. El editor
        // avisa al dar a Play (EditorUI.cpp); aquí no hay Play que pulsar, así
        // que el aviso va nada más cargar la escena.
        if (!scene.findCamera())
            std::cerr << "Aviso: la escena no tiene una camara (CameraComponent); "
                          "el juego no podra renderizar correctamente." << std::endl;

        // Antes de initPresentation(): initImGui y createOffscreenImages leen
        // el flag durante esa inicialización.
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

        // Resuelve el logo: en un paquete exportado esta junto al .exe como
        // splash.png; en dev (sin exportar) se cae a assets/MainEngineLogo.png.
        std::string logoPath = "splash.png";
        {
            std::error_code lec;
            if (!std::filesystem::exists(logoPath, lec) || lec)
                logoPath = "assets/MainEngineLogo.png";
        }

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

        // Un frame de splash antes de la carga pesada (alpha del fade-in inicial).
        pumpSplash(false, 0.0f);

        // La ventana se enseña AQUI, ya con el primer frame del splash
        // presentado: lo primero que ve el usuario es el logo sobre el fondo
        // oscuro del shader, nunca el blanco por defecto de la ventana.
        // Sin splash (logo ausente) se queda oculta hasta justo antes del bucle
        // de juego — ver el show() de mas abajo—, que tambien evita el blanco.
        bool windowShown = false;
        if (haveSplash)
        {
            window.show();
            windowShown = true;
        }

        renderer.initSceneResources(meshes);
        pumpSplash(false, 0.0f);
        // setSceneRoot/setPhysicsManager/setAudioManager (y más abajo
        // setScriptManager) son passthroughs puros al EditorUI embebido del
        // Renderer: en headless no dibujan nada ni cambian ningún cálculo. Se
        // llaman igual, por simetría con sandbox/src/main.cpp, para que el
        // siguiente lector no se pregunte si faltan a propósito. setScene es
        // la excepción del grupo: currentFrameCamera() SÍ lo usa (findCamera()
        // en Play), así que no vale quitarlo ni tratarlo como muerto.
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
        pumpSplash(false, 0.0f);

        // Pasada 2: meshes animados, después de init como exige el Renderer.
        for (auto* go : allNodes)
        {
            if (go->hasMesh() && go->isSkinned())
                go->skinnedRenderIndex = renderer.addSkinnedMesh(*go->getSkinnedMesh());
            pumpSplash(false, 0.0f);
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
        pumpSplash(false, 0.0f);
        renderer.setScriptManager(&scriptManager);

        glfwSetWindowUserPointer(window.getNativeWindow(), &renderer);
        glfwSetFramebufferSizeCallback(window.getNativeWindow(), [](GLFWwindow* w, int, int) {
            static_cast<DonTopo::Renderer*>(glfwGetWindowUserPointer(w))->notifyResize();
        });
        glfwSetKeyCallback(window.getNativeWindow(), [](GLFWwindow* w, int key, int, int action, int) {
            if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
                glfwSetWindowShouldClose(w, GLFW_TRUE);
        });

        // La carga termino: marcar el instante y drenar el resto del splash
        // (hold hasta minTotal + fade-out). Fallback simple (sin crossfade
        // con la escena): el logo funde a su color de fondo y se corta al
        // primer frame de juego. El crossfade real es una mejora posterior.
        if (haveSplash)
        {
            const float loadingDoneAt = sinceSplash();
            for (;;)
            {
                window.pollEvents();
                if (window.shouldClose()) break;
                SplashState s = splashStateAt(splashT, sinceSplash(), true, loadingDoneAt);
                // s.crossfading se ignora a proposito: este fallback solo
                // dibuja el splash (fundido a su color de fondo), nunca la
                // escena debajo. El crossfade con la escena queda fuera de
                // alcance, no es un olvido.
                renderer.drawSplashFrame(s.alpha);
                if (s.done) break;
            }
        }

        // Sin splash la ventana sigue oculta: se enseña aqui, con todo cargado,
        // para que el primer frame que se vea sea el de la escena. Asi el camino
        // "logo ausente" tampoco muestra el blanco de la ventana vacia.
        if (!windowShown)
        {
            window.show();
            windowShown = true;
        }

        scriptManager.onPlayStart();

        // Réplica exacta del botón Play del editor (EditorUI.cpp:167-170): sin
        // esto, un AudioClipComponent con playOnAwake activado suena al pulsar
        // Play en el editor pero sale mudo en el .exe exportado — el diseñador
        // lo activó confiando en lo que oyó, y aquí no hay ningún log que avise.
        scene.traverse([](DonTopo::GameObject* go) {
            if (go->hasAudioClip() && go->getAudioClip()->getPlayOnAwake())
                go->getAudioClip()->play(glm::vec3(go->worldTransform[3]));
        });

        while (!window.shouldClose())
        {
            DonTopo::Input::update();

            auto now = std::chrono::high_resolution_clock::now();
            static auto last = now;
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;

            // Listener 3D: FMOD se inicializa con FMOD_INIT_3D_RIGHTHANDED
            // (AudioManager::init), así que la atenuación y el paneo dependen
            // de a dónde apunte el listener, no solo de dónde esté. Se
            // resuelve por findCamera() en cada iteración -no una vez antes
            // del bucle- porque un script Lua puede destruir GameObjects en
            // cualquier frame; cachear el puntero lo dejaría colgante. Sin
            // cámara en la escena se cae al origen mirando a -Z (mismos
            // valores que traía este código antes del fix), no a un deref de
            // nullptr.
            glm::vec3 listenerPos(0.0f);
            glm::vec3 listenerFwd(0.0f, 0.0f, -1.0f);
            glm::vec3 listenerUp(0.0f, 1.0f, 0.0f);
            if (DonTopo::GameObject* cam = scene.findCamera())
            {
                // Misma convención de ejes que usa el Renderer para construir
                // la imagen que se ve en pantalla (Renderer.cpp:296-304,
                // Renderer::currentFrameCamera en Play) y que confirma
                // camera_tests.cpp: la cámara mira a -Z LOCAL (world[2] es el
                // eje +Z local llevado a mundo, así que el "adelante" real es
                // su negado) y +Y local es "arriba". Si aquí se usara +Z en
                // vez de -Z, el audio 3D quedaría reflejado respecto a lo que
                // se ve por pantalla: los sonidos de la izquierda sonarían a
                // la derecha y viceversa.
                // Base degenerada (algún eje del Transform con escala 0, algo
                // que el editor deja poner desde los campos de Scale): aquí
                // glm::normalize daría NaN y ese NaN llegaría a
                // set3DListenerAttributes, donde FMOD ya no tiene forma de
                // recuperarse — el audio 3D queda roto el resto de la partida.
                // Mismo criterio de epsilon que CameraComponent::viewFromWorld
                // (CameraComponent.cpp:71-74), que resuelve el caso espejo para
                // la matriz de vista; si la base no sirve, se cae a los valores
                // por defecto de arriba (origen, -Z, +Y) en vez de propagar NaN.
                const glm::vec3 camFwdAxis = glm::vec3(cam->worldTransform[2]);
                const glm::vec3 camUpAxis  = glm::vec3(cam->worldTransform[1]);
                if (glm::length(camFwdAxis) >= 1e-6f && glm::length(camUpAxis) >= 1e-6f)
                {
                    listenerPos = glm::vec3(cam->worldTransform[3]);
                    listenerFwd = glm::normalize(-camFwdAxis);
                    listenerUp  = glm::normalize(camUpAxis);
                }
            }
            audio.update(listenerPos, listenerFwd, listenerUp);
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
