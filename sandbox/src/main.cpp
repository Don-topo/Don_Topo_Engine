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
#endif

namespace {

// ─── Selector de proyecto ────────────────────────────────────────────────────
// Lo usan los dos backends: es UI de ImGui pura, no sabe con qué se dibuja.

// Estado de la pantalla. Vive fuera de la función porque tiene que sobrevivir
// de un frame al siguiente: qué proyecto está marcado y qué se escribió en el
// diálogo de crear.
struct ProjectSelectorState {
    std::vector<std::filesystem::path> entries = DonTopo::ProjectContext::discover();
    int                                picked  = -1;
    char                               newName[64] = {};
    std::string                        createError;
};

// Dibuja la pantalla y devuelve el proyecto elegido, o una ruta vacía mientras
// no se haya elegido ninguno.
std::filesystem::path drawProjectSelector(ProjectSelectorState& st)
{
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
    if (st.entries.empty())
        ImGui::TextDisabled("No hay ningun proyecto todavia: crea uno con 'Nuevo proyecto...'.");
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
    if (ImGui::Button("Abrir proyecto", ImVec2(160.0f, 0.0f)))
        confirmed = true;
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Nuevo proyecto...", ImVec2(160.0f, 0.0f)))
    {
        st.newName[0] = '\0';
        st.createError.clear();
        ImGui::OpenPopup("Crear proyecto");
    }

    if (ImGui::BeginPopupModal("Crear proyecto", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Nombre del proyecto:");
        ImGui::SetNextItemWidth(320.0f);
        const bool enter = ImGui::InputText("##NewProjectName", st.newName, sizeof(st.newName),
                                            ImGuiInputTextFlags_EnterReturnsTrue);

        // El error se enseña AQUÍ, en el diálogo, y no se crea nada: nombre
        // repetido (aunque cambien las mayúsculas), vacío, `..`, separadores de
        // ruta o caracteres inválidos en Windows.
        if (!st.createError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", st.createError.c_str());

        if (ImGui::Button("Crear", ImVec2(120.0f, 0.0f)) || enter)
        {
            std::filesystem::path created;
            if (DonTopo::ProjectContext::create(st.newName, created, st.createError))
            {
                // Refresca la lista y deja el proyecto nuevo seleccionado.
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
        if (ImGui::Button("Cancelar", ImVec2(120.0f, 0.0f)))
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

// Deja el editor con ese proyecto abierto. false = el proyecto no valía y el
// selector sigue en pantalla.
bool openChosenProject(DonTopo::EditorUI& editor, DonTopo::ProjectContext& project,
                       const std::filesystem::path& chosen)
{
    project = DonTopo::ProjectContext(chosen);
    if (!project.valid())
        return false;

    editor.setProject(&project);
    // Se recuerda para el PRÓXIMO arranque: es de este project.json de donde
    // saldrá el backend de render. Si falla no se aborta nada —el único efecto
    // es volver a arrancar en Vulkan.
    DonTopo::ProjectContext::writeLastProject(chosen);
    // Y con SU escena, no con la que se montó al arrancar: en un proyecto
    // recién creado está vacía, así que se entra viendo solo el skybox.
    editor.openProjectScene();
    return true;
}

}  // namespace

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

            // El backend lo construye main y la propiedad acaba en el editor;
            // la referencia al tipo concreto se guarda antes de moverlo, porque
            // el ciclo de vida (init, drawFrame, shutdown) no está en la
            // interfaz que consume el editor.
            auto d3d12Owned = std::make_unique<DonTopo::D3D12::D3D12Renderer>();
            DonTopo::D3D12::D3D12Renderer& d3d12 = *d3d12Owned;
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
            // Y el ScriptManager ANTES que la escena, tambien como en Vulkan:
            // los ScriptComponent del árbol guardan sol::table cuyo destructor
            // toca la VM de Lua, así que la escena tiene que morir antes que
            // ella (la destrucción va al revés que la declaración).
            DonTopo::ScriptManager d3dScripts;
            DonTopo::Scene         d3dScene;

            // El proyecto, que es de donde sale la escena y los ajustes. Vive
            // aquí, fuera de todo, porque los paneles lo consultan durante toda
            // la sesión.
            DonTopo::ProjectContext d3dProject;
            if (!lastProject.empty())
                d3dProject = DonTopo::ProjectContext(lastProject);

            // JobSystem y carga asíncrona: sin esto Load Scene cae al camino
            // síncrono y bloquea el frame entero mientras Assimp trabaja.
            DonTopo::JobSystem d3dJobs;
            d3dJobs.start();
            DonTopo::AsyncAssetLoader d3dAssets(d3dJobs);

            glfwSetWindowUserPointer(window.getNativeWindow(), &d3d12);
            glfwSetFramebufferSizeCallback(
                window.getNativeWindow(), [](GLFWwindow* w, int width, int height) {
                    auto* r = static_cast<DonTopo::D3D12::D3D12Renderer*>(glfwGetWindowUserPointer(w));
                    if (r)
                        r->resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
                });

            // El editor, con este backend detrás. A partir de aquí el camino
            // es el mismo que con Vulkan: los paneles hablan con la interfaz.
            DonTopo::EditorUI editor;
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

            // La escena va a una textura y el backbuffer se queda para la
            // interfaz: es lo que necesita el viewport dentro de su panel.
            d3d12.setUiLayer(&editor);
            d3d12.setRenderToTexture(true);

            // Al BACKEND también, no solo al editor: es de donde saca las
            // sondas de reflexión de la escena. Sin esto no ve ninguna y no
            // hornea nada, sin decir por qué.
            d3d12.setScene(&d3dScene);
            d3d12.setSceneRoot(&d3dScene.getRoot());

            editor.setScene(&d3dScene);
            editor.setPhysicsManager(&d3dPhysics);
            editor.setAudioManager(&d3dAudio);
            editor.setAssetLoader(&d3dAssets);

            // Scripting, con el mismo cableado que el camino de Vulkan.
            d3dScripts.setScene(&d3dScene);
            d3dScripts.setPhysicsManager(&d3dPhysics);
            d3dScripts.setAudioManager(&d3dAudio);
            d3dScripts.setLogCallback(
                [&editor](const std::string& msg) { editor.pushExternalLog(msg); });
            d3dScripts.setOnInstantiated([&d3d12](DonTopo::GameObject* go) {
                // Lo que instancia un script hay que subirlo a la GPU: si no,
                // existe en la escena y no se dibuja.
                go->traverse([&d3d12](DonTopo::GameObject* n) {
                    if (!n->hasMesh())
                        return;
                    if (n->isSkinned())
                        n->skinnedRenderIndex = d3d12.addSkinnedMesh(*n->getSkinnedMesh());
                    else
                        n->staticRenderIndex = d3d12.addStaticMesh(*n->getMesh());
                });
            });
            d3dScripts.setOnDestroying([&d3d12, &editor](DonTopo::GameObject* go) {
                // La selección primero: si el editor se queda apuntando a lo que
                // se va a liberar, crashea al dibujar Properties el frame
                // siguiente.
                editor.onGameObjectDestroyed(go);
                d3d12.removeGameObject(go);
            });
            // Scripts/ vive en la raíz del repo y no se copia junto al exe: el
            // hot reload tiene que vigilar los .lua que edita el usuario.
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
                "DirectX 12: el editor corre sobre este backend. Sin UI 2D del juego y sin "
                "sondas de reflexion todavia.");

            // Selector de proyecto, el mismo que con Vulkan: mientras esté
            // activo el bucle solo presenta su frame, así que el editor no
            // aparece hasta elegir. Es quien abre la escena —por
            // openProjectScene, la misma puerta que el Load Scene del menú— y
            // quien deja puesto el ProjectContext del que salen los ajustes.
            //
            // El proyecto recordado NO se abre solo: de él salió el backend con
            // el que se arrancó, pero elegir es del usuario.
            ProjectSelectorState d3dProjectSelector;
            editor.setProjectSelector([&]() -> bool {
                const std::filesystem::path chosen = drawProjectSelector(d3dProjectSelector);
                if (chosen.empty())
                    return false;
                return openChosenProject(editor, d3dProject, chosen);
            });

            // Cámara de vuelo, la misma que ya tenía este camino.
            DonTopo::Camera d3dCamera(glm::vec3(6.0f, 4.5f, 8.0f), -126.87f, -21.8f);
            d3dCamera.moveSpeed = 8.0f;
            d3d12.setCamera(d3dCamera);

            double d3dLastX = 0.0, d3dLastY = 0.0;
            bool   d3dLooking   = false;
            auto   d3dLastFrame = std::chrono::high_resolution_clock::now();

            // Luces: los buffers viven fuera del bucle para no reasignar por
            // frame. Las de relleno son las mismas que usa el camino de Vulkan y
            // solo salen si la escena no aporta ninguna.
            const std::vector<DonTopo::Light> d3dDefaultLights = {
                { glm::vec4(0.0f, 500.0f, 300.0f, 1.0f),     glm::vec4(1.0f, 0.95f, 0.8f, 1.0f) },
                { glm::vec4(-300.0f, 200.0f, -200.0f, 1.0f), glm::vec4(0.4f, 0.5f, 1.0f, 0.8f) },
            };
            std::vector<DonTopo::Light> d3dLights;
            std::vector<float>          d3dLightRadii;

            // Empareja los canvas de la escena con sus slots del backend por
            // ownerId (syncUiCanvases), fuera del bucle para no reasignar cada
            // frame. La caché de sync de cada canvas vive DENTRO de su slot.
            std::vector<DonTopo::UiCanvasBinding> d3dBindings;
            // Y los canvas de PANTALLA en orden de prioridad de input, tambien
            // fuera del bucle: se rellena entero cada frame.
            std::vector<DonTopo::UiCanvas*> d3dUiCanvases;

            while (!window.shouldClose())
            {
                window.pollEvents();

                const auto  d3dNow = std::chrono::high_resolution_clock::now();
                const float d3dDelta =
                    std::chrono::duration<float>(d3dNow - d3dLastFrame).count();
                d3dLastFrame = d3dNow;

                // Selector en pantalla: ni escena, ni scripts, ni cámara. Solo
                // su frame, que lo dibuja el propio editor dentro de
                // buildUiFrame.
                if (editor.isProjectSelectorActive())
                {
                    editor.buildUiFrame(d3d12.viewportTexture(), &d3dScene.getRoot(),
                                        d3dCamera.getViewMatrix());
                    // Con el selector delante no se dibuja escena, pero el
                    // singleton de gizmos es global: vaciarlo aquí también evita
                    // que lo que quedara de antes se arrastre.
                    DonTopo::Gizmos::clear();
                    d3d12.drawFrame();
                    continue;
                }

                // Escena → backend, por frame y ANTES de la interfaz: es lo que
                // hace que mover un objeto en Properties, ocultar una malla,
                // tocar sus reflejos o mover una luz se vean. Sin esto el
                // backend se queda con lo que se le mandó al cargar y el editor
                // se mira pero no se toca. Mismo recorrido que el camino de
                // Vulkan, salvo la parte de Play —física y scripts— que este
                // camino todavía no corre.
                //
                // Recorrido en vivo y no una lista cacheada: el editor puede
                // borrar GameObjects, y un puntero guardado quedaría colgando.
                d3dScripts.pollChanges();

                // Mismo criterio que el camino de Vulkan, y por los mismos
                // motivos: en Play manda el Audio Listener de la escena si lo
                // hay y está habilitado, en Edit Mode manda siempre la cámara
                // del editor (la preview del inspector tiene que oírse desde
                // donde mira el usuario). Con la base degenerada (escala 0) se
                // cae a la cámara en vez de colar un NaN en FMOD, del que ya no
                // se recupera.
                glm::vec3 listenerPos = d3dCamera.getPos();
                glm::vec3 listenerFwd = d3dCamera.getFront();
                glm::vec3 listenerUp  = d3dCamera.getUp();
                if (editor.isPlaying())
                {
                    if (DonTopo::GameObject* lis = d3dScene.findAudioListener())
                    {
                        const glm::vec3 fwdAxis = glm::vec3(lis->worldTransform[2]);
                        const glm::vec3 upAxis  = glm::vec3(lis->worldTransform[1]);
                        // getEnabled() como en Vulkan: sin él, este camino
                        // respetaba un listener deshabilitado y los dos
                        // backends sonaban distinto con la misma escena.
                        if (lis->getAudioListener()->getEnabled() &&
                            glm::length(fwdAxis) > 1e-6f && glm::length(upAxis) > 1e-6f)
                        {
                            listenerPos = glm::vec3(lis->worldTransform[3]);
                            listenerFwd = glm::normalize(-fwdAxis);
                            listenerUp  = glm::normalize(upAxis);
                        }
                    }
                }
                // Fuera del gate de Play: ver el comentario del camino de Vulkan
                // (es la única llamada a System::update() y a
                // set3DListenerAttributes).
                d3dAudio.update(listenerPos, listenerFwd, listenerUp);

                if (editor.isPlaying())
                {
                    d3dPhysics.stepSimulation(d3dDelta);
                    d3dScene.update(d3dDelta, d3dPhysics);
                    d3dScripts.update(d3dDelta);
                }
                else
                {
                    // Sin física corriendo, pero los transforms padre→hijo se
                    // siguen propagando: Properties y los gizmos tienen que
                    // funcionar en Edit. Scene::update haría esto y además
                    // impondría la pose de PhysX sobre cada objeto con collider
                    // dinámico, que es justo lo que impide editarlos.
                    d3dScene.getRoot().updateWorldTransforms();
                    // Igual que en el camino de Vulkan: la preview del
                    // inspector sigue al objeto mientras se arrastra.
                    d3dScene.updateAudioSpatial();
                }

                // tickDeferredDeletes primero, que libera lo borrado;
                // onAssetsLoaded después, que aplica lo que acaba de llegar del
                // loader y cierra el lote con un solo flush. Los dos ANTES del
                // recorrido: un objeto cuyo mesh acaba de aterrizar tiene que
                // estar ya registrado cuando se empujen los transform.
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

                    if (go->skinnedRenderIndex >= 0)
                    {
                        // La visibilidad antes de tocar la animación: el reloj de
                        // un mesh oculto se congela, así que el flag tiene que
                        // estar ya puesto o iría un frame por detrás.
                        d3d12.setSkinnedMeshVisible(go->skinnedRenderIndex, go->meshVisible);
                        if (const auto& anim = go->getAnimator())
                        {
                            // El Animator es el único dueño de animTime: evalúa
                            // en CPU y el backend solo recibe el resultado. En
                            // Edit el grafo no evalúa transiciones.
                            // El estado de Play lo lleva el editor; el backend de
                            // DirectX 12 no lo conoce.
                            anim->update(d3dDelta, editor.isPlaying());
                            // setAnimationBlend siempre: los pose* resuelven ya
                            // el cross-fade y el blend por parámetro, y sin
                            // ninguno de los dos el peso vale 1 y el backend ni
                            // mira el segundo clip.
                            d3d12.setAnimationBlend(go->skinnedRenderIndex,
                                                    (uint32_t)anim->poseClipB(),
                                                    anim->poseTimeB(),
                                                    (uint32_t)anim->poseClipA(),
                                                    anim->poseTimeA(),
                                                    anim->poseWeight(),
                                                    anim->poseLockRootMotion());
                        }
                        else
                        {
                            d3d12.updateAnimation(go->skinnedRenderIndex, d3dDelta);
                        }
                        d3d12.setSkinnedTransform(go->skinnedRenderIndex, go->worldTransform);
                        d3d12.setSkinnedSsr(go->skinnedRenderIndex,
                                            go->ssrEnabled ? go->ssrIntensity : 0.0f);
                    }
                });

                // Luces después del recorrido: sus worldTransform ya están
                // propagados y de ahí salen posición y dirección. Por frame y no
                // en un evento porque mover el GameObject de una luz tiene que
                // moverla en el acto, igual que el transform de una malla.
                if (d3dScene.collectLights(d3dLights, d3dLightRadii) > 0)
                {
                    d3d12.setLights(d3dLights);
                    d3d12.setLightRadii(d3dLightRadii);
                }
                else
                {
                    // Sin una sola luz en la escena, las de relleno: las escenas
                    // del repo son de antes del LightComponent y sin esto el
                    // editor abriría a oscuras.
                    d3d12.setLights(d3dDefaultLights);
                    d3d12.setLightRadii({});
                }

                // UI 2D del juego: resolución, widgets y jerarquía de CADA
                // canvas de la escena, por frame —por lo mismo que los
                // transform, tocar un campo en Properties tiene que verse en
                // el acto—. Sin ningún Canvas la lista va vacía y el sync
                // limpia el árbol.
                d3dBindings.clear();
                d3dScene.collectCanvases(d3dBindings);
                d3d12.syncUiCanvases(d3dBindings);

                // Input de la UI: sin esto el árbol no resuelve estados y los
                // colores del botón, el fundido y el Click no harían nada. El
                // ratón solo entra en Play; el tiempo entra siempre, para que un
                // color recién editado se vea sin darle a Play.
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
                        // La rueda ya la lee ImGui, así que se le pide a él en
                        // vez de duplicar un callback de GLFW.
                        uiInput.scrollDelta = ImGui::GetIO().MouseWheel;
                    }
                    else
                    {
                        uiInput.mousePos = glm::vec2(-1.0f, -1.0f);
                    }
                    // Teclado y mando SOLO en Play, igual que el ratón: en
                    // edición, el Tab y las flechas son del editor.
                    if (editor.isPlaying() && !ImGui::GetIO().WantCaptureKeyboard)
                        DonTopo::fillUiInputKeys(uiInput);
                    else
                        // El gate del PUSH (WantTextInput) no es el mismo que este,
                        // asi que un caracter puede haber entrado en un frame que no
                        // se consume. Sin tirarlo, saldria en el siguiente que si.
                        DonTopo::discardUiInputChars();
                    uiInput.timeSeconds = (float)glfwGetTime();
                    // A TODOS los canvas de pantalla, no solo al primero: ver
                    // dispatchUiInput. Con uiCanvas() los botones de un segundo
                    // canvas se dibujaban pero eran inertes.
                    d3d12.screenUiCanvases(d3dUiCanvases);
                    DonTopo::dispatchUiInput(d3dUiCanvases, uiInput);
                }

                // La interfaz después: lo que se toque aquí lo recoge el
                // recorrido del frame siguiente.
                editor.buildUiFrame(d3d12.viewportTexture(), &d3dScene.getRoot(),
                                    d3dCamera.getViewMatrix());

                // Cámara después de la interfaz, para que arrastrar por un
                // panel no gire la vista.
                {
                    GLFWwindow* native = window.getNativeWindow();

                    // Sobre el panel de la escena, no "donde ImGui no capture":
                    // el viewport ES una ventana de ImGui, así que
                    // WantCaptureMouse vale true justo donde hay que girar la
                    // vista y la cámara no se movía nunca. Mismo criterio que
                    // el camino de Vulkan.
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

                    if (editor.isViewportHovered())
                        d3dCamera.update(native, d3dDelta);

                    d3d12.setCamera(d3dCamera);
                }

                // Gizmos: los rellena el panel del viewport dentro de
                // buildUiFrame (colliders, luces, frustum de la cámara, ejes de
                // la selección) y aquí se suben al backend. En Vulkan de esto se
                // encarga Renderer::drawFrame, que los lee del singleton y los
                // limpia; este camino no pasaba por ahí, así que ni se dibujaban
                // ni se vaciaban NUNCA: el vector crecía frame a frame hasta
                // reventar kMaxGizmoVertices, y lo único que se veía era el
                // aviso de "capacidad de 65536 vertices excedida".
                {
                    const auto& lineas = DonTopo::Gizmos::vertices();
                    d3d12.submitDebugLines(
                        lineas.empty() ? nullptr : &lineas[0].pos.x, lineas.size());
                    DonTopo::Gizmos::clear();
                }

                d3d12.drawFrame();
            }

            // ORDEN CRÍTICO. La GPU sigue con el último frame en vuelo al salir
            // del bucle, y ese frame usa los buffers y la textura de ImGui.
            // Apagar ImGui sin esperar antes se los quita a la GPU debajo, y el
            // proceso muere al cerrar —con volcado de WER, pero sin que nada en
            // pantalla lo delate, porque ya no queda frame que dibujar.
            d3d12.waitIdle();
            editor.shutdownUi();
            // El JobSystem antes que el backend: un job a medias sigue tocando
            // el loader y la escena, y pararlo después sería usarlos ya
            // liberados.
            d3dJobs.shutdown();
            d3d12.shutdown();
            return 0;
        }
#endif

        // El backend lo construye main —que es quien sabe cuál toca— y la
        // propiedad pasa al editor. La referencia al tipo concreto se guarda
        // ANTES de moverlo: el ciclo de vida (init, drawFrame, shutdown) no
        // está en la interfaz, y este camino es el de Vulkan.
        DonTopo::EditorUI editor;

        auto               vulkanRenderer = std::make_unique<DonTopo::Renderer>();
        DonTopo::Renderer& renderer       = *vulkanRenderer;
        editor.setRenderer(std::move(vulkanRenderer));

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
            // Al canvas del juego SOLO en Play y con ImGui sin foco de texto:
            // el mismo gate que ya se le aplica al teclado unas lineas mas
            // abajo. Sin esto, renombrar un GameObject en el Hierarchy tambien
            // escribiria dentro del InputField de la escena.
            auto* ctx = static_cast<AppCtx*>(glfwGetWindowUserPointer(w));
            if (ctx && ctx->ed && ctx->ed->isPlaying() && !ImGui::GetIO().WantTextInput)
                DonTopo::pushUiInputChar(c);
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
        ProjectSelectorState    projectSelector;

        editor.setProjectSelector([&]() -> bool {
            const std::filesystem::path chosen = drawProjectSelector(projectSelector);
            if (chosen.empty())
                return false;
            return openChosenProject(editor, project, chosen);
        });

        // Empareja los canvas de la escena con sus slots del Renderer por
        // ownerId (Renderer::syncUiCanvases), fuera del bucle para no
        // reasignar cada frame. La caché de sync de cada canvas vive DENTRO
        // de su slot, en el Renderer: aquí ya no hace falta ninguna.
        std::vector<DonTopo::UiCanvasBinding> uiBindings;
        // Y los canvas de PANTALLA en orden de prioridad de input, tambien
        // fuera del bucle: se rellena entero cada frame.
        std::vector<DonTopo::UiCanvas*> uiCanvases;

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

            // Listener 3D. En Play, si la escena tiene un Audio Listener (y está
            // habilitado), el audio 3D se oye desde ÉL: posición = columna 3 del
            // worldTransform, forward = -Z local, up = +Y local (misma
            // convención que la cámara y el runtime); con la base degenerada
            // (escala 0) se cae a la cámara en vez de colar un NaN en FMOD, del
            // que ya no se recupera.
            //
            // En Edit Mode manda SIEMPRE la cámara del editor, aunque la escena
            // tenga listener: el botón Play del Audio Clip (PropertiesPanel) es
            // una herramienta de edición y tiene que oírse desde donde el
            // usuario está mirando. Con el listener de la escena mandando aquí,
            // previsualizar un clip 3D de un objeto lejano daría silencio sin
            // ninguna explicación.
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
            // FUERA del gate de Play, a propósito: AudioManager::update es lo
            // único que llama a set3DListenerAttributes y a System::update(),
            // así que dejándolo dentro el listener se quedaba donde lo dejó la
            // última sesión de Play —o en el (0,0,0) mirando a -Z de fábrica de
            // FMOD si nunca se entró— y la preview de un clip 3D sonaba atenuada
            // o muda sin motivo aparente.
            audio.update(listenerPos, listenerFwd, listenerUp);

            if (renderer.isPlaying())
            {
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
                // Pero el seguimiento del audio 3D sí hace falta aquí: la
                // preview del inspector puede estar sonando mientras el usuario
                // arrastra el objeto con el gizmo. En Play lo hace scene.update.
                scene.updateAudioSpatial();
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
                        // setAnimationBlend siempre: los pose* resuelven ya el
                        // cross-fade y el blend por parámetro, y sin ninguno de
                        // los dos el peso vale 1 y el Renderer ni mira el
                        // segundo clip.
                        renderer.setAnimationBlend(go->skinnedRenderIndex,
                                                    (uint32_t)anim->poseClipB(),
                                                    anim->poseTimeB(),
                                                    (uint32_t)anim->poseClipA(),
                                                    anim->poseTimeA(),
                                                    anim->poseWeight(),
                                                    anim->poseLockRootMotion());
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

            // Canvas de UI: la resolución, los widgets y la jerarquía de CADA
            // canvas de la escena salen de sus componentes, no de uno cableado
            // aquí. Va por frame y no en un evento porque tocar un campo en
            // Properties tiene que verse en el acto — también fuera de Play,
            // que es cuando se pinta el gizmo del área útil. Sin ningún Canvas
            // la lista va vacía y syncUiCanvases deja limpio lo que hubiera.
            uiBindings.clear();
            scene.collectCanvases(uiBindings);
            renderer.syncUiCanvases(uiBindings);

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
                    // La rueda ya la lee ImGui: se le pide a él en vez de
                    // duplicar un callback de GLFW.
                    uiInput.scrollDelta = ImGui::GetIO().MouseWheel;
                }
                else
                {
                    // Fuera de todo el canvas: ningún botón queda en Hover.
                    uiInput.mousePos = glm::vec2(-1.0f, -1.0f);
                }
                // Teclado y mando SOLO en Play, igual que el ratón: en edición
                // el Tab y las flechas son del editor, no del juego.
                if (editor.isPlaying() && !ImGui::GetIO().WantCaptureKeyboard)
                    DonTopo::fillUiInputKeys(uiInput);
                else
                    // Mismo motivo que en el camino de D3D12: los dos gates no
                    // coinciden, y lo que no se consume no puede cruzar el frame.
                    DonTopo::discardUiInputChars();
                uiInput.timeSeconds = (float)glfwGetTime();
                // A TODOS los canvas de pantalla, no solo al primero: ver
                // dispatchUiInput.
                renderer.screenUiCanvases(uiCanvases);
                DonTopo::dispatchUiInput(uiCanvases, uiInput);
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
