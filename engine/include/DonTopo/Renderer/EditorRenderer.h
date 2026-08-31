#pragma once

#include "DonTopo/Renderer/RendererState.h"
#include "DonTopo/Renderer/UniformBufferObject.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace DonTopo
{
    class Camera;
    class GameObject;
    class Scene;
    class UiCanvas;
    class UiElement;
    class UiFont;
    class UiLayer;
    class UiTextureAtlas;
    class Window;
    struct Mesh;
    struct SkinnedMesh;
    struct DecodedImage;
    struct UiCanvasBinding;

    // Lo que el editor necesita de un backend de render, por encima de los
    // ajustes de calidad que ya comparten los dos (RendererState).
    //
    // Existe para que los paneles no hablen con el Renderer de Vulkan por su
    // nombre: cualquier backend que implemente esto puede llevar el editor. Lo
    // que no sabe hacer lo dice devolviendo cero o no haciendo nada — un
    // backend sin sondas de reflexión responde 0 sondas, y el panel lo enseña
    // tal cual en vez de esconder la sección.
    //
    // Lo que NO entra aquí: el ciclo de vida (init, drawFrame, shutdown) ni los
    // handles nativos. Eso lo lleva quien construye el backend, que sabe cuál
    // es; el editor solo consume la escena, el viewport y las métricas.
    class EditorRenderer : public RendererState
    {
        public:
            virtual ~EditorRenderer() = default;

            // ── Ciclo de vida ───────────────────────────────────────────────
            // Lo que necesita el RUNTIME para llevar un backend sin saber cuál
            // es. NO son puros: llevan el comportamiento de "este backend no
            // hace eso", que es lo correcto para un splash que no existe o para
            // un backend que no distingue las dos fases de arranque. El editor
            // no pasa por aquí — lo construye main, que sí sabe qué backend hay.
            //
            // Fase 1: poder presentar. Fase 2: lo que depende de las mallas
            // (auto-fit de la cámara y recursos de escena). Un backend que lo
            // haga todo de una vez implementa la primera y deja la segunda.
            virtual void initPresentation(Window& window) { (void)window; }
            virtual void initSceneResources(const std::vector<Mesh>& meshes) { (void)meshes; }
            virtual void initSkybox(const std::array<std::string, 6>& facePaths) { (void)facePaths; }

            // Sin ventana de editor: el que no monte ImGui por su cuenta no
            // tiene nada que apagar.
            virtual void setHeadless(bool headless) { (void)headless; }

            // Splash de arranque. false = no hay, y quien llama sigue sin él en
            // vez de quedarse esperando a un fundido que nunca llega.
            virtual bool beginSplash(const std::string& logoPath) { (void)logoPath; return false; }
            virtual void drawSplashFrame(float alpha) { (void)alpha; }

            virtual void drawFrame(Window& window) { (void)window; }
            virtual void notifyResize() {}
            virtual void shutdown() {}

            // ¿Queda algo por subir del camino asíncrono? El que sube síncrono
            // no tiene cola y responde que no.
            virtual bool hasPendingUploads() const { return false; }

            // Atlas y fuentes de la UI 2D. El dueño es el backend; quien las
            // pide se queda con el puntero. nullptr = no se pudo cargar, y el
            // widget se dibuja con su color plano.
            virtual UiTextureAtlas* loadUiAtlas(const std::string& path)
            {
                (void)path;
                return nullptr;
            }
            virtual UiFont* loadUiFont(const std::string& path, float bakePx = 48.0f)
            {
                (void)path;
                (void)bakePx;
                return nullptr;
            }

            // ── Escena ──────────────────────────────────────────────────────
            // Índice de render del objeto, o -1. decoded son los píxeles que un
            // worker ya descomprimió; nullptr es el camino síncrono.
            virtual int  addStaticMesh(const Mesh& mesh,
                                       const std::vector<DecodedImage>* decoded = nullptr) = 0;
            virtual void rebuildSkinnedMesh(int index, const SkinnedMesh& mesh)            = 0;
            virtual void registerGameObject(GameObject* node)                              = 0;
            virtual void removeGameObject(GameObject* node)                                = 0;
            virtual void removeMeshComponent(GameObject* node)                             = 0;

            // Textura que no se pudo cargar: se sustituye por la de "falta esto"
            // para que el objeto siga viéndose y el fallo se note.
            enum class TextureSlot { Diffuse, Normal, MetallicRoughness };
            virtual void replaceStaticTextureWithMissing(int renderIndex, TextureSlot slot) = 0;

            // Cierra los envíos pendientes y espera: lo usan las transiciones
            // que tienen que ver el resultado en ESTE frame (deshacer un
            // Create, salir de Play).
            virtual void flushUploadsAndWait() = 0;

            // Cierra el lote de subidas del frame sin esperar: lo que aterrice
            // se verá en cuanto su fence señale.
            virtual void flushPendingUploads() = 0;

            // Recalcula el rango de profundidad con lo que hay cargado. Sin
            // esto, cambiar la escena de arranque recorta el fondo.
            virtual void refitCameraRange() = 0;

            // Qué se dibuja con contorno; -1 en ambos para ninguno.
            virtual void setOutlineTarget(int staticIndex, int skinnedIndex) = 0;

            // ── Escena del frame ────────────────────────────────────────────
            // Lo que el bucle principal fija cada vuelta: con qué cámara se
            // dibuja, qué luces hay y dónde está cada objeto.
            virtual void setScene(Scene* scene)                   = 0;
            virtual void setSceneRoot(GameObject* root)           = 0;
            virtual void setCamera(const Camera& camera)          = 0;
            virtual void setLights(const std::vector<Light>& lights) = 0;
            // Radio de alcance por luz, en el mismo orden que setLights. Lo usa
            // el reparto por celdas; vacío = el radio global.
            virtual void setLightRadii(const std::vector<float>& radii) = 0;

            virtual int  addSkinnedMesh(const SkinnedMesh& mesh,
                                        const std::vector<DecodedImage>* decoded = nullptr) = 0;

            virtual void setTransform(size_t objectIndex, const glm::mat4& transform) = 0;
            virtual void setSkinnedTransform(int index, const glm::mat4& transform)   = 0;
            virtual void setObjectMeshVisible(size_t objectIndex, bool visible)       = 0;
            virtual void setSkinnedMeshVisible(int index, bool visible)               = 0;
            // Cuánto refleja el objeto; 0 = nada.
            virtual void setObjectSsr(size_t objectIndex, float strength) = 0;
            virtual void setSkinnedSsr(int index, float strength)         = 0;

            // Avanza el tiempo de animación de un personaje, o fija el que ya
            // calculó un Animator en CPU.
            virtual void updateAnimation(int index, float deltaTime)                    = 0;
            virtual void setAnimationState(int index, uint32_t clipIndex, float animTime) = 0;
            // Cross-fade: los DOS clips en vuelo y el peso de la mezcla.
            // weight 0 = solo prevClip, 1 = solo clipIndex. Es un superconjunto
            // de setAnimationState (que equivale a weight 1), pero se queda
            // aparte para no tocar la firma que ya usan los objetos sin mezcla.
            // lockRootMotion clava la traslación del hueso raíz a la de su bind
            // pose (clip que desplaza el modelo reproducido en el sitio). Va al
            // final y con default para que los callers que no lo usen sigan
            // compilando tal cual.
            virtual void setAnimationBlend(int index, uint32_t clipIndex, float animTime,
                                           uint32_t prevClipIndex, float prevAnimTime,
                                           float weight, bool lockRootMotion = false) = 0;

            // Suelta lo que quedó pendiente de borrar cuando la GPU lo permita.
            virtual void tickDeferredDeletes() = 0;

            // ── Viewport ────────────────────────────────────────────────────
            virtual void     setViewportSize(uint32_t width, uint32_t height) = 0;
            virtual uint32_t renderWidth() const                              = 0;
            virtual uint32_t renderHeight() const                             = 0;
            // Tamano de SALIDA. Con SSAA no es el del render interno, y es el
            // espacio en el que se resuelve el canvas de UI (buildDrawData) y en
            // el que hay que meterle el raton a UiCanvas::updateInput.
            virtual uint32_t uiWidth() const                                  = 0;
            virtual uint32_t uiHeight() const                                 = 0;

            // Identificador de un atlas ya cargado PARA LA INTERFAZ (lo que
            // ImGui::Image entiende por textura), o 0 si ese atlas no está
            // subido. Lo pide el editor de sprites, que necesita enseñar la
            // imagen sobre la que se dibujan los sub-rects. El valor lo fabrica
            // cada backend a su manera y se cachea: registrar la misma textura
            // en cada frame agota el pool de descriptores en segundos.
            virtual uint64_t uiAtlasTextureId(const UiTextureAtlas* atlas)    = 0;
            virtual float    viewportAspect() const                           = 0;

            // La capa de interfaz que dibuja encima. El backend la llama dentro
            // del frame; el editor la fija una vez.
            virtual void setUiLayer(UiLayer* ui) = 0;

            // Árbol de la interfaz 2D del juego, el de PANTALLA. Un backend que
            // todavía no la dibuje devuelve el suyo vacío: el editor lo edita
            // igual.
            virtual UiCanvas& uiCanvas() = 0;

            // TODOS los canvas de PANTALLA, en orden de PRIORIDAD DE INPUT: el
            // de más arriba primero (el último que se dibuja). Es lo que hace
            // falta para repartir el ratón y el teclado entre varios canvas con
            // dispatchUiInput, y para que el clic del editor seleccione lo que
            // el usuario ve ENCIMA.
            //
            // uiCanvas() no vale para eso: devuelve UNO solo (el primero de
            // pantalla), y con él los botones de un segundo canvas se dibujan
            // pero no tienen ni hover, ni colores de estado, ni Click.
            virtual void screenUiCanvases(std::vector<UiCanvas*>& out) = 0;

            // El canvas de UN GameObject, por su id, o nullptr si no tiene. Lo
            // usa el gizmo del canvas SELECCIONADO: `uiCanvas()` le daba el
            // PRIMERO de pantalla, así que seleccionar un segundo canvas pintaba
            // el rect del primero.
            //
            // Va aparte de screenUiCanvases y no dentro porque aquella alimenta a
            // dispatchUiInput, cuya firma es del core de UI
            // (`vector<UiCanvas*>`): meterle el ownerId obligaría a que el core
            // conociera los tipos del Renderer, o a desempaquetar una segunda
            // lista por frame en los tres bucles.
            virtual const UiCanvas* uiCanvasOf(uint64_t ownerId) const = 0;

            // Monta el árbol vivo de CADA canvas de la escena. Sustituye al
            // collect + syncUiWidgets que antes repetían los tres bucles
            // (runtime y sandbox x2) — tres copias de lo mismo es como se
            // desincronizan.
            virtual void syncUiCanvases(const std::vector<UiCanvasBinding>& bindings) = 0;

            // Busca un nodo por nombre en TODOS los canvas, no solo en el de
            // pantalla. Lo usan los nueve gizmos de widget del editor: sin esto,
            // un botón dentro de un canvas de mundo se quedaría sin gizmo y nada
            // lo diría.
            virtual const UiElement* findUiNode(const std::string& name) const = 0;

            // ── Anti-aliasing con recursos detrás ───────────────────────────
            // El modo y las muestras viven en RendererState, pero cambiarlos
            // mueve imágenes y pipelines, y eso lo sabe cada backend.
            virtual void  setAaMode(AaMode mode)   = 0;
            virtual void  setMsaaSamples(int v)    = 0;
            virtual int   maxMsaaSamples() const   = 0;
            virtual void  setSsaaFactor(float v)   = 0;
            // El getter lo da RendererState; aqui solo el interruptor, que es
            // quien mueve los targets.
            virtual void  setSsaoEnabled(bool v)   = 0;

            // El bloom se apaga soltando su cadena de imágenes, así que el
            // interruptor tampoco puede ser un simple bool del estado.
            // El getter ya lo da RendererState: aquí solo hace falta el
            // interruptor, que es el que mueve recursos.
            virtual void  setBloomEnabled(bool v)  = 0;

            // El lado del shadow map mueve la imagen, sus vistas y los
            // framebuffers, así que tampoco puede ser un simple int del estado.
            // El getter ya lo da RendererState. Cada backend decide CUÁNDO
            // rehacerlo: con la GPU en reposo y reescribiendo después los
            // descriptores que apuntaban al mapa viejo.
            virtual void  setShadowResolution(int v) = 0;

            // Modo de presentación. Recrea el swapchain, así que tampoco puede
            // ser un simple valor del estado. El getter ya lo da RendererState.
            //
            // El backend CAE A VSYNC si el modo pedido no está soportado: es el
            // único que Vulkan garantiza siempre y el que DXGI da sin extensión.
            // presentMode() sigue devolviendo lo PEDIDO aunque se haya caído,
            // para que el project.json conserve la intención del usuario si
            // luego abre el proyecto en una máquina que sí lo soporta.
            virtual void  setPresentMode(PresentMode v) = 0;

            // Qué modos puede dar ESTE device. Lo consulta la UI para
            // deshabilitar los que no, con el motivo en un tooltip, en vez de
            // esconderlos: si el core soporta N opciones, la UI ofrece N y el
            // matiz se documenta.
            //
            // Vsync siempre devuelve true.
            virtual bool  presentModeSupported(PresentMode v) const = 0;

            // ── Sondas de reflexión ─────────────────────────────────────────
            virtual void  requestProbeBake(uint64_t ownerId) = 0;
            virtual void  requestProbeBakeAll()              = 0;
            virtual int   probeCount() const                 = 0;
            virtual float lastProbeBakeMs() const            = 0;
            virtual float probeBakeMs(uint64_t ownerId) const = 0;

            // ── Ranuras de objeto ───────────────────────────────────────────
            // Cuántas entradas de render hay vivas y cuántas caben. Borrar un
            // objeto no compacta el vector —los índices están anotados en cada
            // GameObject— pero sí devuelve su hueco, así que un ciclo
            // Play/Stop repetido tiene que dejar `used` donde estaba. Que se
            // vea es lo que distingue el reciclaje de una fuga (H19, H43).
            //
            // capacity == 0 significa "sin tope duro": el backend crece el
            // vector. Vulkan es así; D3D12 tiene 512 y 16, que son los tamaños
            // con los que se repartió el heap de descriptores.
            struct SlotUsage {
                size_t objects         = 0;
                size_t objectCapacity  = 0;
                size_t skinned         = 0;
                size_t skinnedCapacity = 0;
            };
            // No es virtual pura: un backend que no lleve la cuenta devuelve
            // ceros y el panel lo trata como "sin dato", en vez de obligar a
            // los dos a implementarla el día que se añada un tercero.
            virtual SlotUsage slotUsage() const { return {}; }

            // ── Métricas ────────────────────────────────────────────────────
            // En milisegundos de GPU del último frame medido. Cero si el
            // backend no las toma.
            virtual void  setPerfCaptureEnabled(bool on)      = 0;
            virtual float renderGpuMs() const                 = 0;
            virtual float ssaoGpuMs() const                   = 0;
            virtual float ssrGpuMs() const                    = 0;
            virtual float bloomGpuMs() const                  = 0;
            virtual float fogGpuMs() const                    = 0;
            virtual float aaGpuMs() const                     = 0;
            virtual float sceneGpuMs() const                  = 0;
            virtual float shadowGpuMs() const                 = 0;
            virtual float forwardPlusGpuMs() const            = 0;

            // Cuentas del último frame: draws enviados, instancias dibujadas y
            // objetos descartados por el frustum.
            virtual int statDrawCalls() const = 0;
            virtual int statInstances() const = 0;
            virtual int statCulled() const    = 0;
            virtual float forwardPlusAvgPerCell() const       = 0;
            virtual uint32_t forwardPlusOverflowCells() const = 0;
    };
}
