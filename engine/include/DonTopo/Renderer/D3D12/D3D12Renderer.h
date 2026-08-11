#pragma once

#ifdef DT_D3D12_ENABLED

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace DonTopo {

class Window;

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
class D3D12Renderer {
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
