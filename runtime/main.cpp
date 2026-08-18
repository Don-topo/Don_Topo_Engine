// Runtime del juego: carga un .scene y lo ejecuta. Es el wiring de
// sandbox/src/main.cpp menos todo lo del editor — sin ImGui, sin gizmos de
// depuración, sin hot reload y en Play desde el frame 0.
#include "DonTopo/Core/Engine.h"
#include "DonTopo/Core/Window.h"
#include "DonTopo/Core/Input.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/JobSystem.h"
#include "DonTopo/Renderer/Renderer.h"
#include "DonTopo/Renderer/AsyncAssetLoader.h"
#include "DonTopo/Renderer/RenderBackend.h"
#include "DonTopo/Renderer/EditorRenderer.h"
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
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <unordered_set>
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

// Manda std::cout y std::cerr a game.log, junto al ejecutable. El runtime se
// enlaza como subsystem WINDOWS (runtime/CMakeLists.txt) para no abrir una
// consola detras del juego, y sin consola esos flujos no van a ninguna parte:
// los mensajes del motor y los print() de Lua se perderian en silencio.
//
// Se cambia el rdbuf en vez de reabrir stdout con freopen: sin consola el CRT
// de MSVC no tiene stream que reabrir (_fileno(stdout) == -2) y freopen_s falla
// devolviendo error sin llegar a crear el fichero — probado, no es teoria.
// Cambiar el rdbuf no toca los descriptores del sistema, asi que funciona igual
// con consola y sin ella. Lo que escriba una libreria de terceros por printf
// (Assimp, PhysX) sigue sin capturarse; el motor solo usa cout/cerr.
//
// El ofstream se filtra a proposito: cout/cerr guardan su rdbuf, y destruirlo
// al salir dejaria esos punteros colgando durante la destruccion de los objetos
// estaticos, donde todavia puede haber logs. unitbuf hace que cada << llegue al
// disco, asi que un crash no se lleva por delante las ultimas lineas — que son
// justo las que interesan.
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

// Errores que impiden jugar: sin consola, un mensaje por stderr solo acaba en
// game.log y el usuario ve la ventana cerrarse sin explicacion. Va tambien al
// log, que es donde queda el rastro despues de cerrar el dialogo.
void reportFatal(const std::string& msg)
{
    std::cerr << msg << std::endl;
#ifdef _WIN32
    // MessageBoxW y no MessageBoxA: los mensajes vienen en UTF-8 (lo que hay en
    // los .cpp y lo que devuelve what()), y la version ANSI los interpretaria
    // con la codepage del sistema — cualquier acento saldria como garabatos.
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, nullptr, 0);
    if (wlen > 0)
    {
        std::wstring wmsg(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, wmsg.data(), wlen);
        MessageBoxW(nullptr, wmsg.c_str(), L"Don Topo Engine", MB_OK | MB_ICONERROR);
    }
#endif
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const std::filesystem::path exeDir = executableDir();
        std::error_code ec;
        std::filesystem::current_path(exeDir, ec);
        // Despues del current_path: game.log se crea junto al ejecutable, no en
        // el directorio desde el que se lanzo el juego.
        redirectStdioToLogFile();

        const std::string scenePath = (argc > 1) ? argv[1] : "game.scene";

        // Backend de render elegido al exportar. Vive en game.cfg y no en
        // game.scene: es configuracion de arranque, no parte de la escena.
        // Un paquete sin game.cfg —exportado antes de que existiera— arranca
        // con Vulkan, que es lo que hacia siempre.
        //
        // El resultado sale del bloque: es lo que decide que backend se
        // construye mas abajo.
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
                                std::cout << "game.cfg: backend de render desconocido, se usa Vulkan"
                                          << std::endl;
                                requested = DonTopo::RenderBackend::Vulkan;
                            }
                        }
                    }
                }
                catch (const std::exception&)
                {
                    std::cout << "game.cfg ilegible: se usa Vulkan" << std::endl;
                }
            }

            const DonTopo::BackendSelection sel = DonTopo::resolveRenderBackend(requested);
            // std::cout y no printf: el runtime va sin consola y lo que
            // redirige a game.log son los streams de C++, asi que un printf se
            // perderia sin dejar rastro.
            if (!sel.message.empty())
                std::cout << sel.message << std::endl;

            // Lo que de verdad se puede arrancar, no lo que se pidio:
            // resolveRenderBackend ya cayo a Vulkan si DirectX 12 no estaba.
            requestedBackend = sel.backend;
        }

        DonTopo::Engine engine;
        DonTopo::Window window;
        // Oculta de entrada: se enseña tras presentar el primer frame (el del
        // splash). Sin esto, la ventana se hacia visible aqui y Windows pintaba
        // el area de cliente en BLANCO durante los ~520ms que tarda
        // initPresentation en levantar Vulkan — un flash blanco antes del logo.
        window.init(1280, 720, exeDir.stem().string().c_str(), nullptr, /*showOnInit=*/false);
        DonTopo::Input::init(window.getNativeWindow());
        // El backend, construido segun lo que pidio el proyecto. A partir de
        // aqui todo el runtime habla con la interfaz: quien decide cual es, es
        // esta linea y nadie mas.
        std::unique_ptr<DonTopo::EditorRenderer> rendererOwned;
#ifdef DT_D3D12_ENABLED
        if (requestedBackend == DonTopo::RenderBackend::D3D12)
            rendererOwned = std::make_unique<DonTopo::D3D12::D3D12Renderer>();
#endif
        if (!rendererOwned)
            rendererOwned = std::make_unique<DonTopo::Renderer>();
        DonTopo::EditorRenderer& renderer = *rendererOwned;

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

        // Antes de initPresentation(): initImGui y createOffscreenImages leen
        // el flag durante esa inicialización. Adelantado respecto al orden
        // original porque ahora initPresentation corre ANTES de scene.load (ver
        // abajo), y setHeadless debe precederlo igualmente.
        renderer.setHeadless(true);

        // Pool de hilos + loader asíncrono. El JobSystem se declara aquí, ANTES
        // que nada que un job pudiera capturar por referencia, y se apaga a mano
        // antes del teardown de la escena (ver el final): un worker vivo tocando
        // la escena a medio destruir sería un crash al salir.
        DonTopo::JobSystem jobSystem;
        jobSystem.start();
        DonTopo::AsyncAssetLoader assetLoader(jobSystem);

        // Resuelve el logo: en un paquete exportado esta junto al .exe como
        // splash.png; en dev (sin exportar) se cae a assets/MainEngineLogo.png.
        std::string logoPath = "splash.png";
        {
            std::error_code lec;
            if (!std::filesystem::exists(logoPath, lec) || lec)
                logoPath = "assets/MainEngineLogo.png";
        }

        // initPresentation + splash ANTES de scene.load: ninguno depende de la
        // escena (initPresentation es la fase 1 "poder presentar"; el auto-fit y
        // los recursos que necesitan meshes viven en initSceneResources, fase 2,
        // que sigue yendo DESPUÉS de la carga). Así el splash ya está en pantalla
        // mientras se cargan los assets y puede mostrar progreso real.
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
        // oscuro del shader, nunca el blanco por defecto de la ventana. Este
        // timing (show DESPUÉS del primer present del splash) es el que evita el
        // flash blanco y se conserva intacto pese al reordenado.
        // Sin splash (logo ausente) se queda oculta hasta justo antes del bucle
        // de juego — ver el show() de mas abajo—, que tambien evita el blanco.
        bool windowShown = false;
        if (haveSplash)
        {
            window.show();
            windowShown = true;
        }

        // --- Precarga en paralelo (progreso en el splash) ---
        // El coste pesado de scene.load es el Assimp::ReadFile de cada malla,
        // síncrono. Se parsea el JSON de la escena a mano para recolectar los
        // sourcePath únicos, se cargan en los workers, y luego scene.load los
        // consume de una cache en RAM en vez de leer disco — bombeando el splash
        // todo el rato para que la ventana responda y muestre avance.
        DonTopo::PreloadedMeshCache preloaded;
        {
            auto sceneJson = DonTopo::FileManager::readJson(scenePath);
            if (!sceneJson)
            {
                reportFatal("Error: no se pudo cargar la escena '" + scenePath + "'");
                jobSystem.shutdown();
                return EXIT_FAILURE;
            }

            // Set de sourcePath únicos: varios nodos que comparten FBX generan un
            // solo ReadFile (el loader además dedup por path internamente).
            std::unordered_set<std::string> uniquePaths;
            std::function<void(const nlohmann::json&)> collect = [&](const nlohmann::json& node) {
                if (node.contains("mesh") && node["mesh"].is_object())
                {
                    const std::string sp = node["mesh"].value("sourcePath", std::string());
                    if (!sp.empty()) uniquePaths.insert(sp);
                }
                if (auto it = node.find("children"); it != node.end() && it->is_array())
                    for (const auto& child : *it)
                        collect(child);
            };
            if (sceneJson->contains("root") && (*sceneJson)["root"].is_object())
                collect((*sceneJson)["root"]);

            // Encola una petición por path. targetId no se usa aquí (la cache se
            // indexa por path, no por GameObject: aún no hay escena), así que va
            // un índice cualquiera distinto de 0.
            uint64_t reqId = 1;
            for (const auto& p : uniquePaths)
                assetLoader.requestMesh(p, reqId++);

            // Bombea el splash mientras cargan los workers, guardando cada
            // resultado en la cache por path. Un error (fichero movido/roto)
            // deja el path fuera de la cache: scene.load caerá a un ReadFile de
            // disco para ese, exactamente como el camino de siempre.
            while (assetLoader.pending() > 0)
            {
                for (auto& r : assetLoader.pumpCompleted(1000.0f))
                {
                    if (!r.error.empty())
                    {
                        std::cerr << "Precarga fallida '" << r.path << "': " << r.error
                                  << " (se reintentara desde disco al cargar la escena)" << std::endl;
                        continue;
                    }
                    if (r.mesh)
                        preloaded[r.path] = r.mesh;
                }
                pumpSplash(false, 0.0f);
            }
        }

        // Carga de la escena desde la cache. loader == nullptr: NO se usa la ruta
        // async por-GameObject de la Task 8 (que perdería la config de clips del
        // Animator y no haría auto-fit a tiempo). En su lugar, preloaded aporta
        // las mallas ya en RAM y la carga corre por el camino síncrono de
        // siempre, solo que sin ReadFile — mismo modelo de registro, misma
        // config de animación.
        if (!scene.load(scenePath, physics, audio, /*loader=*/nullptr, /*preloaded=*/&preloaded))
        {
            reportFatal("Error: no se pudo cargar la escena '" + scenePath + "'");
            jobSystem.shutdown();
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

        std::vector<DonTopo::GameObject*> allNodes;
        scene.traverse([&](DonTopo::GameObject* go) { allNodes.push_back(go); });

        // Pasada 1: meshes estáticos -> Renderer::init(meshes). Las mallas ya
        // están en los GameObject (vinieron de la cache), así que el auto-fit de
        // initSceneResources funciona igual que con la carga síncrona.
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
        // Solo lo que el Renderer usa de verdad: setSceneRoot (el árbol que
        // recorre para dibujar) y setScene (currentFrameCamera() llama a
        // findCamera() en Play). Los passthroughs de physics/audio/scripts que
        // había aquí eran del editor, que en runtime no existe.
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

        // Pasada 2: meshes animados, después de init como exige el Renderer.
        for (auto* go : allNodes)
        {
            if (go->hasMesh() && go->isSkinned())
                go->skinnedRenderIndex = renderer.addSkinnedMesh(*go->getSkinnedMesh());
            pumpSplash(false, 0.0f);
        }

        // --- Espera de uploads antes del primer frame de juego (correctness) ---
        // addSkinnedMesh NO sube al instante: mete el upload en m_pendingBatch y
        // marca el objeto con un uploadTicket > 0, así que el mesh queda
        // INVISIBLE hasta que el batch se envía (flushPendingUploads) y su fence
        // señala (lo detecta tickDeferredDeletes, que avanza m_lastCompletedTicket).
        // Sin esto el .exe exportado enseñaba los personajes rigged a medio subir
        // —o sea, invisibles— en el primer frame. Se fuerza el envío y se espera
        // a que TODOS los tickets señalen, con el splash todavía en pantalla para
        // que la ventana siga respondiendo y no haya pop-in. Los meshes estáticos
        // no pasan por el batch (uploadTicket == 0), así que ya eran visibles;
        // esto solo hace falta por los skinned.
        renderer.flushPendingUploads();
        while (renderer.hasPendingUploads())
        {
            renderer.tickDeferredDeletes();   // recupera batches completados, avanza m_lastCompletedTicket
            pumpSplash(false, 0.0f);          // splash arriba / ventana viva
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

        glfwSetWindowUserPointer(window.getNativeWindow(), &renderer);
        glfwSetFramebufferSizeCallback(window.getNativeWindow(), [](GLFWwindow* w, int, int) {
            static_cast<DonTopo::EditorRenderer*>(glfwGetWindowUserPointer(w))->notifyResize();
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
        //
        // Gate de reproducción: una escena sin Audio Listener (o con el suyo
        // deshabilitado) no reproduce ningún clip. El gate vive aquí y en el
        // editor, NUNCA dentro de AudioManager ni de AudioClipComponent: esas
        // dos clases se prueban directamente y tienen que seguir sonando sin
        // escena. Un solo aviso pa toda la escena, no uno por clip.
        {
            DonTopo::GameObject* listenerGo = scene.findAudioListener();
            const bool listenerActive = listenerGo && listenerGo->getAudioListener()->getEnabled();
            if (!listenerActive)
                std::cerr << "Sin Audio Listener en la escena: los AudioClip no se reproduciran"
                          << std::endl;
            else
                scene.traverse([](DonTopo::GameObject* go) {
                    if (go->hasAudioClip() && go->getAudioClip()->getPlayOnAwake())
                        go->getAudioClip()->play(glm::vec3(go->worldTransform[3]));
                });
        }

        // Estado del sync de widgets, igual que en el editor: fuera del bucle
        // para actualizar en sitio y cachear atlas y fuentes por ruta.
        DonTopo::UiWidgetSyncCache uiWidgetCache;
        std::vector<std::pair<uint64_t, const DonTopo::ButtonComponent*>> uiButtons;
        std::vector<std::pair<uint64_t, const DonTopo::TextComponent*>> uiTexts;
        std::vector<std::pair<uint64_t, const DonTopo::ProgressBarComponent*>> uiBars;

        while (!window.shouldClose())
        {
            DonTopo::Input::update();

            // Drenaje del buzón de DonTopo.loadScene, al principio del frame:
            // los scripts del frame anterior ya terminaron su tick, así que
            // destruir la escena aquí no mata al GameObject que pidió la carga.
            // Mismo saneamiento que EditorUI::reloadSceneFromJson: soltar los
            // recursos GPU del árbol viejo, resetear índices, cargar, y volver a
            // registrar el árbol entero en el Renderer.
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
                // Síncrono como el restore del editor: sin flush, los meshes del
                // batch diferido no se verían hasta ~2 frames después y el
                // árbol viejo ya no está (parpadeo).
                renderer.flushUploadsAndWait();
                renderer.setSceneRoot(&scene.getRoot());
                // El near/far salio de las mallas de la escena de arranque
                // (initSceneResources): sin recalcularlo, una escena cargada por
                // script mas grande se ve recortada — el skybox el primero.
                // Mismo motivo por el que EditorUI lo llama al recargar.
                renderer.refitCameraRange();
                // El alive set de Lua guardaba punteros de la escena vieja y los
                // GameObject nuevos pueden reusar esas direcciones.
                scriptManager.rebuildAliveSet();
                std::cout << (luaLoaded ? "Escena cargada: " : "Error al cargar escena: ")
                          << luaScenePath << std::endl;
            }

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
            // Audio Listener: si la escena tiene uno (y está habilitado), manda
            // él y no la cámara — misma convención de ejes y mismo guard de base
            // degenerada que el bloque de arriba. Sin listener se queda lo que
            // resolvió la cámara, que es el fallback.
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

            scene.traverse([&](DonTopo::GameObject* go) {
                if (go->staticRenderIndex >= 0)
                {
                    renderer.setTransform(go->staticRenderIndex, go->worldTransform);
                    // El runtime tiene que renderizar igual que el editor: mismo
                    // sink, mismo sitio.
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
                        anim->update(dt, /*playing=*/true);
                        // setAnimationBlend siempre: los pose* resuelven ya el
                        // cross-fade y el blend por parámetro; sin ninguno de
                        // los dos el peso vale 1 y el segundo clip ni se mira.
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
                        renderer.updateAnimation(go->skinnedRenderIndex, dt);
                    }
                    renderer.setSkinnedTransform(go->skinnedRenderIndex, go->worldTransform);
                    renderer.setSkinnedSsr(go->skinnedRenderIndex,
                                           go->ssrEnabled ? go->ssrIntensity : 0.0f);
                }
            });

            // Antes de drawFrame: los scripts Lua pueden instanciar/borrar
            // GameObjects en cualquier frame. tickDeferredDeletes reclama los
            // batches ya señalados (avanza la visibilidad) y drena los borrados
            // diferidos; flushPendingUploads envía el batch de lo instanciado
            // ESTE frame (addStaticMesh/addSkinnedMesh vía setOnInstantiated lo
            // dejan en m_pendingBatch: sin flush no se sube nunca y el objeto
            // queda invisible). Mismo par que el bucle del editor
            // (sandbox/src/main.cpp: tickDeferredDeletes + onAssetsLoaded, que
            // acaba en flushPendingUploads). En régimen estable, sin instanciar
            // nada, el flush es un no-op barato.
            renderer.tickDeferredDeletes();
            renderer.flushPendingUploads();

            // Canvas de UI: misma regla que en el editor — la resolución sale
            // del componente de la escena exportada, por frame.
            if (DonTopo::GameObject* canvasGo = scene.findCanvas())
                canvasGo->getCanvas()->applyTo(renderer.uiCanvas());

            // Widgets: mismo volcado por frame que en el editor.
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
            DonTopo::syncUiWidgets(uiButtons, uiTexts, uiBars, renderer.uiCanvas(),
                                   uiWidgetCache, renderer);

            // Input de la UI: sin esto el árbol no resuelve estados y los cinco
            // colores del botón, el fundido y el Click no existen. El ratón está
            // en píxeles de VENTANA y el canvas trabaja en píxeles de SALIDA,
            // que no tienen por qué coincidir (escalado de la ventana).
            {
                DonTopo::UiInputState uiInput;
                double mx = 0.0, my = 0.0;
                glfwGetCursorPos(window.getNativeWindow(), &mx, &my);
                int ww = 0, wh = 0;
                glfwGetWindowSize(window.getNativeWindow(), &ww, &wh);
                // uiWidth/uiHeight y NO renderWidth/renderHeight: el canvas se
                // resuelve en píxeles de SALIDA, que con SSAA no son los del
                // render (el ratón caía al doble de lejos del cursor).
                const float sx = (ww > 0) ? (float)renderer.uiWidth()  / (float)ww : 1.0f;
                const float sy = (wh > 0) ? (float)renderer.uiHeight() / (float)wh : 1.0f;
                uiInput.mousePos = glm::vec2((float)mx * sx, (float)my * sy);
                uiInput.mouseDown[0] =
                    glfwGetMouseButton(window.getNativeWindow(), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                uiInput.mouseDown[1] =
                    glfwGetMouseButton(window.getNativeWindow(), GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
                uiInput.mouseDown[2] =
                    glfwGetMouseButton(window.getNativeWindow(), GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
                uiInput.timeSeconds = (float)glfwGetTime();
                renderer.uiCanvas().updateInput(uiInput);
            }

            renderer.drawFrame(window);
            window.pollEvents();
        }

        scriptManager.onPlayStop();
        // jobSystem.shutdown() ANTES del teardown de la escena: para y une todos
        // los workers, así ninguno puede tocar la escena mientras se destruye.
        // (En este punto ya no debería quedar nada pendiente —la precarga se
        // drenó entera antes del bucle— pero el orden se respeta igualmente.)
        jobSystem.shutdown();
        scene.shutdown(physics, audio);
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
