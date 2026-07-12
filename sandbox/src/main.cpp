#include "DonTopo/Engine.h"
#include "DonTopo/Window.h"
#include "DonTopo/Renderer.h"
#include "DonTopo/ModelLoader.h"
#include "DonTopo/Cube.h"
#include "DonTopo/Sphere.h"
#include "DonTopo/Plane.h"
#include "DonTopo/Camera.h"
#include "DonTopo/GameObject.h"
#include "DonTopo/Scene.h"
#include "DonTopo/AudioManager.h"
#include "DonTopo/PhysicsManager.h"
#include "DonTopo/ScriptManager.h"
#include "DonTopo/Gizmos.h"
#include "DonTopo/Input.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <chrono>
#include <iostream>
#include <limits>
#include <glm/gtc/matrix_transform.hpp>
#ifdef DT_PHYSX_ENABLED
#include <PxPhysicsAPI.h>
#endif

int main()
{
    try {
        DonTopo::Engine engine;
        DonTopo::Window window;
        window.init(1280, 720, "Don Topo Engine", "assets/MainEngineLogo.png");
        DonTopo::Input::init(window.getNativeWindow());
        DonTopo::Renderer renderer;

        // scene.shutdown(physics, audio) libera explícitamente los colliders/
        // audioclips de la escena antes de destruir physics/audio (ver más abajo).
        // physics/audio se siguen declarando antes que scene como red de
        // seguridad ante una salida por excepción anterior a ese shutdown
        // explícito: en ese caso, el orden de declaración garantiza igualmente
        // que scene se destruya antes que physics/audio.
        DonTopo::PhysicsManager physics;
        physics.init();

        DonTopo::AudioManager audio;
        audio.init();

        // scriptManager se declara ANTES que scene: los ScriptComponent del
        // árbol guardan sol::table cuyo destructor toca la VM Lua, así que
        // scene debe destruirse antes que el sol::state de scriptManager
        // (orden de destrucción = inverso al de declaración).
        DonTopo::ScriptManager scriptManager;

        DonTopo::Scene scene;

        auto soldierMesh = std::make_shared<DonTopo::Mesh>(DonTopo::ModelLoader::load("assets/modelTexture.fbx"));
        auto modelMesh    = std::make_shared<DonTopo::Mesh>(DonTopo::ModelLoader::load("assets/model.fbx"));

        // Suelo (instancia de Plane), altura calculada a partir de soldier/model
        float floorY = std::numeric_limits<float>::max();
        for (auto* m : { soldierMesh.get(), modelMesh.get() })
            for (auto& v : m->vertices)
                floorY = std::min(floorY, v.pos.y);
        auto floorMesh = std::make_shared<DonTopo::Mesh>(DonTopo::Plane::create(1000.0f, floorY));

        // Cubo y esfera de prueba (sin textura -> placeholder checkerboard)
        auto cubeMesh   = std::make_shared<DonTopo::Mesh>(DonTopo::Cube::create(50.0f));
        auto sphereMesh = std::make_shared<DonTopo::Mesh>(DonTopo::Sphere::create(50.0f));

        // Cargar modelo animado antes de init
        auto soldierAnimMesh = std::make_shared<DonTopo::SkinnedMesh>(DonTopo::ModelLoader::loadSkinned("assets/modelAnimation.fbx"));

        auto* soldier = scene.addGameObject("soldier");
        soldier->setMesh(soldierMesh);

        auto* model = scene.addGameObject("model");
        model->setMesh(modelMesh);
        model->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(200.0f, 0.0f, 0.0f));

        auto* floorNode = scene.addGameObject("floor");
        floorNode->setMesh(floorMesh);

        glm::mat4 floorColliderPose = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, floorY - 0.5f, 0.0f));
        floorNode->setBoxCollider(physics.createBoxColliderComponent(
            glm::vec3(500.0f, 0.5f, 500.0f), glm::vec3(0.0f), floorColliderPose, /*useGravity=*/false));

        auto* cube = scene.addGameObject("cube");
        cube->setMesh(cubeMesh);
        cube->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 50.0f, -200.0f));

        cube->updateWorldTransforms();
        cube->setBoxCollider(physics.createBoxColliderComponent(
            glm::vec3(25.0f, 25.0f, 25.0f), glm::vec3(0.0f), cube->worldTransform, /*useGravity=*/true));

#ifdef DT_PHYSX_ENABLED
        {
            physx::PxRaycastBuffer hit;
            physx::PxVec3 origin(cube->worldTransform[3].x, cube->worldTransform[3].y + 200.0f, cube->worldTransform[3].z);
            physx::PxVec3 dir(0.0f, -1.0f, 0.0f);
            bool didHit = physics.raycast(origin, dir, 400.0f, hit);
            std::cout << "[PhysX smoke test] raycast al cubo: " << (didHit ? "HIT" : "MISS") << std::endl;
        }
#endif

        auto* sphere = scene.addGameObject("sphere");
        sphere->setMesh(sphereMesh);
        sphere->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 50.0f, 200.0f));

        auto* soldierAnim = scene.addGameObject("soldier_animado");
        soldierAnim->setMesh(soldierAnimMesh);
        soldierAnim->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(-200.0f, 0.0f, 0.0f));

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

        DonTopo::Camera camera({0.0f, 90.0f, 300.0f});

        //int bgm = audio.loadBGM("assets/audio.mp3");
        //if (bgm >= 0) audio.playBGM(bgm);

        renderer.init(window, meshes);
        renderer.setSceneRoot(&scene.getRoot());
        renderer.setScene(&scene);
        renderer.setPhysicsManager(&physics);
        renderer.setAudioManager(&audio);

        scriptManager.setScene(&scene);
        scriptManager.setPhysicsManager(&physics);
        scriptManager.setAudioManager(&audio);
        scriptManager.setLogCallback([](const std::string& msg) { std::cout << msg << std::endl; });
        scriptManager.setOnInstantiated([&renderer](DonTopo::GameObject* go) {
            go->traverse([&renderer](DonTopo::GameObject* n) {
                if (!n->hasMesh()) return;
                if (n->isSkinned()) n->skinnedRenderIndex = renderer.addSkinnedMesh(*n->getSkinnedMesh());
                else                n->staticRenderIndex  = renderer.addStaticMesh(*n->getMesh());
            });
        });
        scriptManager.setOnDestroying([&renderer](DonTopo::GameObject* go) {
            go->traverse([&renderer](DonTopo::GameObject* n) {
                if (n->hasMesh()) renderer.removeMeshComponent(n);
            });
        });
        scriptManager.init("Scripts");
        renderer.setScriptManager(&scriptManager);

        renderer.setOnAxisSelected([&camera](const glm::vec3& axis) { camera.lookAlongAxis(axis); });

        renderer.initSkybox({
            "assets/skybox/px.png",  // +X
            "assets/skybox/nx.png",  // -X
            "assets/skybox/py.png",  // +Y
            "assets/skybox/ny.png",  // -Y
            "assets/skybox/pz.png",  // +Z
            "assets/skybox/nz.png",  // -Z
        });

        // Pasada 2: meshes animados -> addSkinnedMesh (después de init, como requiere el Renderer)
        for (auto* go : allNodes)
        {
            if (go->hasMesh() && go->isSkinned())
                go->skinnedRenderIndex = renderer.addSkinnedMesh(*go->getSkinnedMesh());
        }

        renderer.setLights({
            { glm::vec4(0.0f, 500.0f, 300.0f, 1.0f),    glm::vec4(1.0f, 0.95f, 0.8f, 1.0f) },
            { glm::vec4(-300.0f, 200.0f, -200.0f, 1.0f), glm::vec4(0.4f, 0.5f, 1.0f, 0.8f) },
        });

        struct AppCtx { DonTopo::Camera* cam; DonTopo::Renderer* rnd; };
        AppCtx ctx{ &camera, &renderer };
        glfwSetWindowUserPointer(window.getNativeWindow(), &ctx);

        glfwSetFramebufferSizeCallback(window.getNativeWindow(), [](GLFWwindow* w, int, int) {
            static_cast<AppCtx*>(glfwGetWindowUserPointer(w))->rnd->notifyResize();
        });

        // Cámara: solo rota con botón derecho y cuando ImGui no captura el ratón
        glfwSetCursorPosCallback(window.getNativeWindow(), [](GLFWwindow* w, double x, double y) {
            ImGui_ImplGlfw_CursorPosCallback(w, x, y);
            static double lastX = x, lastY = y;
            double dx = x - lastX, dy = y - lastY;
            lastX = x; lastY = y;
            auto* ctx = static_cast<AppCtx*>(glfwGetWindowUserPointer(w));
            if (ctx->rnd->isViewportHovered() &&
                glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
            {
                ctx->cam->processMouse((float)dx, (float)dy);
            }
        });

        // Reenviar mouse buttons y scroll a ImGui
        glfwSetMouseButtonCallback(window.getNativeWindow(), [](GLFWwindow* w, int btn, int action, int mods) {
            ImGui_ImplGlfw_MouseButtonCallback(w, btn, action, mods);
        });
        glfwSetScrollCallback(window.getNativeWindow(), [](GLFWwindow* w, double xoff, double yoff) {
            ImGui_ImplGlfw_ScrollCallback(w, xoff, yoff);
        });
        glfwSetCharCallback(window.getNativeWindow(), [](GLFWwindow* w, unsigned int c) {
            ImGui_ImplGlfw_CharCallback(w, c);
        });

        glfwSetKeyCallback(window.getNativeWindow(), [](GLFWwindow* w, int key, int scancode, int action, int mods) {
            ImGui_ImplGlfw_KeyCallback(w, key, scancode, action, mods);
            if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
                glfwSetWindowShouldClose(w, GLFW_TRUE);
            if (key == GLFW_KEY_F && action == GLFW_PRESS && !ImGui::GetIO().WantTextInput)
            {
                auto* ctx = static_cast<AppCtx*>(glfwGetWindowUserPointer(w));
                ctx->rnd->focusSelected(*ctx->cam);
            }
        });

        while (!window.shouldClose())
        {
            DonTopo::Input::update();

            auto now = std::chrono::high_resolution_clock::now();
            static auto last = now;
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;

            if (renderer.isViewportHovered())
                camera.update(window.getNativeWindow(), dt);
            renderer.setCamera(camera);

            if (renderer.isPlaying())
            {
                audio.update(camera.getPos(), camera.getFront(), camera.getUp());
                physics.stepSimulation(dt);
                scene.update(dt, physics);
                scriptManager.update(dt);
            }
            else
            {
                // Sin física corriendo, pero los transforms padre→hijo se
                // siguen propagando: gizmo/Properties deben seguir
                // funcionando en Edit Mode. Scene::update también hace esto,
                // pero además impone la pose de PhysX sobre cada GameObject
                // con collider dinámico — justo lo que hace imposible editar
                // esos objetos hoy (la física los pelea cada frame). Al
                // saltarnos scene.update() entero en Edit Mode, ese pull ya
                // no ocurre.
                scene.getRoot().updateWorldTransforms();
            }

            // Recorrido en vivo (no la lista allNodes cacheada al arrancar): el
            // editor permite borrar GameObjects en tiempo real, así que un
            // puntero cacheado podría quedar colgante tras un delete.
            DonTopo::GameObject* liveCube = nullptr;
            scene.traverse([&](DonTopo::GameObject* go) {
                if (go == cube)
                    liveCube = go;

                if (go->staticRenderIndex >= 0)
                    renderer.setTransform(go->staticRenderIndex, go->worldTransform);

                if (go->skinnedRenderIndex >= 0)
                {
                    renderer.updateAnimation(go->skinnedRenderIndex, dt);
                    renderer.setSkinnedTransform(go->skinnedRenderIndex, go->worldTransform);
                }
            });

            // --- Gizmos: demo de depuración visual (bbox, ray, frustum) ---
            // Los ejes ya no se dibujan fijos aquí: EditorUI::drawSelectionGizmo()
            // los muestra automáticamente sobre cualquier GameObject seleccionado.
            // liveCube (capturado en el traverse de arriba, no el puntero `cube`
            // cacheado en el setup) evita un use-after-free si el usuario borró el
            // GameObject "cube" desde el editor: scene.traverse() solo visita nodos
            // vivos, así que liveCube queda nullptr ese frame en vez de colgante.
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

        // Libera explícitamente colliders/audioclips antes de destruir
        // physics/audio: sin esto, ~BoxCollider() intentaría release() un
        // PxRigidDynamic sobre una PxScene ya liberada (o ~AudioClipComponent
        // llamaría a un AudioManager ya destruido).
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
