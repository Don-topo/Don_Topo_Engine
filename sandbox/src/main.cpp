#include "DonTopo/Core/Engine.h"
#include "DonTopo/Core/Window.h"
#include "DonTopo/Renderer/Renderer.h"
#include "DonTopo/Renderer/ModelLoader.h"
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
#include "DonTopo/Editor/EditorUI.h"
#include "DonTopo/Core/Input.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <chrono>
#include <filesystem>
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
        // El editor es el dueño del Renderer: se declara él y el resto del
        // main sigue trabajando contra la referencia, igual que antes.
        DonTopo::EditorUI  editor;
        DonTopo::Renderer& renderer = editor.renderer();

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
            glm::vec3(500.0f, 0.5f, 500.0f), glm::vec3(0.0f), floorColliderPose, /*dynamic=*/false));

        auto* cube = scene.addGameObject("cube");
        cube->setMesh(cubeMesh);
        cube->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 50.0f, -200.0f));

        cube->updateWorldTransforms();
        cube->setBoxCollider(physics.createBoxColliderComponent(
            glm::vec3(25.0f, 25.0f, 25.0f), glm::vec3(0.0f), cube->worldTransform, /*dynamic=*/true));
        // Rigidbody: hace del cubo un cuerpo simulado (cae con la gravedad de la
        // escena). Sin Rigidbody el collider sería static y no caería.
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
        scriptManager.setOnDestroying([&renderer, &editor](DonTopo::GameObject* go) {
            // Suelta la selección del editor si apunta a go o su subtree ANTES de
            // liberar nada — si no, m_selected queda colgando y el editor crashea
            // al dibujar Properties/gizmo el frame siguiente.
            editor.onGameObjectDestroyed(go);
            // Libera GPU del subtree completo (estático + skinned).
            renderer.removeGameObject(go);
        });
        // Scripts/ vive en la raíz del repo y NO se copia junto al exe
        // (a diferencia de assets/shaders): el hot reload debe vigilar los
        // .lua originales que edita el usuario, no una copia que cada build
        // pisaría. Como el exe corre con CWD en build-ninja/sandbox, se
        // busca la carpeta hacia arriba desde el directorio actual.
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

        // Pasada 2: meshes animados -> addSkinnedMesh (después de init, como requiere el Renderer)
        for (auto* go : allNodes)
        {
            if (go->hasMesh() && go->isSkinned())
                go->skinnedRenderIndex = renderer.addSkinnedMesh(*go->getSkinnedMesh());
        }

        // Luces por defecto: las que se usan cuando la escena abierta no tiene
        // ni un LightComponent. Las escenas del repo son de antes de que
        // existiera el componente y sin esto el editor abriría a oscuras; en
        // cuanto la escena aporta UNA luz, estas desaparecen y manda la escena.
        const std::vector<DonTopo::Light> defaultLights = {
            { glm::vec4(0.0f, 500.0f, 300.0f, 1.0f),     glm::vec4(1.0f, 0.95f, 0.8f, 1.0f) },
            { glm::vec4(-300.0f, 200.0f, -200.0f, 1.0f), glm::vec4(0.4f, 0.5f, 1.0f, 0.8f) },
        };
        std::vector<DonTopo::Light> frameLights;
        std::vector<float>          frameLightRadii;
        renderer.setLights(defaultLights);

        struct AppCtx { DonTopo::Camera* cam; DonTopo::Renderer* rnd; DonTopo::EditorUI* ed; };
        AppCtx ctx{ &camera, &renderer, &editor };
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
            if (ctx->ed->isViewportHovered() &&
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
                ctx->ed->focusSelected(*ctx->cam);
            }
        });

        // JobSystem + loader asíncrono de assets. Se crean tras todo el setup y
        // ANTES del bucle: el drop de FBX y Load Scene encolan aquí, y el pump
        // por frame (más abajo) drena los resultados. El shutdown del JobSystem
        // va lo PRIMERO al salir del bucle (join de los workers antes de destruir
        // Renderer/Scene) — ver el bloque de apagado.
        DonTopo::JobSystem        jobSystem;
        jobSystem.start();
        DonTopo::AsyncAssetLoader assetLoader(jobSystem);
        editor.setAssetLoader(&assetLoader);

        // Estado del sync de widgets: vive FUERA del bucle porque es lo que
        // permite actualizar los botones en sitio (sin recrear el árbol, que
        // reiniciaría el fundido) y cachear atlas y fuentes por ruta.
        DonTopo::UiWidgetSyncCache uiWidgetCache;
        std::vector<std::pair<uint64_t, const DonTopo::ButtonComponent*>> uiButtons;
        std::vector<std::pair<uint64_t, const DonTopo::TextComponent*>> uiTexts;
        std::vector<std::pair<uint64_t, const DonTopo::ProgressBarComponent*>> uiBars;

        while (!window.shouldClose())
        {
            DonTopo::Input::update();
            scriptManager.pollChanges();

            auto now = std::chrono::high_resolution_clock::now();
            static auto last = now;
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;

            if (editor.isViewportHovered())
                camera.update(window.getNativeWindow(), dt);
            renderer.setCamera(camera);

            if (renderer.isPlaying())
            {
                // Audio Listener: si la escena tiene uno (y está habilitado), el
                // audio 3D se oye desde ÉL y no desde la cámara del editor.
                // Posición = columna 3 del worldTransform, forward = -Z local,
                // up = +Y local (misma convención que la cámara y el runtime);
                // con la base degenerada (escala 0) se cae a la cámara en vez de
                // colar un NaN en FMOD, del que ya no se recupera.
                glm::vec3 listenerPos = camera.getPos();
                glm::vec3 listenerFwd = camera.getFront();
                glm::vec3 listenerUp  = camera.getUp();
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
                audio.update(listenerPos, listenerFwd, listenerUp);
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

            // Pump por frame de la carga asíncrona, ANTES del traverse: los
            // objetos cuyo mesh acaba de llegar tienen que estar ya registrados
            // en el Renderer cuando se recorra la escena para empujar transforms.
            // tickDeferredDeletes primero (libera lo borrado); onAssetsLoaded
            // aplica los resultados y cierra el batch con UN solo
            // flushPendingUploads (así ~440 vkQueueWaitIdle se vuelven uno).
            renderer.tickDeferredDeletes();
            editor.onAssetsLoaded(assetLoader.pumpCompleted(2.0f), scene, renderer);

            // Recorrido en vivo (no la lista allNodes cacheada al arrancar): el
            // editor permite borrar GameObjects en tiempo real, así que un
            // puntero cacheado podría quedar colgante tras un delete.
            DonTopo::GameObject* liveCube = nullptr;
            scene.traverse([&](DonTopo::GameObject* go) {
                if (go == cube)
                    liveCube = go;

                if (go->staticRenderIndex >= 0)
                {
                    renderer.setTransform(go->staticRenderIndex, go->worldTransform);
                    // Mismo sitio que el transform: es la única sincronización
                    // por frame que ya cubre Play Mode, Undo/Redo y la carga de
                    // escena sin caminos propios.
                    renderer.setObjectSsr(go->staticRenderIndex,
                                          go->ssrEnabled ? go->ssrIntensity : 0.0f);
                    renderer.setObjectMeshVisible(go->staticRenderIndex, go->meshVisible);
                }

                if (go->skinnedRenderIndex >= 0)
                {
                    // Antes de tocar la animación: updateAnimation congela el
                    // reloj de un mesh oculto, así que el flag tiene que estar ya
                    // puesto o iría un frame por detrás.
                    renderer.setSkinnedMeshVisible(go->skinnedRenderIndex, go->meshVisible);
                    if (const auto& anim = go->getAnimator())
                    {
                        // El Animator es el único dueño de animTime: calcula en
                        // CPU y el Renderer solo recibe el resultado. En Edit el
                        // grafo no evalúa transiciones (solo avanza el tiempo del
                        // estado de entrada); si no, las condiciones "animation
                        // finished" pasearían el grafo solo en el editor.
                        anim->update(dt, renderer.isPlaying());
                        renderer.setAnimationState(go->skinnedRenderIndex,
                                                    (uint32_t)anim->currentClipIndex(),
                                                    anim->animTime());
                    }
                    else
                    {
                        // Sin Animator: clip 0 en bucle, exactamente como antes
                        // de que existiera el componente. Los dos caminos no se
                        // pisan.
                        renderer.updateAnimation(go->skinnedRenderIndex, dt);
                    }
                    renderer.setSkinnedTransform(go->skinnedRenderIndex, go->worldTransform);
                    renderer.setSkinnedSsr(go->skinnedRenderIndex,
                                           go->ssrEnabled ? go->ssrIntensity : 0.0f);
                }
            });

            // Luces de la escena, después del traverse: los worldTransform del
            // frame ya están propagados y de ellos salen posición y dirección.
            // Va por frame y no en un evento porque mover el GameObject de una
            // luz tiene que moverla en el acto, igual que el transform de una
            // malla.
            if (scene.collectLights(frameLights, frameLightRadii) > 0)
            {
                renderer.setLights(frameLights);
                renderer.setLightRadii(frameLightRadii);
            }
            else
            {
                renderer.setLights(defaultLights);
                renderer.setLightRadii({});
            }

            // Canvas de UI: la resolución sale del COMPONENTE de la escena, no
            // de un canvas cableado aquí. Va por frame y no en un evento porque
            // tocar un campo en Properties tiene que verse en el acto — también
            // fuera de Play, que es cuando se pinta el gizmo del área útil.
            if (DonTopo::GameObject* canvasGo = scene.findCanvas())
                canvasGo->getCanvas()->applyTo(renderer.uiCanvas());

            // Widgets: los ButtonComponent de la escena se vuelcan en el árbol
            // vivo del canvas, por frame y por la misma razón que la resolución.
            // Sin Canvas no hay UI: la lista va vacía y el sync limpia el árbol.
            uiButtons.clear();
            uiTexts.clear();
            uiBars.clear();
            if (scene.findCanvas())
                scene.traverse([&](DonTopo::GameObject* n) {
                    if (n->hasButton()) uiButtons.emplace_back(n->id, n->getButton().get());
                    if (n->hasText())   uiTexts.emplace_back(n->id, n->getText().get());
                    if (n->hasProgressBar())
                        uiBars.emplace_back(n->id, n->getProgressBar().get());
                });
            // UN solo sync para todos los widgets: es el dueño de la raíz del
            // canvas, que se reconstruye entera con clearChildren().
            DonTopo::syncUiWidgets(uiButtons, uiTexts, uiBars, renderer.uiCanvas(),
                                   uiWidgetCache, renderer);

            // Input de la UI: sin esto el árbol no resuelve estados, así que los
            // cinco colores del botón, el fundido y el Click no harían nada.
            // El RATÓN solo entra en Play (como en Unity: en edición un botón no
            // se ilumina al pasarle por encima), pero el tiempo entra siempre —
            // así el estado Normal se aplica en cuanto se edita su color y se ve
            // sin darle a Play. La imagen del viewport se dibuja 1:1 con el
            // render, así que restarle su esquina al ratón ya da el píxel del
            // canvas, sin escalar nada.
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
                }
                else
                {
                    // Fuera de todo el canvas: ningún botón queda en Hover.
                    uiInput.mousePos = glm::vec2(-1.0f, -1.0f);
                }
                uiInput.timeSeconds = (float)glfwGetTime();
                renderer.uiCanvas().updateInput(uiInput);
            }

            // --- Gizmos: demo de depuración visual (bbox, ray, frustum) ---
            // Los ejes ya no se dibujan fijos aquí: ViewportPanel::drawSelectionGizmo()
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

        // PRIMERO el JobSystem: si se destruyera después del Renderer/Scene, un
        // worker aún en vuelo (un ReadFile de FBX a medias) podría tocar memoria
        // ya liberada. El join de shutdown() garantiza que ningún hilo sigue vivo
        // antes de empezar a destruir el resto.
        jobSystem.shutdown();

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
