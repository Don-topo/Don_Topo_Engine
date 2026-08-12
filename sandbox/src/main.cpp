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
#include "DonTopo/Editor/ProjectContext.h"
#include "DonTopo/Renderer/D3D12/D3D12Renderer.h"
#include "DonTopo/Core/Input.h"
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
#endif

int main()
{
    try {
        DonTopo::Engine engine;
        DonTopo::Window window;
        window.init(1280, 720, "Don Topo Engine", "assets/MainEngineLogo.png");
        DonTopo::Input::init(window.getNativeWindow());

        // Backend de render. Se decide AQUÍ, antes de crear NADA de GPU, porque
        // el device cuelga de él. El ajuste vive en el project.json, pero el
        // proyecto todavía no se ha elegido (su selector se dibuja con ImGui, o
        // sea con un Renderer ya en marcha): por eso se lee del último proyecto
        // abierto, que es lo que recuerda editor.json.
        //
        // resolveRenderBackend nunca falla: lo que no se pueda arrancar se cae a
        // Vulkan con un motivo.
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
        // Camino DirectX 12. Sale por aquí ANTES de construir el editor, la
        // física, el audio o la escena: nada de eso sabe todavía dibujarse con
        // este backend, y levantarlo para luego no usarlo solo serviría para
        // arrastrar dependencias de Vulkan a un proceso que no lo va a abrir.
        //
        // Este bucle es el esqueleto que las fases siguientes van llenando
        // hasta igualar al de Vulkan. Hoy presenta y procesa eventos.
        if (backend.backend == DonTopo::RenderBackend::D3D12)
        {
            std::cout << backend.message << std::endl;

            DonTopo::D3D12::D3D12Renderer d3d12;
            d3d12.init(window);
            std::cout << "D3D12: adaptador '" << d3d12.adapterName() << "'" << std::endl;

            // La escena del proyecto, cargada con el MISMO Scene::load que usa
            // el editor. La geometría entra por la API pública del backend,
            // igual que hace el editor con el Renderer de Vulkan.
            //
            // El orden de declaración está calcado del camino de Vulkan y por
            // los mismos motivos: los colliders liberan actores sobre la
            // PxScene y los AudioClip sueltan canales, así que la escena tiene
            // que destruirse ANTES que physics y audio.
            DonTopo::PhysicsManager d3dPhysics;
            d3dPhysics.init();
            DonTopo::AudioManager d3dAudio;
            d3dAudio.init();
            DonTopo::Scene d3dScene;

            {
                const std::filesystem::path sceneFile =
                    lastProject.empty() ? std::filesystem::path{}
                                        : lastProject / "scenes" / "main.json";

                bool loaded = false;
                if (!sceneFile.empty() && std::filesystem::exists(sceneFile))
                    loaded = d3dScene.load(sceneFile.string(), d3dPhysics, d3dAudio);

                if (loaded)
                {
                    // Mismo criterio que el editor: cada nodo con malla estática
                    // se sube y se queda con su índice de render, para poder
                    // moverlo después.
                    // Las matrices de mundo se derivan de la jerarquía una vez
                    // cargada: sin esto, un hijo se subiría con la
                    // transformación de su padre sin aplicar.
                    d3dScene.getRoot().updateWorldTransforms();

                    int added        = 0;
                    int addedSkinned = 0;
                    d3dScene.traverse([&](DonTopo::GameObject* node) {
                        if (node == nullptr || !node->hasMesh())
                            return;

                        if (node->isSkinned())
                        {
                            const DonTopo::SkinnedMesh* skinned = node->getSkinnedMesh();
                            if (!skinned)
                                return;
                            const int index = d3d12.addSkinnedMesh(*skinned);
                            if (index < 0)
                                return;
                            node->skinnedRenderIndex = index;
                            d3d12.setSkinnedTransform(static_cast<size_t>(index),
                                                      node->worldTransform);
                            ++addedSkinned;
                            return;
                        }

                        const std::shared_ptr<DonTopo::Mesh> mesh = node->getMesh();
                        if (!mesh)
                            return;
                        const int index = d3d12.addStaticMesh(*mesh);
                        if (index < 0)
                            return;
                        node->staticRenderIndex = index;
                        d3d12.setTransform(static_cast<size_t>(index), node->worldTransform);
                        ++added;
                    });
                    std::cout << "D3D12: escena '" << sceneFile.string() << "' cargada, "
                              << added << " mallas estaticas y " << addedSkinned
                              << " personajes" << std::endl;

                    // Sin personajes en la escena se carga el de pruebas del
                    // repo: el skinning por compute es de lo poco que no se ve
                    // en una escena estática, y dejarlo sin nada que enseñar
                    // haría pasar por rota una ruta que funciona.
                    if (addedSkinned == 0)
                    {
                        try {
                            const DonTopo::SkinnedMesh demo = DonTopo::ModelLoader::loadSkinned(
                                "assets/animatedCharacter/Maw J Laygo.fbx");
                            const int index = d3d12.addSkinnedMesh(demo);
                            if (index >= 0)
                            {
                                // El FBX viene en centímetros: sin reescalar
                                // mediría cien veces la rejilla.
                                d3d12.setSkinnedTransform(
                                    static_cast<size_t>(index),
                                    glm::scale(glm::translate(glm::mat4(1.0f),
                                                              glm::vec3(-3.0f, 0.0f, 0.0f)),
                                               glm::vec3(0.02f)));
                            }
                        } catch (const std::exception&) {
                            // El repo puede no traer el FBX: no es motivo para
                            // tumbar el arranque.
                        }
                    }
                }
                else
                {
                    // Sin proyecto o sin escena: unas cuantas cajas para que el
                    // backend tenga algo que enseñar en vez de un suelo vacío.
                    const DonTopo::Mesh cube = DonTopo::Cube::create(2.0f);
                    const struct { glm::vec3 pos; float scale; } layout[] = {
                        {{0.0f, 1.0f, 0.0f}, 1.0f},
                        {{4.0f, 0.6f, -2.0f}, 0.6f},
                        {{-4.5f, 1.6f, 1.0f}, 1.6f},
                        {{2.0f, 0.4f, 4.0f}, 0.4f},
                    };
                    for (const auto& entry : layout)
                    {
                        const int index = d3d12.addStaticMesh(cube);
                        if (index < 0)
                            continue;
                        d3d12.setTransform(
                            (size_t)index,
                            glm::scale(glm::translate(glm::mat4(1.0f), entry.pos),
                                       glm::vec3(entry.scale)));
                    }
                    std::cout << "D3D12: sin escena de proyecto, " << d3d12.objectCount()
                              << " cajas de muestra" << std::endl;
                }
            }

            glfwSetWindowUserPointer(window.getNativeWindow(), &d3d12);
            glfwSetFramebufferSizeCallback(
                window.getNativeWindow(), [](GLFWwindow* w, int width, int height) {
                    auto* r = static_cast<DonTopo::D3D12::D3D12Renderer*>(glfwGetWindowUserPointer(w));
                    if (r)
                        r->resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
                });

            // ImGui sobre DX12. El backend de render no lo conoce —DonTopoCore
            // no puede depender de ImGui porque el runtime exportado lo enlaza—,
            // así que se monta aquí con los handles que expone.
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGui::StyleColorsDark();
            ImGui_ImplGlfw_InitForOther(window.getNativeWindow(), true);

            // Reparto del rango de descriptores que el renderer tiene apartado.
            // ImGui pide y devuelve por su cuenta, así que hace falta una lista
            // de libres y no un simple contador.
            struct UiDescriptorPool
            {
                uint64_t              cpuStart = 0;
                uint64_t              gpuStart = 0;
                unsigned              stride   = 0;
                unsigned              capacity = 0;
                unsigned              next     = 0;
                std::vector<unsigned> released;
            } uiPool;
            uiPool.cpuStart = d3d12.uiHeapStartCpu();
            uiPool.gpuStart = d3d12.uiHeapStartGpu();
            uiPool.stride   = d3d12.descriptorSize();
            uiPool.capacity = d3d12.uiDescriptorCount();

            ImGui_ImplDX12_InitInfo uiInit = {};
            uiInit.Device            = static_cast<ID3D12Device*>(d3d12.nativeDevice());
            uiInit.CommandQueue      = static_cast<ID3D12CommandQueue*>(d3d12.nativeQueue());
            uiInit.NumFramesInFlight = d3d12.framesInFlight();
            uiInit.RTVFormat         = DXGI_FORMAT_R8G8B8A8_UNORM;
            uiInit.DSVFormat         = DXGI_FORMAT_UNKNOWN;
            uiInit.SrvDescriptorHeap = static_cast<ID3D12DescriptorHeap*>(d3d12.uiDescriptorHeap());
            uiInit.UserData          = &uiPool;
            uiInit.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info,
                                             D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
                                             D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
                auto*    pool  = static_cast<UiDescriptorPool*>(info->UserData);
                unsigned index = 0;
                if (!pool->released.empty()) {
                    index = pool->released.back();
                    pool->released.pop_back();
                } else {
                    // Quedarse sin sitio aquí sería un fallo silencioso que
                    // acabaría pisando descriptores de la escena.
                    if (pool->next >= pool->capacity)
                        throw std::runtime_error(
                            "D3D12: ImGui pidio mas descriptores de los reservados");
                    index = pool->next++;
                }
                outCpu->ptr = pool->cpuStart + static_cast<uint64_t>(index) * pool->stride;
                outGpu->ptr = pool->gpuStart + static_cast<uint64_t>(index) * pool->stride;
            };
            uiInit.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* info,
                                            D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                            D3D12_GPU_DESCRIPTOR_HANDLE) {
                auto* pool = static_cast<UiDescriptorPool*>(info->UserData);
                if (pool->stride == 0 || cpu.ptr < pool->cpuStart)
                    return;
                pool->released.push_back(
                    static_cast<unsigned>((cpu.ptr - pool->cpuStart) / pool->stride));
            };
            ImGui_ImplDX12_Init(&uiInit);

            // Estado del panel. Arranca en DirectX 12 porque es con lo que se
            // ha entrado.
            int  uiBackendChoice = 1;
            bool uiSavePending   = false;
            std::string uiSaveResult;

            // Cámara navegable: WASD/QE para moverse y boton derecho para
            // mirar, igual que el viewport del editor. Sale de (6, 4.5, 8)
            // mirando al origen, que es el encuadre fijo que tenia el backend.
            DonTopo::Camera d3dCamera(glm::vec3(6.0f, 4.5f, 8.0f), -126.87f, -21.8f);
            d3dCamera.moveSpeed = 8.0f;  // la rejilla mide 20: 50 la cruza en medio segundo
            d3d12.setCamera(d3dCamera.getViewMatrix(), d3dCamera.getPos(), d3dCamera.getFov());

            double d3dLastX = 0.0, d3dLastY = 0.0;
            bool   d3dLooking = false;
            auto   d3dLastFrame = std::chrono::high_resolution_clock::now();

            d3d12.setUiDrawCallback([&]() {
                ImGui_ImplDX12_RenderDrawData(
                    ImGui::GetDrawData(),
                    static_cast<ID3D12GraphicsCommandList*>(d3d12.nativeCommandList()));
            });

            while (!window.shouldClose())
            {
                window.pollEvents();

                const auto  d3dNow = std::chrono::high_resolution_clock::now();
                const float d3dDelta =
                    std::chrono::duration<float>(d3dNow - d3dLastFrame).count();
                d3dLastFrame = d3dNow;

                ImGui_ImplDX12_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();

                ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize(ImVec2(430.0f, 0.0f), ImGuiCond_FirstUseEver);
                if (ImGui::Begin("Backend DirectX 12"))
                {
                    ImGui::Text("Adaptador: %s", d3d12.adapterName().c_str());
                    ImGui::Text("%.1f FPS (%.3f ms/frame)", ImGui::GetIO().Framerate,
                                1000.0f / ImGui::GetIO().Framerate);
                    ImGui::Separator();

                    ImGui::TextWrapped(
                        "Este backend dibuja la escena del proyecto, no el editor. Los "
                        "paneles, la jerarquia, el inspector y el viewport solo existen con "
                        "Vulkan.");
                    ImGui::Spacing();
                    ImGui::TextWrapped(
                        "Implementado: presentacion, escena del proyecto con sus texturas, "
                        "personajes animados por compute, iluminacion directa, profundidad, "
                        "sombras en cascada, cielo, IBL, materiales PBR, niebla, bloom con "
                        "tone mapping y FXAA.");
                    ImGui::TextWrapped(
                        "Sin implementar: SSAO, SSR, TAA, MSAA, Forward+, gizmos, UI 2D y el "
                        "editor completo.");
                    ImGui::Spacing();
                    ImGui::TextWrapped(
                        "Camara: WASD para moverse, Q/E para bajar y subir, boton derecho "
                        "para mirar.");

                    ImGui::Separator();
                    ImGui::TextWrapped(
                        "Cambiar de backend requiere reiniciar. Este selector existe porque "
                        "sin el no habria forma de volver a Vulkan: el combo del menu View "
                        "vive en el editor, y el editor no se dibuja aqui.");

                    const char* backendNames[] = {"Vulkan", "DirectX 12"};
                    ImGui::SetNextItemWidth(160.0f);
                    ImGui::Combo("Backend al reiniciar", &uiBackendChoice, backendNames,
                                 IM_ARRAYSIZE(backendNames));

                    if (ImGui::Button("Guardar en el proyecto", ImVec2(190.0f, 0.0f)))
                        uiSavePending = true;

                    if (!uiSaveResult.empty())
                        ImGui::TextWrapped("%s", uiSaveResult.c_str());
                }
                ImGui::End();

                if (uiSavePending)
                {
                    uiSavePending = false;
                    // Se toca SOLO el campo del backend, releyendo y reescribiendo
                    // el project.json tal cual: pasar por readSettings/writeSettings
                    // reescribiria la seccion entera y un proyecto con ajustes a
                    // medias perderia los que no estan en el fichero.
                    if (lastProject.empty())
                    {
                        uiSaveResult = "No hay proyecto recordado: abre uno con Vulkan primero.";
                    }
                    else
                    {
                        const std::filesystem::path file = lastProject / "project.json";
                        nlohmann::json               doc = nlohmann::json::object();
                        {
                            std::ifstream in(file);
                            if (in.is_open())
                            {
                                try { in >> doc; } catch (const std::exception&) { doc = nlohmann::json::object(); }
                            }
                        }
                        if (!doc.is_object())
                            doc = nlohmann::json::object();
                        if (!doc.contains("settings") || !doc["settings"].is_object())
                            doc["settings"] = nlohmann::json::object();
                        doc["settings"]["renderBackend"] =
                            DonTopo::renderBackendName(uiBackendChoice == 0
                                                           ? DonTopo::RenderBackend::Vulkan
                                                           : DonTopo::RenderBackend::D3D12);

                        std::ofstream out(file, std::ios::binary | std::ios::trunc);
                        if (out.is_open())
                        {
                            out << doc.dump(4);
                            out.flush();
                            uiSaveResult = out.good()
                                               ? "Guardado. Cierra y vuelve a abrir para aplicarlo."
                                               : "No se pudo escribir el project.json.";
                        }
                        else
                        {
                            uiSaveResult = "No se pudo abrir el project.json para escribir.";
                        }
                    }
                }

                ImGui::Render();

                // La cámara despues de la UI: si el raton o el teclado los
                // tiene ImGui, arrastrar por un panel no puede girar la vista.
                {
                    GLFWwindow*    native = window.getNativeWindow();
                    const ImGuiIO& io     = ImGui::GetIO();

                    const bool rightDown =
                        !io.WantCaptureMouse &&
                        glfwGetMouseButton(native, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

                    double mouseX = 0.0, mouseY = 0.0;
                    glfwGetCursorPos(native, &mouseX, &mouseY);

                    if (rightDown)
                    {
                        // El primer frame solo ancla la posicion: sin esto el
                        // delta seria la distancia desde donde estuviera el
                        // cursor la ultima vez y la vista daria un salto.
                        if (d3dLooking)
                            d3dCamera.processMouse(static_cast<float>(mouseX - d3dLastX),
                                                   static_cast<float>(mouseY - d3dLastY));
                        d3dLooking = true;
                    }
                    else
                    {
                        d3dLooking = false;
                    }
                    d3dLastX = mouseX;
                    d3dLastY = mouseY;

                    if (!io.WantCaptureKeyboard)
                        d3dCamera.update(native, d3dDelta);

                    d3d12.setCamera(d3dCamera.getViewMatrix(), d3dCamera.getPos(),
                                    d3dCamera.getFov());
                }

                d3d12.drawFrame();
            }

            // ORDEN CRÍTICO. La GPU sigue con el último frame en vuelo al salir
            // del bucle, y ese frame usa los buffers y la textura de ImGui.
            // Apagar ImGui sin esperar antes se los quita a la GPU debajo, y el
            // proceso muere al cerrar —con volcado de WER, pero sin que nada en
            // pantalla lo delate, porque ya no queda frame que dibujar.
            d3d12.waitIdle();
            ImGui_ImplDX12_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            d3d12.shutdown();
            return 0;
        }
#endif

        // El editor es el dueño del Renderer: se declara él y el resto del
        // main sigue trabajando contra la referencia, igual que antes.
        DonTopo::EditorUI  editor;
        DonTopo::Renderer& renderer = editor.renderer();

        editor.setActiveRenderBackend(backend.backend);
        if (!backend.message.empty())
            editor.pushExternalLog(backend.message);

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

        // Escena de arranque: solo lo que necesita el arranque, nada más.
        // renderer.init() exige un `meshes` no vacío —de ahí saca el bbox del
        // auto-fit, y con la lista vacía saldría en infinitos—, así que basta un
        // cubo procedural, que no toca disco. Los tres FBX que cargaba la demo
        // (modelTexture, model y modelAnimation) eran todo el coste de arranque
        // y ya no se cargan: en cuanto se elige proyecto esta escena se
        // reemplaza entera por la del proyecto (EditorUI::openProjectScene), así
        // que cargarlos era trabajo que se tiraba a la basura.
        // El tamaño de este suelo NO es decorativo: renderer.init() saca de él
        // m_cameraDistance (= maxDim * 1.2) y de ahí salen el near y el far de la
        // proyección del editor (near = d*0.001, far = d*3). Con solo el cubo de
        // 50 el far caía a ~180 y, con la cámara en z=300, el skybox se recortaba.
        // Los 1000 son los mismos que tenía el suelo de la demo, así que el
        // encuadre y el skybox se ven igual que antes.
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

        // ─── Selector de proyecto ────────────────────────────────────────────
        // Primer estado del bucle de ImGui que ya existe: misma ventana, mismo
        // device Vulkan y misma sesión de ImGui. Mientras esté activo, el bucle
        // de abajo se salta TODO (scripts, cámara, escena, luces, UI) y solo
        // presenta el frame del selector, así que el editor no aparece hasta
        // elegir proyecto. `project` vive aquí, fuera del bucle: es lo que los
        // paneles consultan durante toda la sesión.
        DonTopo::ProjectContext project;
        // Estado del selector: vive fuera del lambda (que lo captura por
        // referencia) porque tiene que sobrevivir de un frame al siguiente.
        std::vector<std::filesystem::path> projectEntries = DonTopo::ProjectContext::discover();
        int                                projectPicked  = -1;
        bool                               projectCreating = false;
        char                               projectNewName[64] = {};
        std::string                        projectCreateError;

        editor.setProjectSelector([&]() -> bool {
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->WorkPos);
            ImGui::SetNextWindowSize(vp->WorkSize);
            ImGui::Begin("##ProjectSelector", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

            ImGui::TextUnformatted("Don Topo Engine - Proyectos");
            ImGui::TextDisabled("%s", DonTopo::ProjectContext::workspaceDir().string().c_str());
            ImGui::Separator();

            bool                  confirmed = false;
            std::filesystem::path chosen;

            ImGui::BeginChild("##ProjectList", ImVec2(0.0f, vp->WorkSize.y * 0.55f), true);
            if (projectEntries.empty())
                ImGui::TextDisabled("No hay ningun proyecto todavia: crea uno con 'Nuevo proyecto...'.");
            for (int i = 0; i < (int)projectEntries.size(); ++i)
            {
                const std::string label = DonTopo::ProjectContext::readProjectName(projectEntries[i]) +
                                          "###project" + std::to_string(i);
                if (ImGui::Selectable(label.c_str(), projectPicked == i,
                                      ImGuiSelectableFlags_AllowDoubleClick))
                {
                    projectPicked = i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        confirmed = true;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%s", projectEntries[i].filename().string().c_str());
            }
            ImGui::EndChild();

            ImGui::BeginDisabled(projectPicked < 0);
            if (ImGui::Button("Abrir proyecto", ImVec2(160.0f, 0.0f)))
                confirmed = true;
            ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button("Nuevo proyecto...", ImVec2(160.0f, 0.0f)))
            {
                projectCreating    = true;
                projectNewName[0]  = '\0';
                projectCreateError.clear();
                ImGui::OpenPopup("Crear proyecto");
            }

            if (ImGui::BeginPopupModal("Crear proyecto", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextUnformatted("Nombre del proyecto:");
                ImGui::SetNextItemWidth(320.0f);
                const bool enter = ImGui::InputText("##NewProjectName", projectNewName,
                                                    sizeof(projectNewName),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);

                // El error se enseña AQUÍ, en el diálogo, y no se crea nada:
                // nombre repetido (aunque cambien las mayúsculas), vacío, `..`,
                // separadores de ruta o caracteres inválidos en Windows.
                if (!projectCreateError.empty())
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", projectCreateError.c_str());

                if (ImGui::Button("Crear", ImVec2(120.0f, 0.0f)) || enter)
                {
                    std::filesystem::path created;
                    if (DonTopo::ProjectContext::create(projectNewName, created, projectCreateError))
                    {
                        // Refresca la lista y deja el proyecto nuevo seleccionado.
                        projectEntries = DonTopo::ProjectContext::discover();
                        projectPicked  = -1;
                        for (int i = 0; i < (int)projectEntries.size(); ++i)
                            if (projectEntries[i] == created)
                                projectPicked = i;
                        projectCreating = false;
                        projectCreateError.clear();
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancelar", ImVec2(120.0f, 0.0f)))
                {
                    projectCreating = false;
                    projectCreateError.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            if (confirmed && projectPicked >= 0 && projectPicked < (int)projectEntries.size())
                chosen = projectEntries[projectPicked];

            ImGui::End();

            if (chosen.empty())
                return false;

            project = DonTopo::ProjectContext(chosen);
            if (!project.valid())
                return false;

            // A partir de aquí el editor arranca exactamente como siempre, ya
            // con el proyecto puesto: es lo único que cambia respecto a antes.
            editor.setProject(&project);
            // Se recuerda para el PRÓXIMO arranque: es de este project.json de
            // donde saldrá el backend de render. Si falla no se aborta nada —el
            // único efecto es volver a arrancar en Vulkan.
            DonTopo::ProjectContext::writeLastProject(chosen);
            // Y con SU escena, no con la de demo que se montó en el arranque:
            // en un proyecto recién creado está vacía, así que se entra viendo
            // solo el skybox.
            editor.openProjectScene();
            return true;
        });

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

            // Selector de proyecto activo: el frame se limita a presentarlo. Ni
            // scripts, ni cámara, ni escena, ni luces, ni UI — nada de lo de
            // abajo corre hasta que hay proyecto elegido.
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
