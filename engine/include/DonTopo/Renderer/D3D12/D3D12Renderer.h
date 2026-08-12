#pragma once

#ifdef DT_D3D12_ENABLED

#include "DonTopo/Renderer/RendererState.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace DonTopo {

class Window;
struct Mesh;
struct SkinnedMesh;
struct Light;

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
class D3D12Renderer : public RendererState {
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
    void shutdown();

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
    int addStaticMesh(const Mesh& mesh);

    void setTransform(size_t objectIndex, const glm::mat4& transform);
    void setObjectMeshVisible(size_t objectIndex, bool visible);

    // Cuánto refleja este objeto. Sale al alfa de la escena, que es de donde lo
    // lee el trazado de reflejos; a false o a cero, ese objeto no refleja nada.
    void setObjectSsr(size_t objectIndex, bool enabled, float intensity);
    void setSkinnedSsr(size_t index, bool enabled, float intensity);
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
    int addSkinnedMesh(const SkinnedMesh& mesh);

    void setSkinnedTransform(size_t index, const glm::mat4& transform);
    void setSkinnedVisible(size_t index, bool visible);

    // Fija clip y tiempo ya calculados fuera. Mismo contrato que en el
    // Renderer de Vulkan: es un sink, no avanza el tiempo.
    void setAnimationState(size_t index, uint32_t clipIndex, float animTime);

    size_t skinnedCount() const;
    void   clearSkinnedMeshes();

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
