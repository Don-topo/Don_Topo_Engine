#include "DonTopo/Renderer/D3D12/D3D12Renderer.h"

#ifdef DT_D3D12_ENABLED

#include "DonTopo/Core/Window.h"
#include "DonTopo/Renderer/Cube.h"
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/Plane.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Renderer/SkinnedMeshPacking.h"
#include "DonTopo/Renderer/Vertex.h"

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
#include <cmath>
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

// Bloque de luz del UBO. Mismo layout que DonTopo::Light y que el struct Light
// de shaders/triangle.frag.
struct ShaderLight {
    float position[4];
    float color[4];
    float direction[4];  // .w = tipo: 0 point, 1 spot, 2 directional, 3 area
    float params[4];     // range, cos interior, cos exterior, ancho
};

// El UBO de set 0 binding 0, con los offsets EXACTOS que spirv-cross generó al
// traducirlo (packoffset c0/c4/c8/c24/c25/c89/c90). Los static_assert de abajo
// son la única defensa real: un desajuste de offsets CPU/GPU no da error en
// ninguna capa de validación, solo píxeles raros.
struct SceneUbo {
    glm::mat4   view;                    // c0
    glm::mat4   proj;                    // c4
    glm::mat4   lightSpaceMatrix[4];     // c8
    glm::vec4   cascadeSplits;           // c24
    ShaderLight lights[16];              // c25
    glm::vec4   viewPos;                 // c89
    int         numLights;               // c90
};

static_assert(offsetof(SceneUbo, view) == 0, "UBO: view debe ir en c0");
static_assert(offsetof(SceneUbo, proj) == 64, "UBO: proj debe ir en c4");
static_assert(offsetof(SceneUbo, lightSpaceMatrix) == 128, "UBO: lightSpaceMatrix debe ir en c8");
static_assert(offsetof(SceneUbo, cascadeSplits) == 384, "UBO: cascadeSplits debe ir en c24");
static_assert(offsetof(SceneUbo, lights) == 400, "UBO: lights debe ir en c25");
static_assert(offsetof(SceneUbo, viewPos) == 1424, "UBO: viewPos debe ir en c89");
static_assert(offsetof(SceneUbo, numLights) == 1440, "UBO: numLights debe ir en c90");

// Push constants de triangle.vert/pbr.frag: mat4 + 2 float + vec2 = 80 bytes.
struct PushData {
    glm::mat4 transform;
    float     metallic;
    float     roughness;
    glm::vec2 flags;  // x: 1 = coger el model del SSBO de instancias
};
static_assert(sizeof(PushData) == 80, "PushData debe ocupar 80 bytes (20 root constants)");

// Push constants de los tres compute de animación. Los tres comparten bloque de
// 16 bytes; en bone_hierarchy y skinning el cuarto campo no se lee.
struct ComputePush {
    float    animTime;
    uint32_t boneCount;
    uint32_t vertexCount;
    uint32_t clipBase;  // activeClip * boneCount
};
static_assert(sizeof(ComputePush) == 16, "ComputePush debe ocupar 16 bytes");

// Sombras en cascada. Mismos valores que el camino Vulkan
// (Renderer.cpp:1016-1025): sin ellos las cascadas cortan a otras distancias y
// las sombras no coinciden entre backends.
constexpr int   kShadowCascades    = 4;
constexpr UINT  kShadowMapSize     = 2048;
constexpr float kCascadeLambda     = 0.75f;
constexpr float kShadowMaxDistance = 500.0f;
constexpr float kCasterMargin      = 200.0f;

// Bloom: niveles de la cadena de reducción. Mismo número que usa el camino
// Vulkan (su log dice "5 mips").
constexpr int kBloomMips = 5;

// Formato del target donde se dibuja la escena. Coma flotante y no UNORM: el
// umbral del bloom solo tiene sentido si el color puede pasar de 1.0, que es
// justo lo que un backbuffer normalizado recorta.
constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// Push compartido por bloom_down y bloom_up: vec2 + 3 float + int = 24 bytes.
struct BloomPush {
    float srcTexel[2];  // 1 / tamaño del nivel de ORIGEN
    float threshold;
    float knee;
    float radius;    // solo lo usa el upsample
    int   prefilter; // != 0 solo en el primer nivel del downsample
};
static_assert(sizeof(BloomPush) == 24, "BloomPush debe ocupar 24 bytes");

// Reparto del heap de descriptores. Los tres primeros tienen que ir seguidos
// porque el shader de malla los pide como t1..t3, y sceneHdr/bloomMip0 también
// porque el de composición los pide como t0..t1.
constexpr UINT kSrvBaseColor = 0;
constexpr UINT kSrvNormalMap = 1;
constexpr UINT kSrvShadowMap = 2;
constexpr UINT kSrvSceneHdr  = 3;
constexpr UINT kSrvBloomMip  = 4;                          // + nivel
constexpr UINT kUavBloomMip  = kSrvBloomMip + kBloomMips;   // + nivel
constexpr UINT kSrvDepth     = kUavBloomMip + kBloomMips;   // profundidad, para la niebla
constexpr UINT kUavSceneHdr  = kSrvDepth + 1;               // la niebla escribe sobre la escena
constexpr UINT kSrvLdr       = kUavSceneHdr + 1;            // salida de la composición, para FXAA
// Rango reservado para ImGui. No basta con uno: desde la 1.92 su backend de
// DX12 pide descriptores por su cuenta (uno por textura, no solo la fuente) a
// través de los callbacks de reserva que se le pasan al inicializarlo.
constexpr UINT kSrvImGui      = kSrvLdr + 1;
constexpr UINT kImGuiReserved = 16;
constexpr UINT kSrvHeapSize   = kSrvImGui + kImGuiReserved;

// Niebla volumétrica: push propio de 128 bytes.
struct FogPush {
    glm::mat4 invViewProj;
    glm::vec4 camPosDensity;      // xyz = cámara en mundo, w = densidad base
    glm::vec4 lightDirFalloff;    // xyz = dirección de la luz key, w = caída por altura
    glm::vec4 scatterBaseHeight;  // rgb = scattering ya multiplicado por la luz, a = altura ref.
    glm::vec4 gStepsRes;          // x = anisotropía, y = pasos, zw = resolución
};
static_assert(sizeof(FogPush) == 128, "FogPush debe ocupar 128 bytes");

// FXAA: vec2 + 3 float = 20 bytes.
struct FxaaPush {
    float invRes[2];
    float subpix;
    float edgeThreshold;
    float edgeThresholdMin;
};
static_assert(sizeof(FxaaPush) == 20, "FxaaPush debe ocupar 20 bytes");

// Stride del vértice que escribe skinning.comp: 5 vec4 (pos, color, uv, normal,
// tangent). No hay struct C++ equivalente en el motor, se usa el tamaño literal.
constexpr UINT kSkinnedOutputStride = 5 * sizeof(glm::vec4);

// Los constant buffers se enlazan con la dirección alineada a 256 bytes.
constexpr UINT64 kCbvAlignment = 256;

UINT64 alignUp(UINT64 value, UINT64 alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

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

    // ── Malla con material: la ruta de triangle.vert/triangle.frag ──────────
    ComPtr<ID3D12RootSignature> meshRootSignature;
    ComPtr<ID3D12PipelineState> meshPipeline;

    D3D12MA::Allocation*     meshVertexAllocation = nullptr;
    D3D12MA::Allocation*     meshIndexAllocation  = nullptr;
    D3D12_VERTEX_BUFFER_VIEW meshVertexBufferView{};
    D3D12_INDEX_BUFFER_VIEW  meshIndexBufferView{};
    UINT                     meshIndexCount = 0;

    // Matrices por instancia (set 1, binding 0 en GLSL → t0 space1 en HLSL).
    D3D12MA::Allocation* instanceAllocation = nullptr;

    // UBO de escena: uno por frame en vuelo, mapeado de forma persistente. Sin
    // separar por slot, escribirlo mientras la GPU lee el frame anterior daría
    // una imagen a medio actualizar.
    std::array<D3D12MA::Allocation*, kFrameCount> sceneUboAllocations{};
    std::array<void*, kFrameCount>                sceneUboMapped{};

    // Texturas del material. Hoy son 1x1 generadas: el cubo del motor es
    // procedural y no trae ninguna, pero los shaders las exigen igual.
    D3D12MA::Allocation* baseColorAllocation = nullptr;
    D3D12MA::Allocation* normalMapAllocation = nullptr;
    D3D12MA::Allocation* shadowMapAllocation = nullptr;

    // t1, t2 y t3 de space0, en este orden.
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    UINT                         srvSize = 0;

    // Profundidad. Se recrea con la ventana.
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    D3D12MA::Allocation*         depthAllocation = nullptr;

    // ── Personaje animado por compute ───────────────────────────────────────
    // Tres pases encadenados, los mismos que el camino Vulkan:
    //   bone_eval      claves de animación -> transformaciones locales
    //   bone_hierarchy locales + jerarquía -> matrices finales de hueso
    //   skinning       vértices + matrices -> vértices ya deformados
    ComPtr<ID3D12RootSignature> boneEvalRootSignature;
    ComPtr<ID3D12RootSignature> boneHierarchyRootSignature;
    ComPtr<ID3D12RootSignature> skinningRootSignature;
    ComPtr<ID3D12PipelineState> boneEvalPipeline;
    ComPtr<ID3D12PipelineState> boneHierarchyPipeline;
    ComPtr<ID3D12PipelineState> skinningPipeline;
    ComPtr<ID3D12PipelineState> skinnedMeshPipeline;

    D3D12MA::Allocation* posKeysAllocation    = nullptr;
    D3D12MA::Allocation* rotKeysAllocation    = nullptr;
    D3D12MA::Allocation* scaleKeysAllocation  = nullptr;
    D3D12MA::Allocation* boneInfosAllocation  = nullptr;
    D3D12MA::Allocation* inputVertsAllocation = nullptr;
    D3D12MA::Allocation* localXformsAllocation = nullptr;
    D3D12MA::Allocation* finalBonesAllocation  = nullptr;
    D3D12MA::Allocation* outputVertsAllocation = nullptr;
    D3D12MA::Allocation* skinnedIndexAllocation = nullptr;

    D3D12_VERTEX_BUFFER_VIEW skinnedVertexBufferView{};
    D3D12_INDEX_BUFFER_VIEW  skinnedIndexBufferView{};
    UINT                     skinnedIndexCount = 0;
    uint32_t                 boneCount         = 0;
    uint32_t                 skinnedVertexCount = 0;
    uint32_t                 clipBase          = 0;
    float                    animTime          = 0.0f;
    float                    animDuration      = 0.0f;
    bool                     hasSkinnedMesh    = false;
    LARGE_INTEGER            lastTick{};
    LARGE_INTEGER            tickFrequency{};

    // ── Suelo sólido ────────────────────────────────────────────────────────
    // La rejilla son líneas y no recibe sombra: hace falta una superficie de
    // verdad para que se vea algo proyectado.
    D3D12MA::Allocation*     groundVertexAllocation = nullptr;
    D3D12MA::Allocation*     groundIndexAllocation  = nullptr;
    D3D12MA::Allocation*     groundInstanceAllocation = nullptr;
    D3D12MA::Allocation*     characterInstanceAllocation = nullptr;
    D3D12_VERTEX_BUFFER_VIEW groundVertexBufferView{};
    D3D12_INDEX_BUFFER_VIEW  groundIndexBufferView{};
    UINT                     groundIndexCount = 0;

    // ── Sombras en cascada ──────────────────────────────────────────────────
    ComPtr<ID3D12RootSignature> shadowRootSignature;
    ComPtr<ID3D12PipelineState> shadowPipeline;         // vértices del motor (56 B)
    ComPtr<ID3D12PipelineState> shadowSkinnedPipeline;  // salida del compute (80 B)
    ComPtr<ID3D12DescriptorHeap> shadowDsvHeap;         // un DSV por cascada
    D3D12MA::Allocation*         shadowMapArrayAllocation = nullptr;
    UINT                         dsvSize = 0;

    glm::mat4 cascadeMatrices[kShadowCascades]{};
    glm::vec4 cascadeSplits{0.0f};
    // Dirección de la luz: la misma que se escribe en el UBO. La posición solo
    // sirve para orientar, igual que en computeCascades.
    glm::vec3 lightDirection{-0.4f, -1.0f, -0.5f};

    // ── Escena fuera de pantalla y bloom ────────────────────────────────────
    // La escena ya no va directa al backbuffer: se dibuja en un target HDR, el
    // bloom trabaja sobre él y un pase de composición escribe el resultado.
    D3D12MA::Allocation* hdrAllocation = nullptr;
    D3D12MA::Allocation* bloomMipAllocations[kBloomMips]{};
    UINT                 bloomMipWidth[kBloomMips]{};
    UINT                 bloomMipHeight[kBloomMips]{};

    ComPtr<ID3D12RootSignature> bloomRootSignature;
    ComPtr<ID3D12PipelineState> bloomDownPipeline;
    ComPtr<ID3D12PipelineState> bloomUpPipeline;
    ComPtr<ID3D12RootSignature> compositeRootSignature;
    ComPtr<ID3D12PipelineState> compositePipeline;

    // Estado de calidad y efectos compartido con el backend de Vulkan. Es el
    // propio D3D12Renderer: el Impl no lo copia, lo consulta, para que un
    // setBloomIntensity() desde fuera se vea en el frame siguiente sin
    // sincronizar nada.
    RendererState* state = nullptr;

    // El radio del tent del bloom NO está en RendererState: el Renderer de
    // Vulkan no lo expone como ajuste, así que sigue siendo local.
    float bloomRadius = 1.0f;

    // ── Niebla y FXAA ───────────────────────────────────────────────────────
    // La niebla escribe SOBRE el target HDR antes del bloom; FXAA va al final,
    // sobre el resultado ya compuesto y en rango LDR.
    ComPtr<ID3D12RootSignature> fogRootSignature;
    ComPtr<ID3D12PipelineState> fogPipeline;
    ComPtr<ID3D12RootSignature> fxaaRootSignature;
    ComPtr<ID3D12PipelineState> fxaaPipeline;
    D3D12MA::Allocation*        ldrAllocation = nullptr;

    // Los parámetros de niebla y FXAA también salen de RendererState.

    // Interfaz de usuario. La dibuja quien conoce ImGui, no este backend.
    std::function<void()> uiDrawCallback;

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

    // Color de fondo en espacio LINEAL, que es lo que espera el target HDR. El
    // pase de composición le aplica ACES y gamma 2.2, así que un 0,10 de antes
    // —cuando la escena iba directa al backbuffer— saldría ahora como un gris
    // medio. Estos valores son los que dan en pantalla el fondo de siempre.
    float       clearColor[4] = {0.02f, 0.02f, 0.025f, 1.0f};
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

    // Sube una textura 2D (o un array de slices 1x1) y le crea su SRV en el
    // hueco `srvIndex` del heap.
    D3D12MA::Allocation* uploadTexture(const void* pixels, UINT width, UINT height,
                                       UINT arraySize, DXGI_FORMAT format,
                                       UINT bytesPerPixel, UINT srvIndex);

    void createMeshPipeline();
    void createMeshResources();
    void createDepthBuffer();
    void updateSceneUbo();

    // Crea un buffer vacío en VRAM con acceso de escritura desordenada, que es
    // lo que necesitan los destinos de los tres compute.
    D3D12MA::Allocation* createStorageBuffer(UINT64 size, D3D12_RESOURCE_STATES initialState);

    void createSkinningPipelines();
    void createSkinnedResources();
    void recordSkinning();  // los tres dispatch, con sus barreras

    void createShadowResources();
    void computeCascades();   // reparte el frustum y saca una matriz por cascada
    void recordShadowPasses();

    // Target HDR de la escena y niveles del bloom. Dependen del tamaño de la
    // ventana, así que se rehacen en cada resize.
    void createHdrTargets();
    void releaseHdrTargets();
    void createBloomPipelines();
    void recordBloomAndComposite(D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv);
    void createFogAndFxaaPipelines();
    void recordFog();

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
    psoDesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count      = 1;
    psoDesc.SampleMask            = UINT_MAX;

    psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;

    for (auto& rt : psoDesc.BlendState.RenderTarget)
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // La rejilla sí se compara contra la profundidad: es el suelo, y lo que
    // haya delante tiene que taparla.
    psoDesc.DepthStencilState.DepthEnable    = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    psoDesc.DepthStencilState.StencilEnable  = FALSE;

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

D3D12MA::Allocation* D3D12Renderer::Impl::uploadTexture(const void* pixels, UINT width,
                                                        UINT height, UINT arraySize,
                                                        DXGI_FORMAT format, UINT bytesPerPixel,
                                                        UINT srvIndex)
{
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = width;
    texDesc.Height           = height;
    texDesc.DepthOrArraySize = static_cast<UINT16>(arraySize);
    texDesc.MipLevels        = 1;
    texDesc.Format           = format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12MA::ALLOCATION_DESC defaultDesc{};
    defaultDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12MA::Allocation* destination = nullptr;
    throwIfFailed(allocator->CreateResource(&defaultDesc, &texDesc,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            &destination, IID_NULL, nullptr),
                  "D3D12MA::Allocator::CreateResource(textura)");

    // El staging no se escribe fila a fila como en memoria: cada fila va
    // alineada a D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, y cada slice del array es
    // un subrecurso propio con su footprint.
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(arraySize);
    std::vector<UINT>                               rowCounts(arraySize);
    std::vector<UINT64>                             rowSizes(arraySize);
    UINT64                                          stagingSize = 0;
    device->GetCopyableFootprints(&texDesc, 0, arraySize, 0, footprints.data(), rowCounts.data(),
                                  rowSizes.data(), &stagingSize);

    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width            = stagingSize;
    bufferDesc.Height           = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels        = 1;
    bufferDesc.Format           = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12MA::ALLOCATION_DESC uploadDesc{};
    uploadDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

    D3D12MA::Allocation* staging = nullptr;
    HRESULT hr = allocator->CreateResource(&uploadDesc, &bufferDesc,
                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                           &staging, IID_NULL, nullptr);
    if (FAILED(hr)) {
        destination->Release();
        throwIfFailed(hr, "D3D12MA::Allocator::CreateResource(staging de textura)");
    }

    uint8_t*          mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    hr = staging->GetResource()->Map(0, &noRead, reinterpret_cast<void**>(&mapped));
    if (FAILED(hr)) {
        staging->Release();
        destination->Release();
        throwIfFailed(hr, "ID3D12Resource::Map(staging de textura)");
    }

    const auto* source = static_cast<const uint8_t*>(pixels);
    for (UINT slice = 0; slice < arraySize; ++slice) {
        for (UINT row = 0; row < rowCounts[slice]; ++row) {
            std::memcpy(mapped + footprints[slice].Offset +
                            static_cast<UINT64>(row) * footprints[slice].Footprint.RowPitch,
                        source + (static_cast<UINT64>(slice) * height + row) * width * bytesPerPixel,
                        static_cast<size_t>(rowSizes[slice]));
        }
    }
    staging->GetResource()->Unmap(0, nullptr);

    throwIfFailed(allocators[frameIndex]->Reset(), "ID3D12CommandAllocator::Reset(textura)");
    throwIfFailed(commandList->Reset(allocators[frameIndex].Get(), nullptr),
                  "ID3D12GraphicsCommandList::Reset(textura)");

    for (UINT slice = 0; slice < arraySize; ++slice) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource        = destination->GetResource();
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = slice;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource       = staging->GetResource();
        src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprints[slice];

        commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    D3D12_RESOURCE_BARRIER toShader{};
    toShader.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toShader.Transition.pResource   = destination->GetResource();
    toShader.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toShader.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toShader.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &toShader);

    throwIfFailed(commandList->Close(), "ID3D12GraphicsCommandList::Close(textura)");
    ID3D12CommandList* lists[] = {commandList.Get()};
    queue->ExecuteCommandLists(1, lists);
    waitForGpu();
    staging->Release();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                  = format;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (arraySize > 1) {
        srvDesc.ViewDimension                  = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.MipLevels       = 1;
        srvDesc.Texture2DArray.ArraySize       = arraySize;
    } else {
        srvDesc.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(srvIndex) * srvSize;
    device->CreateShaderResourceView(destination->GetResource(), &srvDesc, handle);

    return destination;
}

void D3D12Renderer::Impl::createDepthBuffer()
{
    if (depthAllocation) {
        depthAllocation->Release();
        depthAllocation = nullptr;
    }

    D3D12_RESOURCE_DESC depthDesc{};
    depthDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width            = width;
    depthDesc.Height           = height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels        = 1;
    // TYPELESS y no D32_FLOAT: la niebla necesita LEER esta profundidad como
    // textura, y el mismo recurso no puede declararse a la vez con formato de
    // profundidad y de muestreo.
    depthDesc.Format           = DXGI_FORMAT_R32_TYPELESS;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    // El valor de limpieza va declarado: si el del ClearDepthStencilView no
    // coincide con este, la capa de validación lo señala y la GPU pierde la
    // ruta rápida de limpieza.
    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format               = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth   = 1.0f;

    D3D12MA::ALLOCATION_DESC defaultDesc{};
    defaultDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    throwIfFailed(allocator->CreateResource(&defaultDesc, &depthDesc,
                                            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
                                            &depthAllocation, IID_NULL, nullptr),
                  "D3D12MA::Allocator::CreateResource(depth)");

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format        = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(depthAllocation->GetResource(), &dsvDesc,
                                   dsvHeap->GetCPUDescriptorHandleForHeapStart());

    // Vista de muestreo del mismo recurso, para la niebla. El heap todavía no
    // existe en la primera llamada (init crea el depth antes que el heap): en
    // ese caso la crea createHdrTargets, que corre después.
    if (srvHeap) {
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
        depthSrv.Format                  = DXGI_FORMAT_R32_FLOAT;
        depthSrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrv.Texture2D.MipLevels     = 1;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(kSrvDepth) * srvSize;
        device->CreateShaderResourceView(depthAllocation->GetResource(), &depthSrv, handle);
    }
}

void D3D12Renderer::Impl::createMeshPipeline()
{
    // Root signature de triangle.vert/triangle.frag:
    //   b0 space0  UBO de escena          -> root CBV
    //   b1 space0  push constants         -> 20 root constants
    //   t1..t3     base, normal, sombras  -> tabla de descriptores
    //   t0 space1  matrices por instancia -> root SRV
    // El UBO declara b0 explícitamente y el bloque de push constants no declara
    // registro, así que DXC le asigna el siguiente libre: b1.
    D3D12_DESCRIPTOR_RANGE textureRange{};
    textureRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRange.NumDescriptors     = 3;
    textureRange.BaseShaderRegister = 1;  // t1, t2, t3
    textureRange.RegisterSpace      = 0;
    textureRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 1;
    params[1].Constants.Num32BitValues = sizeof(PushData) / 4;
    params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges   = &textureRange;
    params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    params[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[3].Descriptor.ShaderRegister = 0;
    params[3].Descriptor.RegisterSpace  = 1;
    params[3].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    // Samplers estáticos: los shaders no eligen filtro ni wrap en tiempo de
    // ejecución, así que no hace falta un heap de samplers ni descriptores.
    D3D12_STATIC_SAMPLER_DESC samplers[3]{};
    for (int i = 0; i < 2; ++i) {
        samplers[i].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[i].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[i].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[i].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[i].MaxLOD           = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister   = 1 + i;  // s1, s2
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // s3 es el del sampler2DArrayShadow: comparación, no filtrado normal.
    samplers[2].Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplers[2].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[2].MaxLOD           = D3D12_FLOAT32_MAX;
    samplers[2].ShaderRegister   = 3;
    samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters     = _countof(params);
    rootDesc.pParameters       = params;
    rootDesc.NumStaticSamplers = _countof(samplers);
    rootDesc.pStaticSamplers   = samplers;
    rootDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized,
                                             &errorBlob);
    if (FAILED(hr)) {
        std::string detail;
        if (errorBlob)
            detail.assign(static_cast<const char*>(errorBlob->GetBufferPointer()),
                          errorBlob->GetBufferSize());
        throw std::runtime_error("D3D12: root signature de malla (HRESULT " +
                                 hresultToString(hr) + ") " + detail);
    }

    throwIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                              serialized->GetBufferSize(),
                                              IID_PPV_ARGS(&meshRootSignature)),
                  "ID3D12Device::CreateRootSignature(malla)");

    const std::vector<char> vertexShader = readBinaryFile("shaders/triangle.vert.dxil");
    const std::vector<char> pixelShader  = readBinaryFile("shaders/triangle.frag.dxil");

    // El orden y los offsets salen de DonTopo::Vertex; las semánticas, de cómo
    // spirv-cross traduce layout(location = N).
    const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, pos),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, color),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, uv),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, normal),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 4, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, tangent),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = meshRootSignature.Get();
    psoDesc.VS                    = {vertexShader.data(), vertexShader.size()};
    psoDesc.PS                    = {pixelShader.data(), pixelShader.size()};
    psoDesc.InputLayout           = {inputLayout, _countof(inputLayout)};
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count      = 1;
    psoDesc.SampleMask            = UINT_MAX;

    psoDesc.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode        = D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    // El motor genera los triángulos con el criterio de Vulkan; en D3D12 el
    // eje Y de pantalla va al revés, así que lo que allí es antihorario aquí
    // se ve horario. Sin esto, el cubo se dibuja del revés y desaparece.
    psoDesc.RasterizerState.FrontCounterClockwise = TRUE;

    for (auto& rt : psoDesc.BlendState.RenderTarget)
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.DepthStencilState.DepthEnable    = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    throwIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&meshPipeline)),
                  "ID3D12Device::CreateGraphicsPipelineState(malla)");
}

void D3D12Renderer::Impl::createMeshResources()
{
    // La malla es la del propio motor: mismo generador que usa el editor para
    // las "Basic Shapes", con su Vertex y su orden de índices.
    const Mesh cube = Cube::create(2.0f, glm::vec3(1.0f, 1.0f, 1.0f));

    meshVertexAllocation = uploadBuffer(cube.vertices.data(), cube.vertices.size() * sizeof(Vertex),
                                        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    meshVertexBufferView.BufferLocation = meshVertexAllocation->GetResource()->GetGPUVirtualAddress();
    meshVertexBufferView.SizeInBytes    = static_cast<UINT>(cube.vertices.size() * sizeof(Vertex));
    meshVertexBufferView.StrideInBytes  = sizeof(Vertex);

    meshIndexAllocation = uploadBuffer(cube.indices.data(), cube.indices.size() * sizeof(uint32_t),
                                       D3D12_RESOURCE_STATE_INDEX_BUFFER);
    meshIndexBufferView.BufferLocation = meshIndexAllocation->GetResource()->GetGPUVirtualAddress();
    meshIndexBufferView.SizeInBytes    = static_cast<UINT>(cube.indices.size() * sizeof(uint32_t));
    meshIndexBufferView.Format         = DXGI_FORMAT_R32_UINT;
    meshIndexCount                     = static_cast<UINT>(cube.indices.size());

    // Una sola instancia, y a propósito NO con la identidad: sube el cubo una
    // unidad para que se apoye sobre la rejilla en vez de quedar medio
    // enterrado. Así la posición final demuestra que el shader leyó de verdad
    // este buffer — con la identidad, un SSBO que no se lee daría exactamente
    // la misma imagen que uno que sí.
    const glm::mat4 instanceModel = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    instanceAllocation = uploadBuffer(&instanceModel, sizeof(instanceModel),
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Heap de los tres SRV que pide el fragment shader.
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.NumDescriptors = kSrvHeapSize;
    srvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    throwIfFailed(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap)),
                  "ID3D12Device::CreateDescriptorHeap(SRV)");
    srvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Texturas 1x1: el cubo es procedural y no trae ninguna, pero los shaders
    // las muestrean igual. Blanca para el color base y (0.5, 0.5, 1) para la
    // normal, que es la normal sin perturbar.
    const uint8_t white[4]      = {255, 255, 255, 255};
    const uint8_t flatNormal[4] = {128, 128, 255, 255};
    baseColorAllocation = uploadTexture(white, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 0);
    normalMapAllocation = uploadTexture(flatNormal, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 1);

    // Mapa de sombras: 1x1 por cascada a profundidad máxima. Con cascadeSplits
    // a cero, selectCascade devuelve -1 y el shader ni lo muestrea; existe
    // porque la root signature tiene que satisfacer el t3 que declara.
    const float noShadow[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    shadowMapAllocation = uploadTexture(noShadow, 1, 1, 4, DXGI_FORMAT_R32_FLOAT, 4, 2);

    // UBO por frame en vuelo, mapeado de forma persistente: se reescribe cada
    // frame y desmapear/remapear no aporta nada.
    D3D12_RESOURCE_DESC uboDesc{};
    uboDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    uboDesc.Width            = alignUp(sizeof(SceneUbo), kCbvAlignment);
    uboDesc.Height           = 1;
    uboDesc.DepthOrArraySize = 1;
    uboDesc.MipLevels        = 1;
    uboDesc.Format           = DXGI_FORMAT_UNKNOWN;
    uboDesc.SampleDesc.Count = 1;
    uboDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12MA::ALLOCATION_DESC uploadDesc{};
    uploadDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

    for (UINT i = 0; i < kFrameCount; ++i) {
        throwIfFailed(allocator->CreateResource(&uploadDesc, &uboDesc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                &sceneUboAllocations[i], IID_NULL, nullptr),
                      "D3D12MA::Allocator::CreateResource(UBO)");
        const D3D12_RANGE noRead{0, 0};
        throwIfFailed(sceneUboAllocations[i]->GetResource()->Map(0, &noRead, &sceneUboMapped[i]),
                      "ID3D12Resource::Map(UBO)");
    }
}

void D3D12Renderer::Impl::updateSceneUbo()
{
    const float aspect = (height > 0) ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;

    SceneUbo ubo{};
    ubo.view = glm::lookAtRH(glm::vec3(6.0f, 4.5f, 8.0f), glm::vec3(0.0f, 0.5f, 0.0f),
                             glm::vec3(0.0f, 1.0f, 0.0f));
    ubo.proj = glm::perspectiveRH_ZO(glm::radians(60.0f), aspect, 0.1f, 500.0f);
    // Vulkan tiene el eje Y de pantalla invertido respecto a OpenGL y el motor
    // lo compensa ahí; D3D12 usa la misma orientación que OpenGL, así que aquí
    // NO se invierte.

    ubo.cascadeSplits = cascadeSplits;
    for (int i = 0; i < kShadowCascades; ++i)
        ubo.lightSpaceMatrix[i] = cascadeMatrices[i];

    // Una direccional (tipo 2), que es lo que el shader trata sin atenuación.
    // La dirección tiene que ser LA MISMA con la que se calcularon las
    // cascadas, o la sombra caería en un sitio y la luz vendría de otro.
    ubo.numLights              = 1;
    ubo.lights[0].direction[0] = lightDirection.x;
    ubo.lights[0].direction[1] = lightDirection.y;
    ubo.lights[0].direction[2] = lightDirection.z;
    ubo.lights[0].direction[3] = 2.0f;  // directional
    ubo.lights[0].color[0]     = 1.0f;
    ubo.lights[0].color[1]     = 0.98f;
    ubo.lights[0].color[2]     = 0.94f;
    // Intensidad por debajo de 1: con albedo blanco y la luz a 1.0 la escena
    // entera se planta en 1.0, y entonces el umbral del bloom deja pasar hasta
    // el suelo y lava la imagen. Con 0,7 queda rango por debajo del umbral.
    ubo.lights[0].color[3]     = 0.7f;

    ubo.viewPos = glm::vec4(6.0f, 4.5f, 8.0f, 1.0f);

    std::memcpy(sceneUboMapped[frameIndex], &ubo, sizeof(ubo));
}

D3D12MA::Allocation* D3D12Renderer::Impl::createStorageBuffer(UINT64 size,
                                                              D3D12_RESOURCE_STATES initialState)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = size;
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12MA::Allocation* allocation = nullptr;
    throwIfFailed(allocator->CreateResource(&allocDesc, &desc, initialState, nullptr, &allocation,
                                            IID_NULL, nullptr),
                  "D3D12MA::Allocator::CreateResource(storage)");
    return allocation;
}

void D3D12Renderer::Impl::createSkinningPipelines()
{
    // Una root signature por pase, con EXACTAMENTE los registros que declara
    // cada shader. Todos los buffers son ByteAddressBuffer, así que van como
    // root descriptors y no hacen falta tablas ni heaps.
    //
    // Ojo con los recursos que cambian de vista entre pases: localXforms es u4
    // cuando bone_eval lo escribe y t4 cuando bone_hierarchy lo lee; finalBones
    // es u5 al escribirse y t5 al leerse. Es el mismo buffer.
    struct Slot {
        D3D12_ROOT_PARAMETER_TYPE type;
        UINT                      shaderRegister;
    };

    auto buildRootSignature = [&](const std::vector<Slot>& slots,
                                  ComPtr<ID3D12RootSignature>& out, const char* what) {
        std::vector<D3D12_ROOT_PARAMETER> params;
        params.reserve(slots.size() + 1);

        // Los push constants van SIEMPRE en b0, que es donde DXC coloca el
        // cbuffer del bloque push_constant al no llevar registro explícito.
        D3D12_ROOT_PARAMETER pushParam{};
        pushParam.ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        pushParam.Constants.ShaderRegister = 0;
        pushParam.Constants.Num32BitValues = sizeof(ComputePush) / 4;
        params.push_back(pushParam);

        for (const Slot& slot : slots) {
            D3D12_ROOT_PARAMETER param{};
            param.ParameterType             = slot.type;
            param.Descriptor.ShaderRegister = slot.shaderRegister;
            params.push_back(param);
        }

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = static_cast<UINT>(params.size());
        desc.pParameters   = params.data();
        desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized,
                                                 &errorBlob);
        if (FAILED(hr)) {
            std::string detail;
            if (errorBlob)
                detail.assign(static_cast<const char*>(errorBlob->GetBufferPointer()),
                              errorBlob->GetBufferSize());
            throw std::runtime_error(std::string("D3D12: root signature de ") + what +
                                     " (HRESULT " + hresultToString(hr) + ") " + detail);
        }
        throwIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                  serialized->GetBufferSize(), IID_PPV_ARGS(&out)),
                      "ID3D12Device::CreateRootSignature(compute)");
    };

    using RT = D3D12_ROOT_PARAMETER_TYPE;
    // bone_eval: lee claves y huesos (t0..t3), escribe transformaciones locales (u4)
    buildRootSignature({{RT::D3D12_ROOT_PARAMETER_TYPE_SRV, 0},
                        {RT::D3D12_ROOT_PARAMETER_TYPE_SRV, 1},
                        {RT::D3D12_ROOT_PARAMETER_TYPE_SRV, 2},
                        {RT::D3D12_ROOT_PARAMETER_TYPE_SRV, 3},
                        {RT::D3D12_ROOT_PARAMETER_TYPE_UAV, 4}},
                       boneEvalRootSignature, "bone_eval");
    // bone_hierarchy: lee huesos y locales (t3, t4), escribe matrices finales (u5)
    buildRootSignature({{RT::D3D12_ROOT_PARAMETER_TYPE_SRV, 3},
                        {RT::D3D12_ROOT_PARAMETER_TYPE_SRV, 4},
                        {RT::D3D12_ROOT_PARAMETER_TYPE_UAV, 5}},
                       boneHierarchyRootSignature, "bone_hierarchy");
    // skinning: lee matrices y vértices (t5, t6), escribe vértices deformados (u7)
    buildRootSignature({{RT::D3D12_ROOT_PARAMETER_TYPE_SRV, 5},
                        {RT::D3D12_ROOT_PARAMETER_TYPE_SRV, 6},
                        {RT::D3D12_ROOT_PARAMETER_TYPE_UAV, 7}},
                       skinningRootSignature, "skinning");

    auto buildComputePipeline = [&](const char* dxilPath, ID3D12RootSignature* rootSignature,
                                    ComPtr<ID3D12PipelineState>& out) {
        const std::vector<char> shader = readBinaryFile(dxilPath);
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = rootSignature;
        desc.CS             = {shader.data(), shader.size()};
        throwIfFailed(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&out)),
                      "ID3D12Device::CreateComputePipelineState");
    };

    buildComputePipeline("shaders/bone_eval.comp.dxil", boneEvalRootSignature.Get(),
                         boneEvalPipeline);
    buildComputePipeline("shaders/bone_hierarchy.comp.dxil", boneHierarchyRootSignature.Get(),
                         boneHierarchyPipeline);
    buildComputePipeline("shaders/skinning.comp.dxil", skinningRootSignature.Get(),
                         skinningPipeline);

    // Pipeline gráfico del personaje: MISMO triangle.vert/frag que el cubo, pero
    // el vertex buffer es la salida del compute, que va en vec4 alineados (5 x
    // vec4 = 80 B) en vez del Vertex empaquetado del motor.
    const std::vector<char> vertexShader = readBinaryFile("shaders/triangle.vert.dxil");
    const std::vector<char> pixelShader  = readBinaryFile("shaders/triangle.frag.dxil");

    const D3D12_INPUT_ELEMENT_DESC skinnedLayout[] = {
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 16,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, 32,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 4, DXGI_FORMAT_R32G32B32_FLOAT, 0, 64,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = meshRootSignature.Get();
    psoDesc.VS                    = {vertexShader.data(), vertexShader.size()};
    psoDesc.PS                    = {pixelShader.data(), pixelShader.size()};
    psoDesc.InputLayout           = {skinnedLayout, _countof(skinnedLayout)};
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count      = 1;
    psoDesc.SampleMask            = UINT_MAX;

    psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;
    psoDesc.RasterizerState.FrontCounterClockwise = TRUE;

    for (auto& rt : psoDesc.BlendState.RenderTarget)
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.DepthStencilState.DepthEnable    = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    throwIfFailed(device->CreateGraphicsPipelineState(&psoDesc,
                                                      IID_PPV_ARGS(&skinnedMeshPipeline)),
                  "ID3D12Device::CreateGraphicsPipelineState(skinned)");
}

void D3D12Renderer::Impl::createSkinnedResources()
{
    // Se carga con el MISMO ModelLoader que usa el editor: si el FBX no está,
    // el backend sigue funcionando sin personaje en vez de abortar.
    SkinnedMesh mesh;
    try {
        mesh = ModelLoader::loadSkinned("assets/animatedCharacter/Maw J Laygo.fbx");
    } catch (const std::exception&) {
        return;
    }

    if (mesh.skinnedVertices.empty() || mesh.skeleton.names.empty() || mesh.indices.empty())
        return;

    boneCount          = static_cast<uint32_t>(mesh.skeleton.names.size());
    skinnedVertexCount = static_cast<uint32_t>(mesh.skinnedVertices.size());

    const PackedClips packed = packSkinnedClips(mesh);
    if (packed.boneInfos.empty())
        return;

    // El clip 0 es el que se reproduce. boneInfos va en layout [clip][hueso],
    // así que el offset del clip activo es clip * boneCount.
    clipBase     = 0;
    animDuration = mesh.animationClips.empty() ? 0.0f : mesh.animationClips[0].duration;

    posKeysAllocation   = uploadBuffer(packed.pos.data(), packed.pos.size() * sizeof(GpuPosKey),
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    rotKeysAllocation   = uploadBuffer(packed.rot.data(), packed.rot.size() * sizeof(GpuRotKey),
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    scaleKeysAllocation = uploadBuffer(packed.scale.data(), packed.scale.size() * sizeof(GpuPosKey),
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    boneInfosAllocation = uploadBuffer(packed.boneInfos.data(),
                                       packed.boneInfos.size() * sizeof(GpuBoneInfo),
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    inputVertsAllocation =
        uploadBuffer(mesh.skinnedVertices.data(), mesh.skinnedVertices.size() * sizeof(SkinnedVertex),
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    localXformsAllocation = createStorageBuffer(static_cast<UINT64>(boneCount) * sizeof(glm::mat4),
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    finalBonesAllocation  = createStorageBuffer(static_cast<UINT64>(boneCount) * sizeof(glm::mat4),
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    outputVertsAllocation = createStorageBuffer(
        static_cast<UINT64>(skinnedVertexCount) * kSkinnedOutputStride,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    skinnedIndexAllocation = uploadBuffer(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t),
                                          D3D12_RESOURCE_STATE_INDEX_BUFFER);

    skinnedVertexBufferView.BufferLocation = outputVertsAllocation->GetResource()->GetGPUVirtualAddress();
    skinnedVertexBufferView.SizeInBytes    = skinnedVertexCount * kSkinnedOutputStride;
    skinnedVertexBufferView.StrideInBytes  = kSkinnedOutputStride;

    skinnedIndexBufferView.BufferLocation = skinnedIndexAllocation->GetResource()->GetGPUVirtualAddress();
    skinnedIndexBufferView.SizeInBytes    = static_cast<UINT>(mesh.indices.size() * sizeof(uint32_t));
    skinnedIndexBufferView.Format         = DXGI_FORMAT_R32_UINT;
    skinnedIndexCount                     = static_cast<UINT>(mesh.indices.size());

    QueryPerformanceFrequency(&tickFrequency);
    QueryPerformanceCounter(&lastTick);
    hasSkinnedMesh = true;
}

void D3D12Renderer::Impl::recordSkinning()
{
    ComputePush push{};
    push.animTime    = animTime;
    push.boneCount   = boneCount;
    push.vertexCount = skinnedVertexCount;
    push.clipBase    = clipBase;

    const D3D12_GPU_VIRTUAL_ADDRESS posKeys     = posKeysAllocation->GetResource()->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS rotKeys     = rotKeysAllocation->GetResource()->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS scaleKeys   = scaleKeysAllocation->GetResource()->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS boneInfos   = boneInfosAllocation->GetResource()->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS localXforms = localXformsAllocation->GetResource()->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS finalBones  = finalBonesAllocation->GetResource()->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS inputVerts  = inputVertsAllocation->GetResource()->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS outputVerts = outputVertsAllocation->GetResource()->GetGPUVirtualAddress();

    // Barrera de UAV, no de transición: los pases no cambian de estado, solo
    // hay que garantizar que lo escrito por uno lo vea el siguiente.
    auto uavBarrier = [&](ID3D12Resource* resource) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = resource;
        commandList->ResourceBarrier(1, &barrier);
    };

    // 1) Claves de animación -> transformaciones locales. Un hilo por hueso.
    commandList->SetComputeRootSignature(boneEvalRootSignature.Get());
    commandList->SetPipelineState(boneEvalPipeline.Get());
    commandList->SetComputeRoot32BitConstants(0, sizeof(ComputePush) / 4, &push, 0);
    commandList->SetComputeRootShaderResourceView(1, posKeys);
    commandList->SetComputeRootShaderResourceView(2, rotKeys);
    commandList->SetComputeRootShaderResourceView(3, scaleKeys);
    commandList->SetComputeRootShaderResourceView(4, boneInfos);
    commandList->SetComputeRootUnorderedAccessView(5, localXforms);
    commandList->Dispatch((boneCount + 63) / 64, 1, 1);
    uavBarrier(localXformsAllocation->GetResource());

    // 2) Jerarquía: acumula padre a hijo. Un SOLO hilo a propósito — depende
    // de que el padre ya esté resuelto, y el orden topológico lo garantiza.
    commandList->SetComputeRootSignature(boneHierarchyRootSignature.Get());
    commandList->SetPipelineState(boneHierarchyPipeline.Get());
    commandList->SetComputeRoot32BitConstants(0, sizeof(ComputePush) / 4, &push, 0);
    commandList->SetComputeRootShaderResourceView(1, boneInfos);
    commandList->SetComputeRootShaderResourceView(2, localXforms);
    commandList->SetComputeRootUnorderedAccessView(3, finalBones);
    commandList->Dispatch(1, 1, 1);
    uavBarrier(finalBonesAllocation->GetResource());

    // 3) Deformación de los vértices. Un hilo por vértice.
    commandList->SetComputeRootSignature(skinningRootSignature.Get());
    commandList->SetPipelineState(skinningPipeline.Get());
    commandList->SetComputeRoot32BitConstants(0, sizeof(ComputePush) / 4, &push, 0);
    commandList->SetComputeRootShaderResourceView(1, finalBones);
    commandList->SetComputeRootShaderResourceView(2, inputVerts);
    commandList->SetComputeRootUnorderedAccessView(3, outputVerts);
    commandList->Dispatch((skinnedVertexCount + 63) / 64, 1, 1);

    // De escritura por compute a entrada del ensamblador de vértices: aquí sí
    // cambia el uso del buffer, así que hace falta transición.
    D3D12_RESOURCE_BARRIER toVertexBuffer{};
    toVertexBuffer.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toVertexBuffer.Transition.pResource   = outputVertsAllocation->GetResource();
    toVertexBuffer.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toVertexBuffer.Transition.StateAfter  = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    toVertexBuffer.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &toVertexBuffer);
}

void D3D12Renderer::Impl::releaseHdrTargets()
{
    if (hdrAllocation) {
        hdrAllocation->Release();
        hdrAllocation = nullptr;
    }
    if (ldrAllocation) {
        ldrAllocation->Release();
        ldrAllocation = nullptr;
    }
    for (auto& mip : bloomMipAllocations) {
        if (mip) {
            mip->Release();
            mip = nullptr;
        }
    }
}

void D3D12Renderer::Impl::createHdrTargets()
{
    releaseHdrTargets();

    // Target de la escena, en coma flotante para que el umbral del bloom pueda
    // distinguir lo que pasa de 1.0.
    D3D12_RESOURCE_DESC hdrDesc{};
    hdrDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    hdrDesc.Width            = width;
    hdrDesc.Height           = height;
    hdrDesc.DepthOrArraySize = 1;
    hdrDesc.MipLevels        = 1;
    hdrDesc.Format           = kHdrFormat;
    hdrDesc.SampleDesc.Count = 1;
    // Render target para la escena y acceso desordenado para la niebla, que
    // reescribe este mismo contenido antes de que lo lea el bloom.
    hdrDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_CLEAR_VALUE hdrClear{};
    hdrClear.Format = kHdrFormat;
    std::memcpy(hdrClear.Color, clearColor, sizeof(clearColor));

    D3D12MA::ALLOCATION_DESC defaultDesc{};
    defaultDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    throwIfFailed(allocator->CreateResource(&defaultDesc, &hdrDesc,
                                            D3D12_RESOURCE_STATE_RENDER_TARGET, &hdrClear,
                                            &hdrAllocation, IID_NULL, nullptr),
                  "D3D12MA::Allocator::CreateResource(HDR)");

    // El RTV del target va detrás de los de la swapchain, en el mismo heap.
    D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    hdrRtv.ptr += static_cast<SIZE_T>(kFrameCount) * rtvSize;
    device->CreateRenderTargetView(hdrAllocation->GetResource(), nullptr, hdrRtv);

    D3D12_SHADER_RESOURCE_VIEW_DESC hdrSrv{};
    hdrSrv.Format                  = kHdrFormat;
    hdrSrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    hdrSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    hdrSrv.Texture2D.MipLevels     = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(kSrvSceneHdr) * srvSize;
    device->CreateShaderResourceView(hdrAllocation->GetResource(), &hdrSrv, handle);

    // Vista de escritura del mismo target, la que usa la niebla.
    D3D12_UNORDERED_ACCESS_VIEW_DESC hdrUav{};
    hdrUav.Format        = kHdrFormat;
    hdrUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(kUavSceneHdr) * srvSize;
    device->CreateUnorderedAccessView(hdrAllocation->GetResource(), nullptr, &hdrUav, handle);

    // La profundidad se creó antes que el heap en el arranque, así que su vista
    // de muestreo se registra aquí.
    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
    depthSrv.Format                  = DXGI_FORMAT_R32_FLOAT;
    depthSrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrv.Texture2D.MipLevels     = 1;
    handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(kSrvDepth) * srvSize;
    device->CreateShaderResourceView(depthAllocation->GetResource(), &depthSrv, handle);

    // Target LDR: lo escribe la composición y lo lee FXAA. Sin él, FXAA tendría
    // que leer del backbuffer mientras escribe en él.
    D3D12_RESOURCE_DESC ldrDesc = hdrDesc;
    ldrDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ldrDesc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE ldrClear{};
    ldrClear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    throwIfFailed(allocator->CreateResource(&defaultDesc, &ldrDesc,
                                            D3D12_RESOURCE_STATE_RENDER_TARGET, &ldrClear,
                                            &ldrAllocation, IID_NULL, nullptr),
                  "D3D12MA::Allocator::CreateResource(LDR)");

    D3D12_CPU_DESCRIPTOR_HANDLE ldrRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    ldrRtv.ptr += static_cast<SIZE_T>(kFrameCount + 1) * rtvSize;
    device->CreateRenderTargetView(ldrAllocation->GetResource(), nullptr, ldrRtv);

    D3D12_SHADER_RESOURCE_VIEW_DESC ldrSrv{};
    ldrSrv.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    ldrSrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    ldrSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ldrSrv.Texture2D.MipLevels     = 1;
    handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(kSrvLdr) * srvSize;
    device->CreateShaderResourceView(ldrAllocation->GetResource(), &ldrSrv, handle);

    // Niveles del bloom: texturas independientes en vez de mips de un mismo
    // recurso. Cada una tiene un solo subrecurso, así que su estado se cambia
    // de una pieza y no hay que llevar la cuenta por nivel.
    for (int level = 0; level < kBloomMips; ++level) {
        bloomMipWidth[level]  = (std::max)(1u, width >> (level + 1));
        bloomMipHeight[level] = (std::max)(1u, height >> (level + 1));

        D3D12_RESOURCE_DESC mipDesc{};
        mipDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        mipDesc.Width            = bloomMipWidth[level];
        mipDesc.Height           = bloomMipHeight[level];
        mipDesc.DepthOrArraySize = 1;
        mipDesc.MipLevels        = 1;
        mipDesc.Format           = kHdrFormat;
        mipDesc.SampleDesc.Count = 1;
        mipDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        throwIfFailed(allocator->CreateResource(&defaultDesc, &mipDesc,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                &bloomMipAllocations[level], IID_NULL, nullptr),
                      "D3D12MA::Allocator::CreateResource(bloom)");

        D3D12_SHADER_RESOURCE_VIEW_DESC mipSrv{};
        mipSrv.Format                  = kHdrFormat;
        mipSrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        mipSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        mipSrv.Texture2D.MipLevels     = 1;

        handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(kSrvBloomMip + level) * srvSize;
        device->CreateShaderResourceView(bloomMipAllocations[level]->GetResource(), &mipSrv, handle);

        D3D12_UNORDERED_ACCESS_VIEW_DESC mipUav{};
        mipUav.Format        = kHdrFormat;
        mipUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(kUavBloomMip + level) * srvSize;
        device->CreateUnorderedAccessView(bloomMipAllocations[level]->GetResource(), nullptr,
                                          &mipUav, handle);
    }
}

void D3D12Renderer::Impl::createBloomPipelines()
{
    // Los dos compute comparten firma: constantes, una textura de origen y una
    // imagen de destino. Texture2D y RWTexture2D no pueden ir como root
    // descriptors —solo los buffers pueden—, así que van en tablas.
    D3D12_DESCRIPTOR_RANGE srcRange{};
    srcRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srcRange.NumDescriptors     = 1;
    srcRange.BaseShaderRegister = 0;  // t0

    D3D12_DESCRIPTOR_RANGE dstRange{};
    dstRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    dstRange.NumDescriptors     = 1;
    dstRange.BaseShaderRegister = 1;  // u1

    D3D12_ROOT_PARAMETER bloomParams[3]{};
    bloomParams[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    bloomParams[0].Constants.ShaderRegister = 0;
    bloomParams[0].Constants.Num32BitValues = sizeof(BloomPush) / 4;

    bloomParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    bloomParams[1].DescriptorTable.NumDescriptorRanges = 1;
    bloomParams[1].DescriptorTable.pDescriptorRanges   = &srcRange;

    bloomParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    bloomParams[2].DescriptorTable.NumDescriptorRanges = 1;
    bloomParams[2].DescriptorTable.pDescriptorRanges   = &dstRange;

    // Clamp en los bordes: con wrap, el filtro de 13 taps traería color del
    // lado opuesto de la imagen y el bloom sangraría de un borde a otro.
    D3D12_STATIC_SAMPLER_DESC bloomSampler{};
    bloomSampler.Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    bloomSampler.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    bloomSampler.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    bloomSampler.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    bloomSampler.MaxLOD         = D3D12_FLOAT32_MAX;
    bloomSampler.ShaderRegister = 0;

    auto serializeAndCreate = [&](const D3D12_ROOT_SIGNATURE_DESC& desc,
                                  ComPtr<ID3D12RootSignature>& out, const char* what) {
        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized,
                                                 &errorBlob);
        if (FAILED(hr)) {
            std::string detail;
            if (errorBlob)
                detail.assign(static_cast<const char*>(errorBlob->GetBufferPointer()),
                              errorBlob->GetBufferSize());
            throw std::runtime_error(std::string("D3D12: root signature de ") + what + " (HRESULT " +
                                     hresultToString(hr) + ") " + detail);
        }
        throwIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                  serialized->GetBufferSize(), IID_PPV_ARGS(&out)),
                      "ID3D12Device::CreateRootSignature");
    };

    D3D12_ROOT_SIGNATURE_DESC bloomDesc{};
    bloomDesc.NumParameters     = _countof(bloomParams);
    bloomDesc.pParameters       = bloomParams;
    bloomDesc.NumStaticSamplers = 1;
    bloomDesc.pStaticSamplers   = &bloomSampler;
    serializeAndCreate(bloomDesc, bloomRootSignature, "bloom");

    auto buildComputePipeline = [&](const char* path, ComPtr<ID3D12PipelineState>& out) {
        const std::vector<char>           shader = readBinaryFile(path);
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = bloomRootSignature.Get();
        desc.CS             = {shader.data(), shader.size()};
        throwIfFailed(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&out)),
                      "ID3D12Device::CreateComputePipelineState(bloom)");
    };
    buildComputePipeline("shaders/bloom_down.comp.dxil", bloomDownPipeline);
    buildComputePipeline("shaders/bloom_up.comp.dxil", bloomUpPipeline);

    // Composición: escena + bloom -> backbuffer. t0 y t1 tienen que caer en
    // descriptores contiguos, y por eso sceneHdr y el nivel 0 del bloom están
    // pegados en el heap.
    D3D12_DESCRIPTOR_RANGE compositeRange{};
    compositeRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    compositeRange.NumDescriptors     = 2;
    compositeRange.BaseShaderRegister = 0;  // t0, t1

    D3D12_ROOT_PARAMETER compositeParams[2]{};
    compositeParams[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    compositeParams[0].Constants.ShaderRegister = 0;
    compositeParams[0].Constants.Num32BitValues = 1;  // float intensity
    compositeParams[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

    compositeParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    compositeParams[1].DescriptorTable.NumDescriptorRanges = 1;
    compositeParams[1].DescriptorTable.pDescriptorRanges   = &compositeRange;
    compositeParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC compositeSamplers[2]{};
    for (int i = 0; i < 2; ++i) {
        compositeSamplers[i].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        compositeSamplers[i].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        compositeSamplers[i].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        compositeSamplers[i].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        compositeSamplers[i].MaxLOD           = D3D12_FLOAT32_MAX;
        compositeSamplers[i].ShaderRegister   = i;
        compositeSamplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_ROOT_SIGNATURE_DESC compositeDesc{};
    compositeDesc.NumParameters     = _countof(compositeParams);
    compositeDesc.pParameters       = compositeParams;
    compositeDesc.NumStaticSamplers = _countof(compositeSamplers);
    compositeDesc.pStaticSamplers   = compositeSamplers;
    serializeAndCreate(compositeDesc, compositeRootSignature, "composición");

    // fullscreen.vert genera el triángulo desde el índice de vértice: sin
    // vertex buffer y sin input layout.
    const std::vector<char> fullscreenVs = readBinaryFile("shaders/fullscreen.vert.dxil");
    const std::vector<char> compositePs  = readBinaryFile("shaders/bloom_composite.frag.dxil");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = compositeRootSignature.Get();
    psoDesc.VS                    = {fullscreenVs.data(), fullscreenVs.size()};
    psoDesc.PS                    = {compositePs.data(), compositePs.size()};
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    psoDesc.SampleDesc.Count      = 1;
    psoDesc.SampleMask            = UINT_MAX;

    psoDesc.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    for (auto& rt : psoDesc.BlendState.RenderTarget)
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.DepthStencilState.DepthEnable   = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    throwIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&compositePipeline)),
                  "ID3D12Device::CreateGraphicsPipelineState(composición)");
}

void D3D12Renderer::Impl::createFogAndFxaaPipelines()
{
    auto serializeAndCreate = [&](const D3D12_ROOT_SIGNATURE_DESC& desc,
                                  ComPtr<ID3D12RootSignature>& out, const char* what) {
        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized,
                                                 &errorBlob);
        if (FAILED(hr)) {
            std::string detail;
            if (errorBlob)
                detail.assign(static_cast<const char*>(errorBlob->GetBufferPointer()),
                              errorBlob->GetBufferSize());
            throw std::runtime_error(std::string("D3D12: root signature de ") + what + " (HRESULT " +
                                     hresultToString(hr) + ") " + detail);
        }
        throwIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                  serialized->GetBufferSize(), IID_PPV_ARGS(&out)),
                      "ID3D12Device::CreateRootSignature");
    };

    // ── Niebla ──────────────────────────────────────────────────────────────
    // u0 = escena (lectura y escritura), t1 = profundidad, t3 = sombras,
    // b2 = el mismo UBO de escena. Su cbuffer solo declara hasta cascadeSplits,
    // pero los offsets son los mismos, así que se enlaza el buffer entero.
    D3D12_DESCRIPTOR_RANGE fogHdrRange{};
    fogHdrRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    fogHdrRange.NumDescriptors     = 1;
    fogHdrRange.BaseShaderRegister = 0;  // u0

    D3D12_DESCRIPTOR_RANGE fogDepthRange{};
    fogDepthRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    fogDepthRange.NumDescriptors     = 1;
    fogDepthRange.BaseShaderRegister = 1;  // t1

    D3D12_DESCRIPTOR_RANGE fogShadowRange{};
    fogShadowRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    fogShadowRange.NumDescriptors     = 1;
    fogShadowRange.BaseShaderRegister = 3;  // t3

    D3D12_ROOT_PARAMETER fogParams[5]{};
    fogParams[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    fogParams[0].Constants.ShaderRegister = 0;
    fogParams[0].Constants.Num32BitValues = sizeof(FogPush) / 4;

    fogParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    fogParams[1].Descriptor.ShaderRegister = 2;  // b2

    fogParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    fogParams[2].DescriptorTable.NumDescriptorRanges = 1;
    fogParams[2].DescriptorTable.pDescriptorRanges   = &fogHdrRange;

    fogParams[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    fogParams[3].DescriptorTable.NumDescriptorRanges = 1;
    fogParams[3].DescriptorTable.pDescriptorRanges   = &fogDepthRange;

    fogParams[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    fogParams[4].DescriptorTable.NumDescriptorRanges = 1;
    fogParams[4].DescriptorTable.pDescriptorRanges   = &fogShadowRange;

    D3D12_STATIC_SAMPLER_DESC fogSamplers[2]{};
    fogSamplers[0].Filter         = D3D12_FILTER_MIN_MAG_MIP_POINT;  // profundidad: sin filtrar
    fogSamplers[0].AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    fogSamplers[0].AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    fogSamplers[0].AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    fogSamplers[0].MaxLOD         = D3D12_FLOAT32_MAX;
    fogSamplers[0].ShaderRegister = 1;  // s1

    fogSamplers[1].Filter         = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    fogSamplers[1].AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    fogSamplers[1].AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    fogSamplers[1].AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    fogSamplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    fogSamplers[1].MaxLOD         = D3D12_FLOAT32_MAX;
    fogSamplers[1].ShaderRegister = 3;  // s3

    D3D12_ROOT_SIGNATURE_DESC fogDesc{};
    fogDesc.NumParameters     = _countof(fogParams);
    fogDesc.pParameters       = fogParams;
    fogDesc.NumStaticSamplers = _countof(fogSamplers);
    fogDesc.pStaticSamplers   = fogSamplers;
    serializeAndCreate(fogDesc, fogRootSignature, "niebla");

    {
        const std::vector<char>           shader = readBinaryFile("shaders/fog.comp.dxil");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = fogRootSignature.Get();
        desc.CS             = {shader.data(), shader.size()};
        throwIfFailed(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&fogPipeline)),
                      "ID3D12Device::CreateComputePipelineState(niebla)");
    }

    // ── FXAA ────────────────────────────────────────────────────────────────
    D3D12_DESCRIPTOR_RANGE fxaaRange{};
    fxaaRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    fxaaRange.NumDescriptors     = 1;
    fxaaRange.BaseShaderRegister = 0;  // t0

    D3D12_ROOT_PARAMETER fxaaParams[2]{};
    fxaaParams[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    fxaaParams[0].Constants.ShaderRegister = 0;
    fxaaParams[0].Constants.Num32BitValues = sizeof(FxaaPush) / 4;
    fxaaParams[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

    fxaaParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    fxaaParams[1].DescriptorTable.NumDescriptorRanges = 1;
    fxaaParams[1].DescriptorTable.pDescriptorRanges   = &fxaaRange;
    fxaaParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC fxaaSampler{};
    fxaaSampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    fxaaSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    fxaaSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    fxaaSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    fxaaSampler.MaxLOD           = D3D12_FLOAT32_MAX;
    fxaaSampler.ShaderRegister   = 0;
    fxaaSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC fxaaDesc{};
    fxaaDesc.NumParameters     = _countof(fxaaParams);
    fxaaDesc.pParameters       = fxaaParams;
    fxaaDesc.NumStaticSamplers = 1;
    fxaaDesc.pStaticSamplers   = &fxaaSampler;
    serializeAndCreate(fxaaDesc, fxaaRootSignature, "FXAA");

    const std::vector<char> fullscreenVs = readBinaryFile("shaders/fullscreen.vert.dxil");
    const std::vector<char> fxaaPs       = readBinaryFile("shaders/fxaa.frag.dxil");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = fxaaRootSignature.Get();
    psoDesc.VS                    = {fullscreenVs.data(), fullscreenVs.size()};
    psoDesc.PS                    = {fxaaPs.data(), fxaaPs.size()};
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    psoDesc.SampleDesc.Count      = 1;
    psoDesc.SampleMask            = UINT_MAX;

    psoDesc.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    for (auto& rt : psoDesc.BlendState.RenderTarget)
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.DepthStencilState.DepthEnable   = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    throwIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&fxaaPipeline)),
                  "ID3D12Device::CreateGraphicsPipelineState(FXAA)");
}

void D3D12Renderer::Impl::recordFog()
{
    const D3D12_GPU_DESCRIPTOR_HANDLE heapStart = srvHeap->GetGPUDescriptorHandleForHeapStart();
    auto gpuHandle = [&](UINT index) {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = heapStart;
        handle.ptr += static_cast<UINT64>(index) * srvSize;
        return handle;
    };

    auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                          D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter  = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &barrier);
    };

    // La escena pasa a escritura desordenada y la profundidad a lectura.
    transition(hdrAllocation->GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(depthAllocation->GetResource(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const float     aspect = (height > 0) ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    const glm::mat4 proj   = glm::perspectiveRH_ZO(glm::radians(60.0f), aspect, 0.1f, 500.0f);
    const glm::vec3 camPos(6.0f, 4.5f, 8.0f);
    const glm::mat4 view = glm::lookAtRH(camPos, glm::vec3(0.0f, 0.5f, 0.0f),
                                         glm::vec3(0.0f, 1.0f, 0.0f));

    FogPush push{};
    // La misma proyección con la que se grabó la profundidad. En Vulkan aquí
    // entra su inversión de Y; en D3D12 no hay ninguna que meter.
    push.invViewProj      = glm::inverse(proj * view);
    push.camPosDensity    = glm::vec4(camPos, state->fogDensity());
    push.lightDirFalloff  = glm::vec4(glm::normalize(lightDirection), state->fogHeightFalloff());
    // El scattering va YA multiplicado por el color y la intensidad de la luz.
    push.scatterBaseHeight = glm::vec4(state->fogScatter() * glm::vec3(1.0f, 0.98f, 0.94f) * 0.7f,
                                       state->fogBaseHeight());
    push.gStepsRes = glm::vec4(state->fogAnisotropy(), static_cast<float>(state->fogSteps()),
                               static_cast<float>(width), static_cast<float>(height));

    ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(fogRootSignature.Get());
    commandList->SetPipelineState(fogPipeline.Get());
    commandList->SetComputeRoot32BitConstants(0, sizeof(FogPush) / 4, &push, 0);
    commandList->SetComputeRootConstantBufferView(
        1, sceneUboAllocations[frameIndex]->GetResource()->GetGPUVirtualAddress());
    commandList->SetComputeRootDescriptorTable(2, gpuHandle(kUavSceneHdr));
    commandList->SetComputeRootDescriptorTable(3, gpuHandle(kSrvDepth));
    commandList->SetComputeRootDescriptorTable(4, gpuHandle(kSrvShadowMap));
    commandList->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

    // Y se devuelven a lo que espera el resto del frame.
    transition(depthAllocation->GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_DEPTH_WRITE);
    transition(hdrAllocation->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
}

void D3D12Renderer::Impl::recordBloomAndComposite(D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv)
{
    // El heap lo deja puesto el pase de escena, pero este pase no puede
    // depender de que ese se haya grabado.
    ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);

    const D3D12_GPU_DESCRIPTOR_HANDLE heapStart = srvHeap->GetGPUDescriptorHandleForHeapStart();
    auto gpuHandle = [&](UINT index) {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = heapStart;
        handle.ptr += static_cast<UINT64>(index) * srvSize;
        return handle;
    };

    auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                          D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = resource;
        barrier.Transition.StateBefore  = before;
        barrier.Transition.StateAfter   = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &barrier);
    };

    // La escena deja de ser destino de dibujo y pasa a leerse: primero por el
    // compute del bloom, después por el pase de composición.
    transition(hdrAllocation->GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

    commandList->SetComputeRootSignature(bloomRootSignature.Get());

    // Reducción. El primer nivel lee la escena y aplica el umbral; el resto
    // solo filtra el nivel anterior.
    for (int level = 0; level < kBloomMips; ++level) {
        const UINT srcWidth  = (level == 0) ? width : bloomMipWidth[level - 1];
        const UINT srcHeight = (level == 0) ? height : bloomMipHeight[level - 1];

        BloomPush push{};
        push.srcTexel[0] = 1.0f / static_cast<float>(srcWidth);
        push.srcTexel[1] = 1.0f / static_cast<float>(srcHeight);
        push.threshold   = state->bloomThreshold();
        push.knee        = state->bloomKnee();
        push.radius      = bloomRadius;
        push.prefilter   = (level == 0) ? 1 : 0;

        commandList->SetPipelineState(bloomDownPipeline.Get());
        commandList->SetComputeRoot32BitConstants(0, sizeof(BloomPush) / 4, &push, 0);
        commandList->SetComputeRootDescriptorTable(
            1, gpuHandle(level == 0 ? kSrvSceneHdr : kSrvBloomMip + level - 1));
        commandList->SetComputeRootDescriptorTable(2, gpuHandle(kUavBloomMip + level));
        commandList->Dispatch((bloomMipWidth[level] + 7) / 8, (bloomMipHeight[level] + 7) / 8, 1);

        // Este nivel pasa a ser origen del siguiente paso.
        transition(bloomMipAllocations[level]->GetResource(),
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // Ampliación: cada nivel SUMA el de abajo sobre lo que ya tenía, así que el
    // destino vuelve a acceso desordenado para poder leerse y escribirse.
    for (int level = kBloomMips - 2; level >= 0; --level) {
        BloomPush push{};
        push.srcTexel[0] = 1.0f / static_cast<float>(bloomMipWidth[level + 1]);
        push.srcTexel[1] = 1.0f / static_cast<float>(bloomMipHeight[level + 1]);
        push.threshold   = state->bloomThreshold();
        push.knee        = state->bloomKnee();
        push.radius      = bloomRadius;
        push.prefilter   = 0;

        transition(bloomMipAllocations[level]->GetResource(),
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        commandList->SetPipelineState(bloomUpPipeline.Get());
        commandList->SetComputeRoot32BitConstants(0, sizeof(BloomPush) / 4, &push, 0);
        commandList->SetComputeRootDescriptorTable(1, gpuHandle(kSrvBloomMip + level + 1));
        commandList->SetComputeRootDescriptorTable(2, gpuHandle(kUavBloomMip + level));
        commandList->Dispatch((bloomMipWidth[level] + 7) / 8, (bloomMipHeight[level] + 7) / 8, 1);

        // Y vuelve a origen para el nivel siguiente (o para la composición, si
        // este era el último).
        transition(bloomMipAllocations[level]->GetResource(),
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   level == 0 ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                              : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // Composición al backbuffer.
    //
    // Viewport de ALTURA NEGATIVA, y no es un truco gratuito: fullscreen.vert
    // saca la uv de las mismas coordenadas que la posición (uv = ndc*0.5+0.5)
    // dando por hecho que el NDC y=-1 es la fila de ARRIBA, que es como funciona
    // Vulkan. En D3D12 y=-1 es la de abajo, así que el mismo shader deja la
    // imagen del revés. Invertir el viewport lo corrige sin tocar un shader que
    // comparten los dos backends.
    D3D12_VIEWPORT viewport{};
    viewport.TopLeftY = static_cast<float>(height);
    viewport.Width    = static_cast<float>(width);
    viewport.Height   = -static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};

    // La composición NO va al backbuffer: va al target LDR, que es lo que lee
    // FXAA después. Un pase no puede leer y escribir la misma imagen.
    D3D12_CPU_DESCRIPTOR_HANDLE ldrRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    ldrRtv.ptr += static_cast<SIZE_T>(kFrameCount + 1) * rtvSize;

    commandList->OMSetRenderTargets(1, &ldrRtv, FALSE, nullptr);
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->SetPipelineState(compositePipeline.Get());
    commandList->SetGraphicsRootSignature(compositeRootSignature.Get());
    const float bloomIntensity = state->bloomIntensity();
    commandList->SetGraphicsRoot32BitConstants(0, 1, &bloomIntensity, 0);
    commandList->SetGraphicsRootDescriptorTable(1, gpuHandle(kSrvSceneHdr));
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // Sin vertex buffer: fullscreen.vert saca las tres posiciones del índice.
    commandList->DrawInstanced(3, 1, 0, 0);

    // FXAA sobre el resultado ya compuesto, y de ahí al backbuffer.
    transition(ldrAllocation->GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    FxaaPush fxaaPush{};
    fxaaPush.invRes[0]        = 1.0f / static_cast<float>(width);
    fxaaPush.invRes[1]        = 1.0f / static_cast<float>(height);
    fxaaPush.subpix           = state->fxaaSubpix();
    fxaaPush.edgeThreshold    = state->fxaaEdgeThreshold();
    fxaaPush.edgeThresholdMin = state->fxaaEdgeThresholdMin();

    commandList->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);
    commandList->SetPipelineState(fxaaPipeline.Get());
    commandList->SetGraphicsRootSignature(fxaaRootSignature.Get());
    commandList->SetGraphicsRoot32BitConstants(0, sizeof(FxaaPush) / 4, &fxaaPush, 0);
    commandList->SetGraphicsRootDescriptorTable(1, gpuHandle(kSrvLdr));
    commandList->DrawInstanced(3, 1, 0, 0);

    transition(ldrAllocation->GetResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);

    // La interfaz va encima de todo, sobre el backbuffer y sin post-procesado:
    // suavizar los bordes del texto de la UI lo emborronaría.
    if (uiDrawCallback) {
        commandList->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);
        uiDrawCallback();
    }

    // Todo vuelve al estado con el que arranca el frame siguiente.
    transition(hdrAllocation->GetResource(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(bloomMipAllocations[0]->GetResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (int level = 1; level < kBloomMips; ++level) {
        transition(bloomMipAllocations[level]->GetResource(),
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
}

void D3D12Renderer::Impl::createShadowResources()
{
    // Suelo: la malla del propio motor, la misma que usa el editor.
    // Un pelo por debajo de y=0, que es donde vive la rejilla: en el mismo
    // plano se pelean por la profundidad y las líneas salen punteadas.
    const Mesh ground = Plane::create(200.0f, -0.02f, glm::vec3(0.55f, 0.55f, 0.58f), 20.0f);

    groundVertexAllocation = uploadBuffer(ground.vertices.data(),
                                          ground.vertices.size() * sizeof(Vertex),
                                          D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    groundVertexBufferView.BufferLocation = groundVertexAllocation->GetResource()->GetGPUVirtualAddress();
    groundVertexBufferView.SizeInBytes    = static_cast<UINT>(ground.vertices.size() * sizeof(Vertex));
    groundVertexBufferView.StrideInBytes  = sizeof(Vertex);

    groundIndexAllocation = uploadBuffer(ground.indices.data(),
                                         ground.indices.size() * sizeof(uint32_t),
                                         D3D12_RESOURCE_STATE_INDEX_BUFFER);
    groundIndexBufferView.BufferLocation = groundIndexAllocation->GetResource()->GetGPUVirtualAddress();
    groundIndexBufferView.SizeInBytes    = static_cast<UINT>(ground.indices.size() * sizeof(uint32_t));
    groundIndexBufferView.Format         = DXGI_FORMAT_R32_UINT;
    groundIndexCount                     = static_cast<UINT>(ground.indices.size());

    // shadow.vert SIEMPRE saca el model del buffer de instancias, incluso para
    // objetos que en el pase principal usan el push constant. Por eso cada
    // objeto necesita el suyo, con la MISMA transformación que se usa al
    // dibujarlo: si difieren, la sombra cae donde no está el objeto.
    const glm::mat4 groundModel = glm::mat4(1.0f);
    groundInstanceAllocation = uploadBuffer(&groundModel, sizeof(groundModel),
                                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const glm::mat4 characterModel =
        glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(-3.0f, 0.0f, 0.0f)), glm::vec3(0.02f));
    characterInstanceAllocation = uploadBuffer(&characterModel, sizeof(characterModel),
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Mapa de sombras: un array de profundidad, una capa por cascada.
    D3D12_RESOURCE_DESC shadowDesc{};
    shadowDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    shadowDesc.Width            = kShadowMapSize;
    shadowDesc.Height           = kShadowMapSize;
    shadowDesc.DepthOrArraySize = kShadowCascades;
    shadowDesc.MipLevels        = 1;
    // TYPELESS porque el mismo recurso se ve de dos formas: como profundidad
    // al grabarlo (D32_FLOAT) y como textura al muestrearlo (R32_FLOAT).
    shadowDesc.Format           = DXGI_FORMAT_R32_TYPELESS;
    shadowDesc.SampleDesc.Count = 1;
    shadowDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE shadowClear{};
    shadowClear.Format             = DXGI_FORMAT_D32_FLOAT;
    shadowClear.DepthStencil.Depth = 1.0f;

    D3D12MA::ALLOCATION_DESC defaultDesc{};
    defaultDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    throwIfFailed(allocator->CreateResource(&defaultDesc, &shadowDesc,
                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                            &shadowClear, &shadowMapArrayAllocation, IID_NULL,
                                            nullptr),
                  "D3D12MA::Allocator::CreateResource(shadow map)");

    D3D12_DESCRIPTOR_HEAP_DESC shadowDsvHeapDesc{};
    shadowDsvHeapDesc.NumDescriptors = kShadowCascades;
    shadowDsvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    throwIfFailed(device->CreateDescriptorHeap(&shadowDsvHeapDesc, IID_PPV_ARGS(&shadowDsvHeap)),
                  "ID3D12Device::CreateDescriptorHeap(shadow DSV)");
    dsvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    // Un DSV por capa: cada cascada se graba por separado.
    for (int cascade = 0; cascade < kShadowCascades; ++cascade) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format                         = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.FirstArraySlice = cascade;
        dsvDesc.Texture2DArray.ArraySize       = 1;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(cascade) * dsvSize;
        device->CreateDepthStencilView(shadowMapArrayAllocation->GetResource(), &dsvDesc, handle);
    }

    // El SRV va en el hueco t3, encima del array 1x1 de relleno que ocupaba ese
    // sitio: a partir de aquí el shader muestrea sombras de verdad.
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                         = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension                  = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Shader4ComponentMapping        = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2DArray.MipLevels       = 1;
    srvDesc.Texture2DArray.ArraySize       = kShadowCascades;

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    srvHandle.ptr += static_cast<SIZE_T>(2) * srvSize;
    device->CreateShaderResourceView(shadowMapArrayAllocation->GetResource(), &srvDesc, srvHandle);

    // Root signature del pase de sombras: el MISMO UBO en b0 (los offsets de
    // view/proj/lightSpaceMatrix coinciden con los de triangle), el índice de
    // cascada como root constant y el buffer de instancias.
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 1;
    params[1].Constants.Num32BitValues = 1;  // uint cascade
    params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;

    params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[2].Descriptor.ShaderRegister = 0;
    params[2].Descriptor.RegisterSpace  = 1;
    params[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = _countof(params);
    rootDesc.pParameters   = params;
    rootDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized,
                                             &errorBlob);
    if (FAILED(hr)) {
        std::string detail;
        if (errorBlob)
            detail.assign(static_cast<const char*>(errorBlob->GetBufferPointer()),
                          errorBlob->GetBufferSize());
        throw std::runtime_error("D3D12: root signature de sombras (HRESULT " +
                                 hresultToString(hr) + ") " + detail);
    }
    throwIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                              serialized->GetBufferSize(),
                                              IID_PPV_ARGS(&shadowRootSignature)),
                  "ID3D12Device::CreateRootSignature(sombras)");

    const std::vector<char> shadowVs = readBinaryFile("shaders/shadow.vert.dxil");

    // Dos PSO porque hay dos formatos de vértice: el del motor y el que escribe
    // el compute de skinning. shadow.vert solo lee la posición, así que basta
    // con un elemento, pero el stride tiene que ser el que toca.
    auto buildShadowPipeline = [&](UINT stride, ComPtr<ID3D12PipelineState>& out) {
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature        = shadowRootSignature.Get();
        psoDesc.VS                    = {shadowVs.data(), shadowVs.size()};
        // Sin pixel shader: el pase solo escribe profundidad.
        psoDesc.InputLayout           = {layout, _countof(layout)};
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets      = 0;
        psoDesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
        psoDesc.SampleDesc.Count      = 1;
        psoDesc.SampleMask            = UINT_MAX;

        psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
        // Sin culling en el pase de sombras: descartar caras traseras deja sin
        // proyectar los objetos que se ven por dentro y abre huecos.
        psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
        psoDesc.RasterizerState.DepthClipEnable       = TRUE;
        psoDesc.RasterizerState.FrontCounterClockwise = TRUE;
        // Sesgo de profundidad contra el acné de sombra: sin él, la superficie
        // se sombrea a sí misma en bandas.
        psoDesc.RasterizerState.DepthBias            = 2000;
        psoDesc.RasterizerState.SlopeScaledDepthBias = 2.0f;

        psoDesc.DepthStencilState.DepthEnable    = TRUE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;

        (void)stride;  // el stride va en la vista del vertex buffer, no en el PSO
        throwIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&out)),
                      "ID3D12Device::CreateGraphicsPipelineState(sombras)");
    };

    buildShadowPipeline(sizeof(Vertex), shadowPipeline);
    buildShadowPipeline(kSkinnedOutputStride, shadowSkinnedPipeline);
}

void D3D12Renderer::Impl::computeCascades()
{
    for (auto& matrix : cascadeMatrices)
        matrix = glm::mat4(1.0f);
    cascadeSplits = glm::vec4(0.0f);

    const float aspect = (height > 0) ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    const glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(60.0f), aspect, 0.1f, 500.0f);
    const glm::mat4 view = glm::lookAtRH(glm::vec3(6.0f, 4.5f, 8.0f), glm::vec3(0.0f, 0.5f, 0.0f),
                                         glm::vec3(0.0f, 1.0f, 0.0f));

    // Esquinas del frustum desproyectando el cubo NDC. z de 0 a 1, que es el
    // rango que clipea D3D12 (y también Vulkan).
    const glm::mat4 invViewProj = glm::inverse(proj * view);
    glm::vec3       cornerNear[4], cornerFar[4];
    const float     ndcX[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
    const float     ndcY[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
    for (int i = 0; i < 4; ++i) {
        const glm::vec4 pn = invViewProj * glm::vec4(ndcX[i], ndcY[i], 0.0f, 1.0f);
        const glm::vec4 pf = invViewProj * glm::vec4(ndcX[i], ndcY[i], 1.0f, 1.0f);
        if (std::abs(pn.w) < 1e-8f || std::abs(pf.w) < 1e-8f)
            return;
        cornerNear[i] = glm::vec3(pn) / pn.w;
        cornerFar[i]  = glm::vec3(pf) / pf.w;
    }

    const float camNear = -(view * glm::vec4(cornerNear[0], 1.0f)).z;
    const float camFar  = -(view * glm::vec4(cornerFar[0], 1.0f)).z;
    if (!std::isfinite(camNear) || !std::isfinite(camFar) || camNear <= 0.0f || camFar <= camNear)
        return;

    const float shadowFar = (std::min)(camFar, kShadowMaxDistance);
    if (shadowFar <= camNear)
        return;

    const glm::vec3 lightDir = glm::normalize(lightDirection);
    const glm::vec3 up = std::abs(lightDir.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                      : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::mat4 lightRot    = glm::lookAt(glm::vec3(0.0f), lightDir, up);
    const glm::mat4 invLightRot = glm::inverse(lightRot);

    float prevDist = camNear;
    for (int c = 0; c < kShadowCascades; ++c) {
        const float p        = static_cast<float>(c + 1) / static_cast<float>(kShadowCascades);
        const float logSplit = camNear * std::pow(shadowFar / camNear, p);
        const float uniSplit = camNear + (shadowFar - camNear) * p;
        const float dist     = kCascadeLambda * logSplit + (1.0f - kCascadeLambda) * uniSplit;
        cascadeSplits[c]     = dist;

        const float tNear = (prevDist - camNear) / (camFar - camNear);
        const float tFar  = (dist - camNear) / (camFar - camNear);

        glm::vec3 corners[8];
        for (int i = 0; i < 4; ++i) {
            const glm::vec3 ray = cornerFar[i] - cornerNear[i];
            corners[i]          = cornerNear[i] + ray * tNear;
            corners[i + 4]      = cornerNear[i] + ray * tFar;
        }

        // Esfera envolvente y no AABB: el radio no depende de hacia dónde mire
        // la cámara, así que girar en el sitio no hace latir las sombras.
        glm::vec3 center(0.0f);
        for (const glm::vec3& v : corners)
            center += v;
        center /= 8.0f;
        float radius = 0.0f;
        for (const glm::vec3& v : corners)
            radius = (std::max)(radius, glm::length(v - center));
        radius = std::ceil(radius * 16.0f) / 16.0f;
        if (radius < 1e-4f)
            radius = 1e-4f;

        // Snap del centro a téxeles: sin esto los bordes de sombra hierven al
        // mover la cámara.
        const float unitsPerTexel = (2.0f * radius) / static_cast<float>(kShadowMapSize);
        glm::vec3   centerLS      = glm::vec3(lightRot * glm::vec4(center, 1.0f));
        centerLS.x = std::floor(centerLS.x / unitsPerTexel) * unitsPerTexel;
        centerLS.y = std::floor(centerLS.y / unitsPerTexel) * unitsPerTexel;
        center     = glm::vec3(invLightRot * glm::vec4(centerLS, 1.0f));

        const glm::mat4 lightView =
            glm::lookAt(center - lightDir * (radius + kCasterMargin), center, up);
        const glm::mat4 lightProj =
            glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + kCasterMargin);
        // El camino Vulkan hace aquí lightProj[1][1] *= -1. En D3D12 NO: es la
        // misma inversión del eje Y que ya no se aplica a la proyección de
        // cámara, y repetirla dejaría las sombras del revés.
        cascadeMatrices[c] = lightProj * lightView;

        prevDist = dist;
    }
}

void D3D12Renderer::Impl::recordShadowPasses()
{
    D3D12_RESOURCE_BARRIER toDepthWrite{};
    toDepthWrite.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toDepthWrite.Transition.pResource   = shadowMapArrayAllocation->GetResource();
    toDepthWrite.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toDepthWrite.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    toDepthWrite.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &toDepthWrite);

    D3D12_VIEWPORT shadowViewport{};
    shadowViewport.Width    = static_cast<float>(kShadowMapSize);
    shadowViewport.Height   = static_cast<float>(kShadowMapSize);
    shadowViewport.MaxDepth = 1.0f;
    const D3D12_RECT shadowScissor{0, 0, static_cast<LONG>(kShadowMapSize),
                                   static_cast<LONG>(kShadowMapSize)};

    commandList->SetGraphicsRootSignature(shadowRootSignature.Get());
    commandList->SetGraphicsRootConstantBufferView(
        0, sceneUboAllocations[frameIndex]->GetResource()->GetGPUVirtualAddress());
    commandList->RSSetViewports(1, &shadowViewport);
    commandList->RSSetScissorRects(1, &shadowScissor);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (UINT cascade = 0; cascade < kShadowCascades; ++cascade) {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
        dsv.ptr += static_cast<SIZE_T>(cascade) * dsvSize;

        commandList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        commandList->SetGraphicsRoot32BitConstants(1, 1, &cascade, 0);

        // El suelo no se mete en el mapa: es el receptor, y meterlo solo
        // añadiría su propia superficie como oclusor de sí misma.
        commandList->SetPipelineState(shadowPipeline.Get());
        commandList->SetGraphicsRootShaderResourceView(
            2, instanceAllocation->GetResource()->GetGPUVirtualAddress());
        commandList->IASetVertexBuffers(0, 1, &meshVertexBufferView);
        commandList->IASetIndexBuffer(&meshIndexBufferView);
        commandList->DrawIndexedInstanced(meshIndexCount, 1, 0, 0, 0);

        if (hasSkinnedMesh) {
            commandList->SetPipelineState(shadowSkinnedPipeline.Get());
            commandList->SetGraphicsRootShaderResourceView(
                2, characterInstanceAllocation->GetResource()->GetGPUVirtualAddress());
            commandList->IASetVertexBuffers(0, 1, &skinnedVertexBufferView);
            commandList->IASetIndexBuffer(&skinnedIndexBufferView);
            commandList->DrawIndexedInstanced(skinnedIndexCount, 1, 0, 0, 0);
        }
    }

    D3D12_RESOURCE_BARRIER toShaderResource = toDepthWrite;
    toShaderResource.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    toShaderResource.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commandList->ResourceBarrier(1, &toShaderResource);
}

void D3D12Renderer::Impl::updateViewProj()
{
    const float aspect = (height > 0) ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;

    // Perspective_RH_ZO, no perspective a secas: D3D12 clipea en z=[0,1] igual
    // que Vulkan, y con la convención de OpenGL se pierde la mitad cercana.
    // MISMA cámara que updateSceneUbo: la rejilla y la malla tienen que
    // compartir encuadre o el suelo no cae bajo el cubo.
    const glm::mat4 projection =
        glm::perspectiveRH_ZO(glm::radians(60.0f), aspect, 0.1f, 500.0f);
    const glm::mat4 view = glm::lookAtRH(glm::vec3(6.0f, 4.5f, 8.0f),
                                         glm::vec3(0.0f, 0.5f, 0.0f),
                                         glm::vec3(0.0f, 1.0f, 0.0f));
    viewProj = projection * view;
}

D3D12Renderer::D3D12Renderer() : m_impl(std::make_unique<Impl>())
{
    // El Impl consulta el estado a través de este puntero en vez de copiarlo:
    // así un setBloomIntensity() desde el editor se ve en el frame siguiente.
    m_impl->state = this;

    // La escena de prueba de este backend se enseña con los efectos puestos.
    // RendererState los trae apagados —es el default del motor, pensado para
    // que un proyecto nuevo arranque barato—, así que se encienden aquí y no
    // se cambia ese default por debajo al Renderer de Vulkan.
    setBloomIntensity(0.15f);
    setFogEnabled(true);
}

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
    // Los de la swapchain, más el target HDR de la escena y el LDR intermedio
    // que la composición deja para FXAA.
    rtvHeapDesc.NumDescriptors = kFrameCount + 2;
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

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    throwIfFailed(d.device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&d.dsvHeap)),
                  "ID3D12Device::CreateDescriptorHeap(DSV)");
    d.createDepthBuffer();

    d.createGizmoPipeline();
    d.createGridGeometry();
    d.createMeshPipeline();
    d.createMeshResources();
    d.createSkinningPipelines();
    d.createSkinnedResources();
    // Las sombras van al final: su SRV pisa el array de relleno que dejó
    // createMeshResources en el hueco t3, y necesita el buffer de instancias
    // del cubo ya creado.
    d.createShadowResources();
    d.computeCascades();
    // El target HDR y los niveles del bloom necesitan el heap de descriptores
    // ya creado por createMeshResources.
    d.createHdrTargets();
    d.createBloomPipelines();
    d.createFogAndFxaaPipelines();
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

    // Los tres compute van ANTES de abrir el render target: escriben el vertex
    // buffer que el pase gráfico va a leer este mismo frame.
    if (d.hasSkinnedMesh) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsed = d.tickFrequency.QuadPart > 0
                                   ? static_cast<double>(now.QuadPart - d.lastTick.QuadPart) /
                                         static_cast<double>(d.tickFrequency.QuadPart)
                                   : 0.0;
        d.lastTick = now;
        d.animTime += static_cast<float>(elapsed);
        if (d.animDuration > 0.0f && d.animTime > d.animDuration)
            d.animTime = std::fmod(d.animTime, d.animDuration);

        d.recordSkinning();
    }

    // El UBO se escribe una vez por frame y lo leen los dos pases: el de
    // sombras necesita lightSpaceMatrix, el principal todo lo demás.
    d.updateSceneUbo();

    // Sombras antes del pase principal: triangle.frag muestrea el mapa que se
    // graba aquí.
    if (d.shadowPipeline)
        d.recordShadowPasses();

    D3D12_RESOURCE_BARRIER toRenderTarget{};
    toRenderTarget.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRenderTarget.Transition.pResource   = d.renderTargets[d.frameIndex].Get();
    toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRenderTarget.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    d.commandList->ResourceBarrier(1, &toRenderTarget);

    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    backBufferRtv.ptr += static_cast<SIZE_T>(d.frameIndex) * d.rtvSize;

    // La escena NO se dibuja en el backbuffer: va al target HDR, que es el
    // único sitio donde el umbral del bloom puede distinguir lo que pasa de
    // 1.0. El backbuffer lo escribe después el pase de composición.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(kFrameCount) * d.rtvSize;
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = d.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    d.commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    d.commandList->ClearRenderTargetView(rtv, d.clearColor, 0, nullptr);
    d.commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // Viewport y scissor se ponen cada frame: tras un resize el estado del
    // command list se reinicia y arrastrar el tamaño viejo recortaría la imagen.
    D3D12_VIEWPORT viewport{};
    viewport.Width    = static_cast<float>(d.width);
    viewport.Height   = static_cast<float>(d.height);
    viewport.MaxDepth = 1.0f;
    d.commandList->RSSetViewports(1, &viewport);

    D3D12_RECT scissor{0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height)};
    d.commandList->RSSetScissorRects(1, &scissor);

    // La malla primero: escribe profundidad y así la rejilla que va detrás
    // queda tapada donde toca.
    if (d.meshPipeline && d.meshIndexCount > 0) {
        ID3D12DescriptorHeap* heaps[] = {d.srvHeap.Get()};
        d.commandList->SetDescriptorHeaps(1, heaps);

        d.commandList->SetPipelineState(d.meshPipeline.Get());
        d.commandList->SetGraphicsRootSignature(d.meshRootSignature.Get());
        d.commandList->SetGraphicsRootConstantBufferView(
            0, d.sceneUboAllocations[d.frameIndex]->GetResource()->GetGPUVirtualAddress());

        PushData push{};
        push.transform = glm::mat4(1.0f);
        push.metallic  = 0.0f;
        push.roughness = 0.6f;
        // flags.x = 1: el model sale del buffer de instancias, que es la ruta
        // que usa el motor para la geometría estática.
        push.flags = glm::vec2(1.0f, 0.0f);
        d.commandList->SetGraphicsRoot32BitConstants(1, sizeof(PushData) / 4, &push, 0);

        d.commandList->SetGraphicsRootDescriptorTable(
            2, d.srvHeap->GetGPUDescriptorHandleForHeapStart());
        d.commandList->SetGraphicsRootShaderResourceView(
            3, d.instanceAllocation->GetResource()->GetGPUVirtualAddress());

        d.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d.commandList->IASetVertexBuffers(0, 1, &d.meshVertexBufferView);
        d.commandList->IASetIndexBuffer(&d.meshIndexBufferView);
        d.commandList->DrawIndexedInstanced(d.meshIndexCount, 1, 0, 0, 0);

        // Suelo: el receptor de las sombras. Mismo pipeline, otra malla y otra
        // matriz de instancia.
        if (d.groundIndexCount > 0) {
            d.commandList->SetGraphicsRootShaderResourceView(
                3, d.groundInstanceAllocation->GetResource()->GetGPUVirtualAddress());
            d.commandList->IASetVertexBuffers(0, 1, &d.groundVertexBufferView);
            d.commandList->IASetIndexBuffer(&d.groundIndexBufferView);
            d.commandList->DrawIndexedInstanced(d.groundIndexCount, 1, 0, 0, 0);
        }
    }

    // Personaje: mismos shaders y misma root signature que el cubo, pero el
    // vertex buffer es lo que acaba de escribir el compute.
    if (d.hasSkinnedMesh && d.skinnedMeshPipeline) {
        d.commandList->SetPipelineState(d.skinnedMeshPipeline.Get());
        d.commandList->SetGraphicsRootSignature(d.meshRootSignature.Get());
        d.commandList->SetGraphicsRootConstantBufferView(
            0, d.sceneUboAllocations[d.frameIndex]->GetResource()->GetGPUVirtualAddress());

        PushData push{};
        // Los FBX del personaje vienen en centímetros; sin reescalar mediría
        // cien veces la rejilla. flags.x = 0: el model sale de aquí, no del
        // buffer de instancias, que es la ruta que usa el motor para skinned.
        push.transform = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(-3.0f, 0.0f, 0.0f)),
                                    glm::vec3(0.02f));
        push.metallic  = 0.0f;
        push.roughness = 0.7f;
        push.flags     = glm::vec2(0.0f, 0.0f);
        d.commandList->SetGraphicsRoot32BitConstants(1, sizeof(PushData) / 4, &push, 0);

        d.commandList->SetGraphicsRootDescriptorTable(
            2, d.srvHeap->GetGPUDescriptorHandleForHeapStart());
        d.commandList->SetGraphicsRootShaderResourceView(
            3, d.instanceAllocation->GetResource()->GetGPUVirtualAddress());

        d.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d.commandList->IASetVertexBuffers(0, 1, &d.skinnedVertexBufferView);
        d.commandList->IASetIndexBuffer(&d.skinnedIndexBufferView);
        d.commandList->DrawIndexedInstanced(d.skinnedIndexCount, 1, 0, 0, 0);
    }

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

    // El buffer de vértices deformados vuelve a acceso desordenado: el frame
    // siguiente lo reescribe el compute, y tiene que encontrarlo como lo dejó
    // el anterior o la transición de ida partiría de un estado que no es.
    if (d.hasSkinnedMesh) {
        D3D12_RESOURCE_BARRIER backToUav{};
        backToUav.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        backToUav.Transition.pResource   = d.outputVertsAllocation->GetResource();
        backToUav.Transition.StateBefore = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        backToUav.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        backToUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        d.commandList->ResourceBarrier(1, &backToUav);
    }

    // Niebla ANTES del bloom: reescribe la escena, y lo que el bloom filtre
    // tiene que ser ya lo que se va a ver.
    // Ahora que el interruptor vive en el estado compartido, se respeta: es el
    // mismo que apaga la niebla en el menú View del editor.
    if (d.fogPipeline && d.state->fogEnabled())
        d.recordFog();

    // Bloom, composición con tone mapping y FXAA hasta el backbuffer.
    if (d.compositePipeline)
        d.recordBloomAndComposite(backBufferRtv);

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

    // El buffer de profundidad tiene el tamaño de la ventana: si no se recrea,
    // el test se hace contra una superficie de otro tamaño.
    createDepthBuffer();

    // Y el target HDR con los niveles del bloom, por lo mismo. Solo si ya
    // existían: en el primer arranque los crea init() después del resize.
    if (hdrAllocation)
        createHdrTargets();

    // El aspecto de la proyección depende del tamaño: sin esto la rejilla se
    // deforma al estirar la ventana.
    updateViewProj();

    // Y las cascadas se reparten sobre el frustum de la cámara, que acaba de
    // cambiar de forma: sin recalcularlas, los volúmenes de sombra siguen
    // ajustados al aspecto anterior.
    computeCascades();
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

void D3D12Renderer::waitIdle()
{
    if (m_impl->initialized)
        m_impl->waitForGpu();
}

void D3D12Renderer::setUiDrawCallback(std::function<void()> callback)
{
    m_impl->uiDrawCallback = std::move(callback);
}

void* D3D12Renderer::nativeDevice() const
{
    return m_impl->device.Get();
}

void* D3D12Renderer::nativeCommandList() const
{
    return m_impl->commandList.Get();
}

void* D3D12Renderer::nativeQueue() const
{
    return m_impl->queue.Get();
}

void* D3D12Renderer::uiDescriptorHeap() const
{
    return m_impl->srvHeap.Get();
}

uint64_t D3D12Renderer::uiHeapStartCpu() const
{
    if (!m_impl->srvHeap)
        return 0;
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_impl->srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(kSrvImGui) * m_impl->srvSize;
    return handle.ptr;
}

uint64_t D3D12Renderer::uiHeapStartGpu() const
{
    if (!m_impl->srvHeap)
        return 0;
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_impl->srvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(kSrvImGui) * m_impl->srvSize;
    return handle.ptr;
}

unsigned D3D12Renderer::uiDescriptorCount() const
{
    return kImGuiReserved;
}

unsigned D3D12Renderer::descriptorSize() const
{
    return m_impl->srvSize;
}

int D3D12Renderer::framesInFlight() const
{
    return static_cast<int>(kFrameCount);
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

    // Los UBO están mapeados de forma persistente: se desmapean antes de
    // soltar su allocation.
    for (UINT i = 0; i < kFrameCount; ++i) {
        if (d.sceneUboAllocations[i]) {
            if (d.sceneUboMapped[i]) {
                d.sceneUboAllocations[i]->GetResource()->Unmap(0, nullptr);
                d.sceneUboMapped[i] = nullptr;
            }
            d.sceneUboAllocations[i]->Release();
            d.sceneUboAllocations[i] = nullptr;
        }
    }

    for (auto** allocation : {&d.meshVertexAllocation, &d.meshIndexAllocation,
                              &d.instanceAllocation, &d.baseColorAllocation,
                              &d.normalMapAllocation, &d.shadowMapAllocation,
                              &d.depthAllocation, &d.posKeysAllocation, &d.rotKeysAllocation,
                              &d.scaleKeysAllocation, &d.boneInfosAllocation,
                              &d.inputVertsAllocation, &d.localXformsAllocation,
                              &d.finalBonesAllocation, &d.outputVertsAllocation,
                              &d.skinnedIndexAllocation, &d.groundVertexAllocation,
                              &d.groundIndexAllocation, &d.groundInstanceAllocation,
                              &d.characterInstanceAllocation, &d.shadowMapArrayAllocation}) {
        if (*allocation) {
            (*allocation)->Release();
            *allocation = nullptr;
        }
    }
    d.meshVertexBufferView = {};
    d.meshIndexBufferView  = {};
    d.meshIndexCount       = 0;

    d.skinnedVertexBufferView = {};
    d.skinnedIndexBufferView  = {};
    d.skinnedIndexCount       = 0;
    d.hasSkinnedMesh          = false;

    d.groundVertexBufferView = {};
    d.groundIndexBufferView  = {};
    d.groundIndexCount       = 0;

    d.releaseHdrTargets();
    d.fxaaPipeline.Reset();
    d.fxaaRootSignature.Reset();
    d.fogPipeline.Reset();
    d.fogRootSignature.Reset();
    d.compositePipeline.Reset();
    d.compositeRootSignature.Reset();
    d.bloomUpPipeline.Reset();
    d.bloomDownPipeline.Reset();
    d.bloomRootSignature.Reset();

    d.shadowSkinnedPipeline.Reset();
    d.shadowPipeline.Reset();
    d.shadowRootSignature.Reset();
    d.shadowDsvHeap.Reset();

    d.skinnedMeshPipeline.Reset();
    d.skinningPipeline.Reset();
    d.boneHierarchyPipeline.Reset();
    d.boneEvalPipeline.Reset();
    d.skinningRootSignature.Reset();
    d.boneHierarchyRootSignature.Reset();
    d.boneEvalRootSignature.Reset();

    d.srvHeap.Reset();
    d.dsvHeap.Reset();
    d.meshPipeline.Reset();
    d.meshRootSignature.Reset();
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
