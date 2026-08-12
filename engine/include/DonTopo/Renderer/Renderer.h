#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <memory>
#include <glm/glm.hpp>
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Core/Camera.h"
#include "DonTopo/Core/CameraComponent.h"
#include "DonTopo/Renderer/UniformBufferObject.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Renderer/EditorRenderer.h"
#include "DonTopo/Renderer/RendererState.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/GpuResources.h"
#include "DonTopo/Renderer/SharedGpuMesh.h"
#include "DonTopo/Renderer/DeferredDelete.h"
#include "DonTopo/Renderer/TransferBatch.h"
#include "DonTopo/Renderer/AsyncAssetLoader.h"
#include "DonTopo/Renderer/UiLayer.h"
#include "DonTopo/Renderer/Skybox.h"
#include "DonTopo/Renderer/SplashScreen.h"
#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiSpriteBatch.h"
#include "DonTopo/UI/UiTextureAtlas.h"
#include "DonTopo/UI/UiFont.h"
#include <array>
#include <unordered_map>

namespace DonTopo {

    class Window;
    class GameObject;
    class PhysicsManager;
    class AudioManager;
    class Scene;
    class ScriptManager;

    // Los get/set del estado escalar de calidad y efectos (ambiente, bloom,
    // SSAO, SSR, niebla, FXAA, TAA, Forward+) los pone RendererState: no
    // dependen de Vulkan y los comparte el backend de DirectX 12.
    class Renderer : public EditorRenderer {
        // --- API de edicion (solo la consume DonTopoEditor) ---
        //
        // Tras el split Core/Editor la frontera es de build (dos targets), no
        // de API: estos publicos siguen siendo alcanzables desde cualquier
        // punto de Core aunque se diseñaron para el editor. Se listan aqui
        // porque un futuro punto de extension saldria de esta lista, y porque
        // el proximo lector necesita saber que NO son para el runtime:
        //
        //   setUiLayer                        inyeccion de la capa ImGui
        //   setViewportSize / viewportAspect  el render va a un panel, no a la ventana
        //   setOutlineTarget                  outline de la seleccion
        //   removeMeshComponent               borrar un asset en uso desde el Content Browser
        //   replaceStaticTextureWithMissing   idem, textura -> placeholder
        //   rebuildSkinnedMesh                reimportar un FBX con el editor abierto
        //   requestProbeBake / ...All         bakeo de probes (herramienta de autor)
        //   setPerfCaptureEnabled             captura del PerformancePanel
        //
        // Lo que NO entra, aunque hoy solo lo llame el editor: los get/set de
        // calidad grafica (bloom, fog, ssao, ssr, taa, msaa, ssaa, fxaa,
        // forward+) y los contadores de stats. Un juego exportado tiene
        // motivos legitimos para tocarlos desde su menu de opciones; capar ahi
        // seria cerrar la puerta al caso de uso, no ordenar la frontera.
        //
        // Ojo con dos que la memoria del proyecto daba por solo-editor y no lo
        // son: registerGameObject y flushUploadsAndWait los llama tambien
        // runtime/main.cpp.
        public:
            Renderer()                              = default;
            ~Renderer();
            Renderer(const Renderer&)               = delete;
            Renderer& operator=(const Renderer&)    = delete;
            void init(Window& window, const std::vector<Mesh>& meshes);
            // Fase 1 del arranque: lo minimo para presentar (device, swapchain,
            // render pass, framebuffers, command buffers, sync, capa de UI). No crea
            // pipelines de escena. La usa el runtime para poder dibujar el
            // splash antes de la carga pesada. init() la llama primero.
            void initPresentation(Window& window);
            // Fase 2: pipelines PBR/shadow/compute, offscreen, descriptor sets,
            // subida de mallas estaticas y auto-fit de camara (necesita meshes).
            // init() la llama despues.
            void initSceneResources(const std::vector<Mesh>& meshes);
            // Inicializa el splash sobre el render pass del swapchain (requiere
            // initPresentation ya llamado). false si el logo no carga: el caller
            // se salta el splash. Solo lo llama el runtime; el editor no.
            bool beginSplash(const std::string& logoPath);
            // Presenta un frame con solo el splash a alpha [0,1]. No-op si el
            // splash no se inicializo.
            void drawSplashFrame(float alpha);
            void drawFrame(Window& window);
            // Una vez por frame, desde el bucle principal, ANTES de drawFrame.
            // Es lo que hace avanzar la cola de destrucción diferida.
            void tickDeferredDeletes();
            void shutdown();
            void setCamera(const Camera& camera);
            void notifyResize() { m_framebufferResized = true; }
            // Contorno de seleccion: indices del objeto resaltado en m_objects y
            // en m_skinnedObjects (los mismos que guarda GameObject en
            // staticRenderIndex/skinnedRenderIndex), -1 para "ninguno". Solo lo
            // llama el editor, una vez por frame; el runtime no tiene seleccion
            // y con el default (-1, -1) no se dibuja ningun contorno. El objeto
            // resaltado sigue sujeto al mismo culling que el resto: fuera del
            // frustum no se dibuja nada.
            void setOutlineTarget(int staticIndex, int skinnedIndex)
            {
                m_outlineStaticIndex  = staticIndex;
                m_outlineSkinnedIndex = skinnedIndex;
            }
            // En headless no hay editor que pulse Play: el runtime arranca
            // jugando desde el frame 0. Esto es además lo que hace que
            // currentFrameCamera() elija el CameraComponent de la escena en
            // vez de la cámara de vuelo del editor.
            bool isPlaying() const;
            // Modo runtime: ni UI ni paneles. Solo tiene efecto si se
            // llama ANTES de initPresentation() (o de init(), que la llama) —
            // createOffscreenImages y el arranque de la UI leen el flag
            // durante esa inicialización.
            void setHeadless(bool headless) { m_headless = headless; }
            // Capa de UI, no-propietaria: es el editor quien posee al Renderer
            // y se registra aquí. nullptr (runtime) = no hay pass de UI.
            // Debe fijarse ANTES de initPresentation(), que es quien la
            // arranca.
            void setUiLayer(UiLayer* ui) { m_ui = ui; }

            // Canvas de la UI de JUEGO (no el editor). Se dibuja dentro del pass
            // de composicion, encima de la escena y debajo de ImGui, y vacio no
            // cuesta ni un comando. Devolver la referencia hace de getter y de
            // setter: el arbol se monta sobre uiCanvas().root().
            UiCanvas&       uiCanvas()       { return m_uiCanvas; }
            const UiCanvas& uiCanvas() const { return m_uiCanvas; }
            // Carga un atlas desde disco y le reserva su descriptor set. El
            // Renderer es el dueno; devuelve nullptr si la imagen no se puede
            // leer. Los sprites se anaden luego con addSprite.
            UiTextureAtlas* loadUiAtlas(const std::string& path);
            // Igual que loadUiAtlas pero para un TTF: hornea el atlas MSDF, le
            // reserva su descriptor set y devuelve nullptr si el fichero no se
            // puede leer. El Renderer es el dueno de la fuente. Tampoco cachea
            // por ruta — quien la pida repetidamente (el sync de los botones)
            // lleva su propia cache, igual que con los atlas.
            UiFont* loadUiFont(const std::string& path, float bakePx = 48.0f);
            // currentFrameCamera() necesita preguntarle a la escena por su
            // cámara cada frame (Scene::findCamera es la única fuente de
            // verdad).
            void setScene(Scene* scene) { m_scene = scene; }

            // Aspect del render target. Público porque el gizmo de frustum
            // (ViewportPanel) tiene que usar EXACTAMENTE el mismo que usará la
            // proyección de Play, o dibujaría un encuadre que no se corresponde.
            float viewportAspect() const
            {
                const VkExtent2D e = effectiveViewport();
                return e.height > 0 ? (float)e.width / (float)e.height : 1.0f;
            }
            // Tamano en pixeles del render target, EL MISMO que se le pasa a
            // UiCanvas::buildDrawData. Publico por el gizmo del area util del
            // Canvas: los px que deja el canvas estan en este espacio y hay que
            // pasarlos al de la imagen del panel (que con SSAA no coinciden).
            uint32_t renderWidth()  const { return m_renderExtent.width; }
            uint32_t renderHeight() const { return m_renderExtent.height; }
            // Tamano EXACTO del area de imagen del panel Viewport del editor, en
            // pixeles. Lo llama el editor una vez por frame. Sin esto el render
            // iria al tamano de la VENTANA y el panel lo reescalaria al dibujarlo:
            // ese reescalado bilineal suaviza los bordes por su cuenta -se come
            // el escalonado y con el la diferencia entre modos de anti-aliasing- y
            // ademas deforma la imagen cuando el aspect del panel no coincide con
            // el de la ventana. Cambiarlo recrea los targets, igual que un resize.
            // El runtime no la llama nunca: ahi el destino es el swapchain entero.
            void setViewportSize(uint32_t width, uint32_t height);
            // Recalcula el encuadre de referencia de la escena (centro y tamaño) a
            // partir de lo que hay AHORA en GPU, no de las mallas que recibió
            // init(). Importa porque de m_cameraDistance salen el near y el far de
            // la proyección del editor (near = d*0.001, far = d*3): sin esto una
            // escena cargada después del arranque se dibuja con el rango de otra
            // —la de arranque—, y lo que se sale de ese far (el skybox el primero)
            // se recorta. La llama el editor al cargar escena y cuando aterrizan
            // los assets asíncronos; el runtime no la llama nunca. Si no hay nada
            // que acotar (escena vacía) conserva el rango vigente.
            void refitCameraRange();
            void setSceneRoot(GameObject* root) { m_sceneRoot = root; }
            // Libera mesh/skinnedMesh/texturas en GPU de node y todo su subárbol
            // (llamado por EditorUI justo antes de borrar el nodo del scene graph).
            void removeGameObject(GameObject* node);
            // Inverso de removeGameObject: sube a GPU los meshes (estático o
            // skinned) de node y su subárbol que aún no estén registrados
            // (staticRenderIndex/skinnedRenderIndex < 0). Usado tras
            // reconstruir un subárbol desde JSON — reloadSceneFromJson (toda
            // la escena) y CreateGameObjectCommand/DeleteGameObjectCommand en
            // Command.cpp (un solo subárbol).
            void registerGameObject(GameObject* node);
            // Quita solo el componente Mesh de go (no borra el GameObject ni sus
            // otros componentes). No-op si go es nullptr o no tiene mesh.
            void removeMeshComponent(GameObject* go);
            // Sustituye la textura del slot indicado por el checkerboard
            // "missing" (mismo generador que createTextureImage usa cuando no
            // hay path/bytes). No-op si renderIndex está fuera de rango.
            // Sincroniza con vkDeviceWaitIdle antes de tocar el descriptor
            // set (evita pisar un frame en vuelo). Solo cubre meshes
            // estáticos — no hay UI hoy que asigne meshes skinned.
            void replaceStaticTextureWithMissing(int renderIndex, TextureSlot slot);
            // facePaths: +X, -X, +Y, -Y, +Z, -Z (cualquier formato soportado por stb_image)
            void initSkybox(const std::array<std::string, 6>& facePaths);
            void setTransform(size_t objectIndex, const glm::mat4& transform)
            {
                if (objectIndex < m_objects.size())
                    m_objects[objectIndex].transform = transform;
            }
            // Fuerza de SSR del objeto (0 = no refleja). Viaja hasta el post-pass
            // por el alfa del attachment HDR, que pbr.frag escribe desde
            // PushData::flags.y. Se sincroniza por frame junto al transform, igual
            // que hace el runtime: asi Play Mode, Undo y la carga de escena no
            // necesitan ningun camino propio.
            void setObjectSsr(size_t objectIndex, float strength)
            {
                if (objectIndex < m_objects.size())
                    m_objects[objectIndex].ssrStrength = strength;
            }
            void setSkinnedSsr(int index, float strength)
            {
                if (index >= 0 && index < (int)m_skinnedObjects.size())
                    m_skinnedObjects[index].ssrStrength = strength;
            }
            // Visibilidad del mesh (false = no se dibuja). Se sincroniza por frame
            // junto al transform, igual que el SSR, así que Play Mode, Undo y la
            // carga de escena no necesitan camino propio. La consumen los pases de
            // escena, sombras y AO: oculto no se manda a la GPU en ninguno.
            // Física, selección y contorno no la miran.
            void setObjectMeshVisible(size_t objectIndex, bool visible)
            {
                if (objectIndex < m_objects.size())
                    m_objects[objectIndex].meshVisible = visible;
            }
            void setSkinnedMeshVisible(int index, bool visible)
            {
                if (index >= 0 && index < (int)m_skinnedObjects.size())
                    m_skinnedObjects[index].meshVisible = visible;
            }
            void setLights(const std::vector<Light>& lights){ m_lights = lights; }

            // ── Reflection probes ──────────────────────────────────────────
            // La UI solo ENCOLA: el bake ocurre al principio de drawFrame, que
            // es donde se puede esperar a que la GPU quede libre sin pillar el
            // command buffer a medio grabar (mismo sitio que rebuildAaResources).
            void requestProbeBake(uint64_t ownerId) { m_probeBakeQueue.push_back(ownerId); }
            void requestProbeBakeAll()              { m_probeBakeAllQueued = true; }
            // ms del ULTIMO bake (una sonda o la tanda entera), por timestamps.
            float lastProbeBakeMs() const { return m_probeLastBakeMs; }
            int   probeCount() const      { return (int)m_probes.size(); }
            // Memoria GPU de las capturas persistentes de UNA sonda, en bytes.
            // No cuenta el cubemap de captura, que es uno solo pa todas.
            static constexpr uint64_t probeMemoryBytes()
            {
                // rgba16f = 8 bytes/texel, 6 caras. El prefiltrado suma sus mips
                // (la serie 1 + 1/4 + 1/16 + ... truncada a IBL_PREFILTER_MIPS).
                uint64_t pre = 0;
                for (uint32_t m = 0; m < IBL_PREFILTER_MIPS; m++)
                {
                    const uint64_t s = IBL_PREFILTER_SIZE >> m;
                    pre += s * s * 6ull * 8ull;
                }
                return (uint64_t)IBL_IRRADIANCE_SIZE * IBL_IRRADIANCE_SIZE * 6ull * 8ull + pre;
            }
            // ms del ultimo bake de UNA sonda concreta, o -1 si nunca se bakeo.
            float probeBakeMs(uint64_t ownerId) const
            {
                for (const GpuProbe& p : m_probes)
                    if (p.ownerId == ownerId) return p.baked ? p.bakeMs : -1.0f;
                return -1.0f;
            }

            // Interruptor global: apagado no se graba ni un dispatch de la cadena
            // de mips y la composicion suma bloom cero (el pass LDR NO se salta,
            // que es tambien quien tonemapea).
            void  setBloomEnabled(bool v) override;
            // Coste GPU del bloom + composicion del ultimo frame ya resuelto, en
            // ms. 0 si el dispositivo no soporta timestamps.
            float bloomGpuMs() const         { return m_bloomGpuMs; }

            // SSAO. Apagado por defecto: con el flag a false no se graba ni el
            // depth pre-pass ni los dos dispatches, y el mapa de AO se deja a 1.0
            // una sola vez, asi que la imagen es la misma que antes de la feature
            // y el coste GPU cae a cero.
            void  setSsaoEnabled(bool v);
            // Coste GPU del pre-pass + los dos dispatches, en ms. 0 si el efecto
            // esta apagado o el dispositivo no soporta timestamps.
            float ssaoGpuMs() const          { return m_ssaoGpuMs; }

            // Coste GPU del SSR en ms: los dos dispatches, mas el depth pre-pass
            // cuando es el SSR quien lo pide (con el SSAO encendido ese pre-pass
            // ya lo contabiliza ssaoGpuMs y aqui no se suma dos veces).
            float ssrGpuMs() const           { return m_ssrGpuMs; }

            // Coste GPU del dispatch de niebla en ms. 0 si esta apagada o el
            // dispositivo no soporta timestamps.
            float fogGpuMs() const              { return m_fogGpuMs; }

            // ── Anti-aliasing ────────────────────────────────────────────────
            // Modos EXCLUYENTES: solo uno activo a la vez. None deja el frame
            // exactamente como antes de la feature (ni un comando de mas).
            // El enum vive en RendererState: los dos backends leen el mismo
            // modo. None sin anti-aliasing, Fxaa filtro morfologico sobre la LDR
            // ya tonemapeada, Ssaa supersampling, Msaa multimuestra en el
            // rasterizador y Taa acumulacion temporal con jitter de subpixel.
            using AaMode = RendererState::AaMode;
            // Cambiar de modo puede exigir recrear recursos (SSAA cambia el tamano
            // de TODOS los targets internos; MSAA, el numero de muestras de las
            // imagenes, los render passes y los pipelines). Eso no se hace aqui:
            // se marca y lo resuelve el primer drawFrame siguiente, con la GPU ya
            // en reposo. Los parametros de cada modo, en cambio, viajan por push
            // constant y surten efecto en el frame siguiente sin recrear nada.
            void   setAaMode(AaMode mode);
            // SSAA: multiplicador de resolucion por eje. El coste crece con el
            // CUADRADO, y el tamano se recorta a maxImageDimension2D del device.
            void  setSsaaFactor(float v);
            float ssaaFactor() const              { return m_ssaaFactor; }
            // Muestras por pixel del MSAA. Se recorta a lo que soporte el device
            // (framebufferColorSampleCounts & framebufferDepthSampleCounts).
            void setMsaaSamples(int v);
            int  maxMsaaSamples() const;
            // Coste GPU del pass PROPIO del modo activo (FXAA, resolve de SSAA,
            // acumulacion del TAA). En None y en MSAA vale 0: MSAA no tiene pass
            // propio, su coste esta repartido en la escena y en la composicion,
            // y para verlo hay que mirar renderGpuMs().
            float aaGpuMs() const                 { return m_aaGpuMs; }
            // Coste GPU de TODO el render sin la UI: desde el pass de escena
            // hasta el ultimo pass de post. Se mide SIEMPRE, tambien en None, que
            // es justo lo que lo hace util: es la referencia contra la que se
            // compara el sobrecoste real de SSAA y de MSAA.
            float renderGpuMs() const             { return m_renderGpuMs; }

            // ── Instrumentacion del panel Performance (solo editor) ──────────
            // Apagada por defecto: con el panel cerrado no se graba ni un
            // timestamp mas de los que ya habia, ni se tocan los contadores.
            // El panel la enciende mientras esta visible y la apaga al cerrarse.
            void setPerfCaptureEnabled(bool on);
            bool perfCaptureEnabled() const       { return m_perfCapture; }
            // Coste GPU del pass de sombras y del pass de escena. Valen 0 hasta
            // que la captura lleva dos frames encendida (se leen del frame N-2,
            // que es el que ya espero la fence de este slot).
            float shadowGpuMs() const             { return m_shadowGpuMs; }
            float sceneGpuMs() const              { return m_sceneGpuMs; }
            // Contadores del ultimo frame grabado con la captura encendida.
            // "Culled" son los objetos estaticos + skinned que el frustum dejo
            // fuera del pass de escena.
            int   statDrawCalls() const           { return m_statDrawCalls; }
            int   statInstances() const           { return m_statInstances; }
            int   statCulled() const              { return m_statCulled; }

            // ── Forward+ ─────────────────────────────────────────────────────
            // Radio POR luz, en el mismo orden que setLights. Vacio (lo normal) =
            // todas usan el radio global de arriba.
            void setLightRadii(const std::vector<float>& radii) { m_lightRadii = radii; }
            // Coste GPU del dispatch de culling, en ms. 0 en Off.
            float forwardPlusGpuMs() const        { return m_fpGpuMs; }
            // Media de luces por celda NO VACIA del ultimo frame ya resuelto, y
            // numero de celdas que se pasaron del maximo por celda (esas si
            // pierden luces: es la senal de que hace falta bajar el radio).
            float forwardPlusAvgPerCell() const   { return m_fpAvgPerCell; }
            uint32_t forwardPlusOverflowCells() const { return m_fpOverflowCells; }

            // decoded: píxeles que el worker ya decodificó para este mesh (nullptr
            // en el camino síncrono). Encola todos los uploads en el batch del pump
            // actual y marca el objeto con el ticket vigente: no se dibuja hasta que
            // flushPendingUploads() lo envíe y su fence señale.
            int addSkinnedMesh(const SkinnedMesh& mesh, const std::vector<DecodedImage>* decoded = nullptr);
            // Rehace TODOS los recursos GPU del objeto skinned `index` a partir
            // de `mesh`, en el mismo slot (el skinnedRenderIndex del GameObject
            // no cambia). Necesario tras añadir o quitar clips: los keyframes
            // viven en SSBOs subidos una sola vez, y la GPU tendría la lista
            // vieja. Conserva transform, animTime y activeClip.
            void rebuildSkinnedMesh(int index, const SkinnedMesh& mesh);
            // Añade un mesh estático nuevo (buffers + texturas + descriptor set) y lo
            // registra en m_objects. Devuelve el índice para GameObject::staticRenderIndex.
            // decoded: mismo contrato que addSkinnedMesh (nullptr = camino síncrono).
            int addStaticMesh(const Mesh& mesh, const std::vector<DecodedImage>* decoded = nullptr);
            // Cierra y envía el batch del pump actual. Llamar UNA vez tras
            // procesar todos los resultados del frame.
            void flushPendingUploads();
            // true si hay uploads sin completar: un batch abierto en m_pendingBatch
            // o batches todavía en vuelo. Lo consume el runtime (Task 10) para saber
            // si aún debe seguir bombeando antes de dar la carga por terminada.
            bool hasPendingUploads() const;
            // Cierra el batch pendiente y BLOQUEA hasta que todos los uploads en
            // vuelo hayan completado y sido reclamados, de modo que los objetos
            // recién registrados sean visibles en ESTE frame. Uso reservado a las
            // transiciones síncronas raras iniciadas por el usuario (restore de
            // Play->Stop, undo/redo de un Create): ahí el stall de vkDeviceWaitIdle
            // es aceptable y reproduce la visibilidad inmediata previa a la carga
            // asíncrona. NO usar en el camino async de Load Scene (tiene su modal +
            // pump por frame; bloquear reintroduciría el stall que el modal evita).
            void flushUploadsAndWait();
            void updateAnimation(int index, float deltaTime);
            // Sink puro: fija el clip y el tiempo que el Animator ya ha
            // calculado en CPU. No avanza el tiempo — a diferencia de
            // updateAnimation, que sigue siendo el camino de los objetos SIN
            // AnimatorComponent. Los dos no se pisan: quien tiene Animator nunca
            // pasa por updateAnimation.
            void setAnimationState(int index, uint32_t clipIndex, float animTime);
            void setSkinnedTransform(int index, const glm::mat4& transform);

            // ── Frustum culling ──────────────────────────────────────────────
            // Seis planos en espacio de mundo, con la normal apuntando HACIA
            // DENTRO del volumen: un punto es visible si queda del lado
            // positivo de los seis. Cada plano es (nx, ny, nz, d) con la normal
            // normalizada, de forma que dot(n, p) + d es la distancia con signo.
            struct Frustum {
                glm::vec4 planes[6] = {};
            };
            // Extrae los planos de una matriz viewProj (Gribb-Hartmann). Asume
            // el rango de profundidad de Vulkan z=[0,1] — el plano cercano sale
            // de la fila 2 a secas, no de (fila3 + fila2) como en el convenio
            // de OpenGL. Con matrices *RH_ZO (las que usa este motor, ver
            // updateUniformBuffer) esto es lo correcto; con glm::perspective a
            // secas culearía de más por el lado cercano.
            static Frustum frustumFromViewProj(const glm::mat4& viewProj);
            // AABB en espacio LOCAL del mesh + su transform a mundo. Devuelve
            // false solo si la caja queda entera fuera de algún plano; es un
            // test conservador (puede dar true de más en las esquinas del
            // frustum, nunca false de menos, que sería un objeto desaparecido).
            static bool aabbVisible(const Frustum& frustum,
                                    const glm::vec3& localMin,
                                    const glm::vec3& localMax,
                                    const glm::mat4& model);
            // Radio de una esfera centrada en el ORIGEN LOCAL del modelo que
            // contiene la malla skinned en CUALQUIER pose de CUALQUIER clip. La
            // AABB en reposo no vale: el compute deforma los vértices y un brazo
            // levantado se sale de la caja, así que cullear con ella haría
            // desaparecer al personaje — el peor fallo posible aquí.
            //
            // No evalúa ninguna pose: acota hueso a hueso con los valores
            // EXTREMOS de las keys (alcance acumulado por la jerarquía × escala
            // acumulada, más el radio de la nube de vértices que arrastra cada
            // hueso medido en su propio espacio). Por eso la cota vale también
            // para los tiempos interpolados: mix() no sale del segmento entre
            // sus extremos y slerp() devuelve una rotación, que no cambia
            // normas. Es holgada a propósito — falso positivo sí, falso
            // negativo nunca.
            //
            // Devuelve 0 si no hay con qué acotar (sin huesos o sin vértices);
            // el llamante lo trata como "sin cota" y no culea.
            static float skinnedBoundRadius(const SkinnedMesh& mesh);
            // Matrices de luz de las N cascadas y sus distancias de corte. Las
            // comparten el UBO (para que el fragment shader muestree el shadow
            // map) y el culling del pass de sombras: si las dos se calcularan
            // por separado y una cambiara, se culearían objetos que el shadow
            // map sí necesita. Por eso se calculan UNA vez por frame en draw(),
            // antes de updateUniformBuffer y de recordCommandBuffer, y los dos
            // consumidores leen la caché de abajo.
            void computeCascades();

            // ── Draw batching por instancing ─────────────────────────────────
            // Un draw instanciado: todas las instancias del rango
            // [firstInstance, firstInstance + instanceCount) del SSBO de
            // transforms comparten la entrada compartida sharedIndex, así que
            // se dibujan con un solo vkCmdDrawIndexed.
            struct InstanceBatch {
                int      sharedIndex   = -1;
                uint32_t firstInstance = 0;
                uint32_t instanceCount = 0;
                // Fuerza de SSR común al grupo. Entra en la CLAVE de agrupado
                // junto a sharedIndex: metallic y roughness ya viajaban por
                // entrada compartida, así que dos objetos con la misma malla y
                // distinta fuerza de SSR no pueden compartir push constants.
                // Solo se parten en dos draws cuando los valores difieren.
                float    ssrStrength   = 0.0f;
            };
            // Un objeto ya evaluado por el pass que lo va a dibujar. Las guardas
            // (entrada borrada, upload en vuelo) y el culling por AABB los
            // resuelve el llamante -es quien tiene la caché GPU y el frustum- y
            // llegan aquí resumidos en `visible`. El transform va por puntero: el
            // agrupado solo lo copia al SSBO, no lo guarda.
            struct BatchCandidate {
                int              sharedIndex = -1;
                bool             visible     = false;
                const glm::mat4* transform   = nullptr;
                // Los passes que no pintan color (sombras, depth pre-pass) lo
                // dejan a 0: con un único valor el agrupado sale idéntico al de
                // antes de la feature.
                float            ssr         = 0.0f;
            };
            // Agrupa por sharedIndex los candidatos VISIBLES y deja sus
            // transforms contiguos por grupo en outTransforms (que apunta ya al
            // hueco del SSBO, con sitio para outCapacity matrices). El orden es
            // estable: los grupos salen por orden de primera aparición y dentro
            // de cada grupo se conserva el orden de los candidatos, de modo que
            // el resultado no baila entre frames.
            //
            // firstInstanceBase es el índice ABSOLUTO dentro del SSBO de la
            // primera matriz escrita: los dos passes comparten buffer y el
            // segundo escribe detrás del primero, así que sin base los
            // firstInstance del pass principal apuntarían al rango de sombras.
            //
            // Devuelve cuántas matrices se han escrito. Si no caben todas trunca
            // por grupos (los que no entran salen con instanceCount 0 y se
            // descartan) antes que escribir fuera de rango: el llamante
            // dimensiona el buffer antes, esto es la red de seguridad.
            static uint32_t buildInstanceBatches(const BatchCandidate* candidates,
                                                 size_t                count,
                                                 glm::mat4*            outTransforms,
                                                 uint32_t              outCapacity,
                                                 uint32_t              firstInstanceBase,
                                                 std::vector<InstanceBatch>& outBatches);

        private:

            // Una instancia dibujable. Ya NO posee recursos GPU: buffers,
            // texturas y descriptor set viven en la entrada compartida que
            // apunta sharedIndex, y N objetos con la misma malla+material
            // apuntan todos a la misma. Lo único por instancia es el transform
            // (y el nombre, que es de depuración).
            struct RenderObject
            {
                std::string     name;
                // -1 = sin recursos (nunca construido, o ya liberado desde el
                // editor). Es el chequeo que sustituye al viejo
                // "vertexBuffer == VK_NULL_HANDLE".
                int             sharedIndex         = -1;
                glm::mat4       transform{1.0f};
                // 0 = no refleja. Lo sincroniza el bucle de la aplicación desde
                // el GameObject, igual que el transform.
                float           ssrStrength         = 0.0f;
                // false = lo saltan los pases de escena, de sombras y de AO: el
                // mesh no se manda a la GPU, así que tampoco proyecta ni ocluye.
                bool            meshVisible         = true;
            };

            struct SkinnedMatGfx {
                VkImage         textureImage  = VK_NULL_HANDLE;
                VkDeviceMemory  textureMem    = VK_NULL_HANDLE;
                VkImageView     textureView   = VK_NULL_HANDLE;
                VkSampler       sampler       = VK_NULL_HANDLE;
                VkImage         normalImage   = VK_NULL_HANDLE;
                VkDeviceMemory  normalMem     = VK_NULL_HANDLE;
                VkImageView     normalView    = VK_NULL_HANDLE;
                VkSampler       normalSampler = VK_NULL_HANDLE;
                VkImage         ormImage      = VK_NULL_HANDLE;
                VkDeviceMemory  ormMem        = VK_NULL_HANDLE;
                VkImageView     ormView       = VK_NULL_HANDLE;
                VkSampler       ormSampler    = VK_NULL_HANDLE;
                float           metallic      = 0.0f;
                float           roughness     = 0.5f;
                VkDescriptorSet descSets[2]   = {};
            };

            struct SubMeshDraw {
                uint32_t indexStart;
                uint32_t indexCount;
                uint32_t materialIndex;
            };

            // ABI compartida por los 3 compute shaders. 16 bytes, fijos: el 4º
            // campo era un pad sin usar y ahora lleva el clipBase, así que
            // ningún offset se ha movido.
            struct ComputePush
            {
                float animTime;
                uint32_t boneCount;
                uint32_t vertexCount;
                // activeClip * boneCount: índice base del bloque del clip activo
                // dentro del SSBO de BoneInfos, que va en layout [clip][hueso].
                // Solo lo lee bone_eval.comp; bone_hierarchy y skinning declaran
                // este slot como "pad" y no lo tocan.
                uint32_t clipBase;
            };
            static_assert(sizeof(ComputePush) == 16, "ComputePush debe seguir en 16 bytes: los 3 .comp declaran este layout");

            struct PushData {
                glm::mat4 transform{1.0f};
                float     metallic  = 1.0f;
                float     roughness = 1.0f;
                // flags.x = 1: triangle.vert coge el model matrix del SSBO de
                // instancias por gl_InstanceIndex (ruta estática, agrupada);
                // 0: lo coge de `transform` (ruta skinned, que comparte este
                // vertex shader y dibuja una instancia con su propia matriz).
                // Es el viejo _pad reaprovechado: mismo tipo y offset, así que
                // pbr.frag sigue declarando el bloque igual que siempre.
                glm::vec2 flags{0.0f, 0.0f};
            };
            static_assert(sizeof(PushData) == 80, "PushData must be 80 bytes");

            struct SkinnedRenderObject {
                std::string    name;
                // SSBOs estáticos
                VkBuffer       keyframePosBuffer    = VK_NULL_HANDLE;
                VkDeviceMemory keyframePosMemory    = VK_NULL_HANDLE;
                VkBuffer       keyframeRotBuffer    = VK_NULL_HANDLE;
                VkDeviceMemory keyframeRotMemory    = VK_NULL_HANDLE;
                VkBuffer       keyframeScaleBuffer  = VK_NULL_HANDLE;
                VkDeviceMemory keyframeScaleMemory  = VK_NULL_HANDLE;
                VkBuffer       boneInfoBuffer       = VK_NULL_HANDLE;
                VkDeviceMemory boneInfoMemory       = VK_NULL_HANDLE;
                VkBuffer       inputVertexBuffer    = VK_NULL_HANDLE;
                VkDeviceMemory inputVertexMemory    = VK_NULL_HANDLE;
                // SSBOs dinámicos (escritos por compute)
                VkBuffer       localTransformBuffer = VK_NULL_HANDLE;
                VkDeviceMemory localTransformMemory = VK_NULL_HANDLE;
                VkBuffer       finalBoneBuffer      = VK_NULL_HANDLE;
                VkDeviceMemory finalBoneMemory      = VK_NULL_HANDLE;
                // Output vertex buffer (usado también como VB en graphics)
                VkBuffer       outputVertexBuffer   = VK_NULL_HANDLE;
                VkDeviceMemory outputVertexMemory   = VK_NULL_HANDLE;
                // Index buffer
                VkBuffer       indexBuffer          = VK_NULL_HANDLE;
                VkDeviceMemory indexMemory          = VK_NULL_HANDLE;
                uint32_t       indexCount           = 0;
                uint32_t       vertexCount          = 0;
                uint32_t       boneCount            = 0;
                // Nº de clips concatenados en los SSBOs de keyframes. Solo se usa
                // pa clampar en setAnimationState (Task 3): un clipIndex fuera de
                // rango haría que clipBase apuntara fuera del SSBO de BoneInfos y
                // el compute leyera basura sin que nada avisara.
                uint32_t       clipCount            = 1;
                // Descriptor set de compute
                VkDescriptorSet computeDescSet      = VK_NULL_HANDLE;
                // Texturas y descriptor sets por material
                std::vector<SkinnedMatGfx>  matGfx;
                std::vector<SubMeshDraw>    subMeshes;
                // Fuerza de SSR del objeto, sincronizada desde el GameObject
                // igual que el transform. La ruta skinned dibuja una instancia
                // por submalla, así que aquí no hay agrupado que partir.
                float          ssrStrength          = 0.0f;
                // false = lo saltan el pass de escena y el de sombras. El compute
                // de skinning sí sigue corriendo: el contorno de selección lee
                // sus vértices de salida.
                bool           meshVisible          = true;
                // Estado de animación
                float     animTime       = 0.0f;
                // Índice del clip que se evalúa este frame. Sin blending solo se
                // evalúa uno: los demás residen en el SSBO y no se leen.
                uint32_t  activeClip     = 0;
                float     duration       = 0.0f;
                float     ticksPerSecond = 24.0f;
                glm::mat4 transform      {1.0f};
                // Cota para el frustum culling: esfera centrada en el origen
                // local, válida en toda pose (ver skinnedBoundRadius).
                // hasBounds false = malla sin con qué acotar -> no se culea
                // nunca, que es el lado seguro.
                float     boundRadius    = 0.0f;
                bool      hasBounds      = false;
                // Lado mayor de la AABB de la pose de REPOSO, en espacio local.
                // Solo lo usa el grosor del contorno de selección, que es
                // proporcional al tamaño del objeto: boundRadius no vale ahí
                // porque acota todas las poses y sale varias veces mayor que la
                // malla. 0 = malla sin vértices (el contorno cae a su mínimo).
                float     restMaxExtent  = 0.0f;
                // 0 = subido y visible. >0 = esperando a que la fence del batch
                // con ese ticket señale. Sin esto, el objeto se dibujaría con
                // sus texturas todavía en TRANSFER_DST_OPTIMAL.
                uint64_t  uploadTicket   = 0;
            };

            void createSwapChain(Window& window);
            void createImageViews();
            void createOffscreenRenderPass();
            // Pass LDR que compone HDR + bloom, tonemapea y hospeda ademas el
            // contorno de seleccion y los gizmos: esos dos tienen que quedarse
            // FUERA del tonemap para seguir saliendo con su color plano de
            // siempre, y por eso ya no se dibujan en el pass de escena.
            void createCompositeRenderPass();
            void createRenderPass();
            void createFramebuffers();
            void createOffscreenImages();
            void destroyOffscreenImages();
            // Cadena de mips del bloom + descriptor sets de los tres pasos. Va
            // con el swapchain: la resolucion de partida es la mitad del viewport,
            // asi que redimensionar la recrea entera (destroyBloomImages primero).
            void createBloomImages();
            void destroyBloomImages();
            // Pipelines, layouts, sampler y pool del bloom. Independientes del
            // tamano, se crean una sola vez en initSceneResources.
            void createBloomPipelines();
            // Solo el pipeline del triangulo de composicion. Vive aparte porque
            // el MSAA lo obliga a rehacerse (cambia rasterizationSamples y el
            // render pass) sin tocar los layouts ni los pools del bloom.
            void recreateCompositePipeline();
            void recordBloomPass(VkCommandBuffer cmd);
            // Con el bloom apagado la composicion sigue muestreando la cadena, asi
            // que hay que dejarla en negro y en GENERAL. Pasa UNA vez por imagen
            // (al crearla y al apagar el efecto), no cada frame.
            void recordBloomClear(VkCommandBuffer cmd);
            // SSAO. createSsaoPipelines es independiente del tamano (una sola vez,
            // junto al bloom); las imagenes y los sets van con el swapchain,
            // colgados de createOffscreenImages/destroyOffscreenImages.
            void createSsaoPipelines();
            void createSsaoImages();
            void destroySsaoImages();
            // Depth pre-pass de la escena + los dos dispatches. camFrustum y fc
            // son los MISMOS del frame: el pre-pass tiene que culear con el mismo
            // criterio que el pass de escena o el AO oscureceria contra geometria
            // que luego no se dibuja.
            void recordSsaoPass(VkCommandBuffer cmd, const Frustum& camFrustum, const glm::mat4& proj);
            // Reescribe el binding 7 (mapa de AO) de todos los descriptor sets ya
            // alojados. Necesario tras recrear las imagenes con el swapchain: los
            // sets apuntarian a vistas destruidas.
            void refreshSsaoDescriptors();
            // SSR. Mismo reparto que el SSAO: los pipelines una sola vez, las
            // imagenes y los sets con el swapchain (colgados de
            // createOffscreenImages/destroyOffscreenImages).
            void createSsrPipelines();
            void createSsrImages();
            void destroySsrImages();
            // Los dos dispatches (marcha + suma sobre el HDR). Va DESPUES del
            // pass de escena -necesita el color ya iluminado- y ANTES del bloom,
            // para que el reflejo pase por el umbral del bloom y por el tonemap
            // como el resto de la imagen.
            void recordSsrPass(VkCommandBuffer cmd, const glm::mat4& proj);
            // true si hay algo que grabar: interruptor global puesto Y al menos
            // un objeto visible con fuerza > 0. Con cualquiera de las dos cosas
            // en falso no se graba ni un dispatch (ni se calcula multiplicando
            // por cero), asi que el coste GPU cae a cero.
            bool ssrActive() const;
            // Niebla volumetrica. Mismo reparto que el SSR: el pipeline una
            // sola vez, los descriptor sets con el swapchain (referencian
            // m_hdrView y m_ssaoDepthView, que se recrean con el).
            void createFogPipelines();
            void createFogSets();
            void destroyFogSets();
            // Un solo dispatch que reescribe el HDR in situ. Va DESPUES del
            // pass de escena y del SSR -necesita el color ya iluminado y con
            // los reflejos sumados- y ANTES del bloom, para que la niebla
            // florezca y pase por el tonemap como el resto de la imagen.
            void recordFogPass(VkCommandBuffer cmd, const glm::mat4& view, const glm::mat4& proj);
            // Un solo write del binding 7 sobre `set`. La comparten
            // allocateObjectDescriptorSet, la ruta skinned y el refresh de arriba.
            void writeSsaoBinding(VkDescriptorSet set, int frameIndex);
            // Anti-aliasing. El pass de resolucion es grafico (triangulo de
            // pantalla completa) y no compute: el swapchain es B8G8R8A8_SRGB y
            // Vulkan prohibe las storage images en formatos sRGB.
            void createAaRenderPasses();
            void createAaPipelines();
            // Imagen intermedia, historial del TAA, targets multisample del MSAA,
            // framebuffers y sets. Todo depende del tamano y del modo, asi que va
            // colgado de createOffscreenImages/destroyOffscreenImages.
            void createAaImages();
            void destroyAaImages();
            // Lee m_aaSrcImage (lo que escribio la composicion) y escribe
            // m_offscreenImage con el pipeline del modo activo. En None y en MSAA
            // no graba NADA: la composicion ya habra escrito directamente en
            // m_offscreenImage.
            void recordAaPass(VkCommandBuffer cmd);
            // Reconstruye TODO lo que depende del modo, con la GPU en reposo:
            // extent interno, imagenes, y -solo si cambia el numero de muestras-
            // los render passes de escena y composicion y sus pipelines. Lo llama
            // drawFrame cuando m_aaResourcesDirty esta puesto.
            void rebuildAaResources();
            // Recalcula m_renderExtent a partir del modo y del factor de SSAA,
            // recortando a maxImageDimension2D. Devuelve true si cambio.
            bool updateRenderExtent();
            // true si el modo activo necesita que la composicion escriba en la
            // imagen intermedia en vez de directamente en m_offscreenImage.
            bool needsAaIntermediate() const;
            // Las muestras que deben tener AHORA las imagenes y los pipelines:
            // m_msaaSamples si el modo es Msaa, una si no.
            VkSampleCountFlagBits targetSampleCount() const;
            // Destruye y vuelve a crear los pipelines graficos que viven en el
            // pass de escena y en el de composicion. Solo hace falta al cambiar
            // el numero de muestras: en Vulkan 1.0 rasterizationSamples no es
            // estado dinamico. Los de sombras, depth pre-pass y resolucion no
            // entran: sus render passes se quedan siempre a una muestra.
            void recreateMsaaDependentPipelines();
            // Forward+. Mismo reparto que el SSAO y el SSR: layout, pool,
            // pipelines, queries y los buffers que NO dependen del tamano (luces,
            // parametros, contadores) una sola vez en init; la rejilla y la lista
            // de indices con el swapchain.
            void createFpPipelines();
            void createFpBuffers();
            void destroyFpBuffers();
            // El dispatch de culling del modo activo. Va DESPUES de recordSsaoPass
            // (el tiled lee el depth pre-pass que graba ese) y ANTES del pass de
            // escena, que es quien consume la rejilla. En Off no graba nada.
            void recordFpCullPass(VkCommandBuffer cmd, const glm::mat4& proj);
            // Dimensiones de la rejilla de un modo a la resolucion INTERNA actual.
            // Un solo sitio: lo llaman el dimensionado de los buffers, el bloque de
            // parametros y el dispatch, y si discreparan se leerian celdas fuera.
            void fpGridDims(FpMode mode, uint32_t& gridX, uint32_t& gridY,
                            uint32_t& gridZ, uint32_t& tileSize) const;
            void createCommandBuffers();
            void createSyncObjects();
            void recordCommandBuffer(uint32_t imageIndex);
            // Casco invertido del objeto seleccionado, al final del pass de
            // escena y antes del skybox. camFrustum es el mismo del culling del
            // frame: el objeto resaltado se vuelve a evaluar con el mismo
            // criterio, no se le da paso libre. No-op si no hay seleccion.
            void recordSelectionOutline(VkCommandBuffer cmd, const Frustum& camFrustum);
            void createPipeline();
            std::vector<char> loadShaderFile(const std::string& path);
            VkShaderModule createShaderModule(const std::vector<char>& code);
            void recreateSwapChain(Window& window);
            void createVertexBuffer(const std::vector<Vertex>& v, VkBuffer& buf, VkDeviceMemory& mem, TransferBatch* batch = nullptr);
            void createIndexBuffer(const std::vector<uint32_t>& idx, VkBuffer& buf, VkDeviceMemory& mem, TransferBatch* batch = nullptr);
            void createDescriptorSetLayout();
            void createUniformBuffers();
            void createDescriptorPool();
            void createDescriptorSets();
            void updateUniformBuffer(uint32_t frameIndex);
            void createDepthResources();
            // Resuelve mesh a una entrada compartida y deja obj apuntando a
            // ella: la crea (buffers + texturas) solo si ningún otro objeto
            // había subido ya esa misma malla+material. Devuelve true si ha
            // tenido que crearla — el caller lo usa pa saber si además hay que
            // alojarle el descriptor set.
            bool buildRenderObject(const Mesh& mesh, RenderObject& obj,
                                   TransferBatch* batch = nullptr,
                                   const std::vector<DecodedImage>* decoded = nullptr);
            // Rellena una entrada recién creada. Es el cuerpo que antes estaba
            // en buildRenderObject, sin la parte de resolución de la clave.
            void createSharedGpuMesh(const Mesh& mesh, SharedGpuMesh& gpu,
                                     TransferBatch* batch,
                                     const std::vector<DecodedImage>* decoded);
            void allocateObjectDescriptorSet(SharedGpuMesh& gpu);
            void destroySharedGpuMesh(const SharedGpuMesh& gpu);
            void createShadowResources();
            void recordShadowPass(VkCommandBuffer cmd);
            // Crea imagenes, vistas, sampler y pipelines del IBL y deja los dos
            // cubemaps con un ambiente neutro. Se llama SIEMPRE en init().
            void createIblResources();
            // Rellena los dos cubemaps a partir del cubemap del skybox. No-op si
            // no hay skybox cargado. Una sola vez, desde initSkybox().
            void precomputeIbl();
            void createComputePipelines();
            void destroySkinnedRenderObject(SkinnedRenderObject& obj);
            // Cuerpo compartido por addSkinnedMesh y rebuildSkinnedMesh: crea
            // buffers, sube SSBOs, aloja descriptor sets y carga texturas sobre
            // un SkinnedRenderObject ya vacío.
            void initSkinnedRenderObject(SkinnedRenderObject& obj, const SkinnedMesh& mesh,
                                         TransferBatch* batch = nullptr,
                                         const std::vector<DecodedImage>* decoded = nullptr);
            void recordComputePass(VkCommandBuffer cmd);
            void removeStaticObject(int index);
            void removeSkinnedObject(int index);

            // Sueltan la referencia / encolan la destrucción en lugar de
            // ejecutarla. Son el ÚNICO camino permitido: llamar a
            // destroySharedGpuMesh directamente desde un call site nuevo
            // destruiría recursos que otros objetos siguen usando, y aunque no
            // los hubiera volvería a necesitar un vkDeviceWaitIdle que nadie se
            // acordaría de poner.
            //
            // releaseRenderObject deja obj.sharedIndex en -1 y solo encola la
            // destrucción cuando cae el último holder de la entrada.
            void releaseRenderObject(RenderObject& obj);
            void queueDestroySkinnedRenderObject(SkinnedRenderObject& obj);

            // Cámara efectiva de un frame. eye va aquí porque ubo.viewPos
            // alimenta el specular: sin él, en Play los brillos se calcularían
            // desde la posición de la cámara del editor.
            struct FrameCamera {
                glm::mat4 view;
                glm::mat4 proj;
                glm::vec3 eye;
            };

            // La del CameraComponent en Play (si la escena tiene una), la de
            // vuelo del editor en cualquier otro caso. Único sitio donde se
            // decide: antes la proyección estaba duplicada a pelo en
            // recordCommandBuffer y updateUniformBuffer.
            FrameCamera currentFrameCamera() const;

            GpuDevice                       m_gpu;
            GpuResources                    m_res{ m_gpu };
            VkSwapchainKHR                  m_swapChain                         = VK_NULL_HANDLE;
            VkFormat                        m_swapChainFormat                   = VK_FORMAT_UNDEFINED;
            VkExtent2D                      m_swapChainExtent                   = {};
            std::vector<VkImage>            m_swapChainImages;
            VkColorSpaceKHR                 m_swapChainColorSpace               = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            std::vector<VkImageView>        m_swapChainImageViews;
            VkRenderPass                    m_renderPass                        = VK_NULL_HANDLE;
            std::vector<VkFramebuffer>      m_swapChainFramebuffers;
            std::vector<VkCommandBuffer>    m_commandBuffers;
            static constexpr int            MAX_FRAMES                          = 2;

            // Offscreen render target (resultado LDR ya tonemapeado → textura
            // muestreada por la UI, o blit directo al swapchain en headless).
            // Sigue teniendo el formato del swapchain: la escena ya no se dibuja
            // aqui, la escribe el pass de composicion.
            VkRenderPass                    m_offscreenRenderPass               = VK_NULL_HANDLE;
            VkImage                         m_offscreenImage[MAX_FRAMES]        = {};
            VkDeviceMemory                  m_offscreenMemory[MAX_FRAMES]       = {};
            VkImageView                     m_offscreenView[MAX_FRAMES]         = {};
            VkSampler                       m_offscreenSampler                  = VK_NULL_HANDLE;
            VkFramebuffer                   m_offscreenFramebuffer[MAX_FRAMES]  = {};
            // Handle opaco de la UI, no un VkDescriptorSet: lo produce
            // UiLayer::registerUiTexture y el Renderer solo lo guarda para
            // devolvérselo. Es uint64_t porque ese contrato ya no conoce Vulkan.
            uint64_t                        m_offscreenDescSet[MAX_FRAMES]      = {};

            // ── HDR + bloom ──────────────────────────────────────────────────
            // Formato flotante del target de escena y de la cadena del bloom. El
            // pass de escena tiene que salir en HDR sin recortar a [0,1] o el
            // umbral del bloom no tendria nada por encima que extraer.
            static constexpr VkFormat       kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
            // Target de la escena (lo que antes era m_offscreenImage).
            VkImage                         m_hdrImage[MAX_FRAMES]              = {};
            VkDeviceMemory                  m_hdrMemory[MAX_FRAMES]             = {};
            VkImageView                     m_hdrView[MAX_FRAMES]               = {};

            // Cadena de mips. Una por frame en vuelo: se escribe y se consume
            // dentro del mismo command buffer, pero dos frames pueden solaparse.
            static constexpr uint32_t       BLOOM_MIPS = 5;
            VkImage                         m_bloomImage[MAX_FRAMES]            = {};
            VkDeviceMemory                  m_bloomMemory[MAX_FRAMES]           = {};
            // Una vista 2D por nivel: imageStore no elige mip, igual que en el
            // prefiltrado del IBL. La misma vista hace de storage image y de
            // textura muestreada — la imagen se queda en GENERAL toda la cadena,
            // que es un layout valido para ambas cosas y ahorra el ping-pong.
            VkImageView                     m_bloomMipView[MAX_FRAMES][BLOOM_MIPS] = {};
            VkExtent2D                      m_bloomMipExtent[BLOOM_MIPS]        = {};
            // Niveles realmente usados: un viewport pequeno no da para BLOOM_MIPS.
            uint32_t                        m_bloomMipCount                     = 0;
            VkSampler                       m_bloomSampler                      = VK_NULL_HANDLE;
            VkDescriptorSetLayout           m_bloomDescLayout                   = VK_NULL_HANDLE;
            VkDescriptorPool                m_bloomDescPool                     = VK_NULL_HANDLE;
            VkPipelineLayout                m_bloomPipelineLayout               = VK_NULL_HANDLE;
            VkPipeline                      m_bloomDownPipeline                 = VK_NULL_HANDLE;
            VkPipeline                      m_bloomUpPipeline                   = VK_NULL_HANDLE;
            VkDescriptorSet                 m_bloomDownSets[MAX_FRAMES][BLOOM_MIPS] = {};
            VkDescriptorSet                 m_bloomUpSets[MAX_FRAMES][BLOOM_MIPS]   = {};
            // Compartida por bloom_down.comp y bloom_up.comp: comparten pipeline
            // layout, asi que declaran el mismo bloque aunque cada uno ignore
            // parte de los campos.
            struct BloomPush {
                float    srcTexelX;
                float    srcTexelY;
                float    threshold;
                float    knee;
                float    radius;
                int32_t  prefilter;
            };

            // Pass de composicion: HDR + bloom → tonemap → m_offscreenImage.
            VkRenderPass                    m_compositeRenderPass               = VK_NULL_HANDLE;
            VkFramebuffer                   m_compositeFramebuffer[MAX_FRAMES]  = {};
            VkDescriptorSetLayout           m_compositeDescLayout               = VK_NULL_HANDLE;
            VkDescriptorPool                m_compositeDescPool                 = VK_NULL_HANDLE;
            VkDescriptorSet                 m_compositeSets[MAX_FRAMES]         = {};
            VkPipelineLayout                m_compositePipelineLayout           = VK_NULL_HANDLE;
            VkPipeline                      m_compositePipeline                 = VK_NULL_HANDLE;

            // Cadena pendiente de dejar en negro (efecto recien apagado o imagenes
            // recien creadas). Una por frame en vuelo: cada slot se limpia en su
            // propio command buffer.
            bool                            m_bloomClearPending[MAX_FRAMES]     = {};
            // Coste GPU medido con timestamps: dos por frame en vuelo, leidos el
            // frame siguiente (la fence de ese frame ya senalizo, asi que el
            // resultado esta disponible sin bloquear).
            VkQueryPool                     m_bloomQueryPool                    = VK_NULL_HANDLE;
            bool                            m_timestampsSupported               = false;
            float                           m_timestampPeriod                   = 0.0f;
            bool                            m_bloomQueryPending[MAX_FRAMES]     = {};
            float                           m_bloomGpuMs                        = 0.0f;
            // Frames medidos. Sirve para soltar UNA linea de coste ya en caliente
            // (igual que "IBL precompute: ..."), en vez de ensuciar el log cada
            // frame: el valor en vivo se ve en el menu View del editor.
            uint32_t                        m_bloomMeasuredFrames               = 0;

            // ── SSAO ─────────────────────────────────────────────────────────
            // R32_SFLOAT y no R8: los formatos de un solo canal a 8 bits exigen
            // shaderStorageImageExtendedFormats para hacer de storage image, y
            // este es de los obligatorios en cualquier implementacion.
            static constexpr VkFormat       kSsaoFormat = VK_FORMAT_R32_SFLOAT;
            // Depth propio del pre-pass, SEPARADO de m_depthImage a proposito: ese
            // lo comparten el pass de escena y el de composicion dentro del mismo
            // framebuffer, y el contorno lo testea en
            // DEPTH_STENCIL_ATTACHMENT_OPTIMAL. Con uno propio no hay que tocar ni
            // su usage ni su layout ni el loadOp de nadie.
            VkImage                         m_ssaoDepthImage[MAX_FRAMES]        = {};
            VkDeviceMemory                  m_ssaoDepthMemory[MAX_FRAMES]       = {};
            VkImageView                     m_ssaoDepthView[MAX_FRAMES]         = {};
            VkFramebuffer                   m_ssaoDepthFb[MAX_FRAMES]           = {};
            VkRenderPass                    m_ssaoDepthRenderPass               = VK_NULL_HANDLE;
            // Reutiliza m_shadowPipelineLayout: mismos dos sets (objeto +
            // instancias) y el mismo rango de push constants, que este pipeline
            // simplemente no usa.
            VkPipeline                      m_ssaoDepthPipeline                 = VK_NULL_HANDLE;
            // Resolucion completa: asi pbr.frag muestrea 1:1 con gl_FragCoord y no
            // hay que llevarle la escala a ningun sitio.
            VkImage                         m_ssaoImage[MAX_FRAMES]             = {};
            VkDeviceMemory                  m_ssaoMemory[MAX_FRAMES]            = {};
            VkImageView                     m_ssaoView[MAX_FRAMES]              = {};
            VkImage                         m_ssaoBlurImage[MAX_FRAMES]         = {};
            VkDeviceMemory                  m_ssaoBlurMemory[MAX_FRAMES]        = {};
            VkImageView                     m_ssaoBlurView[MAX_FRAMES]          = {};
            // NEAREST: ni D32_SFLOAT ni R32_SFLOAT garantizan filtrado lineal, y
            // todos los taps son a texel exacto.
            VkSampler                       m_ssaoSampler                       = VK_NULL_HANDLE;
            VkDescriptorSetLayout           m_ssaoDescLayout                    = VK_NULL_HANDLE;
            VkDescriptorPool                m_ssaoDescPool                      = VK_NULL_HANDLE;
            VkPipelineLayout                m_ssaoPipelineLayout                = VK_NULL_HANDLE;
            VkPipeline                      m_ssaoPipeline                      = VK_NULL_HANDLE;
            VkPipeline                      m_ssaoBlurPipeline                  = VK_NULL_HANDLE;
            VkDescriptorSet                 m_ssaoSets[MAX_FRAMES]              = {};
            VkDescriptorSet                 m_ssaoBlurSets[MAX_FRAMES]          = {};
            // Compartida por ssao.comp y ssao_blur.comp, que comparten pipeline
            // layout (el blur solo lee invRes).
            struct SsaoPush {
                float projP00;
                float projP11;
                float projP22;
                float projP32;
                float invResX;
                float invResY;
                float radius;
                float bias;
                float intensity;
                float power;
            };
            // Con el efecto apagado el mapa tiene que valer 1.0 (identidad) y
            // ademas estar en GENERAL, que es el layout que declaran los
            // descriptor sets. Un clear resuelve las dos cosas de golpe, y solo se
            // graba cuando hay algo que limpiar: al crear las imagenes y al
            // apagar el efecto. Fuera de eso, apagado = cero trabajo por frame.
            bool                            m_ssaoClearPending[MAX_FRAMES]      = {};
            // Queries propias: reutilizar las del bloom mezclaria dos medidas.
            VkQueryPool                     m_ssaoQueryPool                     = VK_NULL_HANDLE;
            bool                            m_ssaoQueryPending[MAX_FRAMES]      = {};
            float                           m_ssaoGpuMs                         = 0.0f;
            uint32_t                        m_ssaoMeasuredFrames                = 0;

            // ── SSR ──────────────────────────────────────────────────────────
            // Reflejo aislado, a resolucion completa y en el MISMO formato que el
            // HDR: ssr_resolve.comp lo suma sobre m_hdrImage y los dos son
            // storage images con el mismo qualifier rgba16f.
            VkImage                         m_ssrImage[MAX_FRAMES]              = {};
            VkDeviceMemory                  m_ssrMemory[MAX_FRAMES]             = {};
            VkImageView                     m_ssrView[MAX_FRAMES]               = {};
            // LINEAR: a diferencia del SSAO, el impacto del rayo cae entre
            // texeles y el color de la escena si tiene garantizado el filtrado
            // lineal en R16G16B16A16_SFLOAT. La profundidad se muestrea con
            // m_ssaoSampler (NEAREST), que es el que le corresponde a D32_SFLOAT.
            VkSampler                       m_ssrSampler                        = VK_NULL_HANDLE;
            // Un unico layout para los dos pipelines: ssr_resolve.comp declara el
            // binding 1 y simplemente no lo lee.
            VkDescriptorSetLayout           m_ssrDescLayout                     = VK_NULL_HANDLE;
            VkDescriptorPool                m_ssrDescPool                       = VK_NULL_HANDLE;
            VkPipelineLayout                m_ssrPipelineLayout                 = VK_NULL_HANDLE;
            VkPipeline                      m_ssrPipeline                       = VK_NULL_HANDLE;
            VkPipeline                      m_ssrResolvePipeline                = VK_NULL_HANDLE;
            VkDescriptorSet                 m_ssrSets[MAX_FRAMES]               = {};
            VkDescriptorSet                 m_ssrResolveSets[MAX_FRAMES]        = {};
            // Compartida por ssr.comp y ssr_resolve.comp, que comparten pipeline
            // layout. 48 bytes: los mismos campos y en el mismo orden que el
            // bloque de los dos .comp.
            struct SsrPush {
                float   projP00;
                float   projP11;
                float   projP22;
                float   projP32;
                float   invResX;
                float   invResY;
                float   maxDistance;
                float   thickness;
                int32_t maxSteps;
                int32_t refineSteps;
                float   edgeFade;
                float   intensity;
            };
            static_assert(sizeof(SsrPush) == 48, "SsrPush debe seguir en 48 bytes: los dos .comp declaran este layout");
            // Cuatro queries por frame: [0,1] el depth pre-pass cuando es el SSR
            // quien lo pide, [2,3] los dos dispatches. Reutilizar las del SSAO o
            // las del bloom mezclaria dos medidas.
            VkQueryPool                     m_ssrQueryPool                      = VK_NULL_HANDLE;
            bool                            m_ssrQueryPending[MAX_FRAMES]       = {};
            // recordSsaoPass marca aqui que dejo escritos los timestamps [0,1] de
            // este frame; recordSsrPass solo da la medida por buena si es asi (si
            // no, el par no se habria escrito y la lectura daria NOT_READY).
            bool                            m_ssrStampedPrepass                 = false;
            float                           m_ssrGpuMs                          = 0.0f;
            uint32_t                        m_ssrMeasuredFrames                 = 0;

            // ── Niebla volumetrica ───────────────────────────────────────────
            // Un solo pipeline y un set por frame: HDR como storage (se lee y
            // se reescribe in situ), la profundidad del pre-pass, el UBO del
            // frame (matrices de cascada) y el shadow map de la luz key.
            VkDescriptorSetLayout           m_fogDescLayout                     = VK_NULL_HANDLE;
            VkDescriptorPool                m_fogDescPool                       = VK_NULL_HANDLE;
            VkPipelineLayout                m_fogPipelineLayout                 = VK_NULL_HANDLE;
            VkPipeline                      m_fogPipeline                       = VK_NULL_HANDLE;
            VkDescriptorSet                 m_fogSets[MAX_FRAMES]               = {};
            // 128 bytes exactos -el minimo que Vulkan garantiza-: los mismos
            // campos y en el mismo orden que el bloque de fog.comp.
            struct FogPush {
                glm::mat4 invViewProj;
                glm::vec4 camPosDensity;
                glm::vec4 lightDirFalloff;
                glm::vec4 scatterBaseHeight;
                glm::vec4 gStepsRes;
            };
            static_assert(sizeof(FogPush) == 128, "FogPush debe seguir en 128 bytes: fog.comp declara este layout");
            // Dos queries por frame que acotan el unico dispatch. El depth
            // pre-pass NO entra aqui: ya lo miden el SSAO o el SSR cuando son
            // ellos quienes lo piden.
            VkQueryPool                     m_fogQueryPool                      = VK_NULL_HANDLE;
            bool                            m_fogQueryPending[MAX_FRAMES]       = {};
            float                           m_fogGpuMs                          = 0.0f;
            uint32_t                        m_fogMeasuredFrames                 = 0;

            // ── Anti-aliasing ────────────────────────────────────────────────
            // Modo PEDIDO: lo que ha elegido el usuario y lo que devuelve
            // aaMode(). Puede ir un frame por delante de los recursos.
            // Modo CONSTRUIDO: el que corresponde a las imagenes, framebuffers y
            // pipelines que existen AHORA MISMO. Es el que manda al grabar el
            // frame. Los dos se separan porque setAaMode se llama desde la UI, y
            // la UI se construye DESPUES del punto en el que se pueden recrear
            // recursos: sin esta distincion, el frame del click se grabaria en el
            // modo nuevo con los framebuffers del viejo (o sin ninguno).
            AaMode                          m_aaActiveMode                      = AaMode::None;
            // Lo marca setAaMode/setSsaaFactor/setMsaaSamples cuando el cambio
            // toca recursos (tamano de los targets o numero de muestras). Lo
            // consume drawFrame ANTES de grabar nada, con vkDeviceWaitIdle: en
            // mitad de un frame en vuelo no se puede destruir una imagen.
            bool                            m_aaResourcesDirty                  = false;
            // Resolucion INTERNA del render. Igual a m_swapChainExtent salvo en
            // SSAA, donde es m_swapChainExtent * m_ssaaFactor. Gobierna TODOS los
            // targets intermedios (escena HDR, depth, SSAO, SSR, bloom, la imagen
            // intermedia del AA) y sus viewports. m_swapChainExtent se queda para
            // lo que de verdad tiene el tamano de la ventana: el swapchain, el
            // pass de UI, el blit y m_offscreenImage.
            VkExtent2D                      m_renderExtent                      = {};
            // Tamano al que se PRESENTA la imagen: el del panel del editor, o el
            // del swapchain cuando nadie lo ha fijado (runtime y headless). Es el
            // tamano de m_offscreenImage y del pass de resolucion del AA.
            // PEDIDO: lo ultimo que ha reportado el panel. Igual que con el modo,
            // llega desde la UI y puede ir un frame por delante de los recursos.
            VkExtent2D                      m_viewportExtent                    = {};
            // CONSTRUIDO: el tamano con el que existen ahora las imagenes y los
            // framebuffers. Es el que manda al grabar, y el unico que puede
            // aparecer en un renderArea: pedir un area mayor que el framebuffer
            // es invalido.
            VkExtent2D                      m_viewportActive                    = {};
            VkExtent2D effectiveViewport() const
            {
                return m_viewportActive.width > 0 && m_viewportActive.height > 0
                     ? m_viewportActive : m_swapChainExtent;
            }
            // Destino ALTERNATIVO del pass de composicion cuando el modo activo
            // necesita un pass de resolucion detras (FXAA, SSAA, TAA). Tiene el
            // tamano de m_renderExtent, que en SSAA NO es el de la ventana. Con
            // None y con MSAA no se usa: la composicion escribe directamente en
            // m_offscreenImage, que es la que ve la UI y la que blitea el runtime.
            VkImage                         m_aaSrcImage[MAX_FRAMES]            = {};
            VkDeviceMemory                  m_aaSrcMemory[MAX_FRAMES]           = {};
            VkImageView                     m_aaSrcView[MAX_FRAMES]             = {};
            // Framebuffer del pass de COMPOSICION apuntando a m_aaSrcImage (mas
            // el mismo depth compartido, que el contorno y los gizmos siguen
            // necesitando cargado).
            VkFramebuffer                   m_aaSrcFramebuffer[MAX_FRAMES]      = {};
            // Pass de resolucion: solo color, sin depth, a tamano de VENTANA.
            // Escribe en m_offscreenImage. Es donde corren FXAA, el downsample
            // del SSAA y la acumulacion del TAA, cada uno con su pipeline.
            VkRenderPass                    m_aaRenderPass                      = VK_NULL_HANDLE;
            VkFramebuffer                   m_aaFramebuffer[MAX_FRAMES]         = {};
            // Sampler propio: los tres modos necesitan filtrado LINEAL (muestrean
            // entre texeles) y clamp en los bordes de la pantalla.
            VkSampler                       m_aaSampler                         = VK_NULL_HANDLE;
            // Un binding (la imagen intermedia): lo comparten FXAA y SSAA.
            VkDescriptorSetLayout           m_aaDescLayout                      = VK_NULL_HANDLE;
            VkDescriptorPool                m_aaDescPool                        = VK_NULL_HANDLE;
            VkPipelineLayout                m_fxaaPipelineLayout                = VK_NULL_HANDLE;
            VkPipeline                      m_fxaaPipeline                      = VK_NULL_HANDLE;
            VkPipelineLayout                m_ssaaPipelineLayout                = VK_NULL_HANDLE;
            VkPipeline                      m_ssaaPipeline                      = VK_NULL_HANDLE;
            VkDescriptorSet                 m_aaSets[MAX_FRAMES]                = {};
            // Layout declarado igual en fxaa.frag. Cambiar el orden o el tamano
            // aqui sin tocar el shader no da ningun error: solo colores raros.
            struct FxaaPush {
                float invResX;
                float invResY;
                float subpix;
                float edgeThreshold;
                float edgeThresholdMin;
            };
            static_assert(sizeof(FxaaPush) == 20, "FxaaPush debe seguir en 20 bytes: fxaa.frag declara este layout");

            // SSAA: mismo layout que declara ssaa_resolve.frag.
            struct SsaaPush {
                float invSrcX;      // 1/ancho de la imagen intermedia (la grande)
                float invSrcY;
                int32_t taps;       // muestras por eje del filtro de bajada
            };
            static_assert(sizeof(SsaaPush) == 12, "SsaaPush debe seguir en 12 bytes: ssaa_resolve.frag declara este layout");
            float                           m_ssaaFactor                        = 2.0f;

            // MSAA. m_msaaSamples es lo que PIDE el usuario; m_aaSampleCount es
            // lo que esta construido ahora mismo en las imagenes, los render
            // passes y los pipelines: los dos solo coinciden cuando el modo
            // activo es Msaa y ya se ha reconstruido.
            VkSampleCountFlagBits           m_aaSampleCount                     = VK_SAMPLE_COUNT_1_BIT;
            // Color multisample de la escena: se RESUELVE sobre m_hdrImage al
            // cerrar el pass, asi que el SSAO, el SSR, el bloom y la composicion
            // siguen leyendo exactamente la misma imagen de una muestra que hoy.
            // No lleva STORAGE: las storage images multisample exigen la feature
            // shaderStorageImageMultisample, que no se pide.
            VkImage                         m_msaaHdrImage[MAX_FRAMES]          = {};
            VkDeviceMemory                  m_msaaHdrMemory[MAX_FRAMES]         = {};
            VkImageView                     m_msaaHdrView[MAX_FRAMES]           = {};
            // Color multisample de la composicion, con resolve sobre
            // m_offscreenImage. Existe para que el contorno y los gizmos, que se
            // dibujan en ese pass, tambien salgan suavizados: el depth que cargan
            // es el multisample de la escena y no hay forma de resolverlo en
            // Vulkan 1.0 (VK_KHR_depth_stencil_resolve es 1.2).
            VkImage                         m_msaaLdrImage[MAX_FRAMES]          = {};
            VkDeviceMemory                  m_msaaLdrMemory[MAX_FRAMES]         = {};
            VkImageView                     m_msaaLdrView[MAX_FRAMES]           = {};

            // TAA: historial de dos frames en ping-pong (se lee el del frame
            // anterior y se escribe el de este) mas el pass de acumulacion.
            VkImage                         m_taaHistoryImage[MAX_FRAMES]       = {};
            VkDeviceMemory                  m_taaHistoryMemory[MAX_FRAMES]      = {};
            VkImageView                     m_taaHistoryView[MAX_FRAMES]        = {};
            VkFramebuffer                   m_taaHistoryFramebuffer[MAX_FRAMES] = {};
            // Tres bindings: color de este frame, historial y profundidad.
            VkDescriptorSetLayout           m_taaDescLayout                     = VK_NULL_HANDLE;
            VkDescriptorPool                m_taaDescPool                       = VK_NULL_HANDLE;
            VkPipelineLayout                m_taaPipelineLayout                 = VK_NULL_HANDLE;
            VkPipeline                      m_taaPipeline                       = VK_NULL_HANDLE;
            VkDescriptorSet                 m_taaSets[MAX_FRAMES]               = {};
            // Pass que escribe en el historial. Identico al de resolucion salvo
            // por el finalLayout, que aqui deja la imagen lista para MUESTREARLA
            // el frame siguiente en vez de para presentarla.
            VkRenderPass                    m_taaHistoryRenderPass              = VK_NULL_HANDLE;
            // Layout que declara taa.frag. La reproyeccion viaja como UNA matriz
            // (clip de este frame -> clip del anterior) en vez de dos: dos mat4
            // mas el resto se saldrian de los 128 bytes garantizados.
            struct TaaPush {
                glm::mat4 reproject;
                float     invResX;
                float     invResY;
                float     feedback;
                int32_t   historyValid;
            };
            static_assert(sizeof(TaaPush) == 80, "TaaPush debe seguir en 80 bytes: taa.frag declara este layout");
            // Indice dentro de la secuencia de Halton del jitter de este frame.
            uint32_t                        m_taaJitterIndex                    = 0;
            // Jitter aplicado ESTE frame, en unidades de clip space. Lo necesita
            // taa.frag para deshacerlo al reproyectar.
            glm::vec2                       m_taaJitter                         = glm::vec2(0.0f);
            // La proyeccion CON jitter de este frame. La escribe
            // updateUniformBuffer (que corre antes de grabar) y la usa tambien el
            // skybox: si el skybox se dibujara con la proyeccion sin jitter se
            // desalinearia medio pixel de la geometria y el TAA lo veria como un
            // borde en movimiento permanente.
            glm::mat4                       m_taaJitteredProj                   = glm::mat4(1.0f);
            // View-proj SIN jitter del frame anterior, para la reproyeccion. Y el
            // flag de si ese historial sirve: tras un resize, un cambio de modo o
            // el primer frame no hay nada valido que acumular.
            glm::mat4                       m_taaPrevViewProj                   = glm::mat4(1.0f);
            // La de ESTE frame, tambien sin jitter. La deja recordCommandBuffer y
            // la consume recordAaPass, que corre despues dentro del mismo frame.
            glm::mat4                       m_taaCurrViewProj                   = glm::mat4(1.0f);
            bool                            m_taaHistoryValid                   = false;

            // Queries propias: reutilizar las del bloom mezclaria el coste del
            // tonemap con el del anti-aliasing. Dos pares por frame: [0,1] el
            // pass propio del modo, [2,3] el render completo sin UI.
            VkQueryPool                     m_aaQueryPool                       = VK_NULL_HANDLE;
            bool                            m_aaQueryPending[MAX_FRAMES]        = {};
            bool                            m_aaPassStamped[MAX_FRAMES]         = {};
            float                           m_aaGpuMs                           = 0.0f;
            float                           m_renderGpuMs                       = 0.0f;
            uint32_t                        m_aaMeasuredFrames                  = 0;

            // ── Panel Performance ────────────────────────────────────────────
            // Cuatro queries por frame en vuelo: [0,1] pass de sombras, [2,3]
            // pass de escena. Solo se resetean y escriben si m_perfCapture, y
            // los resultados se leen sin WAIT_BIT del slot de hace dos frames.
            VkQueryPool                     m_perfQueryPool                     = VK_NULL_HANDLE;
            bool                            m_perfQueryPending[MAX_FRAMES]      = {};
            bool                            m_perfCapture                       = false;
            float                           m_shadowGpuMs                       = 0.0f;
            float                           m_sceneGpuMs                        = 0.0f;
            int                             m_statDrawCalls                     = 0;
            int                             m_statInstances                     = 0;
            int                             m_statCulled                        = 0;

            // ── Forward+ ─────────────────────────────────────────────────────
            // Tope de luces que entran en el culling y, a la vez, ancho de la
            // mascara de bits de light_cull_tiled.comp (256 / 32 = 8 palabras).
            static constexpr uint32_t       kFpMaxLights                        = 256;
            // Tope de luces por celda. Una celda que se pase PIERDE luces: por
            // eso se cuentan aparte y se enseñan en la UI.
            static constexpr uint32_t       kFpMaxPerCell                       = 64;
            static constexpr uint32_t       kFpTileSize                         = 16;   // tiled
            static constexpr uint32_t       kFpClusterTile                      = 64;   // clustered, XY
            static constexpr uint32_t       kFpClusterSlices                    = 24;   // clustered, Z
            // Modo PEDIDO (el que devuelve forwardPlusMode()) y modo CONGELADO
            // del frame. Mismo motivo que en el AA: setForwardPlusMode se llama
            // desde la UI, que se construye a mitad de drawFrame, y el bloque de
            // parametros que lee pbr.frag se escribe una sola vez por frame. Sin
            // esta separacion, un click podria dejar el frame con la rejilla de un
            // modo y la lectura del otro.
            FpMode                          m_fpActiveMode                      = FpMode::Off;
            std::vector<float>              m_lightRadii;
            // Bloque de parametros tal cual lo declaran los dos .comp y pbr.frag.
            // std430 con puros escalares de 4 bytes: los offsets son secuenciales.
            struct FpParamsGpu {
                uint32_t mode;
                uint32_t gridX;
                uint32_t gridY;
                uint32_t gridZ;
                uint32_t tileSize;
                uint32_t maxPerCell;
                uint32_t numLights;
                uint32_t pad0;
                float    zNear;
                float    zFar;
                float    sliceScale;
                float    sliceBias;
            };
            static_assert(sizeof(FpParamsGpu) == 48, "FpParamsGpu debe seguir en 48 bytes: los dos .comp y pbr.frag declaran este layout");
            // Una luz del SSBO. viewPosR es la MISMA luz en view space: la calcula
            // la CPU para que el culling no necesite la matriz de vista.
            struct FpLightGpu {
                glm::vec4 posRadius;
                glm::vec4 color;
                glm::vec4 viewPosR;
                // Los dos campos de tipo de DonTopo::Light. Sin ellos el
                // fragment shader no sabria evaluar un spot ni una directional
                // por la ruta Forward+, y el binning no podria dejar la
                // directional siempre visible.
                glm::vec4 direction;    // xyz dir, w tipo
                glm::vec4 params;       // range, cos interior, cos exterior, ancho
            };
            static_assert(sizeof(FpLightGpu) == 80, "FpLightGpu debe seguir en 80 bytes: es el stride std430 del array de luces");
            // Push constant compartida por los dos .comp.
            struct FpPush {
                float    p00;
                float    p11;
                float    p22;
                float    p32;
                uint32_t screenW;
                uint32_t screenH;
                uint32_t pad0;
                uint32_t pad1;
            };
            static_assert(sizeof(FpPush) == 32, "FpPush debe seguir en 32 bytes: los dos .comp declaran este layout");
            // Set propio (el 2 en el pipeline de escena, el 0 en el de culling: es
            // el MISMO VkDescriptorSet, y un set vale en cualquier indice mientras
            // el layout coincida). Seis bindings: params, luces, rejilla, indices,
            // profundidad del pre-pass (solo compute) y contadores (solo compute).
            VkDescriptorSetLayout           m_fpDescLayout                      = VK_NULL_HANDLE;
            VkDescriptorPool                m_fpDescPool                        = VK_NULL_HANDLE;
            VkPipelineLayout                m_fpPipelineLayout                  = VK_NULL_HANDLE;
            VkPipeline                      m_fpTiledPipeline                   = VK_NULL_HANDLE;
            VkPipeline                      m_fpClusteredPipeline               = VK_NULL_HANDLE;
            VkDescriptorSet                 m_fpSets[MAX_FRAMES]                = {};
            // Params, luces y contadores NO dependen del tamano: se crean una vez
            // en createFpPipelines y viven hasta el shutdown. Mapeados en
            // persistente, igual que el UBO.
            VkBuffer                        m_fpParamsBuffer[MAX_FRAMES]        = {};
            VkDeviceMemory                  m_fpParamsMemory[MAX_FRAMES]        = {};
            void*                           m_fpParamsMapped[MAX_FRAMES]        = {};
            VkBuffer                        m_fpLightBuffer[MAX_FRAMES]         = {};
            VkDeviceMemory                  m_fpLightMemory[MAX_FRAMES]         = {};
            void*                           m_fpLightMapped[MAX_FRAMES]         = {};
            VkBuffer                        m_fpStatsBuffer[MAX_FRAMES]         = {};
            VkDeviceMemory                  m_fpStatsMemory[MAX_FRAMES]         = {};
            void*                           m_fpStatsMapped[MAX_FRAMES]         = {};
            // Rejilla e indices SI dependen del tamano: van con el swapchain,
            // colgados de createOffscreenImages/destroyOffscreenImages. Se
            // dimensionan al MAYOR de las dos rejillas para que cambiar de modo no
            // tenga que recrear nada.
            VkBuffer                        m_fpGridBuffer[MAX_FRAMES]          = {};
            VkDeviceMemory                  m_fpGridMemory[MAX_FRAMES]          = {};
            VkBuffer                        m_fpIndexBuffer[MAX_FRAMES]         = {};
            VkDeviceMemory                  m_fpIndexMemory[MAX_FRAMES]         = {};
            VkQueryPool                     m_fpQueryPool                       = VK_NULL_HANDLE;
            bool                            m_fpQueryPending[MAX_FRAMES]        = {};
            float                           m_fpGpuMs                           = 0.0f;
            float                           m_fpAvgPerCell                      = 0.0f;
            uint32_t                        m_fpOverflowCells                   = 0;
            uint32_t                        m_fpMeasuredFrames                  = 0;

            VkSemaphore                     m_imageAvailable[MAX_FRAMES]        = {};
            std::vector<VkSemaphore>        m_renderFinished;
            VkFence                         m_inFlight[MAX_FRAMES]              = {};
            int                             m_currentFrame                      = 0;
            VkPipelineLayout                m_pipelineLayout                    = VK_NULL_HANDLE;
            VkPipeline                      m_pipeline                          = VK_NULL_HANDLE;
            VkPipeline                      m_wireframePipeline                 = VK_NULL_HANDLE;
            // Casco invertido del objeto seleccionado. Dos pipelines por el
            // mismo motivo que los dos wireframe: el vertex input estatico
            // (Vertex) y el skinned (OutputVertex) tienen stride distinto.
            VkPipeline                      m_outlinePipeline                   = VK_NULL_HANDLE;
            VkPipeline                      m_skinnedOutlinePipeline            = VK_NULL_HANDLE;
            // Variante LINE de los dos anteriores, para cuando la escena se
            // dibuja en wireframe: ahí el interior del objeto no escribe
            // profundidad (solo lo hacen las aristas rasterizadas), así que un
            // casco relleno pasaría el depth test entero y taparía el objeto de
            // color plano en vez de bordearlo.
            VkPipeline                      m_outlineWirePipeline               = VK_NULL_HANDLE;
            VkPipeline                      m_skinnedOutlineWirePipeline        = VK_NULL_HANDLE;
            // Objeto resaltado, -1 = ninguno. Solo los fija el editor
            // (setOutlineTarget); en runtime se quedan en -1 para siempre.
            int                             m_outlineStaticIndex                = -1;
            int                             m_outlineSkinnedIndex               = -1;
            bool                            m_framebufferResized                = false;
            bool                            m_headless                          = false;
            VkDescriptorSetLayout           m_descriptorSetLayout               = VK_NULL_HANDLE;
            VkBuffer                        m_uniformBuffers[MAX_FRAMES]        = {};
            VkDeviceMemory                  m_uniformBuffersMemory[MAX_FRAMES]  = {};
            void*                           m_uniformBuffersMapped[MAX_FRAMES]  = {};
            VkDescriptorPool                m_descriptorPool                    = VK_NULL_HANDLE;
            // ── SSBO de transforms por instancia (set 1, binding 0) ──────────
            // Uno por frame-in-flight y mapeado en persistente: el frame
            // anterior puede seguir en vuelo leyendo el suyo. Los dos passes
            // (sombras primero, escena después) comparten el buffer del frame:
            // m_instanceCursor es el nº de matrices ya escritas y hace de base
            // del siguiente pass.
            VkDescriptorSetLayout           m_instanceDescLayout                = VK_NULL_HANDLE;
            VkDescriptorPool                m_instanceDescPool                  = VK_NULL_HANDLE;
            VkDescriptorSet                 m_instanceDescSets[MAX_FRAMES]      = {};
            VkBuffer                        m_instanceBuffers[MAX_FRAMES]       = {};
            VkDeviceMemory                  m_instanceMemory[MAX_FRAMES]        = {};
            void*                           m_instanceMapped[MAX_FRAMES]        = {};
            uint32_t                        m_instanceCapacity[MAX_FRAMES]      = {}; // en matrices
            uint32_t                        m_instanceCursor                    = 0;
            // Scratch reutilizado entre frames y entre passes: el agrupado corre
            // dos veces por frame y no debe alojar nada en ese camino.
            std::vector<BatchCandidate>     m_batchCandidates;
            std::vector<InstanceBatch>      m_instanceBatches;
            // Visibilidad de m_skinnedObjects de ESTE frame, en el mismo orden e
            // indexada igual. Se calcula una sola vez al principio de
            // recordCommandBuffer porque la leen dos sitios: el compute (que va
            // primero en el command buffer) y el dibujo. Que compartan la misma
            // decisión es lo que evita el pop: si el compute se saltara un
            // objeto que luego SÍ se dibuja, su outputVertexBuffer conservaría
            // la pose del último frame en que fue visible.
            std::vector<uint8_t>            m_skinnedVisible;
            void createInstanceResources();
            // Asegura sitio para `matrices` en el buffer del frame actual. Se
            // llama al principio de recordCommandBuffer, con la fence del frame
            // ya esperada: recrear el buffer aquí no pisa nada en vuelo.
            void ensureInstanceCapacity(uint32_t matrices);
            void destroyInstanceBuffer(int frame);
            VkImage                         m_depthImage                        = VK_NULL_HANDLE;
            VkDeviceMemory                  m_depthImageMemory                  = VK_NULL_HANDLE;
            VkImageView                     m_depthImageView                    = VK_NULL_HANDLE;
            glm::vec3                       m_cameraTarget{0.0f};
            float                           m_cameraDistance{5.0f};
            glm::mat4                       m_viewMatrix{1.0f};
            Camera                          m_camera;
            std::vector<Light>              m_lights;

            // Shadow Map (cascadas)
            static constexpr uint32_t       SHADOW_SIZE                         = 2048;
            VkImage                         m_shadowImage                       = VK_NULL_HANDLE;
            VkDeviceMemory                  m_shadowMemory                      = VK_NULL_HANDLE;
            // Vista del array completo: es la que muestrea el fragment shader
            // (sampler2DArrayShadow) y la que va en los descriptor sets.
            VkImageView                     m_shadowView                        = VK_NULL_HANDLE;
            // Una vista de UNA capa por cascada. Solo existen para poder colgar
            // un framebuffer de cada capa; nadie las muestrea.
            VkImageView                     m_shadowLayerViews[SHADOW_CASCADES] {};
            VkSampler                       m_shadowSampler                     = VK_NULL_HANDLE;
            VkRenderPass                    m_shadowRenderPass                  = VK_NULL_HANDLE;
            VkFramebuffer                   m_shadowFramebuffers[SHADOW_CASCADES] {};
            VkPipeline                      m_shadowPipeline                    = VK_NULL_HANDLE;
            // Hermano del anterior para las mallas skinned: mismo shadow.vert,
            // mismo layout, mismo render pass y mismo bias. Solo cambia el
            // vertex input, porque lo que se dibuja es la salida del compute de
            // skinning (5×vec4, stride 80) y el stride es estado de pipeline.
            VkPipeline                      m_shadowSkinnedPipeline             = VK_NULL_HANDLE;
            VkPipelineLayout                m_shadowPipelineLayout              = VK_NULL_HANDLE;
            // Caché por frame que rellena computeCascades(). Identidad y 0 si la
            // escena no tiene luces: en ese caso el pass solo limpia las capas.
            glm::mat4                       m_cascadeMatrices[SHADOW_CASCADES]  { glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) };
            glm::vec4                       m_cascadeSplits                     { 0.0f };

            // ── IBL ────────────────────────────────────────────────────────
            // Dos cubemaps precomputados UNA vez sobre el cubemap del skybox:
            // irradiancia (difuso) y entorno prefiltrado por rugosidad (mips).
            // El termino BRDF no es una textura: pbr.frag usa la aproximacion
            // analitica de Karis, asi que no hay LUT ni un tercer binding.
            //
            // Las imagenes se crean SIEMPRE en init(), con contenido neutro, y
            // solo se rellenan de verdad si initSkybox() ha cargado un cubemap.
            // Asi los descriptor sets nunca apuntan a un handle nulo y una
            // escena sin skybox se ilumina con un ambiente plano en vez de
            // reventar.
            static constexpr uint32_t       IBL_IRRADIANCE_SIZE = 32;
            static constexpr uint32_t       IBL_PREFILTER_SIZE  = 128;
            // Si cambia, cambia tambien IBL_PREFILTER_MIPS en shaders/pbr.frag:
            // ahi va como #define a proposito, pa no tocar el bloque UBO (que
            // esta declarado en 5 shaders y std140 desplazaria en silencio).
            static constexpr uint32_t       IBL_PREFILTER_MIPS  = 5;
            VkImage                         m_iblIrradianceImage   = VK_NULL_HANDLE;
            VkDeviceMemory                  m_iblIrradianceMemory  = VK_NULL_HANDLE;
            // Vista CUBE pa muestrear desde pbr.frag; vista 2D_ARRAY pa que el
            // compute pueda escribirla como storage image (un imageCube de
            // escritura exigiria capacidades que no hacen falta).
            VkImageView                     m_iblIrradianceView    = VK_NULL_HANDLE;
            VkImageView                     m_iblIrradianceStore   = VK_NULL_HANDLE;
            VkImage                         m_iblPrefilterImage    = VK_NULL_HANDLE;
            VkDeviceMemory                  m_iblPrefilterMemory   = VK_NULL_HANDLE;
            VkImageView                     m_iblPrefilterView     = VK_NULL_HANDLE;
            VkImageView                     m_iblPrefilterStore[IBL_PREFILTER_MIPS] {};
            VkSampler                       m_iblSampler           = VK_NULL_HANDLE;
            VkDescriptorSetLayout           m_iblDescLayout        = VK_NULL_HANDLE;
            VkDescriptorPool                m_iblDescPool          = VK_NULL_HANDLE;
            VkPipelineLayout                m_iblPipelineLayout    = VK_NULL_HANDLE;
            VkPipeline                      m_iblIrradiancePipeline = VK_NULL_HANDLE;
            VkPipeline                      m_iblPrefilterPipeline  = VK_NULL_HANDLE;
            // intensity: peso que se hornea en el cubemap resultante. 1.0 en el
            // IBL global (resultado identico al de antes de las sondas) y la
            // intensidad de la probe cuando esto convoluciona su captura.
            struct IblPush { float roughness; uint32_t faceSize; float intensity; };

            // ── Reflection probes ──────────────────────────────────────────
            // Sondas de entorno: capturan la escena desde su posicion en 6 caras
            // y sustituyen al IBL global (bindings 5 y 6 del set 0) en los
            // objetos que caen dentro de su radio. El bake es un EVENTO: no
            // graba ni un comando en el command buffer del frame, asi que el
            // coste GPU por frame con N sondas ya bakeadas es exactamente el
            // mismo que con 0.
            //
            // Lado de captura: un solo cubemap COMPARTIDO por todas las sondas
            // (es un intermedio del bake, no persiste), creado la primera vez
            // que hay algo que bakear. Sin sondas no se crea y no gasta nada.
            static constexpr uint32_t PROBE_FACE_SIZE = 128;
            struct GpuProbe
            {
                uint64_t  ownerId  = 0;          // GameObject::id de la sonda
                glm::vec3 position { 0.0f };
                float     radius    = 0.0f;
                float     intensity = 1.0f;
                // Mismas dos imagenes que el IBL global, por sonda: irradiancia
                // (1 mip) y entorno prefiltrado (IBL_PREFILTER_MIPS).
                VkImage        irradianceImage  = VK_NULL_HANDLE;
                VkDeviceMemory irradianceMemory = VK_NULL_HANDLE;
                VkImageView    irradianceView   = VK_NULL_HANDLE;
                VkImageView    irradianceStore  = VK_NULL_HANDLE;
                VkImage        prefilterImage   = VK_NULL_HANDLE;
                VkDeviceMemory prefilterMemory  = VK_NULL_HANDLE;
                VkImageView    prefilterView    = VK_NULL_HANDLE;
                VkImageView    prefilterStore[IBL_PREFILTER_MIPS] {};
                bool           baked  = false;   // false: todavia con el neutro
                float          bakeMs = 0.0f;    // ultimo bake, timestamps GPU
                // Llamadas a syncReflectionProbes seguidas SIN cambios en los
                // ajustes de la sonda. El auto-bake espera a que llegue a 1: sin
                // esto, arrastrar el slider de Intensity dispararia un bake por
                // frame (con su vkDeviceWaitIdle y sus 7 submits).
                int            settleFrames = 0;
            };
            std::vector<GpuProbe>           m_probes;
            VkImage                         m_probeCaptureImage  = VK_NULL_HANDLE;
            VkDeviceMemory                  m_probeCaptureMemory = VK_NULL_HANDLE;
            VkImageView                     m_probeCaptureView   = VK_NULL_HANDLE;
            // 7 pares: uno por cara mas el de la convolucion. Se suman los
            // deltas en vez de medir del primero al ultimo, que contaria tambien
            // las esperas del host entre submits.
            static constexpr uint32_t       PROBE_QUERY_COUNT = 14;
            VkQueryPool                     m_probeQueryPool     = VK_NULL_HANDLE;
            std::vector<uint64_t>           m_probeBakeQueue;
            bool                            m_probeBakeAllQueued = false;
            float                           m_probeLastBakeMs    = 0.0f;
            // Asignacion resuelta: sharedIndex -> indice en m_probes (-1 = IBL
            // global). Es la CACHE de lo ya escrito en los descriptor sets; solo
            // se reescriben bindings cuando el mapa recalculado difiere de este.
            std::unordered_map<int, int>    m_probeAssignShared;
            std::vector<int>                m_probeAssignSkinned;
            // El bake copia el UBO del frame 0 (luces, cascadas y su shadow map)
            // y solo le sustituye view/proj: sin un frame previo ese buffer es
            // basura, asi que las peticiones esperan.
            bool                            m_uboWritten[MAX_FRAMES] {};

            void  syncReflectionProbes();
            void  createProbeCapture();
            void  createProbeImages(GpuProbe& probe);
            void  destroyProbeImages(GpuProbe& probe);
            void  bakeProbe(GpuProbe& probe);
            void  refreshProbeAssignment();
            void  assignAllToGlobalIbl();
            int   pickProbeFor(const glm::vec3& worldPos) const;
            void  writeIblBindings(VkDescriptorSet set, VkImageView irradiance, VkImageView prefilter);
            // Compute pipelines
            VkPipeline            m_boneEvalPipeline      = VK_NULL_HANDLE;
            VkPipeline            m_boneHierarchyPipeline = VK_NULL_HANDLE;
            VkPipeline            m_skinningPipeline      = VK_NULL_HANDLE;
            VkPipeline            m_skinnedGfxPipeline        = VK_NULL_HANDLE;
            VkPipeline            m_skinnedWireframePipeline  = VK_NULL_HANDLE;
            VkPipelineLayout      m_computePipelineLayout = VK_NULL_HANDLE;
            VkDescriptorSetLayout m_computeDescLayout     = VK_NULL_HANDLE;
            VkDescriptorPool      m_computeDescPool       = VK_NULL_HANDLE;
            std::vector<SkinnedRenderObject> m_skinnedObjects;

            std::vector<RenderObject> m_objects;

            // Recursos GPU compartidos por los objetos estáticos. Los objetos
            // guardan un índice aquí; la tabla los mantiene vivos mientras
            // quede algún holder. (Los skinned no comparten: sus SSBOs de
            // salida los escribe el compute por instancia.)
            SharedGpuMeshCache m_sharedMeshes;

            // Batch abierto donde caen los uploads del pump actual. Se envía en
            // flushPendingUploads() y pasa a m_inFlightBatches.
            std::unique_ptr<TransferBatch> m_pendingBatch;
            struct InFlightBatch { uint64_t ticket; std::unique_ptr<TransferBatch> batch; };
            std::vector<InFlightBatch>     m_inFlightBatches;
            uint64_t                       m_nextUploadTicket      = 1;
            uint64_t                       m_lastCompletedTicket   = 0;

            DeferredDeleteQueue m_deferredDeletes;

            UiLayer* m_ui = nullptr;
            Skybox   m_skybox;
            SplashScreen m_splash;

            // UI de juego. Vive en Core: el runtime exportado dibuja este mismo
            // canvas dentro del pass de composicion, sin nada del editor.
            UiCanvas      m_uiCanvas;
            UiSpriteBatch m_uiBatch;
            UiDrawData    m_uiDrawData;
            std::vector<std::unique_ptr<UiTextureAtlas>> m_uiAtlases;
            std::vector<std::unique_ptr<UiFont>>         m_uiFonts;
            GameObject* m_sceneRoot = nullptr;
            Scene* m_scene = nullptr;
    };
}