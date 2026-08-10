#include "DonTopo/Renderer/D3D12/D3D12Renderer.h"

#ifdef DT_D3D12_ENABLED

#include "DonTopo/Core/Window.h"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <D3D12MemAlloc.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#ifndef NDEBUG
#include <dxgidebug.h>
#endif

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace DonTopo::D3D12 {

namespace {

using Microsoft::WRL::ComPtr;

// Un vértice de la geometría de gizmos: es EXACTAMENTE lo que declara
// shaders/gizmo.vert (location 0 = posición, location 1 = color), y por eso el
// input layout puede ir contra el DXIL traducido sin adaptar nada.
struct GizmoVertex {
    float pos[3];
    float color[3];
};

std::vector<char> readBinaryFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open())
        throw std::runtime_error("D3D12: no se pudo abrir '" + path + "'");

    const std::streamsize size = in.tellg();
    if (size <= 0)
        throw std::runtime_error("D3D12: '" + path + "' está vacío");

    std::vector<char> data(static_cast<size_t>(size));
    in.seekg(0);
    in.read(data.data(), size);
    if (!in)
        throw std::runtime_error("D3D12: lectura incompleta de '" + path + "'");
    return data;
}

// Triple buffer: dos frames en vuelo mientras la GPU trabaja en el tercero. Es
// el mismo criterio que usa la swapchain de Vulkan del motor.
constexpr UINT kFrameCount = 3;

std::string hresultToString(HRESULT hr)
{
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

// Todo fallo de creación aborta el init con el paso concreto que falló: un
// device a medias no se puede usar y esconder el HRESULT solo mueve el crash
// más adelante.
void throwIfFailed(HRESULT hr, const char* step)
{
    if (FAILED(hr))
        throw std::runtime_error(std::string("D3D12: ") + step + " falló (HRESULT " +
                                 hresultToString(hr) + ")");
}

std::string narrow(const wchar_t* wide)
{
    if (wide == nullptr || wide[0] == L'\0')
        return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)
        return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

}  // namespace

struct D3D12Renderer::Impl {
    ComPtr<IDXGIFactory4>       factory;
    ComPtr<IDXGIAdapter1>       adapter;
    ComPtr<ID3D12Device>        device;
    ComPtr<ID3D12CommandQueue>  queue;
    ComPtr<IDXGISwapChain3>     swapChain;

    // D3D12MemoryAllocator lleva su propio contador de referencias con
    // Release(), no es un objeto COM al uso: no vale ComPtr.
    D3D12MA::Allocator* allocator = nullptr;

    // Geometría de gizmos: la ruta más simple que ya usa la escena. Su vertex
    // buffer vive en un heap DEFAULT suballocado por D3D12MA.
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> gizmoPipeline;
    D3D12MA::Allocation*        gridAllocation = nullptr;
    D3D12_VERTEX_BUFFER_VIEW    gridVertexBufferView{};
    UINT                        gridVertexCount = 0;

    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    UINT                         rtvSize = 0;

    std::array<ComPtr<ID3D12Resource>, kFrameCount>         renderTargets;
    std::array<ComPtr<ID3D12CommandAllocator>, kFrameCount> allocators;
    ComPtr<ID3D12GraphicsCommandList>                       commandList;

    // Un valor de fence por slot: el frame N solo puede reusar su allocator
    // cuando la GPU ha pasado del valor que se le asignó la última vez.
    ComPtr<ID3D12Fence>                fence;
    std::array<UINT64, kFrameCount>    fenceValues{};
    HANDLE                             fenceEvent = nullptr;

    UINT frameIndex = 0;
    UINT width      = 0;
    UINT height     = 0;

    // Tamaño anotado por el callback de la ventana, pendiente de aplicar. Ver
    // el comentario de resize() en la cabecera: el trabajo de DXGI no puede
    // hacerse dentro del WindowProc.
    UINT pendingWidth   = 0;
    UINT pendingHeight  = 0;
    bool resizePending  = false;

    float       clearColor[4] = {0.10f, 0.10f, 0.12f, 1.0f};
    std::string adapterName;
    HWND        hwnd        = nullptr;
    bool        initialized = false;

    void waitForGpu();
    void moveToNextFrame();
    void createRenderTargetViews();
    void releaseRenderTargets();
    void applyPendingResize();

    // Sube `size` bytes a un buffer en heap DEFAULT y lo deja en `finalState`.
    // Síncrono: graba la copia, la ejecuta y espera. Solo se usa en init, donde
    // bloquear no cuesta nada; la subida en streaming es de otra fase.
    D3D12MA::Allocation* uploadBuffer(const void* data, size_t size,
                                      D3D12_RESOURCE_STATES finalState);

    void createGizmoPipeline();
    void createGridGeometry();

    // Matriz de cámara del frame. Se recalcula en cada resize porque el aspecto
    // depende del tamaño de la ventana.
    glm::mat4 viewProj{1.0f};
    void      updateViewProj();
};

// Espera a que la GPU vacíe TODO lo enviado. Solo para resize y shutdown: por
// frame se usa moveToNextFrame, que no serializa CPU y GPU.
void D3D12Renderer::Impl::waitForGpu()
{
    if (!queue || !fence || fenceEvent == nullptr)
        return;

    const UINT64 target = fenceValues[frameIndex];
    if (FAILED(queue->Signal(fence.Get(), target)))
        return;

    if (fence->GetCompletedValue() < target) {
        if (SUCCEEDED(fence->SetEventOnCompletion(target, fenceEvent)))
            WaitForSingleObjectEx(fenceEvent, INFINITE, FALSE);
    }
    ++fenceValues[frameIndex];
}

void D3D12Renderer::Impl::moveToNextFrame()
{
    const UINT64 current = fenceValues[frameIndex];
    if (FAILED(queue->Signal(fence.Get(), current)))
        return;

    frameIndex = swapChain->GetCurrentBackBufferIndex();

    // Solo se espera si este slot todavía está en la GPU. Con triple buffer, lo
    // normal es que ya haya terminado y no se bloquee nada.
    if (fence->GetCompletedValue() < fenceValues[frameIndex]) {
        if (SUCCEEDED(fence->SetEventOnCompletion(fenceValues[frameIndex], fenceEvent)))
            WaitForSingleObjectEx(fenceEvent, INFINITE, FALSE);
    }
    fenceValues[frameIndex] = current + 1;
}

void D3D12Renderer::Impl::createRenderTargetViews()
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i) {
        throwIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(&renderTargets[i])),
                      "IDXGISwapChain3::GetBuffer");
        device->CreateRenderTargetView(renderTargets[i].Get(), nullptr, handle);
        handle.ptr += rtvSize;
    }
}

void D3D12Renderer::Impl::releaseRenderTargets()
{
    // ResizeBuffers exige que no quede NINGUNA referencia viva a los buffers
    // antiguos; si queda, devuelve E_INVALIDARG y la swapchain se queda rota.
    for (auto& rt : renderTargets)
        rt.Reset();
}

D3D12MA::Allocation* D3D12Renderer::Impl::uploadBuffer(const void* data, size_t size,
                                                       D3D12_RESOURCE_STATES finalState)
{
    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width            = size;
    bufferDesc.Height           = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels        = 1;
    bufferDesc.Format           = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    // Destino en VRAM. Nace en COPY_DEST porque lo primero que recibe es la
    // copia desde el staging.
    D3D12MA::ALLOCATION_DESC defaultDesc{};
    defaultDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12MA::Allocation* destination = nullptr;
    throwIfFailed(allocator->CreateResource(&defaultDesc, &bufferDesc,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            &destination, IID_NULL, nullptr),
                  "D3D12MA::Allocator::CreateResource(DEFAULT)");

    // Staging en memoria visible por CPU. Se libera al salir de la función: la
    // copia ya habrá terminado porque se espera antes de volver.
    D3D12MA::ALLOCATION_DESC uploadDesc{};
    uploadDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

    D3D12MA::Allocation* staging = nullptr;
    HRESULT hr = allocator->CreateResource(&uploadDesc, &bufferDesc,
                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                           &staging, IID_NULL, nullptr);
    if (FAILED(hr)) {
        destination->Release();
        throwIfFailed(hr, "D3D12MA::Allocator::CreateResource(UPLOAD)");
    }

    void*             mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};  // no se lee nada de vuelta
    hr = staging->GetResource()->Map(0, &noRead, &mapped);
    if (FAILED(hr)) {
        staging->Release();
        destination->Release();
        throwIfFailed(hr, "ID3D12Resource::Map(staging)");
    }
    std::memcpy(mapped, data, size);
    staging->GetResource()->Unmap(0, nullptr);

    // Copia en su propio envío. El command list se reutiliza: hay que dejarlo
    // cerrado, que es como lo espera drawFrame.
    throwIfFailed(allocators[frameIndex]->Reset(), "ID3D12CommandAllocator::Reset(upload)");
    throwIfFailed(commandList->Reset(allocators[frameIndex].Get(), nullptr),
                  "ID3D12GraphicsCommandList::Reset(upload)");

    commandList->CopyBufferRegion(destination->GetResource(), 0, staging->GetResource(), 0, size);

    D3D12_RESOURCE_BARRIER toFinal{};
    toFinal.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toFinal.Transition.pResource   = destination->GetResource();
    toFinal.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toFinal.Transition.StateAfter  = finalState;
    toFinal.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &toFinal);

    throwIfFailed(commandList->Close(), "ID3D12GraphicsCommandList::Close(upload)");

    ID3D12CommandList* lists[] = {commandList.Get()};
    queue->ExecuteCommandLists(1, lists);

    // Sin esperar aquí, el staging se destruiría con la copia todavía en vuelo.
    waitForGpu();

    staging->Release();
    return destination;
}

void D3D12Renderer::Impl::createGizmoPipeline()
{
    // Root signature: los 16 floats de la matriz como root constants. Es el
    // equivalente directo del push_constant de shaders/gizmo.vert, y evita
    // tener que crear un constant buffer y su descriptor para 64 bytes.
    D3D12_ROOT_PARAMETER viewProjParam{};
    viewProjParam.ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    viewProjParam.Constants.ShaderRegister = 0;  // b0
    viewProjParam.Constants.RegisterSpace  = 0;  // space0
    viewProjParam.Constants.Num32BitValues = 16;
    viewProjParam.ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters   = &viewProjParam;
    rootDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &serialized, &errorBlob);
    if (FAILED(hr)) {
        std::string detail;
        if (errorBlob)
            detail.assign(static_cast<const char*>(errorBlob->GetBufferPointer()),
                          errorBlob->GetBufferSize());
        throw std::runtime_error("D3D12: D3D12SerializeRootSignature falló (HRESULT " +
                                 hresultToString(hr) + ") " + detail);
    }

    throwIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                              serialized->GetBufferSize(),
                                              IID_PPV_ARGS(&rootSignature)),
                  "ID3D12Device::CreateRootSignature");

    // Los .dxil los produce el build traduciendo el SPIR-V de los mismos .vert
    // y .frag que usa Vulkan, así que se buscan donde los .spv.
    const std::vector<char> vertexShader = readBinaryFile("shaders/gizmo.vert.dxil");
    const std::vector<char> pixelShader  = readBinaryFile("shaders/gizmo.frag.dxil");

    // Semánticas TEXCOORD0/TEXCOORD1: es como spirv-cross traduce
    // layout(location = N), no una elección nuestra.
    const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GizmoVertex, pos),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GizmoVertex, color),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = rootSignature.Get();
    psoDesc.VS                    = {vertexShader.data(), vertexShader.size()};
    psoDesc.PS                    = {pixelShader.data(), pixelShader.size()};
    psoDesc.InputLayout           = {inputLayout, _countof(inputLayout)};
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat             = DXGI_FORMAT_UNKNOWN;  // sin profundidad todavía
    psoDesc.SampleDesc.Count      = 1;
    psoDesc.SampleMask            = UINT_MAX;

    psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;

    for (auto& rt : psoDesc.BlendState.RenderTarget)
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.DepthStencilState.DepthEnable   = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    throwIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&gizmoPipeline)),
                  "ID3D12Device::CreateGraphicsPipelineState");
}

void D3D12Renderer::Impl::createGridGeometry()
{
    // Rejilla del suelo, la misma referencia visual que dibuja el editor con
    // Vulkan: 41 líneas por eje separadas 1 unidad, con los ejes X y Z
    // marcados en color para que se note la orientación.
    constexpr int   kHalf    = 20;
    constexpr float kSpacing = 1.0f;

    std::vector<GizmoVertex> vertices;
    vertices.reserve(static_cast<size_t>(kHalf * 2 + 1) * 4);

    const float extent = static_cast<float>(kHalf) * kSpacing;
    for (int i = -kHalf; i <= kHalf; ++i) {
        const float offset = static_cast<float>(i) * kSpacing;

        const bool  onAxis = (i == 0);
        const float gz[3]  = {onAxis ? 0.3f : 0.35f, onAxis ? 0.3f : 0.35f, onAxis ? 1.0f : 0.35f};
        vertices.push_back({{offset, 0.0f, -extent}, {gz[0], gz[1], gz[2]}});
        vertices.push_back({{offset, 0.0f, extent}, {gz[0], gz[1], gz[2]}});

        const float gx[3] = {onAxis ? 1.0f : 0.35f, onAxis ? 0.3f : 0.35f, onAxis ? 0.3f : 0.35f};
        vertices.push_back({{-extent, 0.0f, offset}, {gx[0], gx[1], gx[2]}});
        vertices.push_back({{extent, 0.0f, offset}, {gx[0], gx[1], gx[2]}});
    }

    const size_t bytes = vertices.size() * sizeof(GizmoVertex);
    gridAllocation = uploadBuffer(vertices.data(), bytes,
                                  D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    gridVertexBufferView.BufferLocation = gridAllocation->GetResource()->GetGPUVirtualAddress();
    gridVertexBufferView.SizeInBytes    = static_cast<UINT>(bytes);
    gridVertexBufferView.StrideInBytes  = sizeof(GizmoVertex);
    gridVertexCount                     = static_cast<UINT>(vertices.size());
}

void D3D12Renderer::Impl::updateViewProj()
{
    const float aspect = (height > 0) ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;

    // Perspective_RH_ZO, no perspective a secas: D3D12 clipea en z=[0,1] igual
    // que Vulkan, y con la convención de OpenGL se pierde la mitad cercana.
    const glm::mat4 projection =
        glm::perspectiveRH_ZO(glm::radians(60.0f), aspect, 0.1f, 500.0f);
    const glm::mat4 view = glm::lookAtRH(glm::vec3(14.0f, 10.0f, 18.0f),
                                         glm::vec3(0.0f, 0.0f, 0.0f),
                                         glm::vec3(0.0f, 1.0f, 0.0f));
    viewProj = projection * view;
}

D3D12Renderer::D3D12Renderer() : m_impl(std::make_unique<Impl>()) {}

D3D12Renderer::~D3D12Renderer()
{
    shutdown();
}

void D3D12Renderer::init(Window& window)
{
    Impl& d = *m_impl;
    if (d.initialized)
        return;

    GLFWwindow* glfwWindow = window.getNativeWindow();
    if (glfwWindow == nullptr)
        throw std::runtime_error("D3D12: la ventana no está inicializada");

    d.hwnd = glfwGetWin32Window(glfwWindow);
    if (d.hwnd == nullptr)
        throw std::runtime_error("D3D12: glfwGetWin32Window no devolvió un HWND");

    int fbWidth = 0, fbHeight = 0;
    glfwGetFramebufferSize(glfwWindow, &fbWidth, &fbHeight);
    d.width  = static_cast<UINT>(fbWidth > 0 ? fbWidth : 1);
    d.height = static_cast<UINT>(fbHeight > 0 ? fbHeight : 1);

    UINT factoryFlags = 0;
#ifndef NDEBUG
    // Capa de depuración ANTES de crear el device: activarla después no afecta
    // a un device ya creado. Si no está el "Graphics Tools" de Windows, falla y
    // se sigue sin ella en vez de impedir el arranque.
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    throwIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&d.factory)),
                  "CreateDXGIFactory2");

    // Adaptador: se prefiere el de más rendimiento si DXGI 1.6 está disponible;
    // si no, el primero hardware que acepte el feature level. Mismo criterio de
    // descarte de WARP que D3D12Support::querySupport.
    ComPtr<IDXGIAdapter1> adapter;
    {
        ComPtr<IDXGIFactory6> factory6;
        if (SUCCEEDED(d.factory.As(&factory6))) {
            for (UINT i = 0;
                 factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                      IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
                 ++i) {
                DXGI_ADAPTER_DESC1 desc{};
                if (FAILED(adapter->GetDesc1(&desc)))
                    continue;
                if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
                    continue;
                if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                                __uuidof(ID3D12Device), nullptr))) {
                    d.adapterName = narrow(desc.Description);
                    break;
                }
                adapter.Reset();
            }
        }

        if (!adapter) {
            for (UINT i = 0; d.factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 desc{};
                if (FAILED(adapter->GetDesc1(&desc)))
                    continue;
                if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
                    continue;
                if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                                __uuidof(ID3D12Device), nullptr))) {
                    d.adapterName = narrow(desc.Description);
                    break;
                }
                adapter.Reset();
            }
        }
    }

    if (!adapter)
        throw std::runtime_error("D3D12: ningún adaptador hardware soporta FEATURE_LEVEL_11_0");

    throwIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d.device)),
                  "D3D12CreateDevice");

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    throwIfFailed(d.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&d.queue)),
                  "ID3D12Device::CreateCommandQueue");

    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.BufferCount = kFrameCount;
    scDesc.Width       = d.width;
    scDesc.Height      = d.height;
    // UNORM, no SRGB: la conversión a espacio de pantalla la hará el pass de
    // composición cuando exista, igual que en el camino Vulkan.
    scDesc.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain1;
    throwIfFailed(d.factory->CreateSwapChainForHwnd(d.queue.Get(), d.hwnd, &scDesc, nullptr,
                                                    nullptr, &swapChain1),
                  "IDXGIFactory4::CreateSwapChainForHwnd");

    // El fullscreen por Alt+Enter de DXGI se lleva mal con una ventana que
    // gestiona GLFW: se desactiva y el modo de pantalla lo decide el motor.
    d.factory->MakeWindowAssociation(d.hwnd, DXGI_MWA_NO_ALT_ENTER);

    throwIfFailed(swapChain1.As(&d.swapChain), "IDXGISwapChain1::QueryInterface(IDXGISwapChain3)");
    d.frameIndex = d.swapChain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = kFrameCount;
    rtvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    throwIfFailed(d.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&d.rtvHeap)),
                  "ID3D12Device::CreateDescriptorHeap(RTV)");
    d.rtvSize = d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    d.createRenderTargetViews();

    for (UINT i = 0; i < kFrameCount; ++i) {
        throwIfFailed(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(&d.allocators[i])),
                      "ID3D12Device::CreateCommandAllocator");
    }

    throwIfFailed(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              d.allocators[d.frameIndex].Get(), nullptr,
                                              IID_PPV_ARGS(&d.commandList)),
                  "ID3D12Device::CreateCommandList");
    // Se crea en estado abierto y drawFrame espera encontrarla cerrada.
    throwIfFailed(d.commandList->Close(), "ID3D12GraphicsCommandList::Close");

    throwIfFailed(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.fence)),
                  "ID3D12Device::CreateFence");
    d.fenceValues.fill(0);
    d.fenceValues[d.frameIndex] = 1;

    d.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (d.fenceEvent == nullptr)
        throwIfFailed(HRESULT_FROM_WIN32(GetLastError()), "CreateEventW");

    // Suballocador de recursos. Va DESPUÉS del fence porque la primera subida
    // de geometría necesita esperar a la GPU para soltar su staging.
    d.adapter = adapter;

    D3D12MA::ALLOCATOR_DESC allocatorDesc{};
    allocatorDesc.pDevice  = d.device.Get();
    allocatorDesc.pAdapter = d.adapter.Get();
    throwIfFailed(D3D12MA::CreateAllocator(&allocatorDesc, &d.allocator),
                  "D3D12MA::CreateAllocator");

    d.createGizmoPipeline();
    d.createGridGeometry();
    d.updateViewProj();

    d.initialized = true;
}

void D3D12Renderer::drawFrame()
{
    Impl& d = *m_impl;
    if (!d.initialized)
        return;

    // Lo primero del frame: el tamaño que anotó el callback de la ventana. Aquí
    // ya estamos en el bucle principal, fuera del WindowProc, así que se puede
    // tocar DXGI y una excepción tiene por dónde salir.
    d.applyPendingResize();

    ID3D12CommandAllocator* allocator = d.allocators[d.frameIndex].Get();
    if (FAILED(allocator->Reset()))
        return;
    if (FAILED(d.commandList->Reset(allocator, nullptr)))
        return;

    D3D12_RESOURCE_BARRIER toRenderTarget{};
    toRenderTarget.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRenderTarget.Transition.pResource   = d.renderTargets[d.frameIndex].Get();
    toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRenderTarget.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    d.commandList->ResourceBarrier(1, &toRenderTarget);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(d.frameIndex) * d.rtvSize;
    d.commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    d.commandList->ClearRenderTargetView(rtv, d.clearColor, 0, nullptr);

    // Viewport y scissor se ponen cada frame: tras un resize el estado del
    // command list se reinicia y arrastrar el tamaño viejo recortaría la imagen.
    D3D12_VIEWPORT viewport{};
    viewport.Width    = static_cast<float>(d.width);
    viewport.Height   = static_cast<float>(d.height);
    viewport.MaxDepth = 1.0f;
    d.commandList->RSSetViewports(1, &viewport);

    D3D12_RECT scissor{0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height)};
    d.commandList->RSSetScissorRects(1, &scissor);

    if (d.gizmoPipeline && d.gridVertexCount > 0) {
        d.commandList->SetPipelineState(d.gizmoPipeline.Get());
        d.commandList->SetGraphicsRootSignature(d.rootSignature.Get());
        // glm guarda la matriz en columnas y el HLSL traducido la declara
        // row_major: los 16 floats crudos se interpretan igual que en Vulkan,
        // sin transponer.
        d.commandList->SetGraphicsRoot32BitConstants(0, 16, &d.viewProj[0][0], 0);
        d.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        d.commandList->IASetVertexBuffers(0, 1, &d.gridVertexBufferView);
        d.commandList->DrawInstanced(d.gridVertexCount, 1, 0, 0);
    }

    D3D12_RESOURCE_BARRIER toPresent = toRenderTarget;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    d.commandList->ResourceBarrier(1, &toPresent);

    if (FAILED(d.commandList->Close()))
        return;

    ID3D12CommandList* lists[] = {d.commandList.Get()};
    d.queue->ExecuteCommandLists(1, lists);

    // Vsync (SyncInterval=1): mismo comportamiento que el camino Vulkan por
    // defecto, y evita quemar la GPU presentando un clear a miles de fps.
    const HRESULT presentHr = d.swapChain->Present(1, 0);
    if (presentHr == DXGI_ERROR_DEVICE_REMOVED || presentHr == DXGI_ERROR_DEVICE_RESET)
        throw std::runtime_error("D3D12: device perdido durante Present (HRESULT " +
                                 hresultToString(presentHr) + ")");

    d.moveToNextFrame();
}

void D3D12Renderer::resize(uint32_t width, uint32_t height)
{
    Impl& d = *m_impl;
    if (!d.initialized)
        return;
    // Ventana minimizada: DXGI rechaza 0x0 y no hay nada que presentar.
    if (width == 0 || height == 0)
        return;

    d.pendingWidth  = width;
    d.pendingHeight = height;
    d.resizePending = true;
}

void D3D12Renderer::Impl::applyPendingResize()
{
    if (!resizePending)
        return;
    resizePending = false;

    if (pendingWidth == width && pendingHeight == height)
        return;

    // La GPU no puede estar usando los buffers viejos.
    waitForGpu();

    // El valor con el que arrancará el frame siguiente. Se captura AQUÍ, con
    // frameIndex todavía apuntando al slot que acaba de esperar: waitForGpu lo
    // dejó en "completado + 1", que es el único valor que se sabe alcanzable.
    const UINT64 nextFenceValue = fenceValues[frameIndex];

    // Y no puede quedar NINGUNA referencia viva a ellos, o ResizeBuffers falla
    // con E_INVALIDARG. Soltar renderTargets no basta: un command list cerrado
    // retiene los recursos que grabó, y el último frame grabó justamente las
    // barreras del back buffer. Resetearlo suelta esa retención; se vuelve a
    // cerrar porque drawFrame espera encontrarlo cerrado.
    for (auto& allocator : allocators) {
        if (allocator)
            allocator->Reset();
    }
    if (commandList && allocators[0]) {
        commandList->Reset(allocators[0].Get(), nullptr);
        commandList->Close();
    }
    releaseRenderTargets();

    throwIfFailed(swapChain->ResizeBuffers(kFrameCount, pendingWidth, pendingHeight,
                                           DXGI_FORMAT_R8G8B8A8_UNORM, 0),
                  "IDXGISwapChain3::ResizeBuffers");

    width      = pendingWidth;
    height     = pendingHeight;
    frameIndex = swapChain->GetCurrentBackBufferIndex();

    // ResizeBuffers puede devolver el índice a CUALQUIER slot, no al siguiente.
    // Los fenceValues por slot dejan entonces de corresponderse con lo que se
    // ha señalado de verdad: un slot puede quedarse guardando un valor que la
    // GPU ya no va a alcanzar nunca, y la espera de moveToNextFrame es
    // INFINITE — el proceso se cuelga mudo, sin error de la API ni de la capa
    // de validación. Igualarlos al valor vivo es lo que rompe esa trampa.
    fenceValues.fill(nextFenceValue);

    createRenderTargetViews();

    // El aspecto de la proyección depende del tamaño: sin esto la rejilla se
    // deforma al estirar la ventana.
    updateViewProj();
}

void D3D12Renderer::setClearColor(float r, float g, float b, float a)
{
    m_impl->clearColor[0] = r;
    m_impl->clearColor[1] = g;
    m_impl->clearColor[2] = b;
    m_impl->clearColor[3] = a;
}

const std::string& D3D12Renderer::adapterName() const
{
    return m_impl->adapterName;
}

void D3D12Renderer::shutdown()
{
    Impl& d = *m_impl;
    if (!d.initialized)
        return;

    // Nada se libera con trabajo en vuelo: soltar un render target que la GPU
    // todavía lee es una corrupción silenciosa, no un error de la API.
    d.waitForGpu();

    if (d.fenceEvent != nullptr) {
        CloseHandle(d.fenceEvent);
        d.fenceEvent = nullptr;
    }

    d.releaseRenderTargets();

    // Los recursos suballocados van ANTES que el allocator: liberar el
    // allocator con allocations vivas es una fuga que solo aparece en el
    // ReportLiveObjects de abajo.
    if (d.gridAllocation) {
        d.gridAllocation->Release();
        d.gridAllocation = nullptr;
    }
    d.gridVertexBufferView = {};
    d.gridVertexCount      = 0;

    d.gizmoPipeline.Reset();
    d.rootSignature.Reset();

    if (d.allocator) {
        d.allocator->Release();
        d.allocator = nullptr;
    }

    for (auto& allocator : d.allocators)
        allocator.Reset();
    d.commandList.Reset();
    d.fence.Reset();
    d.rtvHeap.Reset();
    d.swapChain.Reset();
    d.queue.Reset();
    d.device.Reset();
    d.adapter.Reset();
    d.factory.Reset();
    d.initialized = false;

#ifndef NDEBUG
    // Con el device ya soltado, lo que siga vivo es una fuga nuestra. Sale por
    // la ventana de depuración (DebugView / el output del depurador), que es
    // donde escribe también la capa de validación.
    {
        ComPtr<IDXGIDebug1> dxgiDebug;
        if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug)))) {
            dxgiDebug->ReportLiveObjects(
                DXGI_DEBUG_ALL,
                static_cast<DXGI_DEBUG_RLO_FLAGS>(DXGI_DEBUG_RLO_SUMMARY | DXGI_DEBUG_RLO_IGNORE_INTERNAL));
        }
    }
#endif
}

}  // namespace DonTopo::D3D12

#endif  // DT_D3D12_ENABLED
