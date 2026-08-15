#include "DonTopo/Renderer/D3D12/D3D12Renderer.h"

#ifdef DT_D3D12_ENABLED

#include "DonTopo/Core/Camera.h"
#include "DonTopo/Core/CameraComponent.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/ReflectionProbeComponent.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/Window.h"
#include "DonTopo/Renderer/Cube.h"
#include "DonTopo/Renderer/Frustum.h"
#include "DonTopo/Renderer/InstanceBatching.h"
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/MeshKey.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/Plane.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Renderer/SkinnedMeshPacking.h"
#include "DonTopo/Renderer/UiLayer.h"
#include "DonTopo/Renderer/Vertex.h"
#include "DonTopo/UI/UiCanvas.h"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <D3D12MemAlloc.h>
#include <stb_image.h>

#include <glm/glm.hpp>

#include <chrono>
#include <optional>
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
#include <unordered_map>
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
    // En el hueco de padding que ya había detrás de numLights, igual que en
    // GLSL: ningún offset anterior se mueve.
    float       ambientIntensity;
};

static_assert(offsetof(SceneUbo, view) == 0, "UBO: view debe ir en c0");
static_assert(offsetof(SceneUbo, proj) == 64, "UBO: proj debe ir en c4");
static_assert(offsetof(SceneUbo, lightSpaceMatrix) == 128, "UBO: lightSpaceMatrix debe ir en c8");
static_assert(offsetof(SceneUbo, cascadeSplits) == 384, "UBO: cascadeSplits debe ir en c24");
static_assert(offsetof(SceneUbo, lights) == 400, "UBO: lights debe ir en c25");
static_assert(offsetof(SceneUbo, viewPos) == 1424, "UBO: viewPos debe ir en c89");
static_assert(offsetof(SceneUbo, numLights) == 1440, "UBO: numLights debe ir en c90");
static_assert(offsetof(SceneUbo, ambientIntensity) == 1444,
              "UBO: ambientIntensity va pegado a numLights");

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

// Medio flotante a mano: los neutros del IBL son cuatro texels y no compensa
// arrastrar DirectXMath por ellos. Vale para valores normales y pequeños, que
// es lo único que se le pasa.
inline uint16_t floatToHalf(float value)
{
    const bool  negative = value < 0.0f;
    float       magnitude = negative ? -value : value;
    if (!(magnitude > 0.0f))
        return negative ? 0x8000u : 0u;

    int exponent = 0;
    while (magnitude >= 2.0f && exponent < 15) {
        magnitude *= 0.5f;
        ++exponent;
    }
    while (magnitude < 1.0f && exponent > -14) {
        magnitude *= 2.0f;
        --exponent;
    }

    const uint16_t biased  = static_cast<uint16_t>(exponent + 15);
    const uint16_t mantissa =
        static_cast<uint16_t>((magnitude - 1.0f) * 1024.0f + 0.5f) & 0x03FFu;
    return static_cast<uint16_t>((negative ? 0x8000u : 0u) | (biased << 10) | mantissa);
}

// Secuencia de Halton en base b: la sucesión de baja discrepancia con la que el
// TAA reparte las muestras dentro del píxel. Cubre el área mucho más
// uniformemente que un aleatorio, que es lo que hace que el promedio temporal
// converja a un supersampling de verdad.
inline float halton(uint32_t index, uint32_t base)
{
    float result = 0.0f;
    float f      = 1.0f;
    while (index > 0) {
        f      /= static_cast<float>(base);
        result += f * static_cast<float>(index % base);
        index  /= base;
    }
    return result;
}

// IBL. Mismos tamaños que el camino de Vulkan (Renderer.h): el prefiltrado
// reparte la rugosidad entre sus mips, y pbr.frag lo da por hecho con un
// #define propio.
constexpr UINT kIblIrradianceSize = 32;
constexpr UINT kIblPrefilterSize  = 128;
constexpr UINT kIblPrefilterMips  = 5;

// Lado de cada cara al capturar una sonda. 128 es lo que usa el camino de
// Vulkan: entra de sobra en el prefiltrado y seis caras a más resolución no se
// notan en un reflejo, que ya va emborronado por la rugosidad.
constexpr UINT kProbeFaceSize = 128;

// Forward+. Mismos valores que Renderer.h: la rejilla, el tope por celda y el
// de luces los dan por hecho los dos compute de culling y pbr.frag.
constexpr uint32_t kFpMaxLights     = 256;
constexpr uint32_t kFpMaxPerCell    = 64;
constexpr uint32_t kFpTileSize      = 16;  // tiled
constexpr uint32_t kFpClusterTile   = 64;  // clustered, XY
constexpr uint32_t kFpClusterSlices = 24;  // clustered, Z

// Una luz tal y como la quiere el culling: la misma que en el UBO más el radio
// y su posición en view space, que es lo que evita recalcularla por celda.
struct FpLightGpu {
    glm::vec4 posRadius;
    glm::vec4 color;
    glm::vec4 viewPosR;
    glm::vec4 direction;
    glm::vec4 params;
};

// Bloque de parámetros que leen el culling y pbr.frag.
struct FpParamsGpu {
    uint32_t mode, gridX, gridY, gridZ;
    uint32_t tileSize, maxPerCell, numLights, pad0;
    float    zNear, zFar, sliceScale, sliceBias;
};

// Push del culling: los cuatro coeficientes de la proyección y el tamaño de
// pantalla.
struct FpPush {
    float    p00, p11, p22, p32;
    uint32_t screenW, screenH, pad0, pad1;
};

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

// Destino de la composición, ya con el tone mapping aplicado. De aquí lee el
// pase final (FXAA/TAA/SSAA) para escribir el backbuffer.
constexpr DXGI_FORMAT kLdrFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

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
// t4..t7 del bloque global. Son los neutros: metallic-roughness a blanco
// (ao = 1 y los factores del push sin escalar), los dos cubemaps con un
// ambiente plano y la oclusión a 1. pbr.frag los muestrea SIEMPRE, así que
// tienen que existir aunque no haya ni material ni entorno ni SSAO.
constexpr UINT kSrvMetalRough = 3;
constexpr UINT kSrvIrradiance = 4;
constexpr UINT kSrvPrefilter  = 5;
constexpr UINT kSrvSsao       = 6;
constexpr UINT kSrvSceneHdr   = 7;
constexpr UINT kSrvBloomMip  = kSrvSceneHdr + 1;            // + nivel
constexpr UINT kUavBloomMip  = kSrvBloomMip + kBloomMips;   // + nivel
constexpr UINT kSrvDepth     = kUavBloomMip + kBloomMips;   // profundidad, para la niebla
constexpr UINT kUavSceneHdr  = kSrvDepth + 1;               // la niebla escribe sobre la escena
constexpr UINT kSrvLdr       = kUavSceneHdr + 1;            // salida de la composición, para FXAA
// Rango reservado para ImGui. No basta con uno: desde la 1.92 su backend de
// DX12 pide descriptores por su cuenta (uno por textura, no solo la fuente) a
// través de los callbacks de reserva que se le pasan al inicializarlo.
constexpr UINT kSrvImGui      = kSrvLdr + 1;
constexpr UINT kImGuiReserved = 16;

// Bloque de descriptores por objeto. pbr.frag pide t1..t7 como UNA tabla
// contigua, así que cada malla necesita sus siete huecos seguidos, en este
// orden: color base, normales, sombras, metallic-roughness, irradiancia,
// prefiltrado y oclusión de pantalla. Los cuatro últimos y el de sombras son
// recursos compartidos: se les crea la vista otra vez dentro de cada bloque,
// que es legal y evita partir la root signature (copiar descriptores desde un
// heap visible al shader no lo permite la API).
//
// Los objetos que pasen del tope se dibujan con el bloque global, que lleva
// los neutros: se ven planos, pero nunca se sale del heap.
constexpr UINT kSrvPerObject   = 7;
constexpr UINT kMaxObjectSlots = 512;
constexpr UINT kSrvObjects     = kSrvImGui + kImGuiReserved;

// Y otro tanto para la malla skinned, que se dibuja por submallas: cada una
// tiene su material en el FBX y por tanto su propio bloque.
constexpr UINT kMaxSkinnedSlots = 16;
constexpr UINT kSrvSkinned      = kSrvObjects + kMaxObjectSlots * kSrvPerObject;

// Cubemap del cielo: una sola vista, la del TextureCube que muestrea t0 de
// skybox.frag.
constexpr UINT kSrvSkybox   = kSrvSkinned + kMaxSkinnedSlots * kSrvPerObject;

// Destinos de los dos compute de IBL: la irradiancia entera y un nivel del
// prefiltrado por mip. Son de escritura, así que van como UAV y no comparten
// hueco con las vistas de lectura que usa pbr.frag.
constexpr UINT kUavIrradiance = kSrvSkybox + 1;
constexpr UINT kUavPrefilter  = kUavIrradiance + 1;  // + mip

// SSAO: la profundidad del pre-pase, el mapa crudo y el emborronado. El
// resultado final lo leen los bloques por objeto en su hueco t7; estos son los
// de la cadena que lo produce.
constexpr UINT kSrvPrepassDepth = kUavPrefilter + kIblPrefilterMips;
constexpr UINT kUavSsaoRaw      = kSrvPrepassDepth + 1;
constexpr UINT kSrvSsaoRaw      = kUavSsaoRaw + 1;
constexpr UINT kUavSsaoBlur     = kSrvSsaoRaw + 1;

// Reflejos en pantalla: el destino del trazado, que luego el resolve suma
// sobre la escena.
constexpr UINT kUavSsr = kUavSsaoBlur + 1;
constexpr UINT kSrvSsr = kUavSsr + 1;

// Motion blur: destino del emborronado. El shader lee píxeles arbitrarios de la
// escena a lo largo de la velocidad, así que no puede escribir sobre la imagen
// que muestrea; de aquí sale la copia de vuelta. Solo UAV: la copia no necesita
// vista.
constexpr UINT kUavMotionBlur = kSrvSsr + 1;

// Historial del TAA: dos imágenes que se alternan, porque el mismo pase lee la
// del frame anterior y escribe la de este.
constexpr UINT kSrvTaaHistory = kUavMotionBlur + 1;  // + índice (0 o 1)

// Imagen del viewport: la escena ya compuesta, cuando en vez de ir al
// backbuffer tiene que acabar dentro de un panel de la interfaz.
constexpr UINT kSrvViewport = kSrvTaaHistory + 2;

// Atlas de la UI 2D: uno por sprite-sheet y uno por fuente. El tope es de
// verdad —pasado él la UI se dibuja sin su textura, no se sale del heap— y con
// 16 sobra para una interfaz de juego con varias fuentes.
constexpr UINT kSrvUiAtlas    = kSrvViewport + 1;
constexpr UINT kMaxUiAtlases  = 16;

// ─── Sondas de reflexión ─────────────────────────────────────────────────────
// Cada sonda tiene lo mismo que el IBL global —irradiancia y entorno
// prefiltrado— más el cubemap donde se captura la escena antes de
// convolucionarla. Ocho por escena: cada una ocupa ~1 MB entre las tres
// imágenes, y pasado el tope los objetos se quedan con el IBL global, que es
// degradarse, no fallar.
constexpr UINT kMaxProbes = 8;

// Huecos por sonda, en este orden: captura (SRV), irradiancia (SRV+UAV) y
// prefiltrado (SRV + un UAV por mip, que cada nivel es una rugosidad distinta y
// se dispara por separado).
constexpr UINT kSrvPerProbe = 1                     // captura
                            + 1 + 1                 // irradiancia: lectura y escritura
                            + 1 + kIblPrefilterMips;// prefiltrado: lectura y un UAV por mip
constexpr UINT kSrvProbes   = kSrvUiAtlas + kMaxUiAtlases;

constexpr UINT kSrvHeapSize = kSrvProbes + kMaxProbes * kSrvPerProbe;

// Push de ssao.comp y ssao_blur.comp: los dos comparten el bloque, así que el
// rango de root constants tiene que ser el mismo para los dos pipelines.
struct SsaoPush {
    glm::vec4 projParams;  // p00, p11, p22, p32 de la proyección del frame
    glm::vec2 invRes;
    float     radius;
    float     bias;
    float     intensity;
    float     power;
};
static_assert(sizeof(SsaoPush) == 40, "SsaoPush debe ocupar 40 bytes");

// Push de ssr.comp y ssr_resolve.comp, que comparten bloque igual que los dos
// del SSAO.
struct SsrPush {
    glm::vec4 projParams;
    glm::vec2 invRes;
    float     maxDistance;
    float     thickness;
    int32_t   maxSteps;
    int32_t   refineSteps;  // búsqueda binaria sobre el último tramo
    float     edgeFade;
    float     intensity;
};
static_assert(sizeof(SsrPush) == 48, "SsrPush debe ocupar 48 bytes");

// Push de taa.frag: la reproyección al frame anterior y el peso del historial.
struct TaaPush {
    glm::mat4 reproject;
    glm::vec2 invRes;
    float     feedback;
    int32_t   historyValid;
};
static_assert(sizeof(TaaPush) == 80, "TaaPush debe ocupar 80 bytes");

// Push de motion_blur.comp: la misma reproyección que el TAA más los tres
// ajustes del efecto.
struct MotionBlurPush {
    glm::mat4 reproject;
    glm::vec2 invRes;
    float     intensity;
    float     maxRadius;  // tope de la estela, en píxeles
    int32_t   samples;
};
static_assert(sizeof(MotionBlurPush) == 84, "MotionBlurPush debe ocupar 84 bytes");

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

// Push de ssaa_resolve.frag: el inverso del tamaño de la imagen GRANDE (la
// fuente) y cuántas muestras por eje hay que promediar.
struct SsaaPush {
    float invSrc[2];
    int   taps;
};
static_assert(sizeof(SsaaPush) == 12, "SsaaPush debe ocupar 12 bytes");

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
    // Los mismos shaders y la misma root signature, pero rellenando solo las
    // aristas. Se crean junto a los sólidos para no rehacer nada al cambiar de
    // modo: el interruptor solo elige cuál se enlaza.
    ComPtr<ID3D12PipelineState> meshWirePipeline;

    // Casco invertido del objeto seleccionado: la misma malla extruida por su
    // normal y con las caras frontales descartadas, así que solo asoma el
    // reborde. Comparte root signature con la malla —outline.vert declara el
    // mismo UBO y el mismo push—, y por eso no necesita nada propio.
    // Líneas de depuración del frame: colliders, ejes, rayos. Se envían desde
    // fuera antes de dibujar y NO persisten al frame siguiente, igual que en el
    // camino de Vulkan.
    D3D12MA::Allocation*     debugLinesAllocation = nullptr;
    void*                    debugLinesMapped     = nullptr;
    size_t                   debugLinesCapacity   = 0;   // en vértices
    UINT                     debugLineVertices    = 0;   // los de ESTE frame
    D3D12_VERTEX_BUFFER_VIEW debugLinesView{};

    void ensureDebugLineBuffer(size_t vertexCount);

    ComPtr<ID3D12PipelineState> outlinePipeline;
    ComPtr<ID3D12PipelineState> outlineSkinnedPipeline;
    // Los mismos contra el target LDR, que es donde se dibuja el contorno: va
    // DESPUÉS del tone mapping para que su naranja no pase por ACES. Los de
    // arriba se quedan porque son los que fija el formato HDR y las muestras
    // del pase de escena, de donde estos heredan todo lo demás.
    ComPtr<ID3D12PipelineState> outlineLdrPipeline;
    ComPtr<ID3D12PipelineState> outlineSkinnedLdrPipeline;

    // Dibuja el casco invertido de lo seleccionado sobre el target LDR, con la
    // profundidad de la escena para que solo asome por donde de verdad se ve.
    void recordSelectionOutline(D3D12_CPU_DESCRIPTOR_HANDLE rtv);
    // Si hay algo seleccionado que dibujar. Lo consulta el pre-pase: con MSAA,
    // la profundidad del pase de escena es multimuestra y el contorno necesita
    // una de una muestra, que es justo la que produce ese pre-pase.
    bool hasOutlineSelection() const;
    int   selectedObject  = -1;   // índice en objects, -1 sin selección
    int   selectedSkinned = -1;   // índice en skinnedObjects
    // En unidades de mundo: con mallas de detalle fino un casco grueso asoma
    // también por las rendijas interiores y el contorno deja de leerse como
    // silueta. 1 cm va bien para personajes de escala humana.
    float outlineWidth    = 0.01f;

    // Geometría estática de la escena. Cada entrada es una malla subida a VRAM
    // con su transformación; el índice que devuelve addStaticMesh es la
    // posición en este vector, igual que en el Renderer de Vulkan.
    struct StaticObject {
        D3D12MA::Allocation*     vertexAllocation = nullptr;
        D3D12MA::Allocation*     indexAllocation  = nullptr;
        D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
        D3D12_INDEX_BUFFER_VIEW  indexBufferView{};
        UINT                     indexCount = 0;
        glm::mat4                transform{1.0f};
        bool                     meshVisible = true;

        // Texturas propias del material, o nullptr si la malla no trae (o el
        // fichero no se pudo leer): en ese caso la terna apunta a las 1x1
        // globales y el objeto sale con color plano, como hasta ahora.
        D3D12MA::Allocation* baseColorAllocation  = nullptr;
        D3D12MA::Allocation* normalMapAllocation  = nullptr;
        D3D12MA::Allocation* metalRoughAllocation = nullptr;

        // Primer hueco de su terna en el heap. kSrvBaseColor = la global.
        UINT  srvBase   = kSrvBaseColor;
        float metallic  = 0.0f;
        float roughness = 0.6f;

        // Fuerza de reflejo del objeto. pbr.frag la vuelca al alfa de la
        // escena, y de ahí la lee el trazado: a cero, ese píxel no refleja.
        float ssrStrength = 0.0f;

        // Caja envolvente en espacio LOCAL, para el frustum culling. Se calcula
        // al subir la malla porque no depende de dónde esté el objeto. Una
        // malla sin vértices no se puede acotar: hasBounds = false y entonces se
        // dibuja siempre, que es el fallo seguro.
        glm::vec3 aabbMin{0.0f};
        glm::vec3 aabbMax{0.0f};
        bool      hasBounds = false;

        // ── Malla compartida ────────────────────────────────────────────────
        // Índice del objeto que SUBIÓ estos buffers y estas texturas. Cien
        // cubos iguales apuntan todos al primero: los vertexBufferView de
        // arriba son copias del suyo, no recursos propios. Sin esto, agrupar
        // "por misma malla" agruparía cero objetos, porque cada uno tendría su
        // propia copia en VRAM.
        int  sharedMesh = -1;
        // Solo el dueño libera. Un duplicado que soltara los buffers dejaría a
        // los demás dibujando con memoria liberada, y eso no lo avisa nadie.
        bool ownsGpu = true;
        // Cuántos objetos vivos apuntan a ESTA malla. Solo lo lleva el dueño
        // (los duplicados se quedan a 0). Hoy nadie borra objetos de uno en uno
        // —removeGameObject los apaga, ver setObjectMeshVisible— así que el
        // único que libera es clearStaticMeshes y se lo lleva todo; el contador
        // está para que el día que aparezca un borrado suelto NO pueda soltar
        // los buffers con duplicados todavía dibujándolos. La guarda vive en
        // releaseStaticObject, que es quien borra, y no en una lista de sitios
        // desde los que está permitido llamar.
        int  sharedRefs = 0;

        // Grupo de dibujo: misma malla Y bloque de descriptores equivalente.
        // Los objetos de un grupo se pintan de un solo draw instanciado. -1
        // mientras no se hayan reconstruido los grupos.
        int  drawGroup = -1;

        // Sube cada vez que a este objeto se le cambia una textura por el
        // neutro de "no se pudo leer". Entra en la clave del grupo: dos objetos
        // con la misma malla pero distinto relleno ya no ven lo mismo, así que
        // no pueden compartir el draw.
        uint32_t materialVariant = 0;
    };
    std::vector<StaticObject> objects;

    // Clave de contenido -> objeto que subió esa malla. Es lo único que hace
    // falta para deduplicar: aquí los objetos no se borran de uno en uno (borrar
    // uno solo lo apaga, ver removeGameObject), así que no hay refcount que
    // llevar; clearStaticMeshes se lo lleva todo por delante.
    std::unordered_map<std::string, int> sharedMeshOwner;

    // Un representante por grupo de dibujo: de él salen los buffers, la terna de
    // descriptores y los factores PBR, que son iguales para todo el grupo.
    std::vector<int> drawGroupRep;
    bool             drawGroupsDirty = true;
    void             rebuildDrawGroups();

    // El ÚNICO sitio que suelta los recursos de un objeto estático. La decisión
    // de si se puede o no vive aquí dentro, y no en una lista de llamantes
    // autorizados: un duplicado nunca posee nada, y el dueño solo suelta cuando
    // no queda nadie más apuntando a su malla. Devuelve si liberó.
    bool releaseStaticObject(StaticObject& object);

    // Matrices por instancia de la escena. shadow.vert saca el model de aquí
    // SIEMPRE —no tiene ruta de push constant— y triangle.vert cuando el draw
    // va instanciado. Va en heap de subida y mapeado: se reescribe cada frame
    // porque los transforms cambian.
    //
    // UNO POR FRAME EN VUELO, y no uno solo: desde que el reparto lo decide el
    // agrupado, el CONTENIDO cambia de frame a frame (el culling mueve a los
    // objetos de tramo). Reescribir un único buffer mientras la GPU lee el
    // frame anterior mezclaría los dos repartos, y el síntoma sería geometría
    // apareciendo en el sitio de otra.
    std::array<D3D12MA::Allocation*, kFrameCount> sceneInstanceAllocations{};
    std::array<void*, kFrameCount>                sceneInstanceMapped{};
    std::array<size_t, kFrameCount>               sceneInstanceCapacity{};

    // El buffer va en dos tramos de este tamaño: el 0 para sombras y pre-pase
    // (todo lo visible) y el 1 para el pase principal (lo que además pasa el
    // frustum). Dos repartos distintos del mismo conjunto de objetos.
    size_t instanceRegionStride = 0;
    static constexpr uint32_t kInstanceRegionShadow = 0;
    static constexpr uint32_t kInstanceRegionScene  = 1;

    void ensureSceneInstanceBuffer(size_t count);
    // Dirección GPU de la matriz `index` del buffer de ESTE frame. Los draws
    // instanciados apuntan el root SRV al principio del rango de su grupo y
    // dibujan con StartInstanceLocation = 0, en vez de dejar la vista al
    // principio del buffer y mover la instancia base.
    //
    // El motivo: el shader viene de GLSL, donde gl_InstanceIndex SÍ incluye la
    // instancia base por especificación, y spirv-cross lo traduce a
    // SV_InstanceID, donde eso no está garantizado igual. En la máquina en que
    // se probó funciona de las dos formas —se comprobó comparando el render
    // contra el camino por objeto— pero desplazar la vista cuesta lo mismo y no
    // depende del driver.
    D3D12_GPU_VIRTUAL_ADDRESS instanceAddress(uint32_t index) const;

    // Candidatos y grupos del frame. Son miembros y no locales para no
    // reasignar sus vectores en cada pase de cada frame.
    std::vector<Batching::BatchCandidate> batchCandidates;
    std::vector<Batching::InstanceBatch>  shadowBatches;
    std::vector<Batching::InstanceBatch>  sceneBatches;

    // Llena el tramo 0 con todo lo visible. Lo comparten el pase de sombras
    // (que dibuja las cuatro cascadas del mismo reparto) y el pre-pase de
    // profundidad, que ven el mismo conjunto.
    void buildShadowBatches();

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

    // Neutros de t4..t7: existen siempre, y son lo que ve una malla sin
    // material de metallic-roughness o una escena sin entorno.
    D3D12MA::Allocation* metalRoughAllocation = nullptr;
    D3D12MA::Allocation* irradianceAllocation = nullptr;
    D3D12MA::Allocation* prefilterAllocation  = nullptr;
    // Mips que tiene AHORA el prefiltrado: uno con el neutro, y los de verdad
    // cuando lo genere el compute. Una vista que declare más mips de los que
    // tiene el recurso es un descriptor inválido: no falla al crearla, se lleva
    // el device por delante cuando algo lo usa.
    UINT                 prefilterMips        = 1;
    D3D12MA::Allocation* ssaoAllocation       = nullptr;

    // Forward+ apagado, pero los cuatro buffers de space2 EXISTEN: pbr.frag los
    // declara sin rama, y un root SRV a cero es una lectura fuera de recurso.
    // Con mode = 0 el shader ni los mira, pero tienen que estar enlazados.
    D3D12MA::Allocation* fpParamsAllocation  = nullptr;
    D3D12MA::Allocation* fpLightsAllocation  = nullptr;
    D3D12MA::Allocation* fpCellsAllocation   = nullptr;
    D3D12MA::Allocation* fpIndicesAllocation = nullptr;

    // t1..t7 de space0, en este orden.
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
    ComPtr<ID3D12PipelineState> skinnedMeshWirePipeline;

    // Submallas de un personaje: el FBX trae un material por trozo (cuerpo,
    // pelo, ropa…), y dibujarlo de una tirada obligaba a darles a todas la
    // misma textura. Un draw por rango con su terna.
    struct SkinnedSubMesh {
        UINT indexStart = 0;
        UINT indexCount = 0;
        UINT srvBase    = kSrvBaseColor;
    };

    // Un personaje animado. Cada uno tiene su esqueleto, sus claves y su
    // buffer de vértices deformados: el compute de skinning escribe ahí, y el
    // pase gráfico lo lee como vertex buffer en el mismo frame.
    struct SkinnedObject {
        D3D12MA::Allocation* posKeys     = nullptr;
        D3D12MA::Allocation* rotKeys     = nullptr;
        D3D12MA::Allocation* scaleKeys   = nullptr;
        D3D12MA::Allocation* boneInfos   = nullptr;
        D3D12MA::Allocation* inputVerts  = nullptr;
        D3D12MA::Allocation* localXforms = nullptr;
        D3D12MA::Allocation* finalBones  = nullptr;
        D3D12MA::Allocation* outputVerts = nullptr;
        D3D12MA::Allocation* indices     = nullptr;

        D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
        D3D12_INDEX_BUFFER_VIEW  indexBufferView{};
        UINT                     indexCount  = 0;
        uint32_t                 boneCount   = 0;
        uint32_t                 vertexCount = 0;

        // boneInfos va en layout [clip][hueso]: el offset del clip activo es
        // clip * boneCount.
        uint32_t clipBase     = 0;
        float    animTime     = 0.0f;
        float    animDuration = 0.0f;
        // Quién manda en animTime. En cuanto alguien de fuera lo mueve
        // —updateAnimation o setAnimationState— el backend deja de avanzarlo por
        // su cuenta: los dos relojes sumando dejarían el clip al doble.
        bool     externalClock = false;

        glm::mat4 transform{1.0f};
        bool      visible = true;
        float     ssrStrength = 0.0f;

        std::vector<SkinnedSubMesh>       subMeshes;
        std::vector<D3D12MA::Allocation*> textures;
    };
    std::vector<SkinnedObject> skinnedObjects;

    // Huecos del rango skinned ya repartidos, en ternas. No se reaprovechan al
    // borrar un objeto suelto porque los personajes se cargan de golpe con la
    // escena; clearSkinnedMeshes lo devuelve a cero.
    UINT nextSkinnedSlot = 0;

    // Matrices de los personajes para el pase de sombras, por lo mismo que
    // sceneInstanceAllocation: shadow.vert saca el model del SSBO siempre.
    // Uno por frame en vuelo, por el mismo motivo que el de la escena: se
    // reescribe desde CPU cada frame mientras la GPU puede seguir leyendo el
    // anterior. Aquí el reparto es fijo (una matriz por personaje, en su
    // índice), así que lo peor que daba el buffer único era una sombra con el
    // transform de un frame antes; con un solo buffer eso no se puede ni
    // detectar ni descartar, y cuesta lo mismo hacerlo bien.
    std::array<D3D12MA::Allocation*, kFrameCount> skinnedInstanceAllocations{};
    std::array<void*, kFrameCount>                skinnedInstanceMapped{};
    std::array<size_t, kFrameCount>               skinnedInstanceCapacity{};

    // Dirección de la matriz del personaje `index`. Igual que instanceAddress:
    // se desplaza la vista en vez de mover la instancia base, que en HLSL no
    // está garantizado que llegue a SV_InstanceID.
    D3D12_GPU_VIRTUAL_ADDRESS skinnedInstanceAddress(size_t index) const
    {
        return skinnedInstanceAllocations[frameIndex]->GetResource()->GetGPUVirtualAddress() +
               static_cast<UINT64>(index) * sizeof(glm::mat4);
    }

    // ── Cielo ───────────────────────────────────────────────────────────
    ComPtr<ID3D12RootSignature> skyboxRootSignature;
    ComPtr<ID3D12PipelineState> skyboxPipeline;
    D3D12MA::Allocation*        skyboxAllocation = nullptr;

    // Carga las seis caras y monta el cubemap. Silenciosa si falta alguna: el
    // fondo se queda en el color de limpieza, que es lo que había antes.
    void createSkyboxResources();
    // Solo la root signature y el pipeline: se rehacen al cambiar de muestras,
    // sin volver a cargar las seis caras.
    void createSkyboxPipelineOnly();
    void recordSkybox();

    ComPtr<ID3D12RootSignature> iblRootSignature;
    ComPtr<ID3D12PipelineState> iblIrradiancePipeline;
    ComPtr<ID3D12PipelineState> iblPrefilterPipeline;

    // Convoluciona el cubemap del cielo en los dos mapas que consume pbr.frag:
    // irradiancia para el difuso y prefiltrado por rugosidad para el especular.
    // Sustituye a los neutros; sin cielo cargado no hace nada.
    void precomputeIbl();

    // Entorno plano para cuando no hay cubemap, y los cuatro buffers de
    // Forward+ que pbr.frag declara sin rama.
    void createNeutralIblCubes();
    void createForwardPlusBuffers();

    // Enlaza los cuatro root SRV de space2. Los tres pases que usan la root
    // signature de malla (estáticos, suelo y personajes) los necesitan.
    void bindForwardPlus();

    // ── Oclusión ambiental en pantalla ──────────────────────────────────
    // El pre-pase escribe SU profundidad, no la del pase de escena: los dos
    // compute la leen como textura, y el pase de escena todavía no ha corrido
    // cuando hace falta (pbr.frag consume el resultado).
    ComPtr<ID3D12PipelineState> depthPrepassPipeline;         // vértices del motor
    ComPtr<ID3D12PipelineState> depthPrepassSkinnedPipeline;  // salida del skinning
    ComPtr<ID3D12RootSignature> depthPrepassRootSignature;
    ComPtr<ID3D12DescriptorHeap> prepassDsvHeap;
    D3D12MA::Allocation*         prepassDepthAllocation = nullptr;

    ComPtr<ID3D12RootSignature> ssaoRootSignature;
    ComPtr<ID3D12PipelineState> ssaoPipeline;
    ComPtr<ID3D12PipelineState> ssaoBlurPipeline;
    D3D12MA::Allocation*        ssaoRawAllocation  = nullptr;
    D3D12MA::Allocation*        ssaoBlurAllocation = nullptr;

    ComPtr<ID3D12RootSignature> ssrRootSignature;
    ComPtr<ID3D12PipelineState> ssrPipeline;
    ComPtr<ID3D12PipelineState> ssrResolvePipeline;
    D3D12MA::Allocation*        ssrAllocation = nullptr;

    ComPtr<ID3D12RootSignature> motionBlurRootSignature;
    ComPtr<ID3D12PipelineState> motionBlurPipeline;
    D3D12MA::Allocation*        motionBlurAllocation = nullptr;

    // Muestras CONSTRUIDAS ahora mismo en los targets y en los pipelines del
    // pase de escena. Lo que pide el usuario vive en el estado compartido; los
    // dos solo coinciden después de recrear, y eso pasa entre frames.
    UINT sampleCount = 1;

    // Color y profundidad multimuestra. Solo existen con MSAA activo: el pase
    // de escena dibuja ahí y al cerrarlo se resuelve sobre el HDR de siempre,
    // que es el que consumen el SSR, la niebla, el bloom y la composición.
    D3D12MA::Allocation* hdrMsAllocation   = nullptr;
    D3D12MA::Allocation* depthMsAllocation = nullptr;

    // Muestras que pide el estado, ya validadas contra lo que soporta el
    // device: 1 si el modo no es MSAA.
    UINT desiredSampleCount() const;

    // La profundidad que pueden LEER la niebla y los reflejos. Con MSAA la del
    // pase de escena es multimuestra y no se muestrea como una textura normal,
    // así que se usa la del pre-pase, que por eso se graba también cuando el
    // SSAO está apagado.
    ID3D12Resource* readableDepth() const
    {
        if (sampleCount > 1 && prepassDepthAllocation)
            return prepassDepthAllocation->GetResource();
        return depthAllocation ? depthAllocation->GetResource() : nullptr;
    }
    UINT readableDepthSrv() const
    {
        return (sampleCount > 1 && prepassDepthAllocation) ? kSrvPrepassDepth : kSrvDepth;
    }
    // Y en qué estado está fuera de esos pases: el del pre-pase lo deja su
    // propio grabado, el de la escena vive en escritura de profundidad.
    D3D12_RESOURCE_STATES readableDepthState() const
    {
        return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
    void applyPendingSampleCount();  // recrea targets y pipelines si cambió

    ComPtr<ID3D12RootSignature> fpCullRootSignature;
    ComPtr<ID3D12PipelineState> fpTiledPipeline;
    ComPtr<ID3D12PipelineState> fpClusteredPipeline;

    // Los dos de entrada van mapeados: se reescriben cada frame con la cámara y
    // las luces vivas. Los tres de salida los llena el culling en la GPU.
    void*  fpParamsMapped = nullptr;
    void*  fpLightsMapped = nullptr;
    D3D12MA::Allocation* fpStatsAllocation = nullptr;
    uint32_t fpCellCount = 0;  // celdas para las que están dimensionados los de salida

    // Las listas se quedan como lectura mientras el pase de escena las consume;
    // el frame siguiente las devuelve a escritura antes de rehacerlas.
    bool fpListsInPixelState = false;

    void createForwardPlusPipelines();
    void ensureForwardPlusGrid(uint32_t cells);
    void updateForwardPlus();   // parámetros y luces del frame
    void recordForwardPlusCull();

    // Árbol de la interfaz 2D del juego. Este backend todavía no la dibuja,
    // pero el editor la edita igual: tenerlo aquí es lo que permite que lo
    // haga sin preguntar con qué backend corre.
    UiCanvas uiCanvas;

    // Capa de interfaz del editor, si la hay. No es propiedad de este backend.
    UiLayer* uiLayer = nullptr;

    // La escena y su raíz, guardadas para quien las pregunte. Este backend no
    // las recorre: la geometría entra por registerGameObject.
    Scene*      scene     = nullptr;
    GameObject* sceneRoot = nullptr;

    // Guardado para que el panel conserve el valor; este backend no dibuja a
    // más resolución todavía.
    float ssaaFactor = 2.0f;

    // Destino alternativo del pase final. Con esto encendido el backbuffer solo
    // lleva interfaz, y la escena viaja como textura a quien la dibuje.
    D3D12MA::Allocation* viewportAllocation = nullptr;
    bool                 renderToTexture    = false;

    ComPtr<ID3D12RootSignature> taaRootSignature;
    ComPtr<ID3D12PipelineState> taaPipeline;
    std::array<D3D12MA::Allocation*, 2> taaHistoryAllocations{};

    // Cuál de las dos historias se escribe este frame. La otra es la que se lee.
    UINT      taaHistoryIndex = 0;
    bool      taaHistoryValid = false;
    uint32_t  taaJitterIndex  = 0;
    glm::mat4 taaCurrViewProj{1.0f};
    glm::mat4 taaPrevViewProj{1.0f};
    // Proyección del frame CON el desplazamiento de subpíxel. Fuera de TAA es
    // la de la cámara tal cual.
    glm::mat4 taaJitteredProj{1.0f};

    void createTaaPipeline();
    void recordTaa(D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv);

    void createSsrPipelines();
    void recordSsr();
    void createMotionBlurPipeline();
    void recordMotionBlur();
    // Encendido, con recursos y con taps que promediar. Lo consultan el pase y
    // el pre-pase de profundidad, que con MSAA es de donde sale el depth.
    bool motionBlurActive() const;

    void createSsaoPipelines();
    void createSsaoTargets();    // depende del tamaño: se rehace al redimensionar
    void releaseSsaoTargets();
    void recordDepthPrepassAndSsao();

    // El emborronado se queda como recurso de lectura mientras el pase de
    // escena lo muestrea; el frame siguiente tiene que devolverlo a escritura
    // antes de volver a dispararlo.
    bool ssaoBlurNeedsUav = false;

    // ── Luces ───────────────────────────────────────────────────────────
    // Las de la escena, tal cual las manda quien la carga. Vacío = ninguna
    // escena las ha puesto todavía, y entonces se usa la direccional de
    // relleno del backend, que es lo que ilumina la escena de arranque.
    std::vector<ShaderLight> sceneLights;

    // ── Cámara ──────────────────────────────────────────────────────────
    // Un solo sitio: la rejilla, la malla, la niebla y el reparto de cascadas
    // tenían cada uno su lookAt copiado, y bastaba con tocar uno para que el
    // suelo dejara de caer bajo los objetos.
    glm::vec3 cameraPos{6.0f, 4.5f, 8.0f};
    glm::mat4 cameraView =
        glm::lookAtRH(glm::vec3(6.0f, 4.5f, 8.0f), glm::vec3(0.0f, 0.5f, 0.0f),
                      glm::vec3(0.0f, 1.0f, 0.0f));
    float cameraFovDeg = 60.0f;

    // Tamaño característico de la escena, del que salen near y far en edición
    // (near = /1000, far = ×3), igual que en el camino de Vulkan. Lo recalcula
    // refitCameraRange cuando cambia la geometría; antes de eso el rango estaba
    // clavado a 0.1-500 y una escena más profunda se recortaba.
    float cameraDistance = 200.0f;

    // perspectiveRH_ZO, no perspective a secas: D3D12 clipea en z=[0,1] igual
    // que Vulkan, y con la convención de OpenGL se pierde la mitad cercana.
    // Proyección con la que se dibuja. Manda, por este orden:
    //   1. la cara de una sonda que se está horneando —90°, cuadrada y con el
    //      rango largo—, que tiene que verla TODO lo que dibuja esa cara;
    //   2. el CameraComponent de la escena, mientras corre Play;
    //   3. la de edición, con el fov que empuja el editor.
    // Resolverlo aquí y no por parámetro es lo que hace que el UBO, la niebla,
    // el cielo y el culling no puedan discrepar.
    glm::mat4 cameraProj() const
    {
        if (probeFaceProj)
            return *probeFaceProj;
        if (sceneCameraProj)
            return *sceneCameraProj;

        return glm::perspectiveRH_ZO(glm::radians(cameraFovDeg), viewportAspectRatio(),
                                     cameraDistance * 0.001f, cameraDistance * 3.0f);
    }
    float viewportAspectRatio() const
    {
        return (height > 0) ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    }
    std::optional<glm::mat4> probeFaceProj;

    // Proyección del CameraComponent, mientras manda. Se resuelve una vez por
    // frame (resolveFrameCamera) y no en cada consulta: cameraProj() se llama
    // una decena de veces por frame y buscar la cámara en el árbol cada vez
    // sería recorrer la escena diez veces para nada.
    std::optional<glm::mat4> sceneCameraProj;
    void                     resolveFrameCamera();

    void ensureSkinnedInstanceBuffer(size_t count);

    // Suelta los recursos GPU de todos los personajes. NO espera a la GPU: los
    // dos sitios que la llaman (shutdown y clearSkinnedMeshes) ya lo han hecho.
    void releaseSkinnedObjects();

    LARGE_INTEGER lastTick{};
    LARGE_INTEGER tickFrequency{};

    // ── Suelo sólido ────────────────────────────────────────────────────────
    // La rejilla son líneas y no recibe sombra: hace falta una superficie de
    // verdad para que se vea algo proyectado.
    D3D12MA::Allocation*     groundVertexAllocation = nullptr;
    D3D12MA::Allocation*     groundIndexAllocation  = nullptr;
    D3D12MA::Allocation*     groundInstanceAllocation = nullptr;
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
    ComPtr<ID3D12RootSignature> ssaaRootSignature;
    ComPtr<ID3D12PipelineState> ssaaPipeline;

    // ─── UI 2D del juego ─────────────────────────────────────────────────────
    // Los quads los arma UiCanvas en CPU (buildDrawData, que no sabe de ninguna
    // API) y aquí solo se suben y se dibujan. Un par de buffers por frame en
    // vuelo, mapeados y con crecimiento por duplicación: el contenido cambia
    // entero cada frame y no compensa un staging.
    ComPtr<ID3D12RootSignature> uiRootSignature;
    ComPtr<ID3D12PipelineState> uiPipeline;
    UiDrawData                  uiDrawData;
    std::array<D3D12MA::Allocation*, kFrameCount> uiVertexAllocations{};
    std::array<void*, kFrameCount>                uiVertexMapped{};
    std::array<UINT, kFrameCount>                 uiVertexCapacity{};
    std::array<D3D12MA::Allocation*, kFrameCount> uiIndexAllocations{};
    std::array<void*, kFrameCount>                uiIndexMapped{};
    std::array<UINT, kFrameCount>                 uiIndexCapacity{};

    // El pase de geometría entero, para poder repetirlo desde otra cámara: es
    // lo que necesita el horneado de una sonda de reflexión.
    void recordSceneGeometry(D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                             UINT targetWidth, UINT targetHeight);

    void createUiPipeline();
    void ensureUiBuffers(UINT vertexCount, UINT indexCount);
    void recordUiCanvas(D3D12_CPU_DESCRIPTOR_HANDLE targetRtv);

    // Atlas y fuentes de la UI. El backend es su dueño: los widgets solo
    // guardan el puntero, que es también la clave con la que el lote dice qué
    // textura quiere.
    std::vector<std::unique_ptr<UiTextureAtlas>>          uiAtlases;
    std::vector<std::unique_ptr<UiFont>>                  uiFonts;
    std::unordered_map<const UiTextureAtlas*, UINT>       uiAtlasSrv;
    std::vector<D3D12MA::Allocation*>                     uiAtlasTextures;
    UINT                                                  uiNextAtlasSlot = 0;

    // Sube los píxeles que el atlas ya tiene cargados y le crea su SRV. false
    // si no hay hueco o la subida falla: el lote se dibujará con la 1x1 blanca.
    bool registerUiAtlas(UiTextureAtlas& atlas);

    // ─── Sondas de reflexión ─────────────────────────────────────────────────
    struct GpuProbe {
        uint64_t  ownerId  = 0;   // GameObject::id de la sonda
        glm::vec3 position{0.0f};
        float     radius    = 0.0f;
        float     intensity = 1.0f;

        // Las tres imágenes: la captura de la escena y las dos que salen de
        // convolucionarla, que son las que acaban en t4 y t5 de los objetos.
        D3D12MA::Allocation* captureAllocation    = nullptr;
        D3D12MA::Allocation* irradianceAllocation = nullptr;
        D3D12MA::Allocation* prefilterAllocation  = nullptr;

        // Primer hueco de su bloque en el heap. El resto sale de sumar, en el
        // orden que fija kSrvPerProbe.
        UINT srvBase = 0;

        bool  baked  = false;  // false: todavía enseña el IBL global
        float bakeMs = 0.0f;   // último horneado, medido con los timestamps

        // El horneado se quedó a medias (la GPU rechazó una lista). Sin esto,
        // "no horneada" haría que se reintentara en CADA frame, y cada intento
        // espera a la GPU siete veces: un fallo permanente dejaría el editor a
        // rastras. Se limpia cuando la sonda cambia, que es cuando vuelve a
        // tener sentido intentarlo.
        bool  bakeFailed = false;
    };
    std::vector<GpuProbe> probes;

    // Índices dentro del bloque de una sonda.
    static constexpr UINT kProbeCaptureSrv    = 0;
    static constexpr UINT kProbeIrradianceSrv = 1;
    static constexpr UINT kProbeIrradianceUav = 2;
    static constexpr UINT kProbePrefilterSrv  = 3;
    static constexpr UINT kProbePrefilterUav  = 4;  // + mip

    // Crea las tres imágenes de una sonda y sus vistas. false si no queda hueco
    // en el heap o la GPU no da la memoria.
    bool createProbeResources(GpuProbe& probe);
    void releaseProbe(GpuProbe& probe);

    // Reconcilia la lista con los ReflectionProbeComponent de la escena: crea
    // las nuevas, suelta las que ya no están y refresca posición, radio e
    // intensidad. Por frame, y sale enseguida cuando no hay ninguna.
    void syncProbes();

    // Los dos compute del IBL sobre una entrada y unos destinos cualesquiera:
    // lo usa el cielo global y lo usa cada sonda.
    void recordIblConvolution(UINT sourceSrv, UINT irradianceUav, UINT prefilterUav,
                              float intensity);

    // Captura la escena desde la sonda —seis caras— y convoluciona el resultado
    // en sus dos cubemaps. Es un EVENTO, no un pase del frame: espera a la GPU,
    // se toma su tiempo y deja la sonda marcada como horneada.
    void bakeProbe(GpuProbe& probe);

    // Profundidad propia del horneado: el buffer del frame tiene el tamaño del
    // render y aquí las caras son de kProbeFaceSize.
    D3D12MA::Allocation* probeDepthAllocation = nullptr;
    void createProbeDepth();

    // Peticiones pendientes: el editor pide hornear y se atiende en el frame
    // siguiente, fuera de cualquier grabado a medias.
    std::vector<uint64_t> probeBakeQueue;
    bool                  probeBakeAllQueued = false;
    float                 probeLastBakeMs    = 0.0f;

    // Qué sonda mira cada objeto, por su índice en `probes`; -1 = el IBL global.
    // Se guarda para no reescribir descriptores en un frame en el que nada
    // cambió, que es el caso normal.
    std::vector<int> probeAssignStatic;
    std::vector<int> probeAssignSkinned;

    // La sonda que le toca a un punto del mundo: la más cercana de las que lo
    // contienen. -1 si ninguna llega.
    int  pickProbeFor(const glm::vec3& worldPos) const;
    // Reescribe t4 y t5 de los bloques cuya sonda haya cambiado. Devuelve
    // cuántos se tocaron.
    int  refreshProbeAssignment();
    // Deja t4/t5 de ese bloque apuntando a la sonda dada, o al IBL global.
    void writeProbeSlots(UINT blockBase, int probeIndex);
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

    // ─── Tiempos de GPU ──────────────────────────────────────────────────────
    // Dos marcas por pase (entrada y salida) y un par más para el frame entero.
    // El panel de rendimiento los pide uno a uno, así que se guardan por pase y
    // no como un total.
    enum TimestampSlot : UINT {
        TsFrame       = 0,
        TsShadow      = 2,
        TsScene       = 4,
        TsForwardPlus = 6,
        TsSsao        = 8,
        TsSsr         = 10,
        TsFog         = 12,
        TsBloom       = 14,
        TsAa          = 16,
        TsCount       = 18,
    };
    ComPtr<ID3D12QueryHeap> timestampHeap;
    ComPtr<ID3D12Resource>  timestampReadback;
    const UINT64*           timestampMapped   = nullptr;
    UINT64                  timestampFreq     = 0;
    std::array<float, TsCount / 2> gpuMs{};

    void createTimestampResources();
    void markTimestamp(UINT slot);
    void readTimestamps();     // los del frame anterior, antes de sobrescribir
    void resolveTimestamps();  // vuelca los de este frame al buffer de lectura

    // Cuentas del frame, para el panel: se rellenan al grabar el pase principal.
    int statDraws       = 0;
    int statInstanced   = 0;
    int statCulledCount = 0;

    // Sin editor delante: se apagan la rejilla y los gizmos, que son suyos. Lo
    // enciende el runtime antes de arrancar.
    bool headless = false;

    UINT frameIndex = 0;
    // Tamaño al que se DIBUJA la escena: profundidad, HDR, oclusión, bloom y el
    // LDR van a este. Con SSAA es un múltiplo del de salida.
    UINT width      = 0;
    UINT height     = 0;
    // Tamaño al que se ENTREGA la imagen: el backbuffer o la textura del panel.
    // Sin SSAA coincide con el de render, y entonces el pase final es un blit
    // con filtro; con SSAA es más pequeño y ese pase promedia.
    UINT outWidth   = 0;
    UINT outHeight  = 0;

    // Tamaño anotado por el callback de la ventana, pendiente de aplicar. Ver
    // el comentario de resize() en la cabecera: el trabajo de DXGI no puede
    // hacerse dentro del WindowProc.
    // Tamaño de la swapchain, que es el de la ventana. width y height son el
    // tamaño de RENDER: coinciden con este salvo cuando la escena va a un
    // panel, y entonces mandan las medidas del panel.
    UINT swapWidth  = 0;
    UINT swapHeight = 0;

    // Tamaño de render pedido desde fuera, pendiente de aplicar entre frames.
    UINT pendingRenderWidth  = 0;
    UINT pendingRenderHeight = 0;

    void applyPendingRenderSize();

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

    // Decodifica la textura del material (embebida o de fichero), la sube y le
    // crea el SRV en `srvIndex`. nullptr si no hay textura o no se pudo leer:
    // quien llama pone ahí la vista de la 1x1 global.
    D3D12MA::Allocation* uploadMaterialTexture(const std::string& path,
                                               const std::vector<uint8_t>& embedded, bool srgb,
                                               UINT srvIndex);

    // Vista de una textura 2D ya subida en un hueco cualquiera. Para repetir
    // las 1x1 globales dentro de la terna de un objeto sin volver a subirlas.
    void createTexture2DSrv(ID3D12Resource* resource, DXGI_FORMAT format, UINT srvIndex);

    // Vista del array de cascadas (o de su relleno 1x1 mientras no exista) en
    // el hueco dado. El bloque de cada objeto necesita la suya.
    void createShadowMapSrv(UINT srvIndex);

    // Vista de un cubemap ya subido en el hueco dado, con sus mips.
    void createCubeSrv(ID3D12Resource* resource, DXGI_FORMAT format, UINT mipLevels,
                       UINT srvIndex);

    // Rellena t3..t7 de un bloque con los recursos compartidos: sombras,
    // los dos cubemaps de entorno y la oclusión. Lo que cambia por objeto son
    // t1 y t2, que los escribe quien lo sube.
    void fillSharedSlots(UINT blockBase);

    // t7 de un bloque: el mapa de oclusion si el SSAO corre, la 1x1 blanca si
    // no. Y el refresco de todos cuando el interruptor cambia.
    void writeAoSlot(UINT blockBase);
    void refreshAoSlots();
    bool aoSlotsUseMap = false;

    void createMeshPipeline();
    void createMeshResources();
    void createDepthBuffer();
    void updateSceneUbo();

    // Crea un buffer vacío en VRAM con acceso de escritura desordenada, que es
    // lo que necesitan los destinos de los tres compute.
    D3D12MA::Allocation* createStorageBuffer(UINT64 size, D3D12_RESOURCE_STATES initialState);

    void createSkinningPipelines();
    // Sube un personaje y devuelve su índice en skinnedObjects, o -1 si la
    // malla no tiene esqueleto, vértices o claves que evaluar.
    int  createSkinnedObject(const SkinnedMesh& mesh);
    void recordSkinning();  // los tres dispatch por personaje, con sus barreras

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

void D3D12Renderer::Impl::createTimestampResources()
{
    // La frecuencia es de la COLA, no del device: es lo que convierte los ticks
    // en segundos. Una cola de copia tendría otra distinta.
    if (FAILED(queue->GetTimestampFrequency(&timestampFreq)) || timestampFreq == 0) {
        // Sin reloj no hay medidas, pero tampoco hay motivo para no dibujar: el
        // panel enseñará ceros.
        timestampFreq = 0;
        return;
    }

    D3D12_QUERY_HEAP_DESC heapDesc{};
    heapDesc.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count = kFrameCount * TsCount;
    if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&timestampHeap)))) {
        timestampFreq = 0;
        return;
    }

    // El destino del resolve va en un heap de lectura: es el único desde el que
    // la CPU puede leer sin copia intermedia.
    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width            = static_cast<UINT64>(kFrameCount) * TsCount * sizeof(UINT64);
    bufferDesc.Height           = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels        = 1;
    bufferDesc.Format           = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&timestampReadback)))) {
        timestampHeap.Reset();
        timestampFreq = 0;
        return;
    }

    // Mapeado de una vez y para siempre: se lee cuando la fence del slot dice
    // que la GPU ya escribió, así que no hace falta mapear y desmapear por
    // frame.
    void* mapped = nullptr;
    if (FAILED(timestampReadback->Map(0, nullptr, &mapped))) {
        timestampReadback.Reset();
        timestampHeap.Reset();
        timestampFreq = 0;
        return;
    }
    timestampMapped = static_cast<const UINT64*>(mapped);
}

void D3D12Renderer::Impl::markTimestamp(UINT slot)
{
    if (!timestampHeap)
        return;
    commandList->EndQuery(timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                          frameIndex * TsCount + slot);
}

void D3D12Renderer::Impl::readTimestamps()
{
    if (!timestampMapped || timestampFreq == 0)
        return;

    // Este slot ya pasó por moveToNextFrame, que esperó su fence: lo que hay en
    // el buffer es del último frame que lo usó, y está completo.
    const UINT64* base    = timestampMapped + static_cast<size_t>(frameIndex) * TsCount;
    const double  toMs    = 1000.0 / static_cast<double>(timestampFreq);

    for (UINT pair = 0; pair < TsCount / 2; ++pair) {
        const UINT64 begin = base[pair * 2];
        const UINT64 end   = base[pair * 2 + 1];
        gpuMs[pair] = (end > begin) ? static_cast<float>((end - begin) * toMs) : 0.0f;
    }
}

void D3D12Renderer::Impl::resolveTimestamps()
{
    if (!timestampHeap || !timestampReadback)
        return;

    const UINT base = frameIndex * TsCount;
    commandList->ResolveQueryData(timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base, TsCount,
                                  timestampReadback.Get(),
                                  static_cast<UINT64>(base) * sizeof(UINT64));
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

void D3D12Renderer::Impl::ensureDebugLineBuffer(size_t vertexCount)
{
    if (vertexCount <= debugLinesCapacity)
        return;

    // Se crece por bloques: una escena con colliders visibles manda miles de
    // vértices y el número sube y baja entre frames.
    const size_t newCapacity = (std::max)(vertexCount, debugLinesCapacity * 2 + 1024);

    if (debugLinesAllocation) {
        // Puede estar en uso por el frame anterior.
        waitForGpu();
        if (debugLinesMapped) {
            debugLinesAllocation->GetResource()->Unmap(0, nullptr);
            debugLinesMapped = nullptr;
        }
        debugLinesAllocation->Release();
        debugLinesAllocation = nullptr;
    }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = newCapacity * sizeof(GizmoVertex);
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    throwIfFailed(allocator->CreateResource(&allocDesc, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr, &debugLinesAllocation, IID_NULL, nullptr),
                  "D3D12MA::Allocator::CreateResource(lineas de depuracion)");

    const D3D12_RANGE noRead{0, 0};
    throwIfFailed(debugLinesAllocation->GetResource()->Map(0, &noRead, &debugLinesMapped),
                  "ID3D12Resource::Map(lineas de depuracion)");

    debugLinesCapacity = newCapacity;
    debugLinesView.BufferLocation = debugLinesAllocation->GetResource()->GetGPUVirtualAddress();
    debugLinesView.SizeInBytes    = static_cast<UINT>(newCapacity * sizeof(GizmoVertex));
    debugLinesView.StrideInBytes  = sizeof(GizmoVertex);
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
    psoDesc.RTVFormats[0]         = kHdrFormat;
    psoDesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count      = sampleCount;
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

D3D12MA::Allocation* D3D12Renderer::Impl::uploadMaterialTexture(
    const std::string& path, const std::vector<uint8_t>& embedded, bool srgb, UINT srvIndex)
{
    int      w = 0, h = 0, channels = 0;
    stbi_uc* pixels = nullptr;
    if (!embedded.empty())
        pixels = stbi_load_from_memory(embedded.data(), static_cast<int>(embedded.size()), &w, &h,
                                       &channels, STBI_rgb_alpha);
    else if (!path.empty())
        pixels = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);

    if (!pixels)
        return nullptr;

    // sRGB para el color base y lineal para las normales: una normal
    // interpretada como color se descodifica con gamma y apunta a otro sitio.
    const DXGI_FORMAT format =
        srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;

    D3D12MA::Allocation* allocation = nullptr;
    try {
        allocation = uploadTexture(pixels, static_cast<UINT>(w), static_cast<UINT>(h), 1, format, 4,
                                   srvIndex);
    } catch (...) {
        stbi_image_free(pixels);
        throw;
    }
    stbi_image_free(pixels);
    return allocation;
}

void D3D12Renderer::Impl::createTexture2DSrv(ID3D12Resource* resource, DXGI_FORMAT format,
                                             UINT srvIndex)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                  = format;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels     = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(srvIndex) * srvSize;
    device->CreateShaderResourceView(resource, &srvDesc, handle);
}

void D3D12Renderer::Impl::createShadowMapSrv(UINT srvIndex)
{
    // El array de cascadas si ya existe; si no, el relleno 1x1, que se creó
    // con las mismas cuatro slices y el mismo formato.
    ID3D12Resource* resource = shadowMapArrayAllocation ? shadowMapArrayAllocation->GetResource()
                                                        : shadowMapAllocation->GetResource();
    if (!resource)
        return;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                   = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension            = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Shader4ComponentMapping  = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.ArraySize = kShadowCascades;

    D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(srvIndex) * srvSize;
    device->CreateShaderResourceView(resource, &srvDesc, handle);
}

void D3D12Renderer::Impl::createCubeSrv(ID3D12Resource* resource, DXGI_FORMAT format,
                                        UINT mipLevels, UINT srvIndex)
{
    if (!resource)
        return;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                  = format;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.TextureCube.MipLevels   = mipLevels;

    D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(srvIndex) * srvSize;
    device->CreateShaderResourceView(resource, &srvDesc, handle);
}

void D3D12Renderer::Impl::fillSharedSlots(UINT blockBase)
{
    createShadowMapSrv(blockBase + 2);

    if (metalRoughAllocation)
        createTexture2DSrv(metalRoughAllocation->GetResource(), DXGI_FORMAT_R8G8B8A8_UNORM,
                           blockBase + 3);
    if (irradianceAllocation)
        createCubeSrv(irradianceAllocation->GetResource(), kHdrFormat, 1, blockBase + 4);
    if (prefilterAllocation)
        createCubeSrv(prefilterAllocation->GetResource(), kHdrFormat, prefilterMips,
                      blockBase + 5);
    writeAoSlot(blockBase);
}

void D3D12Renderer::Impl::writeAoSlot(UINT blockBase)
{
    // t7 = oclusión ambiental. Con el SSAO ENCENDIDO, el mapa del frame; con él
    // apagado, la 1x1 blanca.
    //
    // Esto último no es cosmético: si el SSAO no corre, su mapa no se escribe
    // NUNCA —una textura recién creada en D3D12 no está inicializada— y el
    // shader multiplica el ambiente por lo que haya ahí, que es cero. El
    // síntoma era que todo lo que no recibiera luz directa salía NEGRO, con el
    // IBL bien calculado y subir la intensidad del ambiente sin efecto ninguno:
    // cualquier cosa por cero sigue siendo cero.
    const bool useMap = state->ssaoEnabled() && ssaoBlurAllocation != nullptr;
    if (useMap) {
        createTexture2DSrv(ssaoBlurAllocation->GetResource(), DXGI_FORMAT_R32_FLOAT,
                           blockBase + 6);
        return;
    }
    if (baseColorAllocation)
        createTexture2DSrv(baseColorAllocation->GetResource(), DXGI_FORMAT_R8G8B8A8_UNORM,
                           blockBase + 6);
}

void D3D12Renderer::Impl::refreshAoSlots()
{
    const bool useMap = state->ssaoEnabled() && ssaoBlurAllocation != nullptr;
    if (useMap == aoSlotsUseMap)
        return;

    // Los descriptores pueden estar en uso por el frame en vuelo.
    waitForGpu();
    aoSlotsUseMap = useMap;

    for (const StaticObject& object : objects)
        writeAoSlot(object.srvBase);
    for (const SkinnedObject& character : skinnedObjects)
        for (const SkinnedSubMesh& sub : character.subMeshes)
            writeAoSlot(sub.srvBase);
    writeAoSlot(kSrvBaseColor);
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
    textureRange.NumDescriptors     = kSrvPerObject;
    textureRange.BaseShaderRegister = 1;  // t1..t7
    textureRange.RegisterSpace      = 0;
    textureRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // Forward+ vive en su propio space: cuatro ByteAddressBuffer que van como
    // root SRV, sin tabla ni descriptores.
    D3D12_ROOT_PARAMETER params[8]{};
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

    for (UINT i = 0; i < 4; ++i) {
        params[4 + i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[4 + i].Descriptor.ShaderRegister = i;  // t0..t3 de space2
        params[4 + i].Descriptor.RegisterSpace  = 2;
        params[4 + i].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    // Samplers estáticos: los shaders no eligen filtro ni wrap en tiempo de
    // ejecución, así que no hace falta un heap de samplers ni descriptores.
    // s1, s2 y s4: texturas de material, que se repiten con el UV. s5, s6 y s7
    // van a borde fijo — un cubemap o un mapa de pantalla no se repiten.
    D3D12_STATIC_SAMPLER_DESC samplers[7]{};
    const UINT                wrapRegisters[3]  = {1, 2, 4};
    const UINT                clampRegisters[3] = {5, 6, 7};
    for (int i = 0; i < 3; ++i) {
        samplers[i].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[i].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[i].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[i].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[i].MaxLOD           = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister   = wrapRegisters[i];
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    for (int i = 0; i < 3; ++i) {
        D3D12_STATIC_SAMPLER_DESC& sampler = samplers[3 + i];
        sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD           = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister   = clampRegisters[i];
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // s3 es el del sampler2DArrayShadow: comparación, no filtrado normal.
    samplers[6].Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplers[6].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[6].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[6].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[6].ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[6].MaxLOD           = D3D12_FLOAT32_MAX;
    samplers[6].ShaderRegister   = 3;
    samplers[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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
    const std::vector<char> pixelShader  = readBinaryFile("shaders/pbr.frag.dxil");

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
    psoDesc.RTVFormats[0]         = kHdrFormat;
    psoDesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count      = sampleCount;
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

    // El de alambre, con lo único que cambia: el relleno. Sin cara trasera
    // descartada, que en alambre esconde la mitad de las aristas.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC wireDesc = psoDesc;
    wireDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    wireDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    throwIfFailed(device->CreateGraphicsPipelineState(&wireDesc, IID_PPV_ARGS(&meshWirePipeline)),
                  "ID3D12Device::CreateGraphicsPipelineState(malla en alambre)");

    // El contorno: mismos vértices, otro par de shaders y la cara CONTRARIA
    // descartada.
    {
        const std::vector<char> outlineVs = readBinaryFile("shaders/outline.vert.dxil");
        const std::vector<char> outlinePs = readBinaryFile("shaders/outline.frag.dxil");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC outlineDesc = psoDesc;
        outlineDesc.VS = {outlineVs.data(), outlineVs.size()};
        outlineDesc.PS = {outlinePs.data(), outlinePs.size()};
        outlineDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
        // LESS estricto y no LESS_EQUAL como la malla: el casco cae a la MISMA
        // profundidad que la superficie en las zonas planas, y con el igual
        // incluido pasaría el test y la taparía entera.
        outlineDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        throwIfFailed(device->CreateGraphicsPipelineState(&outlineDesc,
                                                          IID_PPV_ARGS(&outlinePipeline)),
                      "ID3D12Device::CreateGraphicsPipelineState(contorno)");

        // El mismo, pero contra el target LDR: ahí es donde se dibuja de
        // verdad, DESPUÉS del tone mapping, para que su naranja llegue plano en
        // vez de pasar por ACES. Siempre una muestra —el LDR no es
        // multimuestra— y sin escribir profundidad, que ya no es suya.
        outlineDesc.RTVFormats[0]                       = kLdrFormat;
        outlineDesc.SampleDesc.Count                    = 1;
        outlineDesc.DepthStencilState.DepthWriteMask    = D3D12_DEPTH_WRITE_MASK_ZERO;
        throwIfFailed(device->CreateGraphicsPipelineState(&outlineDesc,
                                                          IID_PPV_ARGS(&outlineLdrPipeline)),
                      "ID3D12Device::CreateGraphicsPipelineState(contorno sobre LDR)");
    }

    throwIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&meshPipeline)),
                  "ID3D12Device::CreateGraphicsPipelineState(malla)");
}

void D3D12Renderer::Impl::createMeshResources()
{
    // Buffer de instancias con la identidad. La geometría de la escena usa el
    // push constant para su transformación (flags.x = 0), pero el shader
    // declara el SSBO igual y la root signature tiene que satisfacerlo.
    const glm::mat4 instanceModel{1.0f};
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
    shadowMapAllocation = uploadTexture(noShadow, 1, 1, 4, DXGI_FORMAT_R32_FLOAT, 4, kSrvShadowMap);

    // t4: ORM sin textura. R = oclusión, G = rugosidad, B = metalicidad, y
    // pbr.frag los multiplica por los factores del push: a 255 el material
    // manda entero, que es lo que hacía el shader anterior.
    const uint8_t neutralOrm[4] = {255, 255, 255, 255};
    metalRoughAllocation =
        uploadTexture(neutralOrm, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4, kSrvMetalRough);

    // t7: oclusión de pantalla a 1. Con SSAO apagado el shader multiplica por
    // la unidad y no hace falta ninguna rama.
    const uint8_t noOcclusion[4] = {255, 255, 255, 255};
    ssaoAllocation = uploadTexture(noOcclusion, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4, kSrvSsao);

    // t5 y t6: entorno neutro, los mismos valores que deja el camino de Vulkan
    // cuando no hay cubemap. Un ambiente plano ilumina de forma aburrida, pero
    // sin ellos pbr.frag leería de un descriptor vacío.
    createNeutralIblCubes();

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
    // View-proj SIN jitter: es la que reproyecta el TAA y la que se compara con
    // la del frame anterior.
    taaPrevViewProj = taaCurrViewProj;
    taaCurrViewProj = cameraProj() * cameraView;
    taaJitteredProj = cameraProj();

    if (state->aaMode() == RendererState::AaMode::Taa) {
        // Halton(2,3) desplazado a [-0.5, 0.5] píxeles: 16 posiciones antes de
        // repetir, suficientes para que el promedio sea estable y pocas para
        // que el ciclo no se note al parar la cámara.
        const glm::vec2 jitter(
            (halton(taaJitterIndex + 1, 2) - 0.5f) * state->taaJitterScale(),
            (halton(taaJitterIndex + 1, 3) - 0.5f) * state->taaJitterScale());
        taaJitterIndex = (taaJitterIndex + 1) % 16;

        // En clip space el ancho completo es 2, de ahí el factor. Va sobre la
        // columna de la Z para que el desplazamiento sea constante en pantalla
        // a cualquier profundidad.
        taaJitteredProj[2][0] += 2.0f * jitter.x / static_cast<float>(width);
        taaJitteredProj[2][1] += 2.0f * jitter.y / static_cast<float>(height);
    }

    SceneUbo ubo{};
    ubo.view = cameraView;
    ubo.proj = taaJitteredProj;
    // Vulkan tiene el eje Y de pantalla invertido respecto a OpenGL y el motor
    // lo compensa ahí; D3D12 usa la misma orientación que OpenGL, así que aquí
    // NO se invierte.

    ubo.cascadeSplits = cascadeSplits;
    for (int i = 0; i < kShadowCascades; ++i)
        ubo.lightSpaceMatrix[i] = cascadeMatrices[i];

    if (!sceneLights.empty()) {
        // Las de la escena mandan: sin esto el backend iluminaba con su
        // direccional de relleno y una escena con focos se veía a oscuras
        // aunque los tuviera bien puestos.
        const size_t count = (std::min)(sceneLights.size(), static_cast<size_t>(16));
        std::memcpy(ubo.lights, sceneLights.data(), count * sizeof(ShaderLight));
        ubo.numLights = static_cast<int>(count);

        ubo.viewPos          = glm::vec4(cameraPos, 1.0f);
        ubo.ambientIntensity = state->ambientEnabled() ? state->ambientIntensity() : 0.0f;
        std::memcpy(sceneUboMapped[frameIndex], &ubo, sizeof(ubo));
        return;
    }

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

    ubo.viewPos          = glm::vec4(cameraPos, 1.0f);
    // Apagado = intensidad cero, igual que en el camino de Vulkan: el shader no
    // tiene rama para el ambiente, y sin esto el interruptor "Ambient (IBL)" del
    // menu View no hacia nada con este backend.
    ubo.ambientIntensity = state->ambientEnabled() ? state->ambientIntensity() : 0.0f;

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
    const std::vector<char> pixelShader  = readBinaryFile("shaders/pbr.frag.dxil");

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
    psoDesc.RTVFormats[0]         = kHdrFormat;
    psoDesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count      = sampleCount;
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

    D3D12_GRAPHICS_PIPELINE_STATE_DESC wireDesc = psoDesc;
    wireDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    wireDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    throwIfFailed(device->CreateGraphicsPipelineState(&wireDesc,
                                                      IID_PPV_ARGS(&skinnedMeshWirePipeline)),
                  "ID3D12Device::CreateGraphicsPipelineState(skinned en alambre)");

    {
        // El contorno del personaje va sobre los vértices que deja el skinning,
        // así que hereda ESTE input layout y no el de la malla del motor.
        const std::vector<char> outlineVs = readBinaryFile("shaders/outline.vert.dxil");
        const std::vector<char> outlinePs = readBinaryFile("shaders/outline.frag.dxil");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC outlineDesc = psoDesc;
        outlineDesc.VS = {outlineVs.data(), outlineVs.size()};
        outlineDesc.PS = {outlinePs.data(), outlinePs.size()};
        outlineDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
        // LESS estricto y no LESS_EQUAL como la malla: el casco cae a la MISMA
        // profundidad que la superficie en las zonas planas, y con el igual
        // incluido pasaría el test y la taparía entera.
        outlineDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        throwIfFailed(device->CreateGraphicsPipelineState(&outlineDesc,
                                                          IID_PPV_ARGS(&outlineSkinnedPipeline)),
                      "ID3D12Device::CreateGraphicsPipelineState(contorno skinned)");

        // Variante sobre el target LDR, por lo mismo que la de la malla.
        outlineDesc.RTVFormats[0]                    = kLdrFormat;
        outlineDesc.SampleDesc.Count                 = 1;
        outlineDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        throwIfFailed(
            device->CreateGraphicsPipelineState(&outlineDesc,
                                                IID_PPV_ARGS(&outlineSkinnedLdrPipeline)),
            "ID3D12Device::CreateGraphicsPipelineState(contorno skinned sobre LDR)");
    }
}

int D3D12Renderer::Impl::createSkinnedObject(const SkinnedMesh& mesh)
{
    if (mesh.skinnedVertices.empty() || mesh.skeleton.names.empty() || mesh.indices.empty())
        return -1;

    const PackedClips packed = packSkinnedClips(mesh);
    if (packed.boneInfos.empty())
        return -1;

    SkinnedObject object;
    object.boneCount    = static_cast<uint32_t>(mesh.skeleton.names.size());
    object.vertexCount  = static_cast<uint32_t>(mesh.skinnedVertices.size());
    object.clipBase     = 0;
    object.animDuration = mesh.animationClips.empty() ? 0.0f : mesh.animationClips[0].duration;

    object.posKeys   = uploadBuffer(packed.pos.data(), packed.pos.size() * sizeof(GpuPosKey),
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    object.rotKeys   = uploadBuffer(packed.rot.data(), packed.rot.size() * sizeof(GpuRotKey),
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    object.scaleKeys = uploadBuffer(packed.scale.data(), packed.scale.size() * sizeof(GpuPosKey),
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    object.boneInfos = uploadBuffer(packed.boneInfos.data(),
                                    packed.boneInfos.size() * sizeof(GpuBoneInfo),
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    object.inputVerts =
        uploadBuffer(mesh.skinnedVertices.data(), mesh.skinnedVertices.size() * sizeof(SkinnedVertex),
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    object.localXforms = createStorageBuffer(static_cast<UINT64>(object.boneCount) * sizeof(glm::mat4),
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    object.finalBones  = createStorageBuffer(static_cast<UINT64>(object.boneCount) * sizeof(glm::mat4),
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    object.outputVerts = createStorageBuffer(
        static_cast<UINT64>(object.vertexCount) * kSkinnedOutputStride,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    object.indices = uploadBuffer(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t),
                                  D3D12_RESOURCE_STATE_INDEX_BUFFER);

    object.vertexBufferView.BufferLocation = object.outputVerts->GetResource()->GetGPUVirtualAddress();
    object.vertexBufferView.SizeInBytes    = object.vertexCount * kSkinnedOutputStride;
    object.vertexBufferView.StrideInBytes  = kSkinnedOutputStride;

    object.indexBufferView.BufferLocation = object.indices->GetResource()->GetGPUVirtualAddress();
    object.indexBufferView.SizeInBytes = static_cast<UINT>(mesh.indices.size() * sizeof(uint32_t));
    object.indexBufferView.Format      = DXGI_FORMAT_R32_UINT;
    object.indexCount                  = static_cast<UINT>(mesh.indices.size());

    // Materiales por submalla. Sin subMeshRanges (FBX de una sola pieza) se
    // toma el material del propio Mesh, que es lo que rellena ModelLoader.
    struct RangeSrc {
        uint32_t        start;
        uint32_t        count;
        const Material* material;
    };
    std::vector<RangeSrc> ranges;
    if (!mesh.subMeshRanges.empty()) {
        for (const SubMeshRange& range : mesh.subMeshRanges) {
            const Material* material = range.materialIndex < mesh.materials.size()
                                           ? &mesh.materials[range.materialIndex]
                                           : &mesh.material;
            ranges.push_back({range.indexStart, range.indexCount, material});
        }
    } else {
        ranges.push_back({0, static_cast<uint32_t>(mesh.indices.size()), &mesh.material});
    }

    for (const RangeSrc& range : ranges) {
        SkinnedSubMesh sub;
        sub.indexStart = range.start;
        sub.indexCount = range.count;

        // Las ternas se reparten entre TODOS los personajes de la escena, no
        // por personaje: pasado el tope, la submalla cae a la terna global.
        if (nextSkinnedSlot < kMaxSkinnedSlots) {
            const UINT slot = kSrvSkinned + nextSkinnedSlot * kSrvPerObject;
            ++nextSkinnedSlot;
            sub.srvBase = slot;

            D3D12MA::Allocation* base = uploadMaterialTexture(
                range.material->texturePath, range.material->embeddedTexture, true, slot);
            if (base)
                object.textures.push_back(base);
            else
                createTexture2DSrv(baseColorAllocation->GetResource(),
                                   DXGI_FORMAT_R8G8B8A8_UNORM, slot);

            D3D12MA::Allocation* normal = uploadMaterialTexture(
                range.material->normalMapPath, range.material->embeddedNormalMap, false, slot + 1);
            if (normal)
                object.textures.push_back(normal);
            else
                createTexture2DSrv(normalMapAllocation->GetResource(),
                                   DXGI_FORMAT_R8G8B8A8_UNORM, slot + 1);

            fillSharedSlots(slot);

            if (D3D12MA::Allocation* orm =
                    uploadMaterialTexture(range.material->metallicRoughnessPath,
                                          range.material->embeddedMetallicRoughness, false,
                                          slot + 3))
                object.textures.push_back(orm);
        }
        object.subMeshes.push_back(sub);
    }

    // El reloj de animación arranca con el primer personaje: hasta entonces no
    // hay nada que avanzar, y dejarlo a cero daría un salto en el primer frame.
    if (skinnedObjects.empty()) {
        QueryPerformanceFrequency(&tickFrequency);
        QueryPerformanceCounter(&lastTick);
    }

    skinnedObjects.push_back(std::move(object));
    return static_cast<int>(skinnedObjects.size() - 1);
}

void D3D12Renderer::Impl::ensureSkinnedInstanceBuffer(size_t count)
{
    if (count == 0 || count <= skinnedInstanceCapacity[frameIndex])
        return;

    const size_t newCapacity = (std::max)(count, skinnedInstanceCapacity[frameIndex] * 2 + 16);

    if (skinnedInstanceAllocations[frameIndex]) {
        waitForGpu();
        if (skinnedInstanceMapped[frameIndex]) {
            skinnedInstanceAllocations[frameIndex]->GetResource()->Unmap(0, nullptr);
            skinnedInstanceMapped[frameIndex] = nullptr;
        }
        skinnedInstanceAllocations[frameIndex]->Release();
        skinnedInstanceAllocations[frameIndex] = nullptr;
    }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = newCapacity * sizeof(glm::mat4);
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

    throwIfFailed(allocator->CreateResource(&allocDesc, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr, &skinnedInstanceAllocations[frameIndex],
                                            IID_NULL, nullptr),
                  "D3D12MA::Allocator::CreateResource(instancias de personajes)");

    const D3D12_RANGE noRead{0, 0};
    throwIfFailed(skinnedInstanceAllocations[frameIndex]->GetResource()->Map(
                      0, &noRead, &skinnedInstanceMapped[frameIndex]),
                  "ID3D12Resource::Map(instancias de personajes)");
    skinnedInstanceCapacity[frameIndex] = newCapacity;
}

void D3D12Renderer::Impl::releaseSkinnedObjects()
{
    for (SkinnedObject& character : skinnedObjects) {
        for (D3D12MA::Allocation** allocation :
             {&character.posKeys, &character.rotKeys, &character.scaleKeys, &character.boneInfos,
              &character.inputVerts, &character.localXforms, &character.finalBones,
              &character.outputVerts, &character.indices}) {
            if (*allocation) {
                (*allocation)->Release();
                *allocation = nullptr;
            }
        }
        for (D3D12MA::Allocation* texture : character.textures)
            if (texture)
                texture->Release();
        character.textures.clear();
    }
    skinnedObjects.clear();
    nextSkinnedSlot = 0;
}

void D3D12Renderer::Impl::recordSkinning()
{
    // Barrera de UAV, no de transición: los pases no cambian de estado, solo
    // hay que garantizar que lo escrito por uno lo vea el siguiente.
    auto uavBarrier = [&](ID3D12Resource* resource) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = resource;
        commandList->ResourceBarrier(1, &barrier);
    };

    for (const SkinnedObject& object : skinnedObjects) {
        if (object.vertexCount == 0)
            continue;

        ComputePush push{};
        push.animTime    = object.animTime;
        push.boneCount   = object.boneCount;
        push.vertexCount = object.vertexCount;
        push.clipBase    = object.clipBase;

        const D3D12_GPU_VIRTUAL_ADDRESS posKeys     = object.posKeys->GetResource()->GetGPUVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS rotKeys     = object.rotKeys->GetResource()->GetGPUVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS scaleKeys   = object.scaleKeys->GetResource()->GetGPUVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS boneInfos   = object.boneInfos->GetResource()->GetGPUVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS localXforms = object.localXforms->GetResource()->GetGPUVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS finalBones  = object.finalBones->GetResource()->GetGPUVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS inputVerts  = object.inputVerts->GetResource()->GetGPUVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS outputVerts = object.outputVerts->GetResource()->GetGPUVirtualAddress();

        // 1) Claves de animación -> transformaciones locales. Un hilo por hueso.
        commandList->SetComputeRootSignature(boneEvalRootSignature.Get());
        commandList->SetPipelineState(boneEvalPipeline.Get());
        commandList->SetComputeRoot32BitConstants(0, sizeof(ComputePush) / 4, &push, 0);
        commandList->SetComputeRootShaderResourceView(1, posKeys);
        commandList->SetComputeRootShaderResourceView(2, rotKeys);
        commandList->SetComputeRootShaderResourceView(3, scaleKeys);
        commandList->SetComputeRootShaderResourceView(4, boneInfos);
        commandList->SetComputeRootUnorderedAccessView(5, localXforms);
        commandList->Dispatch((object.boneCount + 63) / 64, 1, 1);
        uavBarrier(object.localXforms->GetResource());

        // 2) Jerarquía: acumula padre a hijo. Un SOLO hilo a propósito — depende
        // de que el padre ya esté resuelto, y el orden topológico lo garantiza.
        commandList->SetComputeRootSignature(boneHierarchyRootSignature.Get());
        commandList->SetPipelineState(boneHierarchyPipeline.Get());
        commandList->SetComputeRoot32BitConstants(0, sizeof(ComputePush) / 4, &push, 0);
        commandList->SetComputeRootShaderResourceView(1, boneInfos);
        commandList->SetComputeRootShaderResourceView(2, localXforms);
        commandList->SetComputeRootUnorderedAccessView(3, finalBones);
        commandList->Dispatch(1, 1, 1);
        uavBarrier(object.finalBones->GetResource());

        // 3) Deformación de los vértices. Un hilo por vértice.
        commandList->SetComputeRootSignature(skinningRootSignature.Get());
        commandList->SetPipelineState(skinningPipeline.Get());
        commandList->SetComputeRoot32BitConstants(0, sizeof(ComputePush) / 4, &push, 0);
        commandList->SetComputeRootShaderResourceView(1, finalBones);
        commandList->SetComputeRootShaderResourceView(2, inputVerts);
        commandList->SetComputeRootUnorderedAccessView(3, outputVerts);
        commandList->Dispatch((object.vertexCount + 63) / 64, 1, 1);

        // De escritura por compute a entrada del ensamblador de vértices: aquí sí
        // cambia el uso del buffer, así que hace falta transición.
        D3D12_RESOURCE_BARRIER toVertexBuffer{};
        toVertexBuffer.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toVertexBuffer.Transition.pResource   = object.outputVerts->GetResource();
        toVertexBuffer.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toVertexBuffer.Transition.StateAfter  = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        toVertexBuffer.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &toVertexBuffer);
    }
}

void D3D12Renderer::Impl::releaseHdrTargets()
{
    if (viewportAllocation) {
        viewportAllocation->Release();
        viewportAllocation = nullptr;
    }
    for (auto* allocation : taaHistoryAllocations) {
        if (allocation)
            allocation->Release();
    }
    taaHistoryAllocations = {};
    taaHistoryValid       = false;

    for (auto** allocation : {&hdrMsAllocation, &depthMsAllocation}) {
        if (*allocation) {
            (*allocation)->Release();
            *allocation = nullptr;
        }
    }
    if (ssrAllocation) {
        ssrAllocation->Release();
        ssrAllocation = nullptr;
    }
    if (motionBlurAllocation) {
        motionBlurAllocation->Release();
        motionBlurAllocation = nullptr;
    }
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

    // Destino del trazado de reflejos: mismo formato y tamaño que la escena,
    // porque lo que guarda es color de la escena reproyectado.
    {
        D3D12_RESOURCE_DESC ssrDesc{};
        ssrDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        ssrDesc.Width            = width;
        ssrDesc.Height           = height;
        ssrDesc.DepthOrArraySize = 1;
        ssrDesc.MipLevels        = 1;
        ssrDesc.Format           = kHdrFormat;
        ssrDesc.SampleDesc.Count = 1;
        ssrDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        throwIfFailed(allocator->CreateResource(&allocDesc, &ssrDesc,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                &ssrAllocation, IID_NULL, nullptr),
                      "D3D12MA::Allocator::CreateResource(SSR)");

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format        = kHdrFormat;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(kUavSsr) * srvSize;
        device->CreateUnorderedAccessView(ssrAllocation->GetResource(), nullptr, &uavDesc, handle);

        createTexture2DSrv(ssrAllocation->GetResource(), kHdrFormat, kSrvSsr);
    }

    // Destino del motion blur: la misma escena emborronada, así que mismo
    // formato y tamaño. No lleva SRV porque de aquí solo sale una CopyResource.
    {
        D3D12_RESOURCE_DESC blurDesc{};
        blurDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        blurDesc.Width            = width;
        blurDesc.Height           = height;
        blurDesc.DepthOrArraySize = 1;
        blurDesc.MipLevels        = 1;
        blurDesc.Format           = kHdrFormat;
        blurDesc.SampleDesc.Count = 1;
        blurDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        throwIfFailed(allocator->CreateResource(&allocDesc, &blurDesc,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                &motionBlurAllocation, IID_NULL, nullptr),
                      "D3D12MA::Allocator::CreateResource(motion blur)");

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format        = kHdrFormat;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(kUavMotionBlur) * srvSize;
        device->CreateUnorderedAccessView(motionBlurAllocation->GetResource(), nullptr, &uavDesc,
                                          handle);
    }

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

    if (sampleCount > 1) {
        // Color y profundidad del pase de escena con MSAA. El resto de la
        // cadena (SSR, niebla, bloom, composición) sigue leyendo los de una
        // muestra: entre medias va un resolve.
        D3D12_RESOURCE_DESC msDesc{};
        msDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        msDesc.Width            = width;
        msDesc.Height           = height;
        msDesc.DepthOrArraySize = 1;
        msDesc.MipLevels        = 1;
        msDesc.Format           = kHdrFormat;
        msDesc.SampleDesc.Count = sampleCount;
        msDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE msClear{};
        msClear.Format = kHdrFormat;

        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        throwIfFailed(allocator->CreateResource(&allocDesc, &msDesc,
                                                D3D12_RESOURCE_STATE_RENDER_TARGET, &msClear,
                                                &hdrMsAllocation, IID_NULL, nullptr),
                      "D3D12MA::Allocator::CreateResource(HDR multimuestra)");

        D3D12_CPU_DESCRIPTOR_HANDLE msRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        msRtv.ptr += static_cast<SIZE_T>(kFrameCount + 2) * rtvSize;
        device->CreateRenderTargetView(hdrMsAllocation->GetResource(), nullptr, msRtv);

        msDesc.Format = DXGI_FORMAT_D32_FLOAT;
        msDesc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE depthClear{};
        depthClear.Format             = DXGI_FORMAT_D32_FLOAT;
        depthClear.DepthStencil.Depth = 1.0f;
        throwIfFailed(allocator->CreateResource(&allocDesc, &msDesc,
                                                D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
                                                &depthMsAllocation, IID_NULL, nullptr),
                      "D3D12MA::Allocator::CreateResource(profundidad multimuestra)");

        D3D12_DEPTH_STENCIL_VIEW_DESC msDsv{};
        msDsv.Format        = DXGI_FORMAT_D32_FLOAT;
        msDsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;

        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        dsvHandle.ptr += dsvSize;
        device->CreateDepthStencilView(depthMsAllocation->GetResource(), &msDsv, dsvHandle);
    }

    {
        // Imagen del viewport, del formato del backbuffer: es su sustituto. Va
        // al tamaño de SALIDA, no al de render: con SSAA la escena se dibuja más
        // grande y el pase de bajada la promedia hasta aquí.
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = outWidth;
        desc.Height           = outHeight;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        throwIfFailed(allocator->CreateResource(&allocDesc, &desc,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                                                &viewportAllocation, IID_NULL, nullptr),
                      "D3D12MA::Allocator::CreateResource(viewport)");

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(kFrameCount + 5) * rtvSize;
        device->CreateRenderTargetView(viewportAllocation->GetResource(), nullptr, rtv);

        createTexture2DSrv(viewportAllocation->GetResource(), DXGI_FORMAT_R8G8B8A8_UNORM,
                           kSrvViewport);
    }

    // Historial del TAA: dos imágenes del formato de la composición, que es lo
    // que el pase mezcla.
    for (UINT i = 0; i < 2; ++i) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = width;
        desc.Height           = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        throwIfFailed(allocator->CreateResource(&allocDesc, &desc,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                                                &taaHistoryAllocations[i], IID_NULL, nullptr),
                      "D3D12MA::Allocator::CreateResource(historial del TAA)");

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(kFrameCount + 3 + i) * rtvSize;
        device->CreateRenderTargetView(taaHistoryAllocations[i]->GetResource(), nullptr, rtv);

        createTexture2DSrv(taaHistoryAllocations[i]->GetResource(), DXGI_FORMAT_R8G8B8A8_UNORM,
                           kSrvTaaHistory + i);
    }

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

void D3D12Renderer::Impl::createNeutralIblCubes()
{
    // Medio flotante, que es el formato del IBL de verdad: así el mismo hueco
    // sirve luego para el resultado de los compute sin recrear la vista.
    auto uploadNeutralCube = [&](const float rgb[3], UINT srvIndex) {
        std::array<uint16_t, 4 * 6> texels{};
        for (UINT face = 0; face < 6; ++face) {
            texels[face * 4 + 0] = floatToHalf(rgb[0]);
            texels[face * 4 + 1] = floatToHalf(rgb[1]);
            texels[face * 4 + 2] = floatToHalf(rgb[2]);
            texels[face * 4 + 3] = floatToHalf(1.0f);
        }
        D3D12MA::Allocation* allocation =
            uploadTexture(texels.data(), 1, 1, 6, kHdrFormat, 8, srvIndex);
        createCubeSrv(allocation->GetResource(), kHdrFormat, 1, srvIndex);
        return allocation;
    };

    const float irradianceNeutral[3] = {0.075f, 0.080f, 0.090f};
    const float prefilterNeutral[3]  = {0.100f, 0.120f, 0.150f};
    irradianceAllocation = uploadNeutralCube(irradianceNeutral, kSrvIrradiance);
    prefilterAllocation  = uploadNeutralCube(prefilterNeutral, kSrvPrefilter);
}

void D3D12Renderer::Impl::recordIblConvolution(UINT sourceSrv, UINT irradianceUav,
                                               UINT prefilterUav, float intensity)
{
    if (!iblIrradiancePipeline || !iblPrefilterPipeline || !iblRootSignature)
        return;

    ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(iblRootSignature.Get());

    auto gpuHandle = [&](UINT index) {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = srvHeap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(index) * srvSize;
        return handle;
    };

    struct IblPush {
        float    roughness;
        uint32_t faceSize;
        float    intensity;
    };

    // Irradiancia: un dispatch para las seis caras a la vez (z = cara).
    {
        const IblPush push{0.0f, kIblIrradianceSize, intensity};
        commandList->SetPipelineState(iblIrradiancePipeline.Get());
        commandList->SetComputeRoot32BitConstants(0, 3, &push, 0);
        commandList->SetComputeRootDescriptorTable(1, gpuHandle(sourceSrv));
        commandList->SetComputeRootDescriptorTable(2, gpuHandle(irradianceUav));
        const UINT groups = (kIblIrradianceSize + 7) / 8;
        commandList->Dispatch(groups, groups, 6);
    }

    // Prefiltrado: un dispatch por mip, con su rugosidad y su tamaño.
    commandList->SetPipelineState(iblPrefilterPipeline.Get());
    for (UINT mip = 0; mip < kIblPrefilterMips; ++mip) {
        const UINT  size      = (std::max)(kIblPrefilterSize >> mip, 1u);
        const float roughness = static_cast<float>(mip) / static_cast<float>(kIblPrefilterMips - 1);
        const IblPush push{roughness, size, intensity};
        commandList->SetComputeRoot32BitConstants(0, 3, &push, 0);
        commandList->SetComputeRootDescriptorTable(1, gpuHandle(sourceSrv));
        commandList->SetComputeRootDescriptorTable(2, gpuHandle(prefilterUav + mip));
        const UINT groups = (size + 7) / 8;
        commandList->Dispatch(groups, groups, 6);
    }
}

void D3D12Renderer::Impl::precomputeIbl()
{
    // Sin cielo no hay nada que convolucionar: se quedan los neutros.
    if (!skyboxAllocation)
        return;

    // Los dos destinos, con el mismo formato que los neutros a los que
    // sustituyen. CUBE lo da la vista, no el recurso: para el compute es un
    // array de seis capas y para pbr.frag un TextureCube.
    auto createCubeTarget = [&](UINT size, UINT mips) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = size;
        desc.Height           = size;
        desc.DepthOrArraySize = 6;
        desc.MipLevels        = static_cast<UINT16>(mips);
        desc.Format           = kHdrFormat;
        desc.SampleDesc.Count = 1;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        D3D12MA::Allocation* allocation = nullptr;
        throwIfFailed(allocator->CreateResource(&allocDesc, &desc,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                &allocation, IID_NULL, nullptr),
                      "D3D12MA::Allocator::CreateResource(IBL)");
        return allocation;
    };

    if (irradianceAllocation) {
        irradianceAllocation->Release();
        irradianceAllocation = nullptr;
    }
    if (prefilterAllocation) {
        prefilterAllocation->Release();
        prefilterAllocation = nullptr;
    }
    irradianceAllocation = createCubeTarget(kIblIrradianceSize, 1);
    prefilterAllocation  = createCubeTarget(kIblPrefilterSize, kIblPrefilterMips);
    prefilterMips        = kIblPrefilterMips;

    // Un UAV por destino: el de irradiancia cubre sus seis capas; el del
    // prefiltrado va por mip, porque cada nivel es una rugosidad distinta y se
    // dispara por separado.
    auto createArrayUav = [&](ID3D12Resource* resource, UINT mip, UINT srvIndex) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format                      = kHdrFormat;
        uavDesc.ViewDimension               = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        uavDesc.Texture2DArray.MipSlice     = mip;
        uavDesc.Texture2DArray.ArraySize    = 6;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(srvIndex) * srvSize;
        device->CreateUnorderedAccessView(resource, nullptr, &uavDesc, handle);
    };

    createArrayUav(irradianceAllocation->GetResource(), 0, kUavIrradiance);
    for (UINT mip = 0; mip < kIblPrefilterMips; ++mip)
        createArrayUav(prefilterAllocation->GetResource(), mip, kUavPrefilter + mip);

    // Root signature común a los dos compute: el push de tres floats, el
    // cubemap del cielo en t0 y el destino en u1.
    if (!iblRootSignature) {
        D3D12_DESCRIPTOR_RANGE envRange{};
        envRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        envRange.NumDescriptors     = 1;
        envRange.BaseShaderRegister = 0;  // t0

        D3D12_DESCRIPTOR_RANGE outRange{};
        outRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        outRange.NumDescriptors     = 1;
        outRange.BaseShaderRegister = 1;  // u1

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = 3;  // roughness, faceSize, intensity

        params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges   = &envRange;

        params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges   = &outRange;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD         = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;  // s0

        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters     = _countof(params);
        rootDesc.pParameters       = params;
        rootDesc.NumStaticSamplers = 1;
        rootDesc.pStaticSamplers   = &sampler;

        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errorBlob;
        HRESULT          hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                          &serialized, &errorBlob);
        if (FAILED(hr)) {
            std::string detail;
            if (errorBlob)
                detail.assign(static_cast<const char*>(errorBlob->GetBufferPointer()),
                              errorBlob->GetBufferSize());
            throw std::runtime_error("D3D12: root signature del IBL (HRESULT " +
                                     hresultToString(hr) + ") " + detail);
        }
        throwIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                  serialized->GetBufferSize(),
                                                  IID_PPV_ARGS(&iblRootSignature)),
                      "ID3D12Device::CreateRootSignature(IBL)");

        auto buildCompute = [&](const char* path, ComPtr<ID3D12PipelineState>& out) {
            const std::vector<char>           code = readBinaryFile(path);
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = iblRootSignature.Get();
            desc.CS             = {code.data(), code.size()};
            throwIfFailed(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&out)),
                          "ID3D12Device::CreateComputePipelineState(IBL)");
        };
        buildCompute("shaders/ibl_irradiance.comp.dxil", iblIrradiancePipeline);
        buildCompute("shaders/ibl_prefilter.comp.dxil", iblPrefilterPipeline);
    }

    // Se graba y se espera aquí mismo: esto corre una vez al cargar el cielo,
    // no por frame, y el resto del init ya bloquea igual.
    throwIfFailed(allocators[frameIndex]->Reset(), "ID3D12CommandAllocator::Reset(IBL)");
    throwIfFailed(commandList->Reset(allocators[frameIndex].Get(), nullptr),
                  "ID3D12GraphicsCommandList::Reset(IBL)");

    ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(iblRootSignature.Get());

    auto gpuHandle = [&](UINT index) {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = srvHeap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(index) * srvSize;
        return handle;
    };

    // Los mismos dispatches que usa cada sonda: entrada, destinos e intensidad
    // por parametro, y el resto identico.
    recordIblConvolution(kSrvSkybox, kUavIrradiance, kUavPrefilter, 1.0f);

    // De destino de escritura a textura de lectura: pbr.frag los muestrea en el
    // pase de escena del mismo frame en adelante.
    D3D12_RESOURCE_BARRIER toShader[2]{};
    for (int i = 0; i < 2; ++i) {
        toShader[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toShader[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toShader[i].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toShader[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    toShader[0].Transition.pResource = irradianceAllocation->GetResource();
    toShader[1].Transition.pResource = prefilterAllocation->GetResource();
    commandList->ResourceBarrier(2, toShader);

    throwIfFailed(commandList->Close(), "ID3D12GraphicsCommandList::Close(IBL)");
    ID3D12CommandList* lists[] = {commandList.Get()};
    queue->ExecuteCommandLists(1, lists);
    waitForGpu();

    // Las vistas de lectura, ahora sobre los recursos nuevos: el bloque global
    // y el de cada objeto ya cargado.
    createCubeSrv(irradianceAllocation->GetResource(), kHdrFormat, 1, kSrvIrradiance);
    createCubeSrv(prefilterAllocation->GetResource(), kHdrFormat, prefilterMips, kSrvPrefilter);
    // Solo los dos huecos de entorno: rehacer el bloque entero pisaría el
    // metallic-roughness propio de cada malla con el neutro.
    auto refreshEnv = [&](UINT blockBase) {
        createCubeSrv(irradianceAllocation->GetResource(), kHdrFormat, 1, blockBase + 4);
        createCubeSrv(prefilterAllocation->GetResource(), kHdrFormat, prefilterMips,
                      blockBase + 5);
    };
    for (const StaticObject& object : objects)
        if (object.srvBase != kSrvBaseColor)
            refreshEnv(object.srvBase);
    for (const SkinnedObject& character : skinnedObjects)
        for (const SkinnedSubMesh& sub : character.subMeshes)
            if (sub.srvBase != kSrvBaseColor)
                refreshEnv(sub.srvBase);

}

void D3D12Renderer::Impl::createForwardPlusBuffers()
{
    // Forward+ apagado: mode = 0 y una rejilla de una celda. pbr.frag lee mode
    // antes que nada y se queda con el bucle sobre las luces del UBO, pero los
    // cuatro buffers tienen que estar enlazados igual.
    // Parámetros y luces van en heap de subida y mapeados: se reescriben cada
    // frame con la cámara y las luces vivas.
    auto createMapped = [&](UINT64 bytes, D3D12MA::Allocation** allocation, void** mapped) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = bytes;
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        throwIfFailed(allocator->CreateResource(&allocDesc, &desc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                allocation, IID_NULL, nullptr),
                      "D3D12MA::Allocator::CreateResource(Forward+)");
        const D3D12_RANGE noRead{0, 0};
        throwIfFailed((*allocation)->GetResource()->Map(0, &noRead, mapped),
                      "ID3D12Resource::Map(Forward+)");
    };

    createMapped(sizeof(FpParamsGpu), &fpParamsAllocation, &fpParamsMapped);
    createMapped(sizeof(FpLightGpu) * kFpMaxLights, &fpLightsAllocation, &fpLightsMapped);

    // Con Forward+ apagado no hay rejilla, pero los cuatro buffers tienen que
    // estar enlazados igual: pbr.frag los declara sin rama. Una celda basta.
    ensureForwardPlusGrid(1);

    // Y el bloque, escrito ya en Off: pbr.frag lee mode antes que nada.
    FpParamsGpu off{};
    off.gridX = off.gridY = off.gridZ = 1;
    off.tileSize   = kFpTileSize;
    off.maxPerCell = kFpMaxPerCell;
    off.zNear      = 0.1f;
    off.zFar       = 500.0f;
    std::memcpy(fpParamsMapped, &off, sizeof(off));
}

void D3D12Renderer::Impl::createSkyboxResources()
{
    // Mismo orden de caras que el camino de Vulkan: +X, -X, +Y, -Y, +Z, -Z,
    // que es el que espera un TextureCube por slice.
    const char* facePaths[6] = {
        "assets/skybox/px.png", "assets/skybox/nx.png", "assets/skybox/py.png",
        "assets/skybox/ny.png", "assets/skybox/pz.png", "assets/skybox/nz.png",
    };

    int      faceWidth = 0, faceHeight = 0, channels = 0;
    stbi_uc* faces[6] = {};
    bool     ok       = true;
    for (int i = 0; i < 6; ++i) {
        int w = 0, h = 0;
        faces[i] = stbi_load(facePaths[i], &w, &h, &channels, STBI_rgb_alpha);
        if (!faces[i]) {
            ok = false;
            break;
        }
        if (i == 0) {
            faceWidth  = w;
            faceHeight = h;
        } else if (w != faceWidth || h != faceHeight) {
            // Un cubemap con caras de distinto tamaño no es un cubemap: el
            // recurso es UNO con seis slices del mismo tamaño.
            ok = false;
            break;
        }
    }

    if (ok && faceWidth > 0 && faceHeight > 0) {
        // Las seis caras seguidas, que es como uploadTexture recorre el array.
        const size_t         faceBytes = static_cast<size_t>(faceWidth) * faceHeight * 4;
        std::vector<uint8_t> cube(faceBytes * 6);
        for (int i = 0; i < 6; ++i)
            std::memcpy(cube.data() + faceBytes * i, faces[i], faceBytes);

        skyboxAllocation = uploadTexture(cube.data(), static_cast<UINT>(faceWidth),
                                         static_cast<UINT>(faceHeight), 6,
                                         DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4, kSrvSkybox);

        // uploadTexture deja un SRV de array 2D; el shader declara TextureCube,
        // y con la vista de array la dirección de muestreo no significa nada.
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.TextureCube.MipLevels   = 1;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(kSrvSkybox) * srvSize;
        device->CreateShaderResourceView(skyboxAllocation->GetResource(), &srvDesc, handle);
    }

    for (stbi_uc* face : faces)
        if (face)
            stbi_image_free(face);

    if (!skyboxAllocation)
        return;

    createSkyboxPipelineOnly();
}

void D3D12Renderer::Impl::createSkyboxPipelineOnly()
{
    if (!skyboxAllocation)
        return;

    // Root signature: la invViewProj como root constants (b0) y el cubemap en
    // una tabla (t0). El vertex shader no lee vértices —saca las tres esquinas
    // del SV_VertexID—, así que no hay input layout.
    D3D12_DESCRIPTOR_RANGE cubeRange{};
    cubeRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    cubeRange.NumDescriptors     = 1;
    cubeRange.BaseShaderRegister = 0;  // t0

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;  // b0
    params[0].Constants.Num32BitValues = 16;
    params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;

    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &cubeRange;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister   = 0;  // s0
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters     = _countof(params);
    rootDesc.pParameters       = params;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers   = &sampler;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT          hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                      &serialized, &errorBlob);
    if (FAILED(hr)) {
        std::string detail;
        if (errorBlob)
            detail.assign(static_cast<const char*>(errorBlob->GetBufferPointer()),
                          errorBlob->GetBufferSize());
        throw std::runtime_error("D3D12: root signature del cielo (HRESULT " +
                                 hresultToString(hr) + ") " + detail);
    }
    throwIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                              serialized->GetBufferSize(),
                                              IID_PPV_ARGS(&skyboxRootSignature)),
                  "ID3D12Device::CreateRootSignature(cielo)");

    const std::vector<char> vertexShader = readBinaryFile("shaders/skybox.vert.dxil");
    const std::vector<char> pixelShader  = readBinaryFile("shaders/skybox.frag.dxil");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = skyboxRootSignature.Get();
    psoDesc.VS                    = {vertexShader.data(), vertexShader.size()};
    psoDesc.PS                    = {pixelShader.data(), pixelShader.size()};
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets      = 1;
    // El cielo va al target HDR, con la escena: así lo tonemapea la
    // composición como todo lo demás y puede generar bloom.
    psoDesc.RTVFormats[0]    = kHdrFormat;
    psoDesc.DSVFormat        = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = sampleCount;
    psoDesc.SampleMask       = UINT_MAX;

    psoDesc.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    for (auto& rt : psoDesc.BlendState.RenderTarget)
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // El triángulo sale con z = 1: se dibuja al final, solo donde no haya
    // geometría, y NO escribe profundidad — la niebla lee ese buffer y un
    // cielo a distancia máxima le haría teñir la pantalla entera.
    psoDesc.DepthStencilState.DepthEnable    = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    psoDesc.DepthStencilState.StencilEnable  = FALSE;

    throwIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&skyboxPipeline)),
                  "ID3D12Device::CreateGraphicsPipelineState(cielo)");
}

UINT D3D12Renderer::Impl::desiredSampleCount() const
{
    if (state->aaMode() != RendererState::AaMode::Msaa)
        return 1;

    // Lo que pida el usuario, recortado a lo que el device acepte para el
    // formato de la escena: pedir 8 donde solo hay 4 no falla al crear la
    // textura, falla al crear el pipeline, y ahí ya es tarde.
    UINT wanted = static_cast<UINT>((std::max)(1, state->msaaSamples()));
    while (wanted > 1) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{};
        levels.Format      = kHdrFormat;
        levels.SampleCount = wanted;
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels,
                                                  sizeof(levels))) &&
            levels.NumQualityLevels > 0)
            break;
        wanted /= 2;
    }
    return wanted;
}

void D3D12Renderer::Impl::applyPendingRenderSize()
{
    const UINT wantedOut =
        (renderToTexture && pendingRenderWidth > 0) ? pendingRenderWidth : swapWidth;
    const UINT wantedOutH =
        (renderToTexture && pendingRenderHeight > 0) ? pendingRenderHeight : swapHeight;
    if (wantedOut == 0 || wantedOutH == 0)
        return;

    // Y el tamaño de dibujo, que con SSAA es el de salida multiplicado por el
    // factor. El tope de una textura 2D en D3D12 son 16384 por lado: pedir más
    // no falla al crear el recurso, falla al usarlo.
    UINT wanted  = wantedOut;
    UINT wantedH = wantedOutH;
    if (state->aaMode() == RendererState::AaMode::Ssaa && ssaaFactor > 1.0f) {
        constexpr UINT kMaxTextureSide = 16384;
        wanted  = (std::min)(static_cast<UINT>(std::lround(wantedOut * ssaaFactor)), kMaxTextureSide);
        wantedH = (std::min)(static_cast<UINT>(std::lround(wantedOutH * ssaaFactor)), kMaxTextureSide);
    }

    if (wanted == width && wantedH == height && wantedOut == outWidth && wantedOutH == outHeight)
        return;

    // Todo lo interno es del tamaño de render: profundidad, escena, bloom,
    // oclusión, historial y el LDR. La imagen del panel es la excepción —va al
    // de salida— y la swapchain NO se toca.
    waitForGpu();
    width     = wanted;
    height    = wantedH;
    outWidth  = wantedOut;
    outHeight = wantedOutH;

    createDepthBuffer();
    if (hdrAllocation)
        createHdrTargets();
    if (ssaoRawAllocation) {
        createSsaoTargets();
        ssaoBlurNeedsUav = false;
    }
    updateViewProj();
    computeCascades();
}

void D3D12Renderer::Impl::applyPendingSampleCount()
{
    const UINT wanted = desiredSampleCount();
    if (wanted == sampleCount)
        return;

    // Cambia el número de muestras de los targets Y de todos los pipelines que
    // dibujan en ellos: hay que esperar a que la GPU suelte los viejos.
    waitForGpu();
    sampleCount = wanted;

    createHdrTargets();
    createMeshPipeline();
    createSkinningPipelines();
    createGizmoPipeline();
    createSkyboxPipelineOnly();

}

void D3D12Renderer::Impl::createForwardPlusPipelines()
{
    // t0 parámetros, t1 luces, u2 celdas, u3 índices, t4 profundidad, u5
    // estadísticas. Los buffers van como root SRV/UAV —son ByteAddressBuffer y
    // no necesitan descriptor— y la profundidad, que sí es textura, en tabla.
    D3D12_DESCRIPTOR_RANGE depthRange{};
    depthRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    depthRange.NumDescriptors     = 1;
    depthRange.BaseShaderRegister = 4;  // t4

    D3D12_ROOT_PARAMETER params[7]{};
    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = sizeof(FpPush) / 4;

    params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0;  // t0
    params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[2].Descriptor.ShaderRegister = 1;  // t1
    params[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[3].Descriptor.ShaderRegister = 2;  // u2
    params[4].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[4].Descriptor.ShaderRegister = 3;  // u3
    params[5].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[5].Descriptor.ShaderRegister = 5;  // u5

    params[6].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[6].DescriptorTable.NumDescriptorRanges = 1;
    params[6].DescriptorTable.pDescriptorRanges   = &depthRange;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter         = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD         = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 4;  // s4

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters     = _countof(params);
    rootDesc.pParameters       = params;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers   = &sampler;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT          hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                      &serialized, &errorBlob);
    if (FAILED(hr)) {
        std::string detail;
        if (errorBlob)
            detail.assign(static_cast<const char*>(errorBlob->GetBufferPointer()),
                          errorBlob->GetBufferSize());
        throw std::runtime_error("D3D12: root signature del culling (HRESULT " +
                                 hresultToString(hr) + ") " + detail);
    }
    throwIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                              serialized->GetBufferSize(),
                                              IID_PPV_ARGS(&fpCullRootSignature)),
                  "ID3D12Device::CreateRootSignature(culling)");

    auto buildCompute = [&](const char* path, ComPtr<ID3D12PipelineState>& out) {
        const std::vector<char>           code = readBinaryFile(path);
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = fpCullRootSignature.Get();
        desc.CS             = {code.data(), code.size()};
        throwIfFailed(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&out)),
                      "ID3D12Device::CreateComputePipelineState(culling)");
    };
    buildCompute("shaders/light_cull_tiled.comp.dxil", fpTiledPipeline);
    buildCompute("shaders/light_cull_clustered.comp.dxil", fpClusteredPipeline);
}

void D3D12Renderer::Impl::ensureForwardPlusGrid(uint32_t cells)
{
    if (cells == 0 || cells == fpCellCount)
        return;

    // Los de salida se rehacen: su tamaño depende de la rejilla, y la rejilla
    // del tamaño de la ventana y del modo.
    waitForGpu();
    for (auto** allocation : {&fpCellsAllocation, &fpIndicesAllocation, &fpStatsAllocation}) {
        if (*allocation) {
            (*allocation)->Release();
            *allocation = nullptr;
        }
    }

    auto createStorage = [&](UINT64 bytes) {
        return createStorageBuffer(bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    };
    fpCellsAllocation   = createStorage(static_cast<UINT64>(cells) * 2 * sizeof(uint32_t));
    fpIndicesAllocation = createStorage(static_cast<UINT64>(cells) * kFpMaxPerCell * sizeof(uint32_t));
    fpStatsAllocation   = createStorage(4 * sizeof(uint32_t));
    fpCellCount         = cells;
}

void D3D12Renderer::Impl::updateForwardPlus()
{
    if (!fpParamsMapped)
        return;

    const RendererState::FpMode mode = state->forwardPlusMode();
    const uint32_t tileSize = (mode == RendererState::FpMode::Clustered) ? kFpClusterTile : kFpTileSize;
    const uint32_t gridZ    = (mode == RendererState::FpMode::Clustered) ? kFpClusterSlices : 1u;
    const uint32_t gridX    = (width + tileSize - 1) / tileSize;
    const uint32_t gridY    = (height + tileSize - 1) / tileSize;

    ensureForwardPlusGrid(gridX * gridY * gridZ);

    // zNear y zFar salen de la propia proyección (RH_ZO): p22 = f/(n-f) y
    // p32 = f*n/(n-f). Sacarlos de ahí es lo que mantiene la rejilla pegada a
    // la cámara que se esté usando, sin duplicar sus planos en otro sitio.
    const glm::mat4 proj  = cameraProj();
    const float     p22   = proj[2][2];
    const float     p32   = proj[3][2];
    const float     zNear = (p22 != 0.0f) ? p32 / p22 : 0.1f;
    const float     zFar  = (p22 != -1.0f) ? p32 / (p22 + 1.0f) : 1000.0f;

    const uint32_t count =
        (std::min)(static_cast<uint32_t>(sceneLights.size()), kFpMaxLights);

    FpParamsGpu fp{};
    fp.mode       = static_cast<uint32_t>(mode);
    fp.gridX      = gridX;
    fp.gridY      = gridY;
    fp.gridZ      = gridZ;
    fp.tileSize   = tileSize;
    fp.maxPerCell = kFpMaxPerCell;
    fp.numLights  = count;
    fp.zNear      = zNear;
    fp.zFar       = zFar;
    // Inverso del reparto logarítmico del culling clustered:
    // slice = log2(z) * scale + bias.
    const float logRatio = std::log2((std::max)(zFar / zNear, 1.0001f));
    fp.sliceScale        = static_cast<float>(gridZ) / logRatio;
    fp.sliceBias         = -std::log2(zNear) * fp.sliceScale;
    std::memcpy(fpParamsMapped, &fp, sizeof(fp));

    if (fpLightsMapped && count > 0) {
        auto* dst = static_cast<FpLightGpu*>(fpLightsMapped);
        for (uint32_t i = 0; i < count; ++i) {
            const ShaderLight& light = sceneLights[i];
            const glm::vec3    world(light.position[0], light.position[1], light.position[2]);
            // El radio no viaja en la luz del UBO: se toma el alcance, que es lo
            // que el editor ya edita por luz (params.x).
            const float radius = light.params[0];
            const glm::vec3 view = glm::vec3(cameraView * glm::vec4(world, 1.0f));

            dst[i].posRadius = glm::vec4(world, radius);
            dst[i].color     = glm::vec4(light.color[0], light.color[1], light.color[2],
                                         light.color[3]);
            dst[i].viewPosR  = glm::vec4(view, radius);
            dst[i].direction = glm::vec4(light.direction[0], light.direction[1],
                                         light.direction[2], light.direction[3]);
            dst[i].params    = glm::vec4(light.params[0], light.params[1], light.params[2],
                                         light.params[3]);
        }
    }
}

void D3D12Renderer::Impl::recordForwardPlusCull()
{
    const RendererState::FpMode mode = state->forwardPlusMode();
    if (mode == RendererState::FpMode::Off || !fpCullRootSignature || !fpCellsAllocation)
        return;

    const bool clustered = (mode == RendererState::FpMode::Clustered);
    const uint32_t tileSize = clustered ? kFpClusterTile : kFpTileSize;
    const uint32_t gridX    = (width + tileSize - 1) / tileSize;
    const uint32_t gridY    = (height + tileSize - 1) / tileSize;
    const uint32_t gridZ    = clustered ? kFpClusterSlices : 1u;

    // El tiled reduce la profundidad del tile: necesita la del pre-pase, que ya
    // está grabada. El clustered no la lee, pero la declara igual.
    const bool depthReady = prepassDepthAllocation != nullptr;
    if (!clustered && !depthReady)
        return;

    if (fpListsInPixelState) {
        D3D12_RESOURCE_BARRIER backToUav[2]{};
        for (int i = 0; i < 2; ++i) {
            backToUav[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            backToUav[i].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            backToUav[i].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            backToUav[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        backToUav[0].Transition.pResource = fpCellsAllocation->GetResource();
        backToUav[1].Transition.pResource = fpIndicesAllocation->GetResource();
        commandList->ResourceBarrier(2, backToUav);
        fpListsInPixelState = false;
    }

    D3D12_RESOURCE_BARRIER depthToRead{};
    depthToRead.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    depthToRead.Transition.pResource   = prepassDepthAllocation->GetResource();
    depthToRead.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    depthToRead.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    depthToRead.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &depthToRead);

    ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(fpCullRootSignature.Get());
    commandList->SetPipelineState(clustered ? fpClusteredPipeline.Get() : fpTiledPipeline.Get());

    const glm::mat4 proj = cameraProj();
    FpPush          push{};
    push.p00     = proj[0][0];
    push.p11     = proj[1][1];
    push.p22     = proj[2][2];
    push.p32     = proj[3][2];
    push.screenW = width;
    push.screenH = height;
    commandList->SetComputeRoot32BitConstants(0, sizeof(FpPush) / 4, &push, 0);

    commandList->SetComputeRootShaderResourceView(
        1, fpParamsAllocation->GetResource()->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(
        2, fpLightsAllocation->GetResource()->GetGPUVirtualAddress());
    commandList->SetComputeRootUnorderedAccessView(
        3, fpCellsAllocation->GetResource()->GetGPUVirtualAddress());
    commandList->SetComputeRootUnorderedAccessView(
        4, fpIndicesAllocation->GetResource()->GetGPUVirtualAddress());
    commandList->SetComputeRootUnorderedAccessView(
        5, fpStatsAllocation->GetResource()->GetGPUVirtualAddress());

    D3D12_GPU_DESCRIPTOR_HANDLE depthTable = srvHeap->GetGPUDescriptorHandleForHeapStart();
    depthTable.ptr += static_cast<UINT64>(kSrvPrepassDepth) * srvSize;
    commandList->SetComputeRootDescriptorTable(6, depthTable);

    // Un grupo por celda: el tiled tiene un hilo por píxel del tile (16x16) y el
    // clustered reparte 4x4x4 celdas por grupo.
    if (clustered)
        commandList->Dispatch((gridX + 3) / 4, (gridY + 3) / 4, (gridZ + 3) / 4);
    else
        commandList->Dispatch(gridX, gridY, 1);

    // Las listas pasan a lectura del pase de escena, y la profundidad vuelve a
    // escritura para el frame siguiente.
    D3D12_RESOURCE_BARRIER toScene[3]{};
    toScene[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toScene[0].Transition.pResource   = fpCellsAllocation->GetResource();
    toScene[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toScene[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toScene[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    toScene[1]                      = toScene[0];
    toScene[1].Transition.pResource = fpIndicesAllocation->GetResource();

    toScene[2] = depthToRead;
    std::swap(toScene[2].Transition.StateBefore, toScene[2].Transition.StateAfter);
    commandList->ResourceBarrier(3, toScene);

    fpListsInPixelState = true;
}

void D3D12Renderer::Impl::createTaaPipeline()
{
    // t0 la imagen del frame, t1 el historial, t2 la profundidad. Cada una en
    // su tabla: viven en huecos del heap que no son contiguos.
    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    for (UINT i = 0; i < 3; ++i) {
        ranges[i].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors     = 1;
        ranges[i].BaseShaderRegister = i;
    }

    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = sizeof(TaaPush) / 4;
    for (UINT i = 0; i < 3; ++i) {
        params[1 + i].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[1 + i].DescriptorTable.pDescriptorRanges   = &ranges[i];
        params[1 + i].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    // La imagen y el historial se filtran —la reproyección cae entre píxeles—;
    // la profundidad no.
    D3D12_STATIC_SAMPLER_DESC samplers[3]{};
    for (UINT i = 0; i < 3; ++i) {
        samplers[i].Filter = (i == 2) ? D3D12_FILTER_MIN_MAG_MIP_POINT
                                      : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[i].AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].MaxLOD         = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters     = _countof(params);
    rootDesc.pParameters       = params;
    rootDesc.NumStaticSamplers = _countof(samplers);
    rootDesc.pStaticSamplers   = samplers;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT          hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                      &serialized, &errorBlob);
    if (FAILED(hr)) {
        std::string detail;
        if (errorBlob)
            detail.assign(static_cast<const char*>(errorBlob->GetBufferPointer()),
                          errorBlob->GetBufferSize());
        throw std::runtime_error("D3D12: root signature del TAA (HRESULT " + hresultToString(hr) +
                                 ") " + detail);
    }
    throwIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                              serialized->GetBufferSize(),
                                              IID_PPV_ARGS(&taaRootSignature)),
                  "ID3D12Device::CreateRootSignature(TAA)");

    const std::vector<char> vertexShader = readBinaryFile("shaders/fullscreen.vert.dxil");
    const std::vector<char> pixelShader  = readBinaryFile("shaders/taa.frag.dxil");

    // Dos destinos: el backbuffer y el historial de este frame, que es lo que
    // leerá el siguiente. El shader escribe los dos en la misma pasada.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = taaRootSignature.Get();
    psoDesc.VS                    = {vertexShader.data(), vertexShader.size()};
    psoDesc.PS                    = {pixelShader.data(), pixelShader.size()};
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets      = 2;
    psoDesc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.RTVFormats[1]         = DXGI_FORMAT_R8G8B8A8_UNORM;
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

    throwIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&taaPipeline)),
                  "ID3D12Device::CreateGraphicsPipelineState(TAA)");
}

void D3D12Renderer::Impl::recordTaa(D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv)
{
    if (!taaPipeline || !taaHistoryAllocations[0] || !taaHistoryAllocations[1])
        return;

    const UINT writeIndex = taaHistoryIndex;
    const UINT readIndex  = 1 - taaHistoryIndex;

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

    transition(taaHistoryAllocations[writeIndex]->GetResource(),
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(readableDepth(), readableDepthState(),
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_CPU_DESCRIPTOR_HANDLE historyRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    historyRtv.ptr += static_cast<SIZE_T>(kFrameCount + 3 + writeIndex) * rtvSize;

    const D3D12_CPU_DESCRIPTOR_HANDLE targets[2] = {backBufferRtv, historyRtv};
    commandList->OMSetRenderTargets(2, targets, FALSE, nullptr);

    // Altura negativa por lo mismo que la composición: fullscreen.vert saca la
    // uv del NDC dando por hecho la orientación de Vulkan.
    D3D12_VIEWPORT viewport{};
    viewport.TopLeftY = static_cast<float>(height);
    viewport.Width    = static_cast<float>(width);
    viewport.Height   = -static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetPipelineState(taaPipeline.Get());
    commandList->SetGraphicsRootSignature(taaRootSignature.Get());

    TaaPush push{};
    // Del clip de ESTE frame al del anterior, las dos sin jitter: el
    // desplazamiento de subpíxel es ruido de muestreo, no movimiento de cámara,
    // y meterlo aquí arrastraría el historial.
    push.reproject    = taaPrevViewProj * glm::inverse(taaCurrViewProj);
    push.invRes       = glm::vec2(1.0f / static_cast<float>(width),
                                  1.0f / static_cast<float>(height));
    push.feedback     = state->taaFeedback();
    push.historyValid = taaHistoryValid ? 1 : 0;
    commandList->SetGraphicsRoot32BitConstants(0, sizeof(TaaPush) / 4, &push, 0);

    auto gpuHandle = [&](UINT index) {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = srvHeap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(index) * srvSize;
        return handle;
    };
    commandList->SetGraphicsRootDescriptorTable(1, gpuHandle(kSrvLdr));
    commandList->SetGraphicsRootDescriptorTable(2, gpuHandle(kSrvTaaHistory + readIndex));
    commandList->SetGraphicsRootDescriptorTable(3, gpuHandle(readableDepthSrv()));

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);

    transition(taaHistoryAllocations[writeIndex]->GetResource(),
               D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(readableDepth(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               readableDepthState());

    // El historial de este frame es el que leerá el siguiente.
    taaHistoryIndex = readIndex;
    taaHistoryValid = true;
}

void D3D12Renderer::Impl::createSsrPipelines()
{
    D3D12_DESCRIPTOR_RANGE sceneRange{};
    sceneRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    sceneRange.NumDescriptors     = 1;
    sceneRange.BaseShaderRegister = 0;  // t0

    D3D12_DESCRIPTOR_RANGE depthRange{};
    depthRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    depthRange.NumDescriptors     = 1;
    depthRange.BaseShaderRegister = 1;  // t1

    D3D12_DESCRIPTOR_RANGE outputRange{};
    outputRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    outputRange.NumDescriptors     = 1;
    outputRange.BaseShaderRegister = 2;  // u2

    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = sizeof(SsrPush) / 4;

    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &sceneRange;

    params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges   = &depthRange;

    params[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges   = &outputRange;

    // s0 filtra —el rayo cae entre píxeles de la escena— y s1 no: interpolar
    // dos profundidades de superficies distintas da un valor que no existe.
    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    samplers[0].Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[1].Filter         = D3D12_FILTER_MIN_MAG_MIP_POINT;
    for (int i = 0; i < 2; ++i) {
        samplers[i].AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].MaxLOD         = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = static_cast<UINT>(i);
    }

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters     = _countof(params);
    rootDesc.pParameters       = params;
    rootDesc.NumStaticSamplers = _countof(samplers);
    rootDesc.pStaticSamplers   = samplers;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT          hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                      &serialized, &errorBlob);
    if (FAILED(hr)) {
        std::string detail;
        if (errorBlob)
            detail.assign(static_cast<const char*>(errorBlob->GetBufferPointer()),
                          errorBlob->GetBufferSize());
        throw std::runtime_error("D3D12: root signature del SSR (HRESULT " + hresultToString(hr) +
                                 ") " + detail);
    }
    throwIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                              serialized->GetBufferSize(),
                                              IID_PPV_ARGS(&ssrRootSignature)),
                  "ID3D12Device::CreateRootSignature(SSR)");

    auto buildCompute = [&](const char* path, ComPtr<ID3D12PipelineState>& out) {
        const std::vector<char>           code = readBinaryFile(path);
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = ssrRootSignature.Get();
        desc.CS             = {code.data(), code.size()};
        throwIfFailed(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&out)),
                      "ID3D12Device::CreateComputePipelineState(SSR)");
    };
    buildCompute("shaders/ssr.comp.dxil", ssrPipeline);
    buildCompute("shaders/ssr_resolve.comp.dxil", ssrResolvePipeline);
}

void D3D12Renderer::Impl::recordSsr()
{
    if (!ssrPipeline || !ssrAllocation || !hdrAllocation)
        return;

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

    const glm::mat4 proj = cameraProj();
    SsrPush         push{};
    push.projParams  = glm::vec4(proj[0][0], proj[1][1], proj[2][2], proj[3][2]);
    push.invRes      = glm::vec2(1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height));
    push.maxDistance = state->ssrMaxDistance();
    push.thickness   = state->ssrThickness();
    push.maxSteps    = state->ssrMaxSteps();
    // El refinado no tiene ajuste propio en el estado compartido: son pasos de
    // bisección sobre el último tramo, y con menos de cuatro el borde del
    // reflejo se escalona.
    push.refineSteps = 5;
    push.edgeFade    = state->ssrEdgeFade();
    push.intensity   = state->ssrIntensity();

    const UINT groupsX = (width + 7) / 8;
    const UINT groupsY = (height + 7) / 8;

    ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);

    // Trazado: la escena ya dibujada como textura, la profundidad del pase de
    // escena (la completa, no la del pre-pase) y el destino propio.
    transition(hdrAllocation->GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(readableDepth(), readableDepthState(),
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    commandList->SetComputeRootSignature(ssrRootSignature.Get());
    commandList->SetPipelineState(ssrPipeline.Get());
    commandList->SetComputeRoot32BitConstants(0, sizeof(SsrPush) / 4, &push, 0);
    commandList->SetComputeRootDescriptorTable(1, gpuHandle(kSrvSceneHdr));
    commandList->SetComputeRootDescriptorTable(2, gpuHandle(readableDepthSrv()));
    commandList->SetComputeRootDescriptorTable(3, gpuHandle(kUavSsr));
    commandList->Dispatch(groupsX, groupsY, 1);

    // Resolve: el reflejo se suma sobre la escena, que vuelve a ser destino de
    // escritura.
    transition(ssrAllocation->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(hdrAllocation->GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    commandList->SetPipelineState(ssrResolvePipeline.Get());
    commandList->SetComputeRoot32BitConstants(0, sizeof(SsrPush) / 4, &push, 0);
    commandList->SetComputeRootDescriptorTable(1, gpuHandle(kSrvSsr));
    commandList->SetComputeRootDescriptorTable(2, gpuHandle(readableDepthSrv()));
    commandList->SetComputeRootDescriptorTable(3, gpuHandle(kUavSceneHdr));
    commandList->Dispatch(groupsX, groupsY, 1);

    // Y todo como estaba: la niebla, que va detrás, espera encontrar la escena
    // como render target y la profundidad en escritura.
    transition(ssrAllocation->GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(hdrAllocation->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(readableDepth(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               readableDepthState());
}

bool D3D12Renderer::Impl::motionBlurActive() const
{
    // Menos de dos taps no promedia nada: el resultado sería el píxel central y
    // la copia de vuelta escribiría la misma imagen con el coste de un dispatch
    // entero.
    return state->motionBlurEnabled() && state->motionBlurSamples() >= 2;
}

void D3D12Renderer::Impl::createMotionBlurPipeline()
{
    // Mismo reparto que el SSR: la escena en t0, la profundidad en t1 y el
    // destino en u2, que es como spirv-cross traduce los bindings 0, 1 y 2 del
    // set 0.
    D3D12_DESCRIPTOR_RANGE sceneRange{};
    sceneRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    sceneRange.NumDescriptors     = 1;
    sceneRange.BaseShaderRegister = 0;  // t0

    D3D12_DESCRIPTOR_RANGE depthRange{};
    depthRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    depthRange.NumDescriptors     = 1;
    depthRange.BaseShaderRegister = 1;  // t1

    D3D12_DESCRIPTOR_RANGE outputRange{};
    outputRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    outputRange.NumDescriptors     = 1;
    outputRange.BaseShaderRegister = 2;  // u2

    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = sizeof(MotionBlurPush) / 4;

    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &sceneRange;

    params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges   = &depthRange;

    params[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges   = &outputRange;

    // s0 filtra —los taps caen entre píxeles— y s1 no: interpolar dos
    // profundidades de superficies distintas da un valor que no existe. CLAMP
    // para que un tap del borde no traiga color del lado opuesto.
    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    samplers[0].Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[1].Filter         = D3D12_FILTER_MIN_MAG_MIP_POINT;
    for (int i = 0; i < 2; ++i) {
        samplers[i].AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].MaxLOD         = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = static_cast<UINT>(i);
    }

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters     = _countof(params);
    rootDesc.pParameters       = params;
    rootDesc.NumStaticSamplers = _countof(samplers);
    rootDesc.pStaticSamplers   = samplers;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT          hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                      &serialized, &errorBlob);
    if (FAILED(hr)) {
        std::string detail;
        if (errorBlob)
            detail.assign(static_cast<const char*>(errorBlob->GetBufferPointer()),
                          errorBlob->GetBufferSize());
        throw std::runtime_error("D3D12: root signature del motion blur (HRESULT " +
                                 hresultToString(hr) + ") " + detail);
    }
    throwIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                              serialized->GetBufferSize(),
                                              IID_PPV_ARGS(&motionBlurRootSignature)),
                  "ID3D12Device::CreateRootSignature(motion blur)");

    const std::vector<char>           code = readBinaryFile("shaders/motion_blur.comp.dxil");
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = motionBlurRootSignature.Get();
    desc.CS             = {code.data(), code.size()};
    throwIfFailed(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&motionBlurPipeline)),
                  "ID3D12Device::CreateComputePipelineState(motion blur)");
}

void D3D12Renderer::Impl::recordMotionBlur()
{
    if (!motionBlurPipeline || !motionBlurAllocation || !hdrAllocation)
        return;

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

    MotionBlurPush push{};
    // La MISMA matriz que reproyecta el TAA: clip de este frame (sin jitter) →
    // clip del anterior. Las dos view-proj se actualizan todos los frames, esté
    // el TAA activo o no.
    push.reproject = taaPrevViewProj * glm::inverse(taaCurrViewProj);
    push.invRes    = glm::vec2(1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height));
    push.intensity = state->motionBlurIntensity();
    push.maxRadius = state->motionBlurMaxRadius();
    push.samples   = state->motionBlurSamples();

    ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);

    transition(hdrAllocation->GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(readableDepth(), readableDepthState(),
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    commandList->SetComputeRootSignature(motionBlurRootSignature.Get());
    commandList->SetPipelineState(motionBlurPipeline.Get());
    commandList->SetComputeRoot32BitConstants(0, sizeof(MotionBlurPush) / 4, &push, 0);
    commandList->SetComputeRootDescriptorTable(1, gpuHandle(kSrvSceneHdr));
    commandList->SetComputeRootDescriptorTable(2, gpuHandle(readableDepthSrv()));
    commandList->SetComputeRootDescriptorTable(3, gpuHandle(kUavMotionBlur));
    commandList->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

    // La copia de vuelta, y no un segundo dispatch: es una copia 1:1 de la
    // imagen entera, que es lo que mejor hace el hardware.
    transition(motionBlurAllocation->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(hdrAllocation->GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COPY_DEST);

    commandList->CopyResource(hdrAllocation->GetResource(),
                              motionBlurAllocation->GetResource());

    // Y todo como estaba: el bloom espera encontrar la escena como render target
    // y la profundidad en escritura.
    transition(motionBlurAllocation->GetResource(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(hdrAllocation->GetResource(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(readableDepth(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               readableDepthState());
}

void D3D12Renderer::Impl::createSsaoPipelines()
{
    auto serialize = [&](const D3D12_ROOT_SIGNATURE_DESC& desc, ComPtr<ID3D12RootSignature>& out,
                         const char* what) {
        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errorBlob;
        HRESULT          hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                          &serialized, &errorBlob);
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

    // ── Pre-pase de profundidad ───────────────────────────────────────────
    // depth_prepass.vert declara el UBO recortado a view y proj —std140 los
    // deja en los mismos offsets—, y saca el model del buffer de instancias.
    {
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

        params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor.ShaderRegister = 0;
        params[1].Descriptor.RegisterSpace  = 1;
        params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters = _countof(params);
        rootDesc.pParameters   = params;
        rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        serialize(rootDesc, depthPrepassRootSignature, "pre-pase de profundidad");
    }

    const std::vector<char> prepassVs = readBinaryFile("shaders/depth_prepass.vert.dxil");

    // Solo la posición: el shader no lee nada más, y así el mismo VS sirve para
    // los vértices del motor y para los que escribe el skinning, que difieren
    // en el tamaño de cada vértice pero no en dónde empieza.
    D3D12_INPUT_ELEMENT_DESC positionOnly[] = {
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
         0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature                        = depthPrepassRootSignature.Get();
    psoDesc.VS                                    = {prepassVs.data(), prepassVs.size()};
    psoDesc.InputLayout                           = {positionOnly, _countof(positionOnly)};
    psoDesc.PrimitiveTopologyType                 = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets                      = 0;
    psoDesc.DSVFormat                             = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count                      = 1;
    psoDesc.SampleMask                            = UINT_MAX;
    psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;
    psoDesc.DepthStencilState.DepthEnable         = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask      = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc           = D3D12_COMPARISON_FUNC_LESS;

    throwIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&depthPrepassPipeline)),
                  "ID3D12Device::CreateGraphicsPipelineState(pre-pase)");
    throwIfFailed(
        device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&depthPrepassSkinnedPipeline)),
        "ID3D12Device::CreateGraphicsPipelineState(pre-pase skinned)");

    // ── Los dos compute ───────────────────────────────────────────────────
    {
        D3D12_DESCRIPTOR_RANGE inputRange{};
        inputRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        inputRange.NumDescriptors     = 1;
        inputRange.BaseShaderRegister = 0;  // t0

        D3D12_DESCRIPTOR_RANGE outputRange{};
        outputRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        outputRange.NumDescriptors     = 1;
        outputRange.BaseShaderRegister = 1;  // u1

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = sizeof(SsaoPush) / 4;

        params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges   = &inputRange;

        params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges   = &outputRange;

        // La profundidad se muestrea sin filtrar: interpolar dos profundidades
        // de superficies distintas da un valor que no está en ninguna.
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter         = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD         = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;  // s0

        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters     = _countof(params);
        rootDesc.pParameters       = params;
        rootDesc.NumStaticSamplers = 1;
        rootDesc.pStaticSamplers   = &sampler;
        serialize(rootDesc, ssaoRootSignature, "SSAO");
    }

    auto buildCompute = [&](const char* path, ComPtr<ID3D12PipelineState>& out) {
        const std::vector<char>           code = readBinaryFile(path);
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = ssaoRootSignature.Get();
        desc.CS             = {code.data(), code.size()};
        throwIfFailed(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&out)),
                      "ID3D12Device::CreateComputePipelineState(SSAO)");
    };
    buildCompute("shaders/ssao.comp.dxil", ssaoPipeline);
    buildCompute("shaders/ssao_blur.comp.dxil", ssaoBlurPipeline);
}

void D3D12Renderer::Impl::releaseSsaoTargets()
{
    for (auto** allocation : {&prepassDepthAllocation, &ssaoRawAllocation, &ssaoBlurAllocation}) {
        if (*allocation) {
            (*allocation)->Release();
            *allocation = nullptr;
        }
    }
}

void D3D12Renderer::Impl::createSsaoTargets()
{
    releaseSsaoTargets();

    // Profundidad propia del pre-pase. TYPELESS porque el mismo recurso se
    // escribe como profundidad y se lee como textura.
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = width;
        desc.Height           = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_R32_TYPELESS;
        desc.SampleDesc.Count = 1;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format             = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil.Depth = 1.0f;

        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        throwIfFailed(allocator->CreateResource(&allocDesc, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                &clearValue, &prepassDepthAllocation, IID_NULL,
                                                nullptr),
                      "D3D12MA::Allocator::CreateResource(profundidad del pre-pase)");

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format        = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(prepassDepthAllocation->GetResource(), &dsvDesc,
                                       prepassDsvHeap->GetCPUDescriptorHandleForHeapStart());

        createTexture2DSrv(prepassDepthAllocation->GetResource(), DXGI_FORMAT_R32_FLOAT,
                           kSrvPrepassDepth);
    }

    // Mapa crudo y emborronado, a resolución completa como en Vulkan: pbr.frag
    // lo muestrea por coordenada de pantalla y da por hecho que es 1:1.
    auto createAoTarget = [&](UINT uavIndex, int srvIndex) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = width;
        desc.Height           = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        D3D12MA::Allocation* allocation = nullptr;
        throwIfFailed(allocator->CreateResource(&allocDesc, &desc,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                &allocation, IID_NULL, nullptr),
                      "D3D12MA::Allocator::CreateResource(SSAO)");

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format        = DXGI_FORMAT_R32_FLOAT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(uavIndex) * srvSize;
        device->CreateUnorderedAccessView(allocation->GetResource(), nullptr, &uavDesc, handle);

        if (srvIndex >= 0)
            createTexture2DSrv(allocation->GetResource(), DXGI_FORMAT_R32_FLOAT,
                               static_cast<UINT>(srvIndex));
        return allocation;
    };

    ssaoRawAllocation  = createAoTarget(kUavSsaoRaw, kSrvSsaoRaw);
    ssaoBlurAllocation = createAoTarget(kUavSsaoBlur, -1);

    // Y t7 de cada bloque, por writeAoSlot y NO apuntando al mapa a pelo: con el
    // SSAO apagado ese mapa no se escribe nunca y vale cero, que multiplicado al
    // ambiente lo apaga entero. Esto corre al arrancar y en CADA redimensionado,
    // así que ponerlo a mano dejaba los bloques en un estado que no se
    // correspondía con el interruptor.
    //
    // El bloque GLOBAL entra aquí: es el único que no pasa por fillSharedSlots,
    // y es el que usa el suelo del motor —el que se dibuja cuando la escena no
    // trae mallas—. Sin esto, ese suelo salía NEGRO tapando el cielo.
    writeAoSlot(kSrvBaseColor);
    for (const StaticObject& object : objects)
        if (object.srvBase != kSrvBaseColor)
            writeAoSlot(object.srvBase);
    for (const SkinnedObject& character : skinnedObjects)
        for (const SkinnedSubMesh& sub : character.subMeshes)
            if (sub.srvBase != kSrvBaseColor)
                writeAoSlot(sub.srvBase);

    // Y el estado que consulta refreshAoSlots para saber si hay algo que
    // cambiar: sin esto se quedaría creyendo que los bloques dicen otra cosa.
    aoSlotsUseMap = state->ssaoEnabled() && ssaoBlurAllocation != nullptr;
}

void D3D12Renderer::Impl::recordDepthPrepassAndSsao()
{
    // La profundidad del pre-pase tiene cuatro clientes: la oclusión, el
    // reparto de luces por tile, y —cuando la del pase de escena es
    // multimuestra y no se puede muestrear— la niebla y los reflejos. Se graba
    // si la quiere alguno; los dos dispatch de oclusión siguen atados al SSAO.
    if (!prepassDepthAllocation)
        return;

    const bool wantsSsao = state->ssaoEnabled();
    const bool wantsCull = state->forwardPlusMode() == RendererState::FpMode::Tiled;
    // Con MSAA, el contorno de la selección también la necesita: la del pase de
    // escena es multimuestra y no se puede emparejar con el target LDR sobre el
    // que se dibuja.
    // El motion blur es el quinto cliente: reproyecta esta misma profundidad al
    // frame anterior para sacar la velocidad de cada píxel.
    const bool wantsMultisampleDepth =
        sampleCount > 1 &&
        (state->fogEnabled() || state->ssrEnabled() || hasOutlineSelection() ||
         motionBlurActive());
    if (!wantsSsao && !wantsCull && !wantsMultisampleDepth)
        return;

    if (ssaoBlurNeedsUav) {
        D3D12_RESOURCE_BARRIER backToUav{};
        backToUav.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        backToUav.Transition.pResource   = ssaoBlurAllocation->GetResource();
        backToUav.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        backToUav.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        backToUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &backToUav);
        ssaoBlurNeedsUav = false;
    }

    // ── 1. Profundidad ────────────────────────────────────────────────────
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = prepassDsvHeap->GetCPUDescriptorHandleForHeapStart();
    commandList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    D3D12_VIEWPORT viewport{};
    viewport.Width    = static_cast<float>(width);
    viewport.Height   = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    commandList->RSSetViewports(1, &viewport);
    D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    commandList->RSSetScissorRects(1, &scissor);

    commandList->SetGraphicsRootSignature(depthPrepassRootSignature.Get());
    commandList->SetGraphicsRootConstantBufferView(
        0, sceneUboAllocations[frameIndex]->GetResource()->GetGPUVirtualAddress());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Mismo reparto que el de sombras: los dos quieren todo lo visible.
    if (!shadowBatches.empty() && sceneInstanceAllocations[frameIndex]) {
        commandList->SetPipelineState(depthPrepassPipeline.Get());
        for (const Batching::InstanceBatch& batch : shadowBatches) {
            const StaticObject& rep = objects[static_cast<size_t>(
                drawGroupRep[static_cast<size_t>(batch.sharedIndex)])];
            commandList->SetGraphicsRootShaderResourceView(1,
                                                           instanceAddress(batch.firstInstance));
            commandList->IASetVertexBuffers(0, 1, &rep.vertexBufferView);
            commandList->IASetIndexBuffer(&rep.indexBufferView);
            commandList->DrawIndexedInstanced(rep.indexCount, batch.instanceCount, 0, 0, 0);
        }
    }

    if (!skinnedObjects.empty() && skinnedInstanceAllocations[frameIndex]) {
        commandList->SetPipelineState(depthPrepassSkinnedPipeline.Get());
        for (size_t i = 0; i < skinnedObjects.size(); ++i) {
            const SkinnedObject& character = skinnedObjects[i];
            if (!character.visible || character.indexCount == 0)
                continue;
            commandList->SetGraphicsRootShaderResourceView(1, skinnedInstanceAddress(i));
            commandList->IASetVertexBuffers(0, 1, &character.vertexBufferView);
            commandList->IASetIndexBuffer(&character.indexBufferView);
            commandList->DrawIndexedInstanced(character.indexCount, 1, 0, 0, 0);
        }
    }

    if (!wantsSsao) {
        // Solo hacía falta la profundidad: los demás la leen por su cuenta.
        return;
    }

    // ── 2. Oclusión ───────────────────────────────────────────────────────
    D3D12_RESOURCE_BARRIER depthToRead{};
    depthToRead.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    depthToRead.Transition.pResource   = prepassDepthAllocation->GetResource();
    depthToRead.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    depthToRead.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    depthToRead.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &depthToRead);

    ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(ssaoRootSignature.Get());

    auto gpuHandle = [&](UINT index) {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = srvHeap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(index) * srvSize;
        return handle;
    };

    // Los cuatro coeficientes con los que el shader reconstruye la posición en
    // view space, sacados de la proyección de ESTE frame.
    const glm::mat4 proj = cameraProj();
    SsaoPush        push{};
    push.projParams = glm::vec4(proj[0][0], proj[1][1], proj[2][2], proj[3][2]);
    push.invRes     = glm::vec2(1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height));
    push.radius     = state->ssaoRadius();
    push.bias       = state->ssaoBias();
    push.intensity  = state->ssaoIntensity();
    push.power      = state->ssaoPower();

    const UINT groupsX = (width + 7) / 8;
    const UINT groupsY = (height + 7) / 8;

    commandList->SetPipelineState(ssaoPipeline.Get());
    commandList->SetComputeRoot32BitConstants(0, sizeof(SsaoPush) / 4, &push, 0);
    commandList->SetComputeRootDescriptorTable(1, gpuHandle(kSrvPrepassDepth));
    commandList->SetComputeRootDescriptorTable(2, gpuHandle(kUavSsaoRaw));
    commandList->Dispatch(groupsX, groupsY, 1);

    // El crudo pasa a entrada del blur.
    D3D12_RESOURCE_BARRIER rawToRead{};
    rawToRead.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rawToRead.Transition.pResource   = ssaoRawAllocation->GetResource();
    rawToRead.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    rawToRead.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    rawToRead.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &rawToRead);

    commandList->SetPipelineState(ssaoBlurPipeline.Get());
    commandList->SetComputeRoot32BitConstants(0, sizeof(SsaoPush) / 4, &push, 0);
    commandList->SetComputeRootDescriptorTable(1, gpuHandle(kSrvSsaoRaw));
    commandList->SetComputeRootDescriptorTable(2, gpuHandle(kUavSsaoBlur));
    commandList->Dispatch(groupsX, groupsY, 1);

    // Y a leerlo el pase de escena. Los tres vuelven a su estado de partida
    // para que el frame siguiente encuentre lo mismo que este.
    D3D12_RESOURCE_BARRIER toScene[3]{};
    toScene[0] = rawToRead;
    std::swap(toScene[0].Transition.StateBefore, toScene[0].Transition.StateAfter);

    toScene[1].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toScene[1].Transition.pResource   = ssaoBlurAllocation->GetResource();
    toScene[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toScene[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toScene[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    toScene[2] = depthToRead;
    std::swap(toScene[2].Transition.StateBefore, toScene[2].Transition.StateAfter);
    commandList->ResourceBarrier(3, toScene);

    // El emborronado se queda como lectura durante el pase de escena; el frame
    // siguiente lo devuelve a escritura antes de volver a dispararlo.
    ssaoBlurNeedsUav = true;
}

void D3D12Renderer::Impl::bindForwardPlus()
{
    if (!fpParamsAllocation)
        return;
    commandList->SetGraphicsRootShaderResourceView(
        4, fpParamsAllocation->GetResource()->GetGPUVirtualAddress());
    commandList->SetGraphicsRootShaderResourceView(
        5, fpLightsAllocation->GetResource()->GetGPUVirtualAddress());
    commandList->SetGraphicsRootShaderResourceView(
        6, fpCellsAllocation->GetResource()->GetGPUVirtualAddress());
    commandList->SetGraphicsRootShaderResourceView(
        7, fpIndicesAllocation->GetResource()->GetGPUVirtualAddress());
}

void D3D12Renderer::Impl::recordSkybox()
{
    // En alambre el cielo sobra: taparía la geometría que se quiere ver por
    // dentro, y el camino de Vulkan también lo omite.
    if (!skyboxPipeline || state->isWireframeMode())
        return;

    // La vista SIN traslación: el cielo no se acerca al andar, solo gira.
    const glm::mat4 rotView     = glm::mat4(glm::mat3(cameraView));
    const glm::mat4 invViewProj = glm::inverse(cameraProj() * rotView);

    ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetPipelineState(skyboxPipeline.Get());
    commandList->SetGraphicsRootSignature(skyboxRootSignature.Get());
    commandList->SetGraphicsRoot32BitConstants(0, 16, &invViewProj[0][0], 0);

    D3D12_GPU_DESCRIPTOR_HANDLE table = srvHeap->GetGPUDescriptorHandleForHeapStart();
    table.ptr += static_cast<UINT64>(kSrvSkybox) * srvSize;
    commandList->SetGraphicsRootDescriptorTable(1, table);

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);
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

    // Bajada del SSAA: misma forma que el FXAA —triángulo de pantalla completa
    // leyendo la imagen ya compuesta— pero con otro push y otro shader. La
    // fuente es más grande que el destino y el shader promedia la huella de
    // cada píxel; el sampler no basta, que solo miraría los cuatro texeles del
    // centro.
    D3D12_ROOT_PARAMETER ssaaParams[2]{};
    ssaaParams[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    ssaaParams[0].Constants.ShaderRegister = 0;
    ssaaParams[0].Constants.Num32BitValues = sizeof(SsaaPush) / 4;
    ssaaParams[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

    ssaaParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    ssaaParams[1].DescriptorTable.NumDescriptorRanges = 1;
    ssaaParams[1].DescriptorTable.pDescriptorRanges   = &fxaaRange;
    ssaaParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC ssaaDesc{};
    ssaaDesc.NumParameters     = _countof(ssaaParams);
    ssaaDesc.pParameters       = ssaaParams;
    ssaaDesc.NumStaticSamplers = 1;
    ssaaDesc.pStaticSamplers   = &fxaaSampler;
    serializeAndCreate(ssaaDesc, ssaaRootSignature, "SSAA");

    const std::vector<char> ssaaPs = readBinaryFile("shaders/ssaa_resolve.frag.dxil");
    psoDesc.pRootSignature         = ssaaRootSignature.Get();
    psoDesc.PS                     = {ssaaPs.data(), ssaaPs.size()};
    throwIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&ssaaPipeline)),
                  "ID3D12Device::CreateGraphicsPipelineState(SSAA)");
}

void D3D12Renderer::Impl::createUiPipeline()
{
    // Root signature: la ortográfica como root constants (b0) y el atlas en una
    // tabla (t0). Todo lo demás —modo, grosor del contorno, colores— viaja por
    // vértice, que es lo que permite que el texto caiga en el mismo lote que el
    // panel que tiene detrás.
    D3D12_DESCRIPTOR_RANGE atlasRange{};
    atlasRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    atlasRange.NumDescriptors     = 1;
    atlasRange.BaseShaderRegister = 0;  // t0

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;  // b0
    params[0].Constants.Num32BitValues = 16;
    params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;

    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &atlasRange;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister   = 0;  // s0
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters     = _countof(params);
    rootDesc.pParameters       = params;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers   = &sampler;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errorBlob;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized,
                                           &errorBlob)))
        return;
    if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                           serialized->GetBufferSize(),
                                           IID_PPV_ARGS(&uiRootSignature))))
        return;

    const std::vector<char> vs = readBinaryFile("shaders/ui.vert.dxil");
    const std::vector<char> ps = readBinaryFile("shaders/ui.frag.dxil");
    if (vs.empty() || ps.empty())
        return;

    // El mismo UiVertex que arma el canvas: posición en píxeles, uv, color y los
    // dos vec4 que llevan el modo y el contorno.
    // Todas las semánticas son TEXCOORDn, incluida la posición: el HLSL sale de
    // traducir el SPIR-V y spirv-cross nombra las entradas por su location, no
    // por lo que signifiquen. Poner POSITION aquí crea el pipeline y deja el
    // atributo sin enlazar.
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(UiVertex, pos),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(UiVertex, uv),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(UiVertex, color),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(UiVertex, params),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(UiVertex, effect),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = uiRootSignature.Get();
    psoDesc.VS                    = {vs.data(), vs.size()};
    psoDesc.PS                    = {ps.data(), ps.size()};
    psoDesc.InputLayout           = {layout, _countof(layout)};
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    psoDesc.SampleDesc.Count      = 1;
    psoDesc.SampleMask            = UINT_MAX;

    psoDesc.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    // Alfa recto, no premultiplicado: el shader devuelve el color sin
    // multiplicar por el alfa, igual que en Vulkan.
    D3D12_RENDER_TARGET_BLEND_DESC& blend = psoDesc.BlendState.RenderTarget[0];
    blend.BlendEnable           = TRUE;
    blend.SrcBlend              = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp               = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha         = D3D12_BLEND_ONE;
    blend.DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.DepthStencilState.DepthEnable   = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    throwIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&uiPipeline)),
                  "ID3D12Device::CreateGraphicsPipelineState(UI 2D)");
}

bool D3D12Renderer::Impl::createProbeResources(GpuProbe& probe)
{
    if (probe.srvBase == 0)
        return false;

    // Un cubemap: seis capas de una textura 2D. CUBE lo dice la VISTA, no el
    // recurso —para el compute es un array y para pbr.frag un TextureCube—, y
    // por eso la misma imagen sirve para las dos cosas.
    auto createCube = [&](UINT size, UINT mips, D3D12_RESOURCE_STATES state,
                          D3D12_RESOURCE_FLAGS flags) -> D3D12MA::Allocation* {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = size;
        desc.Height           = size;
        desc.DepthOrArraySize = 6;
        desc.MipLevels        = static_cast<UINT16>(mips);
        desc.Format           = kHdrFormat;
        desc.SampleDesc.Count = 1;
        desc.Flags            = flags;

        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        D3D12MA::Allocation* allocation = nullptr;
        if (FAILED(allocator->CreateResource(&allocDesc, &desc, state, nullptr, &allocation,
                                             IID_NULL, nullptr)))
            return nullptr;
        return allocation;
    };

    // La captura es el destino de las seis pasadas de escena, así que nace como
    // render target; las otras dos las escriben los compute.
    probe.captureAllocation =
        createCube(kProbeFaceSize, 1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    probe.irradianceAllocation =
        createCube(kIblIrradianceSize, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    probe.prefilterAllocation =
        createCube(kIblPrefilterSize, kIblPrefilterMips, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    if (!probe.captureAllocation || !probe.irradianceAllocation || !probe.prefilterAllocation) {
        releaseProbe(probe);
        return false;
    }

    // Vistas de lectura: las tres como TextureCube, que es lo que muestrean el
    // compute de convolución (la captura) y pbr.frag (las otras dos).
    createCubeSrv(probe.captureAllocation->GetResource(), kHdrFormat, 1,
                  probe.srvBase + kProbeCaptureSrv);
    createCubeSrv(probe.irradianceAllocation->GetResource(), kHdrFormat, 1,
                  probe.srvBase + kProbeIrradianceSrv);
    createCubeSrv(probe.prefilterAllocation->GetResource(), kHdrFormat, kIblPrefilterMips,
                  probe.srvBase + kProbePrefilterSrv);

    // Y las de escritura, como array 2D: un UAV para la irradiancia y uno por
    // mip del prefiltrado.
    auto createArrayUav = [&](ID3D12Resource* resource, UINT mip, UINT index) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format                   = kHdrFormat;
        uavDesc.ViewDimension            = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        uavDesc.Texture2DArray.MipSlice  = mip;
        uavDesc.Texture2DArray.ArraySize = 6;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * srvSize;
        device->CreateUnorderedAccessView(resource, nullptr, &uavDesc, handle);
    };

    createArrayUav(probe.irradianceAllocation->GetResource(), 0,
                   probe.srvBase + kProbeIrradianceUav);
    for (UINT mip = 0; mip < kIblPrefilterMips; ++mip)
        createArrayUav(probe.prefilterAllocation->GetResource(), mip,
                       probe.srvBase + kProbePrefilterUav + mip);

    probe.baked = false;
    return true;
}

void D3D12Renderer::Impl::syncProbes()
{
    // Camino rápido: sin escena o sin sondas por ningún lado no se toca nada.
    // Es el caso de todas las escenas que no las usan.
    if (!scene && probes.empty())
        return;

    struct Desc {
        uint64_t  id;
        glm::vec3 position;
        float     radius;
        float     intensity;
    };
    std::vector<Desc> descs;
    if (scene) {
        scene->traverse([&](GameObject* go) {
            if (!go || !go->hasReflectionProbe())
                return;
            const auto& probe = go->getReflectionProbe();
            descs.push_back({go->id, glm::vec3(go->worldTransform[3]), probe->getRadius(),
                             probe->getIntensity()});
        });
    }
    if (descs.empty() && probes.empty())
        return;

    // Bajas: sondas cuyo GameObject ya no está. Se sueltan sus imágenes y su
    // hueco del heap queda libre para la siguiente.
    for (size_t i = probes.size(); i-- > 0;) {
        const uint64_t owner = probes[i].ownerId;
        const bool     alive =
            std::any_of(descs.begin(), descs.end(), [owner](const Desc& d) { return d.id == owner; });
        if (alive)
            continue;

        // ANTES de soltar sus imágenes: devolver al entorno global TODO lo que
        // la miraba. Si no, quedarían descriptores apuntando a memoria liberada
        // —y un SRV colgante no da error al crearse: mata el device después, sin
        // decir de qué—. Se reescriben todos porque al borrar del vector los
        // índices de las demás se desplazan, así que la asignación entera deja
        // de valer; el refresco de más abajo la recompone.
        for (const StaticObject& object : objects)
            writeProbeSlots(object.srvBase, -1);
        for (const SkinnedObject& character : skinnedObjects)
            for (const SkinnedSubMesh& sub : character.subMeshes)
                writeProbeSlots(sub.srvBase, -1);
        probeAssignStatic.assign(objects.size(), -1);
        probeAssignSkinned.assign(skinnedObjects.size(), -1);
        // La sonda entra en la clave del grupo de dibujo, y aquí se reasigna a
        // mano sin pasar por refreshProbeAssignment: sin esto los grupos se
        // quedarían partidos por una sonda que ya no existe. No se ve mal
        // —todos los bloques vuelven al IBL global, así que cada grupo sigue
        // siendo coherente—, pero son draws de más para siempre.
        drawGroupsDirty = true;

        releaseProbe(probes[i]);
        probes.erase(probes.begin() + static_cast<long>(i));
    }

    // Altas y actualizaciones.
    for (const Desc& desc : descs) {
        auto it = std::find_if(probes.begin(), probes.end(),
                               [&desc](const GpuProbe& p) { return p.ownerId == desc.id; });
        if (it == probes.end()) {
            if (probes.size() >= kMaxProbes)
                continue;  // pasado el tope, esos objetos se quedan con el IBL global

            // El primer bloque LIBRE, no el que toque por tamaño de la lista:
            // al borrar una sonda del medio el vector se compacta pero las
            // demás conservan su bloque, así que contar sondas daría un hueco
            // ya ocupado y las dos escribirían sobre los mismos descriptores.
            UINT slot = kMaxProbes;
            for (UINT candidate = 0; candidate < kMaxProbes; ++candidate) {
                const UINT base = kSrvProbes + candidate * kSrvPerProbe;
                const bool taken =
                    std::any_of(probes.begin(), probes.end(),
                                [base](const GpuProbe& p) { return p.srvBase == base; });
                if (!taken) {
                    slot = candidate;
                    break;
                }
            }
            if (slot >= kMaxProbes)
                continue;

            GpuProbe probe;
            probe.ownerId = desc.id;
            probe.srvBase = kSrvProbes + slot * kSrvPerProbe;
            if (!createProbeResources(probe))
                continue;

            probe.position  = desc.position;
            probe.radius    = desc.radius;
            probe.intensity = desc.intensity;
            probes.push_back(probe);
            continue;
        }

        // Mover la sonda o cambiar su radio invalida lo horneado: lo que
        // capturó era otra vista.
        if (it->position != desc.position || it->radius != desc.radius ||
            it->intensity != desc.intensity) {
            it->position   = desc.position;
            it->radius     = desc.radius;
            it->intensity  = desc.intensity;
            it->baked      = false;
            it->bakeFailed = false;
        }
    }

    // Peticiones de horneado. Se atienden AQUÍ, al principio del frame y antes
    // de grabar nada, porque hornear reescribe la cámara y el UBO y espera a la
    // GPU: a mitad de un frame sería grabar sobre lo ya grabado.
    //
    // Pedirlo a mano limpia también la marca de fallo: es la forma que tiene el
    // usuario de decir "vuelve a intentarlo".
    if (probeBakeAllQueued) {
        for (GpuProbe& probe : probes) {
            probe.baked      = false;
            probe.bakeFailed = false;
        }
        probeBakeAllQueued = false;
    }
    for (const uint64_t owner : probeBakeQueue) {
        auto it = std::find_if(probes.begin(), probes.end(),
                               [owner](const GpuProbe& p) { return p.ownerId == owner; });
        if (it != probes.end()) {
            it->baked      = false;
            it->bakeFailed = false;
        }
    }
    probeBakeQueue.clear();

    // Una por frame: seis pasadas de escena más la convolución es demasiado
    // para hacerlo de golpe con varias sondas, y así el editor sigue
    // respondiendo mientras se hornean.
    for (GpuProbe& probe : probes) {
        if (probe.baked || probe.bakeFailed)
            continue;
        bakeProbe(probe);
        break;
    }

    // Y quién mira a quién. Detrás del horneado: una sonda recién horneada ya
    // puede entrar, y una que se fue deja de estar en la lista.
    refreshProbeAssignment();
}

int D3D12Renderer::Impl::pickProbeFor(const glm::vec3& worldPos) const
{
    // La más cercana de las que lo alcanzan. Sin sonda que llegue, -1: ese
    // objeto se queda con el entorno global, que es lo que hacía siempre.
    int   best         = -1;
    float bestDistance = 0.0f;
    for (size_t i = 0; i < probes.size(); ++i) {
        // Una sonda sin hornear todavía tiene sus cubemaps en blanco: usarla
        // apagaría el reflejo del objeto hasta que termine.
        if (!probes[i].baked)
            continue;
        const float distance = glm::length(worldPos - probes[i].position);
        if (distance > probes[i].radius)
            continue;
        if (best < 0 || distance < bestDistance) {
            best         = static_cast<int>(i);
            bestDistance = distance;
        }
    }
    return best;
}

void D3D12Renderer::Impl::writeProbeSlots(UINT blockBase, int probeIndex)
{
    // t4 = irradiancia, t5 = prefiltrado. Mismos huecos que rellena
    // fillSharedSlots; aquí solo se cambia a qué imagen apuntan.
    if (probeIndex >= 0 && probeIndex < static_cast<int>(probes.size())) {
        const GpuProbe& probe = probes[static_cast<size_t>(probeIndex)];
        createCubeSrv(probe.irradianceAllocation->GetResource(), kHdrFormat, 1, blockBase + 4);
        createCubeSrv(probe.prefilterAllocation->GetResource(), kHdrFormat, kIblPrefilterMips,
                      blockBase + 5);
        return;
    }

    if (irradianceAllocation)
        createCubeSrv(irradianceAllocation->GetResource(), kHdrFormat, 1, blockBase + 4);
    if (prefilterAllocation)
        createCubeSrv(prefilterAllocation->GetResource(), kHdrFormat, prefilterMips, blockBase + 5);
}

int D3D12Renderer::Impl::refreshProbeAssignment()
{
    probeAssignStatic.resize(objects.size(), -1);
    probeAssignSkinned.resize(skinnedObjects.size(), -1);

    int changed = 0;

    // Los descriptores que se van a reescribir pueden estar en uso por el frame
    // en vuelo. Se espera UNA vez, y solo si de verdad hay algo que cambiar.
    bool waited    = false;
    auto ensureIdle = [&]() {
        if (!waited) {
            waitForGpu();
            waited = true;
        }
    };

    for (size_t i = 0; i < objects.size(); ++i) {
        // El CENTRO del objeto en mundo, no su origen: una malla larga con el
        // pivote fuera del radio se quedaría sin sonda por nada.
        const StaticObject& object = objects[i];
        const glm::vec3     center =
            object.hasBounds ? glm::vec3(object.transform *
                                     glm::vec4((object.aabbMin + object.aabbMax) * 0.5f, 1.0f))
                             : glm::vec3(object.transform[3]);

        const int wanted = pickProbeFor(center);
        if (probeAssignStatic[i] == wanted)
            continue;

        ensureIdle();
        probeAssignStatic[i] = wanted;
        writeProbeSlots(object.srvBase, wanted);
        // La sonda entra en la clave del grupo de dibujo: dos objetos con la
        // misma malla y distinta sonda ya no pueden compartir draw.
        drawGroupsDirty = true;
        ++changed;
    }

    for (size_t i = 0; i < skinnedObjects.size(); ++i) {
        const SkinnedObject& character = skinnedObjects[i];
        const int            wanted    = pickProbeFor(glm::vec3(character.transform[3]));
        if (probeAssignSkinned[i] == wanted)
            continue;

        ensureIdle();
        probeAssignSkinned[i] = wanted;
        // Un personaje tiene un bloque por submalla y todas miran la misma
        // sonda: el objeto es uno solo.
        for (const SkinnedSubMesh& sub : character.subMeshes)
            writeProbeSlots(sub.srvBase, wanted);
        ++changed;
    }

    return changed;
}

void D3D12Renderer::Impl::createProbeDepth()
{
    if (probeDepthAllocation)
        return;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = kProbeFaceSize;
    desc.Height           = kProbeFaceSize;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear{};
    clear.Format             = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;

    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(allocator->CreateResource(&allocDesc, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                         &clear, &probeDepthAllocation, IID_NULL, nullptr)))
        return;

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format        = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

    D3D12_CPU_DESCRIPTOR_HANDLE handle = dsvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(2) * dsvSize;  // 0 escena, 1 multimuestra, 2 sondas
    device->CreateDepthStencilView(probeDepthAllocation->GetResource(), &dsvDesc, handle);
}

void D3D12Renderer::Impl::bakeProbe(GpuProbe& probe)
{
    if (!probe.captureAllocation || !meshPipeline)
        return;

    createProbeDepth();
    if (!probeDepthAllocation)
        return;

    // Esto no es un pase del frame: se graba en su propia lista y se espera. La
    // captura reescribe el UBO de escena y la cámara, que el frame en vuelo
    // está usando.
    waitForGpu();

    // Direcciones y "up" de las seis caras. Los up van NEGADOS respecto a la
    // lista clásica de OpenGL y la proyección espeja X además de Y: dos espejos
    // son una rotación, así que el sentido de las caras —y con él el descarte
    // de caras traseras— se conserva y el cubemap sale con la orientación que
    // espera el muestreo. Es lo mismo que hizo falta para el cielo.
    static const glm::vec3 kDirs[6] = {
        {1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f},
    };
    static const glm::vec3 kUps[6] = {
        {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, -1.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f},
    };

    // Lo que se toca y hay que devolver: la cámara del frame y el tamaño de
    // render, que el pase de geometría usa para el viewport y el culling.
    const glm::mat4 savedView     = cameraView;
    const glm::vec3 savedPos      = cameraPos;
    const float     savedFov      = cameraFovDeg;
    const glm::mat4 savedViewProj = viewProj;

    // Un RTV por cara, en los seis huecos que el heap reserva al final.
    const UINT kProbeRtvBase = kFrameCount + 6;
    for (UINT face = 0; face < 6; ++face) {
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format                         = kHdrFormat;
        rtvDesc.ViewDimension                  = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Texture2DArray.FirstArraySlice = face;
        rtvDesc.Texture2DArray.ArraySize       = 1;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(kProbeRtvBase + face) * rtvSize;
        device->CreateRenderTargetView(probe.captureAllocation->GetResource(), &rtvDesc, handle);
    }

    ID3D12CommandAllocator* allocator = allocators[frameIndex].Get();

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

    auto beginList = [&]() {
        return SUCCEEDED(allocator->Reset()) && SUCCEEDED(commandList->Reset(allocator, nullptr));
    };
    auto submitAndWait = [&]() {
        if (FAILED(commandList->Close()))
            return false;
        ID3D12CommandList* lists[] = {commandList.Get()};
        queue->ExecuteCommandLists(1, lists);
        waitForGpu();
        return true;
    };

    D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
    dsv.ptr += static_cast<SIZE_T>(2) * dsvSize;

    const auto bakeStart = std::chrono::high_resolution_clock::now();

    // Una lista POR CARA, enviada y esperada antes de grabar la siguiente. Las
    // seis comparten el UBO de escena —una sola dirección de constant buffer— y
    // lo que la GPU lee es lo que haya en esa memoria cuando EJECUTA, no cuando
    // se grabó: de corrido, las seis caras salían con la cámara de la última y
    // el cubemap era seis copias de la misma vista. Vale igual para cualquier
    // buffer por frame que se reescriba entre caras.
    bool inRenderTarget = false;
    bool facesOk        = true;

    for (UINT face = 0; face < 6; ++face) {
        if (!beginList()) {
            facesOk = false;
            break;
        }

        if (face == 0)
            transition(probe.captureAllocation->GetResource(),
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);

        // 90° por cara y un rango generoso: la sonda ve toda la escena, no el
        // encuadre del jugador.
        cameraView       = glm::lookAtRH(probe.position, probe.position + kDirs[face], kUps[face]);
        cameraPos        = probe.position;
        cameraFovDeg = 90.0f;

        glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 0.1f, 20000.0f);
        proj[0][0] *= -1.0f;  // el espejo en X que compensa los "up" negados
        proj[1][1] *= -1.0f;
        probeFaceProj = proj;
        viewProj      = proj * cameraView;

        // El UBO con la vista de ESTA cara: es de donde pbr.frag saca la
        // posición del ojo y la proyección. Las luces y las cascadas se dejan
        // como están —el mapa de sombras que hay en la GPU es el de esas
        // matrices, y recalcularlas aquí lo descuadraría.
        updateSceneUbo();

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(kProbeRtvBase + face) * rtvSize;

        recordSceneGeometry(rtv, dsv, kProbeFaceSize, kProbeFaceSize);

        if (!submitAndWait()) {
            facesOk = false;
            break;
        }
        inRenderTarget = true;
    }

    // La vuelta a lectura se graba aunque una cara haya fallado: dejar el
    // cubemap en RENDER_TARGET descuadraría la barrera del siguiente horneado,
    // que lo espera en PIXEL_SHADER_RESOURCE.
    bool convolved = false;
    if (inRenderTarget && beginList()) {
        transition(probe.captureAllocation->GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // Y la convolución: los mismos dos compute del IBL global, pero leyendo
        // la captura de esta sonda y escribiendo en sus cubemaps.
        if (facesOk)
            recordIblConvolution(probe.srvBase + kProbeCaptureSrv,
                                 probe.srvBase + kProbeIrradianceUav,
                                 probe.srvBase + kProbePrefilterUav, probe.intensity);

        convolved = submitAndWait() && facesOk;
    }

    // Tiempo de pared, no de GPU: aquí se espera a que termine, así que la
    // espera ES el coste, y es lo que interesa saber al que hornea.
    probe.bakeMs = std::chrono::duration<float, std::milli>(
                       std::chrono::high_resolution_clock::now() - bakeStart)
                       .count();
    probeLastBakeMs = probe.bakeMs;

    // Y todo como estaba: el frame siguiente dibuja desde la cámara del jugador.
    cameraView       = savedView;
    cameraPos        = savedPos;
    cameraFovDeg = savedFov;
    viewProj         = savedViewProj;
    probeFaceProj.reset();
    updateSceneUbo();

    // Sin las seis caras y su convolución, la sonda NO queda horneada: marcarla
    // igual la dejaría en la lista de candidatas con sus cubemaps a medias, y
    // los objetos que le tocaran reflejarían eso.
    probe.baked      = convolved;
    probe.bakeFailed = !convolved;

}

void D3D12Renderer::Impl::releaseProbe(GpuProbe& probe)
{
    // Sus imágenes pueden estar en el frame en vuelo: quitar una sonda es un
    // evento raro (borrar el GameObject), así que esperar sale más barato que
    // llevar una lista de borrado diferido.
    waitForGpu();

    for (D3D12MA::Allocation** allocation :
         {&probe.captureAllocation, &probe.irradianceAllocation, &probe.prefilterAllocation}) {
        if (*allocation) {
            (*allocation)->Release();
            *allocation = nullptr;
        }
    }
    probe.baked = false;
}

bool D3D12Renderer::Impl::registerUiAtlas(UiTextureAtlas& atlas)
{
    if (atlas.sourcePixels().empty() || atlas.width() == 0 || atlas.height() == 0)
        return false;
    if (uiNextAtlasSlot >= kMaxUiAtlases)
        return false;

    // El formato lo decide el CONTENIDO: un atlas de sprites es color y va en
    // sRGB; el de una fuente son distancias y en sRGB saldría deformado sin que
    // la validación diga una palabra.
    const DXGI_FORMAT format = atlas.sourceIsSrgb() ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                    : DXGI_FORMAT_R8G8B8A8_UNORM;

    const UINT slot = kSrvUiAtlas + uiNextAtlasSlot;
    D3D12MA::Allocation* texture =
        uploadTexture(atlas.sourcePixels().data(), atlas.width(), atlas.height(), 1, format, 4,
                      slot);
    if (!texture)
        return false;

    uiAtlasTextures.push_back(texture);
    uiAtlasSrv[&atlas] = slot;
    ++uiNextAtlasSlot;
    return true;
}

void D3D12Renderer::Impl::ensureUiBuffers(UINT vertexCount, UINT indexCount)
{
    auto grow = [&](D3D12MA::Allocation*& allocation, void*& mapped, UINT& capacity, UINT needed,
                    UINT stride) {
        if (needed <= capacity && allocation)
            return;

        // Duplicando: reasignar cada frame por un vértice de más sería un
        // create/destroy por frame.
        UINT next = capacity ? capacity : 256;
        while (next < needed)
            next *= 2;

        if (allocation) {
            // Puede estar en uso por un frame anterior: crecer es raro (solo
            // cuando la UI se complica), así que esperar sale más barato que
            // llevar una lista de borrado diferido.
            waitForGpu();
            allocation->GetResource()->Unmap(0, nullptr);
            allocation->Release();
            allocation = nullptr;
            mapped     = nullptr;
        }

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = static_cast<UINT64>(next) * stride;
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        if (FAILED(allocator->CreateResource(&allocDesc, &desc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             &allocation, IID_NULL, nullptr)))
            return;

        if (FAILED(allocation->GetResource()->Map(0, nullptr, &mapped))) {
            // Recién creado y sin usar: se puede soltar sin esperar a nadie.
            allocation->Release();
            allocation = nullptr;
            return;
        }
        capacity = next;
    };

    grow(uiVertexAllocations[frameIndex], uiVertexMapped[frameIndex], uiVertexCapacity[frameIndex],
         vertexCount, sizeof(UiVertex));
    grow(uiIndexAllocations[frameIndex], uiIndexMapped[frameIndex], uiIndexCapacity[frameIndex],
         indexCount, sizeof(uint16_t));
}

void D3D12Renderer::Impl::recordUiCanvas(D3D12_CPU_DESCRIPTOR_HANDLE targetRtv)
{
    if (!uiPipeline)
        return;

    // Los quads, en CPU. Al tamaño de SALIDA: la UI se mide en píxeles de
    // pantalla, no en los de render, que con SSAA son otros.
    uiCanvas.buildDrawData(outWidth, outHeight, uiDrawData);

    if (uiDrawData.empty() || uiDrawData.vertices.empty() || uiDrawData.indices.empty())
        return;

    ensureUiBuffers(static_cast<UINT>(uiDrawData.vertices.size()),
                    static_cast<UINT>(uiDrawData.indices.size()));
    if (!uiVertexMapped[frameIndex] || !uiIndexMapped[frameIndex])
        return;

    std::memcpy(uiVertexMapped[frameIndex], uiDrawData.vertices.data(),
                uiDrawData.vertices.size() * sizeof(UiVertex));
    std::memcpy(uiIndexMapped[frameIndex], uiDrawData.indices.data(),
                uiDrawData.indices.size() * sizeof(uint16_t));

    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = uiVertexAllocations[frameIndex]->GetResource()->GetGPUVirtualAddress();
    vbv.SizeInBytes    = static_cast<UINT>(uiDrawData.vertices.size() * sizeof(UiVertex));
    vbv.StrideInBytes  = sizeof(UiVertex);

    D3D12_INDEX_BUFFER_VIEW ibv{};
    ibv.BufferLocation = uiIndexAllocations[frameIndex]->GetResource()->GetGPUVirtualAddress();
    ibv.SizeInBytes    = static_cast<UINT>(uiDrawData.indices.size() * sizeof(uint16_t));
    ibv.Format         = DXGI_FORMAT_R16_UINT;

    // Ortográfica en píxeles con el origen ARRIBA a la izquierda, que es como
    // vienen las posiciones. Sin voltear nada más: el viewport de salida ya va
    // con altura negativa en el resto de pases, así que aquí se pone recto.
    const glm::mat4 proj = glm::orthoRH_ZO(0.0f, static_cast<float>(outWidth),
                                           static_cast<float>(outHeight), 0.0f, -1.0f, 1.0f);

    D3D12_VIEWPORT viewport{};
    viewport.Width    = static_cast<float>(outWidth);
    viewport.Height   = static_cast<float>(outHeight);
    viewport.MaxDepth = 1.0f;

    const D3D12_GPU_DESCRIPTOR_HANDLE heapStart = srvHeap->GetGPUDescriptorHandleForHeapStart();
    auto gpuHandle = [&](UINT index) {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = heapStart;
        handle.ptr += static_cast<UINT64>(index) * srvSize;
        return handle;
    };

    ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->OMSetRenderTargets(1, &targetRtv, FALSE, nullptr);
    commandList->RSSetViewports(1, &viewport);
    commandList->SetPipelineState(uiPipeline.Get());
    commandList->SetGraphicsRootSignature(uiRootSignature.Get());
    commandList->SetGraphicsRoot32BitConstants(0, 16, &proj[0][0], 0);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &vbv);
    commandList->IASetIndexBuffer(&ibv);

    for (const UiBatch& batch : uiDrawData.batches) {
        if (batch.indexCount == 0)
            continue;

        // El recorte del nodo, en píxeles de pantalla. Un lote sin scissor
        // propio se recorta al viewport entero.
        D3D12_RECT scissor{0, 0, static_cast<LONG>(outWidth), static_cast<LONG>(outHeight)};
        if (!batch.scissor.empty()) {
            scissor.left   = batch.scissor.x;
            scissor.top    = batch.scissor.y;
            scissor.right  = batch.scissor.x + static_cast<LONG>(batch.scissor.width);
            scissor.bottom = batch.scissor.y + static_cast<LONG>(batch.scissor.height);
        }
        commandList->RSSetScissorRects(1, &scissor);

        // Sin atlas, la 1x1 blanca: multiplicar por (1,1,1,1) deja el color del
        // vértice tal cual, así que un panel plano no necesita ni pipeline
        // aparte. Con atlas, el suyo; y si no llegó a subirse, otra vez la
        // blanca —se verá el color plano en vez del sprite, pero no un
        // descriptor de otro.
        UINT srv = kSrvBaseColor;
        if (batch.atlas) {
            const auto it = uiAtlasSrv.find(batch.atlas);
            if (it != uiAtlasSrv.end())
                srv = it->second;
        }
        commandList->SetGraphicsRootDescriptorTable(1, gpuHandle(srv));
        commandList->DrawIndexedInstanced(batch.indexCount, 1, batch.firstIndex, 0, 0);
    }
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

    const glm::mat4  proj   = cameraProj();
    const glm::vec3  camPos = cameraPos;
    const glm::mat4& view   = cameraView;

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
    commandList->SetComputeRootDescriptorTable(3, gpuHandle(readableDepthSrv()));
    commandList->SetComputeRootDescriptorTable(4, gpuHandle(kSrvShadowMap));
    commandList->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

    // Y se devuelven a lo que espera el resto del frame.
    transition(readableDepth(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               readableDepthState());
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

    // El contorno de lo seleccionado, aquí y no en el pase de escena: sobre la
    // imagen ya tonemapeada su naranja llega plano, que es el que lo distingue
    // del amarillo de los colliders y del cian del frustum. De paso deja de
    // colarse en la captura de una sonda, que es geometría de la escena y no
    // decoración del editor.
    recordSelectionOutline(ldrRtv);

    // FXAA sobre el resultado ya compuesto, y de ahí al backbuffer.
    transition(ldrAllocation->GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // El pase final escribe al destino, que puede ser MÁS PEQUEÑO que lo que se
    // acaba de dibujar: con SSAA el viewport de salida es el del panel y el
    // shader promedia. Sin SSAA los dos tamaños coinciden y esto es lo de antes.
    D3D12_VIEWPORT outViewport{};
    outViewport.TopLeftY = static_cast<float>(outHeight);
    outViewport.Width    = static_cast<float>(outWidth);
    outViewport.Height   = -static_cast<float>(outHeight);
    outViewport.MaxDepth = 1.0f;
    const D3D12_RECT outScissor{0, 0, static_cast<LONG>(outWidth), static_cast<LONG>(outHeight)};
    commandList->RSSetViewports(1, &outViewport);
    commandList->RSSetScissorRects(1, &outScissor);

    markTimestamp(TsAa);
    if (state->aaMode() == RendererState::AaMode::Ssaa && ssaaPipeline) {
        // Bajada por promedio: una muestra por texel de origen y por eje, que es
        // lo que define el supersampling. A factor 2 son los cuatro texeles que
        // caen dentro del píxel de destino.
        SsaaPush ssaaPush{};
        ssaaPush.invSrc[0] = 1.0f / static_cast<float>(width);
        ssaaPush.invSrc[1] = 1.0f / static_cast<float>(height);
        ssaaPush.taps      = (std::max)(1, static_cast<int>(std::lround(ssaaFactor)));

        commandList->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);
        commandList->SetPipelineState(ssaaPipeline.Get());
        commandList->SetGraphicsRootSignature(ssaaRootSignature.Get());
        commandList->SetGraphicsRoot32BitConstants(0, sizeof(SsaaPush) / 4, &ssaaPush, 0);
        commandList->SetGraphicsRootDescriptorTable(1, gpuHandle(kSrvLdr));
        commandList->DrawInstanced(3, 1, 0, 0);

        taaHistoryValid = false;
    } else if (state->aaMode() == RendererState::AaMode::Taa) {
        // El TAA ocupa el sitio del FXAA: mezcla esta imagen con la del frame
        // anterior y escribe a la vez el backbuffer y el historial siguiente.
        recordTaa(backBufferRtv);
    } else {
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

        // Sin acumulación temporal el historial deja de valer: al volver a TAA
        // hay que empezar de cero o el primer frame mezcla una imagen vieja.
        taaHistoryValid = false;
    }
    markTimestamp(TsAa + 1);

    // UI del juego, encima de la escena ya compuesta y por debajo de la del
    // editor (que se graba después, sobre el backbuffer). Con el canvas vacío no
    // graba ni un comando.
    recordUiCanvas(backBufferRtv);

    transition(ldrAllocation->GetResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);

    // La interfaz va encima de todo, sobre el backbuffer y sin post-procesado:
    // suavizar los bordes del texto de la UI lo emborronaría. Con la escena en
    // textura la dibuja quien llama, DESPUÉS de que esa textura pase a lectura:
    // aquí todavía es el destino del pase.
    if (uiDrawCallback && !renderToTexture) {
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
    createShadowMapSrv(kSrvShadowMap);

    // Y la misma vista en la terna de cada objeto ya cargado: si esto se
    // rehiciera con escena en pantalla, sus t3 apuntarían al recurso viejo.
    for (const StaticObject& object : objects)
        if (object.srvBase != kSrvBaseColor)
            createShadowMapSrv(object.srvBase + 2);
    for (const SkinnedObject& character : skinnedObjects)
        for (const SkinnedSubMesh& sub : character.subMeshes)
            if (sub.srvBase != kSrvBaseColor)
                createShadowMapSrv(sub.srvBase + 2);

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

        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        // Se graban las caras TRASERAS, no las frontales. Es lo que evita que
        // una superficie se sombree a sí misma: la cara que se ilumina no entra
        // en el mapa, así que su profundidad no puede quedar por delante de sí
        // misma. Con caras frontales o sin culling, un plano grande —el suelo de
        // un proyecto -- sale entero en sombra por mucho sesgo que se le ponga.
        // A cambio, una malla abierta de una sola cara no proyecta sombra.
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
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

    const glm::mat4  proj = cameraProj();
    const glm::mat4& view = cameraView;

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

void D3D12Renderer::Impl::ensureSceneInstanceBuffer(size_t count)
{
    if (count == 0 || count <= sceneInstanceCapacity[frameIndex])
        return;

    // Se crece por bloques para no rehacer el buffer cada vez que entra una
    // malla al cargar una escena.
    const size_t newCapacity = (std::max)(count, sceneInstanceCapacity[frameIndex] * 2 + 64);

    if (sceneInstanceAllocations[frameIndex]) {
        // Puede estar en uso por el frame anterior.
        waitForGpu();
        if (sceneInstanceMapped[frameIndex]) {
            sceneInstanceAllocations[frameIndex]->GetResource()->Unmap(0, nullptr);
            sceneInstanceMapped[frameIndex] = nullptr;
        }
        sceneInstanceAllocations[frameIndex]->Release();
        sceneInstanceAllocations[frameIndex] = nullptr;
    }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = newCapacity * sizeof(glm::mat4);
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

    throwIfFailed(allocator->CreateResource(&allocDesc, &desc,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            &sceneInstanceAllocations[frameIndex], IID_NULL,
                                            nullptr),
                  "D3D12MA::Allocator::CreateResource(instancias de escena)");

    const D3D12_RANGE noRead{0, 0};
    throwIfFailed(sceneInstanceAllocations[frameIndex]->GetResource()->Map(
                      0, &noRead, &sceneInstanceMapped[frameIndex]),
                  "ID3D12Resource::Map(instancias de escena)");
    sceneInstanceCapacity[frameIndex] = newCapacity;
}

D3D12_GPU_VIRTUAL_ADDRESS D3D12Renderer::Impl::instanceAddress(uint32_t index) const
{
    return sceneInstanceAllocations[frameIndex]->GetResource()->GetGPUVirtualAddress() +
           static_cast<UINT64>(index) * sizeof(glm::mat4);
}

bool D3D12Renderer::Impl::releaseStaticObject(StaticObject& object)
{
    // Un duplicado lleva COPIAS de los handles del dueño: soltarlas liberaría
    // dos veces el mismo recurso. Solo baja el recuento del dueño.
    if (!object.ownsGpu) {
        if (object.sharedMesh >= 0 && object.sharedMesh < static_cast<int>(objects.size()))
            --objects[static_cast<size_t>(object.sharedMesh)].sharedRefs;
        object.vertexAllocation     = nullptr;
        object.indexAllocation      = nullptr;
        object.baseColorAllocation  = nullptr;
        object.normalMapAllocation  = nullptr;
        object.metalRoughAllocation = nullptr;
        return false;
    }

    // El dueño con duplicados vivos NO puede soltar: los dejaría dibujando con
    // memoria liberada, que no lo avisa ni la capa de validación.
    if (--object.sharedRefs > 0)
        return false;

    for (D3D12MA::Allocation** allocation :
         {&object.vertexAllocation, &object.indexAllocation, &object.baseColorAllocation,
          &object.normalMapAllocation, &object.metalRoughAllocation}) {
        if (*allocation)
            (*allocation)->Release();
        *allocation = nullptr;
    }
    return true;
}

void D3D12Renderer::Impl::rebuildDrawGroups()
{
    drawGroupRep.clear();
    drawGroupsDirty = false;
    if (objects.empty())
        return;

    // La clave NO es solo la malla. Dos objetos que la comparten se pintan del
    // mismo draw, y un draw enlaza UN bloque de descriptores: solo pueden ir
    // juntos si el bloque de los dos dice lo mismo. Lo que puede diferir con la
    // misma malla es la sonda de reflexión (t4/t5) y el relleno de una textura
    // que no se pudo leer, así que los dos entran en la clave.
    //
    // El resto de lo por-objeto no hace falta aquí: metallic y roughness salen
    // del material, que ya está en la clave de contenido de la malla, y la
    // fuerza de SSR la parte el propio agrupado.
    std::unordered_map<uint64_t, int> byKey;
    byKey.reserve(objects.size());

    for (size_t i = 0; i < objects.size(); ++i) {
        StaticObject& object = objects[i];
        const int     probe  = i < probeAssignStatic.size() ? probeAssignStatic[i] : -1;
        const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(object.sharedMesh)) << 40) ^
                             (static_cast<uint64_t>(static_cast<uint32_t>(probe + 1)) << 24) ^
                             static_cast<uint64_t>(object.materialVariant);

        auto found = byKey.find(key);
        if (found != byKey.end()) {
            object.drawGroup = found->second;
            continue;
        }

        object.drawGroup = static_cast<int>(drawGroupRep.size());
        byKey.emplace(key, object.drawGroup);
        drawGroupRep.push_back(static_cast<int>(i));
    }
}

void D3D12Renderer::Impl::buildShadowBatches()
{
    shadowBatches.clear();
    if (objects.empty() || !sceneInstanceMapped[frameIndex])
        return;

    // Sin frustum: el pase de sombras dibuja las cuatro cascadas y el pre-pase
    // cubre la pantalla entera, así que los dos quieren TODO lo visible. La
    // fuerza de SSR se deja a 0 porque ninguno de los dos pinta color.
    batchCandidates.clear();
    batchCandidates.reserve(objects.size());
    for (const StaticObject& object : objects)
        batchCandidates.push_back({object.drawGroup,
                                   object.meshVisible && object.indexCount > 0, &object.transform,
                                   0.0f});

    auto* matrices = static_cast<glm::mat4*>(sceneInstanceMapped[frameIndex]);
    const uint32_t base = kInstanceRegionShadow * static_cast<uint32_t>(instanceRegionStride);
    Batching::buildInstanceBatches(batchCandidates.data(), batchCandidates.size(), matrices + base,
                                   static_cast<uint32_t>(instanceRegionStride), base,
                                   shadowBatches);
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
        //
        // Un draw por grupo de malla, con la vista de instancias apuntando al
        // principio del rango del grupo: de ahí saca shadow.vert su model.
        if (!shadowBatches.empty() && sceneInstanceAllocations[frameIndex]) {
            commandList->SetPipelineState(shadowPipeline.Get());

            for (const Batching::InstanceBatch& batch : shadowBatches) {
                const StaticObject& rep = objects[static_cast<size_t>(
                    drawGroupRep[static_cast<size_t>(batch.sharedIndex)])];
                commandList->SetGraphicsRootShaderResourceView(
                    2, instanceAddress(batch.firstInstance));
                commandList->IASetVertexBuffers(0, 1, &rep.vertexBufferView);
                commandList->IASetIndexBuffer(&rep.indexBufferView);
                commandList->DrawIndexedInstanced(rep.indexCount, batch.instanceCount, 0, 0, 0);
            }
        }

        if (!skinnedObjects.empty() && skinnedInstanceAllocations[frameIndex]) {
            commandList->SetPipelineState(shadowSkinnedPipeline.Get());

            for (size_t i = 0; i < skinnedObjects.size(); ++i) {
                const SkinnedObject& character = skinnedObjects[i];
                if (!character.visible || character.indexCount == 0)
                    continue;
                commandList->SetGraphicsRootShaderResourceView(2, skinnedInstanceAddress(i));
                commandList->IASetVertexBuffers(0, 1, &character.vertexBufferView);
                commandList->IASetIndexBuffer(&character.indexBufferView);
                commandList->DrawIndexedInstanced(character.indexCount, 1, 0, 0, 0);
            }
        }
    }

    D3D12_RESOURCE_BARRIER toShaderResource = toDepthWrite;
    toShaderResource.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    toShaderResource.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commandList->ResourceBarrier(1, &toShaderResource);
}

void D3D12Renderer::Impl::updateViewProj()
{
    viewProj = cameraProj() * cameraView;
}

void D3D12Renderer::Impl::resolveFrameCamera()
{
    // Por defecto manda la cámara de edición, que es la que empuja setCamera
    // cada frame. Se limpia SIEMPRE: al parar Play la vista tiene que volver
    // sola, sin guardar ni restaurar nada.
    sceneCameraProj.reset();

    // headless = juego exportado, que está SIEMPRE en Play y no tiene editor
    // que le empuje una cámara: si no se resuelve aquí, se queda con la vista
    // por defecto del backend y no mira nunca por la cámara de la escena.
    // Misma regla que Renderer::isPlaying() en el camino de Vulkan.
    const bool playing = headless || (uiLayer && uiLayer->isPlaying());
    if (!playing || !scene)
        return;

    GameObject* cam = scene->findCamera();
    if (!cam || !cam->hasCameraComponent())
        return;

    const auto& component = cam->getCameraComponent();

    // projectionMatrix trae el Y flip de Vulkan cocinado dentro. Aquí sobra:
    // este backend no invierte el eje (ver updateSceneUbo). Se deshace en vez
    // de rehacer la matriz a mano para que ortográfica, near/far y fov sigan
    // saliendo de un único sitio.
    glm::mat4 proj = component->projectionMatrix(viewportAspectRatio());
    proj[1][1] *= -1.0f;
    sceneCameraProj = proj;

    // La vista y el ojo también salen del componente. cameraView y cameraPos se
    // pisan sin más: el editor los vuelve a empujar en el frame siguiente, así
    // que no hay estado que restaurar.
    cameraView = CameraComponent::viewFromWorld(cam->worldTransform);
    cameraPos  = glm::vec3(cam->worldTransform[3]);
}

D3D12Renderer::D3D12Renderer() : m_impl(std::make_unique<Impl>())
{
    // El Impl consulta el estado a través de este puntero en vez de copiarlo:
    // así un setBloomIntensity() desde el editor se ve en el frame siguiente.
    m_impl->state = this;

    // Aquí NO se enciende ningún efecto. Los encendía —niebla y un bloom más
    // fuerte— cuando este backend tenía su propio bucle de prueba y quería
    // enseñarlos; detrás del editor eso pisa lo que diga el proyecto, y una
    // escena real con la niebla puesta y una sola luz lejana se ve NEGRA. El
    // default del motor es el de RendererState, igual que para Vulkan.
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
    // Al arrancar, el render es del tamaño de la ventana: no hay panel todavía,
    // ni SSAA que multiplique nada.
    d.swapWidth  = d.width;
    d.swapHeight = d.height;
    d.outWidth   = d.width;
    d.outHeight  = d.height;

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
    // Los de la swapchain, el HDR, el LDR de la composición y el color
    // multimuestra del pase de escena cuando hay MSAA.
    // + historias del TAA, viewport y las seis caras del horneado de sondas.
    rtvHeapDesc.NumDescriptors = kFrameCount + 6 + 6;
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
    // La profundidad de siempre y la multimuestra.
    dsvHeapDesc.NumDescriptors = 3;  // escena, escena multimuestra y caras de sonda
    dsvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    throwIfFailed(d.device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&d.dsvHeap)),
                  "ID3D12Device::CreateDescriptorHeap(DSV)");
    d.createDepthBuffer();

    d.createGizmoPipeline();
    d.createGridGeometry();
    d.createMeshPipeline();
    d.createMeshResources();
    d.createForwardPlusBuffers();
    D3D12_DESCRIPTOR_HEAP_DESC prepassDsvDesc{};
    prepassDsvDesc.NumDescriptors = 1;
    prepassDsvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    throwIfFailed(d.device->CreateDescriptorHeap(&prepassDsvDesc, IID_PPV_ARGS(&d.prepassDsvHeap)),
                  "ID3D12Device::CreateDescriptorHeap(DSV del pre-pase)");
    d.createSsaoPipelines();
    d.createSsaoTargets();

    d.createSkinningPipelines();
    // Las sombras van al final: su SRV pisa el array de relleno que dejó
    // createMeshResources en el hueco t3, y necesita el buffer de instancias
    // del cubo ya creado.
    d.createShadowResources();
    d.computeCascades();
    // El target HDR y los niveles del bloom necesitan el heap de descriptores
    // ya creado por createMeshResources.
    d.createHdrTargets();
    d.createBloomPipelines();
    // El cielo despues del heap y del target HDR: usa un hueco del primero y
    // dibuja en el segundo.
    d.createSkyboxResources();
    // El IBL sale del cielo recién cargado, así que va detrás.
    d.precomputeIbl();
    d.createFogAndFxaaPipelines();
    d.createSsrPipelines();
    d.createMotionBlurPipeline();
    d.createTaaPipeline();
    d.createForwardPlusPipelines();
    d.createUiPipeline();
    d.createTimestampResources();
    d.updateViewProj();

    d.initialized = true;
}

bool D3D12Renderer::Impl::hasOutlineSelection() const
{
    if (selectedObject >= 0 && selectedObject < static_cast<int>(objects.size()) &&
        objects[static_cast<size_t>(selectedObject)].meshVisible &&
        objects[static_cast<size_t>(selectedObject)].indexCount > 0)
        return true;
    if (selectedSkinned >= 0 && selectedSkinned < static_cast<int>(skinnedObjects.size()) &&
        skinnedObjects[static_cast<size_t>(selectedSkinned)].visible &&
        skinnedObjects[static_cast<size_t>(selectedSkinned)].indexCount > 0)
        return true;
    return false;
}

void D3D12Renderer::Impl::recordSelectionOutline(D3D12_CPU_DESCRIPTOR_HANDLE rtv)
{
    if (!outlineLdrPipeline || !hasOutlineSelection())
        return;

    // La profundidad de la escena, en una versión que se pueda emparejar con el
    // target LDR: los dos tienen que coincidir en número de muestras. Sin MSAA
    // vale la del pase de escena; con MSAA esa es multimuestra y se usa la del
    // pre-pase, que por eso se graba también cuando hay algo seleccionado.
    const bool multisampled = sampleCount > 1;
    if (multisampled && !prepassDepthAllocation)
        return;

    D3D12_CPU_DESCRIPTOR_HANDLE dsv =
        multisampled ? prepassDsvHeap->GetCPUDescriptorHandleForHeapStart()
                     : dsvHeap->GetCPUDescriptorHandleForHeapStart();

    commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    // Viewport SIN altura negativa, al revés que el de la composición: aquel lo
    // invierte porque fullscreen.vert da por hecha la orientación de Vulkan,
    // pero esto es geometría de verdad y sale igual que en el pase de escena.
    D3D12_VIEWPORT viewport{};
    viewport.Width    = static_cast<float>(width);
    viewport.Height   = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    commandList->SetGraphicsRootSignature(meshRootSignature.Get());
    commandList->SetGraphicsRootConstantBufferView(
        0, sceneUboAllocations[frameIndex]->GetResource()->GetGPUVirtualAddress());
    // outline.vert/frag solo miran el UBO y el push, pero la root signature
    // declara los otros dos rangos: se dejan apuntando a algo válido en vez de
    // a cero.
    commandList->SetGraphicsRootDescriptorTable(2, srvHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->SetGraphicsRootShaderResourceView(
        3, instanceAllocation->GetResource()->GetGPUVirtualAddress());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    auto drawOutline = [&](ID3D12PipelineState* pipeline, const glm::mat4& transform,
                           const D3D12_VERTEX_BUFFER_VIEW& vertexView,
                           const D3D12_INDEX_BUFFER_VIEW& indexView, UINT indexCount) {
        if (indexCount == 0)
            return;
        commandList->SetPipelineState(pipeline);

        PushData push{};
        push.transform = transform;
        // flags.y lleva el grosor de la extrusión, que es el hueco que esa vec2
        // tenía libre.
        push.flags = glm::vec2(0.0f, outlineWidth);
        commandList->SetGraphicsRoot32BitConstants(1, sizeof(PushData) / 4, &push, 0);

        commandList->IASetVertexBuffers(0, 1, &vertexView);
        commandList->IASetIndexBuffer(&indexView);
        commandList->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
    };

    if (selectedObject >= 0 && selectedObject < static_cast<int>(objects.size())) {
        const StaticObject& object = objects[static_cast<size_t>(selectedObject)];
        if (object.meshVisible)
            drawOutline(outlineLdrPipeline.Get(), object.transform, object.vertexBufferView,
                        object.indexBufferView, object.indexCount);
    }
    if (selectedSkinned >= 0 && selectedSkinned < static_cast<int>(skinnedObjects.size())) {
        const SkinnedObject& character = skinnedObjects[static_cast<size_t>(selectedSkinned)];
        if (character.visible)
            drawOutline(outlineSkinnedLdrPipeline.Get(), character.transform,
                        character.vertexBufferView, character.indexBufferView,
                        character.indexCount);
    }
}

void D3D12Renderer::Impl::recordSceneGeometry(D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                                              D3D12_CPU_DESCRIPTOR_HANDLE dsv, UINT targetWidth,
                                              UINT targetHeight)
{
    // TODO el pase de geometria: limpiar, mallas, personajes, contorno, cielo y
    // las lineas del editor. Sale de drawFrame para poder repetirlo con otra
    // camara y otro destino, que es lo que necesita el horneado de una sonda de
    // reflexion: seis caras, la misma escena.
    //
    // Lo que NO entra: las marcas de tiempo (miden el pase del frame, no una
    // cara) y el post-procesado, que va detras y sobre el target HDR.
    commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // Viewport y scissor se ponen cada frame: tras un resize el estado del
    // command list se reinicia y arrastrar el tamaño viejo recortaría la imagen.
    D3D12_VIEWPORT viewport{};
    viewport.Width    = static_cast<float>(targetWidth);
    viewport.Height   = static_cast<float>(targetHeight);
    viewport.MaxDepth = 1.0f;
    commandList->RSSetViewports(1, &viewport);

    D3D12_RECT scissor{0, 0, static_cast<LONG>(targetWidth), static_cast<LONG>(targetHeight)};
    commandList->RSSetScissorRects(1, &scissor);

    // La malla primero: escribe profundidad y así la rejilla que va detrás
    // queda tapada donde toca.
    if (meshPipeline) {
        ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
        commandList->SetDescriptorHeaps(1, heaps);

        const bool wireframe = state->isWireframeMode();
        commandList->SetPipelineState(wireframe ? meshWirePipeline.Get()
                                                  : meshPipeline.Get());
        commandList->SetGraphicsRootSignature(meshRootSignature.Get());
        commandList->SetGraphicsRootConstantBufferView(
            0, sceneUboAllocations[frameIndex]->GetResource()->GetGPUVirtualAddress());

        commandList->SetGraphicsRootDescriptorTable(
            2, srvHeap->GetGPUDescriptorHandleForHeapStart());
        commandList->SetGraphicsRootShaderResourceView(
            3, instanceAllocation->GetResource()->GetGPUVirtualAddress());
        bindForwardPlus();
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Geometría de la escena, agrupada por malla compartida: N cubos
        // iguales visibles salen de UN draw instanciado, y cada instancia coge
        // su matriz del tramo del grupo (flags.x = 1). El culling sigue siendo
        // por objeto —lo que decide es si el objeto entra o no en su grupo—.
        //
        // El reparto se rehace aquí y no una vez por frame porque esta función
        // también graba las seis caras de una sonda, y cada una ve un conjunto
        // distinto. Que se pueda reusar el mismo tramo es cosa de que cada cara
        // se envía y se espera antes de grabar la siguiente.
        const Culling::Frustum cameraFrustum = Culling::frustumFromViewProj(viewProj);

        batchCandidates.clear();
        batchCandidates.reserve(objects.size());
        for (const StaticObject& object : objects) {
            // El test del frustum es conservador —puede dejar pasar algo que no
            // se ve, nunca quitar algo que sí—, y una malla sin caja se dibuja
            // siempre.
            const bool dibujable = object.meshVisible && object.indexCount > 0;
            const bool visible =
                dibujable && (!object.hasBounds ||
                              Culling::aabbVisible(cameraFrustum, object.aabbMin, object.aabbMax,
                                                   object.transform));
            if (dibujable && !visible)
                ++statCulledCount;

            batchCandidates.push_back({object.drawGroup, visible, &object.transform,
                                       state->ssrEnabled() ? object.ssrStrength : 0.0f});
        }

        const uint32_t sceneBase =
            kInstanceRegionScene * static_cast<uint32_t>(instanceRegionStride);
        sceneBatches.clear();
        if (sceneInstanceMapped[frameIndex] && instanceRegionStride > 0) {
            auto* matrices = static_cast<glm::mat4*>(sceneInstanceMapped[frameIndex]);
            Batching::buildInstanceBatches(batchCandidates.data(), batchCandidates.size(),
                                           matrices + sceneBase,
                                           static_cast<uint32_t>(instanceRegionStride), sceneBase,
                                           sceneBatches);
        }

        for (const Batching::InstanceBatch& batch : sceneBatches) {
            const StaticObject& rep = objects[static_cast<size_t>(
                drawGroupRep[static_cast<size_t>(batch.sharedIndex)])];

            // La terna del representante vale para todo el grupo: comparten
            // malla, material y sonda, que es justo lo que decide el grupo.
            D3D12_GPU_DESCRIPTOR_HANDLE table = srvHeap->GetGPUDescriptorHandleForHeapStart();
            table.ptr += static_cast<UINT64>(rep.srvBase) * srvSize;
            commandList->SetGraphicsRootDescriptorTable(2, table);
            commandList->SetGraphicsRootShaderResourceView(3,
                                                           instanceAddress(batch.firstInstance));

            PushData push{};
            // flags.x = 1: el model sale del buffer de instancias, uno por
            // instancia. El transform del push constant no se mira.
            push.metallic  = rep.metallic;
            push.roughness = rep.roughness;
            push.flags     = glm::vec2(1.0f, batch.ssrStrength);
            commandList->SetGraphicsRoot32BitConstants(1, sizeof(PushData) / 4, &push, 0);

            commandList->IASetVertexBuffers(0, 1, &rep.vertexBufferView);
            commandList->IASetIndexBuffer(&rep.indexBufferView);
            commandList->DrawIndexedInstanced(rep.indexCount, batch.instanceCount, 0, 0, 0);
            ++statDraws;
            statInstanced += static_cast<int>(batch.instanceCount);
        }

        // Suelo: receptor de sombras y referencia visual, NO parte de la
        // escena. Solo se dibuja cuando no hay geometría cargada: un proyecto
        // suele traer su propio plano, y superponerle otro deja los dos
        // peleándose por la profundidad y proyectándose sombra el uno al otro.
        if (groundIndexCount > 0 && objects.empty()) {
            PushData groundPush{};
            groundPush.transform = glm::mat4(1.0f);
            groundPush.metallic  = 0.0f;
            groundPush.roughness = 0.9f;
            groundPush.flags     = glm::vec2(0.0f, 0.0f);
            commandList->SetGraphicsRoot32BitConstants(1, sizeof(PushData) / 4, &groundPush, 0);
            commandList->IASetVertexBuffers(0, 1, &groundVertexBufferView);
            commandList->IASetIndexBuffer(&groundIndexBufferView);
            commandList->DrawIndexedInstanced(groundIndexCount, 1, 0, 0, 0);
            ++statDraws;
            ++statInstanced;
        }
    }

    // Personajes: mismos shaders y misma root signature que el cubo, pero el
    // vertex buffer es lo que acaba de escribir el compute.
    if (!skinnedObjects.empty() && skinnedMeshPipeline) {
        commandList->SetPipelineState(state->isWireframeMode()
                                            ? skinnedMeshWirePipeline.Get()
                                            : skinnedMeshPipeline.Get());
        commandList->SetGraphicsRootSignature(meshRootSignature.Get());
        commandList->SetGraphicsRootConstantBufferView(
            0, sceneUboAllocations[frameIndex]->GetResource()->GetGPUVirtualAddress());
        commandList->SetGraphicsRootShaderResourceView(
            3, instanceAllocation->GetResource()->GetGPUVirtualAddress());
        bindForwardPlus();
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        for (const SkinnedObject& character : skinnedObjects) {
            if (!character.visible || character.indexCount == 0)
                continue;

            PushData push{};
            // flags.x = 0: el model sale de aquí, no del buffer de instancias,
            // que es la ruta que usa el motor para skinne
            push.transform = character.transform;
            push.metallic  = 0.0f;
            push.roughness = 0.7f;
            push.flags = glm::vec2(0.0f, state->ssrEnabled() ? character.ssrStrength : 0.0f);
            commandList->SetGraphicsRoot32BitConstants(1, sizeof(PushData) / 4, &push, 0);

            commandList->IASetVertexBuffers(0, 1, &character.vertexBufferView);
            commandList->IASetIndexBuffer(&character.indexBufferView);

            // Un draw por submalla, cada una con la terna de su material.
            for (const SkinnedSubMesh& sub : character.subMeshes) {
                if (sub.indexCount == 0)
                    continue;
                D3D12_GPU_DESCRIPTOR_HANDLE table = srvHeap->GetGPUDescriptorHandleForHeapStart();
                table.ptr += static_cast<UINT64>(sub.srvBase) * srvSize;
                commandList->SetGraphicsRootDescriptorTable(2, table);
                commandList->DrawIndexedInstanced(sub.indexCount, 1, sub.indexStart, 0, 0);
                ++statDraws;
                ++statInstanced;
            }
        }
    }

    // El cielo al final de la geometria: se apoya en la profundidad ya escrita
    // para salir solo donde no hay nada, y asi no paga sombreado por pixeles
    // que va a tapar la escena.
    recordSkybox();

    // La rejilla y las líneas de depuración son cosa del EDITOR: en un juego
    // exportado no pintan nada, y salían igual porque este backend no miraba el
    // modo headless.
    // La rejilla y las líneas de depuración comparten pipeline y formato de
    // vértice, pero NO condición: antes las líneas colgaban del `if` de la
    // rejilla, así que una escena sin rejilla se llevaba por delante también los
    // gizmos del editor. Cada una con la suya.
    const bool dibujaRejilla = gridVertexCount > 0;
    const bool dibujaLineas  = debugLineVertices > 0 && debugLinesAllocation;

    if (gizmoPipeline && !headless && (dibujaRejilla || dibujaLineas)) {
        commandList->SetPipelineState(gizmoPipeline.Get());
        commandList->SetGraphicsRootSignature(rootSignature.Get());
        // glm guarda la matriz en columnas y el HLSL traducido la declara
        // row_major: los 16 floats crudos se interpretan igual que en Vulkan,
        // sin transponer.
        commandList->SetGraphicsRoot32BitConstants(0, 16, &viewProj[0][0], 0);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);

        if (dibujaRejilla) {
            commandList->IASetVertexBuffers(0, 1, &gridVertexBufferView);
            commandList->DrawInstanced(gridVertexCount, 1, 0, 0);
        }

        // Las que haya mandado este frame quien dibuja (en el editor,
        // ViewportPanel a través de submitDebugLines): colliders, luces,
        // frustum de la cámara y ejes de la selección.
        if (dibujaLineas) {
            commandList->IASetVertexBuffers(0, 1, &debugLinesView);
            commandList->DrawInstanced(debugLineVertices, 1, 0, 0);
        }
    }

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

    // Y un cambio de tamaño del panel, que mueve todo lo interno sin tocar la
    // swapchain.
    d.applyPendingRenderSize();

    // Y un cambio de anti-aliasing, que mueve targets y pipelines: aquí, entre
    // frames, no en mitad de uno.
    d.applyPendingSampleCount();

    // Los tiempos que dejó la última vez que se usó este slot: moveToNextFrame
    // ya esperó su fence, así que están completos. Se leen ANTES de grabar
    // nada, porque el frame que empieza los va a sobrescribir.
    d.readTimestamps();

    // Qué cámara manda este frame: la de edición o la de la escena si corre
    // Play. Va antes que todo lo que dibuja o mide —las sondas hornean con el
    // UBO, las cascadas se reparten sobre el frustum— porque cambia la
    // proyección y con ella el culling y el rango de sombras.
    d.resolveFrameCamera();
    d.updateViewProj();
    d.computeCascades();

    // Los grupos de dibujo y el buffer de instancias, ANTES que las sondas: el
    // horneado graba el pase de geometría seis veces, y ese pase agrupa y
    // escribe en este buffer. Sin esto, el primer horneado tras cargar una
    // escena capturaba el cielo y nada más, porque los objetos aún no tenían
    // grupo asignado.
    if (!d.objects.empty()) {
        if (d.drawGroupsDirty)
            d.rebuildDrawGroups();
        d.instanceRegionStride = d.objects.size();
        d.ensureSceneInstanceBuffer(d.instanceRegionStride * 2);
    }

    // Sondas: altas, bajas y horneado. ANTES de abrir el frame, porque hornear
    // graba en esta misma lista de comandos y espera a la GPU — con el frame a
    // medias, el Reset del allocator falla y no se hornea nada, en silencio.
    d.syncProbes();

    // Y el hueco de oclusion, que depende del interruptor del SSAO.
    d.refreshAoSlots();

    ID3D12CommandAllocator* allocator = d.allocators[d.frameIndex].Get();
    if (FAILED(allocator->Reset()))
        return;
    if (FAILED(d.commandList->Reset(allocator, nullptr)))
        return;

    // TODAS las marcas al arranque del frame, y luego cada pase sobrescribe las
    // suyas. El resolve copia el rango entero, así que una query que este frame
    // no se escriba conservaría el tick de hace tres frames y daría una resta
    // absurda —se vio un Forward+ de 735 ms con el modo apagado—. Escribiéndolas
    // todas, un pase que no corre mide cero, que es la verdad.
    for (UINT slot = 0; slot < Impl::TsCount; ++slot)
        d.markTimestamp(slot);

    // Las cuentas del frame anterior ya las ha leído el panel (la interfaz se
    // construye antes de grabar), así que aquí se pueden reiniciar.
    d.statDraws       = 0;
    d.statInstanced   = 0;
    d.statCulledCount = 0;

    // Los tres compute van ANTES de abrir el render target: escriben el vertex
    // buffer que el pase gráfico va a leer este mismo frame.
    if (!d.skinnedObjects.empty()) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsed = d.tickFrequency.QuadPart > 0
                                   ? static_cast<double>(now.QuadPart - d.lastTick.QuadPart) /
                                         static_cast<double>(d.tickFrequency.QuadPart)
                                   : 0.0;
        d.lastTick = now;

        // Cada personaje avanza en su propio ciclo: los clips no duran lo
        // mismo, y un tiempo compartido haría saltar a los cortos.
        for (Impl::SkinnedObject& character : d.skinnedObjects) {
            // Salvo los que lleva alguien de fuera: el editor avanza el reloj
            // por frame desde el Animator del GameObject, y sumar aquí también
            // los pondría al doble de velocidad.
            if (character.externalClock)
                continue;
            character.animTime += static_cast<float>(elapsed);
            if (character.animDuration > 0.0f && character.animTime > character.animDuration)
                character.animTime = std::fmod(character.animTime, character.animDuration);
        }

        d.recordSkinning();
    }

    // El UBO se escribe una vez por frame y lo leen los dos pases: el de
    // sombras necesita lightSpaceMatrix, el principal todo lo demás.
    d.updateSceneUbo();

    // Y el reparto del tramo de sombras/pre-pase. Se reescribe entero: mover un
    // objeto no tiene por qué avisar al renderer. Va aquí, con el frame ya
    // abierto, porque el horneado de una sonda pudo cambiar la asignación de
    // sondas y con ella los grupos.
    if (!d.objects.empty()) {
        if (d.drawGroupsDirty)
            d.rebuildDrawGroups();
        d.buildShadowBatches();
    } else {
        d.shadowBatches.clear();
        d.instanceRegionStride = 0;
    }

    // Lo mismo para los personajes: el pase de sombras los dibuja con
    // StartInstanceLocation, y shadow.vert saca su model de este buffer.
    if (!d.skinnedObjects.empty()) {
        d.ensureSkinnedInstanceBuffer(d.skinnedObjects.size());
        if (d.skinnedInstanceMapped[d.frameIndex]) {
            auto* matrices = static_cast<glm::mat4*>(d.skinnedInstanceMapped[d.frameIndex]);
            for (size_t i = 0; i < d.skinnedObjects.size(); ++i)
                matrices[i] = d.skinnedObjects[i].transform;
        }
    }

    // Sombras antes del pase principal: pbr.frag muestrea el mapa que se graba
    // aquí.
    if (d.shadowPipeline) {
        d.markTimestamp(Impl::TsShadow);
        d.recordShadowPasses();
        d.markTimestamp(Impl::TsShadow + 1);
    }

    // Y la oclusión, que necesita su propia profundidad y la produce con dos
    // compute: pbr.frag la multiplica al ambiente en el pase siguiente.
    d.markTimestamp(Impl::TsSsao);
    d.recordDepthPrepassAndSsao();
    d.markTimestamp(Impl::TsSsao + 1);

    // Reparto de luces por celda. Va detrás del pre-pase porque el modo tiled
    // reduce la profundidad de cada tile a partir de él.
    d.updateForwardPlus();
    d.markTimestamp(Impl::TsForwardPlus);
    d.recordForwardPlusCull();
    d.markTimestamp(Impl::TsForwardPlus + 1);

    D3D12_RESOURCE_BARRIER toRenderTarget{};
    toRenderTarget.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRenderTarget.Transition.pResource   = d.renderTargets[d.frameIndex].Get();
    toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRenderTarget.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    d.commandList->ResourceBarrier(1, &toRenderTarget);

    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    backBufferRtv.ptr += static_cast<SIZE_T>(d.frameIndex) * d.rtvSize;

    // Con la escena en textura, el pase final escribe ahí y el backbuffer se
    // queda para la interfaz, que es quien la dibujará dentro de su panel.
    D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv = backBufferRtv;
    const bool toTexture = d.renderToTexture && d.viewportAllocation;
    if (toTexture) {
        sceneRtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        sceneRtv.ptr += static_cast<SIZE_T>(kFrameCount + 5) * d.rtvSize;

        D3D12_RESOURCE_BARRIER toTarget{};
        toTarget.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toTarget.Transition.pResource   = d.viewportAllocation->GetResource();
        toTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toTarget.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        d.commandList->ResourceBarrier(1, &toTarget);
    }

    // La escena NO se dibuja en el backbuffer: va al target HDR, que es el
    // único sitio donde el umbral del bloom puede distinguir lo que pasa de
    // 1.0. El backbuffer lo escribe después el pase de composición.
    // Con MSAA la escena se dibuja en el par multimuestra y se resuelve al
    // cerrar el pase; sin él, directo al HDR de siempre.
    const bool multisampled = d.sampleCount > 1 && d.hdrMsAllocation && d.depthMsAllocation;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(multisampled ? kFrameCount + 2 : kFrameCount) * d.rtvSize;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = d.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    if (multisampled)
        dsv.ptr += d.dsvSize;
    d.markTimestamp(Impl::TsScene);
    d.recordSceneGeometry(rtv, dsv, d.width, d.height);
    d.markTimestamp(Impl::TsScene + 1);

    // El buffer de vértices deformados vuelve a acceso desordenado: el frame
    // siguiente lo reescribe el compute, y tiene que encontrarlo como lo dejó
    // el anterior o la transición de ida partiría de un estado que no es.
    for (const Impl::SkinnedObject& character : d.skinnedObjects) {
        if (character.vertexCount == 0)
            continue;
        D3D12_RESOURCE_BARRIER backToUav{};
        backToUav.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        backToUav.Transition.pResource   = character.outputVerts->GetResource();
        backToUav.Transition.StateBefore = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        backToUav.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        backToUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        d.commandList->ResourceBarrier(1, &backToUav);
    }

    if (multisampled) {
        // Multimuestra a una muestra: de aquí en adelante todo el post lee el
        // HDR de siempre, que es el único que tiene UAV y vistas de lectura.
        D3D12_RESOURCE_BARRIER toResolve[2]{};
        toResolve[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toResolve[0].Transition.pResource   = d.hdrMsAllocation->GetResource();
        toResolve[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toResolve[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        toResolve[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        toResolve[1]                      = toResolve[0];
        toResolve[1].Transition.pResource = d.hdrAllocation->GetResource();
        toResolve[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_DEST;
        d.commandList->ResourceBarrier(2, toResolve);

        d.commandList->ResolveSubresource(d.hdrAllocation->GetResource(), 0,
                                          d.hdrMsAllocation->GetResource(), 0, kHdrFormat);

        for (int i = 0; i < 2; ++i)
            std::swap(toResolve[i].Transition.StateBefore, toResolve[i].Transition.StateAfter);
        d.commandList->ResourceBarrier(2, toResolve);
    }

    // Reflejos antes de la niebla: leen la escena tal cual salió del pase y le
    // suman lo reflejado; la niebla va después porque tiñe TODO lo que hay.
    if (d.state->ssrEnabled()) {
        d.markTimestamp(Impl::TsSsr);
        d.recordSsr();
        d.markTimestamp(Impl::TsSsr + 1);
    }

    // Niebla ANTES del bloom: reescribe la escena, y lo que el bloom filtre
    // tiene que ser ya lo que se va a ver.
    // Ahora que el interruptor vive en el estado compartido, se respeta: es el
    // mismo que apaga la niebla en el menú View del editor.
    if (d.fogPipeline && d.state->fogEnabled()) {
        d.markTimestamp(Impl::TsFog);
        d.recordFog();
        d.markTimestamp(Impl::TsFog + 1);
    }

    // Motion blur detrás de la niebla y antes del bloom: emborrona la imagen tal
    // y como se va a ver, y la estela arrastra los highlights para que florezcan
    // con ellos. Apagado no graba ni un comando.
    if (d.motionBlurActive())
        d.recordMotionBlur();

    // Bloom, composición con tone mapping y FXAA hasta el backbuffer. El
    // anti-aliasing se cronometra dentro: TAA y FXAA van cosidos a este pase.
    if (d.compositePipeline) {
        d.markTimestamp(Impl::TsBloom);
        d.recordBloomAndComposite(sceneRtv);
        d.markTimestamp(Impl::TsBloom + 1);
    }

    if (toTexture) {
        // Y a lectura, que es como la quiere la interfaz.
        D3D12_RESOURCE_BARRIER toRead{};
        toRead.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRead.Transition.pResource   = d.viewportAllocation->GetResource();
        toRead.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toRead.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toRead.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        d.commandList->ResourceBarrier(1, &toRead);

        // El backbuffer no lo ha tocado nadie: se limpia para que la interfaz
        // no dibuje sobre lo del frame anterior.
        const float uiClear[4] = {0.05f, 0.05f, 0.06f, 1.0f};
        d.commandList->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);
        d.commandList->ClearRenderTargetView(backBufferRtv, uiClear, 0, nullptr);

        if (d.uiDrawCallback) {
            ID3D12DescriptorHeap* heaps[] = {d.srvHeap.Get()};
            d.commandList->SetDescriptorHeaps(1, heaps);
            d.uiDrawCallback();
        }
    }

    D3D12_RESOURCE_BARRIER toPresent = toRenderTarget;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    d.commandList->ResourceBarrier(1, &toPresent);

    // La última marca y el volcado, ya con todo grabado.
    d.markTimestamp(Impl::TsFrame + 1);
    d.resolveTimestamps();

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

    swapWidth  = pendingWidth;
    swapHeight = pendingHeight;
    // Sin panel, el render es del tamaño de la ventana. Con panel manda el
    // panel, y redimensionar la ventana no tiene por qué moverlo. El de render
    // sale del de salida, que con SSAA no son el mismo: lo recalcula
    // applyPendingRenderSize en el frame siguiente, aquí basta con dejar el de
    // salida al día.
    if (!renderToTexture || pendingRenderWidth == 0) {
        outWidth  = pendingWidth;
        outHeight = pendingHeight;
        width     = pendingWidth;
        height    = pendingHeight;
    }
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

    // Y los del SSAO, que también son del tamaño de la ventana.
    if (ssaoRawAllocation) {
        createSsaoTargets();
        ssaoBlurNeedsUav = false;  // recién creado: ya está en escritura
    }

    // El aspecto de la proyección depende del tamaño: sin esto la rejilla se
    // deforma al estirar la ventana.
    updateViewProj();

    // Y las cascadas se reparten sobre el frustum de la cámara, que acaba de
    // cambiar de forma: sin recalcularlas, los volúmenes de sombra siguen
    // ajustados al aspecto anterior.
    computeCascades();
}

void D3D12Renderer::setCamera(const glm::mat4& view, const glm::vec3& position, float fovDegrees)
{
    Impl& d      = *m_impl;
    d.cameraView = view;
    d.cameraPos  = position;
    if (fovDegrees > 0.0f)
        d.cameraFovDeg = fovDegrees;

    d.updateViewProj();

    // Las cascadas se reparten sobre el frustum de la cámara: sin recalcularlas
    // aquí seguirían cubriendo el volumen del encuadre anterior, y la sombra se
    // quedaría atrás al mover la vista.
    d.computeCascades();
}

void D3D12Renderer::setLights(const Light* lights, size_t count)
{
    Impl& d = *m_impl;
    d.sceneLights.clear();
    if (!lights || count == 0)
        return;

    count = (std::min)(count, static_cast<size_t>(16));
    d.sceneLights.resize(count);
    std::memcpy(d.sceneLights.data(), lights, count * sizeof(ShaderLight));

    // La dirección de las cascadas sale de la POSICIÓN de la primera luz, del
    // tipo que sea, igual que en el camino de Vulkan: la sombra en cascada la
    // proyecta siempre la luz 0, y tomar su dirección de otro sitio la pondría
    // donde no llega la luz.
    const glm::vec3 first(d.sceneLights[0].position[0], d.sceneLights[0].position[1],
                          d.sceneLights[0].position[2]);
    if (glm::length(first) > 1e-6f) {
        d.lightDirection = -glm::normalize(first);
        d.computeCascades();
    }
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

int D3D12Renderer::addStaticMesh(const Mesh& mesh, const std::vector<DecodedImage>*)
{
    Impl& d = *m_impl;
    if (!d.initialized || mesh.vertices.empty() || mesh.indices.empty())
        return -1;

    Impl::StaticObject object;

    // ¿Ya está esta misma malla con este mismo material en VRAM? La clave es de
    // CONTENIDO, así que dos cubos creados por separado la comparten. El
    // duplicado se queda con los handles del dueño y no sube ni un byte.
    const std::string key   = makeSharedMeshKey(mesh);
    auto              found = d.sharedMeshOwner.find(key);
    const bool        reusa = found != d.sharedMeshOwner.end() &&
                       found->second < static_cast<int>(d.objects.size());

    if (reusa) {
        Impl::StaticObject& owner = d.objects[static_cast<size_t>(found->second)];
        ++owner.sharedRefs;
        object.vertexAllocation  = owner.vertexAllocation;
        object.indexAllocation   = owner.indexAllocation;
        object.vertexBufferView  = owner.vertexBufferView;
        object.indexBufferView   = owner.indexBufferView;
        object.indexCount        = owner.indexCount;
        object.baseColorAllocation  = owner.baseColorAllocation;
        object.normalMapAllocation  = owner.normalMapAllocation;
        object.metalRoughAllocation = owner.metalRoughAllocation;
        object.aabbMin    = owner.aabbMin;
        object.aabbMax    = owner.aabbMax;
        object.hasBounds  = owner.hasBounds;
        object.sharedMesh = found->second;
        object.ownsGpu    = false;
    } else {
        object.vertexAllocation =
            d.uploadBuffer(mesh.vertices.data(), mesh.vertices.size() * sizeof(Vertex),
                           D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        object.vertexBufferView.BufferLocation =
            object.vertexAllocation->GetResource()->GetGPUVirtualAddress();
        object.vertexBufferView.SizeInBytes =
            static_cast<UINT>(mesh.vertices.size() * sizeof(Vertex));
        object.vertexBufferView.StrideInBytes = sizeof(Vertex);

        object.indexAllocation =
            d.uploadBuffer(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t),
                           D3D12_RESOURCE_STATE_INDEX_BUFFER);
        object.indexBufferView.BufferLocation =
            object.indexAllocation->GetResource()->GetGPUVirtualAddress();
        object.indexBufferView.SizeInBytes =
            static_cast<UINT>(mesh.indices.size() * sizeof(uint32_t));
        object.indexBufferView.Format = DXGI_FORMAT_R32_UINT;
        object.indexCount             = static_cast<UINT>(mesh.indices.size());

        // Caja envolvente local, de una vez y para siempre: no depende del
        // transform, así que moverlo o rotarlo no obliga a recalcularla.
        // Paréntesis alrededor del nombre: windows.h define max como macro y
        // sin ellos no compila.
        glm::vec3 lo((std::numeric_limits<float>::max)());
        glm::vec3 hi(std::numeric_limits<float>::lowest());
        for (const Vertex& v : mesh.vertices) {
            lo = (glm::min)(lo, v.pos);
            hi = (glm::max)(hi, v.pos);
        }
        object.aabbMin   = lo;
        object.aabbMax   = hi;
        object.hasBounds = true;

        object.sharedMesh = static_cast<int>(d.objects.size());
        object.ownsGpu    = true;
        object.sharedRefs = 1;  // él mismo
    }

    object.metallic  = mesh.material.metallic;
    object.roughness = mesh.material.roughness;

    // Terna propia en el heap mientras queden huecos. Pasado el tope se queda
    // con la global: peor aspecto, pero nunca escribe fuera del heap.
    //
    // El bloque de descriptores NO se comparte aunque la malla sí: t4 y t5
    // llevan la sonda de reflexión que le toca a ESTE objeto, y dos cubos
    // iguales en dos habitaciones distintas reflejan cosas distintas. Lo que se
    // comparte son los recursos a los que apuntan, que es donde está la memoria.
    if (d.objects.size() < kMaxObjectSlots) {
        const UINT slot = kSrvObjects + static_cast<UINT>(d.objects.size()) * kSrvPerObject;
        object.srvBase  = slot;

        if (reusa) {
            // Mismos recursos, vistas nuevas. Los formatos son los que eligió
            // uploadMaterialTexture: sRGB para el color, lineal para el resto.
            if (object.baseColorAllocation)
                d.createTexture2DSrv(object.baseColorAllocation->GetResource(),
                                     DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, slot + 0);
            else
                d.createTexture2DSrv(d.baseColorAllocation->GetResource(),
                                     DXGI_FORMAT_R8G8B8A8_UNORM, slot + 0);

            if (object.normalMapAllocation)
                d.createTexture2DSrv(object.normalMapAllocation->GetResource(),
                                     DXGI_FORMAT_R8G8B8A8_UNORM, slot + 1);
            else
                d.createTexture2DSrv(d.normalMapAllocation->GetResource(),
                                     DXGI_FORMAT_R8G8B8A8_UNORM, slot + 1);

            d.fillSharedSlots(slot);

            if (object.metalRoughAllocation)
                d.createTexture2DSrv(object.metalRoughAllocation->GetResource(),
                                     DXGI_FORMAT_R8G8B8A8_UNORM, slot + 3);
        } else {
            object.baseColorAllocation = d.uploadMaterialTexture(
                mesh.material.texturePath, mesh.material.embeddedTexture, true, slot + 0);
            if (!object.baseColorAllocation)
                d.createTexture2DSrv(d.baseColorAllocation->GetResource(),
                                     DXGI_FORMAT_R8G8B8A8_UNORM, slot + 0);

            object.normalMapAllocation = d.uploadMaterialTexture(
                mesh.material.normalMapPath, mesh.material.embeddedNormalMap, false, slot + 1);
            if (!object.normalMapAllocation)
                d.createTexture2DSrv(d.normalMapAllocation->GetResource(),
                                     DXGI_FORMAT_R8G8B8A8_UNORM, slot + 1);

            // t3..t7: sombras, entorno y oclusión, que son de todos.
            d.fillSharedSlots(slot);

            // Y encima, el ORM propio si el material lo trae: pisa el neutro que
            // acaba de dejar fillSharedSlots.
            object.metalRoughAllocation =
                d.uploadMaterialTexture(mesh.material.metallicRoughnessPath,
                                        mesh.material.embeddedMetallicRoughness, false, slot + 3);
        }
    }

    d.objects.push_back(object);
    if (!reusa)
        d.sharedMeshOwner.emplace(key, static_cast<int>(d.objects.size() - 1));
    d.drawGroupsDirty = true;
    return static_cast<int>(d.objects.size() - 1);
}

void D3D12Renderer::setTransform(size_t objectIndex, const glm::mat4& transform)
{
    if (objectIndex < m_impl->objects.size())
        m_impl->objects[objectIndex].transform = transform;
}

void D3D12Renderer::setObjectSsr(size_t objectIndex, float strength)
{
    if (objectIndex < m_impl->objects.size())
        m_impl->objects[objectIndex].ssrStrength = strength;
}

void D3D12Renderer::setSkinnedSsr(int index, float strength)
{
    if (index >= 0 && static_cast<size_t>(index) < m_impl->skinnedObjects.size())
        m_impl->skinnedObjects[index].ssrStrength = strength;
}

void D3D12Renderer::setScene(Scene* scene)
{
    m_impl->scene = scene;
}

void D3D12Renderer::setSceneRoot(GameObject* root)
{
    m_impl->sceneRoot = root;
}

void D3D12Renderer::setCamera(const Camera& camera)
{
    setCamera(camera.getViewMatrix(), camera.getPos(), camera.getFov());
}

void D3D12Renderer::setLights(const std::vector<Light>& lights)
{
    setLights(lights.data(), lights.size());
}

void D3D12Renderer::setLightRadii(const std::vector<float>& radii)
{
    // El reparto por celdas saca el radio del alcance de cada luz (params.x),
    // así que esta lista no hace falta aquí. Se acepta para cumplir la
    // interfaz y para que el día que se separen los dos valores haya un sitio
    // donde recogerla.
    (void)radii;
}

void D3D12Renderer::tickDeferredDeletes()
{
    // Nada pendiente: en este backend las liberaciones esperan a la GPU en el
    // momento en que se piden.
}

void D3D12Renderer::updateAnimation(int index, float deltaTime)
{
    Impl& d = *m_impl;
    if (index < 0 || static_cast<size_t>(index) >= d.skinnedObjects.size())
        return;

    // Avanza el reloj de ESE personaje y lo mantiene dentro del clip. El reloj
    // interno del backend sigue existiendo para quien no llame aquí, pero para
    // este personaje se apaga: mandan desde fuera.
    Impl::SkinnedObject& character = d.skinnedObjects[index];
    character.externalClock        = true;
    character.animTime += deltaTime;
    if (character.animDuration > 0.0f && character.animTime > character.animDuration)
        character.animTime = std::fmod(character.animTime, character.animDuration);
}

void D3D12Renderer::setObjectMeshVisible(size_t objectIndex, bool visible)
{
    if (objectIndex < m_impl->objects.size())
        m_impl->objects[objectIndex].meshVisible = visible;
}

size_t D3D12Renderer::objectCount() const
{
    return m_impl->objects.size();
}

void D3D12Renderer::clearStaticMeshes()
{
    Impl& d = *m_impl;
    if (d.objects.empty())
        return;

    // Los buffers pueden estar en uso por el último frame presentado: soltarlos
    // con trabajo en vuelo es una corrupción silenciosa, no un error de la API.
    d.waitForGpu();

    // Se sueltan TODOS a la vez, así que primero se anula el recuento: la
    // guarda de releaseStaticObject protege el borrado de UNO suelto, y aquí no
    // queda nadie vivo que pueda seguir apuntando a estos buffers.
    for (Impl::StaticObject& object : d.objects)
        object.sharedRefs = object.ownsGpu ? 1 : 0;
    for (Impl::StaticObject& object : d.objects)
        d.releaseStaticObject(object);

    d.objects.clear();
    d.sharedMeshOwner.clear();
    d.drawGroupRep.clear();
    d.drawGroupsDirty = true;
}

int D3D12Renderer::addSkinnedMesh(const SkinnedMesh& mesh, const std::vector<DecodedImage>*)
{
    Impl& d = *m_impl;
    if (!d.initialized)
        return -1;
    return d.createSkinnedObject(mesh);
}

void D3D12Renderer::setSkinnedTransform(int index, const glm::mat4& transform)
{
    if (index >= 0 && static_cast<size_t>(index) < m_impl->skinnedObjects.size())
        m_impl->skinnedObjects[index].transform = transform;
}

void D3D12Renderer::setSkinnedMeshVisible(int index, bool visible)
{
    if (index >= 0 && static_cast<size_t>(index) < m_impl->skinnedObjects.size())
        m_impl->skinnedObjects[index].visible = visible;
}

void D3D12Renderer::setAnimationState(int index, uint32_t clipIndex, float animTime)
{
    Impl& d = *m_impl;
    if (index < 0 || static_cast<size_t>(index) >= d.skinnedObjects.size())
        return;
    Impl::SkinnedObject& character = d.skinnedObjects[index];
    character.clipBase             = clipIndex * character.boneCount;
    character.animTime             = animTime;
    // El Animator del GameObject es el dueño del reloj: el backend no vuelve a
    // sumarle tiempo por su cuenta.
    character.externalClock = true;
}

size_t D3D12Renderer::skinnedCount() const
{
    return m_impl->skinnedObjects.size();
}

void D3D12Renderer::clearSkinnedMeshes()
{
    Impl& d = *m_impl;
    if (d.skinnedObjects.empty())
        return;

    // Sus buffers pueden estar en uso por el último frame presentado, y el de
    // vértices deformados lo escribe el compute de ese mismo frame.
    d.waitForGpu();
    d.releaseSkinnedObjects();
}

void D3D12Renderer::waitIdle()
{
    if (m_impl->initialized)
        m_impl->waitForGpu();
}

void D3D12Renderer::submitDebugLines(const float* vertices, size_t vertexCount)
{
    Impl& d = *m_impl;
    d.debugLineVertices = 0;
    if (!d.initialized || !vertices || vertexCount == 0)
        return;

    d.ensureDebugLineBuffer(vertexCount);
    if (!d.debugLinesMapped)
        return;

    std::memcpy(d.debugLinesMapped, vertices, vertexCount * sizeof(GizmoVertex));
    d.debugLineVertices = static_cast<UINT>(vertexCount);
}

glm::mat4 D3D12Renderer::viewProjMatrix() const
{
    // La del frame, sin el desplazamiento del TAA: quien desproyecta un clic
    // quiere la cámara, no el ruido de muestreo.
    return m_impl->cameraProj() * m_impl->cameraView;
}

void D3D12Renderer::rebuildSkinnedMesh(int index, const SkinnedMesh& mesh)
{
    Impl& d = *m_impl;
    if (index < 0 || index >= static_cast<int>(d.skinnedObjects.size()))
        return;

    // Se conservan sitio, transformación y estado de animación: el índice de
    // render del GameObject no puede moverse, y quien lo reconstruye —añadir o
    // quitar clips— no espera que el personaje salte a la pose inicial.
    const Impl::SkinnedObject previous = d.skinnedObjects[index];

    const int created = d.createSkinnedObject(mesh);
    if (created < 0)
        return;

    Impl::SkinnedObject rebuilt = d.skinnedObjects.back();
    d.skinnedObjects.pop_back();

    rebuilt.transform   = previous.transform;
    rebuilt.visible     = previous.visible;
    rebuilt.ssrStrength = previous.ssrStrength;
    rebuilt.animTime    = previous.animTime;
    rebuilt.clipBase    = previous.clipBase;

    // Los recursos viejos pueden estar en uso por el último frame presentado.
    d.waitForGpu();
    Impl::SkinnedObject& slot = d.skinnedObjects[index];
    for (D3D12MA::Allocation** allocation :
         {&slot.posKeys, &slot.rotKeys, &slot.scaleKeys, &slot.boneInfos, &slot.inputVerts,
          &slot.localXforms, &slot.finalBones, &slot.outputVerts, &slot.indices}) {
        if (*allocation) {
            (*allocation)->Release();
            *allocation = nullptr;
        }
    }
    for (D3D12MA::Allocation* texture : slot.textures)
        if (texture)
            texture->Release();
    slot.textures.clear();

    slot = std::move(rebuilt);
}

void D3D12Renderer::registerGameObject(GameObject* node)
{
    if (!node)
        return;

    node->traverse([this](GameObject* child) {
        if (!child || !child->hasMesh())
            return;

        if (child->isSkinned()) {
            const SkinnedMesh* skinned = child->getSkinnedMesh();
            if (!skinned)
                return;
            const int index = addSkinnedMesh(*skinned);
            if (index < 0)
                return;
            child->skinnedRenderIndex = index;
            setSkinnedTransform(index, child->worldTransform);
            setSkinnedSsr(index, child->ssrEnabled ? child->ssrIntensity : 0.0f);
            return;
        }

        const std::shared_ptr<Mesh> mesh = child->getMesh();
        if (!mesh)
            return;
        const int index = addStaticMesh(*mesh);
        if (index < 0)
            return;
        child->staticRenderIndex = index;
        setTransform(static_cast<size_t>(index), child->worldTransform);
        setObjectSsr(static_cast<size_t>(index), child->ssrEnabled ? child->ssrIntensity : 0.0f);
    });
}

void D3D12Renderer::removeGameObject(GameObject* node)
{
    if (!node)
        return;

    // Los huecos NO se compactan: los índices de render de los demás objetos
    // están anotados en sus GameObject, y moverlos dejaría a todos apuntando a
    // otra malla. El objeto se apaga y su sitio queda libre para nada.
    node->traverse([this](GameObject* child) {
        if (!child)
            return;
        if (child->staticRenderIndex >= 0) {
            setObjectMeshVisible(static_cast<size_t>(child->staticRenderIndex), false);
            child->staticRenderIndex = -1;
        }
        if (child->skinnedRenderIndex >= 0) {
            setSkinnedMeshVisible(child->skinnedRenderIndex, false);
            child->skinnedRenderIndex = -1;
        }
    });
}

void D3D12Renderer::removeMeshComponent(GameObject* node)
{
    if (!node)
        return;
    if (node->staticRenderIndex >= 0) {
        setObjectMeshVisible(static_cast<size_t>(node->staticRenderIndex), false);
        node->staticRenderIndex = -1;
    }
    if (node->skinnedRenderIndex >= 0) {
        setSkinnedMeshVisible(node->skinnedRenderIndex, false);
        node->skinnedRenderIndex = -1;
    }
}

void D3D12Renderer::replaceStaticTextureWithMissing(int renderIndex, TextureSlot slot)
{
    Impl& d = *m_impl;
    if (renderIndex < 0 || renderIndex >= static_cast<int>(d.objects.size()))
        return;

    Impl::StaticObject& object = d.objects[renderIndex];
    if (object.srvBase == kSrvBaseColor)
        return;  // sin bloque propio: dibuja con los neutros globales

    // Su bloque deja de decir lo mismo que el de los que comparten esta malla,
    // así que deja de poder compartir draw con ellos.
    ++object.materialVariant;
    d.drawGroupsDirty = true;

    // El neutro que ya existe para cada hueco. No es el damero magenta del
    // camino de Vulkan, pero deja el objeto visible en vez de con basura, que
    // es lo que importa cuando una textura no se ha podido leer.
    switch (slot) {
        case TextureSlot::Diffuse:
            d.createTexture2DSrv(d.baseColorAllocation->GetResource(),
                                 DXGI_FORMAT_R8G8B8A8_UNORM, object.srvBase);
            break;
        case TextureSlot::Normal:
            d.createTexture2DSrv(d.normalMapAllocation->GetResource(),
                                 DXGI_FORMAT_R8G8B8A8_UNORM, object.srvBase + 1);
            break;
        case TextureSlot::MetallicRoughness:
            d.createTexture2DSrv(d.metalRoughAllocation->GetResource(),
                                 DXGI_FORMAT_R8G8B8A8_UNORM, object.srvBase + 3);
            break;
    }
}

void D3D12Renderer::setBloomEnabled(bool v)
{
    setBloomEnabledFlag(v);
}

void D3D12Renderer::flushPendingUploads()
{
    // Nada pendiente: cada subida se envía y se espera en el momento.
}

void D3D12Renderer::flushUploadsAndWait()
{
    m_impl->waitForGpu();
}

void D3D12Renderer::refitCameraRange()
{
    Impl& d = *m_impl;

    // Suelo del rango: una escena diminuta (o con todo en el mismo punto) daría
    // far ~0 y no se vería ni el cielo. 200 deja far=600 y near=0.2, que cubre
    // la cámara con la que abre el editor sin recortar props pequeños. Mismo
    // valor que el camino de Vulkan, para que las dos den el mismo encuadre.
    constexpr float kMinCameraDistance = 200.0f;

    // Paréntesis alrededor de min/max: windows.h los define como macro.
    glm::vec3 lo((std::numeric_limits<float>::max)());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    bool      any = false;

    for (const Impl::StaticObject& object : d.objects) {
        if (!object.hasBounds)
            continue;

        // Las 8 esquinas de la AABB local llevadas a mundo: con el objeto
        // rotado o escalado, la caja alineada a ejes de la malla ya no acota.
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 corner((c & 1) ? object.aabbMax.x : object.aabbMin.x,
                                   (c & 2) ? object.aabbMax.y : object.aabbMin.y,
                                   (c & 4) ? object.aabbMax.z : object.aabbMin.z);
            const glm::vec3 world = glm::vec3(object.transform * glm::vec4(corner, 1.0f));
            lo = (glm::min)(lo, world);
            hi = (glm::max)(hi, world);
        }
        any = true;
    }

    // De los personajes solo entra su origen: aquí no se guarda una cota de la
    // pose como en Vulkan, y esto solo fija near/far — no culea nada. Sirve
    // para que una escena que sea solo personajes no se quede sin rango.
    for (const Impl::SkinnedObject& character : d.skinnedObjects) {
        const glm::vec3 origin(character.transform[3]);
        lo  = (glm::min)(lo, origin);
        hi  = (glm::max)(hi, origin);
        any = true;
    }

    // Nada acotable: conserva el rango vigente en vez de dejarlo en infinitos.
    // Es lo que pasa con la escena vacía de un proyecto recién creado.
    if (!any)
        return;

    const float maxDim = (glm::max)(hi.x - lo.x, (glm::max)(hi.y - lo.y, hi.z - lo.z));
    d.cameraDistance   = (glm::max)(maxDim * 1.2f, kMinCameraDistance);

    // El rango acaba de cambiar de forma: sin esto, las cascadas seguirían
    // repartidas sobre el frustum anterior hasta el siguiente movimiento de
    // cámara.
    d.updateViewProj();
    d.computeCascades();
}

void D3D12Renderer::setOutlineTarget(int staticIndex, int skinnedIndex)
{
    setSelection(staticIndex, skinnedIndex);
}

uint32_t D3D12Renderer::renderWidth() const
{
    return m_impl->width;
}

uint32_t D3D12Renderer::renderHeight() const
{
    return m_impl->height;
}

float D3D12Renderer::viewportAspect() const
{
    const Impl& d = *m_impl;
    return (d.height > 0) ? static_cast<float>(d.width) / static_cast<float>(d.height) : 1.0f;
}

void D3D12Renderer::setUiLayer(UiLayer* ui)
{
    Impl& d  = *m_impl;
    d.uiLayer = ui;

    // La capa de interfaz se graba por el mismo hueco que ya existía para
    // ImGui: quien la pone deja de tener que registrar el callback a mano.
    if (!ui) {
        setUiDrawCallback(nullptr);
        return;
    }
    setUiDrawCallback([this]() {
        if (m_impl->uiLayer)
            m_impl->uiLayer->recordUi(m_impl->commandList.Get());
    });
}

UiCanvas& D3D12Renderer::uiCanvas()
{
    return m_impl->uiCanvas;
}

void D3D12Renderer::initSceneResources(const std::vector<Mesh>& meshes)
{
    // La fase 2 del arranque para este backend: subir lo que ya hay. El auto-fit
    // de la cámara y los recursos que dependen del tamaño de la escena los
    // resuelve init(), que aquí ya corrió.
    for (const Mesh& mesh : meshes)
        addStaticMesh(mesh);
    refitCameraRange();
}

void D3D12Renderer::drawFrame(Window& window)
{
    // La ventana no hace falta: el tamaño llega por resize() desde su callback.
    (void)window;
    drawFrame();
}

void D3D12Renderer::setHeadless(bool headless)
{
    m_impl->headless = headless;
}

void D3D12Renderer::notifyResize()
{
    // Vulkan solo marca un flag porque su swapchain se recrea sola al fallar el
    // present. Aquí el tamaño lo trae el callback de la ventana, que ya llama a
    // resize(): no queda nada por hacer.
}

UiTextureAtlas* D3D12Renderer::loadUiAtlas(const std::string& path)
{
    Impl& d = *m_impl;
    if (!d.initialized)
        return nullptr;

    auto atlas = std::make_unique<UiTextureAtlas>();
    if (!atlas->loadPixelsFromFile(path))
        return nullptr;
    if (!d.registerUiAtlas(*atlas))
        return nullptr;

    d.uiAtlases.push_back(std::move(atlas));
    return d.uiAtlases.back().get();
}

UiFont* D3D12Renderer::loadUiFont(const std::string& path, float bakePx)
{
    Impl& d = *m_impl;
    if (!d.initialized)
        return nullptr;

    auto font = std::make_unique<UiFont>();
    // El horneado es CPU: FreeType y MSDF no saben de backends. Lo único propio
    // es subir el atlas que sale de ahí.
    if (!font->bakeFromFile(path, bakePx))
        return nullptr;
    if (!d.registerUiAtlas(font->atlas()))
        return nullptr;

    d.uiFonts.push_back(std::move(font));
    return d.uiFonts.back().get();
}

void D3D12Renderer::setAaMode(AaMode mode)
{
    // Solo se anota: los targets y los pipelines los rehace el frame siguiente,
    // con la GPU en reposo.
    setAaModeFlag(mode);
}

void D3D12Renderer::setMsaaSamples(int v)
{
    setMsaaSamplesFlag(v);
}

int D3D12Renderer::maxMsaaSamples() const
{
    const Impl& d = *m_impl;
    if (!d.device)
        return 1;

    int best = 1;
    for (UINT samples = 2; samples <= 8; samples *= 2) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{};
        levels.Format      = kHdrFormat;
        levels.SampleCount = samples;
        if (SUCCEEDED(d.device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                                    &levels, sizeof(levels))) &&
            levels.NumQualityLevels > 0)
            best = static_cast<int>(samples);
    }
    return best;
}

void D3D12Renderer::setSsaoEnabled(bool v)
{
    setSsaoEnabledFlag(v);
}

void D3D12Renderer::setSsaaFactor(float v)
{
    // Se guarda para que el panel conserve el valor, pero este backend no
    // dibuja a más resolución: el modo SSAA no está implementado aquí.
    m_impl->ssaaFactor = v;
}

float D3D12Renderer::ssaaFactor() const
{
    return m_impl->ssaaFactor;
}

void D3D12Renderer::requestProbeBake(uint64_t ownerId)
{
    // Solo se apunta: hornear reescribe la camara y espera a la GPU, asi que se
    // hace al principio del frame siguiente y no en mitad de lo que sea que
    // este haciendo quien llama.
    m_impl->probeBakeQueue.push_back(ownerId);
}

void D3D12Renderer::requestProbeBakeAll() { m_impl->probeBakeAllQueued = true; }
int   D3D12Renderer::probeCount() const { return static_cast<int>(m_impl->probes.size()); }
float D3D12Renderer::lastProbeBakeMs() const { return m_impl->probeLastBakeMs; }
float D3D12Renderer::probeBakeMs(uint64_t ownerId) const
{
    for (const Impl::GpuProbe& probe : m_impl->probes)
        if (probe.ownerId == ownerId)
            return probe.bakeMs;
    return 0.0f;
}

void     D3D12Renderer::setPerfCaptureEnabled(bool) {}
// Tiempos de GPU: los mide el par de marcas de cada pase, leídos con dos frames
// de retraso —que es cuando la GPU ya ha terminado el que los escribió— igual
// que en el camino de Vulkan.
float D3D12Renderer::renderGpuMs() const { return m_impl->gpuMs[Impl::TsFrame / 2]; }
float D3D12Renderer::ssaoGpuMs() const { return m_impl->gpuMs[Impl::TsSsao / 2]; }
float D3D12Renderer::ssrGpuMs() const { return m_impl->gpuMs[Impl::TsSsr / 2]; }
float D3D12Renderer::bloomGpuMs() const { return m_impl->gpuMs[Impl::TsBloom / 2]; }
float D3D12Renderer::fogGpuMs() const { return m_impl->gpuMs[Impl::TsFog / 2]; }
float D3D12Renderer::aaGpuMs() const { return m_impl->gpuMs[Impl::TsAa / 2]; }
float D3D12Renderer::sceneGpuMs() const { return m_impl->gpuMs[Impl::TsScene / 2]; }
float D3D12Renderer::shadowGpuMs() const { return m_impl->gpuMs[Impl::TsShadow / 2]; }
float D3D12Renderer::forwardPlusGpuMs() const { return m_impl->gpuMs[Impl::TsForwardPlus / 2]; }
int   D3D12Renderer::statDrawCalls() const { return m_impl->statDraws; }
int   D3D12Renderer::statInstances() const { return m_impl->statInstanced; }
// Objetos que el frustum dejó fuera este frame. Solo estáticos: los personajes
// se dibujan siempre (ver el pase principal).
int   D3D12Renderer::statCulled() const { return m_impl->statCulledCount; }
float    D3D12Renderer::forwardPlusAvgPerCell() const { return 0.0f; }
uint32_t D3D12Renderer::forwardPlusOverflowCells() const { return 0; }

void D3D12Renderer::setSelection(int staticIndex, int skinnedIndex)
{
    m_impl->selectedObject  = staticIndex;
    m_impl->selectedSkinned = skinnedIndex;
}

void D3D12Renderer::setOutlineWidth(float width)
{
    m_impl->outlineWidth = width;
}

void D3D12Renderer::setRenderToTexture(bool enabled)
{
    m_impl->renderToTexture = enabled;
}

void D3D12Renderer::setViewportSize(uint32_t width, uint32_t height)
{
    // Solo se anota: recrear targets exige la GPU en reposo, y eso lo hace
    // drawFrame al empezar el siguiente. Un panel plegado da cero y se ignora,
    // como el minimizado de la ventana.
    if (width == 0 || height == 0)
        return;
    m_impl->pendingRenderWidth  = width;
    m_impl->pendingRenderHeight = height;
}

uint64_t D3D12Renderer::viewportTexture() const
{
    const Impl& d = *m_impl;
    if (!d.viewportAllocation || !d.srvHeap)
        return 0;
    D3D12_GPU_DESCRIPTOR_HANDLE handle = d.srvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(kSrvViewport) * d.srvSize;
    return handle.ptr;
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

    // Geometría de la escena, antes que el allocator. Por la misma función que
    // clearStaticMeshes, para que el criterio de quién suelta viva en un solo
    // sitio; aquí se cierra todo, así que el recuento se anula antes. Este
    // camino ADEMÁS soltaba el ORM, que clearStaticMeshes se dejaba.
    for (Impl::StaticObject& object : d.objects)
        object.sharedRefs = object.ownsGpu ? 1 : 0;
    for (Impl::StaticObject& object : d.objects)
        d.releaseStaticObject(object);

    d.objects.clear();
    d.sharedMeshOwner.clear();
    d.drawGroupRep.clear();

    for (UINT i = 0; i < kFrameCount; ++i) {
        if (!d.sceneInstanceAllocations[i])
            continue;
        if (d.sceneInstanceMapped[i]) {
            d.sceneInstanceAllocations[i]->GetResource()->Unmap(0, nullptr);
            d.sceneInstanceMapped[i] = nullptr;
        }
        d.sceneInstanceAllocations[i]->Release();
        d.sceneInstanceAllocations[i] = nullptr;
        d.sceneInstanceCapacity[i]    = 0;
    }

    for (auto** allocation : {&d.instanceAllocation, &d.baseColorAllocation,
                              &d.normalMapAllocation, &d.shadowMapAllocation,
                              &d.depthAllocation, &d.skyboxAllocation, &d.metalRoughAllocation,
                              &d.irradianceAllocation, &d.prefilterAllocation, &d.ssaoAllocation,
                              &d.fpParamsAllocation, &d.fpLightsAllocation, &d.fpCellsAllocation,
                              &d.fpIndicesAllocation, &d.groundVertexAllocation,
                              &d.groundIndexAllocation, &d.groundInstanceAllocation,
                              &d.shadowMapArrayAllocation}) {
        if (*allocation) {
            (*allocation)->Release();
            *allocation = nullptr;
        }
    }
    d.releaseSkinnedObjects();

    if (d.debugLinesAllocation) {
        if (d.debugLinesMapped) {
            d.debugLinesAllocation->GetResource()->Unmap(0, nullptr);
            d.debugLinesMapped = nullptr;
        }
        d.debugLinesAllocation->Release();
        d.debugLinesAllocation = nullptr;
        d.debugLinesCapacity   = 0;
        d.debugLineVertices    = 0;
    }

    for (UINT i = 0; i < kFrameCount; ++i) {
        if (!d.skinnedInstanceAllocations[i])
            continue;
        if (d.skinnedInstanceMapped[i]) {
            d.skinnedInstanceAllocations[i]->GetResource()->Unmap(0, nullptr);
            d.skinnedInstanceMapped[i] = nullptr;
        }
        d.skinnedInstanceAllocations[i]->Release();
        d.skinnedInstanceAllocations[i] = nullptr;
        d.skinnedInstanceCapacity[i]    = 0;
    }

    d.groundVertexBufferView = {};
    d.groundIndexBufferView  = {};
    d.groundIndexCount       = 0;

    d.skyboxPipeline.Reset();
    d.skyboxRootSignature.Reset();
    d.releaseSsaoTargets();
    d.depthPrepassPipeline.Reset();
    d.depthPrepassSkinnedPipeline.Reset();
    d.depthPrepassRootSignature.Reset();
    d.fpTiledPipeline.Reset();
    d.fpClusteredPipeline.Reset();
    d.fpCullRootSignature.Reset();
    if (d.fpStatsAllocation) {
        d.fpStatsAllocation->Release();
        d.fpStatsAllocation = nullptr;
    }
    d.outlinePipeline.Reset();
    d.outlineSkinnedPipeline.Reset();
    d.outlineLdrPipeline.Reset();
    d.outlineSkinnedLdrPipeline.Reset();
    d.meshWirePipeline.Reset();
    d.skinnedMeshWirePipeline.Reset();
    d.taaPipeline.Reset();
    d.taaRootSignature.Reset();
    d.ssrPipeline.Reset();
    d.ssrResolvePipeline.Reset();
    d.ssrRootSignature.Reset();
    d.motionBlurPipeline.Reset();
    d.motionBlurRootSignature.Reset();
    d.ssaoPipeline.Reset();
    d.ssaoBlurPipeline.Reset();
    d.ssaoRootSignature.Reset();
    d.prepassDsvHeap.Reset();
    d.iblIrradiancePipeline.Reset();
    d.iblPrefilterPipeline.Reset();
    d.iblRootSignature.Reset();

    d.releaseHdrTargets();

    for (Impl::GpuProbe& probe : d.probes)
        d.releaseProbe(probe);
    d.probes.clear();

    // Buffers de la UI 2D: van mapeados, así que primero Unmap.
    for (UINT i = 0; i < kFrameCount; ++i) {
        if (d.uiVertexAllocations[i]) {
            d.uiVertexAllocations[i]->GetResource()->Unmap(0, nullptr);
            d.uiVertexAllocations[i]->Release();
            d.uiVertexAllocations[i] = nullptr;
            d.uiVertexMapped[i]      = nullptr;
            d.uiVertexCapacity[i]    = 0;
        }
        if (d.uiIndexAllocations[i]) {
            d.uiIndexAllocations[i]->GetResource()->Unmap(0, nullptr);
            d.uiIndexAllocations[i]->Release();
            d.uiIndexAllocations[i] = nullptr;
            d.uiIndexMapped[i]      = nullptr;
            d.uiIndexCapacity[i]    = 0;
        }
    }
    d.uiPipeline.Reset();
    d.uiRootSignature.Reset();

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
