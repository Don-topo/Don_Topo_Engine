#pragma once

#ifdef DT_D3D12_ENABLED

#include "DonTopo/Renderer/EditorRenderer.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace DonTopo {

class Window;
struct Mesh;
struct SkinnedMesh;
struct Light;
class UiTextureAtlas;
class UiFont;

namespace D3D12 {

// Backend de presentación DirectX 12.
//
// ALCANCE DE HOY: device, cola de comandos, swapchain, sincronización por
// fence, resize y limpieza. Presenta un color de fondo y nada más — no hay
// mallas, materiales, cámaras, sombras, post ni UI. Eso llega en las fases
// siguientes, y hasta entonces el editor bajo este backend es una ventana que
// presenta. Está dicho en el Log al arrancar: no es un stub que finja trabajar.
//
// El estado de DX12 vive en un Impl oculto para que este header no arrastre
// d3d12.h a todo el que incluya el motor; por eso las libs van PRIVATE en el
// CMakeLists de DonTopoCore.
// Hereda de RendererState igual que el Renderer de Vulkan: los parámetros de
// bloom, niebla, SSAO, SSR y anti-aliasing son los mismos valores en los dos
// backends, y tenerlos una sola vez es lo que permite que el mismo panel de
// opciones sirva para ambos.
class D3D12Renderer : public EditorRenderer {
public:
    D3D12Renderer();
    ~D3D12Renderer();

    D3D12Renderer(const D3D12Renderer&)            = delete;
    D3D12Renderer& operator=(const D3D12Renderer&) = delete;

    // Crea device, cola y swapchain sobre la ventana ya existente. Lanza
    // std::runtime_error con el HRESULT y el paso que falló: quien llama decide
    // si aborta o cae a otro backend, pero nunca se queda a medio construir.
    void init(Window& window);

    // Espera a que la GPU termine y libera todo. Idempotente: el destructor la
    // llama, así que llamarla a mano antes no hace daño.
    void shutdown() override;

    // ── Ciclo de vida por la interfaz ────────────────────────────────────────
    // Lo que el runtime llama sin saber qué backend hay. Este monta todo en
    // init(), así que la fase 1 es esa misma llamada y la 2 se queda en subir
    // las mallas; el cielo ya lo carga init por su cuenta.
    void initPresentation(Window& window) override { init(window); }
    void initSceneResources(const std::vector<Mesh>& meshes) override;
    void drawFrame(Window& window) override;
    void notifyResize() override;
    void setHeadless(bool headless) override;

    // Bloquea hasta que la GPU vacía todo lo enviado. Hace falta antes de
    // liberar recursos que no son de este backend —los de ImGui, por ejemplo—:
    // el último frame presentado sigue en vuelo, y soltar debajo sus buffers de
    // vértices o su textura corrompe el trabajo en curso.
    void waitIdle();

    // Un frame completo: espera el fence de este slot, graba el clear, ejecuta
    // y presenta.
    void drawFrame();

    // ANOTA el nuevo tamaño; no toca la swapchain. El trabajo real lo hace
    // drawFrame() al empezar el frame siguiente.
    //
    // Esta separación NO es un capricho: quien llama a esto es el callback de
    // GLFW, que Windows despacha desde dentro del WindowProc. Tocar DXGI ahí
    // ya sería arriesgado, pero lo que lo hace inaceptable es que cualquier
    // excepción tendría que desenrollar a través del despachador de callbacks
    // del kernel (KiUserCallbackDispatcher), cosa que en x64 no se puede: el
    // proceso se cuelga sin dejar ni un mensaje de error.
    //
    // width/height a 0 (ventana minimizada) se ignoran: DXGI rechaza un tamaño
    // nulo y no hay nada que presentar.
    void resize(uint32_t width, uint32_t height);

    void setClearColor(float r, float g, float b, float a);

    // La escena y su raíz. Este backend no las recorre por su cuenta —la
    // geometría entra por registerGameObject—, pero las guarda para que quien
    // se las dio pueda preguntárselas.
    void setScene(Scene* scene) override;
    void setSceneRoot(GameObject* root) override;

    // Cámara del frame a partir de la del motor: view, posición y campo de
    // visión salen de ella.
    void setCamera(const Camera& camera) override;

    void setLights(const std::vector<Light>& lights) override;
    void setLightRadii(const std::vector<float>& radii) override;

    // Aquí no hay borrados diferidos: las liberaciones esperan a la GPU en el
    // momento. Se implementa para cumplir la interfaz.
    void tickDeferredDeletes() override;

    // Luces de la escena, en el mismo formato que el UBO de los shaders (hasta
    // 16; el resto se descarta). Sin llamar a esto —o con count 0— el backend
    // ilumina con una direccional propia, que es lo que da luz a la escena de
    // arranque cuando no hay proyecto.
    //
    // La POSICIÓN de la primera manda además en el reparto de las cascadas de
    // sombra, igual que en el Renderer de Vulkan: la sombra la proyecta siempre
    // la luz 0, sea del tipo que sea.
    void setLights(const Light* lights, size_t count);

    // Encuadre con el que se dibuja todo: escena, rejilla, niebla y el reparto
    // de las cascadas de sombra. `view` y `position` tienen que ser de la misma
    // cámara — la niebla desproyecta con una y sitúa el ojo con la otra.
    // fovDegrees <= 0 conserva el que hubiera.
    //
    // Recalcula las cascadas, así que se llama cuando la cámara se mueve, no
    // por frame incondicionalmente.
    void setCamera(const glm::mat4& view, const glm::vec3& position, float fovDegrees = 0.0f);

    // --- Escena ----------------------------------------------------------
    //
    // Mismos nombres y misma semántica que el Renderer de Vulkan, para que
    // quien construye la escena no tenga que saber con qué backend corre.

    // Sube la malla a VRAM y devuelve su índice, que es el que hay que usar
    // luego en setTransform/setObjectMeshVisible. -1 si la malla está vacía.
    //
    // decoded se ignora: aquí las subidas son síncronas y la descompresión ya
    // viene hecha dentro del propio Mesh.
    int addStaticMesh(const Mesh& mesh,
                      const std::vector<DecodedImage>* decoded = nullptr) override;

    void setTransform(size_t objectIndex, const glm::mat4& transform) override;
    void setObjectMeshVisible(size_t objectIndex, bool visible) override;

    // Cuánto refleja este objeto. Sale al alfa de la escena, que es de donde lo
    // lee el trazado de reflejos; a cero, ese objeto no refleja nada.
    void setObjectSsr(size_t objectIndex, float strength) override;
    void setSkinnedSsr(int index, float strength) override;
    size_t objectCount() const;

    // Suelta toda la geometría estática. Espera a la GPU antes de liberar:
    // los buffers pueden estar en uso por el último frame presentado.
    void clearStaticMeshes();

    // Sube un personaje animado: claves, esqueleto, vértices sin deformar y el
    // buffer donde el compute escribe los ya deformados. -1 si la malla no
    // trae esqueleto, vértices o clips.
    //
    // La animación avanza sola con el reloj del backend, reproduciendo en
    // bucle el clip activo (el 0 al cargar). Quien tenga un Animator que la
    // calcule en CPU usa setAnimationState y no depende de ese reloj.
    int addSkinnedMesh(const SkinnedMesh& mesh,
                       const std::vector<DecodedImage>* decoded = nullptr) override;
    void rebuildSkinnedMesh(int index, const SkinnedMesh& mesh) override;

    void setSkinnedTransform(int index, const glm::mat4& transform) override;
    void setSkinnedMeshVisible(int index, bool visible) override;

    // Fija clip y tiempo ya calculados fuera. Mismo contrato que en el
    // Renderer de Vulkan: es un sink, no avanza el tiempo.
    void setAnimationState(int index, uint32_t clipIndex, float animTime) override;
    void setAnimationBlend(int index, uint32_t clipIndex, float animTime,
                           uint32_t prevClipIndex, float prevAnimTime, float weight,
                           bool lockRootMotion = false) override;
    void updateAnimation(int index, float deltaTime) override;

    // Proyección por vista del frame, la misma con la que se dibuja: es lo que
    // necesita quien desproyecte un clic del viewport para saber a qué apunta.
    glm::mat4 viewProjMatrix() const;

    // Líneas de depuración de ESTE frame: colliders, ejes, rayos. El formato es
    // el de DonTopo::GizmoVertex —posición y color, tres float cada uno, sin
    // hueco entre ellos—, y se pasa como float suelto para no arrastrar aquí
    // Gizmos.h, que incluye vulkan.h.
    //
    // Cada llamada REEMPLAZA lo enviado antes, y lo enviado no persiste al
    // frame siguiente: quien las dibuja las vuelve a mandar cada vez, igual que
    // con el Gizmos del camino de Vulkan.
    void submitDebugLines(const float* vertices, size_t vertexCount);

    // Qué se dibuja con contorno de selección: índices de addStaticMesh y de
    // addSkinnedMesh, o -1 para ninguno. Se puede tener uno de cada.
    void setSelection(int staticIndex, int skinnedIndex);
    // Grosor de la extrusión del casco, en unidades de mundo.
    void setOutlineWidth(float width);

    size_t skinnedCount() const;
    void   clearSkinnedMeshes();

    // --- Lo que pide el editor ------------------------------------------
    //
    // Sube o suelta la geometría de un nodo y de sus hijos. Es la misma
    // operación que hace el camino del sandbox a mano, con los índices de
    // render anotados en el propio GameObject.
    void registerGameObject(GameObject* node) override;
    void removeGameObject(GameObject* node) override;
    void removeMeshComponent(GameObject* node) override;

    void replaceStaticTextureWithMissing(int renderIndex, TextureSlot slot) override;

    // Las subidas de este backend son síncronas: basta con esperar a la GPU.
    void flushUploadsAndWait() override;

    // El rango de profundidad aquí es fijo (0.1 a 500), así que no hay nada que
    // reajustar. Se implementa para cumplir la interfaz y para que quien la
    // llame no tenga que preguntar con qué backend corre.
    void refitCameraRange() override;

    void setOutlineTarget(int staticIndex, int skinnedIndex) override;

    uint32_t renderWidth() const override;
    uint32_t renderHeight() const override;
    uint32_t uiWidth() const override;
    uint32_t uiHeight() const override;
    uint64_t uiAtlasTextureId(const UiTextureAtlas* atlas) override;
    float    viewportAspect() const override;

    void      setUiLayer(UiLayer* ui) override;
    UiCanvas& uiCanvas() override;

    // TODOS los de pantalla, en orden de prioridad de input (el de más arriba
    // primero). Mismo criterio y misma función libre que en Vulkan.
    void screenUiCanvases(std::vector<UiCanvas*>& out) override;

    // El canvas de un GameObject por su id (nullptr si no tiene). Misma
    // funcion libre que en Vulkan.
    const UiCanvas* uiCanvasOf(uint64_t ownerId) const override;

    // Monta el árbol vivo de CADA canvas de la escena. Mismo contrato que en
    // el Renderer de Vulkan: uiCanvas() sigue devolviendo solo el de pantalla.
    void syncUiCanvases(const std::vector<UiCanvasBinding>& bindings) override;
    // Busca un nodo por nombre en TODOS los canvas, no solo en el de pantalla.
    const UiElement* findUiNode(const std::string& name) const override;

    // Atlas y fuentes de la UI 2D. Mismas firmas que en el camino de Vulkan: el
    // sync de widgets las llama por plantilla, sin saber qué backend hay debajo.
    // El dueño es el backend; quien las pide se queda solo con el puntero.
    UiTextureAtlas* loadUiAtlas(const std::string& path) override;
    UiFont*         loadUiFont(const std::string& path, float bakePx = 48.0f) override;

    // Cambiar de modo o de muestras mueve targets y pipelines, y eso lo aplica
    // el frame siguiente con la GPU en reposo.
    void  setAaMode(AaMode mode) override;
    void  setMsaaSamples(int v) override;
    int   maxMsaaSamples() const override;
    void  setSsaoEnabled(bool v) override;

    // El bloom aquí no suelta nada al apagarse: la cadena de imágenes vive con
    // los demás targets. El interruptor es el del estado compartido.
    void  setBloomEnabled(bool v) override;
    // El cielo del proyecto. Antes NO se sobrescribia y este backend usaba
    // seis rutas a fuego, con lo que ignoraba el skybox de la escena y con el
    // el IBL global que sale de convolucionarlo.
    void  initSkybox(const std::array<std::string, 6>& facePaths) override;
    void  setShadowResolution(int v) override;

    // Sin lote de subidas: aquí cada una se envía y se espera al hacerla.
    void  flushPendingUploads() override;

    // SSAA no está en este backend: se acepta el valor y se guarda, pero no
    // cambia la resolución de render. El panel lo enseña igual.
    void  setSsaaFactor(float v) override;

    // Sondas de reflexión: todavía no. Cero sondas y cero milisegundos, que es
    // lo que el panel enseña.
    void  requestProbeBake(uint64_t ownerId) override;
    void  requestProbeBakeAll() override;
    int   probeCount() const override;
    float lastProbeBakeMs() const override;
    float probeBakeMs(uint64_t ownerId) const override;

    // Métricas por pase: sin consultas de tiempo en este backend todavía.
    void     setPerfCaptureEnabled(bool on) override;
    float    renderGpuMs() const override;
    float    ssaoGpuMs() const override;
    float    ssrGpuMs() const override;
    float    bloomGpuMs() const override;
    float    fogGpuMs() const override;
    float    aaGpuMs() const override;
    float    sceneGpuMs() const override;
    float    shadowGpuMs() const override;
    float    forwardPlusGpuMs() const override;

    // Cuentas del frame: este backend todavía no las lleva.
    int      statDrawCalls() const override;
    int      statInstances() const override;
    int      statCulled() const override;
    float    forwardPlusAvgPerCell() const override;
    uint32_t forwardPlusOverflowCells() const override;

    // Descripción del adaptador con el que se creó el device. Vacía antes de
    // init(). Se usa para dejarlo en el Log.
    const std::string& adapterName() const;

    // --- Enganche de interfaz de usuario --------------------------------
    //
    // DonTopoCore no puede depender de ImGui (el runtime exportado lo enlaza y
    // ahí no existe), así que el backend no dibuja la UI: expone lo justo para
    // que quien sí conoce ImGui lo haga desde fuera. Los punteros se devuelven
    // como void* a propósito, para no arrastrar d3d12.h a este header.

    // Se invoca dentro del frame, con la lista de comandos abierta y el
    // backbuffer ya como destino, justo después del último post-efecto.
    void setUiDrawCallback(std::function<void()> callback);

    // Manda la escena ya compuesta a una textura en vez de al backbuffer, que
    // pasa a llevar SOLO la interfaz. Es lo que necesita un viewport dentro de
    // un panel: quien dibuja la UI recibe la escena como imagen.
    void setRenderToTexture(bool enabled);

    // Tamaño al que se dibuja la escena cuando va a textura: el del panel que
    // la muestra, para que salga 1:1 y no reescalada. Se anota y se aplica
    // entre frames, como el resize de la ventana; 0 se ignora.
    void setViewportSize(uint32_t width, uint32_t height) override;

    // Descriptor GPU de esa textura, listo para usarse como ImTextureID. 0 si
    // todavía no hay imagen (antes de init). El heap es el mismo que se expone
    // en uiDescriptorHeap(), así que el backend de ImGui ya lo tiene enlazado.
    uint64_t viewportTexture() const;

    void* nativeDevice() const;       // ID3D12Device*
    void* nativeCommandList() const;  // ID3D12GraphicsCommandList*, solo válido dentro del callback
    void* nativeQueue() const;        // ID3D12CommandQueue*
    void* uiDescriptorHeap() const;   // ID3D12DescriptorHeap* con el rango reservado a la UI

    // Primer descriptor del rango reservado, y cuántos hay. El backend de ImGui
    // reserva por su cuenta según le hagan falta, así que necesita el rango
    // entero y no solo el hueco de la fuente.
    uint64_t uiHeapStartCpu() const;
    uint64_t uiHeapStartGpu() const;
    unsigned uiDescriptorCount() const;
    unsigned descriptorSize() const;
    int      framesInFlight() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace D3D12
}  // namespace DonTopo

#endif  // DT_D3D12_ENABLED
