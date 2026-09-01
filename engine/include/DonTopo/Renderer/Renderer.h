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
#include "DonTopo/Renderer/Frustum.h"
#include "DonTopo/Renderer/SlotPool.h"
#include "DonTopo/Renderer/SkinnedBounds.h"
#include "DonTopo/Renderer/InstanceBatching.h"
#include "DonTopo/Renderer/RendererState.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/GpuResources.h"
#include "DonTopo/Renderer/SharedGpuMesh.h"
#include "DonTopo/Renderer/RenderObjects.h"
#include "DonTopo/Renderer/DeferredDelete.h"
#include "DonTopo/Renderer/TransferBatch.h"
#include "DonTopo/Renderer/AsyncAssetLoader.h"
#include "DonTopo/Renderer/UiLayer.h"
#include "DonTopo/Renderer/Skybox.h"
#include "DonTopo/Renderer/SplashScreen.h"
#include "DonTopo/Renderer/Passes/AaPass.h"
#include "DonTopo/Renderer/Passes/BloomPass.h"
#include "DonTopo/Renderer/Passes/DepthPrepassPass.h"
#include "DonTopo/Renderer/Passes/FogPass.h"
#include "DonTopo/Renderer/Passes/ForwardPlusPass.h"
#include "DonTopo/Renderer/Passes/IblPass.h"
#include "DonTopo/Renderer/Passes/ReflectionProbePass.h"
#include "DonTopo/Renderer/Passes/ShadowPass.h"
#include "DonTopo/Renderer/Passes/SkinningPass.h"
#include "DonTopo/Renderer/Passes/SsaoPass.h"
#include "DonTopo/Renderer/Passes/SsrPass.h"
#include "DonTopo/Renderer/Passes/MotionBlurPass.h"
#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiSpriteBatch.h"
#include "DonTopo/UI/UiTextureAtlas.h"
#include "DonTopo/UI/UiFont.h"
#include "DonTopo/UI/UiWidgetSync.h"
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

            // Canvas de la UI de JUEGO (no el editor), el de PANTALLA. Se dibuja
            // dentro del pass de composicion, encima de la escena y debajo de
            // ImGui, y vacio no cuesta ni un comando. Devolver la referencia hace
            // de getter y de setter: el arbol se monta sobre uiCanvas().root().
            //
            // Con N canvas (Task 5 del canvas de mundo) esto ya no es un campo:
            // busca el primer slot de pantalla entre m_uiSlots. Sin ninguno (la
            // escena solo tiene canvas de mundo, o ninguno) cae a
            // m_uiCanvasFallback, que es persistente y vacio — devolver la
            // referencia a un temporal dejaria a los gizmos leyendo memoria
            // muerta.
            UiCanvas&       uiCanvas() override;
            const UiCanvas& uiCanvas() const;

            // TODOS los de pantalla, en orden de prioridad de input (el de mas
            // arriba primero). Es lo que reparte el raton entre varios canvas.
            void screenUiCanvases(std::vector<UiCanvas*>& out) override;

            // El canvas de un GameObject por su id (nullptr si no tiene). Lo usa
            // el gizmo del canvas seleccionado, que no puede tirar de uiCanvas().
            const UiCanvas* uiCanvasOf(uint64_t ownerId) const override;

            // Monta el arbol vivo de CADA canvas de la escena, uno por
            // CanvasComponent. Sustituye al collect + syncUiWidgets que antes
            // repetian los tres bucles (runtime y sandbox x2).
            void syncUiCanvases(const std::vector<UiCanvasBinding>& bindings) override;

            // Busca un nodo por nombre en TODOS los canvas, no solo en el de
            // pantalla. Lo necesitan los gizmos de widget del editor: sin esto,
            // un boton dentro de un canvas de mundo se quedaria sin gizmo.
            const UiElement* findUiNode(const std::string& name) const override;
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
            // Tamano en pixeles del render target INTERNO. Con SSAA es mayor
            // que el de salida y NO es el espacio del canvas de UI: para eso
            // estan uiWidth/uiHeight.
            uint32_t renderWidth()  const { return m_renderExtent.width; }
            uint32_t renderHeight() const { return m_renderExtent.height; }
            // Tamano en pixeles de SALIDA, el mismo que se le pasa a
            // UiCanvas::buildDrawData y en el que hay que meterle el raton a
            // UiCanvas::updateInput. Publico porque el canvas se resuelve en el
            // y quien le pasa el raton (runtime y editor) tiene que usarlo.
            uint32_t uiWidth()  const { return effectiveViewport().width; }
            uint32_t uiHeight() const { return effectiveViewport().height; }
            uint64_t uiAtlasTextureId(const UiTextureAtlas* atlas) override;
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
            // Espera a la GPU antes de reescribir el descriptor set: hacerlo
            // con un command buffer en vuelo que lo tenga bindeado es uso
            // inválido sin UPDATE_AFTER_BIND, y estos sets no lo piden (H25).
            // Los RECURSOS viejos, en cambio, van a la cola de destrucción
            // diferida y no dependen de esa espera. Solo cubre meshes estáticos
            // — no hay UI hoy que asigne meshes skinned.
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
            // Todo esto vive en ReflectionProbePass; aqui solo se delega para no
            // mover la API que ve el editor. La UI solo ENCOLA: el bake ocurre
            // al principio de drawFrame, que es donde se puede esperar a que la
            // GPU quede libre sin pillar el command buffer a medio grabar
            // (mismo sitio que rebuildAaResources).
            void requestProbeBake(uint64_t ownerId) { m_probePass.requestBake(ownerId); }
            void requestProbeBakeAll()              { m_probePass.requestBakeAll(); }
            // ms del ULTIMO bake (una sonda o la tanda entera), por timestamps.
            float lastProbeBakeMs() const { return m_probePass.lastBakeMs(); }
            int   probeCount() const      { return m_probePass.count(); }
            // Memoria GPU de las capturas persistentes de UNA sonda, en bytes.
            // No cuenta el cubemap de captura, que es uno solo pa todas.
            static constexpr uint64_t staticProbeMemoryBytes()
            {
                return ReflectionProbePass::probeMemoryBytes();
            }
            uint64_t probeMemoryBytes() const override { return staticProbeMemoryBytes(); }
            // ms del ultimo bake de UNA sonda concreta, o -1 si nunca se bakeo.
            float probeBakeMs(uint64_t ownerId) const { return m_probePass.bakeMs(ownerId); }

            // Interruptor global: apagado no se graba ni un dispatch de la cadena
            // de mips y la composicion suma bloom cero (el pass LDR NO se salta,
            // que es tambien quien tonemapea).
            void  setBloomEnabled(bool v) override;
            void  setShadowResolution(int v) override;
            void  setPresentMode(PresentMode v) override;
            bool  presentModeSupported(PresentMode v) const override;
            // Coste GPU del bloom + composicion del ultimo frame ya resuelto, en
            // ms. 0 si el dispositivo no soporta timestamps.
            float bloomGpuMs() const         { return m_bloomPass.gpuMs(); }

            // SSAO. Apagado por defecto: con el flag a false no se graba ni el
            // depth pre-pass ni los dos dispatches, y el mapa de AO se deja a 1.0
            // una sola vez, asi que la imagen es la misma que antes de la feature
            // y el coste GPU cae a cero.
            void  setSsaoEnabled(bool v);
            // Coste GPU del pre-pass + los dos dispatches, en ms. 0 si el efecto
            // esta apagado o el dispositivo no soporta timestamps.
            float ssaoGpuMs() const          { return m_ssaoPass.gpuMs(); }

            // Coste GPU del SSR en ms: los dos dispatches, mas el depth pre-pass
            // cuando es el SSR quien lo pide (con el SSAO encendido ese pre-pass
            // ya lo contabiliza ssaoGpuMs y aqui no se suma dos veces).
            float ssrGpuMs() const           { return m_ssrPass.gpuMs(); }

            // Coste GPU del dispatch de niebla en ms. 0 si esta apagada o el
            // dispositivo no soporta timestamps.
            float fogGpuMs() const              { return m_fogPass.gpuMs(); }

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
            // Sin tope duro: los vectores crecen. Lo que no crece —desde que
            // hay pool— es el numero de entradas vivas tras un ciclo Play/Stop.
            SlotUsage slotUsage() const override
            {
                SlotUsage u;
                u.objects = m_objects.size() - m_staticSlots.freeCount();
                u.skinned = m_skinnedObjects.size() - m_skinnedSlots.freeCount();
                return u;
            }
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
            // Objetos que se quedaron sin sitio en el SSBO de instancias de
            // este frame y por tanto sin sombra ni profundidad. Debe ser
            // SIEMPRE 0: si sube, la capacidad esta mal dimensionada. Antes ni
            // se contaba y el sintoma era una sombra que no estaba (H23).
            int   statInstanceOverflow() const override { return m_statInstanceOverflow; }

            // ── Forward+ ─────────────────────────────────────────────────────
            // Radio POR luz, en el mismo orden que setLights. Vacio (lo normal) =
            // todas usan el radio global de arriba.
            void setLightRadii(const std::vector<float>& radii) { m_lightRadii = radii; }
            // Coste GPU del dispatch de culling, en ms. 0 en Off.
            float forwardPlusGpuMs() const        { return m_fpPass.gpuMs(); }
            // Media de luces por celda NO VACIA del ultimo frame ya resuelto, y
            // numero de celdas que se pasaron del maximo por celda (esas si
            // pierden luces: es la senal de que hace falta bajar el radio).
            float forwardPlusAvgPerCell() const   { return m_fpPass.avgPerCell(); }
            uint32_t forwardPlusOverflowCells() const { return m_fpPass.overflowCells(); }

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
            // Igual que setAnimationState pero con el segundo clip de un
            // cross-fade. weight 0 = solo prevClip, 1 = solo clipIndex.
            // lockRootMotion clava la traslación del hueso raíz a la de su bind
            // pose. Default false = comportamiento de siempre.
            void setAnimationBlend(int index, uint32_t clipIndex, float animTime,
                                   uint32_t prevClipIndex, float prevAnimTime, float weight,
                                   bool lockRootMotion = false);
            void setSkinnedTransform(int index, const glm::mat4& transform);

            // ── Frustum culling ──────────────────────────────────────────────
            // La geometría del culling vive en Renderer/Frustum.h desde que hay
            // un segundo backend que la necesita; aquí quedan los alias y los
            // envoltorios, que es por donde entran los tests y el resto de este
            // fichero.
            using Frustum = Culling::Frustum;
            static Frustum frustumFromViewProj(const glm::mat4& viewProj);
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
            // ── Draw batching por instancing ─────────────────────────────────
            // El agrupado vive en Renderer/InstanceBatching.h desde que hay un
            // segundo backend que lo necesita, igual que el culling en
            // Renderer/Frustum.h. Estos alias y el delegado de abajo se quedan
            // para no tocar a los llamantes ni a instancing_tests.cpp.
            using InstanceBatch  = Batching::InstanceBatch;
            using BatchCandidate = Batching::BatchCandidate;
            static uint32_t buildInstanceBatches(const BatchCandidate* candidates,
                                                 size_t                count,
                                                 glm::mat4*            outTransforms,
                                                 uint32_t              outCapacity,
                                                 uint32_t              firstInstanceBase,
                                                 std::vector<InstanceBatch>& outBatches);

        private:

            // RenderObject, SkinnedMatGfx, SubMeshDraw, PushData y
            // SkinnedRenderObject viven en RenderObjects.h desde que el bake de
            // las sondas salio a su propio pase: ReflectionProbePass los recibe
            // por su Context para redibujar la escena, y meter este header
            // dentro de un pase seria circular. Estan en el mismo namespace, asi
            // que aqui se siguen nombrando sin calificar.

            void createSwapChain(Window& window);
            void createImageViews();
            void createOffscreenRenderPass();
            // Pass de la UI de juego sobre la imagen final ya anti-aliaseada.
            void createUiRenderPass();
            // Pass LDR que compone HDR + bloom, tonemapea y hospeda ademas el
            // contorno de seleccion y los gizmos: esos dos tienen que quedarse
            // FUERA del tonemap para seguir saliendo con su color plano de
            // siempre, y por eso ya no se dibujan en el pass de escena.
            void createCompositeRenderPass();
            void createRenderPass();
            void createFramebuffers();
            void createOffscreenImages();
            void destroyOffscreenImages();
            // Los dos bindings de bloom_composite.frag (escena HDR + mip 0 del
            // bloom). Van con el swapchain, igual que la cadena.
            void createCompositeSets();
            // El paquete de estado compartido que necesita BloomPass.
            BloomPass::Context bloomCtx();
            // Cadena de mips del bloom + descriptor sets de los tres pasos. Va
            // con el swapchain: la resolucion de partida es la mitad del viewport,
            // asi que redimensionar la recrea entera (destroyBloomImages primero).
            void createBloomImages();
            void destroyBloomImages();
            // El pipeline de la composicion y sus layouts, mas la resolucion del
            // soporte de timestamps. Independientes del tamano, una sola vez en
            // initSceneResources.
            void createBloomPipelines();
            // Solo el pipeline del triangulo de composicion. Vive aparte porque
            // el MSAA lo obliga a rehacerse (cambia rasterizationSamples y el
            // render pass) sin tocar los layouts ni los pools.
            void recreateCompositePipeline();
            // SSAO y depth pre-pass. Los dos pases viven en sus clases; esto
            // solo arma el paquete de estado compartido de cada uno.
            SsaoPass::Context         ssaoCtx();
            DepthPrepassPass::Context depthPrepassCtx();
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
            // El pase vive en SsrPass; esto solo arma el paquete de estado
            // compartido que necesita para cada llamada.
            SsrPass::Context ssrCtx();
            // El recorrido de m_objects/m_skinnedObjects que decide si hay algo
            // que reflejar: las listas son del Renderer, asi que el bucle se
            // queda aqui y el resultado entra en el Context.
            bool anyObjectWithSsr() const;
            // Niebla volumetrica. Mismo reparto que el SSR: el pipeline una
            // sola vez, los descriptor sets con el swapchain (referencian
            // m_hdrView y m_ssaoDepthView, que se recrean con el). El pase vive
            // en FogPass; esto solo arma el paquete de estado compartido que
            // necesita para cada llamada.
            FogPass::Context fogCtx();
            // Motion blur de camara. Mismo reparto que el SSR: el pipeline una
            // sola vez, y las imagenes y los descriptor sets con el swapchain.
            // El pase vive en MotionBlurPass; esto solo arma el paquete de
            // estado compartido que necesita para cada llamada.
            MotionBlurPass::Context motionBlurCtx();
            // Un solo write del binding 7 sobre `set`. La comparten
            // allocateObjectDescriptorSet, la ruta skinned y el refresh de arriba.
            void writeSsaoBinding(VkDescriptorSet set, int frameIndex);
            // Anti-aliasing. El pass de resolucion es grafico (triangulo de
            // pantalla completa) y no compute: el swapchain es B8G8R8A8_SRGB y
            // Vulkan prohibe las storage images en formatos sRGB.
            // Anti-aliasing. El pase de RESOLUCION vive en AaPass; aqui quedan
            // los dos pools de queries (el del AA mide ademas el frame entero)
            // y los targets multisample, que son attachments de los render
            // passes de escena y composicion.
            AaPass::Context aaCtx();
            void createAaQueryPools();
            void createMsaaImages();
            void destroyMsaaImages();
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
            ForwardPlusPass::Context fpCtx();
            void createCommandBuffers();
            void createSyncObjects();
            // Graba el frame entero. Era una funcion de 755 lineas seguidas
            // (H7): ahora orquesta, y cada pase grande vive en su metodo, como
            // ya hacian recordShadowPass y recordSsaoPass. La extraccion fue
            // corta-pega puro; lo unico que cambio es que las locales que
            // cruzaban de un bloque a otro pasaron a parametros, y ahi se ve de
            // un vistazo lo que antes habia que rastrear a mano.
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
            // Aloja MAX_FRAMES sets seguidos y devuelve el POOL del que salen,
            // que es lo que hace falta luego para liberarlos. Encadena un pool
            // nuevo cuando el ultimo se llena, en vez de lanzar: el pool se
            // dimensionaba UNA vez con las mallas del arranque mas 128 de
            // margen, y pasado ese margen la carga de escena se caia.
            //
            // Se encadena en lugar de recrear uno mayor porque recrearlo
            // invalidaria los sets de todo lo ya cargado.
            //
            // VK_NULL_HANDLE si no se pudo.
            VkDescriptorPool allocateSharedSets(VkDescriptorSet* outSets);
            bool             addDescriptorPool();
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
            // El shadow map vive en ShadowPass; esto arma su paquete de estado
            // compartido (los dos set layouts que declara shadow.vert).
            ShadowPass::Context shadowCtx();
            // Los DRAWS de las N cascadas: se quedan aqui porque salen de
            // m_objects/m_skinnedObjects y del SSBO de instancias, cuyo cursor
            // comparten este pass, el depth pre-pass y el de escena. El pase
            // pone el render pass, el pipeline y el push del indice.
            void recordShadowPass(VkCommandBuffer cmd);
            // IBL global y sondas de reflexion. Los dos pases viven en sus
            // clases; esto solo arma el paquete de estado compartido de cada
            // uno. El del bake es largo a proposito: la captura REDIBUJA la
            // escena, asi que necesita el pass offscreen entero y las listas de
            // objetos, que son del Renderer y se quedan aqui.
            IblPass::Context             iblCtx();
            ReflectionProbePass::Context probeCtx();
            // Los tres dispatches del skinning viven en SkinningPass; esto arma
            // su paquete de estado compartido (las listas, que son del Renderer).
            SkinningPass::Context skinningCtx();
            // Los cuatro pipelines GRAFICOS de las mallas con huesos. Se quedan
            // aqui porque dependen del numero de muestras de MSAA y del render
            // pass de escena.
            void createSkinnedGraphicsPipelines();
            void destroySkinnedRenderObject(SkinnedRenderObject& obj);
            // Cuerpo compartido por addSkinnedMesh y rebuildSkinnedMesh: crea
            // buffers, sube SSBOs, aloja descriptor sets y carga texturas sobre
            // un SkinnedRenderObject ya vacío.
            void initSkinnedRenderObject(SkinnedRenderObject& obj, const SkinnedMesh& mesh,
                                         TransferBatch* batch = nullptr,
                                         const std::vector<DecodedImage>* decoded = nullptr);
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

            // Pass 1: la escena 3D a su render target propio.
            void recordScenePass(VkCommandBuffer cmd, const FrameCamera& fc,
                                  const Frustum& camFrustum, bool perfStamp);
            // Bloom y composicion: del HDR de la escena al LDR de pantalla. Se
            // lleva uiExtent y los contadores de UI porque el canvas de PANTALLA
            // se dibuja aqui, sobre la imagen ya compuesta.
            void recordBloomAndComposite(VkCommandBuffer cmd, const FrameCamera& fc,
                                          const Frustum& camFrustum, VkExtent2D uiExtent,
                                          uint32_t uiScreenVertices, uint32_t uiScreenIndices);
            // Pass 2: la UI 2D sobre la imagen del swapchain.
            void recordUiPass(VkCommandBuffer cmd, uint32_t imageIndex);

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

            // ── Pass propio de la UI de juego ────────────────────────────────
            // La UI se dibuja DESPUES del anti-aliasing, sobre la imagen final y
            // a una muestra. Antes iba dentro del pass de composicion, o sea
            // ANTES del AA: con FXAA el texto se suavizaba de mas y con TAA
            // dejaba estela al moverse, mientras el backend de DirectX 12 -que
            // la dibuja sobre el back buffer ya resuelto- salia limpio. Es
            // ademas lo que hace que con SSAA la UI se dibuje una sola vez a la
            // resolucion de salida en vez de supersamplearse para nada.
            //
            // loadOp LOAD y los dos layouts en SHADER_READ_ONLY: la imagen ya
            // trae la escena resuelta y tiene que quedarse como estaba para el
            // panel del editor y para el blit del runtime.
            VkRenderPass                    m_uiRenderPass                      = VK_NULL_HANDLE;
            VkFramebuffer                   m_uiFramebuffer[MAX_FRAMES]         = {};
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

            // Cadena de mips del bloom: la tiene BloomPass entera. Su sampler y
            // el mip 0 salen por sus getters, que es lo que necesita el set de
            // composicion de aqui abajo.
            BloomPass                       m_bloomPass;

            // Pass de composicion: HDR + bloom → tonemap → m_offscreenImage.
            VkRenderPass                    m_compositeRenderPass               = VK_NULL_HANDLE;
            VkFramebuffer                   m_compositeFramebuffer[MAX_FRAMES]  = {};
            VkDescriptorSetLayout           m_compositeDescLayout               = VK_NULL_HANDLE;
            VkDescriptorPool                m_compositeDescPool                 = VK_NULL_HANDLE;
            VkDescriptorSet                 m_compositeSets[MAX_FRAMES]         = {};
            VkPipelineLayout                m_compositePipelineLayout           = VK_NULL_HANDLE;
            VkPipeline                      m_compositePipeline                 = VK_NULL_HANDLE;

            // Propiedades del device que comparten todos los pases con queries.
            // Las resuelve createBloomPipelines, que es el primero en pedir un
            // pool.
            bool                            m_timestampsSupported               = false;
            float                           m_timestampPeriod                   = 0.0f;

            // ── SSAO + depth pre-pass ─────────────────────────────────
            // La profundidad del pre-pass NO es del SSAO aunque naciera con
            // el: la comparten el SSR, el TAA, el Forward+ tiled, la niebla y
            // el motion blur. Por eso son dos clases, y el sampler NEAREST de
            // esa profundidad sale por DepthPrepassPass::sampler().
            DepthPrepassPass                m_depthPrepass;
            SsaoPass                        m_ssaoPass;

            // ── SSR ──────────────────────────────────────────────────────────
            // Imagen del reflejo, sampler, pipelines, sets y queries son suyos;
            // el Renderer solo lo posee y decide cuando crear, grabar y
            // destruir. El sampler y el pool de queries salen por sus getters:
            // los usan el motion blur y el depth pre-pass, que no son del pase.
            SsrPass                         m_ssrPass;
            // Se queda en el Renderer porque lo ESCRIBE el depth pre-pass
            // (recordSsaoPass) y no el pase: marca que dejo escritos los
            // timestamps [0,1] de este frame. SsrPass solo da la medida por
            // buena si es asi (si no, el par no se habria escrito y la lectura
            // daria NOT_READY).
            bool                            m_ssrStampedPrepass                 = false;

            // ── Niebla volumetrica ───────────────────────────────────────────
            // Pipeline, sets y queries de tiempo son suyos; el Renderer solo lo
            // posee y decide cuando crear, grabar y destruir.
            FogPass                         m_fogPass;

            // ── Motion blur ──────────────────────────────────────────────────
            // Imagenes, sets y pipeline son suyos; el Renderer solo lo posee y
            // decide cuando crear, grabar y destruir.
            MotionBlurPass                  m_motionBlurPass;

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
            // El shadow map pedido no es el que hay montado. Se atiende entre
            // frames, igual que m_aaResourcesDirty: recrearlo con la GPU en
            // marcha soltaria una imagen que el frame en vuelo esta leyendo.
            bool                            m_shadowResourcesDirty              = false;
            void rebuildShadowResources();
            // Reescribe el binding 3 (el shadow map) en TODOS los descriptor
            // sets: los de cada malla compartida y los de cada material de
            // personaje. Sin esto, tras un resize apuntan a una vista muerta.
            void refreshShadowDescriptors();
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
            // Imagen intermedia, historial del TAA, framebuffers, sets y los
            // tres pipelines de resolucion: todo eso lo tiene AaPass. De el
            // salen tambien el framebuffer alternativo de la composicion y las
            // dos matrices view-proj que consume el motion blur.
            AaPass                          m_aaPass;


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

            // Queries propias: reutilizar las del bloom mezclaria el coste del
            // tonemap con el del anti-aliasing. Dos pares por frame: [0,1] el
            // pass propio del modo, [2,3] el render completo sin UI.
            VkQueryPool                     m_aaQueryPool                       = VK_NULL_HANDLE;
            bool                            m_aaQueryPending[MAX_FRAMES]        = {};
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
            int                             m_statInstanceOverflow              = 0;

            // ── Forward+ ───────────────────────────────────
            // El pase entero -layout, pipelines, buffers, sets y queries- lo
            // tiene ForwardPlusPass. Su descriptor set es el 2 del pipeline de
            // escena y su layout entra en ese pipeline layout, asi que los dos
            // salen por sus getters.
            ForwardPlusPass                 m_fpPass;
            // Modo PEDIDO (el que devuelve forwardPlusMode()) y modo CONGELADO
            // del frame. Mismo motivo que en el AA: setForwardPlusMode se llama
            // desde la UI, que se construye a mitad de drawFrame, y el bloque de
            // parametros que lee pbr.frag se escribe una sola vez por frame. Sin
            // esta separacion, un click podria dejar el frame con la rejilla de un
            // modo y la lectura del otro.
            FpMode                          m_fpActiveMode                      = FpMode::Off;
            std::vector<float>              m_lightRadii;

            // A donde apunta una luz de PUNTO, que no tiene direccion propia: el
            // centro de la escena. Se recalcula una vez por frame y lo leen los
            // DOS consumidores de la direccion de la luz key —las cascadas y la
            // niebla—, que tienen que ver exactamente el mismo valor o el
            // in-scattering apunta a un lado y el shadow map esta construido
            // hacia otro (H65). Escena vacia: se queda en el origen, que es lo
            // que hacia antes siempre.
            glm::vec3                       m_sceneCenter                       {0.0f};

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
            // Modos de presentacion que soporta ESTE device, cacheados al crear
            // el swapchain (que es donde ya hay surface y physicalDevice). FIFO
            // no entra: la spec lo garantiza siempre.
            bool                            m_mailboxDisponible                 = false;
            bool                            m_immediateDisponible               = false;
            bool                            m_headless                          = false;
            VkDescriptorSetLayout           m_descriptorSetLayout               = VK_NULL_HANDLE;
            VkBuffer                        m_uniformBuffers[MAX_FRAMES]        = {};
            VkDeviceMemory                  m_uniformBuffersMemory[MAX_FRAMES]  = {};
            void*                           m_uniformBuffersMapped[MAX_FRAMES]  = {};
            // Cadena de pools, no uno solo: ver allocateSharedSets. Cada
            // SharedGpuMesh y cada SkinnedMatGfx se acuerda de cual es el suyo.
            std::vector<VkDescriptorPool>   m_descriptorPools;
            static constexpr uint32_t       kSharedSetsPerPool                  = 128 * MAX_FRAMES;
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

            // Shadow map de cascadas. El texture array, sus dos pipelines y el
            // reparto de cascadas viven en ShadowPass; lo que el Renderer sigue
            // necesitando de él son la vista y el sampler (binding 3 de cada
            // descriptor set), su pipeline layout (que presta al depth
            // pre-pass) y las matrices de cascada (que copia al UBO).
            ShadowPass                      m_shadowPass;

            // ── IBL y sondas ───────────────────────────────────────────────
            // Los dos cubemaps globales viven en IblPass y las sondas en
            // ReflectionProbePass, que reusa sus pipelines de convolucion. Lo
            // unico que el Renderer sigue necesitando de ellos son las dos
            // vistas y el sampler del IBL global, que escribe en los bindings 5
            // y 6 de cada descriptor set (allocateObjectDescriptorSet y la ruta
            // skinned).
            IblPass                         m_iblPass;
            ReflectionProbePass             m_probePass;

            // El bake copia el UBO del frame 0 (luces, cascadas y su shadow map)
            // y solo le sustituye view/proj: sin un frame previo ese buffer es
            // basura, asi que las peticiones esperan. Se queda aqui porque lo
            // escribe updateUniformBuffer; el pase lo lee por su Context.
            bool                            m_uboWritten[MAX_FRAMES] {};

            // Skinning por compute: los tres pipelines, su layout, su pool y sus
            // descriptor sets viven en SkinningPass. De el salen tambien los
            // sets de compute que aloja initSkinnedRenderObject.
            SkinningPass          m_skinningPass;
            // Los graficos, en cambio, se quedan: dependen del MSAA.
            VkPipeline            m_skinnedGfxPipeline        = VK_NULL_HANDLE;
            VkPipeline            m_skinnedWireframePipeline  = VK_NULL_HANDLE;
            std::vector<SkinnedRenderObject> m_skinnedObjects;

            std::vector<RenderObject> m_objects;

            // Huecos libres de los dos vectores de arriba. Borrar un objeto no
            // puede compactar —los indices viven anotados en cada GameObject—,
            // asi que el hueco se recicla en la siguiente alta en vez de
            // dejarlos crecer sin fin (H19).
            SlotPool m_staticSlots;
            SlotPool m_skinnedSlots;

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

            // UI de juego. Vive en Core: el runtime exportado dibuja estos mismos
            // canvas dentro del pass de composicion, sin nada del editor.
            //
            // Uno por CanvasComponent de la escena — syncUiCanvases() los
            // empareja por ownerId (matchUiCanvasSlots), así que reordenar los
            // canvas en la jerarquía no resetea el árbol ni la caché del que no
            // se ha movido. m_uiBatch es UNO SOLO y compartido: sus buffers de
            // vértices/índices por frame se sub-asignan (UiSpriteBatch::beginFrame
            // + record) para que cada canvas escriba en su propio hueco del
            // mismo VkBuffer en vez de pisar al anterior.
            std::vector<std::unique_ptr<UiCanvasSlot>> m_uiSlots;
            // Los de MUNDO del frame, ya ordenados de lejos a cerca por
            // sortWorldCanvasesBackToFront. Miembro y no local del bucle de
            // grabado para no reasignar el vector en cada frame; lo limpia la
            // propia funcion de orden.
            std::vector<UiCanvasSlot*>                m_uiWorldOrder;
            // Repliegue de uiCanvas() cuando la escena no tiene ningún canvas de
            // pantalla. Persistente y vacío: devolver una referencia a un temporal
            // dejaría a los gizmos leyendo memoria muerta.
            UiCanvas      m_uiCanvasFallback;
            UiSpriteBatch m_uiBatch;
            std::vector<std::unique_ptr<UiTextureAtlas>> m_uiAtlases;
            std::vector<std::unique_ptr<UiFont>>         m_uiFonts;
            // Por RUTA: la misma imagen pedida dos veces es el mismo atlas, no
            // dos. Sin esto cada consulta del editor subia otra textura y otro
            // descriptor set, y el pool de UiSpriteBatch son 32. Tambien es lo
            // que deja al editor tocar los sprites del atlas que se esta
            // dibujando en vez de los de una copia suya.
            std::unordered_map<std::string, UiTextureAtlas*> m_uiAtlasByPath;
            // Handle de ImGui por atlas, cacheado: ImGui_ImplVulkan_AddTexture
            // reserva un descriptor set POR LLAMADA, y llamarlo cada frame se
            // come el pool del editor en segundos.
            std::unordered_map<const UiTextureAtlas*, uint64_t> m_uiAtlasImGuiId;
            GameObject* m_sceneRoot = nullptr;
            Scene* m_scene = nullptr;
    };
}