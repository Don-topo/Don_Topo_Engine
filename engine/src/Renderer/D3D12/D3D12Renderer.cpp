#include "DonTopo/Renderer/IkBlock.h"
#include "DonTopo/Renderer/PoseBlock.h"
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
#include "DonTopo/Renderer/MaterialTextureSource.h"
#include "DonTopo/Renderer/SlotPool.h"
#include "DonTopo/Renderer/D3D12/D3D12Support.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/PlaceholderTexture.h"
#include "DonTopo/Renderer/RenderConstants.h"
#include "DonTopo/Renderer/SelectionOutline.h"
#include "DonTopo/Renderer/Plane.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Renderer/SplashScreen.h"  // loadSplashImage (no Vulkan use)
#include "DonTopo/Renderer/MeshClock.h"
#include "DonTopo/Renderer/SkinnedMeshPacking.h"
#include "DonTopo/Renderer/SharedTextureCache.h"
#include "DonTopo/Renderer/TaaJitter.h"
#include "DonTopo/Renderer/UiLayer.h"
#include "DonTopo/Renderer/UniformBufferObject.h"
#include "DonTopo/Renderer/Vertex.h"
#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiWidgetSync.h"

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
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace DonTopo::D3D12 {

namespace {

using Microsoft::WRL::ComPtr;

// A vertex of the gizmo geometry: it is EXACTLY what shaders/gizmo.vert declares
// (location 0 = position, location 1 = color), and that is why the input layout
// can go against the translated DXIL without adapting anything.
struct GizmoVertex {
    float pos[3];
    float color[3];
};

// Light block of the UBO. Same layout as DonTopo::Light and as the Light struct
// in shaders/triangle.frag.
struct ShaderLight {
    float position[4];
    float color[4];
    float direction[4];  // .w = type: 0 point, 1 spot, 2 directional, 3 area
    float params[4];     // range, inner cos, outer cos, width
};

// The set 0 binding 0 UBO, with the EXACT offsets that spirv-cross generates when
// translating the GLSL. The static_asserts below are the only real defense: a
// CPU/GPU offset mismatch raises no error in any validation layer, only odd
// pixels.
//
// The concrete registers go on each field and NOT in this list, which went
// stale every time the block grew: the ones here were those from before
// lightSpaceMatrix became 12 matrices and MAX_LIGHTS became 64.
struct SceneUbo {
    glm::mat4   view;                    // c0
    glm::mat4   proj;                    // c4
    // SIX and not four: it is the maximum between the 4 cascades of a directional and
    // the 6 faces of a point light's cubemap, which never coexist. When it grows it
    // shifts EVERYTHING behind it by 128 bytes, hence the offsets below.
    // TEN: six from the key light (4 cascades, or 6 cubemap faces, or 1 for a spot)
    // plus four secondary spots of one layer each. When it grows it shifts everything
    // behind it, hence the offsets.
    glm::mat4   lightSpaceMatrix[SHADOW_MATRICES];   // c8
    glm::vec4   cascadeSplits;           // c56
    ShaderLight lights[MAX_LIGHTS];      // c57
    glm::vec4   viewPos;                 // c313
    int         numLights;               // c314
    // In the padding gap that was already behind numLights, just like in
    // GLSL: no earlier offset moves.
    float       ambientIntensity;
};

static_assert(offsetof(SceneUbo, view) == 0, "UBO: view must be at c0");
static_assert(offsetof(SceneUbo, proj) == 64, "UBO: proj must be at c4");
static_assert(offsetof(SceneUbo, lightSpaceMatrix) == 128, "UBO: lightSpaceMatrix must be at c8");
static_assert(offsetof(SceneUbo, cascadeSplits) == 896, "UBO: cascadeSplits must be at c56");
static_assert(offsetof(SceneUbo, lights) == 912, "UBO: lights must be at c57");
// The three behind the array moved 3072 bytes when MAX_LIGHTS went from 16
// to 64 (48 more lights x 64 bytes): 1936 -> 5008, 1952 -> 5024, 1956 -> 5028.
static_assert(offsetof(SceneUbo, viewPos) == 5008, "UBO: viewPos must be at c313");
static_assert(offsetof(SceneUbo, numLights) == 5024, "UBO: numLights must be at c314");
static_assert(offsetof(SceneUbo, ambientIntensity) == 5028,
              "UBO: ambientIntensity sits right after numLights");
// The offsets above are fixed numbers ON PURPOSE: they describe the layout that
// the HLSL packoffsets declare, which does not come from this file. Computing them
// from MAX_LIGHTS would make them follow the array and they would stop catching exactly
// the failure they guard against: C++ and the shader no longer counting the same thing.
//
// This assert is the one that warns: raising the cap shifts viewPos and numLights, so
// the six GLSL files, the translated HLSL and the three offsets above must be touched
// IN THE SAME commit. Without it, the UBO would be read shifted and silently.
static_assert(MAX_LIGHTS == 64,
              "MAX_LIGHTS changed: adjust the offsets above and the "
              "#define in shaders/lights_config.glsl, or the shader will read a "
              "shifted block");

// Push constants de triangle.vert/pbr.frag: mat4 + 2 float + vec2 = 80 bytes.
struct PushData {
    glm::mat4 transform;
    float     metallic;
    float     roughness;
    glm::vec2 flags;  // x: 1 = take the model from the instance SSBO
};
static_assert(sizeof(PushData) == 80, "PushData must take 80 bytes (20 root constants)");

// Push constants of the three animation computes. The three share a 16-byte
// block; in bone_hierarchy and skinning the fourth field is not read.
// Mirror of SkinningPass::Push (Vulkan): both backends compile THE SAME
// .comp files, so this block and that one must match field by field.
struct ComputePush {
    uint32_t boneCount;
    uint32_t vertexCount;
    // --- Only read by bone_eval.comp ---
    uint32_t rootMotionMode;    // 0 free, 1 root pinned to bind, 2 only X and Z
    uint32_t poseBlockOffset;   // in uints: copy of the frame's pose block
    // --- bone_ik.comp y bone_hierarchy.comp ---
    uint32_t ikBlockOffset;     // in uints: copy of the frame's IK block
    uint32_t flags;             // bit 0: the hierarchy writes world only
};
static_assert(sizeof(ComputePush) == 24, "ComputePush: mirror of SkinningPass::Push and of the 4 .comp shaders");

// Half float by hand: the IBL neutrals are four texels and it is not worth
// dragging in DirectXMath for them. It works for normal and small values, which
// is all that is passed to it.
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

// IBL. The sizes live in RenderConstants.h, shared with the Vulkan path: they
// were duplicated here with a comment saying they were "the same", which was
// the only defense against them ceasing to be.
constexpr UINT kIblIrradianceSize = IBL_IRRADIANCE_SIZE;
constexpr UINT kIblPrefilterSize  = IBL_PREFILTER_SIZE;
constexpr UINT kIblPrefilterMips  = IBL_PREFILTER_MIPS;

// Side of each face when capturing a probe. 128 is what the Vulkan path uses: it
// is more than enough for the prefiltering and six faces at higher resolution are
// not noticeable in a reflection, which is already blurred by roughness.
constexpr UINT kProbeFaceSize = 128;

// Forward+. Same values as Renderer.h: the grid, the per-cell cap and the
// light cap are taken for granted by both culling computes and pbr.frag.
constexpr uint32_t kFpMaxLights     = 256;
constexpr uint32_t kFpMaxPerCell    = 64;
// Words of the culling statistics block, the same layout that
// light_cull_tiled.comp declares: [0] lights distributed, [1] cells with any light,
// [2] overflowed cells, [3] unused.
constexpr uint32_t kFpStatsWords    = 4;
constexpr uint32_t kFpTileSize      = 16;  // tiled
constexpr uint32_t kFpClusterTile   = 64;  // clustered, XY
constexpr uint32_t kFpClusterSlices = 24;  // clustered, Z

// A light as culling wants it: the same as in the UBO plus the radius
// and its view space position, which avoids recomputing it per cell.
struct FpLightGpu {
    glm::vec4 posRadius;
    glm::vec4 color;
    glm::vec4 viewPosR;
    glm::vec4 direction;
    glm::vec4 params;
};

// Parameter block read by the culling and pbr.frag.
struct FpParamsGpu {
    uint32_t mode, gridX, gridY, gridZ;
    uint32_t tileSize, maxPerCell, numLights, pad0;
    float    zNear, zFar, sliceScale, sliceBias;
};

// Culling push: the four projection coefficients and the screen
// size.
struct FpPush {
    float    p00, p11, p22, p32;
    uint32_t screenW, screenH, pad0, pad1;
};

// Cascaded shadows. All three come from UniformBufferObject.h, which this file
// already includes: they were copied here with their value hardcoded, and nothing forced
// them to keep matching. The SceneUbo static_asserts would NOT have caught it:
// they guard the block offsets, not how many layers the shadow map has.
constexpr int   kShadowCascades    = SHADOW_CASCADES;
constexpr int   kShadowLayers      = SHADOW_MATRICES;
constexpr int   kShadowKeyLayers   = SHADOW_KEY_MATRICES;
// DEFAULT side of the shadow map. The one in use lives in RendererState
// (shadowResolution) and is applied by applyPendingShadowSize.
constexpr UINT  kShadowMapSizeDefault = 2048;
// kCascadeLambda and kShadowMaxDistance no longer live here: the user chooses them and
// they come from RendererState (cascadeLambda/shadowDistance), just like in Vulkan.

// Bloom: levels of the reduction chain. Same number the Vulkan path
// uses (its log says "5 mips").
constexpr int kBloomMips = BLOOM_MIPS;

// Format of the target the scene is drawn into. Floating point and not UNORM: the
// bloom threshold only makes sense if the color can exceed 1.0, which is
// exactly what a normalized backbuffer clips.
constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// Composition target, with tone mapping already applied. The final pass
// (FXAA/TAA/SSAA) reads from here to write the backbuffer.
constexpr DXGI_FORMAT kLdrFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

// Push shared by bloom_down and bloom_up: vec2 + 3 float + int = 24 bytes.
struct BloomPush {
    float srcTexel[2];  // 1 / size of the SOURCE level
    float threshold;
    float knee;
    float radius;    // solo lo usa el upsample
    int   prefilter; // != 0 only on the first downsample level
};
static_assert(sizeof(BloomPush) == 24, "BloomPush must take 24 bytes");

// Layout of the descriptor heap. The first three must be contiguous
// because the mesh shader asks for them as t1..t3, and sceneHdr/bloomMip0 too
// because the composition one asks for them as t0..t1.
constexpr UINT kSrvBaseColor = 0;
constexpr UINT kSrvNormalMap = 1;
constexpr UINT kSrvShadowMap = 2;
// t4..t7 of the global block. They are the neutrals: metallic-roughness at white
// (ao = 1 and the push factors unscaled), the two cubemaps with a flat
// ambient and occlusion at 1. pbr.frag ALWAYS samples them, so they
// have to exist even when there is no material, environment or SSAO.
constexpr UINT kSrvMetalRough = 3;
constexpr UINT kSrvIrradiance = 4;
constexpr UINT kSrvPrefilter  = 5;
constexpr UINT kSrvSsao       = 6;
constexpr UINT kSrvSceneHdr   = 7;
constexpr UINT kSrvBloomMip  = kSrvSceneHdr + 1;            // + level
constexpr UINT kUavBloomMip  = kSrvBloomMip + kBloomMips;   // + level
constexpr UINT kSrvDepth     = kUavBloomMip + kBloomMips;   // depth, for the fog
constexpr UINT kUavSceneHdr  = kSrvDepth + 1;               // the fog writes over the scene
constexpr UINT kSrvLdr       = kUavSceneHdr + 1;            // composition output, for FXAA
// Range reserved for ImGui. One is not enough: since 1.92 its DX12 backend
// asks for descriptors on its own (one per texture, not just the font) through
// the allocation callbacks passed to it when it is initialized.
constexpr UINT kSrvImGui      = kSrvLdr + 1;
constexpr UINT kImGuiReserved = 16;

// Per-object descriptor block. pbr.frag asks for t1..t7 as ONE contiguous
// table, so each mesh needs its seven slots in a row, in this
// order: base color, normals, shadows, metallic-roughness, irradiance,
// prefiltered and screen-space occlusion. The last four and the shadow one are
// shared resources: their view is created again inside each block,
// which is legal and avoids splitting the root signature (copying descriptors from a
// shader-visible heap is not allowed by the API).
//
// Objects beyond the cap are drawn with the global block, which carries
// the neutrals: they look flat, but the heap is never exceeded.
constexpr UINT kSrvPerObject   = 7;
constexpr UINT kMaxObjectSlots = 512;
constexpr UINT kSrvObjects     = kSrvImGui + kImGuiReserved;

// And the same for the skinned mesh, which is drawn by submeshes: each one
// has its material in the FBX and therefore its own block.
constexpr UINT kMaxSkinnedSlots = 16;
constexpr UINT kSrvSkinned      = kSrvObjects + kMaxObjectSlots * kSrvPerObject;

// Sky cubemap: a single view, that of the TextureCube sampled by t0 of
// skybox.frag.
constexpr UINT kSrvSkybox   = kSrvSkinned + kMaxSkinnedSlots * kSrvPerObject;

// Targets of the two IBL computes: the whole irradiance and one level of the
// prefiltered map per mip. They are write targets, so they go as UAVs and do not share
// a slot with the read views used by pbr.frag.
constexpr UINT kUavIrradiance = kSrvSkybox + 1;
constexpr UINT kUavPrefilter  = kUavIrradiance + 1;  // + mip

// SSAO: the pre-pass depth, the raw map and the blurred one. The
// final result is read by the per-object blocks in their t7 slot; these are
// those of the chain that produces it.
constexpr UINT kSrvPrepassDepth = kUavPrefilter + kIblPrefilterMips;
constexpr UINT kUavSsaoRaw      = kSrvPrepassDepth + 1;
constexpr UINT kSrvSsaoRaw      = kUavSsaoRaw + 1;
constexpr UINT kUavSsaoBlur     = kSrvSsaoRaw + 1;

// Screen-space reflections: the trace target, which the resolve then adds
// onto the scene.
constexpr UINT kUavSsr = kUavSsaoBlur + 1;
constexpr UINT kSrvSsr = kUavSsr + 1;

// Motion blur: blur target. The shader reads arbitrary pixels of the
// scene along the velocity, so it cannot write over the image it
// samples; the copy back comes from here. UAV only: the copy does not need a
// view.
constexpr UINT kUavMotionBlur = kSrvSsr + 1;

// TAA history: two images that alternate, because the same pass reads the
// one from the previous frame and writes this frame's.
constexpr UINT kSrvTaaHistory = kUavMotionBlur + 1;  // + index (0 or 1)

// Viewport image: the already composed scene, when instead of going to the
// backbuffer it has to end up inside an interface panel.
constexpr UINT kSrvViewport = kSrvTaaHistory + 2;

// 2D UI atlases: one per sprite-sheet and one per font. The cap is
// real (past it the UI is drawn without its texture, the heap is not exceeded) and with
// 16 there is plenty for a game interface with several fonts.
constexpr UINT kSrvUiAtlas    = kSrvViewport + 1;
constexpr UINT kMaxUiAtlases  = 16;

// Shared thumbnail atlas of the Content Browser: ONE slot of its own, so as not to
// spend one of the 16 of the game's 2D UI.
constexpr UINT kSrvThumbAtlas = kSrvUiAtlas + kMaxUiAtlases;

// ─── Reflection probes ───────────────────────────────────────────────────────
// Each probe has the same as the global IBL (irradiance and prefiltered
// environment) plus the cubemap where the scene is captured before
// convolving it. Eight per scene: each takes ~1 MB across the three
// images, and past the cap the objects keep the global IBL, which is
// degrading, not failing.
constexpr UINT kMaxProbes = 8;

// Slots per probe, in this order: capture (SRV), irradiance (SRV+UAV) and
// prefiltered (SRV + one UAV per mip, since each level is a different roughness and
// is dispatched separately).
constexpr UINT kSrvPerProbe = 1                     // capture
                            + 1 + 1                 // irradiance: read and write
                            + 1 + kIblPrefilterMips;// prefiltered: read and one UAV per mip
constexpr UINT kSrvProbes   = kSrvThumbAtlas + 1;

// Contiguous pair to compose with bloom OFF. bloom_composite.frag
// does `color += bloom * intensity` ALWAYS, and its table asks for [scene, bloom]
// in a row, so turning the effect off cannot be solved by sending intensity=0:
// the mip chain comes from a heap that was not zeroed and in R16G16B16A16_FLOAT
// that can be an inf, so `inf * 0` gives NaN. Vulkan avoids it by clearing
// the chain to black (see Renderer::setBloomEnabled); here it is cheaper to have
// a second pair (the same scene and a 1x1 black) and not record a single
// command with bloom off.
constexpr UINT kSrvCompositeOff = kSrvProbes + kMaxProbes * kSrvPerProbe;  // +0 scene, +1 black

// The startup splash logo (beginSplash).
constexpr UINT kSrvSplash = kSrvCompositeOff + 2;

constexpr UINT kSrvHeapSize = kSrvSplash + 1;

// Push of ssao.comp and ssao_blur.comp: both share the block, so the
// root constants range has to be the same for both pipelines.
struct SsaoPush {
    glm::vec4 projParams;  // p00, p11, p22, p32 of the frame's projection
    glm::vec2 invRes;
    float     radius;
    float     bias;
    float     intensity;
    float     power;
};
static_assert(sizeof(SsaoPush) == 40, "SsaoPush must take 40 bytes");

// Push of ssr.comp and ssr_resolve.comp, which share a block just like the two
// of SSAO.
struct SsrPush {
    glm::vec4 projParams;
    glm::vec2 invRes;
    float     maxDistance;
    float     thickness;
    int32_t   maxSteps;
    int32_t   refineSteps;  // binary search over the last segment
    float     edgeFade;
    float     intensity;
};
static_assert(sizeof(SsrPush) == 48, "SsrPush must take 48 bytes");

// Push of taa.frag: the reprojection to the previous frame and the history weight.
struct TaaPush {
    glm::mat4 reproject;
    glm::vec2 invRes;
    float     feedback;
    int32_t   historyValid;
};
static_assert(sizeof(TaaPush) == 80, "TaaPush must take 80 bytes");

// Push of motion_blur.comp: the same reprojection as TAA plus the three
// settings of the effect.
struct MotionBlurPush {
    glm::mat4 reproject;
    glm::vec2 invRes;
    float     intensity;
    float     maxRadius;  // cap of the trail, in pixels
    int32_t   samples;
};
static_assert(sizeof(MotionBlurPush) == 84, "MotionBlurPush must take 84 bytes");

// Volumetric fog: its own 128-byte push.
struct FogPush {
    glm::mat4 invViewProj;
    glm::vec4 camPosDensity;      // xyz = camera in world, w = base density
    glm::vec4 lightDirFalloff;    // xyz = key light direction, w = height falloff
    glm::vec4 scatterBaseHeight;  // rgb = scattering already multiplied by the light, a = reference height
    glm::vec4 gStepsRes;          // x = anisotropy, y = steps, zw = resolution
};
static_assert(sizeof(FogPush) == 128, "FogPush must take 128 bytes");

// FXAA: vec2 + 3 float = 20 bytes.
struct FxaaPush {
    float invRes[2];
    float subpix;
    float edgeThreshold;
    float edgeThresholdMin;
};
static_assert(sizeof(FxaaPush) == 20, "FxaaPush must take 20 bytes");

// Push of ssaa_resolve.frag: the inverse of the size of the LARGE image (the
// source) and how many samples per axis to average.
struct SsaaPush {
    float invSrc[2];
    int   taps;
};
static_assert(sizeof(SsaaPush) == 12, "SsaaPush must take 12 bytes");

// Stride of the vertex written by skinning.comp: 5 vec4 (pos, color, uv, normal,
// tangent). There is no equivalent C++ struct in the engine, so the literal size is used.
constexpr UINT kSkinnedOutputStride = 5 * sizeof(glm::vec4);

// Constant buffers are bound with the address aligned to 256 bytes.
constexpr UINT64 kCbvAlignment = 256;

UINT64 alignUp(UINT64 value, UINT64 alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

std::vector<char> readBinaryFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open())
        throw std::runtime_error("D3D12: could not open '" + path + "'");

    const std::streamsize size = in.tellg();
    if (size <= 0)
        throw std::runtime_error("D3D12: '" + path + "' is empty");

    std::vector<char> data(static_cast<size_t>(size));
    in.seekg(0);
    in.read(data.data(), size);
    if (!in)
        throw std::runtime_error("D3D12: incomplete read of '" + path + "'");
    return data;
}

// Triple buffering: two frames in flight while the GPU works on the third. It is
// the same criterion used by the engine's Vulkan swapchain.
constexpr UINT kFrameCount = 3;

// ── RTV heap layout ─────────────────────────────────────────────────────────
//
// The SRV heap has had named indices from the start (kSrvObjects,
// kUavPrefilter…); the RTV one had been left without them. It asked for
// `kFrameCount + 6 + 6` and the thirteen places that use it added their offset by hand
// (`kFrameCount + 2`, `kFrameCount + 3 + i`, `kFrameCount + 5`…), without a single
// static_assert. It fit exactly by chance, and that `6 + 6` did not mean
// what it looked like: the first 6 is FIVE targets plus one, not a group (H44).
//
// Adding a target meant writing outside the heap, which in D3D12 raises no error when
// creating the view: the write lands in another descriptor's memory and the failure
// shows up later, somewhere else and with no apparent relation.
constexpr UINT kRtvBackBuffer  = 0;                  // one per swapchain image
constexpr UINT kRtvSceneHdr    = kFrameCount;        // HDR color of the scene
constexpr UINT kRtvLdr         = kRtvSceneHdr + 1;   // result after the tonemap
constexpr UINT kRtvSceneMsaa   = kRtvLdr + 1;        // multisampled scene
// TAA history: two of them, alternating per frame (ping-pong).
constexpr UINT kRtvTaaHistory      = kRtvSceneMsaa + 1;
constexpr UINT kRtvTaaHistoryCount = 2;
// Texture the editor viewport draws to when it is not presented directly.
constexpr UINT kRtvViewport    = kRtvTaaHistory + kRtvTaaHistoryCount;
// The six faces of ONE probe: they are redone for each probe that gets baked, which goes
// one at a time.
constexpr UINT kRtvProbeFace   = kRtvViewport + 1;
constexpr UINT kRtvProbeFaces  = 6;
// sRGB views of the swapchain images, one per image, used only by the startup
// splash: the swapchain is UNORM, and writing through an sRGB view is what makes
// the splash look exactly like the Vulkan one (sRGB swapchain there).
constexpr UINT kRtvSplash      = kRtvProbeFace + kRtvProbeFaces;
// What has to be requested. Derived, so that adding a target above moves it
// by itself instead of forcing someone to remember.
constexpr UINT kRtvCount       = kRtvSplash + kFrameCount;

static_assert(kRtvCount == 2 * kFrameCount + 12,
              "The RTV heap layout changed: check that nobody adds "
              "offsets by hand and that kRtvCount still covers the last index");

std::string hresultToString(HRESULT hr)
{
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

// ─── Diagnostics: log to FILE ────────────────────────────────────────────────
// The editor is launched from the explorer and nobody sees its stdout; the D3D12
// debug layer writes through OutputDebugString, which is not visible without a debugger
// either. Everything here goes ADDITIONALLY to a file next to the executable, which is the
// only thing the user can bring when what they see is "an error window
// with no message".
//
// It is INSTRUMENTATION: it does not change render behavior, it only reports
// what happens when something goes wrong.
std::ofstream& diagStream()
{
    // Function static: it is opened the first time someone writes and
    // closed when the process exits. `app` and not `trunc`: if the editor were
    // relaunched, the log of the previous session is still there.
    static std::ofstream out("d3d12_diag.log", std::ios::out | std::ios::app);
    return out;
}

void diagLog(const std::string& line)
{
    std::ofstream& out = diagStream();
    if (!out)
        return;
    const auto ahora = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    localtime_s(&tm, &ahora);
    char sello[32] = {};
    std::snprintf(sello, sizeof(sello), "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    // flush on every line: if the process dies at the next instruction, what
    // was just written has to be ALREADY on disk.
    out << sello << line << std::endl;
    // And also to standard output, for whoever does have a console.
    std::fputs((std::string(sello) + line + "\n").c_str(), stderr);
}

// Hook to the DRED dump. Impl::init installs it when the device exists, and
// throwIfFailed calls it (it is a free function and has no device) when the
// HRESULT that aborts it is a device loss. Without this, the only
// place that looks at DXGI_ERROR_DEVICE_REMOVED would be Present.
void (*g_volcarDeviceRemoved)(const char*, HRESULT) = nullptr;

bool esPerdidaDeDevice(HRESULT hr)
{
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR ||
           hr == DXGI_ERROR_INVALID_CALL;
}

// Every creation failure aborts init with the specific step that failed: a
// half-built device cannot be used and hiding the HRESULT only moves the crash
// further along.
void throwIfFailed(HRESULT hr, const char* step)
{
    if (FAILED(hr)) {
        // The dump BEFORE throwing: the exception goes up to main and from there to
        // process termination, and by then the device is no longer there for
        // anyone to ask it anything.
        if (esPerdidaDeDevice(hr) && g_volcarDeviceRemoved)
            g_volcarDeviceRemoved(step, hr);
        throw std::runtime_error(std::string("D3D12: ") + step + " failed (HRESULT " +
                                 hresultToString(hr) + ")");
    }
}

// narrow() used to live here, copied byte for byte from D3D12Support.cpp (H48). Now
// there is only one, declared in D3D12Support.h: both converted text that
// ends up in the same log, so diverging would have produced two different
// encodings for the same string without anything warning.
using DonTopo::D3D12::narrow;

}  // namespace

struct D3D12Renderer::Impl {
    ComPtr<IDXGIFactory4>       factory;
    ComPtr<IDXGIAdapter1>       adapter;
    ComPtr<ID3D12Device>        device;
    ComPtr<ID3D12CommandQueue>  queue;
    ComPtr<IDXGISwapChain3>     swapChain;

    // D3D12MemoryAllocator keeps its own reference counter with
    // Release(), it is not a regular COM object: ComPtr does not work.
    D3D12MA::Allocator* allocator = nullptr;

    // Gizmo geometry: the simplest path the scene already uses. Its vertex
    // buffer lives in a DEFAULT heap suballocated by D3D12MA.
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> gizmoPipeline;
    D3D12MA::Allocation*        gridAllocation = nullptr;
    D3D12_VERTEX_BUFFER_VIEW    gridVertexBufferView{};
    UINT                        gridVertexCount = 0;

    // ── Mesh with material: the triangle.vert/triangle.frag path ────────────
    ComPtr<ID3D12RootSignature> meshRootSignature;
    ComPtr<ID3D12PipelineState> meshPipeline;
    // The same shaders and the same root signature, but filling only the
    // edges. They are created alongside the solid ones so nothing has to be redone when changing
    // mode: the switch only chooses which one is bound.
    ComPtr<ID3D12PipelineState> meshWirePipeline;

    // Inverted hull of the selected object: the same mesh extruded along its
    // normal and with front faces discarded, so only the
    // rim shows. It shares the root signature with the mesh (outline.vert declares the
    // same UBO and the same push), and that is why it needs nothing of its own.
    // Debug lines of the frame: colliders, axes, rays. They are sent from
    // outside before drawing and do NOT persist to the next frame, just like in the
    // Vulkan path.
    D3D12MA::Allocation*     debugLinesAllocation = nullptr;
    void*                    debugLinesMapped     = nullptr;
    size_t                   debugLinesCapacity   = 0;   // in vertices
    UINT                     debugLineVertices    = 0;   // those of THIS frame
    D3D12_VERTEX_BUFFER_VIEW debugLinesView{};

    void ensureDebugLineBuffer(size_t vertexCount);

    ComPtr<ID3D12PipelineState> outlinePipeline;
    ComPtr<ID3D12PipelineState> outlineSkinnedPipeline;
    // The same ones against the LDR target, which is where the outline is drawn: it goes
    // AFTER tone mapping so its orange does not go through ACES. The ones
    // above stay because they are the ones fixed by the HDR format and the scene pass
    // samples, from which these inherit everything else.
    ComPtr<ID3D12PipelineState> outlineLdrPipeline;
    ComPtr<ID3D12PipelineState> outlineSkinnedLdrPipeline;

    // Draws the inverted hull of the selection onto the LDR target, with the
    // scene's depth so that it only shows where it is really visible.
    void recordSelectionOutline(D3D12_CPU_DESCRIPTOR_HANDLE rtv);
    // Whether there is something selected to draw. The pre-pass queries it: with MSAA,
    // the scene pass depth is multisampled and the outline needs
    // a single-sample one, which is exactly what that pre-pass produces.
    bool hasOutlineSelection() const;
    int   selectedObject  = -1;   // index into objects, -1 with no selection
    int   selectedSkinned = -1;   // index into skinnedObjects
    // In world units: with fine-detail meshes a thick hull also shows through
    // the inner gaps and the outline stops reading as a
    // silhouette. 1 cm works well for human-scale characters.
    float outlineWidth    = 0.01f;

    // Static geometry of the scene. Each entry is a mesh uploaded to VRAM
    // with its transform; the index that addStaticMesh returns is the
    // position in this vector, just like in the Vulkan Renderer.
    struct StaticObject {
        D3D12MA::Allocation*     vertexAllocation = nullptr;
        D3D12MA::Allocation*     indexAllocation  = nullptr;
        D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
        D3D12_INDEX_BUFFER_VIEW  indexBufferView{};
        UINT                     indexCount = 0;
        glm::mat4                transform{1.0f};
        bool                     meshVisible = true;

        // The material's own textures, or nullptr if the mesh has none (or the
        // file could not be read): in that case the triplet points at the global 1x1
        // ones and the object comes out with a flat color, as before.
        D3D12MA::Allocation* baseColorAllocation  = nullptr;
        D3D12MA::Allocation* normalMapAllocation  = nullptr;
        D3D12MA::Allocation* metalRoughAllocation = nullptr;

        // First slot of its triplet in the heap. kSrvBaseColor = the global one.
        UINT  srvBase   = kSrvBaseColor;
        float metallic  = 0.0f;
        float roughness = 0.6f;
        // Whether the material has an ORM map. It is decided on register and on rebuild,
        // which is where the material is at hand, and setObjectMaterialFactors
        // queries it: with a map, the sliders cannot override the
        // texture and both factors stay at 1.0.
        bool  hasOrmMap = false;

        // Reflection strength of the object. pbr.frag dumps it into the scene's
        // alpha, and from there the trace reads it: at zero, that pixel does not reflect.
        float ssrStrength = 0.0f;

        // Bounding box in LOCAL space, for frustum culling. It is computed
        // when uploading the mesh because it does not depend on where the object is. A
        // mesh without vertices cannot be bounded: hasBounds = false and then it is
        // always drawn, which is the safe failure.
        glm::vec3 aabbMin{0.0f};
        glm::vec3 aabbMax{0.0f};
        bool      hasBounds = false;

        // ── Shared mesh ─────────────────────────────────────────────────────
        // Index of the object that UPLOADED these buffers and these textures. A hundred
        // identical cubes all point to the first one: the vertexBufferViews
        // above are copies of its own, not resources of their own. Without this, grouping
        // "by same mesh" would group zero objects, because each one would have its
        // own copy in VRAM.
        int  sharedMesh = -1;
        // Only the owner releases. A duplicate that released the buffers would leave
        // the others drawing with freed memory, and nobody warns about that.
        bool ownsGpu = true;
        // How many live objects point to THIS mesh. Only the owner keeps it
        // (duplicates stay at 0). Today nobody deletes objects one by one
        // (removeGameObject turns them off, see setObjectMeshVisible) so the
        // only one that releases is clearStaticMeshes and it takes everything; the counter
        // is there so that the day a standalone delete appears it CANNOT release
        // the buffers with duplicates still drawing them. The guard lives in
        // releaseStaticObject, which is what deletes, and not in a list of places
        // from which calling is allowed.
        int  sharedRefs = 0;
        // Content key with which this object entered sharedMeshOwner,
        // empty if it is not the owner. When releasing it that entry has to be removed from the
        // map: otherwise, a new mesh with the same key would reuse the buffers
        // of an already recycled slot, which by then contains ANOTHER mesh.
        std::string sharedKey;
        // Retired owner with duplicates still alive. It cannot release its
        // buffers or give up its slot (the duplicates are still drawing them), so
        // it stays as storage; the last duplicate to die finishes it off.
        bool pendingRelease = false;
        // The slot is in the pool. Prevents a second removeGameObject on
        // the same subtree from getting in here again.
        bool slotFree = false;

        // Draw group: same mesh AND equivalent descriptor block.
        // The objects of a group are painted with a single instanced draw. -1
        // while the groups have not been rebuilt.
        int  drawGroup = -1;

        // Goes up every time one of this object's textures is swapped for the
        // "could not be read" neutral. It enters the group key: two objects
        // with the same mesh but different filler no longer see the same thing, so
        // they cannot share the draw.
        uint32_t materialVariant = 0;
    };
    std::vector<StaticObject> objects;

    // Content key -> object that uploaded that mesh. The entry is deleted
    // when that object is released: since slots are recycled (P11), an old
    // index can point to a completely different mesh.
    std::unordered_map<std::string, int> sharedMeshOwner;

    // Free slots of `objects`. Recycling them ALSO recycles their descriptor
    // block, because the block is derived from the index
    // (kSrvObjects + index * kSrvPerObject).
    SlotPool objectSlots;
    // Triplets of the skinned range that have become free, by absolute srvBase.
    // They are kept apart from `nextSkinnedSlot` because they are handed out per SUBMESH and a
    // character takes as many as it has materials.
    std::vector<UINT> freeSkinnedSrv;
    SlotPool          skinnedSlots;

    // Releases the resources of object `index` and returns its slot to the pool.
    // Returns whether the slot became free: an owner with live duplicates CANNOT give
    // it up, and then it stays turned off until the last duplicate dies.
    bool releaseObjectSlot(size_t index);
    // The same for a character. It also returns the triplets of its submeshes
    // to the pool.
    bool releaseSkinnedSlot(size_t index);

    // One representative per draw group: the buffers, the descriptor triplet and
    // the PBR factors come from it, which are the same for the whole group.
    std::vector<int> drawGroupRep;
    bool             drawGroupsDirty = true;
    void             rebuildDrawGroups();

    // The ONLY place that releases the resources of a static object. The decision
    // of whether it can or not lives in here, and not in a list of authorized
    // callers: a duplicate never owns anything, and the owner only releases when
    // nobody else is left pointing at its mesh. Returns whether it released.
    bool releaseStaticObject(StaticObject& object);

    // Per-instance matrices of the scene. shadow.vert takes the model from here
    // ALWAYS (it has no push constant path) and triangle.vert when the draw
    // is instanced. It goes in an upload heap and mapped: it is rewritten every frame
    // because transforms change.
    //
    // ONE PER FRAME IN FLIGHT, and not a single one: since the split is decided by
    // grouping, the CONTENT changes from frame to frame (culling moves
    // objects between ranges). Rewriting a single buffer while the GPU reads the
    // previous frame would mix both splits, and the symptom would be geometry
    // showing up in another one's place.
    std::array<D3D12MA::Allocation*, kFrameCount> sceneInstanceAllocations{};
    std::array<void*, kFrameCount>                sceneInstanceMapped{};
    std::array<size_t, kFrameCount>               sceneInstanceCapacity{};

    // The buffer goes in two ranges of this size: 0 for shadows and pre-pass
    // (everything visible) and 1 for the main pass (what also passes the
    // frustum). Two different splits of the same set of objects.
    size_t instanceRegionStride = 0;
    static constexpr uint32_t kInstanceRegionShadow = 0;
    static constexpr uint32_t kInstanceRegionScene  = 1;

    void ensureSceneInstanceBuffer(size_t count);
    // GPU address of matrix `index` in THIS frame's buffer. Instanced draws
    // point the root SRV at the start of their group's range and
    // draw with StartInstanceLocation = 0, instead of leaving the view at the
    // start of the buffer and moving the base instance.
    //
    // The reason: the shader comes from GLSL, where gl_InstanceIndex DOES include the
    // base instance by specification, and spirv-cross translates it to
    // SV_InstanceID, where that is not guaranteed to be the same. On the machine where
    // it was tested it works both ways (checked by comparing the render
    // against the per-object path) but shifting the view costs the same and
    // does not depend on the driver.
    D3D12_GPU_VIRTUAL_ADDRESS instanceAddress(uint32_t index) const;

    // Candidates and groups of the frame. They are members and not locals so as not to
    // reallocate their vectors on every pass of every frame.
    std::vector<Batching::BatchCandidate> batchCandidates;
    std::vector<Batching::InstanceBatch>  shadowBatches;
    std::vector<Batching::InstanceBatch>  sceneBatches;

    // Fills range 0 with everything visible. It is shared by the shadow pass
    // (which draws the four cascades of the same split) and the depth
    // pre-pass, which see the same set.
    void buildShadowBatches();

    // Per-instance matrices (set 1, binding 0 in GLSL → t0 space1 in HLSL).
    D3D12MA::Allocation* instanceAllocation = nullptr;

    // Scene UBO: one per frame in flight, persistently mapped. Without
    // separating by slot, writing it while the GPU reads the previous frame would give
    // a half-updated image.
    std::array<D3D12MA::Allocation*, kFrameCount> sceneUboAllocations{};
    std::array<void*, kFrameCount>                sceneUboMapped{};

    // Material textures. Today they are generated 1x1: the engine's cube is
    // procedural and has none, but the shaders require them anyway.
    D3D12MA::Allocation* baseColorAllocation = nullptr;
    // The "this texture is missing" checkerboard. Separate from the white one above on
    // purpose: white is the legitimate filler of a primitive without
    // material, and this one is the signal that a declared file could not be
    // read. See PlaceholderTexture.h.
    D3D12MA::Allocation* missingTextureAllocation = nullptr;
    D3D12MA::Allocation* normalMapAllocation = nullptr;
    D3D12MA::Allocation* shadowMapAllocation = nullptr;

    // Neutrals of t4..t7: they always exist, and they are what a mesh without a
    // metallic-roughness material or a scene without an environment sees.
    D3D12MA::Allocation* metalRoughAllocation = nullptr;
    D3D12MA::Allocation* irradianceAllocation = nullptr;
    D3D12MA::Allocation* prefilterAllocation  = nullptr;
    // Mips the prefilter has NOW: one with the neutral, and the real ones
    // once the compute generates it. A view that declares more mips than the
    // resource has is an invalid descriptor: it does not fail when created, it takes
    // the device down when something uses it.
    UINT                 prefilterMips        = 1;
    D3D12MA::Allocation* ssaoAllocation       = nullptr;
    // 1x1 black in HDR format: the "bloom" that the composition reads when the
    // effect is off (see kSrvCompositeOff).
    D3D12MA::Allocation* bloomBlackAllocation = nullptr;

    // Forward+ off, but the four space2 buffers EXIST: pbr.frag declares them
    // without a branch, and a root SRV at zero is an out-of-resource read.
    // With mode = 0 the shader does not even look at them, but they have to be bound.
    D3D12MA::Allocation* fpParamsAllocation  = nullptr;
    D3D12MA::Allocation* fpLightsAllocation  = nullptr;
    // Per-light range sent by the caller, in the same order as setLights.
    // Empty (or shorter than the light list) means "for that light, the global
    // radius from RendererState". Same criterion as ForwardPlusPass in
    // Vulkan: it is the only piece of data Light does not carry in the UBO, because it does not fit without
    // moving the std140 layout that 5 shaders declare.
    std::vector<float> lightRadii;
    D3D12MA::Allocation* fpCellsAllocation   = nullptr;
    D3D12MA::Allocation* fpIndicesAllocation = nullptr;

    // t1..t7 of space0, in this order.
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    UINT                         srvSize = 0;

    // Depth. It is recreated with the window.
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    D3D12MA::Allocation*         depthAllocation = nullptr;

    // ── Character animated by compute ───────────────────────────────────────
    // Three chained passes, the same ones as the Vulkan path:
    //   bone_eval      animation keys -> local transforms
    //   bone_hierarchy locals + hierarchy -> final bone matrices
    //   skinning       vertices + matrices -> already deformed vertices
    ComPtr<ID3D12RootSignature> boneEvalRootSignature;
    ComPtr<ID3D12RootSignature> boneHierarchyRootSignature;
    ComPtr<ID3D12RootSignature> boneIkRootSignature;
    ComPtr<ID3D12RootSignature> skinningRootSignature;
    ComPtr<ID3D12PipelineState> boneEvalPipeline;
    ComPtr<ID3D12PipelineState> boneHierarchyPipeline;
    ComPtr<ID3D12PipelineState> boneIkPipeline;
    ComPtr<ID3D12PipelineState> skinningPipeline;
    ComPtr<ID3D12PipelineState> skinnedMeshPipeline;
    ComPtr<ID3D12PipelineState> skinnedMeshWirePipeline;

    // Submeshes of a character: the FBX brings one material per piece (body,
    // hair, clothes…), and drawing it in one go forced giving all of them the
    // same texture. One draw per range with its triplet.
    struct SkinnedSubMesh {
        UINT indexStart = 0;
        UINT indexCount = 0;
        UINT srvBase    = kSrvBaseColor;
        // Per SUBMESH and not per character, because that is what they are: one material
        // per submesh, just like the texture triplet next to it. While
        // they lived outside of here they were two constants in the draw pass
        // (0.0 and 0.7), so in this backend a character could not be
        // metallic, neither with the slider nor with an ORM map: the bug that motivated the
        // factors feature, alive only for skinned. The defaults are those of
        // Material (Material.h) so that a submesh that never gets
        // filled in looks like the untouched mesh, not like an invented value.
        float metallic  = 0.0f;
        float roughness = 0.5f;
    };

    // An animated character. Each one has its skeleton, its keys and its
    // buffer of deformed vertices: the skinning compute writes there, and the
    // graphics pass reads it as a vertex buffer in the same frame.
    struct SkinnedObject {
        D3D12MA::Allocation* posKeys     = nullptr;
        D3D12MA::Allocation* rotKeys     = nullptr;
        D3D12MA::Allocation* scaleKeys   = nullptr;
        D3D12MA::Allocation* boneInfos   = nullptr;
        D3D12MA::Allocation* inputVerts  = nullptr;
        D3D12MA::Allocation* localXforms = nullptr;
        D3D12MA::Allocation* finalBones  = nullptr;
        // TRS of the pose (written by bone_eval) and the frozen one, 3 vec4 per bone.
        D3D12MA::Allocation* poseTrs     = nullptr;
        D3D12MA::Allocation* frozenTrs   = nullptr;
        // Pose block (PoseBlock.h), one copy per frame in flight, in an
        // UPLOAD heap mapped forever.
        D3D12MA::Allocation* poseBlock   = nullptr;
        void*                poseBlockMapped = nullptr;
        // Copy of the pose masks (the Animator's only lasts during
        // setAnimationPose).
        std::vector<uint8_t> poseMasks[kMaxLayersPose];
        // IK: what the Animator sends and its block, with one copy per frame.
        AnimationIk          ik;
        D3D12MA::Allocation* ikBlock = nullptr;
        void*                ikBlockMapped = nullptr;
        D3D12MA::Allocation* outputVerts = nullptr;
        D3D12MA::Allocation* indices     = nullptr;

        D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
        D3D12_INDEX_BUFFER_VIEW  indexBufferView{};
        UINT                     indexCount  = 0;
        uint32_t                 boneCount   = 0;
        uint32_t                 vertexCount = 0;
        // Clip blocks of the BoneInfos SSBO, to bound the index that
        // comes from outside before multiplying it by boneCount (clampClipIndex).
        uint32_t                 clipCount   = 1;
        // Longest side of the REST POSE box, in local space. Used by
        // the outline thickness, which is proportional to the object's size.
        float                    restMaxExtent = 0.0f;

        // boneInfos uses a [clip][bone] layout: the offset of the active clip is
        // clip * boneCount.
        uint32_t clipBase     = 0;
        // animTime and animDuration are in Assimp TICKS, which is what the
        // compute reads. Without ticksPerSecond one cannot go from the frame's seconds
        // to ticks: it was missing, and that is why this backend played the path
        // without an Animator between 24 and 30 times slower (A13).
        float    animTime       = 0.0f;
        float    animDuration   = 0.0f;
        float    ticksPerSecond = 0.0f;
        // The pose sent by an Animator (setAnimationPose). Without it, a single
        // sample: clipBase at animTime.
        AnimationPose pose;
        bool          hasPose = false;
        // Who is in charge of animTime. As soon as someone outside moves it
        // (updateAnimation or setAnimationState) the backend stops advancing it on
        // its own: both clocks adding up would leave the clip at double.
        bool     externalClock = false;

        glm::mat4 transform{1.0f};
        bool      visible = true;
        // Its slot is in the pool: see releaseSkinnedSlot.
        bool      slotFree = false;
        float     ssrStrength = 0.0f;

        std::vector<SkinnedSubMesh>       subMeshes;
        std::vector<D3D12MA::Allocation*> textures;
    };
    std::vector<SkinnedObject> skinnedObjects;
    // Material textures of the characters, shared among those coming
    // from the same FBX (before, each one uploaded its own copy: ~218 MB per character with
    // modelAnimation.fbx). The views are still from each one's triplet.
    SharedTextureCache<D3D12MA::Allocation*> skinnedTextures;
    // Read-only buffers of a character (B6): keys, bone infos, input vertices
    // and indices, shared among clones by content key (skinnedGeometryKey).
    // The per-character ones (pose, output vertices) stay in SkinnedObject.
    struct SkinnedGeometry {
        D3D12MA::Allocation* posKeys = nullptr;
        D3D12MA::Allocation* rotKeys = nullptr;
        D3D12MA::Allocation* scaleKeys = nullptr;
        D3D12MA::Allocation* boneInfos = nullptr;
        D3D12MA::Allocation* inputVerts = nullptr;
        D3D12MA::Allocation* indices = nullptr;
        bool operator==(const SkinnedGeometry& o) const
        {
            return inputVerts == o.inputVerts && indices == o.indices;
        }
    };
    SharedTextureCache<SkinnedGeometry> skinnedGeometry;
    // Everything a character holds: shared geometry and textures through their
    // caches, its own buffers, and its SRV triplets back to the pool. The ONLY
    // release path: three hand-written copies had drifted (the rebuild one
    // freed shared textures behind the cache's back).
    void releaseSkinnedResources(SkinnedObject& character);

    // Slots of the skinned range already handed out, in triplets. They are not reused when
    // deleting a single object because characters are loaded all at once with the
    // scene; clearSkinnedMeshes returns it to zero.
    UINT nextSkinnedSlot = 0;

    // Character matrices for the shadow pass, for the same reason as
    // sceneInstanceAllocation: shadow.vert always takes the model from the SSBO.
    // One per frame in flight, for the same reason as the scene's: it is
    // rewritten from the CPU every frame while the GPU may still be reading the
    // previous one. Here the split is fixed (one matrix per character, at its
    // index), so the worst the single buffer produced was a shadow with the
    // transform of one frame earlier; with a single buffer that can neither be
    // detected nor ruled out, and doing it right costs the same.
    std::array<D3D12MA::Allocation*, kFrameCount> skinnedInstanceAllocations{};
    std::array<void*, kFrameCount>                skinnedInstanceMapped{};
    std::array<size_t, kFrameCount>               skinnedInstanceCapacity{};

    // Address of the matrix of character `index`. Like instanceAddress:
    // the view is shifted instead of moving the base instance, which in HLSL is not
    // guaranteed to reach SV_InstanceID.
    D3D12_GPU_VIRTUAL_ADDRESS skinnedInstanceAddress(size_t index) const
    {
        return skinnedInstanceAllocations[frameIndex]->GetResource()->GetGPUVirtualAddress() +
               static_cast<UINT64>(index) * sizeof(glm::mat4);
    }

    // ── Sky ─────────────────────────────────────────────────────────────
    ComPtr<ID3D12RootSignature> skyboxRootSignature;
    ComPtr<ID3D12PipelineState> skyboxPipeline;
    D3D12MA::Allocation*        skyboxAllocation = nullptr;

    // Loads the six faces and builds the cubemap. Silent if any is missing: the
    // background stays at the clear color, which is what there was before.
    // The six faces of the sky. By default the usual ones, so that a
    // caller that never uses initSkybox behaves as before.
    std::array<std::string, 6> skyboxFacePaths = {
        "assets/skybox/px.png", "assets/skybox/nx.png", "assets/skybox/py.png",
        "assets/skybox/ny.png", "assets/skybox/pz.png", "assets/skybox/nz.png"};
    // Only the cubemap and its view, without the pipeline: it is the only thing that has to be
    // redone when changing sky, and this way the reload does not leak the PSO.
    bool loadSkyboxCubemap();
    void createSkyboxResources();
    // Only the root signature and the pipeline: they are redone when changing sample count,
    // without reloading the six faces.
    void createSkyboxPipelineOnly();
    void recordSkybox();

    ComPtr<ID3D12RootSignature> iblRootSignature;
    ComPtr<ID3D12PipelineState> iblIrradiancePipeline;
    ComPtr<ID3D12PipelineState> iblPrefilterPipeline;

    // Convolves the sky cubemap into the two maps that pbr.frag consumes:
    // irradiance for the diffuse and roughness-prefiltered for the specular.
    // It replaces the neutrals; with no sky loaded it does nothing.
    void precomputeIbl();

    // Flat environment for when there is no cubemap, and the four Forward+
    // buffers that pbr.frag declares without a branch.
    void createNeutralIblCubes();
    void createForwardPlusBuffers();

    // Binds the four space2 root SRVs. The three passes that use the mesh root
    // signature (statics, ground and characters) need them.
    void bindForwardPlus();

    // ── Screen-space ambient occlusion ──────────────────────────────────
    // The pre-pass writes ITS depth, not the scene pass's: both computes
    // read it as a texture, and the scene pass has not yet run
    // when it is needed (pbr.frag consumes the result).
    ComPtr<ID3D12PipelineState> depthPrepassPipeline;         // engine vertices
    ComPtr<ID3D12PipelineState> depthPrepassSkinnedPipeline;  // skinning output
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

    // Samples BUILT right now in the targets and in the scene pass
    // pipelines. What the user requests lives in the shared state; the
    // two only match after recreating, and that happens between frames.
    UINT sampleCount = 1;

    // Multisampled color and depth. They only exist with MSAA active: the scene pass
    // draws there and on closing it is resolved onto the usual HDR,
    // which is the one consumed by SSR, fog, bloom and composition.
    D3D12MA::Allocation* hdrMsAllocation   = nullptr;
    D3D12MA::Allocation* depthMsAllocation = nullptr;

    // Samples requested by the state, already validated against what the
    // device supports: 1 if the mode is not MSAA.
    UINT desiredSampleCount() const;

    // The depth that fog and reflections can READ. With MSAA the scene
    // pass one is multisampled and is not sampled like a normal texture,
    // so the pre-pass one is used, which is why it is also recorded when
    // SSAO is off.
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
    // And what state it is in outside those passes: the pre-pass one is left by its
    // own recording, the scene one lives in depth-write.
    D3D12_RESOURCE_STATES readableDepthState() const
    {
        return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
    void applyPendingSampleCount();  // recreates targets and pipelines if it changed

    ComPtr<ID3D12RootSignature> fpCullRootSignature;
    ComPtr<ID3D12PipelineState> fpTiledPipeline;
    ComPtr<ID3D12PipelineState> fpClusteredPipeline;

    // The two input ones are mapped: they are rewritten every frame with the camera and
    // the live lights. The three output ones are filled by the culling on the GPU.
    void*  fpParamsMapped = nullptr;
    void*  fpLightsMapped = nullptr;
    D3D12MA::Allocation* fpStatsAllocation = nullptr;
    // Reading the culling statistics. The compute accumulates them with
    // atomicAdd in fpStatsAllocation, which lives in a DEFAULT heap and the CPU cannot
    // read: they are copied to a READBACK buffer with one region per frame in
    // flight and read kFrameCount frames later, exactly the same as the
    // timestamps (see readTimestamps).
    ComPtr<ID3D12Resource> fpStatsReadback;
    const uint32_t*        fpStatsMapped = nullptr;
    // Four zeros in an UPLOAD heap. The shader ACCUMULATES, so the buffer has
    // to be at zero before every dispatch, and in D3D12 a DEFAULT buffer cannot
    // be memset from the CPU: this is copied over it.
    D3D12MA::Allocation*   fpStatsZeros       = nullptr;
    void*                  fpStatsZerosMapped = nullptr;
    float                  fpAvgPerCell       = 0.0f;
    uint32_t               fpOverflowCells    = 0;
    uint32_t fpCellCount = 0;  // cells the output ones are sized for

    // The lists stay as read while the scene pass consumes them;
    // the next frame returns them to write before rebuilding them.
    bool fpListsInPixelState = false;

    void createForwardPlusPipelines();
    void ensureForwardPlusGrid(uint32_t cells);
    void updateForwardPlus();   // parameters and lights of the frame
    void recordForwardPlusCull();

    // Tree of the game's 2D interface, ONE per CanvasComponent in the
    // scene, same scheme as the Vulkan Renderer (UiCanvasSlot,
    // matched by ownerId with matchUiCanvasSlots). The editor keeps
    // editing the screen one via uiCanvas() without asking which backend it
    // runs on.
    std::vector<std::unique_ptr<UiCanvasSlot>> uiSlots;
    // Fallback of uiCanvas() with no screen canvas in the scene.
    // Persistent and empty: a reference to a temporary would leave the editor
    // reading dead memory.
    UiCanvas uiCanvasFallback;

    // Editor interface layer, if there is one. It is not owned by this backend.
    UiLayer* uiLayer = nullptr;

    // The scene and its root, stored for whoever asks. This backend does not
    // walk them: the geometry comes in through registerGameObject.
    Scene*      scene     = nullptr;
    GameObject* sceneRoot = nullptr;

    // Stored so the panel keeps the value; this backend does not draw at
    // higher resolution yet.

    // Alternative target of the final pass. With this on, the backbuffer only
    // carries interface, and the scene travels as a texture to whoever draws it.
    D3D12MA::Allocation* viewportAllocation = nullptr;
    bool                 renderToTexture    = false;

    ComPtr<ID3D12RootSignature> taaRootSignature;
    ComPtr<ID3D12PipelineState> taaPipeline;
    std::array<D3D12MA::Allocation*, 2> taaHistoryAllocations{};

    // Which of the two histories is written this frame. The other one is read.
    UINT      taaHistoryIndex = 0;
    bool      taaHistoryValid = false;
    // ─── First use of render targets ─────────────────────────────────────────
    // D3D12MA allocates with D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, and the first use of
    // a render target coming from such a heap requires a Discard/Clear/Copy BEFORE
    // the draw (even if the draw covers it entirely). Without that, the debug layer
    // emits an id=1422 per resource at startup and on EVERY recreation on
    // resize, and those errors bury the real ones in d3d12_diag.log.
    //
    // One per resource because each one is first used at a different moment: the
    // TAA history does ping-pong and first uses 0 in one frame and 1 in the
    // next. ldrAllocation is declared further below, with the rest of the post.
    std::array<bool, 2> taaHistoryInicializada{};
    bool                viewportInicializado = false;
    bool                ldrInicializado      = false;

    // First-uses `resource` if it had not been. It must be called with the resource ALREADY in
    // RENDER_TARGET: that is what DiscardResource requires. Discard and not Clear because
    // the draw that follows writes every pixel, so there is no need to
    // pay for the fill; all that is needed is to stop lying to the API
    // about whether the contents matter.
    void estrenarRenderTarget(ID3D12Resource* recurso, bool& estrenado)
    {
        if (estrenado || !recurso)
            return;
        commandList->DiscardResource(recurso, nullptr);
        estrenado = true;
    }
    uint32_t  taaJitterIndex  = 0;
    glm::mat4 taaCurrViewProj{1.0f};
    glm::mat4 taaPrevViewProj{1.0f};
    // Projection of the frame WITH the subpixel offset. Outside TAA it is
    // the camera's as is.
    glm::mat4 taaJitteredProj{1.0f};

    void createTaaPipeline();
    void recordTaa(D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv);

    void createSsrPipelines();
    void recordSsr();
    void createMotionBlurPipeline();
    void recordMotionBlur();
    // On, with resources and with taps to average. Queried by the pass and by
    // the depth pre-pass, which with MSAA is where the depth comes from.
    bool motionBlurActive() const;

    void createSsaoPipelines();
    void createSsaoTargets();    // depends on the size: it is redone on resize
    void releaseSsaoTargets();
    void recordDepthPrepassAndSsao();

    // The blur stays as a read resource while the scene pass
    // samples it; the next frame has to return it to write
    // before dispatching it again.
    bool ssaoBlurNeedsUav = false;

    // ── Lights ──────────────────────────────────────────────────────────
    // Those of the scene, as sent by whoever loads it. Empty = no
    // scene has set them yet, and then the backend's filler directional is
    // used, which is what lights the startup scene.
    std::vector<ShaderLight> sceneLights;

    // ── Camera ──────────────────────────────────────────────────────────
    // A single place: the grid, the mesh, the fog and the cascade split
    // each had their own copied lookAt, and touching just one was enough to make the
    // ground stop falling under the objects.
    glm::vec3 cameraPos{6.0f, 4.5f, 8.0f};
    glm::mat4 cameraView =
        glm::lookAtRH(glm::vec3(6.0f, 4.5f, 8.0f), glm::vec3(0.0f, 0.5f, 0.0f),
                      glm::vec3(0.0f, 1.0f, 0.0f));
    float cameraFovDeg = 60.0f;

    // Characteristic size of the scene, from which near and far come in edit mode
    // (near = /1000, far = x3), just like in the Vulkan path. It is recomputed by
    // refitCameraRange when the geometry changes; before that the range was
    // pinned to 0.1-500 and a deeper scene got clipped.
    float cameraDistance = 200.0f;

    // perspectiveRH_ZO, not plain perspective: D3D12 clips at z=[0,1] just
    // like Vulkan, and with the OpenGL convention the near half is lost.
    // Projection used to draw. It is decided, in this order, by:
    //   1. the face of a probe being baked (90 degrees, square and with the
    //      long range), which EVERYTHING drawn by that face has to see;
    //   2. the scene's CameraComponent, while Play runs;
    //   3. the edit one, with the fov pushed by the editor.
    // Resolving it here and not through a parameter is what keeps the UBO, the fog,
    // the sky and the culling from disagreeing.
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

    // Projection of the CameraComponent, while it is in charge. It is resolved once per
    // frame (resolveFrameCamera) and not on every query: cameraProj() is called
    // about ten times per frame and looking for the camera in the tree every time
    // would be walking the scene ten times for nothing.
    std::optional<glm::mat4> sceneCameraProj;
    void                     resolveFrameCamera();

    void ensureSkinnedInstanceBuffer(size_t count);

    // Releases the GPU resources of all characters. It does NOT wait for the GPU: the
    // two places that call it (shutdown and clearSkinnedMeshes) have already done so.
    void releaseSkinnedObjects();

    LARGE_INTEGER lastTick{};
    LARGE_INTEGER tickFrequency{};

    // ── Solid ground ────────────────────────────────────────────────────────
    // The grid is lines and does not receive shadow: a real surface is needed
    // for anything projected to be visible.
    D3D12MA::Allocation*     groundVertexAllocation = nullptr;
    D3D12MA::Allocation*     groundIndexAllocation  = nullptr;
    D3D12MA::Allocation*     groundInstanceAllocation = nullptr;
    D3D12_VERTEX_BUFFER_VIEW groundVertexBufferView{};
    D3D12_INDEX_BUFFER_VIEW  groundIndexBufferView{};
    UINT                     groundIndexCount = 0;

    // ── Cascaded shadows ────────────────────────────────────────────────────
    ComPtr<ID3D12RootSignature> shadowRootSignature;
    ComPtr<ID3D12PipelineState> shadowPipeline;         // engine vertices (56 B)
    ComPtr<ID3D12PipelineState> shadowSkinnedPipeline;  // compute output (80 B)
    ComPtr<ID3D12DescriptorHeap> shadowDsvHeap;         // one DSV per cascade
    D3D12MA::Allocation*         shadowMapArrayAllocation = nullptr;
    // Side of the map that is BUILT right now, and the one the UI requests and has not yet
    // been applied (0 = nothing pending). The change is not made where it is requested:
    // releasing the map in the middle of a frame would pull it from under the
    // in-flight command list.
    UINT shadowMapSize        = kShadowMapSizeDefault;
    UINT pendingShadowMapSize = 0;
    // Last side already recorded in the log, so as not to repeat the
    // line every frame.
    UINT loggedShadowSize     = 0;
    void applyPendingShadowSize();
    UINT                         dsvSize = 0;

    glm::mat4 cascadeMatrices[kShadowLayers]{};
    glm::vec4 cascadeSplits{0.0f};
    // How many layers of the map have a valid matrix in THIS frame: 4 with the
    // cascades of a directional, 1 with the perspective face of a spot, 0
    // with no lights. It only bounds the DRAWS; the clear of each layer is done anyway.
    // The adapter allows presenting without waiting for the refresh. It is resolved when
    // creating the swapchain, which is when the flag has to be requested.
    bool      tearingDisponible = false;

    UINT      activeLayers = 0;
    // Secondary spots with a slot. They take layers [kShadowKeyLayers, +extra).
    UINT      extraLayers  = 0;
    // Slot of each light, or -1 if it casts none. Light 0 is always -1: the key uses the
    // first kShadowKeyLayers.
    int       shadowSlot[MAX_LIGHTS] = {};
    // Faces each light took: 1 one face, SHADOW_KEY_MATRICES a cubemap.
    int       shadowFaces[MAX_LIGHTS] = {};
    // Light direction: the same one written to the UBO. The position is only
    // used for orientation, just like in computeCascades.
    glm::vec3 lightDirection{-0.4f, -1.0f, -0.5f};

    // ── Offscreen scene and bloom ───────────────────────────────────────────
    // The scene no longer goes straight to the backbuffer: it is drawn into an HDR target, the
    // bloom works on it and a composition pass writes the result.
    D3D12MA::Allocation* hdrAllocation = nullptr;
    // Levels with a useful size. The reservation is always kBloomMips (the descriptor
    // layout takes it for granted), but with a small viewport fewer are used,
    // just like in Vulkan.
    UINT                 bloomMipCount = 0;
    D3D12MA::Allocation* bloomMipAllocations[kBloomMips]{};
    UINT                 bloomMipWidth[kBloomMips]{};
    UINT                 bloomMipHeight[kBloomMips]{};

    ComPtr<ID3D12RootSignature> bloomRootSignature;
    ComPtr<ID3D12PipelineState> bloomDownPipeline;
    ComPtr<ID3D12PipelineState> bloomUpPipeline;
    ComPtr<ID3D12RootSignature> compositeRootSignature;
    ComPtr<ID3D12PipelineState> compositePipeline;

    // Quality and effects state shared with the Vulkan backend. It is
    // D3D12Renderer itself: the Impl does not copy it, it queries it, so that a
    // setBloomIntensity() from outside shows up in the next frame without
    // synchronizing anything.
    RendererState* state = nullptr;

    // The bloom tent radius is NOT in RendererState: the Vulkan
    // Renderer does not expose it as a setting, so it stays local.
    float bloomRadius = 1.0f;

    // ── Fog and FXAA ────────────────────────────────────────────────────────
    // The fog writes ON the HDR target before the bloom; FXAA goes last,
    // on the already composed result and in LDR range.
    ComPtr<ID3D12RootSignature> fogRootSignature;
    ComPtr<ID3D12PipelineState> fogPipeline;
    ComPtr<ID3D12RootSignature> fxaaRootSignature;
    ComPtr<ID3D12PipelineState> fxaaPipeline;
    ComPtr<ID3D12RootSignature> ssaaRootSignature;
    ComPtr<ID3D12PipelineState> ssaaPipeline;

    // ── Startup splash (beginSplash / drawSplashFrame) ─────────────────────
    ComPtr<ID3D12RootSignature> splashRootSignature;
    ComPtr<ID3D12PipelineState> splashPipeline;
    D3D12MA::Allocation*        splashLogo       = nullptr;
    float                       splashLogoAspect = 1.0f;

    // ─── Game 2D UI ──────────────────────────────────────────────────────────
    // The quads are built by UiCanvas on the CPU (buildDrawData, which knows of no
    // API) and here they are only uploaded and drawn. A pair of buffers per frame in
    // flight, mapped and growing by doubling: the content changes
    // entirely every frame and a staging buffer does not pay off.
    ComPtr<ID3D12RootSignature> uiRootSignature;
    ComPtr<ID3D12PipelineState> uiPipeline;

    // The TWO variants of WORLD canvas. They share root signature, shaders and
    // vertex layout with the screen one; the only thing that changes is that they draw
    // into the SCENE target (kHdrFormat + D32_FLOAT + sampleCount) and the depth
    // test: one so that a wall hides the sign and another for what
    // always goes on top, like a health bar.
    ComPtr<ID3D12PipelineState> uiWorldPipelineDepth;    // geometry hides it
    ComPtr<ID3D12PipelineState> uiWorldPipelineNoDepth;  // always on top

    std::array<D3D12MA::Allocation*, kFrameCount> uiVertexAllocations{};
    std::array<void*, kFrameCount>                uiVertexMapped{};
    std::array<UINT, kFrameCount>                 uiVertexCapacity{};
    std::array<D3D12MA::Allocation*, kFrameCount> uiIndexAllocations{};
    std::array<void*, kFrameCount>                uiIndexMapped{};
    std::array<UINT, kFrameCount>                 uiIndexCapacity{};

    // Suballocation cursors INSIDE the buffer of the current frame. They are
    // MEMBERS and not locals of a function because two passes share them: the
    // world canvases are recorded in the scene one and the screen ones in the
    // composition one, and the second has to continue where the first left off. With
    // a cursor local to each one, the screen ones would write over the
    // vertices of the world ones, which the GPU has not read yet, because it reads the
    // buffer when the command list EXECUTES, not when it is recorded. beginUiFrame() sets them
    // to 0 once per frame.
    UINT uiVertexCursor = 0;
    UINT uiIndexCursor  = 0;
    // What beginUiFrame() counted for the SCREEN canvases: they are the
    // only ones recorded by recordUiCanvas(), and with 0 there is nothing to draw.
    UINT uiScreenVertices = 0;
    UINT uiScreenIndices  = 0;
    // The world ones in paint order, far to near. A member and not a local
    // so as not to reallocate the vector every frame.
    std::vector<UiCanvasSlot*> uiWorldOrder;

    // The whole geometry pass, so it can be repeated from another camera: it is
    // what the baking of a reflection probe needs.
    void recordSceneGeometry(D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                             UINT targetWidth, UINT targetHeight);

    void createUiPipeline();
    // The two world variants, against the SCENE target. It also serves as
    // "recreate": if there were any, it releases them before compiling the new ones. It
    // has to be called EVERY TIME sampleCount changes, or the PSO stays compiled
    // for a sample count that is no longer the target's, and in D3D12 that
    // shows up as device lost, not as a debug layer error.
    void createUiWorldPipelines();
    void ensureUiBuffers(UINT vertexCount, UINT indexCount);

    // ONCE per frame, BEFORE the scene pass (which is where the world
    // canvases are recorded). It builds the draw data of ALL canvases, computes the
    // model matrix of the world ones, sizes the buffer pair with the
    // total of the whole frame and sets the cursors to 0. See the comment on
    // uiVertexCursor: splitting it into two calls, one per pass, silently corrupts the
    // buffer.
    void beginUiFrame();

    // Suballocates this canvas's slot in the frame buffer, checks that it
    // fits, copies and binds the two views. Returns false if it does not fit: better not to
    // draw that canvas than to write OUTSIDE the mapped memory, which is a
    // HOST write that no validation layer sees. A single one for both
    // paths (world and screen) so the guard cannot stay in only one.
    bool bindUiCanvasGeometry(const UiDrawData& data);

    // The WORLD canvases, at the end of the scene pass: after the geometry
    // and the sky, so the depth already written occludes them.
    void recordWorldCanvases(UINT targetWidth, UINT targetHeight);

    // transform is proj*view*model already multiplied: for screen canvases it
    // is the usual orthographic (computed by the caller), and for a world
    // one it also carries the camera and the canvas matrix.
    void recordUiCanvas(D3D12_CPU_DESCRIPTOR_HANDLE targetRtv, const glm::mat4& transform);

    // UI atlases and fonts. The backend owns them: the widgets only
    // keep the pointer, which is also the key with which the batch says which
    // texture it wants.
    std::vector<std::unique_ptr<UiTextureAtlas>>          uiAtlases;
    std::vector<std::unique_ptr<UiFont>>                  uiFonts;
    // By PATH: the same image requested twice is the same atlas, and this way the
    // editor touches the sprites of the atlas being drawn, not those of a
    // copy. Same criterion as the Vulkan backend.
    std::unordered_map<std::string, UiTextureAtlas*>      uiAtlasByPath;
    std::unordered_map<const UiTextureAtlas*, UINT>       uiAtlasSrv;
    std::vector<D3D12MA::Allocation*>                     uiAtlasTextures;
    UINT                                                  uiNextAtlasSlot = 0;

    // Shared thumbnail atlas (see EditorRenderer::uiThumbnailAtlasId). It is
    // created the first time it is requested; if it fails, it is not retried every frame.
    D3D12MA::Allocation* thumbAtlas       = nullptr;
    bool                 thumbAtlasFailed = false;
    bool ensureThumbAtlas();
    bool uploadThumbnailTiles(const ThumbnailTile* tiles, size_t count);

    // Uploads the pixels the atlas already has loaded and creates its SRV. false
    // if there is no slot or the upload fails: the batch will be drawn with the 1x1 white.
    bool registerUiAtlas(UiTextureAtlas& atlas);

    // ─── Reflection probes ───────────────────────────────────────────────────
    struct GpuProbe {
        uint64_t  ownerId  = 0;   // GameObject::id of the probe
        glm::vec3 position{0.0f};
        float     radius    = 0.0f;
        float     intensity = 1.0f;

        // The three images: the scene capture and the two that come out of
        // convolving it, which are the ones that end up in t4 and t5 of the objects.
        D3D12MA::Allocation* captureAllocation    = nullptr;
        D3D12MA::Allocation* irradianceAllocation = nullptr;
        D3D12MA::Allocation* prefilterAllocation  = nullptr;

        // First slot of its block in the heap. The rest comes from adding, in the
        // order set by kSrvPerProbe.
        UINT srvBase = 0;

        bool  baked  = false;  // false: still shows the global IBL
        float bakeMs = 0.0f;   // last bake, measured with the timestamps

        // The bake was left half done (the GPU rejected a list). Without this,
        // "not baked" would make it retry on EVERY frame, and each attempt
        // waits on the GPU seven times: a permanent failure would leave the editor
        // crawling. It is cleared when the probe changes, which is when trying
        // again makes sense once more.
        bool  bakeFailed = false;
    };
    std::vector<GpuProbe> probes;

    // Indices inside a probe's block.
    static constexpr UINT kProbeCaptureSrv    = 0;
    static constexpr UINT kProbeIrradianceSrv = 1;
    static constexpr UINT kProbeIrradianceUav = 2;
    static constexpr UINT kProbePrefilterSrv  = 3;
    static constexpr UINT kProbePrefilterUav  = 4;  // + mip

    // Creates the three images of a probe and their views. false if there is no slot left
    // in the heap or the GPU does not give the memory.
    bool createProbeResources(GpuProbe& probe);
    void releaseProbe(GpuProbe& probe);

    // Reconciles the list with the scene's ReflectionProbeComponents: creates
    // the new ones, releases those that are gone and refreshes position, radius and
    // intensity. Per frame, and returns right away when there are none.
    void syncProbes();

    // The two IBL computes over any input and destinations:
    // used by the global sky and by each probe.
    void recordIblConvolution(UINT sourceSrv, UINT irradianceUav, UINT prefilterUav,
                              float intensity);

    // Captures the scene from the probe (six faces) and convolves the result
    // into its two cubemaps. It is an EVENT, not a pass of the frame: it waits on the GPU,
    // takes its time and leaves the probe marked as baked.
    void bakeProbe(GpuProbe& probe);

    // Depth of the bake's own: the frame's buffer has the size of the
    // render and here the faces are kProbeFaceSize.
    D3D12MA::Allocation* probeDepthAllocation = nullptr;
    void createProbeDepth();

    // Pending requests: the editor asks to bake and it is handled in the next
    // frame, outside any half-done recording.
    std::vector<uint64_t> probeBakeQueue;
    bool                  probeBakeAllQueued = false;
    float                 probeLastBakeMs    = 0.0f;

    // Which probe each object looks at, by its index in `probes`; -1 = the global IBL.
    // It is stored so as not to rewrite descriptors in a frame where nothing
    // changed, which is the normal case.
    std::vector<int> probeAssignStatic;
    std::vector<int> probeAssignSkinned;

    // The probe that applies to a world point: the nearest of those that
    // contain it. -1 if none reaches it.
    int  pickProbeFor(const glm::vec3& worldPos) const;
    // Rewrites t4 and t5 of the blocks whose probe has changed. Returns
    // how many were touched.
    int  refreshProbeAssignment();
    // Leaves t4/t5 of that block pointing at the given probe, or at the global IBL.
    void writeProbeSlots(UINT blockBase, int probeIndex);
    D3D12MA::Allocation*        ldrAllocation = nullptr;

    // The fog and FXAA parameters also come from RendererState.

    // User interface. It is drawn by whoever knows ImGui, not this backend.
    std::function<void()> uiDrawCallback;

    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    UINT                         rtvSize = 0;

    std::array<ComPtr<ID3D12Resource>, kFrameCount>         renderTargets;
    std::array<ComPtr<ID3D12CommandAllocator>, kFrameCount> allocators;
    ComPtr<ID3D12GraphicsCommandList>                       commandList;

    // One fence value per slot: frame N can only reuse its allocator
    // when the GPU has passed the value assigned to it the last time.
    ComPtr<ID3D12Fence>                fence;
    std::array<UINT64, kFrameCount>    fenceValues{};
    HANDLE                             fenceEvent = nullptr;

    // ─── GPU times ───────────────────────────────────────────────────────────
    // Two marks per pass (entry and exit) and one more pair for the whole frame.
    // The performance panel asks for them one by one, so they are stored per pass and
    // not as a total.
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
        // Motion blur was measured in NO backend: the pass had existed for
        // a long time and its cost did not show up in the panel.
        TsMotionBlur  = 18,
        TsCount       = 20,
    };
    ComPtr<ID3D12QueryHeap> timestampHeap;
    ComPtr<ID3D12Resource>  timestampReadback;
    const UINT64*           timestampMapped   = nullptr;
    UINT64                  timestampFreq     = 0;
    std::array<float, TsCount / 2> gpuMs{};

    void createTimestampResources();
    void markTimestamp(UINT slot);
    // PerformancePanel switch. With the panel closed no queries are recorded
    // nor draws counted: in Vulkan it was already so (m_perfCapture) and here
    // it was a no-op, so the switch lied and the counts of the two
    // backends could not be compared.
    bool perfCapture = true;
    void readTimestamps();     // those of the previous frame, before overwriting
    void resolveTimestamps();  // dumps this frame's into the read buffer

    // Frame counts, for the panel: they are filled in when recording the main pass.
    int statDraws       = 0;
    int statInstanced   = 0;
    int statCulledCount = 0;

    // With no editor in front: the grid and gizmos, which belong to it, are turned off. The
    // runtime turns it on before starting.
    bool headless = false;

    UINT frameIndex = 0;
    // Size at which the scene is DRAWN: depth, HDR, occlusion, bloom and the
    // LDR go at this one. With SSAA it is a multiple of the output one.
    UINT width      = 0;
    UINT height     = 0;
    // Size at which the image is DELIVERED: the backbuffer or the panel's texture.
    // Without SSAA it matches the render one, and then the final pass is a blit
    // with filtering; with SSAA it is smaller and that pass averages.
    UINT outWidth   = 0;
    UINT outHeight  = 0;

    // Size recorded by the window callback, pending to apply. See
    // the comment of resize() in the header: the DXGI work cannot be
    // done inside the WindowProc.
    // Size of the swapchain, which is the window's. width and height are the
    // RENDER size: they match this one except when the scene goes to a
    // panel, and then the panel's measurements rule.
    UINT swapWidth  = 0;
    UINT swapHeight = 0;

    // Render size requested from outside, pending to apply between frames.
    UINT pendingRenderWidth  = 0;
    UINT pendingRenderHeight = 0;

    void applyPendingRenderSize();

    UINT pendingWidth   = 0;
    UINT pendingHeight  = 0;
    bool resizePending  = false;

    // Background color in LINEAR space, which is what the HDR target expects. The
    // composition pass applies ACES and gamma 2.2 to it, so the 0.10 from before
    // (when the scene went straight to the backbuffer) would now come out as a mid
    // gray. These values are the ones that give the usual background on screen.
    float       clearColor[4] = {0.02f, 0.02f, 0.025f, 1.0f};
    std::string adapterName;
    HWND        hwnd        = nullptr;
    bool        initialized = false;

    // ─── Diagnostics ─────────────────────────────────────────────────────────
    // The message queue of the debug layer, so it can be DRAINED to a
    // file: by itself it only writes through OutputDebugString.
    ComPtr<ID3D12InfoQueue> infoQueue;
    // The DRED dump is done ONCE: the device loss is detected by
    // several places in a row (Present, throwIfFailed, shutdown) and repeating the
    // same breadcrumb listing only buries the first one, which is the good one.
    bool deviceRemovedVolcado = false;

    // The fence protocol broke. It matters because ALL callers of
    // waitForGpu() release resources right after it returns: if the Signal
    // failed, the wait waited for nobody and what gets released may still be
    // in the GPU's hands. From here on no more work is submitted.
    //
    // The warning is NOT given by throwing from waitForGpu/moveToNextFrame: the
    // destructor (~D3D12Renderer) goes through shutdown() and from there to waitForGpu(),
    // and an exception leaving a destructor is std::terminate. It is flagged
    // here and drawFrame throws it, which is outside that path and is where
    // Present already throws for the same reason.
    bool        deviceLost      = false;
    HRESULT     deviceLostHr    = S_OK;
    const char* deviceLostDonde = nullptr;

    // Consecutive frames that could be neither recorded nor submitted. A Reset or a Close
    // that fails in isolation is a lost frame and little else (nothing is submitted, so
    // there is no half-done state on the GPU), but chained they are a device that
    // no longer responds, and continuing to try only buries the first error.
    int                  framesDescartadosSeguidos = 0;
    static constexpr int kMaxFramesDescartados     = 3;

    // Writes to file whatever the debug layer has accumulated since the
    // last time, and empties the queue.
    void drainInfoQueue();
    // Reason for the loss + DRED auto-breadcrumbs (which operations the GPU
    // completed and which one was left half done) + failed page, if any.
    void dumpDeviceRemoved(const char* donde, HRESULT hr);
    // Failure that breaks the fence protocol: records it, drains the debug
    // layer and marks the device as lost. It does NOT throw (see deviceLost).
    void notarDeviceLost(const char* donde, HRESULT hr);
    // Frame that could be neither recorded nor submitted. Records it and, if it repeats
    // kMaxFramesDescartados times in a row, escalates to device loss.
    void notarFrameDescartado(const char* donde, HRESULT hr);

    void waitForGpu();
    void moveToNextFrame();
    void createRenderTargetViews();
    void releaseRenderTargets();
    void applyPendingResize();

    // Uploads `size` bytes to a buffer in a DEFAULT heap and leaves it in `finalState`.
    // Synchronous: records the copy, executes it and waits. It is only used in init, where
    // blocking costs nothing; streaming upload belongs to another phase.
    D3D12MA::Allocation* uploadBuffer(const void* data, size_t size,
                                      D3D12_RESOURCE_STATES finalState);

    void createGizmoPipeline();
    void createGridGeometry();

    // Uploads a 2D texture (or an array of 1x1 slices) and creates its SRV in the
    // `srvIndex` slot of the heap.
    // `mips` are levels 1..N-1 of a 2D texture (arraySize == 1); with
    // arraySize > 1 they are not used.
    D3D12MA::Allocation* uploadTexture(const void* pixels, UINT width, UINT height,
                                       UINT arraySize, DXGI_FORMAT format,
                                       UINT bytesPerPixel, UINT srvIndex,
                                       const TextureMip* mips = nullptr, size_t mipCount = 0);

    // Decodes the material texture (embedded or from file), uploads it and creates its
    // SRV at `srvIndex`. nullptr if there is no texture or it could not be read:
    // the caller puts the global 1x1 view there. The format is decided by
    // resolveSrgb: the slot (`kind`) gives the default value and the sidecar can
    // override it.
    D3D12MA::Allocation* uploadMaterialTexture(const std::string& path,
                                               const std::vector<uint8_t>& embedded,
                                               TextureKind kind, UINT srvIndex);

    // View of an already uploaded 2D texture in any slot. To repeat
    // the global 1x1s inside an object's triplet without uploading them again.
    void createTexture2DSrv(ID3D12Resource* resource, DXGI_FORMAT format, UINT srvIndex);

    // View of the cascade array (or of its 1x1 filler while it does not exist) in
    // the given slot. Each object's block needs its own.
    void createShadowMapSrv(UINT srvIndex);

    // View of an already uploaded cubemap in the given slot, with its mips.
    void createCubeSrv(ID3D12Resource* resource, DXGI_FORMAT format, UINT mipLevels,
                       UINT srvIndex);

    // Fills t3..t7 of a block with the shared resources: shadows,
    // the two environment cubemaps and occlusion. What changes per object are
    // t1 and t2, which are written by whoever uploads it.
    void fillSharedSlots(UINT blockBase);

    // t7 of a block: the occlusion map if SSAO runs, the white 1x1 if
    // not. And the refresh of all of them when the switch changes.
    void writeAoSlot(UINT blockBase);
    void refreshAoSlots();
    bool aoSlotsUseMap = false;

    void createMeshPipeline();
    void createMeshResources();
    void createDepthBuffer();
    void updateSceneUbo();

    // Creates an empty buffer in VRAM with unordered access, which is
    // what the destinations of the three computes need.
    D3D12MA::Allocation* createStorageBuffer(UINT64 size, D3D12_RESOURCE_STATES initialState);

    void createSkinningPipelines();
    // Uploads a character and returns its index in skinnedObjects, or -1 if the
    // mesh has no skeleton, vertices or keys to evaluate.
    int  createSkinnedObject(const SkinnedMesh& mesh);
    void recordSkinning();  // the three dispatches per character, with their barriers

    void createShadowResources();
    void computeCascades();   // splits the frustum and produces one matrix per cascade
    void recordShadowPasses();

    // HDR target of the scene and bloom levels. They depend on the window
    // size, so they are redone on every resize.
    void createHdrTargets();
    void releaseHdrTargets();
    void createBloomPipelines();
    void recordBloomAndComposite(D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv);
    void createFogAndFxaaPipelines();
    void recordFog();

    // Camera matrix of the frame. It is recomputed on every resize because the aspect
    // depends on the window size.
    glm::mat4 viewProj{1.0f};
    void      updateViewProj();
};

namespace {

#ifndef NDEBUG
// Readable name of each operation that DRED records. Without this, the breadcrumbs come out
// as numbers and one has to go to d3d12.h to translate them by hand. It goes under the same
// guard as the breadcrumbs: in Release they are not recorded, so there is nothing to
// translate and this would be a dead function.
const char* nombreDeOperacion(D3D12_AUTO_BREADCRUMB_OP op)
{
    switch (op) {
        case D3D12_AUTO_BREADCRUMB_OP_SETMARKER:                return "SetMarker";
        case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT:               return "BeginEvent";
        case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT:                 return "EndEvent";
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED:            return "DrawInstanced";
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED:     return "DrawIndexedInstanced";
        case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT:          return "ExecuteIndirect";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCH:                 return "Dispatch";
        case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION:         return "CopyBufferRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION:        return "CopyTextureRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE:             return "CopyResource";
        case D3D12_AUTO_BREADCRUMB_OP_COPYTILES:                return "CopyTiles";
        case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE:       return "ResolveSubresource";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW:    return "ClearRenderTargetView";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "ClearUnorderedAccessView";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW:    return "ClearDepthStencilView";
        case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER:          return "ResourceBarrier";
        case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE:            return "ExecuteBundle";
        case D3D12_AUTO_BREADCRUMB_OP_PRESENT:                  return "Present";
        case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA:         return "ResolveQueryData";
        case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION:          return "BeginSubmission";
        case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION:            return "EndSubmission";
        case D3D12_AUTO_BREADCRUMB_OP_WRITEBUFFERIMMEDIATE:     return "WriteBufferImmediate";
        case D3D12_AUTO_BREADCRUMB_OP_SETPIPELINESTATE1:        return "SetPipelineState1";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCHMESH:             return "DispatchMesh";
        case D3D12_AUTO_BREADCRUMB_OP_BARRIER:                  return "Barrier";
        case D3D12_AUTO_BREADCRUMB_OP_BEGIN_COMMAND_LIST:       return "BeginCommandList";
        default:                                                return "Op";
    }
}
#endif  // NDEBUG

}  // namespace

void D3D12Renderer::Impl::drainInfoQueue()
{
    if (!infoQueue)
        return;

    const UINT64 total = infoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < total; ++i) {
        SIZE_T bytes = 0;
        if (FAILED(infoQueue->GetMessage(i, nullptr, &bytes)) || bytes == 0)
            continue;
        std::vector<char> buffer(bytes);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        if (FAILED(infoQueue->GetMessage(i, msg, &bytes)))
            continue;
        const char* gravedad = "INFO";
        switch (msg->Severity) {
            case D3D12_MESSAGE_SEVERITY_CORRUPTION: gravedad = "CORRUPTION"; break;
            case D3D12_MESSAGE_SEVERITY_ERROR:      gravedad = "ERROR";      break;
            case D3D12_MESSAGE_SEVERITY_WARNING:    gravedad = "WARNING";      break;
            default: break;
        }
        diagLog(std::string("[layer ") + gravedad + " id=" + std::to_string(msg->ID) + "] " +
                std::string(msg->pDescription, msg->DescriptionByteLength > 0
                                                   ? msg->DescriptionByteLength - 1
                                                   : 0));
    }
    if (total > 0)
        infoQueue->ClearStoredMessages();
}

void D3D12Renderer::Impl::dumpDeviceRemoved(const char* donde, HRESULT hr)
{
    if (deviceRemovedVolcado)
        return;
    deviceRemovedVolcado = true;

    diagLog("==================== DEVICE LOST ====================");
    diagLog(std::string("Detected at: ") + (donde ? donde : "?") + "  HRESULT " +
            hresultToString(hr));

    if (!device) {
        diagLog("There is no device to ask.");
        return;
    }

    const HRESULT motivo = device->GetDeviceRemovedReason();
    diagLog(std::string("GetDeviceRemovedReason() = ") + hresultToString(motivo) + " (" +
            (motivo == DXGI_ERROR_DEVICE_HUNG            ? "DEVICE_HUNG: the GPU did not respond (TDR)"
             : motivo == DXGI_ERROR_DEVICE_REMOVED       ? "DEVICE_REMOVED"
             : motivo == DXGI_ERROR_DEVICE_RESET         ? "DEVICE_RESET: device reset"
             : motivo == DXGI_ERROR_DRIVER_INTERNAL_ERROR ? "DRIVER_INTERNAL_ERROR"
             : motivo == DXGI_ERROR_INVALID_CALL         ? "INVALID_CALL: the app asked for something illegal"
             : motivo == S_OK                            ? "S_OK: the device is NOT lost"
                                                         : "other") +
            ")");

    // Whatever the debug layer had stored: it usually explains the why
    // much better than the breadcrumbs.
    drainInfoQueue();

    // The reason above and the layer's queue come out in BOTH
    // configurations: they are cheap and are the first thing one wants to read. What
    // changes from one to the other are the breadcrumbs, which are not even recorded in Release.
    ComPtr<ID3D12DeviceRemovedExtendedData1> dred1;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dred1)))) {
#ifndef NDEBUG
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 migas{};
        if (SUCCEEDED(dred1->GetAutoBreadcrumbsOutput1(&migas))) {
            diagLog("--- Auto-breadcrumbs (DRED) ---");
            const D3D12_AUTO_BREADCRUMB_NODE1* nodo = migas.pHeadAutoBreadcrumbNode;
            int listas = 0;
            for (; nodo != nullptr; nodo = nodo->pNext) {
                const UINT total  = nodo->BreadcrumbCount;
                const UINT hechas = nodo->pLastBreadcrumbValue ? *nodo->pLastBreadcrumbValue : 0;
                // A list with ALL its breadcrumbs done finished fine: it is not the
                // culprit and only gets in the way in the log.
                if (total == 0 || hechas == total)
                    continue;
                ++listas;
                diagLog(std::string("Command list '") +
                        (nodo->pCommandListDebugNameA ? nodo->pCommandListDebugNameA : "(unnamed)") +
                        "' on queue '" +
                        (nodo->pCommandQueueDebugNameA ? nodo->pCommandQueueDebugNameA : "(unnamed)") +
                        "': completed " + std::to_string(hechas) + " of " +
                        std::to_string(total) + " operations.");
                // Window around the cut: just enough to see what came
                // before and what was left unexecuted.
                const UINT desde = hechas > 12 ? hechas - 12 : 0;
                const UINT hasta = (hechas + 4 < total) ? hechas + 4 : total;
                for (UINT i = desde; i < hasta; ++i) {
                    const char* marca = (i < hechas) ? "  ok  " : (i == hechas ? " >>>> " : "  --  ");
                    std::string linea = std::string(marca) + "[" + std::to_string(i) + "] " +
                                        nombreDeOperacion(nodo->pCommandHistory[i]);
                    // Contexts: they are attached by SetBreadcrumbContext, which this
                    // backend does not use yet, but if one day it does they come out
                    // here and say WHICH canvas or WHICH mesh it was.
                    for (UINT c = 0; c < nodo->BreadcrumbContextsCount; ++c) {
                        if (nodo->pBreadcrumbContexts[c].BreadcrumbIndex != i)
                            continue;
                        linea += "  ctx=" + narrow(nodo->pBreadcrumbContexts[c].pContextString);
                    }
                    diagLog(linea);
                }
            }
            if (listas == 0)
                diagLog("No command list was left half-done: the GPU finished everything submitted.");
        } else {
            diagLog("GetAutoBreadcrumbsOutput1 failed: DRED never got enabled.");
        }
#else
        // No breadcrumbs, but said out loud: whoever reads this log in Release has
        // to know that the list of operations is NOT missing due to a failure, but
        // because it is not recorded (see the comment in init), and that reproducing it in
        // Debug gives them the exact command.
        diagLog("Auto-breadcrumbs not available: they are not recorded in Release (they cost a "
                "WriteBufferImmediate per command). Repeating this in Debug gives the "
                "exact operation that was left half-done.");
#endif

        // The page fault DOES go in both: it costs nothing per frame and it is what
        // says which object was in the address that blew up (or had just died).
        D3D12_DRED_PAGE_FAULT_OUTPUT fallo{};
        if (SUCCEEDED(dred1->GetPageFaultAllocationOutput(&fallo)) &&
            fallo.PageFaultVA != 0) {
            char va[32] = {};
            std::snprintf(va, sizeof(va), "0x%llX",
                          static_cast<unsigned long long>(fallo.PageFaultVA));
            diagLog(std::string("--- Page fault at GPU VA ") + va + " ---");
            auto listar = [](const char* titulo, const D3D12_DRED_ALLOCATION_NODE* n) {
                for (int i = 0; n != nullptr && i < 8; n = n->pNext, ++i)
                    diagLog(std::string(titulo) + ": " +
                            (n->ObjectNameA ? n->ObjectNameA : "(unnamed)"));
            };
            listar("  LIVE object at that address", fallo.pHeadExistingAllocationNode);
            listar("  object RECENTLY FREED there", fallo.pHeadRecentFreedAllocationNode);
        }
    } else {
        diagLog("The device does not expose ID3D12DeviceRemovedExtendedData1: no breadcrumbs and no "
                "page fault.");
    }
    diagLog("========================================================");
}

void D3D12Renderer::Impl::notarDeviceLost(const char* donde, HRESULT hr)
{
    // The HRESULT of the call that failed is usually the consequence, not the
    // cause: a Signal on a dead device returns E_FAIL and the one that knows the real
    // reason is GetDeviceRemovedReason().
    const HRESULT motivo = device ? device->GetDeviceRemovedReason() : S_OK;
    const HRESULT culpa  = FAILED(motivo) ? motivo : hr;

    // It is ALWAYS logged, even if it was already marked: the place where it is
    // detected the second time says how the engine continued after the first
    // failure, which is exactly what is not visible today.
    std::string linea = std::string("UNRECOVERABLE FAILURE at ") + (donde ? donde : "?") +
                        ": HRESULT " + hresultToString(hr);
    if (FAILED(motivo) && motivo != hr)
        linea += " (GetDeviceRemovedReason: " + hresultToString(motivo) + ")";
    diagLog(linea);

    drainInfoQueue();

    if (!deviceLost) {
        deviceLost      = true;
        deviceLostHr    = culpa;
        deviceLostDonde = donde;
        // dumpDeviceRemoved already has its own latch, but the GPU is only
        // asked when there is reason to believe it is gone: an
        // E_INVALIDARG from recording a list wrong has no breadcrumbs to show.
        if (esPerdidaDeDevice(hr) || FAILED(motivo))
            dumpDeviceRemoved(donde, culpa);
    }
}

void D3D12Renderer::Impl::notarFrameDescartado(const char* donde, HRESULT hr)
{
    ++framesDescartadosSeguidos;
    diagLog(std::string("Frame discarded at ") + (donde ? donde : "?") + ": HRESULT " +
            hresultToString(hr) + " (" + std::to_string(framesDescartadosSeguidos) + " of " +
            std::to_string(kMaxFramesDescartados) + " in a row).");
    drainInfoQueue();

    if (framesDescartadosSeguidos >= kMaxFramesDescartados)
        notarDeviceLost(donde, hr);
}

// Waits for the GPU to drain EVERYTHING submitted. Only for resize and shutdown: per
// frame moveToNextFrame is used, which does not serialize CPU and GPU.
void D3D12Renderer::Impl::waitForGpu()
{
    if (!queue || !fence || fenceEvent == nullptr)
        return;

    const UINT64 target = fenceValues[frameIndex];
    // Without a Signal there is nothing to wait for, and the caller releases resources as soon as
    // this returns. Returning silently is silent corruption, not a warning.
    if (const HRESULT hr = queue->Signal(fence.Get(), target); FAILED(hr)) {
        notarDeviceLost("ID3D12CommandQueue::Signal (waitForGpu)", hr);
        return;
    }

    if (fence->GetCompletedValue() < target) {
        if (SUCCEEDED(fence->SetEventOnCompletion(target, fenceEvent))) {
            // It keeps waiting WITHOUT LIMIT (leaving early would release
            // resources the GPU is still reading), but in slices: if the GPU
            // hangs (TDR), this fence is never signaled and the editor freezes
            // without saying a word. Each slice that expires writes down the
            // reason. 5 s is twice the default TDR (2 s).
            while (WaitForSingleObjectEx(fenceEvent, 5000, FALSE) == WAIT_TIMEOUT) {
                diagLog("waitForGpu: the fence is still not signaled (expected " +
                        std::to_string(target) + ", completed " +
                        std::to_string(fence->GetCompletedValue()) + ").");
                dumpDeviceRemoved("waitForGpu (the fence does not advance)",
                                  device ? device->GetDeviceRemovedReason() : E_FAIL);
            }
        }
    }
    ++fenceValues[frameIndex];
}

void D3D12Renderer::Impl::createTimestampResources()
{
    // The frequency belongs to the QUEUE, not the device: it is what converts ticks
    // into seconds. A copy queue would have a different one.
    if (FAILED(queue->GetTimestampFrequency(&timestampFreq)) || timestampFreq == 0) {
        // Without a clock there are no measurements, but there is no reason not to draw either: the
        // panel will show zeros.
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

    // The resolve destination goes in a readback heap: it is the only one from which
    // the CPU can read without an intermediate copy.
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

    // Mapped once and forever: it is read when the slot's fence says
    // the GPU has already written, so there is no need to map and unmap per
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
    if (!timestampHeap || !perfCapture)
        return;
    commandList->EndQuery(timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                          frameIndex * TsCount + slot);
}

void D3D12Renderer::Impl::readTimestamps()
{
    if (!timestampMapped || timestampFreq == 0)
        return;

    // This slot already went through moveToNextFrame, which waited on its fence: what is in
    // the buffer belongs to the last frame that used it, and it is complete.
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
    // Same as in waitForGpu: if this fails, the next frame would reset an
    // allocator whose lists may still be executing, because neither frameIndex
    // nor fenceValues get to advance.
    if (const HRESULT hr = queue->Signal(fence.Get(), current); FAILED(hr)) {
        notarDeviceLost("ID3D12CommandQueue::Signal (moveToNextFrame)", hr);
        return;
    }

    frameIndex = swapChain->GetCurrentBackBufferIndex();

    // It only waits if this slot is still on the GPU. With triple buffering, the
    // normal case is that it has already finished and nothing blocks.
    if (fence->GetCompletedValue() < fenceValues[frameIndex]) {
        if (SUCCEEDED(fence->SetEventOnCompletion(fenceValues[frameIndex], fenceEvent))) {
            // Same slice and same reason as in waitForGpu: it waits just as
            // long, but a fence that does not advance leaves a trace instead of freezing
            // the frame loop silently.
            while (WaitForSingleObjectEx(fenceEvent, 5000, FALSE) == WAIT_TIMEOUT) {
                diagLog("moveToNextFrame: the fence is still not signaled (expected " +
                        std::to_string(fenceValues[frameIndex]) + ", completed " +
                        std::to_string(fence->GetCompletedValue()) + ").");
                dumpDeviceRemoved("moveToNextFrame (the fence does not advance)",
                                  device ? device->GetDeviceRemovedReason() : E_FAIL);
            }
        }
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

    // The splash's sRGB views of the same images. Created here, next to the
    // normal ones, so a ResizeBuffers (which re-runs this) never leaves them
    // pointing at released buffers.
    D3D12_RENDER_TARGET_VIEW_DESC srgbView{};
    srgbView.Format        = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    srgbView.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    for (UINT i = 0; i < kFrameCount; ++i) {
        D3D12_CPU_DESCRIPTOR_HANDLE splashHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        splashHandle.ptr += static_cast<SIZE_T>(kRtvSplash + i) * rtvSize;
        device->CreateRenderTargetView(renderTargets[i].Get(), &srgbView, splashHandle);
    }
}

void D3D12Renderer::Impl::releaseRenderTargets()
{
    // ResizeBuffers requires that NO live reference remain to the old
    // buffers; if one remains, it returns E_INVALIDARG and the swapchain is left broken.
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

    // Destination in VRAM. It is born in COPY_DEST because the first thing it receives is the
    // copy from the staging buffer.
    D3D12MA::ALLOCATION_DESC defaultDesc{};
    defaultDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12MA::Allocation* destination = nullptr;
    throwIfFailed(allocator->CreateResource(&defaultDesc, &bufferDesc,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            &destination, IID_NULL, nullptr),
                  "D3D12MA::Allocator::CreateResource(DEFAULT)");

    // Staging in CPU-visible memory. It is released on leaving the function: the
    // copy will already have finished because it is waited on before returning.
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
    const D3D12_RANGE noRead{0, 0};  // nothing is read back
    hr = staging->GetResource()->Map(0, &noRead, &mapped);
    if (FAILED(hr)) {
        staging->Release();
        destination->Release();
        throwIfFailed(hr, "ID3D12Resource::Map(staging)");
    }
    std::memcpy(mapped, data, size);
    staging->GetResource()->Unmap(0, nullptr);

    // Copy in its own submission. The command list is reused: it has to be left
    // closed, which is how drawFrame expects it.
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

    // Without waiting here, the staging buffer would be destroyed with the copy still in flight.
    waitForGpu();

    staging->Release();
    return destination;
}

void D3D12Renderer::Impl::ensureDebugLineBuffer(size_t vertexCount)
{
    if (vertexCount <= debugLinesCapacity)
        return;

    // It grows in blocks: a scene with visible colliders sends thousands of
    // vertices and the number goes up and down between frames.
    const size_t newCapacity = (std::max)(vertexCount, debugLinesCapacity * 2 + 1024);

    if (debugLinesAllocation) {
        // It may be in use by the previous frame.
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
                  "D3D12MA::Allocator::CreateResource(debug lines)");

    const D3D12_RANGE noRead{0, 0};
    throwIfFailed(debugLinesAllocation->GetResource()->Map(0, &noRead, &debugLinesMapped),
                  "ID3D12Resource::Map(debug lines)");

    debugLinesCapacity = newCapacity;
    debugLinesView.BufferLocation = debugLinesAllocation->GetResource()->GetGPUVirtualAddress();
    debugLinesView.SizeInBytes    = static_cast<UINT>(newCapacity * sizeof(GizmoVertex));
    debugLinesView.StrideInBytes  = sizeof(GizmoVertex);
}

void D3D12Renderer::Impl::createGizmoPipeline()
{
    // Root signature: the 16 floats of the matrix as root constants. It is the
    // direct equivalent of the push_constant in shaders/gizmo.vert, and avoids
    // having to create a constant buffer and its descriptor for 64 bytes.
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
        throw std::runtime_error("D3D12: D3D12SerializeRootSignature failed (HRESULT " +
                                 hresultToString(hr) + ") " + detail);
    }

    throwIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                              serialized->GetBufferSize(),
                                              IID_PPV_ARGS(&rootSignature)),
                  "ID3D12Device::CreateRootSignature");

    // The .dxil files are produced by the build by translating the SPIR-V of the same .vert
    // and .frag that Vulkan uses, so they are looked for where the .spv are.
    const std::vector<char> vertexShader = readBinaryFile("shaders/gizmo.vert.dxil");
    const std::vector<char> pixelShader  = readBinaryFile("shaders/gizmo.frag.dxil");

    // TEXCOORD0/TEXCOORD1 semantics: it is how spirv-cross translates
    // layout(location = N), not a choice of ours.
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

    // The grid IS compared against depth: it is the ground, and whatever is
    // in front of it has to cover it.
    psoDesc.DepthStencilState.DepthEnable    = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    psoDesc.DepthStencilState.StencilEnable  = FALSE;

    throwIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&gizmoPipeline)),
                  "ID3D12Device::CreateGraphicsPipelineState");
}

void D3D12Renderer::Impl::createGridGeometry()
{
    // Ground grid, the same visual reference the editor draws with
    // Vulkan: 41 lines per axis spaced 1 unit apart, with the X and Z axes
    // marked in color so the orientation is noticeable.
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
                                                        UINT srvIndex,
                                                        const TextureMip* mips, size_t mipCount)
{
    // With a slice array there are no mips (the array's ones are used by the sky cubemap
    // and the shadows, which are single-level).
    const UINT mipLevels    = 1 + (arraySize == 1 ? static_cast<UINT>(mipCount) : 0);
    const UINT subresources = mipLevels * arraySize;

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = width;
    texDesc.Height           = height;
    texDesc.DepthOrArraySize = static_cast<UINT16>(arraySize);
    texDesc.MipLevels        = static_cast<UINT16>(mipLevels);
    texDesc.Format           = format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12MA::ALLOCATION_DESC defaultDesc{};
    defaultDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12MA::Allocation* destination = nullptr;
    throwIfFailed(allocator->CreateResource(&defaultDesc, &texDesc,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            &destination, IID_NULL, nullptr),
                  "D3D12MA::Allocator::CreateResource(texture)");

    // The staging buffer is not written row by row as in memory: each row is
    // aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, and each slice of the array is
    // its own subresource with its footprint.
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
    std::vector<UINT>                               rowCounts(subresources);
    std::vector<UINT64>                             rowSizes(subresources);
    UINT64                                          stagingSize = 0;
    device->GetCopyableFootprints(&texDesc, 0, subresources, 0, footprints.data(), rowCounts.data(),
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
        throwIfFailed(hr, "D3D12MA::Allocator::CreateResource(texture staging)");
    }

    uint8_t*          mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    hr = staging->GetResource()->Map(0, &noRead, reinterpret_cast<void**>(&mapped));
    if (FAILED(hr)) {
        staging->Release();
        destination->Release();
        throwIfFailed(hr, "ID3D12Resource::Map(texture staging)");
    }

    // Subresource i = level + slice * mipLevels. Level 0 comes from `pixels`
    // (slice by slice) and the others from `mips`, each with its own width.
    const auto* source = static_cast<const uint8_t*>(pixels);
    for (UINT i = 0; i < subresources; ++i) {
        const UINT slice = i / mipLevels;
        const UINT level = i % mipLevels;
        const uint8_t* src = level == 0
            ? source + static_cast<UINT64>(slice) * height * width * bytesPerPixel
            : mips[level - 1].rgba.data();
        const UINT srcWidth = level == 0 ? width : mips[level - 1].w;
        for (UINT row = 0; row < rowCounts[i]; ++row) {
            std::memcpy(mapped + footprints[i].Offset +
                            static_cast<UINT64>(row) * footprints[i].Footprint.RowPitch,
                        src + static_cast<UINT64>(row) * srcWidth * bytesPerPixel,
                        static_cast<size_t>(rowSizes[i]));
        }
    }
    staging->GetResource()->Unmap(0, nullptr);

    throwIfFailed(allocators[frameIndex]->Reset(), "ID3D12CommandAllocator::Reset(texture)");
    throwIfFailed(commandList->Reset(allocators[frameIndex].Get(), nullptr),
                  "ID3D12GraphicsCommandList::Reset(texture)");

    for (UINT i = 0; i < subresources; ++i) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource        = destination->GetResource();
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = i;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource       = staging->GetResource();
        src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprints[i];

        commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    D3D12_RESOURCE_BARRIER toShader{};
    toShader.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toShader.Transition.pResource   = destination->GetResource();
    toShader.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toShader.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toShader.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &toShader);

    throwIfFailed(commandList->Close(), "ID3D12GraphicsCommandList::Close(texture)");
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
        srvDesc.Texture2D.MipLevels = mipLevels;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(srvIndex) * srvSize;
    device->CreateShaderResourceView(destination->GetResource(), &srvDesc, handle);

    return destination;
}

D3D12MA::Allocation* D3D12Renderer::Impl::uploadMaterialTexture(
    const std::string& path, const std::vector<uint8_t>& embedded, TextureKind kind, UINT srvIndex)
{
    const DecodedTexture tex = decodeMaterialTexture(path, embedded);
    if (!tex)
        return nullptr;  // the caller (outside this function) puts its filler

    // The slot gives the default value (sRGB for base color, linear for normals:
    // a normal interpreted as color is decoded with gamma and points somewhere
    // else) and the import sidecar can override it. resolveSrgb is the only
    // place that decides it.
    const bool srgb = resolveSrgb(kind, tex.colorSpace);
    const DXGI_FORMAT format =
        srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;

    // `tex` releases the pixels on exit, also if uploadTexture throws: the
    // try/catch that was here only existed so as not to leak them in that case.
    return uploadTexture(tex.pixels.get(), static_cast<UINT>(tex.w), static_cast<UINT>(tex.h), 1,
                         format, 4, srvIndex, tex.mips.data(), tex.mips.size());
}

// The format the resource was created with: the reuse views read it from here
// instead of assuming it from the slot (the sidecar may have changed it).
static DXGI_FORMAT formatOf(D3D12MA::Allocation* a)
{
    return a->GetResource()->GetDesc().Format;
}

void D3D12Renderer::Impl::createTexture2DSrv(ID3D12Resource* resource, DXGI_FORMAT format,
                                             UINT srvIndex)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                  = format;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    // -1 = all levels of the resource: 1 for the global 1x1s, N for a
    // material texture with mips.
    srvDesc.Texture2D.MipLevels     = static_cast<UINT>(-1);

    D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(srvIndex) * srvSize;
    device->CreateShaderResourceView(resource, &srvDesc, handle);
}

void D3D12Renderer::Impl::createShadowMapSrv(UINT srvIndex)
{
    // The cascade array if it already exists; otherwise, the 1x1 filler, which was created
    // with the same four slices and the same format.
    ID3D12Resource* resource = shadowMapArrayAllocation ? shadowMapArrayAllocation->GetResource()
                                                        : shadowMapAllocation->GetResource();
    if (!resource)
        return;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                   = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension            = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Shader4ComponentMapping  = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.ArraySize = kShadowLayers;

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
    // t7 = ambient occlusion. With SSAO ON, the frame's map; with it
    // off, the white 1x1.
    //
    // The latter is not cosmetic: if SSAO does not run, its map is NEVER
    // written (a freshly created texture in D3D12 is not initialized) and the
    // shader multiplies the ambient by whatever is there, which is zero. The
    // symptom was that everything not receiving direct light came out BLACK, with the
    // IBL correctly computed and raising the ambient intensity having no effect at all:
    // anything times zero is still zero.
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

    // The descriptors may be in use by the in-flight frame.
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
    // TYPELESS and not D32_FLOAT: the fog needs to READ this depth as a
    // texture, and the same resource cannot be declared at once with a depth
    // format and a sampling one.
    depthDesc.Format           = DXGI_FORMAT_R32_TYPELESS;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    // The clear value is declared: if the one in ClearDepthStencilView does not
    // match this one, the validation layer flags it and the GPU loses the
    // fast clear path.
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

    // Sampling view of the same resource, for the fog. The heap does not yet
    // exist on the first call (init creates the depth before the heap): in
    // that case createHdrTargets creates it, which runs afterwards.
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
    // Root signature of triangle.vert/triangle.frag:
    //   b0 space0  scene UBO              -> root CBV
    //   b1 space0  push constants         -> 20 root constants
    //   t1..t3     base, normal, shadows  -> descriptor table
    //   t0 space1  per-instance matrices  -> root SRV
    // The UBO declares b0 explicitly and the push constants block declares no
    // register, so DXC assigns it the next free one: b1.
    D3D12_DESCRIPTOR_RANGE textureRange{};
    textureRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRange.NumDescriptors     = kSrvPerObject;
    textureRange.BaseShaderRegister = 1;  // t1..t7
    textureRange.RegisterSpace      = 0;
    textureRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // Forward+ lives in its own space: four ByteAddressBuffers that go as
    // root SRVs, with no table or descriptors.
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

    // Static samplers: the shaders do not choose filter or wrap at run
    // time, so no sampler heap or descriptors are needed.
    // s1, s2 and s4: material textures, which repeat with the UV. s5, s6 and s7
    // use a fixed border: a cubemap or a screen map do not repeat.
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
    // s3 is that of the sampler2DArrayShadow: comparison, not normal filtering.
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
        throw std::runtime_error("D3D12: mesh root signature (HRESULT " +
                                 hresultToString(hr) + ") " + detail);
    }

    throwIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                              serialized->GetBufferSize(),
                                              IID_PPV_ARGS(&meshRootSignature)),
                  "ID3D12Device::CreateRootSignature(mesh)");

    const std::vector<char> vertexShader = readBinaryFile("shaders/triangle.vert.dxil");
    const std::vector<char> pixelShader  = readBinaryFile("shaders/pbr.frag.dxil");

    // The order and offsets come from DonTopo::Vertex; the semantics, from how
    // spirv-cross translates layout(location = N).
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
    // The engine generates the triangles with the Vulkan convention; in D3D12 the
    // screen Y axis goes the other way, so what is counterclockwise there
    // looks clockwise here. Without this, the cube is drawn inside out and disappears.
    psoDesc.RasterizerState.FrontCounterClockwise = TRUE;

    for (auto& rt : psoDesc.BlendState.RenderTarget)
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.DepthStencilState.DepthEnable    = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    // The wireframe one, with the only thing that changes: the fill. Without back
    // face culling, which in wireframe hides half of the edges.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC wireDesc = psoDesc;
    wireDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    wireDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    throwIfFailed(device->CreateGraphicsPipelineState(&wireDesc, IID_PPV_ARGS(&meshWirePipeline)),
                  "ID3D12Device::CreateGraphicsPipelineState(wireframe mesh)");

    // The outline: same vertices, another pair of shaders and the OPPOSITE face
    // culled.
    {
        const std::vector<char> outlineVs = readBinaryFile("shaders/outline.vert.dxil");
        const std::vector<char> outlinePs = readBinaryFile("shaders/outline.frag.dxil");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC outlineDesc = psoDesc;
        outlineDesc.VS = {outlineVs.data(), outlineVs.size()};
        outlineDesc.PS = {outlinePs.data(), outlinePs.size()};
        outlineDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
        // Strict LESS and not LESS_EQUAL like the mesh: the hull falls at the SAME
        // depth as the surface in flat areas, and with equality
        // included it would pass the test and cover it entirely.
        outlineDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        throwIfFailed(device->CreateGraphicsPipelineState(&outlineDesc,
                                                          IID_PPV_ARGS(&outlinePipeline)),
                      "ID3D12Device::CreateGraphicsPipelineState(outline)");

        // The same one, but against the LDR target: that is where it is really
        // drawn, AFTER tone mapping, so its orange arrives flat instead
        // of going through ACES. Always one sample (the LDR is not
        // multisampled) and without writing depth, which is no longer its own.
        outlineDesc.RTVFormats[0]                       = kLdrFormat;
        outlineDesc.SampleDesc.Count                    = 1;
        outlineDesc.DepthStencilState.DepthWriteMask    = D3D12_DEPTH_WRITE_MASK_ZERO;
        throwIfFailed(device->CreateGraphicsPipelineState(&outlineDesc,
                                                          IID_PPV_ARGS(&outlineLdrPipeline)),
                      "ID3D12Device::CreateGraphicsPipelineState(outline over LDR)");
    }

    throwIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&meshPipeline)),
                  "ID3D12Device::CreateGraphicsPipelineState(mesh)");
}

void D3D12Renderer::Impl::createMeshResources()
{
    // Instance buffer with the identity. The scene geometry uses the push
    // constant for its transform (flags.x = 0), but the shader declares
    // the SSBO anyway and the root signature has to satisfy it.
    const glm::mat4 instanceModel{1.0f};
    instanceAllocation = uploadBuffer(&instanceModel, sizeof(instanceModel),
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Heap of the three SRVs the fragment shader asks for.
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.NumDescriptors = kSrvHeapSize;
    srvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    throwIfFailed(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap)),
                  "ID3D12Device::CreateDescriptorHeap(SRV)");
    srvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // 1x1 textures: the cube is procedural and has none, but the shaders
    // sample them anyway. White for the base color and (0.5, 0.5, 1) for the
    // normal, which is the unperturbed normal.
    const uint8_t white[4]      = {255, 255, 255, 255};
    const uint8_t flatNormal[4] = {128, 128, 255, 255};
    baseColorAllocation = uploadTexture(white, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 0);
    // And the checkerboard. uploadTexture also writes the SRV of the slot it
    // is given, so it is given 0 and immediately afterwards the white is restored: that
    // global slot is the neutral one, and the checkerboard is only referenced from the
    // block of the object whose texture has failed.
    {
        const std::vector<uint8_t> damero = makeMissingTextureRgba();
        missingTextureAllocation =
            uploadTexture(damero.data(), kMissingTextureSize, kMissingTextureSize, 1,
                          DXGI_FORMAT_R8G8B8A8_UNORM, 4, 0);
        createTexture2DSrv(baseColorAllocation->GetResource(), DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    }
    normalMapAllocation = uploadTexture(flatNormal, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 1);

    // Shadow map: 1x1 per cascade at maximum depth. With cascadeSplits
    // at zero, selectCascade returns -1 and the shader does not even sample it; it exists
    // because the root signature has to satisfy the t3 it declares.
    const float noShadow[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    shadowMapAllocation = uploadTexture(noShadow, 1, 1, 4, DXGI_FORMAT_R32_FLOAT, 4, kSrvShadowMap);

    // t4: ORM without texture. R = occlusion, G = roughness, B = metalness, and
    // pbr.frag multiplies them by the push factors: at 255 the material
    // rules entirely, which is what the previous shader did.
    const uint8_t neutralOrm[4] = {255, 255, 255, 255};
    metalRoughAllocation =
        uploadTexture(neutralOrm, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4, kSrvMetalRough);

    // t7: screen occlusion at 1. With SSAO off the shader multiplies by
    // one and no branch is needed.
    const uint8_t noOcclusion[4] = {255, 255, 255, 255};
    ssaoAllocation = uploadTexture(noOcclusion, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4, kSrvSsao);

    // The "bloom" of the off path: 1x1 black in the same format as the
    // chain. Eight zero bytes are (0,0,0,0) in R16G16B16A16_FLOAT, so the
    // composition adds exactly nothing, and without depending on the mip chain
    // having valid contents, which it does not have when off.
    const uint8_t bloomBlack[8] = {};
    bloomBlackAllocation =
        uploadTexture(bloomBlack, 1, 1, 1, kHdrFormat, 8, kSrvCompositeOff + 1);

    // t5 and t6: neutral environment, the same values the Vulkan path leaves
    // when there is no cubemap. A flat ambient lights in a boring way, but
    // without them pbr.frag would read from an empty descriptor.
    createNeutralIblCubes();

    // UBO per frame in flight, persistently mapped: it is rewritten every
    // frame and unmapping/remapping adds nothing.
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
    // View-proj WITHOUT jitter: it is the one TAA reprojects and the one compared with
    // the previous frame's.
    taaPrevViewProj = taaCurrViewProj;
    taaCurrViewProj = cameraProj() * cameraView;
    taaJitteredProj = cameraProj();

    if (state->aaMode() == RendererState::AaMode::Taa) {
        // Sequence and application in TaaJitter.h, shared with the Vulkan
        // path: they were written twice and letting them drift apart raises no error, it only
        // makes TAA converge to a different image depending on the backend.
        const glm::vec2 jitter = taaJitterPixels(taaJitterIndex, state->taaJitterScale());
        applyTaaJitter(taaJitteredProj, jitter,
                       static_cast<float>(width), static_cast<float>(height));
    }

    SceneUbo ubo{};
    ubo.view = cameraView;
    ubo.proj = taaJitteredProj;
    // Vulkan has the screen Y axis inverted relative to OpenGL and the engine
    // compensates for it there; D3D12 uses the same orientation as OpenGL, so here it is
    // NOT inverted.

    ubo.cascadeSplits = cascadeSplits;
    // All SIX, not four: with a point light faces 4 and 5 also carry a
    // matrix. Copying only 4 would leave two cubemap faces with the identity.
    for (int i = 0; i < kShadowLayers; ++i)
        ubo.lightSpaceMatrix[i] = cascadeMatrices[i];

    if (!sceneLights.empty()) {
        // The scene's lights rule: without this the backend lit with its
        // filler directional and a scene with spots looked dark
        // even though they were placed correctly.
        const size_t count = (std::min)(sceneLights.size(), static_cast<size_t>(MAX_LIGHTS));
        std::memcpy(ubo.lights, sceneLights.data(), count * sizeof(ShaderLight));
        ubo.numLights = static_cast<int>(count);

        // AFTER the memcpy, which would otherwise overwrite it. position.w of the key light:
        // 1 = its shadow was recorded as a CUBEMAP. The shader reads it to pick the
        // path instead of deducing it from the type, because a very wide spot
        // also ends up in the cubemap. It comes from what the pass DID (how many
        // layers it left valid) and not from recomputing the criterion here: two copies
        // of a criterion is what broke H65. That slot was free, no
        // shader read position.w.
        ubo.lights[0].position[3] = (activeLayers == kShadowKeyLayers) ? 1.0f : 0.0f;
        // And the rest, their slot + 1 (0 = casts none).
        // Slot + 1 with the SIGN telling the path: positive one face,
        // negative six-face cubemap. Same code as in Vulkan, because the
        // same shader reads it.
        for (int i = 1; i < ubo.numLights; i++) {
            if (shadowSlot[i] < 0) { ubo.lights[i].position[3] = 0.0f; continue; }
            const float codigo = (float)(shadowSlot[i] + 1);
            ubo.lights[i].position[3] =
                (shadowFaces[i] == SHADOW_KEY_MATRICES) ? -codigo : codigo;
        }

        ubo.viewPos          = glm::vec4(cameraPos, 1.0f);
        ubo.ambientIntensity = state->ambientEnabled() ? state->ambientIntensity() : 0.0f;
        std::memcpy(sceneUboMapped[frameIndex], &ubo, sizeof(ubo));
        return;
    }

    // A directional (type 2), which is what the shader treats without attenuation.
    // The direction has to be THE SAME one the cascades were computed with,
    // or the shadow would fall in one place and the light would come from another.
    ubo.numLights              = 1;
    ubo.lights[0].direction[0] = lightDirection.x;
    ubo.lights[0].direction[1] = lightDirection.y;
    ubo.lights[0].direction[2] = lightDirection.z;
    ubo.lights[0].direction[3] = 2.0f;  // directional
    ubo.lights[0].position[3]  = 0.0f;  // directional: cascades, never cubemap
    ubo.lights[0].color[0]     = 1.0f;
    ubo.lights[0].color[1]     = 0.98f;
    ubo.lights[0].color[2]     = 0.94f;
    // Intensity below 1: with white albedo and the light at 1.0 the whole scene
    // lands at 1.0, and then the bloom threshold lets even the
    // ground through and washes out the image. With 0.7 there is range left below the threshold.
    ubo.lights[0].color[3]     = 0.7f;

    ubo.viewPos          = glm::vec4(cameraPos, 1.0f);
    // Off = zero intensity, just like in the Vulkan path: the shader has no
    // branch for the ambient, and without this the "Ambient (IBL)" switch in the
    // View menu did nothing with this backend.
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
    // One root signature per pass, with EXACTLY the registers each shader
    // declares. All buffers are ByteAddressBuffers, so they go as
    // root descriptors and no tables or heaps are needed.
    //
    // Watch out for resources that change view between passes: localXforms is u4
    // when bone_eval writes it and t4 when bone_hierarchy reads it; finalBones
    // is u5 when written and t5 when read. It is the same buffer.
    struct Slot {
        D3D12_ROOT_PARAMETER_TYPE type;
        UINT                      shaderRegister;
    };

    auto buildRootSignature = [&](const std::vector<Slot>& slots,
                                  ComPtr<ID3D12RootSignature>& out, const char* what) {
        std::vector<D3D12_ROOT_PARAMETER> params;
        params.reserve(slots.size() + 1);

        // The push constants ALWAYS go in b0, which is where DXC places the
        // push_constant block's cbuffer when it has no explicit register.
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
            throw std::runtime_error(std::string("D3D12: root signature of ") + what +
                                     " (HRESULT " + hresultToString(hr) + ") " + detail);
        }
        throwIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                  serialized->GetBufferSize(), IID_PPV_ARGS(&out)),
                      "ID3D12Device::CreateRootSignature(compute)");
    };

    using RT = D3D12_ROOT_PARAMETER_TYPE;
    // bone_eval: reads keys and bones (t0..t3), writes local transforms (u4)
    buildRootSignature({{RT::D3D12_ROOT_PARAMETER_TYPE_SRV, 0},
                        {RT::D3D12_ROOT_PARAMETER_TYPE_SRV, 1},
                        {RT::D3D12_ROOT_PARAMETER_TYPE_SRV, 2},
                        {RT::D3D12_ROOT_PARAMETER_TYPE_SRV, 3},
                        {RT::D3D12_ROOT_PARAMETER_TYPE_UAV, 4},
                        // poseTrs (u8, writable) and frozenTrs (t9, read-only):
                        // spirv-cross gives the binding's register.
                        {RT::D3D12_ROOT_PARAMETER_TYPE_UAV, 8},
                        {RT::D3D12_ROOT_PARAMETER_TYPE_SRV, 9},
                        // Pose block (t10), one copy per frame: the offset
                        // of this frame's goes in the root constants.
                        {RT::D3D12_ROOT_PARAMETER_TYPE_SRV, 10}},
                       boneEvalRootSignature, "bone_eval");
    // bone_hierarchy: reads bones and locals (t3, t4), writes final matrices (u5)
    buildRootSignature({{RT::D3D12_ROOT_PARAMETER_TYPE_SRV, 3},
                        {RT::D3D12_ROOT_PARAMETER_TYPE_SRV, 4},
                        {RT::D3D12_ROOT_PARAMETER_TYPE_UAV, 5}},
                       boneHierarchyRootSignature, "bone_hierarchy");
    // bone_ik: writes the locals (u4) and reads the hierarchy's worlds (t5),
    // the bones (t3) and the IK block (t11).
    buildRootSignature({{RT::D3D12_ROOT_PARAMETER_TYPE_UAV, 4},
                        {RT::D3D12_ROOT_PARAMETER_TYPE_SRV, 5},
                        {RT::D3D12_ROOT_PARAMETER_TYPE_SRV, 3},
                        {RT::D3D12_ROOT_PARAMETER_TYPE_SRV, 11}},
                       boneIkRootSignature, "bone_ik");
    // skinning: reads matrices and vertices (t5, t6), writes deformed vertices (u7)
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
    buildComputePipeline("shaders/bone_ik.comp.dxil", boneIkRootSignature.Get(), boneIkPipeline);
    buildComputePipeline("shaders/skinning.comp.dxil", skinningRootSignature.Get(),
                         skinningPipeline);

    // Character graphics pipeline: SAME triangle.vert/frag as the cube, but
    // the vertex buffer is the compute output, which goes in aligned vec4 (5 x
    // vec4 = 80 B) instead of the engine's packed Vertex.
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
                  "ID3D12Device::CreateGraphicsPipelineState(wireframe skinned)");

    {
        // The character's outline goes over the vertices left by the skinning,
        // so it inherits THIS input layout and not the engine mesh's.
        const std::vector<char> outlineVs = readBinaryFile("shaders/outline.vert.dxil");
        const std::vector<char> outlinePs = readBinaryFile("shaders/outline.frag.dxil");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC outlineDesc = psoDesc;
        outlineDesc.VS = {outlineVs.data(), outlineVs.size()};
        outlineDesc.PS = {outlinePs.data(), outlinePs.size()};
        outlineDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
        // Strict LESS and not LESS_EQUAL like the mesh: the hull falls at the SAME
        // depth as the surface in flat areas, and with equality
        // included it would pass the test and cover it entirely.
        outlineDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        throwIfFailed(device->CreateGraphicsPipelineState(&outlineDesc,
                                                          IID_PPV_ARGS(&outlineSkinnedPipeline)),
                      "ID3D12Device::CreateGraphicsPipelineState(skinned outline)");

        // Variant on the LDR target, for the same reason as the mesh's.
        outlineDesc.RTVFormats[0]                    = kLdrFormat;
        outlineDesc.SampleDesc.Count                 = 1;
        outlineDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        throwIfFailed(
            device->CreateGraphicsPipelineState(&outlineDesc,
                                                IID_PPV_ARGS(&outlineSkinnedLdrPipeline)),
            "ID3D12Device::CreateGraphicsPipelineState(skinned outline over LDR)");
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
    object.clipCount    = skinnedClipCount(mesh);
    object.vertexCount  = static_cast<uint32_t>(mesh.skinnedVertices.size());
    object.clipBase     = 0;
    object.animDuration = mesh.animationClips.empty() ? 0.0f : mesh.animationClips[0].duration;
    // Same default as Vulkan: an FBX without mTicksPerSecond plays at 24.
    object.ticksPerSecond = mesh.animationClips.empty()
                                ? 0.0f
                                : (mesh.animationClips[0].ticksPerSecond > 0.0f
                                       ? mesh.animationClips[0].ticksPerSecond
                                       : 24.0f);

    // Read-only buffers, shared among clones (B6).
    const SkinnedGeometry geo = skinnedGeometry.acquire(skinnedGeometryKey(mesh, packed), [&] {
        SkinnedGeometry g;
        // Half-created (VRAM exhausted): the uploads are synchronous, so what
        // was already made can be released right away.
        try {
        g.posKeys   = uploadBuffer(packed.pos.data(), packed.pos.size() * sizeof(GpuPosKey),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g.rotKeys   = uploadBuffer(packed.rot.data(), packed.rot.size() * sizeof(GpuRotKey),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g.scaleKeys = uploadBuffer(packed.scale.data(), packed.scale.size() * sizeof(GpuPosKey),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g.boneInfos = uploadBuffer(packed.boneInfos.data(),
                                   packed.boneInfos.size() * sizeof(GpuBoneInfo),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g.inputVerts =
            uploadBuffer(mesh.skinnedVertices.data(), mesh.skinnedVertices.size() * sizeof(SkinnedVertex),
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g.indices = uploadBuffer(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t),
                                 D3D12_RESOURCE_STATE_INDEX_BUFFER);
        } catch (...) {
            for (D3D12MA::Allocation* a : {g.posKeys, g.rotKeys, g.scaleKeys, g.boneInfos, g.inputVerts, g.indices})
                if (a)
                    a->Release();
            throw;
        }
        return g;
    });
    // From here on `object` holds cache references (geometry, then textures
    // and SRV triplets): a throw before it is stored must give them back, or
    // the shared entry stays pinned for the rest of the process.
    bool stored = false;
    struct Unwind {
        Impl* self; SkinnedObject* o; const bool* done;
        ~Unwind() { if (!*done) self->releaseSkinnedResources(*o); }
    } unwind{ this, &object, &stored };
    object.posKeys    = geo.posKeys;
    object.rotKeys    = geo.rotKeys;
    object.scaleKeys  = geo.scaleKeys;
    object.boneInfos  = geo.boneInfos;
    object.inputVerts = geo.inputVerts;
    object.indices    = geo.indices;
    // Extent of the rest pose, for the outline thickness. Same
    // computation as the Vulkan path.
    object.restMaxExtent = 0.0f;
    if (!mesh.skinnedVertices.empty()) {
        glm::vec3 bMin(mesh.skinnedVertices[0].position);
        glm::vec3 bMax = bMin;
        for (const SkinnedVertex& v : mesh.skinnedVertices) {
            bMin = (glm::min)(bMin, glm::vec3(v.position));
            bMax = (glm::max)(bMax, glm::vec3(v.position));
        }
        const glm::vec3 e    = bMax - bMin;
        object.restMaxExtent = (glm::max)(e.x, (glm::max)(e.y, e.z));
    }

    object.localXforms = createStorageBuffer(static_cast<UINT64>(object.boneCount) * sizeof(glm::mat4),
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    object.finalBones  = createStorageBuffer(static_cast<UINT64>(object.boneCount) * sizeof(glm::mat4),
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    object.poseTrs     = createStorageBuffer(static_cast<UINT64>(object.boneCount) * 3 * sizeof(glm::vec4) * kMaxLayersPose,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    object.frozenTrs   = createStorageBuffer(static_cast<UINT64>(object.boneCount) * 3 * sizeof(glm::vec4) * kMaxLayersPose,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = static_cast<UINT64>(kFrameCount) * poseBlockUints(object.boneCount) * sizeof(uint32_t);
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        throwIfFailed(allocator->CreateResource(&allocDesc, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                nullptr, &object.poseBlock, IID_NULL, nullptr),
                      "D3D12MA::Allocator::CreateResource(pose block)");
        object.poseBlock->GetResource()->SetName(L"skinning pose block");
        const D3D12_RANGE noRead{0, 0};
        throwIfFailed(object.poseBlock->GetResource()->Map(0, &noRead, &object.poseBlockMapped),
                      "ID3D12Resource::Map(pose block)");

        desc.Width = static_cast<UINT64>(kFrameCount) * ikBlockUints() * sizeof(uint32_t);
        throwIfFailed(allocator->CreateResource(&allocDesc, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                nullptr, &object.ikBlock, IID_NULL, nullptr),
                      "D3D12MA::Allocator::CreateResource(IK block)");
        object.ikBlock->GetResource()->SetName(L"skinning ik block");
        throwIfFailed(object.ikBlock->GetResource()->Map(0, &noRead, &object.ikBlockMapped),
                      "ID3D12Resource::Map(IK block)");
    }
    object.outputVerts = createStorageBuffer(
        static_cast<UINT64>(object.vertexCount) * kSkinnedOutputStride,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    object.vertexBufferView.BufferLocation = object.outputVerts->GetResource()->GetGPUVirtualAddress();
    object.vertexBufferView.SizeInBytes    = object.vertexCount * kSkinnedOutputStride;
    object.vertexBufferView.StrideInBytes  = kSkinnedOutputStride;

    object.indexBufferView.BufferLocation = object.indices->GetResource()->GetGPUVirtualAddress();
    object.indexBufferView.SizeInBytes = static_cast<UINT>(mesh.indices.size() * sizeof(uint32_t));
    object.indexBufferView.Format      = DXGI_FORMAT_R32_UINT;
    object.indexCount                  = static_cast<UINT>(mesh.indices.size());

    // Materials per submesh. Without subMeshRanges (single-piece FBX) the
    // Mesh's own material is taken, which is what ModelLoader fills in.
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

        // Same criterion as addStaticMesh and as Vulkan (SkinnedMatGfx): with an
        // ORM map the map rules and both factors are forced to 1.0, because the
        // shader MULTIPLIES them by what it reads from the texture. It is decided here,
        // with the material at hand, and not in the draw pass, which no longer has
        // anywhere to get it from.
        //
        // It is filled in even if the submesh ends up without its own triplet (haySlot
        // false, past kMaxSkinnedSlots): then it samples the global neutral ORM,
        // so the factor is still the only thing that decides. It is the same
        // as what addStaticMesh does, which also writes them before knowing whether
        // there will be a slot.
        const bool tieneMapaOrm =
            chooseTextureSource(range.material->metallicRoughnessPath,
                                range.material->embeddedMetallicRoughness) != TextureSource::None;
        sub.metallic  = tieneMapaOrm ? 1.0f : range.material->metallic;
        sub.roughness = tieneMapaOrm ? 1.0f : range.material->roughness;

        // The triplets are shared among ALL characters in the scene, not
        // per character: past the cap, the submesh falls back to the global triplet.
        // First those returned by a deleted character.
        UINT slot = 0;
        bool haySlot = false;
        if (!freeSkinnedSrv.empty()) {
            slot = freeSkinnedSrv.back();
            freeSkinnedSrv.pop_back();
            haySlot = true;
        } else if (nextSkinnedSlot < kMaxSkinnedSlots) {
            slot = kSrvSkinned + nextSkinnedSlot * kSrvPerObject;
            ++nextSkinnedSlot;
            haySlot = true;
        }
        if (haySlot) {
            sub.srvBase = slot;

            // Shared among characters of the same FBX. On a cache miss it uploads
            // and writes the view (uploadMaterialTexture); on a hit it only
            // writes this character's view over the already uploaded image, with
            // the same format. nullptr = no texture: the caller puts its
            // filler, and the cache does not store it.
            auto pedirTextura = [&](const std::string& ruta, const std::vector<uint8_t>& emb,
                                    TextureKind tipo, UINT srvIndex) -> D3D12MA::Allocation* {
                bool creada = false;
                D3D12MA::Allocation* a = skinnedTextures.acquire(
                    makeTextureKey(ruta, emb, tipo, textureKeySuffix(ruta)),
                    [&] { return uploadMaterialTexture(ruta, emb, tipo, srvIndex); }, &creada);
                // On a hit, the format is that of the already uploaded resource (resolveSrgb
                // decided it when it was created), not an assumption by slot.
                if (a && !creada)
                    createTexture2DSrv(a->GetResource(), formatOf(a), srvIndex);
                return a;
            };

            D3D12MA::Allocation* base = pedirTextura(
                range.material->texturePath, range.material->embeddedTexture, TextureKind::BaseColor, slot);
            if (base)
                object.textures.push_back(base);
            else
                createTexture2DSrv(baseColorAllocation->GetResource(),
                                   DXGI_FORMAT_R8G8B8A8_UNORM, slot);

            D3D12MA::Allocation* normal = pedirTextura(
                range.material->normalMapPath, range.material->embeddedNormalMap, TextureKind::Normal, slot + 1);
            if (normal)
                object.textures.push_back(normal);
            else
                createTexture2DSrv(normalMapAllocation->GetResource(),
                                   DXGI_FORMAT_R8G8B8A8_UNORM, slot + 1);

            fillSharedSlots(slot);

            if (D3D12MA::Allocation* orm =
                    pedirTextura(range.material->metallicRoughnessPath,
                                 range.material->embeddedMetallicRoughness, TextureKind::Orm,
                                 slot + 3))
                object.textures.push_back(orm);
        }
        object.subMeshes.push_back(sub);
    }

    // The animation clock starts with the first character: until then there is
    // nothing to advance, and leaving it at zero would cause a jump in the first frame.
    if (skinnedObjects.empty()) {
        QueryPerformanceFrequency(&tickFrequency);
        QueryPerformanceCounter(&lastTick);
    }

    // Recycled slot if there is one; otherwise, the vector grows as usual.
    const int slot = skinnedSlots.acquire();
    stored = true;
    if (slot < 0) {
        skinnedObjects.push_back(std::move(object));
        return static_cast<int>(skinnedObjects.size() - 1);
    }
    skinnedObjects[static_cast<size_t>(slot)] = std::move(object);
    return slot;
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
                  "D3D12MA::Allocator::CreateResource(character instances)");

    const D3D12_RANGE noRead{0, 0};
    throwIfFailed(skinnedInstanceAllocations[frameIndex]->GetResource()->Map(
                      0, &noRead, &skinnedInstanceMapped[frameIndex]),
                  "ID3D12Resource::Map(character instances)");
    skinnedInstanceCapacity[frameIndex] = newCapacity;
}

void D3D12Renderer::Impl::releaseSkinnedResources(SkinnedObject& character)
{
    SkinnedGeometry geo;
    geo.posKeys    = character.posKeys;
    geo.rotKeys    = character.rotKeys;
    geo.scaleKeys  = character.scaleKeys;
    geo.boneInfos  = character.boneInfos;
    geo.inputVerts = character.inputVerts;
    geo.indices    = character.indices;
    const auto releaseGeo = [](const SkinnedGeometry& g) {
        for (D3D12MA::Allocation* a : {g.posKeys, g.rotKeys, g.scaleKeys, g.boneInfos, g.inputVerts, g.indices})
            if (a)
                a->Release();
    };
    if (skinnedGeometry.contains(geo))
        skinnedGeometry.release(geo, releaseGeo);
    else if (!(geo == SkinnedGeometry{}))
        releaseGeo(geo);
    for (D3D12MA::Allocation** allocation :
         {&character.posKeys, &character.rotKeys, &character.scaleKeys, &character.boneInfos,
          &character.inputVerts, &character.indices})
        *allocation = nullptr;

    for (D3D12MA::Allocation** allocation :
         {&character.localXforms, &character.finalBones, &character.poseTrs, &character.frozenTrs,
          &character.poseBlock, &character.ikBlock, &character.outputVerts}) {
        if (*allocation) {
            (*allocation)->Release();
            *allocation = nullptr;
        }
    }
    for (D3D12MA::Allocation* texture : character.textures)
        if (texture)
            skinnedTextures.release(texture, [](D3D12MA::Allocation* const& a) { a->Release(); });
    character.textures.clear();

    // The triplets go back to the pool. A character takes one per submesh, so
    // without this the cap of 16 ran out after two or three scene reloads
    // even with not a single character alive.
    for (const SkinnedSubMesh& sub : character.subMeshes)
        if (sub.srvBase >= kSrvSkinned &&
            sub.srvBase < kSrvSkinned + kMaxSkinnedSlots * kSrvPerObject)
            freeSkinnedSrv.push_back(sub.srvBase);
    character.subMeshes.clear();
}

void D3D12Renderer::Impl::releaseSkinnedObjects()
{
    for (SkinnedObject& character : skinnedObjects)
        releaseSkinnedResources(character);
    skinnedObjects.clear();
    // All characters were released: the caches have to be empty. If not,
    // someone skipped the release; a warning is given and nothing is freed blindly.
    if (skinnedTextures.size() != 0)
        diagLog("[D3D12] " + std::to_string(skinnedTextures.size()) +
                " character textures not released at shutdown");
    if (skinnedGeometry.size() != 0)
        diagLog("[D3D12] " + std::to_string(skinnedGeometry.size()) +
                " character geometries not released at shutdown");
    skinnedSlots.clear();
    freeSkinnedSrv.clear();
    nextSkinnedSlot = 0;
}

void D3D12Renderer::Impl::recordSkinning()
{
    std::vector<SkinnedObject*> activos;
    activos.reserve(skinnedObjects.size());
    for (SkinnedObject& object : skinnedObjects)
        if (object.vertexCount != 0)
            activos.push_back(&object);
    if (activos.empty())
        return;

    // This frame's pose block, in its copy: the Animator's pose or,
    // without it, a sample (clipBase at animTime) with the backend's clock.
    for (SkinnedObject* object : activos) {
        if (!object->poseBlockMapped)
            continue;
        const uint32_t B = object->boneCount;
        AnimationPose unica;
        unica.count = 1;
        unica.samples[0] = { B > 0 ? static_cast<int>(object->clipBase / B) : 0, object->animTime, 1.0f, 0 };
        AnimationPose& pose = object->hasPose ? object->pose : unica;
        // The masks point to the object's copy (the Animator's is no longer valid).
        for (int L = 0; L < kMaxLayersPose; L++)
            pose.layers[L].mask = object->poseMasks[L].empty() ? nullptr : &object->poseMasks[L];
        writePoseBlock(pose, B, static_cast<uint32_t*>(object->poseBlockMapped) +
                                    static_cast<size_t>(frameIndex) * poseBlockUints(B));
        if (object->ikBlockMapped)
            writeIkBlock(object->ik, static_cast<uint32_t*>(object->ikBlockMapped) +
                                         static_cast<size_t>(frameIndex) * ikBlockUints());
    }

    auto pushDe = [this](const SkinnedObject& object) {
        ComputePush push{};
        push.boneCount       = object.boneCount;
        push.vertexCount     = object.vertexCount;
        push.rootMotionMode  = object.hasPose ? object.pose.rootMotionMode : 0u;
        push.poseBlockOffset = frameIndex * poseBlockUints(object.boneCount);
        push.ikBlockOffset   = frameIndex * ikBlockUints();
        return push;
    };

    // Freeze the on-screen pose of each layer with an interrupted fade before
    // evaluating: poseTrs holds the previous frame's. Once per request.
    for (SkinnedObject* object : activos) {
        if (!object->hasPose)
            continue;
        const UINT64 capa = static_cast<UINT64>(object->boneCount) * 3 * sizeof(glm::vec4);
        for (int L = 0; L < object->pose.layerCount; L++) {
            if (!object->pose.layers[L].freezeNow)
                continue;
            D3D12_RESOURCE_BARRIER aCopia[2]{};
            for (int b = 0; b < 2; b++) {
                aCopia[b].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                aCopia[b].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                aCopia[b].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }
            aCopia[0].Transition.pResource  = object->poseTrs->GetResource();
            aCopia[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            aCopia[1].Transition.pResource  = object->frozenTrs->GetResource();
            aCopia[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            commandList->ResourceBarrier(2, aCopia);
            commandList->CopyBufferRegion(object->frozenTrs->GetResource(), capa * L,
                                          object->poseTrs->GetResource(), capa * L, capa);
            for (int b = 0; b < 2; b++)
                std::swap(aCopia[b].Transition.StateBefore, aCopia[b].Transition.StateAfter);
            commandList->ResourceBarrier(2, aCopia);
            object->pose.layers[L].freezeNow = false;
        }
    }
    auto gpu = [](const auto& buffer) { return buffer->GetResource()->GetGPUVirtualAddress(); };

    // Three PHASES for all characters, not three passes per character: each one's
    // buffers are its own, so within a phase they do not depend on
    // each other and ONE barrier between phases is enough. Before it was two UAV barriers
    // per character, which serialized all of them (docs/animation-audit.md, row
    // 9). The UAV barrier with no resource covers all UAV accesses in the list.
    // UAV barrier and not a transition: the passes do not change state, it is only
    // necessary to guarantee that what one phase wrote is seen by the next.
    auto barreraEntreFases = [&]() {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = nullptr;
        commandList->ResourceBarrier(1, &barrier);
    };

    // 1) Animation keys -> local transforms. One thread per bone.
    commandList->SetComputeRootSignature(boneEvalRootSignature.Get());
    commandList->SetPipelineState(boneEvalPipeline.Get());
    for (const SkinnedObject* object : activos) {
        const ComputePush push = pushDe(*object);
        commandList->SetComputeRoot32BitConstants(0, sizeof(ComputePush) / 4, &push, 0);
        commandList->SetComputeRootShaderResourceView(1, gpu(object->posKeys));
        commandList->SetComputeRootShaderResourceView(2, gpu(object->rotKeys));
        commandList->SetComputeRootShaderResourceView(3, gpu(object->scaleKeys));
        commandList->SetComputeRootShaderResourceView(4, gpu(object->boneInfos));
        commandList->SetComputeRootUnorderedAccessView(5, gpu(object->localXforms));
        commandList->SetComputeRootUnorderedAccessView(6, gpu(object->poseTrs));
        commandList->SetComputeRootShaderResourceView(7, gpu(object->frozenTrs));
        commandList->SetComputeRootShaderResourceView(8, gpu(object->poseBlock));
        commandList->Dispatch((object->boneCount + 63) / 64, 1, 1);
    }
    barreraEntreFases();

    // 1b) Characters with IK: the hierarchy runs one extra time, WITHOUT pass
    // 2 (flags bit 0), so bone_ik can read the world transforms and
    // correct the locals. The others pay nothing.
    std::vector<SkinnedObject*> conIk;
    for (SkinnedObject* object : activos)
        if (object->ik.count > 0) conIk.push_back(object);
    if (!conIk.empty()) {
        commandList->SetComputeRootSignature(boneHierarchyRootSignature.Get());
        commandList->SetPipelineState(boneHierarchyPipeline.Get());
        for (const SkinnedObject* object : conIk) {
            ComputePush push = pushDe(*object);
            push.flags = 1u;
            commandList->SetComputeRoot32BitConstants(0, sizeof(ComputePush) / 4, &push, 0);
            commandList->SetComputeRootShaderResourceView(1, gpu(object->boneInfos));
            commandList->SetComputeRootShaderResourceView(2, gpu(object->localXforms));
            commandList->SetComputeRootUnorderedAccessView(3, gpu(object->finalBones));
            commandList->Dispatch(1, 1, 1);
        }
        barreraEntreFases();
        commandList->SetComputeRootSignature(boneIkRootSignature.Get());
        commandList->SetPipelineState(boneIkPipeline.Get());
        for (const SkinnedObject* object : conIk) {
            ComputePush push = pushDe(*object);
            push.flags = 1u;
            commandList->SetComputeRoot32BitConstants(0, sizeof(ComputePush) / 4, &push, 0);
            commandList->SetComputeRootUnorderedAccessView(1, gpu(object->localXforms));
            commandList->SetComputeRootShaderResourceView(2, gpu(object->finalBones));
            commandList->SetComputeRootShaderResourceView(3, gpu(object->boneInfos));
            commandList->SetComputeRootShaderResourceView(4, gpu(object->ikBlock));
            commandList->Dispatch(1, 1, 1);
        }
        barreraEntreFases();
    }

    // 2) Hierarchy: accumulates parent to child. One workgroup per character, which goes
    // through depth levels (bone_hierarchy.comp).
    commandList->SetComputeRootSignature(boneHierarchyRootSignature.Get());
    commandList->SetPipelineState(boneHierarchyPipeline.Get());
    for (const SkinnedObject* object : activos) {
        const ComputePush push = pushDe(*object);
        commandList->SetComputeRoot32BitConstants(0, sizeof(ComputePush) / 4, &push, 0);
        commandList->SetComputeRootShaderResourceView(1, gpu(object->boneInfos));
        commandList->SetComputeRootShaderResourceView(2, gpu(object->localXforms));
        commandList->SetComputeRootUnorderedAccessView(3, gpu(object->finalBones));
        commandList->Dispatch(1, 1, 1);
    }
    barreraEntreFases();

    // 3) Vertex deformation. One thread per vertex.
    commandList->SetComputeRootSignature(skinningRootSignature.Get());
    commandList->SetPipelineState(skinningPipeline.Get());
    for (const SkinnedObject* object : activos) {
        const ComputePush push = pushDe(*object);
        commandList->SetComputeRoot32BitConstants(0, sizeof(ComputePush) / 4, &push, 0);
        commandList->SetComputeRootShaderResourceView(1, gpu(object->finalBones));
        commandList->SetComputeRootShaderResourceView(2, gpu(object->inputVerts));
        commandList->SetComputeRootUnorderedAccessView(3, gpu(object->outputVerts));
        commandList->Dispatch((object->vertexCount + 63) / 64, 1, 1);
    }

    // From compute write to vertex assembler input: here the buffer's usage
    // does change, so a transition is needed. All in a
    // single call.
    std::vector<D3D12_RESOURCE_BARRIER> alDibujo(activos.size());
    for (size_t i = 0; i < activos.size(); i++) {
        D3D12_RESOURCE_BARRIER& b = alDibujo[i];
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = activos[i]->outputVerts->GetResource();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    commandList->ResourceBarrier((UINT)alDibujo.size(), alDibujo.data());
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
    taaHistoryAllocations  = {};
    taaHistoryValid        = false;
    // The new resources again come out of a heap that is not zeroed, so
    // the first use has to be repeated. Without this startup stops warning but
    // every resize still first-uses without first-using, which is where most of
    // the id=1422 came from.
    taaHistoryInicializada = {};
    viewportInicializado   = false;
    ldrInicializado        = false;

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

    // Reflection trace target: same format and size as the scene,
    // because what it stores is reprojected scene color.
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

    // Motion blur target: the same blurred scene, so same
    // format and size. It has no SRV because only a CopyResource comes out of here.
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

    // Scene target, in floating point so the bloom threshold can
    // tell apart what exceeds 1.0.
    D3D12_RESOURCE_DESC hdrDesc{};
    hdrDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    hdrDesc.Width            = width;
    hdrDesc.Height           = height;
    hdrDesc.DepthOrArraySize = 1;
    hdrDesc.MipLevels        = 1;
    hdrDesc.Format           = kHdrFormat;
    hdrDesc.SampleDesc.Count = 1;
    // Render target for the scene and unordered access for the fog, which
    // rewrites this same content before the bloom reads it.
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

    // The target's RTV goes after those of the swapchain, in the same heap.
    D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    hdrRtv.ptr += static_cast<SIZE_T>(kRtvSceneHdr) * rtvSize;
    device->CreateRenderTargetView(hdrAllocation->GetResource(), nullptr, hdrRtv);

    if (sampleCount > 1) {
        // Color and depth of the scene pass with MSAA. The rest of the
        // chain (SSR, fog, bloom, composition) keeps reading the single-sample ones:
        // a resolve goes in between.
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
                      "D3D12MA::Allocator::CreateResource(multisampled HDR)");

        D3D12_CPU_DESCRIPTOR_HANDLE msRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        msRtv.ptr += static_cast<SIZE_T>(kRtvSceneMsaa) * rtvSize;
        device->CreateRenderTargetView(hdrMsAllocation->GetResource(), nullptr, msRtv);

        msDesc.Format = DXGI_FORMAT_D32_FLOAT;
        msDesc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE depthClear{};
        depthClear.Format             = DXGI_FORMAT_D32_FLOAT;
        depthClear.DepthStencil.Depth = 1.0f;
        throwIfFailed(allocator->CreateResource(&allocDesc, &msDesc,
                                                D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
                                                &depthMsAllocation, IID_NULL, nullptr),
                      "D3D12MA::Allocator::CreateResource(multisampled depth)");

        D3D12_DEPTH_STENCIL_VIEW_DESC msDsv{};
        msDsv.Format        = DXGI_FORMAT_D32_FLOAT;
        msDsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;

        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        dsvHandle.ptr += dsvSize;
        device->CreateDepthStencilView(depthMsAllocation->GetResource(), &msDsv, dsvHandle);
    }

    {
        // Viewport image, in the backbuffer's format: it is its substitute. It goes
        // at the OUTPUT size, not the render one: with SSAA the scene is drawn larger
        // and the downsample pass averages it down to here.
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
        rtv.ptr += static_cast<SIZE_T>(kRtvViewport) * rtvSize;
        device->CreateRenderTargetView(viewportAllocation->GetResource(), nullptr, rtv);

        createTexture2DSrv(viewportAllocation->GetResource(), DXGI_FORMAT_R8G8B8A8_UNORM,
                           kSrvViewport);
    }

    // TAA history: two images in the composition's format, which is what
    // the pass blends.
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
                      "D3D12MA::Allocator::CreateResource(TAA history)");

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(kRtvTaaHistory + i) * rtvSize;
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

    // The same view, again, in the first slot of the bloom-off pair:
    // the composition table requires [scene, bloom] CONTIGUOUS and the
    // black already lives in the second. Duplicating an SRV of the same resource is legal and
    // costs no memory. It goes here and not in createMeshResources because hdrAllocation
    // is redone on every resize and the view has to follow it.
    handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(kSrvCompositeOff) * srvSize;
    device->CreateShaderResourceView(hdrAllocation->GetResource(), &hdrSrv, handle);

    // Write view of the same target, the one the fog uses.
    D3D12_UNORDERED_ACCESS_VIEW_DESC hdrUav{};
    hdrUav.Format        = kHdrFormat;
    hdrUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(kUavSceneHdr) * srvSize;
    device->CreateUnorderedAccessView(hdrAllocation->GetResource(), nullptr, &hdrUav, handle);

    // The depth was created before the heap at startup, so its sampling
    // view is registered here.
    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
    depthSrv.Format                  = DXGI_FORMAT_R32_FLOAT;
    depthSrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrv.Texture2D.MipLevels     = 1;
    handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(kSrvDepth) * srvSize;
    device->CreateShaderResourceView(depthAllocation->GetResource(), &depthSrv, handle);

    // LDR target: written by the composition and read by FXAA. Without it, FXAA would have
    // to read from the backbuffer while writing to it.
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
    ldrRtv.ptr += static_cast<SIZE_T>(kRtvLdr) * rtvSize;
    device->CreateRenderTargetView(ldrAllocation->GetResource(), nullptr, ldrRtv);

    D3D12_SHADER_RESOURCE_VIEW_DESC ldrSrv{};
    ldrSrv.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    ldrSrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    ldrSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ldrSrv.Texture2D.MipLevels     = 1;
    handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(kSrvLdr) * srvSize;
    device->CreateShaderResourceView(ldrAllocation->GetResource(), &ldrSrv, handle);

    // Bloom levels: independent textures instead of mips of a single
    // resource. Each one has a single subresource, so its state is changed
    // in one piece and there is no need to keep count per level.
    // Levels that are REALLY useful. The Vulkan path (BloomPass::createImages) stops
    // as soon as a level would drop below 2 px, and here all five were always used,
    // clamping to 1x1: with a small viewport D3D12 blurred over degenerate mips
    // (which average the whole screen) and Vulkan did not. Same rule in
    // both, or the same narrow panel gives two different images.
    //
    // The RESERVATION stays at kBloomMips because the descriptor layout takes it
    // for granted (kUavBloomMip = kSrvBloomMip + kBloomMips); what is bounded is
    // how many are used.
    bloomMipCount = 0;
    {
        UINT w = width / 2, h = height / 2;
        while (bloomMipCount < static_cast<UINT>(kBloomMips) && w >= 2 && h >= 2) {
            ++bloomMipCount;
            w /= 2;
            h /= 2;
        }
    }

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
    // The two computes share a signature: constants, a source texture and a
    // destination image. Texture2D and RWTexture2D cannot go as root
    // descriptors (only buffers can), so they go in tables.
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

    // Clamp at the edges: with wrap, the 13-tap filter would bring color from the
    // opposite side of the image and the bloom would bleed from one edge to the other.
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
            throw std::runtime_error(std::string("D3D12: root signature of ") + what + " (HRESULT " +
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

    // Composition: scene + bloom -> backbuffer. t0 and t1 have to land in
    // contiguous descriptors, and that is why sceneHdr and bloom level 0 are
    // stuck together in the heap.
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
    serializeAndCreate(compositeDesc, compositeRootSignature, "composite");

    // fullscreen.vert generates the triangle from the vertex index: no
    // vertex buffer and no input layout.
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
                  "ID3D12Device::CreateGraphicsPipelineState(composite)");
}

void D3D12Renderer::Impl::createNeutralIblCubes()
{
    // Half float, which is the real IBL's format: this way the same slot
    // later serves for the compute result without recreating the view.
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

    // Irradiance: one dispatch for all six faces at once (z = face).
    {
        const IblPush push{0.0f, kIblIrradianceSize, intensity};
        commandList->SetPipelineState(iblIrradiancePipeline.Get());
        commandList->SetComputeRoot32BitConstants(0, 3, &push, 0);
        commandList->SetComputeRootDescriptorTable(1, gpuHandle(sourceSrv));
        commandList->SetComputeRootDescriptorTable(2, gpuHandle(irradianceUav));
        const UINT groups = (kIblIrradianceSize + 7) / 8;
        commandList->Dispatch(groups, groups, 6);
    }

    // Prefilter: one dispatch per mip, with its roughness and its size.
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
    // Without a sky there is nothing to convolve: the neutrals stay.
    if (!skyboxAllocation)
        return;

    // The two destinations, with the same format as the neutrals they
    // replace. CUBE comes from the view, not the resource: for the compute it is an
    // array of six layers and for pbr.frag a TextureCube.
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

    // One UAV per destination: the irradiance one covers its six layers; the
    // prefilter one goes per mip, because each level is a different roughness and is
    // dispatched separately.
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

    // Root signature common to both computes: the three-float push, the
    // sky cubemap in t0 and the destination in u1.
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
            throw std::runtime_error("D3D12: IBL root signature (HRESULT " +
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

    // It is recorded and waited on right here: this runs once when the sky loads,
    // not per frame, and the rest of init already blocks the same way.
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

    // The same dispatches each probe uses: input, destinations and intensity
    // as parameters, and everything else identical.
    recordIblConvolution(kSrvSkybox, kUavIrradiance, kUavPrefilter, 1.0f);

    // From write destination to read texture: pbr.frag samples them in the
    // scene pass from the same frame onwards.
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

    // The read views, now over the new resources: the global block
    // and that of each already loaded object.
    createCubeSrv(irradianceAllocation->GetResource(), kHdrFormat, 1, kSrvIrradiance);
    createCubeSrv(prefilterAllocation->GetResource(), kHdrFormat, prefilterMips, kSrvPrefilter);
    // Only the two environment slots: redoing the whole block would overwrite each
    // mesh's own metallic-roughness with the neutral.
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
    // Forward+ off: mode = 0 and a one-cell grid. pbr.frag reads mode
    // before anything else and sticks to the loop over the UBO lights, but the
    // four buffers have to be bound anyway.
    // Parameters and lights go in an upload heap and mapped: they are rewritten every
    // frame with the camera and the live lights.
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

    // The zeros used to clear the statistics block before every
    // dispatch. They are written ONCE: the contents never change.
    createMapped(kFpStatsWords * sizeof(uint32_t), &fpStatsZeros, &fpStatsZerosMapped);
    std::memset(fpStatsZerosMapped, 0, kFpStatsWords * sizeof(uint32_t));

    // And the read destination. Failing is no reason not to draw: without it,
    // the statistics stay at zero and the rest of Forward+ works the same.
    {
        D3D12_HEAP_PROPERTIES readbackHeap{};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = static_cast<UINT64>(kFrameCount) * kFpStatsWords * sizeof(uint32_t);
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (SUCCEEDED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&fpStatsReadback)))) {
            void* mapped = nullptr;
            if (SUCCEEDED(fpStatsReadback->Map(0, nullptr, &mapped)))
                fpStatsMapped = static_cast<const uint32_t*>(mapped);
            else
                fpStatsReadback.Reset();
        }
    }

    // With Forward+ off there is no grid, but the four buffers have to be
    // bound anyway: pbr.frag declares them without a branch. One cell is enough.
    ensureForwardPlusGrid(1);

    // And the block, already written as Off: pbr.frag reads mode before anything else.
    FpParamsGpu off{};
    off.gridX = off.gridY = off.gridZ = 1;
    off.tileSize   = kFpTileSize;
    off.maxPerCell = kFpMaxPerCell;
    off.zNear      = 0.1f;
    off.zFar       = 500.0f;
    std::memcpy(fpParamsMapped, &off, sizeof(off));
}

bool D3D12Renderer::Impl::loadSkyboxCubemap()
{
    // Same face order as the Vulkan path: +X, -X, +Y, -Y, +Z, -Z,
    // which is what a TextureCube expects per slice.
    const std::array<std::string, 6>& facePaths = skyboxFacePaths;

    // Reload: the previous cubemap is released here. The caller has to
    // have stopped the GPU beforehand.
    if (skyboxAllocation) {
        skyboxAllocation->Release();
        skyboxAllocation = nullptr;
    }

    int      faceWidth = 0, faceHeight = 0, channels = 0;
    stbi_uc* faces[6] = {};
    bool     ok       = true;
    for (int i = 0; i < 6; ++i) {
        int w = 0, h = 0;
        faces[i] = stbi_load(facePaths[i].c_str(), &w, &h, &channels, STBI_rgb_alpha);
        if (!faces[i]) {
            ok = false;
            break;
        }
        if (i == 0) {
            faceWidth  = w;
            faceHeight = h;
        } else if (w != faceWidth || h != faceHeight) {
            // A cubemap with faces of different sizes is not a cubemap: the
            // resource is ONE with six slices of the same size.
            ok = false;
            break;
        }
    }

    if (ok && faceWidth > 0 && faceHeight > 0) {
        // The six faces in a row, which is how uploadTexture walks the array.
        const size_t         faceBytes = static_cast<size_t>(faceWidth) * faceHeight * 4;
        std::vector<uint8_t> cube(faceBytes * 6);
        for (int i = 0; i < 6; ++i)
            std::memcpy(cube.data() + faceBytes * i, faces[i], faceBytes);

        skyboxAllocation = uploadTexture(cube.data(), static_cast<UINT>(faceWidth),
                                         static_cast<UINT>(faceHeight), 6,
                                         DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4, kSrvSkybox);

        // uploadTexture leaves a 2D array SRV; the shader declares TextureCube,
        // and with the array view the sampling direction means nothing.
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

    return skyboxAllocation != nullptr;
}

void D3D12Renderer::Impl::createSkyboxResources()
{
    if (loadSkyboxCubemap())
        createSkyboxPipelineOnly();
}

void D3D12Renderer::Impl::createSkyboxPipelineOnly()
{
    if (!skyboxAllocation)
        return;

    // Root signature: the invViewProj as root constants (b0) and the cubemap in
    // a table (t0). The vertex shader reads no vertices (it takes the three corners
    // from SV_VertexID), so there is no input layout.
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
        throw std::runtime_error("D3D12: sky root signature (HRESULT " +
                                 hresultToString(hr) + ") " + detail);
    }
    throwIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                              serialized->GetBufferSize(),
                                              IID_PPV_ARGS(&skyboxRootSignature)),
                  "ID3D12Device::CreateRootSignature(sky)");

    const std::vector<char> vertexShader = readBinaryFile("shaders/skybox.vert.dxil");
    const std::vector<char> pixelShader  = readBinaryFile("shaders/skybox.frag.dxil");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = skyboxRootSignature.Get();
    psoDesc.VS                    = {vertexShader.data(), vertexShader.size()};
    psoDesc.PS                    = {pixelShader.data(), pixelShader.size()};
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets      = 1;
    // The sky goes to the HDR target, with the scene: this way the composition
    // tonemaps it like everything else and it can generate bloom.
    psoDesc.RTVFormats[0]    = kHdrFormat;
    psoDesc.DSVFormat        = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = sampleCount;
    psoDesc.SampleMask       = UINT_MAX;

    psoDesc.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    for (auto& rt : psoDesc.BlendState.RenderTarget)
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // The triangle comes out with z = 1: it is drawn last, only where there is no
    // geometry, and does NOT write depth: the fog reads that buffer and a
    // sky at maximum distance would make it tint the whole screen.
    psoDesc.DepthStencilState.DepthEnable    = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    psoDesc.DepthStencilState.StencilEnable  = FALSE;

    throwIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&skyboxPipeline)),
                  "ID3D12Device::CreateGraphicsPipelineState(sky)");
}

UINT D3D12Renderer::Impl::desiredSampleCount() const
{
    if (state->aaMode() != RendererState::AaMode::Msaa)
        return 1;

    // What the user requests, clamped to what the device accepts for the
    // scene's format: asking for 8 where there are only 4 does not fail when creating the
    // texture, it fails when creating the pipeline, and by then it is too late.
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

    // And the draw size, which with SSAA is the output one multiplied by the
    // factor. The cap of a 2D texture in D3D12 is 16384 per side: asking for more
    // does not fail when creating the resource, it fails when using it.
    UINT wanted  = wantedOut;
    UINT wantedH = wantedOutH;
    if (state->aaMode() == RendererState::AaMode::Ssaa && state->ssaaFactor() > 1.0f) {
        constexpr UINT kMaxTextureSide = 16384;
        wanted  = (std::min)(static_cast<UINT>(std::lround(wantedOut * state->ssaaFactor())),
                             kMaxTextureSide);
        wantedH = (std::min)(static_cast<UINT>(std::lround(wantedOutH * state->ssaaFactor())),
                             kMaxTextureSide);
    }

    if (wanted == width && wantedH == height && wantedOut == outWidth && wantedOutH == outHeight)
        return;

    // Everything internal is at render size: depth, scene, bloom,
    // occlusion, history and the LDR. The panel image is the exception (it goes at
    // the output size) and the swapchain is NOT touched.
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

void D3D12Renderer::Impl::applyPendingShadowSize()
{
    if (pendingShadowMapSize == 0 || pendingShadowMapSize == shadowMapSize) {
        pendingShadowMapSize = 0;
        return;
    }
    if (!shadowMapArrayAllocation) {
        // There is no shadow pass yet: the size will be taken when it is created. The pending value is NOT
        // cleared, so it gets applied as soon as it exists.
        diagLog("shadow: resize a " + std::to_string(pendingShadowMapSize) +
                " deferred, there is no map yet.");
        return;
    }

    diagLog("shadow: resize " + std::to_string(shadowMapSize) + " -> " +
            std::to_string(pendingShadowMapSize) + " (" + std::to_string(objects.size()) +
            " objects, " + std::to_string(skinnedObjects.size()) + " characters).");

    // The map may be in the previous frame. It is a quality setting that gets
    // touched once in a blue moon, so waiting is cheaper than carrying a
    // deferred delete for this.
    waitForGpu();

    shadowMapSize        = pendingShadowMapSize;
    pendingShadowMapSize = 0;

    shadowMapArrayAllocation->Release();
    shadowMapArrayAllocation = nullptr;

    D3D12_RESOURCE_DESC shadowDesc{};
    shadowDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    shadowDesc.Width            = shadowMapSize;
    shadowDesc.Height           = shadowMapSize;
    shadowDesc.DepthOrArraySize = kShadowLayers;
    shadowDesc.MipLevels        = 1;
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
                  "D3D12MA::Allocator::CreateResource(shadow map, resize)");

    // The DSV heap is not redone (it still has kShadowCascades slots) but
    // the VIEWS hang off the old resource, so they have to be rewritten.
    for (int cascade = 0; cascade < kShadowLayers; ++cascade) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format                         = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.FirstArraySlice = cascade;
        dsvDesc.Texture2DArray.ArraySize       = 1;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(cascade) * dsvSize;
        device->CreateDepthStencilView(shadowMapArrayAllocation->GetResource(), &dsvDesc, handle);
    }

    // And all the t3, just as createShadowResources does when building it: the
    // global one and that of each already loaded object and submesh. Without this they sample
    // a resource that has just died.
    createShadowMapSrv(kSrvShadowMap);
    for (const StaticObject& object : objects)
        if (object.srvBase != kSrvBaseColor)
            createShadowMapSrv(object.srvBase + 2);
    for (const SkinnedObject& character : skinnedObjects)
        for (const SkinnedSubMesh& sub : character.subMeshes)
            if (sub.srvBase != kSrvBaseColor)
                createShadowMapSrv(sub.srvBase + 2);

    // From the resource, not from the variable: if this does not say what was requested, the one
    // that is wrong is the CreateResource above and not the path that leads to it.
    const D3D12_RESOURCE_DESC hecho = shadowMapArrayAllocation->GetResource()->GetDesc();
    diagLog("shadow: map rebuilt, " + std::to_string(hecho.Width) + "x" +
            std::to_string(hecho.Height) + ", " + std::to_string(hecho.DepthOrArraySize) +
            " layers.");
}

void D3D12Renderer::Impl::applyPendingSampleCount()
{
    const UINT wanted = desiredSampleCount();
    if (wanted == sampleCount)
        return;

    // Changes the number of samples of the targets AND of all the pipelines that
    // draw into them: the GPU has to be waited on to release the old ones.
    waitForGpu();
    sampleCount = wanted;

    createHdrTargets();
    createMeshPipeline();
    createSkinningPipelines();
    createGizmoPipeline();
    createSkyboxPipelineOnly();
    // The WORLD canvases also draw into the scene target, so their
    // two PSOs carry their SampleDesc. It is the ONLY recreation path that
    // affects them: a size change (applyPendingResize/applyPendingRenderSize)
    // moves the resources but not the samples, and in D3D12 a PSO is not tied to
    // any render pass object. Forgetting it would leave both compiled for
    // another sample count: device lost when using them, without a single warning.
    createUiWorldPipelines();
}

void D3D12Renderer::Impl::createForwardPlusPipelines()
{
    // t0 parameters, t1 lights, u2 cells, u3 indices, t4 depth, u5
    // statistics. The buffers go as root SRV/UAV (they are ByteAddressBuffers and
    // need no descriptor) and the depth, which is a texture, in a table.
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
        throw std::runtime_error("D3D12: culling root signature (HRESULT " +
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

    // The output ones are redone: their size depends on the grid, and the grid
    // on the window size and the mode.
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
    fpStatsAllocation   = createStorage(kFpStatsWords * sizeof(uint32_t));
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

    // zNear and zFar come from the projection itself (RH_ZO): p22 = f/(n-f) and
    // p32 = f*n/(n-f). Taking them from there is what keeps the grid attached to
    // the camera being used, without duplicating its planes elsewhere.
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
    // Inverse of the logarithmic split of clustered culling:
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
            // The radius does not travel in the UBO light. Same criterion as
            // ForwardPlusPass in Vulkan: the one the caller sends for THIS light,
            // and if there is none, the global one from RendererState (which is the one edited by the
            // "Light radius" slider of the Forward+ panel).
            const float radius = (i < lightRadii.size()) ? lightRadii[i]
                                                         : state->forwardPlusLightRadius();
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

    // Tiled reduces the tile's depth by reading it; clustered does not
    // read it. But that is what the SHADER does: this code needs it just the same in
    // both modes, because it transitions it twice (the barrier down here
    // and its inverse in toScene[2]) and binds kSrvPrepassDepth in table 6 without
    // looking at the mode. That is why the guard cannot let clustered through.
    //
    // Today it is not reached: init() calls createSsaoTargets() unconditionally and the
    // only place that releases the resource without recreating it is shutdown(). It is
    // hardened because the previous guard claimed the opposite of what the
    // body does, and that contradiction is what bills the piece the day
    // the SSAO targets are created only with the effect on.
    if (!prepassDepthAllocation)
        return;

    // Those of the last frame that used this slot, BEFORE overwriting them. The
    // slot already went through moveToNextFrame, which waited on its fence, so what is there
    // is complete. Same criterion and same lag as readTimestamps.
    //
    // [0] lights distributed, [1] cells with any light, [2] overflowed cells:
    // the same layout the Vulkan path reads in ForwardPlusPass.
    if (fpStatsMapped) {
        const uint32_t* s = fpStatsMapped + static_cast<size_t>(frameIndex) * kFpStatsWords;
        fpAvgPerCell      = (s[1] > 0) ? static_cast<float>(s[0]) / static_cast<float>(s[1]) : 0.0f;
        fpOverflowCells   = s[2];
    }

    // To zero before the dispatch: the shader ACCUMULATES with atomicAdd, so without
    // this the count would be that of the whole session.
    if (fpStatsZeros) {
        D3D12_RESOURCE_BARRIER toCopy{};
        toCopy.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy.Transition.pResource   = fpStatsAllocation->GetResource();
        toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toCopy.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &toCopy);

        commandList->CopyBufferRegion(fpStatsAllocation->GetResource(), 0,
                                      fpStatsZeros->GetResource(), 0,
                                      kFpStatsWords * sizeof(uint32_t));

        std::swap(toCopy.Transition.StateBefore, toCopy.Transition.StateAfter);
        commandList->ResourceBarrier(1, &toCopy);
    }

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

    // One group per cell: tiled has one thread per pixel of the tile (16x16) and
    // clustered distributes 4x4x4 cells per group.
    if (clustered)
        commandList->Dispatch((gridX + 3) / 4, (gridY + 3) / 4, (gridZ + 3) / 4);
    else
        commandList->Dispatch(gridX, gridY, 1);

    // And to THIS slot's read region, to read it when its turn comes around again.
    if (fpStatsReadback) {
        D3D12_RESOURCE_BARRIER toSrc{};
        toSrc.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toSrc.Transition.pResource   = fpStatsAllocation->GetResource();
        toSrc.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toSrc.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toSrc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &toSrc);

        commandList->CopyBufferRegion(
            fpStatsReadback.Get(),
            static_cast<UINT64>(frameIndex) * kFpStatsWords * sizeof(uint32_t),
            fpStatsAllocation->GetResource(), 0, kFpStatsWords * sizeof(uint32_t));

        std::swap(toSrc.Transition.StateBefore, toSrc.Transition.StateAfter);
        commandList->ResourceBarrier(1, &toSrc);
    }

    // The lists move to read for the scene pass, and the depth goes back to
    // write for the next frame.
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
    // t0 the frame's image, t1 the history, t2 the depth. Each in
    // its own table: they live in heap slots that are not contiguous.
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

    // The image and the history are filtered (the reprojection lands between pixels);
    // the depth is not.
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
        throw std::runtime_error("D3D12: TAA root signature (HRESULT " + hresultToString(hr) +
                                 ") " + detail);
    }
    throwIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                              serialized->GetBufferSize(),
                                              IID_PPV_ARGS(&taaRootSignature)),
                  "ID3D12Device::CreateRootSignature(TAA)");

    const std::vector<char> vertexShader = readBinaryFile("shaders/fullscreen.vert.dxil");
    const std::vector<char> pixelShader  = readBinaryFile("shaders/taa.frag.dxil");

    // Two destinations: the backbuffer and this frame's history, which is what
    // the next one will read. The shader writes both in the same pass.
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

    // After the transition, which is where DiscardResource wants it.
    estrenarRenderTarget(taaHistoryAllocations[writeIndex]->GetResource(),
                         taaHistoryInicializada[writeIndex]);

    transition(readableDepth(), readableDepthState(),
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_CPU_DESCRIPTOR_HANDLE historyRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    historyRtv.ptr += static_cast<SIZE_T>(kRtvTaaHistory + writeIndex) * rtvSize;

    const D3D12_CPU_DESCRIPTOR_HANDLE targets[2] = {backBufferRtv, historyRtv};
    commandList->OMSetRenderTargets(2, targets, FALSE, nullptr);

    // Negative height for the same reason as the composition: fullscreen.vert derives the
    // uv from NDC assuming the Vulkan orientation.
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
    // From THIS frame's clip to the previous one's, both without jitter: the
    // subpixel offset is sampling noise, not camera motion,
    // and putting it in here would drag the history along.
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

    // This frame's history is the one the next will read.
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

    // s0 filters (the ray lands between scene pixels) and s1 does not: interpolating
    // two depths from different surfaces gives a value that does not exist.
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
        throw std::runtime_error("D3D12: SSR root signature (HRESULT " + hresultToString(hr) +
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
    // Refinement has no setting of its own in the shared state: they are bisection
    // steps over the last segment, and with fewer than four the reflection's edge
    // gets stair-stepped.
    push.refineSteps = 5;
    push.edgeFade    = state->ssrEdgeFade();
    push.intensity   = state->ssrIntensity();

    const UINT groupsX = (width + 7) / 8;
    const UINT groupsY = (height + 7) / 8;

    ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);

    // Trace: the already drawn scene as a texture, the scene pass depth
    // (the full one, not the pre-pass's) and the own destination.
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

    // Resolve: the reflection is added onto the scene, which becomes a write
    // destination again.
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

    // And everything as it was: the fog, which comes after, expects to find the scene
    // as a render target and the depth in write state.
    transition(ssrAllocation->GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(hdrAllocation->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(readableDepth(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               readableDepthState());
}

bool D3D12Renderer::Impl::motionBlurActive() const
{
    // Fewer than two taps average nothing: the result would be the center pixel and
    // the copy back would write the same image at the cost of an entire
    // dispatch.
    return state->motionBlurEnabled() && state->motionBlurSamples() >= 2;
}

void D3D12Renderer::Impl::createMotionBlurPipeline()
{
    // Same layout as SSR: the scene in t0, the depth in t1 and the
    // destination in u2, which is how spirv-cross translates bindings 0, 1 and 2 of
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

    // s0 filters (the taps land between pixels) and s1 does not: interpolating two
    // depths from different surfaces gives a value that does not exist. CLAMP
    // so that an edge tap does not bring color from the opposite side.
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
        throw std::runtime_error("D3D12: motion blur root signature (HRESULT " +
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
    // The SAME matrix TAA reprojects with: this frame's clip (without jitter) ->
    // the previous frame's clip. Both view-projs are updated every frame, whether
    // TAA is active or not.
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

    // The copy back, and not a second dispatch: it is a 1:1 copy of the
    // whole image, which is what the hardware does best.
    transition(motionBlurAllocation->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(hdrAllocation->GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COPY_DEST);

    commandList->CopyResource(hdrAllocation->GetResource(),
                              motionBlurAllocation->GetResource());

    // And everything as it was: the bloom expects to find the scene as a render target
    // and the depth in write state.
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
            throw std::runtime_error(std::string("D3D12: root signature of ") + what + " (HRESULT " +
                                     hresultToString(hr) + ") " + detail);
        }
        throwIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                  serialized->GetBufferSize(), IID_PPV_ARGS(&out)),
                      "ID3D12Device::CreateRootSignature");
    };

    // ── Depth pre-pass ────────────────────────────────────────────────────
    // depth_prepass.vert declares the UBO trimmed to view and proj (std140 leaves
    // them at the same offsets), and takes the model from the instance buffer.
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
        serialize(rootDesc, depthPrepassRootSignature, "depth prepass");
    }

    const std::vector<char> prepassVs = readBinaryFile("shaders/depth_prepass.vert.dxil");

    // Only the position: the shader reads nothing else, and this way the same VS serves
    // the engine's vertices and those written by the skinning, which differ
    // in the size of each vertex but not in where it starts.
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
                  "ID3D12Device::CreateGraphicsPipelineState(prepass)");
    throwIfFailed(
        device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&depthPrepassSkinnedPipeline)),
        "ID3D12Device::CreateGraphicsPipelineState(skinned prepass)");

    // ── The two computes ──────────────────────────────────────────────────
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

        // The depth is sampled unfiltered: interpolating two depths
        // from different surfaces gives a value that is in neither.
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

    // The pre-pass's own depth. TYPELESS because the same resource is
    // written as depth and read as a texture.
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
                      "D3D12MA::Allocator::CreateResource(prepass depth)");

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format        = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(prepassDepthAllocation->GetResource(), &dsvDesc,
                                       prepassDsvHeap->GetCPUDescriptorHandleForHeapStart());

        createTexture2DSrv(prepassDepthAllocation->GetResource(), DXGI_FORMAT_R32_FLOAT,
                           kSrvPrepassDepth);
    }

    // Raw and blurred map, at full resolution as in Vulkan: pbr.frag
    // samples it by screen coordinate and takes for granted that it is 1:1.
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

    // And t7 of each block, through writeAoSlot and NOT pointing at the map bare: with
    // SSAO off that map is never written and is zero, which multiplied into the
    // ambient turns it off entirely. This runs at startup and on EVERY resize,
    // so setting it by hand left the blocks in a state that did not
    // match the switch.
    //
    // The GLOBAL block comes in here: it is the only one that does not go through fillSharedSlots,
    // and it is the one used by the engine's ground (the one drawn when the scene
    // has no meshes). Without this, that ground came out BLACK covering the sky.
    writeAoSlot(kSrvBaseColor);
    for (const StaticObject& object : objects)
        if (object.srvBase != kSrvBaseColor)
            writeAoSlot(object.srvBase);
    for (const SkinnedObject& character : skinnedObjects)
        for (const SkinnedSubMesh& sub : character.subMeshes)
            if (sub.srvBase != kSrvBaseColor)
                writeAoSlot(sub.srvBase);

    // And the state that refreshAoSlots queries to know whether there is anything to
    // change: without this it would keep believing the blocks say something else.
    aoSlotsUseMap = state->ssaoEnabled() && ssaoBlurAllocation != nullptr;
}

void D3D12Renderer::Impl::recordDepthPrepassAndSsao()
{
    // The pre-pass depth has four clients: occlusion, the per-tile
    // light split, and (when the scene pass one is
    // multisampled and cannot be sampled) the fog and the reflections. It is recorded
    // if any of them wants it; the two occlusion dispatches stay tied to SSAO.
    if (!prepassDepthAllocation)
        return;

    const bool wantsSsao = state->ssaoEnabled();
    const bool wantsCull = state->forwardPlusMode() == RendererState::FpMode::Tiled;
    // With MSAA, the selection outline also needs it: the scene pass one
    // is multisampled and cannot be paired with the LDR target it
    // is drawn onto.
    // Motion blur is the fifth client: it reprojects this same depth to the
    // previous frame to get each pixel's velocity.
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

    // ── 1. Depth ──────────────────────────────────────────────────────────
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

    // Same split as the shadow one: both want everything visible.
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
        // Only the depth was needed: the others read it on their own.
        return;
    }

    // ── 2. Occlusion ──────────────────────────────────────────────────────
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

    // The four coefficients with which the shader reconstructs the position in
    // view space, taken from THIS frame's projection.
    //
    // WITHOUT the Y-flip that the Vulkan path does carry, and it does not matter: this pass does not
    // leave screen space (it reconstructs and reprojects with the SAME p11),
    // so the sign cancels out. Verified by comparing the two images.
    // The full explanation is in ssao.comp.
    //
    // Careful about generalizing it: the fog does go out to world with the full matrix and
    // there the inversion has to be added by hand (see recordFog).
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

    // The raw one becomes the blur's input.
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

    // And for the scene pass to read it. All three go back to their starting state
    // so the next frame finds the same as this one.
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

    // The blur stays as read during the scene pass; the next frame
    // returns it to write before dispatching it again.
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
    // In wireframe the sky is unnecessary: it would cover the geometry one wants to see
    // from inside, and the Vulkan path omits it too.
    if (!skyboxPipeline || state->isWireframeMode())
        return;

    // The view WITHOUT translation: the sky does not get closer when walking, it only rotates.
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
            throw std::runtime_error(std::string("D3D12: root signature of ") + what + " (HRESULT " +
                                     hresultToString(hr) + ") " + detail);
        }
        throwIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                  serialized->GetBufferSize(), IID_PPV_ARGS(&out)),
                      "ID3D12Device::CreateRootSignature");
    };

    // ── Fog ─────────────────────────────────────────────────────────────────
    // u0 = scene (read and write), t1 = depth, t3 = shadows,
    // b2 = the same scene UBO. Its cbuffer declares up to lights (it needs the
    // TYPE of the key light to know how to sample the shadow map) and stops
    // dead, but the offsets are the same, so the whole buffer is bound.
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
    fogSamplers[0].Filter         = D3D12_FILTER_MIN_MAG_MIP_POINT;  // depth: unfiltered
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
    serializeAndCreate(fogDesc, fogRootSignature, "fog");

    {
        const std::vector<char>           shader = readBinaryFile("shaders/fog.comp.dxil");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = fogRootSignature.Get();
        desc.CS             = {shader.data(), shader.size()};
        throwIfFailed(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&fogPipeline)),
                      "ID3D12Device::CreateComputePipelineState(fog)");
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

    // SSAA downsample: same shape as FXAA (a full-screen triangle
    // reading the already composed image) but with another push and another shader. The
    // source is larger than the destination and the shader averages each pixel's
    // footprint; the sampler is not enough, as it would only look at the four
    // center texels.
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
    // Root signature: the orthographic as root constants (b0) and the atlas in a
    // table (t0). Everything else (mode, outline thickness, colors) travels per
    // vertex, which is what lets text land in the same batch as the
    // panel behind it.
    D3D12_DESCRIPTOR_RANGE atlasRange{};
    atlasRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    atlasRange.NumDescriptors     = 1;
    atlasRange.BaseShaderRegister = 0;  // t0

    // 17 dwords and not 16, and ALL visibility and not VERTEX: the push
    // constants block of ui.vert/ui.frag now carries, after the mat4, an
    // `int linearOutput` read by the PIXEL shader (it undoes the gamma when the
    // destination is linear HDR, which is the case of world canvases in the scene
    // pass). The HLSL comes from translating that same GLSL with spirv-cross, so
    // the b0 cbuffer has 17 dwords and both stages use it; leaving it at
    // 16/VERTEX would break PSO creation against this root signature.
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;  // b0
    params[0].Constants.Num32BitValues = 17;
    params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

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

    // The same UiVertex that the canvas builds: position in pixels, uv, color and the
    // two vec4 that carry the mode and the outline.
    // All semantics are TEXCOORDn, including the position: the HLSL comes from
    // translating the SPIR-V and spirv-cross names the inputs by their location, not
    // by what they mean. Putting POSITION here creates the pipeline and leaves the
    // attribute unbound.
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

    // Straight alpha, not premultiplied: the shader returns the color without
    // multiplying by alpha, just like in Vulkan.
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

    // And the world ones, which rely on the root signature just created.
    createUiWorldPipelines();
}

void D3D12Renderer::Impl::createUiWorldPipelines()
{
    // Without a root signature there is nothing to compile against: it happens if createUiPipeline()
    // gave up earlier (missing shaders, failed serialization). It is not an
    // error, it is that this backend is left without UI.
    if (!uiRootSignature)
        return;

    const std::vector<char> vs = readBinaryFile("shaders/ui.vert.dxil");
    const std::vector<char> ps = readBinaryFile("shaders/ui.frag.dxil");
    if (vs.empty() || ps.empty())
        return;

    // The SAME layout as the screen one: the quads of a world canvas come out of
    // the same buildDrawData, only the matrix that projects them changes.
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
    // Here is the difference from the screen one: the destination is the SCENE target,
    // which is linear HDR, has depth and can be multisampled. A
    // PSO with formats or samples that do not match the target does NOT fail when
    // created; it crashes when used.
    psoDesc.RTVFormats[0]         = kHdrFormat;
    psoDesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count      = sampleCount;
    psoDesc.SampleMask            = UINT_MAX;

    psoDesc.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    // No face culling: a world canvas can be viewed from behind, and with
    // billboard off that is normal when walking around it.
    psoDesc.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    D3D12_RENDER_TARGET_BLEND_DESC& blend = psoDesc.BlendState.RenderTarget[0];
    blend.BlendEnable           = TRUE;
    blend.SrcBlend              = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp               = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha         = D3D12_BLEND_ONE;
    blend.DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Depth WRITE is off in BOTH variants, on purpose:
    // the UI uses alpha, and writing depth would make the quads of the same
    // canvas clip each other depending on the order they leave the batcher.
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    psoDesc.DepthStencilState.StencilEnable  = FALSE;

    // The old ones are released BEFORE compiling: this function is also the
    // "recreate" of the sample count change, and applyPendingSampleCount() already waited
    // for the GPU to release the command lists that used them.
    uiWorldPipelineDepth.Reset();
    uiWorldPipelineNoDepth.Reset();

    psoDesc.DepthStencilState.DepthEnable = TRUE;
    throwIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&uiWorldPipelineDepth)),
                  "ID3D12Device::CreateGraphicsPipelineState(world UI with depth)");

    psoDesc.DepthStencilState.DepthEnable = FALSE;
    throwIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&uiWorldPipelineNoDepth)),
                  "ID3D12Device::CreateGraphicsPipelineState(world UI without depth)");
}

bool D3D12Renderer::Impl::createProbeResources(GpuProbe& probe)
{
    if (probe.srvBase == 0)
        return false;

    // A cubemap: six layers of a 2D texture. CUBE is stated by the VIEW, not the
    // resource (for the compute it is an array and for pbr.frag a TextureCube), and
    // that is why the same image serves for both things.
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

    // The capture is the destination of the six scene passes, so it is born as a
    // render target; the other two are written by the computes.
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

    // Read views: all three as TextureCube, which is what the convolution
    // compute (the capture) and pbr.frag (the other two) sample.
    createCubeSrv(probe.captureAllocation->GetResource(), kHdrFormat, 1,
                  probe.srvBase + kProbeCaptureSrv);
    createCubeSrv(probe.irradianceAllocation->GetResource(), kHdrFormat, 1,
                  probe.srvBase + kProbeIrradianceSrv);
    createCubeSrv(probe.prefilterAllocation->GetResource(), kHdrFormat, kIblPrefilterMips,
                  probe.srvBase + kProbePrefilterSrv);

    // And the write ones, as a 2D array: one UAV for the irradiance and one per
    // prefilter mip.
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
    // Fast path: with no scene or no probes anywhere nothing is touched.
    // It is the case of all scenes that do not use them.
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

    // Removals: probes whose GameObject is no longer there. Their images are released and their
    // heap slot is left free for the next one.
    for (size_t i = probes.size(); i-- > 0;) {
        const uint64_t owner = probes[i].ownerId;
        const bool     alive =
            std::any_of(descs.begin(), descs.end(), [owner](const Desc& d) { return d.id == owner; });
        if (alive)
            continue;

        // BEFORE releasing their images: return to the global environment EVERYTHING that
        // looked at it. Otherwise, descriptors would be left pointing at freed memory
        // (and a dangling SRV raises no error when created: it kills the device later, without
        // saying which one). All of them are rewritten because when deleting from the vector the
        // indices of the others shift, so the whole assignment
        // stops being valid; the refresh below rebuilds it.
        for (const StaticObject& object : objects)
            writeProbeSlots(object.srvBase, -1);
        for (const SkinnedObject& character : skinnedObjects)
            for (const SkinnedSubMesh& sub : character.subMeshes)
                writeProbeSlots(sub.srvBase, -1);
        probeAssignStatic.assign(objects.size(), -1);
        probeAssignSkinned.assign(skinnedObjects.size(), -1);
        // The probe enters the draw group key, and here it is reassigned by
        // hand without going through refreshProbeAssignment: without this the groups would
        // stay split by a probe that no longer exists. It does not look wrong
        // (all blocks go back to the global IBL, so each group is still
        // coherent), but they are extra draws forever.
        drawGroupsDirty = true;

        releaseProbe(probes[i]);
        probes.erase(probes.begin() + static_cast<long>(i));
    }

    // Additions and updates.
    for (const Desc& desc : descs) {
        auto it = std::find_if(probes.begin(), probes.end(),
                               [&desc](const GpuProbe& p) { return p.ownerId == desc.id; });
        if (it == probes.end()) {
            if (probes.size() >= kMaxProbes)
                continue;  // past the cap, those objects keep the global IBL

            // The first FREE block, not the one that falls out of the list size:
            // when deleting a probe from the middle the vector is compacted but the
            // others keep their block, so counting probes would give a slot
            // already taken and both would write over the same descriptors.
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

        // Moving the probe or changing its radius invalidates what was baked: what it
        // captured was another view.
        if (it->position != desc.position || it->radius != desc.radius ||
            it->intensity != desc.intensity) {
            it->position   = desc.position;
            it->radius     = desc.radius;
            it->intensity  = desc.intensity;
            it->baked      = false;
            it->bakeFailed = false;
        }
    }

    // Bake requests. They are handled HERE, at the start of the frame and before
    // recording anything, because baking rewrites the camera and the UBO and waits on the
    // GPU: in the middle of a frame it would be recording over what is already recorded.
    //
    // Requesting it by hand also clears the failure mark: it is the user's way
    // of saying "try again".
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

    // One per frame: six scene passes plus the convolution is too much
    // to do all at once with several probes, and this way the editor keeps
    // responding while they bake.
    for (GpuProbe& probe : probes) {
        if (probe.baked || probe.bakeFailed)
            continue;
        bakeProbe(probe);
        break;
    }

    // And who looks at whom. After the baking: a freshly baked probe can
    // already come in, and one that left is no longer in the list.
    refreshProbeAssignment();
}

int D3D12Renderer::Impl::pickProbeFor(const glm::vec3& worldPos) const
{
    // The nearest of those that reach it. With no probe reaching, -1: that
    // object keeps the global environment, which is what it always did.
    int   best         = -1;
    float bestDistance = 0.0f;
    for (size_t i = 0; i < probes.size(); ++i) {
        // A probe not yet baked has its cubemaps blank: using it
        // would turn off the object's reflection until it finishes.
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
    // t4 = irradiance, t5 = prefilter. Same slots that fillSharedSlots fills;
    // here only which image they point at is changed.
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

    // The descriptors about to be rewritten may be in use by the in-flight
    // frame. It waits ONCE, and only if there is really something to change.
    bool waited    = false;
    auto ensureIdle = [&]() {
        if (!waited) {
            waitForGpu();
            waited = true;
        }
    };

    for (size_t i = 0; i < objects.size(); ++i) {
        // The object's CENTER in world, not its origin: a long mesh with the
        // pivot outside the radius would be left without a probe for nothing.
        const StaticObject& object = objects[i];
        if (object.slotFree)
            continue;   // no mesh to assign to any probe
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
        // The probe enters the draw group key: two objects with the
        // same mesh and a different probe can no longer share a draw.
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
        // A character has one block per submesh and all of them look at the same
        // probe: the object is a single one.
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
    handle.ptr += static_cast<SIZE_T>(2) * dsvSize;  // 0 scene, 1 multisampled, 2 probes
    device->CreateDepthStencilView(probeDepthAllocation->GetResource(), &dsvDesc, handle);
}

void D3D12Renderer::Impl::bakeProbe(GpuProbe& probe)
{
    if (!probe.captureAllocation || !meshPipeline)
        return;

    createProbeDepth();
    if (!probeDepthAllocation)
        return;

    // This is not a pass of the frame: it is recorded in its own list and waited on. The
    // capture rewrites the scene UBO and the camera, which the in-flight frame
    // is using.
    waitForGpu();

    // Directions and "up" of the six faces. The ups are NEGATED relative to the
    // classic OpenGL list and the projection mirrors X as well as Y: two mirrors
    // make a rotation, so the face winding (and with it the back-face
    // culling) is preserved and the cubemap comes out with the orientation that
    // sampling expects. It is the same thing that was needed for the sky.
    static const glm::vec3 kDirs[6] = {
        {1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f},
    };
    static const glm::vec3 kUps[6] = {
        {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, -1.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f},
    };

    // What is touched and has to be given back: the frame's camera and the render
    // size, which the geometry pass uses for the viewport and the culling.
    const glm::mat4 savedView     = cameraView;
    const glm::vec3 savedPos      = cameraPos;
    const float     savedFov      = cameraFovDeg;
    const glm::mat4 savedViewProj = viewProj;

    // One RTV per face, in the six slots the heap reserves at the end.
    const UINT kProbeRtvBase = kRtvProbeFace;
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

    // One list PER FACE, submitted and waited on before recording the next. All
    // six share the scene UBO (a single constant buffer address) and
    // what the GPU reads is whatever is in that memory when it EXECUTES, not when
    // it was recorded: run back to back, the six faces came out with the last one's camera and
    // the cubemap was six copies of the same view. The same applies to any
    // per-frame buffer that gets rewritten between faces.
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

        // 90 degrees per face and a generous range: the probe sees the whole scene, not the
        // player's framing.
        cameraView       = glm::lookAtRH(probe.position, probe.position + kDirs[face], kUps[face]);
        cameraPos        = probe.position;
        cameraFovDeg = 90.0f;

        glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 0.1f, 20000.0f);
        proj[0][0] *= -1.0f;  // the X mirror that compensates for the negated "up"s
        proj[1][1] *= -1.0f;
        probeFaceProj = proj;
        viewProj      = proj * cameraView;

        // The UBO with THIS face's view: it is where pbr.frag takes the eye
        // position and the projection from. The lights and the cascades are left
        // as they are (the shadow map on the GPU is that of those
        // matrices, and recomputing them here would throw it off).
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

    // The return to read is recorded even if a face failed: leaving the
    // cubemap in RENDER_TARGET would throw off the next bake's barrier,
    // which expects it in PIXEL_SHADER_RESOURCE.
    bool convolved = false;
    if (inRenderTarget && beginList()) {
        transition(probe.captureAllocation->GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // And the convolution: the same two computes of the global IBL, but reading
        // this probe's capture and writing into its cubemaps.
        if (facesOk)
            recordIblConvolution(probe.srvBase + kProbeCaptureSrv,
                                 probe.srvBase + kProbeIrradianceUav,
                                 probe.srvBase + kProbePrefilterUav, probe.intensity);

        convolved = submitAndWait() && facesOk;
    }

    // Wall-clock time, not GPU time: here it waits for it to finish, so the
    // wait IS the cost, and that is what the person baking wants to know.
    probe.bakeMs = std::chrono::duration<float, std::milli>(
                       std::chrono::high_resolution_clock::now() - bakeStart)
                       .count();
    probeLastBakeMs = probe.bakeMs;

    // And everything as it was: the next frame draws from the player's camera.
    cameraView       = savedView;
    cameraPos        = savedPos;
    cameraFovDeg = savedFov;
    viewProj         = savedViewProj;
    probeFaceProj.reset();
    updateSceneUbo();

    // Without the six faces and their convolution, the probe is NOT left baked: marking it
    // anyway would leave it in the candidate list with its cubemaps half done, and
    // the objects that fell to it would reflect that.
    probe.baked      = convolved;
    probe.bakeFailed = !convolved;

}

void D3D12Renderer::Impl::releaseProbe(GpuProbe& probe)
{
    // Its images may be in the in-flight frame: removing a probe is a rare
    // event (deleting the GameObject), so waiting is cheaper than
    // keeping a deferred delete list.
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

    // The format is decided by the CONTENT: a sprite atlas is color and goes in
    // sRGB; a font's is distances and in sRGB it would come out distorted without
    // validation saying a word.
    const DXGI_FORMAT format = atlas.sourceIsSrgb() ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                    : DXGI_FORMAT_R8G8B8A8_UNORM;

    const UINT slot = kSrvUiAtlas + uiNextAtlasSlot;
    D3D12MA::Allocation* texture =
        uploadTexture(atlas.sourcePixels().data(), atlas.width(), atlas.height(), 1, format, 4,
                      slot);
    if (!texture)
        return false;

    // Name so that the D3D12MA JSON (and ReportLiveObjects) can tell it apart.
    texture->SetName(L"UiAtlas");
    diagLog("registerUiAtlas: atlas " + std::to_string(atlas.width()) + "x" +
            std::to_string(atlas.height()) + " in slot " + std::to_string(slot) +
            (atlas.sourceIsSrgb() ? " (sRGB, sprites)" : " (linear, font)"));
    uiAtlasTextures.push_back(texture);
    uiAtlasSrv[&atlas] = slot;
    ++uiNextAtlasSlot;
    return true;
}

namespace
{
    // ImGui's RTV in D3D12 is R8G8B8A8_UNORM (EditorUI::initUiD3D12 /
    // uiInfo.d3dRtvFormat): sampling an sRGB SRV into a UNORM RTV would make the image
    // come out darker. The atlas is UNORM, so the image's sRGB bytes
    // reach the screen as they are. If the manual check says otherwise, this is the
    // only constant to change.
    constexpr DXGI_FORMAT kThumbAtlasFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
}

bool D3D12Renderer::Impl::ensureThumbAtlas()
{
    if (thumbAtlas) return true;
    if (thumbAtlasFailed || !initialized || !srvHeap) return false;

    try
    {
        // Transparent. uploadTexture leaves it in PIXEL_SHADER_RESOURCE and creates its SRV.
        const std::vector<uint8_t> blank(static_cast<size_t>(kThumbAtlasSize) * kThumbAtlasSize * 4, 0);
        thumbAtlas = uploadTexture(blank.data(), kThumbAtlasSize, kThumbAtlasSize, 1,
                                   kThumbAtlasFormat, 4, kSrvThumbAtlas);
    }
    catch (const std::exception&)
    {
        thumbAtlas = nullptr;
    }
    if (!thumbAtlas)
    {
        thumbAtlasFailed = true;
        return false;
    }
    thumbAtlas->SetName(L"ThumbnailAtlas");
    diagLog("ensureThumbAtlas: thumbnail atlas " + std::to_string(kThumbAtlasSize) + "x" +
            std::to_string(kThumbAtlasSize) + " in slot " + std::to_string(kSrvThumbAtlas));
    return true;
}

bool D3D12Renderer::Impl::uploadThumbnailTiles(const ThumbnailTile* tiles, size_t count)
{
    if (!thumbAtlas || !tiles || count == 0) return false;

    // A cell is 64 rows of 256 bytes: the row already meets
    // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT and the whole cell (16 KiB) meets
    // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, so the N cells go
    // contiguous in the staging buffer with no padding.
    constexpr UINT kRowBytes  = kThumbCell * 4;
    constexpr UINT kTileBytes = kRowBytes * kThumbCell;
    static_assert(kRowBytes % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0);
    static_assert(kTileBytes % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0);

    for (size_t i = 0; i < count; ++i)
        if (tiles[i].slot >= kThumbSlotCount || !tiles[i].rgba)
            return false;

    try
    {
        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width            = static_cast<UINT64>(kTileBytes) * count;
        bufferDesc.Height           = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels        = 1;
        bufferDesc.Format           = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12MA::ALLOCATION_DESC uploadDesc{};
        uploadDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

        D3D12MA::Allocation* staging = nullptr;
        throwIfFailed(allocator->CreateResource(&uploadDesc, &bufferDesc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                &staging, IID_NULL, nullptr),
                      "D3D12MA::Allocator::CreateResource(thumbnail staging)");

        uint8_t*          mapped = nullptr;
        const D3D12_RANGE noRead{0, 0};
        HRESULT hr = staging->GetResource()->Map(0, &noRead, reinterpret_cast<void**>(&mapped));
        if (FAILED(hr)) {
            staging->Release();
            throwIfFailed(hr, "ID3D12Resource::Map(thumbnail staging)");
        }
        for (size_t i = 0; i < count; ++i)
            std::memcpy(mapped + i * kTileBytes, tiles[i].rgba, kTileBytes);
        staging->GetResource()->Unmap(0, nullptr);

        // Same path as uploadTexture: one list, one wait. The N cells
        // share the two barriers and the wait.
        throwIfFailed(allocators[frameIndex]->Reset(), "ID3D12CommandAllocator::Reset(thumbnails)");
        throwIfFailed(commandList->Reset(allocators[frameIndex].Get(), nullptr),
                      "ID3D12GraphicsCommandList::Reset(thumbnails)");

        D3D12_RESOURCE_BARRIER toCopy{};
        toCopy.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy.Transition.pResource   = thumbAtlas->GetResource();
        toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toCopy.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &toCopy);

        for (size_t i = 0; i < count; ++i)
        {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource        = thumbAtlas->GetResource();
            dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource                          = staging->GetResource();
            src.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset             = static_cast<UINT64>(i) * kTileBytes;
            src.PlacedFootprint.Footprint.Format   = kThumbAtlasFormat;
            src.PlacedFootprint.Footprint.Width    = kThumbCell;
            src.PlacedFootprint.Footprint.Height   = kThumbCell;
            src.PlacedFootprint.Footprint.Depth    = 1;
            src.PlacedFootprint.Footprint.RowPitch = kRowBytes;

            const UINT x = (tiles[i].slot % kThumbAtlasCells) * kThumbCell;
            const UINT y = (tiles[i].slot / kThumbAtlasCells) * kThumbCell;
            commandList->CopyTextureRegion(&dst, x, y, 0, &src, nullptr);
        }

        D3D12_RESOURCE_BARRIER toShader = toCopy;
        toShader.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        toShader.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        commandList->ResourceBarrier(1, &toShader);

        throwIfFailed(commandList->Close(), "ID3D12GraphicsCommandList::Close(thumbnails)");
        ID3D12CommandList* lists[] = {commandList.Get()};
        queue->ExecuteCommandLists(1, lists);
        waitForGpu();
        staging->Release();
    }
    catch (const std::exception&)
    {
        return false;
    }
    return true;
}

void D3D12Renderer::Impl::ensureUiBuffers(UINT vertexCount, UINT indexCount)
{
    auto grow = [&](D3D12MA::Allocation*& allocation, void*& mapped, UINT& capacity, UINT needed,
                    UINT stride) {
        if (needed <= capacity && allocation)
            return;

        // Doubling: reallocating every frame for one extra vertex would be a
        // create/destroy per frame.
        UINT next = capacity ? capacity : 256;
        while (next < needed)
            next *= 2;

        if (allocation) {
            // It may be in use by a previous frame: growing is rare (only
            // when the UI gets more complex), so waiting is cheaper than
            // keeping a deferred delete list.
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
            // Freshly created and unused: it can be released without waiting for anyone.
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

void D3D12Renderer::Impl::beginUiFrame()
{
    // The draw data of ALL the frame's canvases, and the sizing of the buffer pair,
    // in ONE place and BEFORE the scene pass. It is not inside
    // recordUiCanvas (which is where it used to live) because the WORLD canvases are recorded
    // in the SCENE pass, which runs earlier: if the buffer were sized there,
    // the world ones would arrive with the PREVIOUS frame's capacity (or 0) and the
    // guard in bindUiCanvasGeometry would discard them SILENTLY. And worse: if
    // ensureUiBuffers had to GROW mid-frame, it would release the resource
    // on which the world ones have already recorded their view and the GPU would read a dead
    // address on execute.
    uiVertexCursor   = 0;
    uiIndexCursor    = 0;
    uiScreenVertices = 0;
    uiScreenIndices  = 0;
    if (!uiPipeline)
        return;

    for (auto& s : uiSlots)
    {
        if (!s) continue;
        if (s->mode == UiCanvasRenderMode::World)
        {
            // The MODEL matrix, here and not in syncUiCanvases: it needs the
            // camera view for the billboard, and syncUiCanvases does not
            // have it. It also falls BEFORE sortWorldCanvasesBackToFront, which
            // sorts by reading the position in model[3]; computing it only at
            // recording time would leave it at zero for the sort.
            //
            // In World mode the canvas is NOT fitted to any screen: its
            // space is its own referenceResolution, which is what
            // CanvasComponent::applyTo sets.
            const glm::vec2 tam = s->canvas.referenceResolution;
            s->model = uiWorldCanvasMatrix(s->component, tam, s->worldTransform, cameraView);
            s->canvas.buildDrawData(static_cast<uint32_t>(tam.x), static_cast<uint32_t>(tam.y),
                                    s->drawData);
        }
        else
        {
            // The screen ones, at OUTPUT size: the UI is measured in screen pixels,
            // not render pixels, which with SSAA are different.
            s->canvas.buildDrawData(outWidth, outHeight, s->drawData);
        }
    }

    // The count is done by the free function in UiWidgetSync.h, which is the one tested
    // without a GPU (test_ui_frame_totals_suma_mundo_y_pantalla).
    const UiFrameTotals totales = uiFrameTotals(uiSlots);
    uiScreenVertices = totales.screenVertices;
    uiScreenIndices  = totales.screenIndices;
    if (totales.vertices == 0 || totales.indices == 0)
        return;

    ensureUiBuffers(totales.vertices, totales.indices);
}

bool D3D12Renderer::Impl::bindUiCanvasGeometry(const UiDrawData& data)
{
    if (!uiVertexMapped[frameIndex] || !uiIndexMapped[frameIndex])
        return false;

    // Suballocation INSIDE the same buffer: each canvas writes starting where
    // the previous one left off (bumpUiCursor) and binds ITS own view, with the
    // offset already put into BufferLocation. Without this (or with views always starting
    // at byte 0, which is what it did with a single canvas) the second
    // canvas would overwrite the vertices of the first, and since the GPU reads the buffer at
    // EXECUTE of the command list (not at record time), the N draws would all come out with
    // the geometry of the LAST one.
    const UINT vertexCount = static_cast<UINT>(data.vertices.size());
    const UINT indexCount  = static_cast<UINT>(data.indices.size());
    const UINT vertexBase  = bumpUiCursor(uiVertexCursor, vertexCount);
    const UINT indexBase   = bumpUiCursor(uiIndexCursor,  indexCount);

    // The cursor is sized by beginUiFrame() with the total of ALL the frame's
    // canvases. If that total did not match what is really written here,
    // this would write OUTSIDE the mapped memory without any validation layer
    // seeing it. Better not to draw that canvas than to corrupt the buffer.
    if (!uiCursorFits(vertexBase, vertexCount, uiVertexCapacity[frameIndex]) ||
        !uiCursorFits(indexBase,  indexCount,  uiIndexCapacity[frameIndex]))
        return false;

    std::memcpy(static_cast<UiVertex*>(uiVertexMapped[frameIndex]) + vertexBase,
                data.vertices.data(), data.vertices.size() * sizeof(UiVertex));
    std::memcpy(static_cast<uint16_t*>(uiIndexMapped[frameIndex]) + indexBase,
                data.indices.data(), data.indices.size() * sizeof(uint16_t));

    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = uiVertexAllocations[frameIndex]->GetResource()->GetGPUVirtualAddress()
                       + static_cast<UINT64>(vertexBase) * sizeof(UiVertex);
    vbv.SizeInBytes    = static_cast<UINT>(data.vertices.size() * sizeof(UiVertex));
    vbv.StrideInBytes  = sizeof(UiVertex);

    D3D12_INDEX_BUFFER_VIEW ibv{};
    ibv.BufferLocation = uiIndexAllocations[frameIndex]->GetResource()->GetGPUVirtualAddress()
                       + static_cast<UINT64>(indexBase) * sizeof(uint16_t);
    ibv.SizeInBytes    = static_cast<UINT>(data.indices.size() * sizeof(uint16_t));
    ibv.Format         = DXGI_FORMAT_R16_UINT;

    commandList->IASetVertexBuffers(0, 1, &vbv);
    commandList->IASetIndexBuffer(&ibv);
    return true;
}

void D3D12Renderer::Impl::recordWorldCanvases(UINT targetWidth, UINT targetHeight)
{
    if (!uiWorldPipelineDepth || !uiWorldPipelineNoDepth)
        return;

    // The order is set by the free function in UiWidgetSync.h, which is the one tested
    // without a GPU (test_world_canvases_se_ordenan_de_lejos_a_cerca). The draw
    // data and the model matrix are already done, in beginUiFrame().
    sortWorldCanvasesBackToFront(uiSlots, cameraView, uiWorldOrder);
    if (uiWorldOrder.empty())
        return;

    const D3D12_GPU_DESCRIPTOR_HANDLE heapStart = srvHeap->GetGPUDescriptorHandleForHeapStart();
    auto gpuHandle = [&](UINT index) {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = heapStart;
        handle.ptr += static_cast<UINT64>(index) * srvSize;
        return handle;
    };

    ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootSignature(uiRootSignature.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // The render target and viewport are already those of the scene pass: this function
    // is called with the geometry and sky just recorded and nothing in between
    // that changes them, so the depth that is written is the one that has to
    // occlude. What IS restored is the scissor, because the loop below does NOT
    // touch it (see the limitation) and the composition pass leaves it wherever it likes.
    D3D12_RECT scissor{0, 0, static_cast<LONG>(targetWidth), static_cast<LONG>(targetHeight)};
    commandList->RSSetScissorRects(1, &scissor);

    // KNOWN LIMITATION, same as in Vulkan: `clipChildren` does not clip in a
    // world canvas. The batcher's scissor is in canvas pixels and a
    // D3D12_RECT only understands target pixels; on screen the mapping is a
    // scale, but a world canvas is PROJECTED (it can come out rotated, in
    // perspective or split by the edge) and there is no axis-aligned
    // rectangle that represents it. Clipping with the unprojected rect would cover pieces
    // that are visible.
    for (UiCanvasSlot* s : uiWorldOrder)
    {
        const UiDrawData& data = s->drawData;
        if (data.empty() || data.vertices.empty() || data.indices.empty()) continue;
        if (!bindUiCanvasGeometry(data)) continue;

        // depthTest chooses the pipeline: true = a wall hides it; false = always
        // on top. Depth WRITE is off in both.
        commandList->SetPipelineState(s->depthTest ? uiWorldPipelineDepth.Get()
                                                   : uiWorldPipelineNoDepth.Get());

        // The matrix goes as ROOT CONSTANTS, not in a shared buffer: it stays
        // recorded in the command list, so each canvas keeps ITS OWN
        // even if the GPU does not execute until much later. With one CBV per frame
        // rewritten between draws, the N canvases would all come out with the LAST one's matrix,
        // which is exactly what happened with the six faces of a
        // probe, and the symptom was ABSENT geometry, not equal matrices.
        // The projection is the JITTERED one, the same updateSceneUbo gives the
        // geometry: with TAA, a world canvas without jitter would leave a permanent
        // edge against everything that does carry it. Outside TAA,
        // taaJitteredProj is cameraProj() as is and this is viewProj.
        const glm::mat4 mvp = taaJitteredProj * cameraView * s->model;
        commandList->SetGraphicsRoot32BitConstants(0, 16, &mvp[0][0], 0);
        // Dword 16 is linearOutput, and here it goes to 1: the destination is the SCENE
        // target, which is kHdrFormat (R16G16B16A16_FLOAT), that is, LINEAR HDR, and
        // whatever is written there later goes through bloom_composite (ACES +
        // pow(1/2.2)). Without undoing the gamma here, a 0.5 would end up at ~0.80 on
        // screen: the sign comes out WASHED OUT and no layer says so.
        const UINT linearOutput = 1;
        commandList->SetGraphicsRoot32BitConstants(0, 1, &linearOutput, 16);

        for (const UiBatch& batch : data.batches) {
            // `scissor.empty()` ALSO discards, just like in Vulkan
            // (UiSpriteBatch.cpp): a node with clipChildren whose clip rect
            // ended up with width or height 0 (a ScrollView with the content
            // scrolled completely out) emits NOTHING. Here the per-batch scissor cannot be applied
            // (the canvas is projected and there is no axis-aligned rect
            // that represents it, see the comment above),
            // but an already empty clip is respected: without this half, that
            // ScrollView was drawn WHOLE and unclipped. Careful: it is NOT the same
            // case as the SCREEN path (recordUiCanvas), where
            // scissor.empty() means "no clip of its own" and falls back to the whole
            // viewport; that one predates this and stays as it is there.
            if (batch.indexCount == 0 || batch.scissor.empty())
                continue;

            UINT srv = kSrvBaseColor;
            if (batch.atlas) {
                const auto it = uiAtlasSrv.find(batch.atlas);
                if (it != uiAtlasSrv.end())
                    srv = it->second;
            }
            commandList->SetGraphicsRootDescriptorTable(1, gpuHandle(srv));
            // firstIndex is LOCAL to this canvas: the offset was already applied by the index
            // view of bindUiCanvasGeometry.
            commandList->DrawIndexedInstanced(batch.indexCount, 1, batch.firstIndex, 0, 0);
        }
    }
}

void D3D12Renderer::Impl::recordUiCanvas(D3D12_CPU_DESCRIPTOR_HANDLE targetRtv, const glm::mat4& transform)
{
    if (!uiPipeline)
        return;

    // The draw data and the buffer sizing are already done: beginUiFrame() did them,
    // before the scene pass. Here only the SCREEN canvases are recorded,
    // which are the only ones in this pass; the WORLD ones were already recorded
    // inside the scene one, and their vertices are still in the same unread buffer.
    if (uiScreenVertices == 0 || uiScreenIndices == 0)
        return;
    if (!uiVertexMapped[frameIndex] || !uiIndexMapped[frameIndex])
        return;

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
    commandList->SetGraphicsRoot32BitConstants(0, 16, &transform[0][0], 0);
    // Dword 16 is linearOutput. SCREEN canvases are drawn onto the
    // back buffer, which is already tonemapped and with gamma applied by
    // bloom_composite, so it goes to 0: the number written here is ALREADY the
    // one seen. The WORLD ones go to 1 (see recordWorldCanvases). If it were not
    // pushed, the pixel shader would read an undefined value and the color would come out
    // wrong as soon as it was not 0.
    const UINT linearOutput = 0;
    commandList->SetGraphicsRoot32BitConstants(0, 1, &linearOutput, 16);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (auto& s : uiSlots)
    {
        if (!s || s->mode != UiCanvasRenderMode::ScreenSpace) continue;
        const UiDrawData& data = s->drawData;
        if (data.empty() || data.vertices.empty() || data.indices.empty()) continue;

        // The cursors are MEMBERS and continue where the world canvases left them
        // in the scene pass: their vertices are in this same buffer and
        // the GPU has not read them yet (it reads at EXECUTE, not at record time).
        if (!bindUiCanvasGeometry(data)) continue;

        for (const UiBatch& batch : data.batches) {
            if (batch.indexCount == 0)
                continue;

            // The node's clip, in screen pixels. A batch without its own scissor
            // is clipped to the whole viewport.
            D3D12_RECT scissor{0, 0, static_cast<LONG>(outWidth), static_cast<LONG>(outHeight)};
            if (!batch.scissor.empty()) {
                scissor.left   = batch.scissor.x;
                scissor.top    = batch.scissor.y;
                scissor.right  = batch.scissor.x + static_cast<LONG>(batch.scissor.width);
                scissor.bottom = batch.scissor.y + static_cast<LONG>(batch.scissor.height);
            }
            commandList->RSSetScissorRects(1, &scissor);

            // Without an atlas, the white 1x1: multiplying by (1,1,1,1) leaves the vertex
            // color as is, so a flat panel does not even need a separate
            // pipeline. With an atlas, its own; and if it never got uploaded, the
            // white again (the flat color will show instead of the sprite, but not
            // another one's descriptor).
            UINT srv = kSrvBaseColor;
            if (batch.atlas) {
                const auto it = uiAtlasSrv.find(batch.atlas);
                if (it != uiAtlasSrv.end())
                    srv = it->second;
            }
            commandList->SetGraphicsRootDescriptorTable(1, gpuHandle(srv));
            // firstIndex is LOCAL to this canvas: the offset was already applied by the
            // index view above.
            commandList->DrawIndexedInstanced(batch.indexCount, 1, batch.firstIndex, 0, 0);
        }
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

    // The scene goes to unordered access and the depth to read.
    transition(hdrAllocation->GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // readableDepth() and not depthAllocation: with MSAA the readable depth is
    // the pre-pass one, which is ANOTHER resource, and the transition back further
    // down already used readableDepth(). Asymmetric ones left the scene pass one
    // in read forever and touched the pre-pass one from a state that
    // was not its own.
    transition(readableDepth(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const glm::mat4  proj   = cameraProj();
    const glm::vec3  camPos = cameraPos;
    const glm::mat4& view   = cameraView;

    FogPush push{};
    // fog.comp reconstructs the world position with clip = vec4(uv*2-1, d, 1),
    // that is, it takes for granted that the TOP ROW (uv.y = 0) falls at ndc.y = -1.
    // That is the Vulkan convention, whose projection carries the Y inversion.
    //
    // The GRAPHICS passes of this backend compensate for that with the negative-height
    // viewport, but this is a COMPUTE: there is no viewport to put it in, so
    // the inversion has to go in the matrix. Without it the reconstruction comes out
    // mirrored vertically and the fog looks upside down.
    //
    // The comment that was here said exactly the opposite ("in D3D12 there is
    // none to put in") and is what hid the bug.
    glm::mat4 projFog = proj;
    projFog[1][1]    *= -1.0f;
    push.invViewProj  = glm::inverse(projFog * view);
    push.camPosDensity    = glm::vec4(camPos, state->fogDensity());
    push.lightDirFalloff  = glm::vec4(glm::normalize(lightDirection), state->fogHeightFalloff());
    // The scattering goes ALREADY multiplied by the light's color and intensity.
    // The key light's color is folded in here over the fog tint, just like
    // in the Vulkan path: the shader multiplies only once.
    //
    // Before, this was a HARDCODED WARM tint (1.0, 0.98, 0.94) at 70%, which
    // ignored the scene's light: choosing pure white gave gray, and raising the
    // light intensity did not brighten the fog.
    glm::vec3 lightColor(0.0f);
    if (!sceneLights.empty()) {
        lightColor = glm::vec3(sceneLights[0].color[0], sceneLights[0].color[1],
                               sceneLights[0].color[2]) *
                     sceneLights[0].color[3];
    }
    push.scatterBaseHeight = glm::vec4(state->fogScatter() * lightColor,
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

    // And they are returned to what the rest of the frame expects.
    transition(readableDepth(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               readableDepthState());
    transition(hdrAllocation->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
}

void D3D12Renderer::Impl::recordBloomAndComposite(D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv)
{
    // The heap is left set by the scene pass, but this pass cannot
    // depend on that one having been recorded.
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

    // The scene stops being a draw target and becomes read: first by the
    // bloom compute, then by the composition pass.
    transition(hdrAllocation->GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

    // Off means OFF: not a dispatch and not a barrier of the chain,
    // just like in Vulkan (Renderer.cpp, `if (bloomEnabled())`). The composition
    // below takes care of reading black instead of mips that stay
    // as they were. Until now this flag was not queried anywhere in the
    // backend, so the editor's switch turned nothing off.
    // bloomMipCount == 0 is a viewport so small it does not fit even one level.
    // It goes down the same path as bloom off: the composition reads black instead
    // of a mip nobody has written. Vulkan does the same (BloomPass
    // returns with m_mipCount == 0 and its composition checks it).
    const bool bloomOn = state->bloomEnabled() && bloomMipCount > 0;

    if (bloomOn) {
        commandList->SetComputeRootSignature(bloomRootSignature.Get());

        // Reduction. The first level reads the scene and applies the threshold; the rest
        // only filter the previous level.
        for (int level = 0; level < static_cast<int>(bloomMipCount); ++level) {
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

            // This level becomes the source of the next step.
            transition(bloomMipAllocations[level]->GetResource(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        // Upsampling: each level ADDS the one below onto what it already had, so the
        // destination goes back to unordered access to be both read and written.
        for (int level = static_cast<int>(bloomMipCount) - 2; level >= 0; --level) {
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

            // And it goes back to source for the next level (or for the composition, if
            // this was the last one).
            transition(bloomMipAllocations[level]->GetResource(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       level == 0 ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                  : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }

    // Composition to the backbuffer.
    //
    // NEGATIVE-HEIGHT viewport, and it is not a gratuitous trick: fullscreen.vert
    // derives the uv from the same coordinates as the position (uv = ndc*0.5+0.5)
    // taking for granted that NDC y=-1 is the TOP row, which is how
    // Vulkan works. In D3D12 y=-1 is the bottom one, so the same shader leaves the
    // image upside down. Inverting the viewport fixes it without touching a shader that
    // both backends share.
    D3D12_VIEWPORT viewport{};
    viewport.TopLeftY = static_cast<float>(height);
    viewport.Width    = static_cast<float>(width);
    viewport.Height   = -static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};

    // The composition does NOT go to the backbuffer: it goes to the LDR target, which is what
    // FXAA reads afterwards. A pass cannot read and write the same image.
    D3D12_CPU_DESCRIPTOR_HANDLE ldrRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    ldrRtv.ptr += static_cast<SIZE_T>(kRtvLdr) * rtvSize;

    // It is created already in RENDER_TARGET (see createHdrTargets) and stays in that state
    // until the transition after compositing, so here it can be
    // first-used as is.
    estrenarRenderTarget(ldrAllocation->GetResource(), ldrInicializado);

    commandList->OMSetRenderTargets(1, &ldrRtv, FALSE, nullptr);
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->SetPipelineState(compositePipeline.Get());
    commandList->SetGraphicsRootSignature(compositeRootSignature.Get());
    const float bloomIntensity = state->bloomIntensity();
    commandList->SetGraphicsRoot32BitConstants(0, 1, &bloomIntensity, 0);
    // With bloom on, the usual pair: scene and mip 0 of the
    // chain. Off, the one at the end of the heap: the same scene and a black, so
    // that the shader's `+= bloom * intensity` adds exactly zero without reading
    // mips that nobody has written this frame.
    commandList->SetGraphicsRootDescriptorTable(
        1, gpuHandle(bloomOn ? kSrvSceneHdr : kSrvCompositeOff));
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // No vertex buffer: fullscreen.vert derives the three positions from the index.
    commandList->DrawInstanced(3, 1, 0, 0);

    // The outline of the selection, here and not in the scene pass: over the
    // already tonemapped image its orange arrives flat, which is what tells it apart
    // from the yellow of the colliders and the cyan of the frustum. As a bonus it stops
    // leaking into a probe's capture, which is scene geometry and not
    // editor decoration.
    recordSelectionOutline(ldrRtv);

    // FXAA over the already composed result, and from there to the backbuffer.
    transition(ldrAllocation->GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // The final pass writes to the destination, which can be SMALLER than what was
    // just drawn: with SSAA the output viewport is the panel's and the
    // shader averages. Without SSAA both sizes match and this is as before.
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
        // Downsample by averaging: one sample per source texel and per axis, which is
        // what defines supersampling. At factor 2 it is the four texels that
        // fall inside the destination pixel.
        SsaaPush ssaaPush{};
        ssaaPush.invSrc[0] = 1.0f / static_cast<float>(width);
        ssaaPush.invSrc[1] = 1.0f / static_cast<float>(height);
        ssaaPush.taps      = (std::max)(1, static_cast<int>(std::lround(state->ssaaFactor())));

        commandList->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);
        commandList->SetPipelineState(ssaaPipeline.Get());
        commandList->SetGraphicsRootSignature(ssaaRootSignature.Get());
        commandList->SetGraphicsRoot32BitConstants(0, sizeof(SsaaPush) / 4, &ssaaPush, 0);
        commandList->SetGraphicsRootDescriptorTable(1, gpuHandle(kSrvLdr));
        commandList->DrawInstanced(3, 1, 0, 0);

        taaHistoryValid = false;
    } else if (state->aaMode() == RendererState::AaMode::Taa) {
        // TAA takes FXAA's place: it blends this image with the previous frame's
        // and writes both the backbuffer and the next history at once.
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

        // Without temporal accumulation the history stops being valid: when going back to TAA
        // one has to start from zero or the first frame blends an old image.
        taaHistoryValid = false;
    }
    markTimestamp(TsAa + 1);

    // Game UI, on top of the already composed scene and below the editor's
    // (which is recorded afterwards, over the backbuffer). With an empty canvas it
    // records not a single command.
    //
    // Orthographic in pixels with the origin at the TOP left, which is how
    // positions come. Nothing else is flipped: the output viewport already has
    // negative height in the rest of the passes, so here it is set straight.
    const glm::mat4 uiProj = glm::orthoRH_ZO(0.0f, static_cast<float>(outWidth),
                                             static_cast<float>(outHeight), 0.0f, -1.0f, 1.0f);
    recordUiCanvas(backBufferRtv, uiProj);

    transition(ldrAllocation->GetResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);

    // The interface goes on top of everything, over the backbuffer and without post-processing:
    // smoothing the UI text edges would blur it. With the scene in a
    // texture it is drawn by the caller, AFTER that texture goes to read:
    // here it is still the pass's destination.
    if (uiDrawCallback && !renderToTexture) {
        commandList->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);
        uiDrawCallback();
    }

    // Everything goes back to the state the next frame starts with.
    transition(hdrAllocation->GetResource(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    // Only if the chain actually ran: with bloom off the mips have not
    // moved from UNORDERED_ACCESS, and "returning" them from a state they
    // are not in is a barrier error, not a correction.
    if (bloomOn) {
        transition(bloomMipAllocations[0]->GetResource(),
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (int level = 1; level < kBloomMips; ++level) {
            transition(bloomMipAllocations[level]->GetResource(),
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
}

void D3D12Renderer::Impl::createShadowResources()
{
    // Ground: the engine's own mesh, the same one the editor uses.
    // A hair below y=0, which is where the grid lives: in the same
    // plane they fight over depth and the lines come out dotted.
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

    // shadow.vert ALWAYS takes the model from the instance buffer, even for
    // objects that in the main pass use the push constant. That is why each
    // object needs its own, with the SAME transform used when
    // drawing it: if they differ, the shadow falls where the object is not.
    const glm::mat4 groundModel = glm::mat4(1.0f);
    groundInstanceAllocation = uploadBuffer(&groundModel, sizeof(groundModel),
                                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Shadow map: a depth array, one layer per cascade.
    D3D12_RESOURCE_DESC shadowDesc{};
    shadowDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    shadowDesc.Width            = shadowMapSize;
    shadowDesc.Height           = shadowMapSize;
    shadowDesc.DepthOrArraySize = kShadowLayers;
    shadowDesc.MipLevels        = 1;
    // TYPELESS because the same resource is seen in two ways: as depth
    // when recording it (D32_FLOAT) and as a texture when sampling it (R32_FLOAT).
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
    shadowDsvHeapDesc.NumDescriptors = kShadowLayers;
    shadowDsvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    throwIfFailed(device->CreateDescriptorHeap(&shadowDsvHeapDesc, IID_PPV_ARGS(&shadowDsvHeap)),
                  "ID3D12Device::CreateDescriptorHeap(shadow DSV)");
    dsvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    // One DSV per layer: each cascade is recorded separately.
    for (int cascade = 0; cascade < kShadowLayers; ++cascade) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format                         = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.FirstArraySlice = cascade;
        dsvDesc.Texture2DArray.ArraySize       = 1;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(cascade) * dsvSize;
        device->CreateDepthStencilView(shadowMapArrayAllocation->GetResource(), &dsvDesc, handle);
    }

    // The SRV goes in the t3 slot, over the 1x1 filler array that occupied that
    // place: from here on the shader samples real shadows.
    createShadowMapSrv(kSrvShadowMap);

    // And the same view in the triplet of each already loaded object: if this were
    // redone with a scene on screen, their t3 would point at the old resource.
    for (const StaticObject& object : objects)
        if (object.srvBase != kSrvBaseColor)
            createShadowMapSrv(object.srvBase + 2);
    for (const SkinnedObject& character : skinnedObjects)
        for (const SkinnedSubMesh& sub : character.subMeshes)
            if (sub.srvBase != kSrvBaseColor)
                createShadowMapSrv(sub.srvBase + 2);

    // Root signature of the shadow pass: the SAME UBO in b0 (the offsets of
    // view/proj/lightSpaceMatrix match those of triangle), the cascade
    // index as a root constant and the instance buffer.
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
        throw std::runtime_error("D3D12: shadow root signature (HRESULT " +
                                 hresultToString(hr) + ") " + detail);
    }
    throwIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                              serialized->GetBufferSize(),
                                              IID_PPV_ARGS(&shadowRootSignature)),
                  "ID3D12Device::CreateRootSignature(shadows)");

    const std::vector<char> shadowVs = readBinaryFile("shaders/shadow.vert.dxil");

    // Two PSOs because there are two vertex formats: the engine's and the one written by
    // the skinning compute. shadow.vert only reads the position, so one
    // element is enough, but the stride has to be the right one.
    auto buildShadowPipeline = [&](UINT stride, ComPtr<ID3D12PipelineState>& out) {
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature        = shadowRootSignature.Get();
        psoDesc.VS                    = {shadowVs.data(), shadowVs.size()};
        // No pixel shader: the pass only writes depth.
        psoDesc.InputLayout           = {layout, _countof(layout)};
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets      = 0;
        psoDesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
        psoDesc.SampleDesc.Count      = 1;
        psoDesc.SampleMask            = UINT_MAX;

        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        // BACK faces are recorded, not the front ones. It is what prevents
        // a surface from shading itself: the face that gets lit does not enter
        // the map, so its depth cannot end up in front of
        // itself. With front faces or no culling, a large plane (a project's
        // ground) comes out entirely in shadow no matter how much bias is put on it.
        // In exchange, an open single-sided mesh casts no shadow.
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
        psoDesc.RasterizerState.DepthClipEnable       = TRUE;
        psoDesc.RasterizerState.FrontCounterClockwise = TRUE;
        // Depth bias against shadow acne: without it, the surface
        // shades itself in bands.
        psoDesc.RasterizerState.DepthBias            = 2000;
        psoDesc.RasterizerState.SlopeScaledDepthBias = 2.0f;

        psoDesc.DepthStencilState.DepthEnable    = TRUE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;

        (void)stride;  // the stride goes in the vertex buffer view, not in the PSO
        throwIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&out)),
                      "ID3D12Device::CreateGraphicsPipelineState(shadows)");
    };

    buildShadowPipeline(sizeof(Vertex), shadowPipeline);
    buildShadowPipeline(kSkinnedOutputStride, shadowSkinnedPipeline);
}

void D3D12Renderer::Impl::computeCascades()
{
    for (auto& matrix : cascadeMatrices)
        matrix = glm::mat4(1.0f);
    cascadeSplits = glm::vec4(0.0f);
    activeLayers  = 0;
    extraLayers   = 0;
    for (int& r : shadowSlot) r = -1;

    // The secondary spots FIRST, because the key light branches further down
    // exit with return. Same helper as Vulkan: the split HAS to be
    // identical or the same scene would have shadows on different lights depending on the
    // backend.
    if (!sceneLights.empty()) {
        const int n = (std::min)(static_cast<int>(sceneLights.size()), MAX_LIGHTS);
        extraLayers = repartirSombrasExtra(
            sceneLights.data(), n,
            [](const ShaderLight& l) { return static_cast<int>(l.direction[3] + 0.5f); },
            [](const ShaderLight& l) {
                return glm::vec4(l.params[0], l.params[1], l.params[2], l.params[3]);
            },
            shadowSlot, shadowFaces);

        for (int i = 1; i < n; i++) {
            if (shadowSlot[i] < 0) continue;
            const ShaderLight& l = sceneLights[i];
            const glm::vec4 pos(l.position[0], l.position[1], l.position[2], l.position[3]);
            const glm::vec4 dir(l.direction[0], l.direction[1], l.direction[2], l.direction[3]);
            const glm::vec4 par(l.params[0], l.params[1], l.params[2], l.params[3]);

            // How many faces this light gets was said by the SPLIT, which is
            // shared with Vulkan. Asking it again here would be a second
            // copy of the criterion, and answering differently would record fewer faces than
            // were reserved.
            // flipY = false: here it is absorbed by the negative-height viewport.
            const bool ok = (shadowFaces[i] == SHADOW_KEY_MATRICES)
                ? pointShadowMatrices(pos, par, /*flipY=*/false,
                                      &cascadeMatrices[shadowSlot[i]])
                : spotShadowMatrix(pos, dir, par, /*flipY=*/false,
                                   cascadeMatrices[shadowSlot[i]]);
            if (!ok) {
                shadowSlot[i]  = -1;
                shadowFaces[i] = 0;
            }
        }
    }

    // POINT: six-face cubemap. It does not use the camera frustum either, so
    // it exits through here just like the spot.
    // A SPOT that is too wide also enters through here, same criterion as
    // Vulkan: above a 90 degree cone a single face spreads the texels
    // over too much world, and it gets worse with tan(FOV/2).
    const int tipoKey = sceneLights.empty()
                            ? -1
                            : static_cast<int>(sceneLights[0].direction[3] + 0.5f);
    const glm::vec4 paramsKey =
        sceneLights.empty()
            ? glm::vec4(0.0f)
            : glm::vec4(sceneLights[0].params[0], sceneLights[0].params[1],
                        sceneLights[0].params[2], sceneLights[0].params[3]);
    if (tipoKey == static_cast<int>(LightType::Point) ||
        (tipoKey == static_cast<int>(LightType::Spot) && spotNecesitaCubemap(paramsKey))) {
        const ShaderLight& key = sceneLights[0];
        // flipY = false: here the Y convention is absorbed by the shadow pass's
        // negative-height viewport, just like with the cascades.
        if (pointShadowMatrices(
                glm::vec4(key.position[0], key.position[1], key.position[2], key.position[3]),
                glm::vec4(key.params[0], key.params[1], key.params[2], key.params[3]),
                /*flipY=*/false, cascadeMatrices)) {
            activeLayers = kShadowKeyLayers;
        }
        return;
    }

    // SPOT: a single perspective face, in layer 0. It does not use the camera
    // frustum at all (the volume is set by the light's cone), so it exits through
    // here before computing it. Same function as Vulkan: the matrix HAS to
    // be identical or the bias works out on one backend and not on the other.
    if (!sceneLights.empty() &&
        static_cast<int>(sceneLights[0].direction[3] + 0.5f) ==
            static_cast<int>(LightType::Spot)) {
        const ShaderLight& key = sceneLights[0];
        if (spotShadowMatrix(
                glm::vec4(key.position[0], key.position[1], key.position[2], key.position[3]),
                glm::vec4(key.direction[0], key.direction[1], key.direction[2], key.direction[3]),
                glm::vec4(key.params[0], key.params[1], key.params[2], key.params[3]),
                // flipY = false: here the Y convention is absorbed by the
                // negative-height viewport of the shadow pass, just like with the
                // cascades' orthographic. Inverting the matrix too would be
                // applying it twice.
                /*flipY=*/false, cascadeMatrices[0])) {
            activeLayers = 1;
        }
        return;
    }

    // The cascade split lives in cascadeShadowMatrices, shared with the
    // Vulkan path: they were the same lines of math written twice, and
    // a divergence between them went undetected by anything (H3).
    const glm::mat4  proj = cameraProj();
    const glm::mat4& view = cameraView;

    // The key light's direction: that of a point light points at the center of the
    // SCENE, which changes when any object moves and not only when the
    // lights are touched. That is why it is resolved here and not in setLights.
    if (!sceneLights.empty()) {
        SceneCenter centro;
        for (const StaticObject& object : objects) {
            if (object.slotFree)
                continue;   // a free slot is at the origin and would pull on the average
            centro.add(object.transform);
        }
        for (const SkinnedObject& character : skinnedObjects)
            centro.add(character.transform);

        glm::vec3 aim(0.0f);
        if (centro.get(aim)) {
            const ShaderLight& key = sceneLights[0];
            glm::vec3          dir;
            if (keyLightDirection(glm::vec4(key.position[0], key.position[1], key.position[2],
                                            key.position[3]),
                                  glm::vec4(key.direction[0], key.direction[1], key.direction[2],
                                            key.direction[3]),
                                  aim, dir)) {
                lightDirection = dir;
            }
        }
    }

    // flipY = false: here the Y convention is absorbed by the negative-height
    // viewport of the shadow pass, not by the matrix.
    if (!cascadeShadowMatrices(view, proj, glm::normalize(lightDirection),
                               state->shadowDistance(), state->cascadeLambda(),
                               shadowMapSize, /*flipY=*/false,
                               cascadeMatrices, cascadeSplits)) {
        return;
    }
    activeLayers = kShadowCascades;

}

void D3D12Renderer::Impl::ensureSceneInstanceBuffer(size_t count)
{
    if (count == 0 || count <= sceneInstanceCapacity[frameIndex])
        return;

    // It grows in blocks so as not to redo the buffer every time a
    // mesh comes in while loading a scene.
    const size_t newCapacity = (std::max)(count, sceneInstanceCapacity[frameIndex] * 2 + 64);

    if (sceneInstanceAllocations[frameIndex]) {
        // It may be in use by the previous frame.
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
                  "D3D12MA::Allocator::CreateResource(scene instances)");

    const D3D12_RANGE noRead{0, 0};
    throwIfFailed(sceneInstanceAllocations[frameIndex]->GetResource()->Map(
                      0, &noRead, &sceneInstanceMapped[frameIndex]),
                  "ID3D12Resource::Map(scene instances)");
    sceneInstanceCapacity[frameIndex] = newCapacity;
}

D3D12_GPU_VIRTUAL_ADDRESS D3D12Renderer::Impl::instanceAddress(uint32_t index) const
{
    return sceneInstanceAllocations[frameIndex]->GetResource()->GetGPUVirtualAddress() +
           static_cast<UINT64>(index) * sizeof(glm::mat4);
}

bool D3D12Renderer::Impl::releaseStaticObject(StaticObject& object)
{
    // A duplicate carries COPIES of the owner's handles: releasing them would free
    // the same resource twice. It only lowers the owner's count.
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

    // The owner with live duplicates CANNOT release: it would leave them drawing with
    // freed memory, which not even the validation layer warns about.
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

bool D3D12Renderer::Impl::releaseObjectSlot(size_t index)
{
    if (index >= objects.size())
        return false;
    StaticObject& object = objects[index];
    if (object.slotFree)
        return false;

    // This has to be kept BEFORE releaseStaticObject touches it: for a
    // duplicate it nulls the handles, and then the whole struct is reset.
    const bool duplicado = !object.ownsGpu;
    const int  duenyo    = object.sharedMesh;

    // The criterion of who may release lives there; this function only decides what to
    // do with the SLOT according to what that one answered.
    const bool solto = releaseStaticObject(object);

    if (!solto && !duplicado) {
        // Owner with live duplicates. Giving up the slot would leave them drawing
        // memory that another object would have reassigned, and not even the
        // validation layer warns about that: the handles are still valid, just no longer
        // theirs. It is turned off and waits.
        object.meshVisible    = false;
        object.pendingRelease = true;
        drawGroupsDirty       = true;
        return false;
    }

    const std::string key = object.sharedKey;
    // The descriptor block is KEPT: it belongs to the index, not the object
    // (kSrvObjects + index * kSrvPerObject), and whoever reuses the slot will
    // compute exactly the same one. Returning it to kSrvBaseColor would make the
    // loops that refresh probes, shadows and AO for all objects (the ones that
    // do not filter by srvBase) write into the GLOBAL slot: a dead
    // entry overwriting the neutral texture used by objects without a material.
    const UINT srvBase = object.srvBase;
    object             = StaticObject{};
    object.srvBase     = srvBase;
    object.meshVisible = false;
    object.slotFree    = true;
    if (!key.empty()) {
        auto it = sharedMeshOwner.find(key);
        if (it != sharedMeshOwner.end() && it->second == static_cast<int>(index))
            sharedMeshOwner.erase(it);
    }
    objectSlots.release(static_cast<int>(index));
    drawGroupsDirty = true;

    // The duplicate that just died may have been the last one keeping
    // an already retired owner alive. Without this its buffers stayed until the next
    // clearStaticMeshes, which is exactly the leak P11 closes.
    if (duplicado && duenyo >= 0 && static_cast<size_t>(duenyo) < objects.size()) {
        StaticObject& previo = objects[static_cast<size_t>(duenyo)];
        if (previo.pendingRelease && previo.sharedRefs <= 0) {
            // releaseStaticObject decrements before comparing, so its
            // count is left at 1 so that it drops to 0 and releases.
            previo.sharedRefs     = 1;
            previo.pendingRelease = false;
            releaseObjectSlot(static_cast<size_t>(duenyo));
        }
    }
    return true;
}

bool D3D12Renderer::Impl::releaseSkinnedSlot(size_t index)
{
    if (index >= skinnedObjects.size())
        return false;
    SkinnedObject& character = skinnedObjects[index];
    if (character.slotFree)
        return false;

    releaseSkinnedResources(character);

    character          = SkinnedObject{};
    character.visible  = false;
    character.slotFree = true;
    skinnedSlots.release(static_cast<int>(index));
    return true;
}

void D3D12Renderer::Impl::rebuildDrawGroups()
{
    drawGroupRep.clear();
    drawGroupsDirty = false;
    if (objects.empty())
        return;

    // The key is NOT just the mesh. Two objects that share it are painted in the
    // same draw, and a draw binds ONE descriptor block: they can only go
    // together if both blocks say the same thing. What can differ with the
    // same mesh is the reflection probe (t4/t5) and the filler of a texture
    // that could not be read, so both enter the key.
    //
    // The rest of the per-object data is not needed here: metallic and roughness come
    // from the material, which is already in the mesh's content key, and the
    // SSR strength is split by the grouping itself.
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

    // No frustum: the shadow pass draws all four cascades and the pre-pass
    // covers the whole screen, so both want EVERYTHING visible. The SSR
    // strength is left at 0 and the PBR factors at their default value
    // because neither paints color: all three enter the grouping key, so with a single
    // value FEWER draws come out and the resulting map is identical.
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

    // NEGATIVE HEIGHT, for the same reason as the rest of this backend's passes: the
    // shader that READS the map takes the Vulkan orientation for granted.
    //
    // What decides the mapping are the matrix and the viewport TOGETHER, and until now
    // only the matrix had been looked at. With g = the point's Y in light space
    // (GL convention, which is what comes out of orthoRH_ZO without inverting):
    //
    //   Vulkan   lightProj[1][1] *= -1  ->  NDC y = -g, and its viewport puts
    //            y=-1 at the TOP, so it writes to row (1-g)/2.
    //            pbr.frag reads uv.v = (-g)*0.5+0.5 = (1-g)/2.  They match.
    //
    //   D3D12    without inverting      ->  NDC y = g, and its viewport puts
    //            y=+1 at the TOP, so it also wrote to (1-g)/2 (which is why the
    //            comment in computeCascades is right not to invert
    //            the matrix again), BUT the shader reads uv.v = g*0.5+0.5 = (1+g)/2.
    //            Vertically mirrored.
    //
    // Inverting the viewport puts the write at (1+g)/2, which is exactly where the
    // shader looks. The matrix stays as it is.
    D3D12_VIEWPORT shadowViewport{};
    shadowViewport.TopLeftY = static_cast<float>(shadowMapSize);
    shadowViewport.Width    = static_cast<float>(shadowMapSize);
    shadowViewport.Height   = -static_cast<float>(shadowMapSize);
    shadowViewport.MaxDepth = 1.0f;
    const D3D12_RECT shadowScissor{0, 0, static_cast<LONG>(shadowMapSize),
                                   static_cast<LONG>(shadowMapSize)};

    commandList->SetGraphicsRootSignature(shadowRootSignature.Get());
    commandList->SetGraphicsRootConstantBufferView(
        0, sceneUboAllocations[frameIndex]->GetResource()->GetGPUVirtualAddress());
    commandList->RSSetViewports(1, &shadowViewport);
    commandList->RSSetScissorRects(1, &shadowScissor);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (UINT cascade = 0; cascade < kShadowLayers; ++cascade) {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
        dsv.ptr += static_cast<SIZE_T>(cascade) * dsvSize;

        commandList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        commandList->SetGraphicsRoot32BitConstants(1, 1, &cascade, 0);

        // It is drawn in the key light's layers (from 0) and in those of the secondary
        // spots (from kShadowKeyLayers). The ones in between are cleared anyway
        // (the clear above is what leaves them usable) but drawing into
        // them would be recording with the identity matrix on layers nobody
        // samples.
        const bool esDeLaKey   = cascade < activeLayers;
        const bool esDeUnExtra = cascade >= static_cast<UINT>(kShadowKeyLayers) &&
                                 cascade <  static_cast<UINT>(kShadowKeyLayers) + extraLayers;
        if (!esDeLaKey && !esDeUnExtra)
            continue;

        // The ground is not put in the map: it is the receiver, and putting it in would only
        // add its own surface as an occluder of itself.
        //
        // One draw per mesh group, with the instance view pointing at the
        // start of the group's range: that is where shadow.vert takes its model from.
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
    // By default the edit camera rules, which is the one setCamera pushes
    // every frame. It is ALWAYS cleared: when Play stops the view has to go back
    // by itself, without saving or restoring anything.
    sceneCameraProj.reset();

    // headless = exported game, which is ALWAYS in Play and has no editor
    // to push a camera to it: if it is not resolved here, it stays with the backend's
    // default view and never looks through the scene's camera.
    // Same rule as Renderer::isPlaying() in the Vulkan path.
    const bool playing = headless || (uiLayer && uiLayer->isPlaying());
    if (!playing || !scene)
        return;

    GameObject* cam = scene->findCamera();
    if (!cam || !cam->hasCameraComponent())
        return;

    const auto& component = cam->getCameraComponent();

    // projectionMatrix carries Vulkan's Y flip baked in. Here it is unwanted:
    // this backend does not invert the axis (see updateSceneUbo). It is undone instead
    // of rebuilding the matrix by hand so that orthographic, near/far and fov keep
    // coming from a single place.
    glm::mat4 proj = component->projectionMatrix(viewportAspectRatio());
    proj[1][1] *= -1.0f;
    sceneCameraProj = proj;

    // The view and the eye also come from the component. cameraView and cameraPos are
    // simply overwritten: the editor pushes them again in the next frame, so
    // there is no state to restore.
    cameraView = CameraComponent::viewFromWorld(cam->worldTransform);
    cameraPos  = glm::vec3(cam->worldTransform[3]);
}

D3D12Renderer::D3D12Renderer() : m_impl(std::make_unique<Impl>())
{
    // The Impl queries the state through this pointer instead of copying it:
    // this way a setBloomIntensity() from the editor shows up in the next frame.
    m_impl->state = this;

    // NO effect is turned on here. It used to turn them on (fog and a stronger
    // bloom) when this backend had its own test loop and wanted to
    // show them off; behind the editor that overrides what the project says, and a real
    // scene with fog set and a single distant light looks BLACK. The
    // engine default is RendererState's, just like for Vulkan.
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
        throw std::runtime_error("D3D12: the window is not initialized");

    d.hwnd = glfwGetWin32Window(glfwWindow);
    if (d.hwnd == nullptr)
        throw std::runtime_error("D3D12: glfwGetWin32Window did not return an HWND");

    int fbWidth = 0, fbHeight = 0;
    glfwGetFramebufferSize(glfwWindow, &fbWidth, &fbHeight);
    d.width  = static_cast<UINT>(fbWidth > 0 ? fbWidth : 1);
    d.height = static_cast<UINT>(fbHeight > 0 ? fbHeight : 1);
    // At startup, the render is the size of the window: there is no panel yet,
    // nor SSAA multiplying anything.
    d.swapWidth  = d.width;
    d.swapHeight = d.height;
    d.outWidth   = d.width;
    d.outHeight  = d.height;

    UINT factoryFlags = 0;
#ifndef NDEBUG
    // Debug layer BEFORE creating the device: enabling it afterwards does not affect
    // an already created device. If Windows "Graphics Tools" is not present, it fails and
    // we continue without it instead of preventing startup.
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    // DRED (Device Removed Extended Data). It is the only way to know WHICH
    // operation hung the GPU when the device is lost, and it does not depend on the
    // debug layer or on the "Graphics Tools". Like the layer, it has to be
    // requested BEFORE creating the device: it is a process setting that the device
    // reads when it is born.
    //
    // The two halves do NOT cost the same and that is why they do not share the same guard:
    //
    //   - The AUTO-BREADCRUMBS record a WriteBufferImmediate for EVERY command
    //     put into the list. That is a per-frame cost, in every frame,
    //     and Release is the configuration the game is exported from: they go
    //     only in Debug. If someone comes along in six months to "fix"
    //     this guard by removing it, let them know what they are paying, and that to
    //     chase a GPU hang it is enough to reproduce it in Debug, where
    //     the breadcrumbs ARE present.
    //   - The PAGE FAULT records nothing per command: it only makes the
    //     runtime remember the names of the allocations so it can say what
    //     was at the address that blew up. It stays on in both
    //     configurations, because it is exactly what one wants to have when the
    //     hang shows up on a player's machine and there is no second take.
    {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings)))) {
            dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
#ifndef NDEBUG
            dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            diagLog("DRED enabled (auto-breadcrumbs + page fault).");
#else
            diagLog("DRED enabled (page fault only; breadcrumbs cost per frame "
                    "and are Debug only).");
#endif
        } else {
            diagLog("DRED NOT available on this system.");
        }
    }

    throwIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&d.factory)),
                  "CreateDXGIFactory2");

    // Adapter: the highest-performance one is preferred if DXGI 1.6 is available;
    // otherwise, the first hardware one that accepts the feature level. Same WARP
    // discard criterion as D3D12Support::querySupport.
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
        throw std::runtime_error("D3D12: no hardware adapter supports FEATURE_LEVEL_11_0");

    throwIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d.device)),
                  "D3D12CreateDevice");

    // The debug layer's message queue, so it can be drained to
    // file: by itself it writes through OutputDebugString, which without a debugger
    // nobody reads. It only exists if the layer is active (Debug + Graphics
    // Tools); otherwise, the QueryInterface fails and nothing happens here.
    if (SUCCEEDED(d.device->QueryInterface(IID_PPV_ARGS(&d.infoQueue))))
        diagLog("Debug layer message queue redirected to this file.");

    // The hook for throwIfFailed, which is a free function and has no device.
    // It goes on a translation-unit static because there is no more than one D3D12Renderer
    // per process (the device hangs off it and main creates one).
    static D3D12Renderer::Impl* impl = nullptr;
    impl                             = &d;
    g_volcarDeviceRemoved            = [](const char* donde, HRESULT hr) {
        if (impl)
            impl->dumpDeviceRemoved(donde, hr);
    };

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    throwIfFailed(d.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&d.queue)),
                  "ID3D12Device::CreateCommandQueue");

    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.BufferCount = kFrameCount;
    scDesc.Width       = d.width;
    scDesc.Height      = d.height;
    // UNORM, not SRGB: the conversion to screen space will be done by the
    // composition pass when it exists, just like in the Vulkan path.
    scDesc.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.SampleDesc.Count = 1;

    // Tearing: it is what allows presenting WITHOUT waiting for the refresh, that is, the
    // equivalent of VK_PRESENT_MODE_IMMEDIATE. It depends on the adapter and the OS,
    // so it is queried; and the flag has to be requested when CREATING the swapchain,
    // even though the mode is chosen later on each Present. Without it, a Present(0,
    // ALLOW_TEARING) fails.
    {
        BOOL permitido = FALSE;
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(d.factory.As(&factory5))) {
            if (FAILED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                     &permitido, sizeof(permitido)))) {
                permitido = FALSE;
            }
        }
        d.tearingDisponible = (permitido == TRUE);
        if (d.tearingDisponible)
            scDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    ComPtr<IDXGISwapChain1> swapChain1;
    throwIfFailed(d.factory->CreateSwapChainForHwnd(d.queue.Get(), d.hwnd, &scDesc, nullptr,
                                                    nullptr, &swapChain1),
                  "IDXGIFactory4::CreateSwapChainForHwnd");

    // DXGI's Alt+Enter fullscreen does not get along with a window
    // managed by GLFW: it is disabled and the display mode is decided by the engine.
    d.factory->MakeWindowAssociation(d.hwnd, DXGI_MWA_NO_ALT_ENTER);

    throwIfFailed(swapChain1.As(&d.swapChain), "IDXGISwapChain1::QueryInterface(IDXGISwapChain3)");
    d.frameIndex = d.swapChain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    // Those of the swapchain, plus the scene's HDR target and the intermediate LDR
    // that the composition leaves for FXAA.
    // Those of the swapchain, the HDR, the composition's LDR and the
    // multisampled color of the scene pass when there is MSAA.
    // + TAA histories, viewport and the six faces of the probe bake.
    rtvHeapDesc.NumDescriptors = kRtvCount;
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
    // Names so that the DRED breadcrumbs say WHICH list and WHICH queue they talk about,
    // instead of "(unnamed)".
    d.commandList->SetName(L"ListaPrincipal");
    d.queue->SetName(L"ColaDirecta");
    // It is created in the open state and drawFrame expects to find it closed.
    throwIfFailed(d.commandList->Close(), "ID3D12GraphicsCommandList::Close");

    throwIfFailed(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.fence)),
                  "ID3D12Device::CreateFence");
    d.fenceValues.fill(0);
    d.fenceValues[d.frameIndex] = 1;

    d.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (d.fenceEvent == nullptr)
        throwIfFailed(HRESULT_FROM_WIN32(GetLastError()), "CreateEventW");

    // Resource suballocator. It goes AFTER the fence because the first geometry
    // upload needs to wait on the GPU to release its staging.
    d.adapter = adapter;

    D3D12MA::ALLOCATOR_DESC allocatorDesc{};
    allocatorDesc.pDevice  = d.device.Get();
    allocatorDesc.pAdapter = d.adapter.Get();
    throwIfFailed(D3D12MA::CreateAllocator(&allocatorDesc, &d.allocator),
                  "D3D12MA::CreateAllocator");

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    // The usual depth and the multisampled one.
    dsvHeapDesc.NumDescriptors = 3;  // scene, multisampled scene and probe faces
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
                  "ID3D12Device::CreateDescriptorHeap(prepass DSV)");
    d.createSsaoPipelines();
    d.createSsaoTargets();

    d.createSkinningPipelines();
    // Shadows go last: their SRV overwrites the filler array that createMeshResources
    // left in the t3 slot, and it needs the cube's instance buffer
    // already created.
    d.createShadowResources();
    d.computeCascades();
    // The HDR target and the bloom levels need the descriptor heap
    // already created by createMeshResources.
    d.createHdrTargets();
    d.createBloomPipelines();
    // The sky after the heap and the HDR target: it uses a slot of the first and
    // draws into the second.
    d.createSkyboxResources();
    // The IBL comes from the freshly loaded sky, so it goes after.
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

    // The scene's depth, in a version that can be paired with the
    // LDR target: both have to match in sample count. Without MSAA the scene pass one
    // works; with MSAA that one is multisampled and the pre-pass one is used,
    // which is why it is also recorded when something is selected.
    const bool multisampled = sampleCount > 1;
    if (multisampled && !prepassDepthAllocation)
        return;

    D3D12_CPU_DESCRIPTOR_HANDLE dsv =
        multisampled ? prepassDsvHeap->GetCPUDescriptorHandleForHeapStart()
                     : dsvHeap->GetCPUDescriptorHandleForHeapStart();

    commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    // Viewport WITHOUT negative height, unlike the composition's: that one
    // inverts it because fullscreen.vert takes the Vulkan orientation for granted,
    // but this is real geometry and comes out the same as in the scene pass.
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
    // outline.vert/frag only look at the UBO and the push, but the root signature
    // declares the other two ranges: they are left pointing at something valid instead of
    // at zero.
    commandList->SetGraphicsRootDescriptorTable(2, srvHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->SetGraphicsRootShaderResourceView(
        3, instanceAllocation->GetResource()->GetGPUVirtualAddress());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    auto drawOutline = [&](ID3D12PipelineState* pipeline, const glm::mat4& transform,
                           float localExtent,
                           const D3D12_VERTEX_BUFFER_VIEW& vertexView,
                           const D3D12_INDEX_BUFFER_VIEW& indexView, UINT indexCount) {
        if (indexCount == 0)
            return;
        commandList->SetPipelineState(pipeline);

        PushData push{};
        push.transform = transform;
        // flags.y carries the extrusion thickness, which is the slot that vec2
        // had free. Proportional to the object's size IN WORLD, just like the
        // Vulkan path: a flat thickness eats small objects and is not
        // visible on large ones. The minimum of 1 unit prevents a tiny mesh
        // from being left without an outline.
        push.flags = glm::vec2(0.0f, outlineThickness(localExtent, transform));
        commandList->SetGraphicsRoot32BitConstants(1, sizeof(PushData) / 4, &push, 0);

        commandList->IASetVertexBuffers(0, 1, &vertexView);
        commandList->IASetIndexBuffer(&indexView);
        commandList->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
    };

    if (selectedObject >= 0 && selectedObject < static_cast<int>(objects.size())) {
        const StaticObject& object = objects[static_cast<size_t>(selectedObject)];
        if (object.meshVisible)
        {
            // The static's box is in LOCAL space (see StaticObject), which
            // is exactly what outlineThickness expects.
            const glm::vec3 e = object.aabbMax - object.aabbMin;
            const float ext = object.hasBounds ? (glm::max)(e.x, (glm::max)(e.y, e.z)) : 0.0f;
            drawOutline(outlineLdrPipeline.Get(), object.transform, ext,
                        object.vertexBufferView, object.indexBufferView, object.indexCount);
        }
    }
    if (selectedSkinned >= 0 && selectedSkinned < static_cast<int>(skinnedObjects.size())) {
        const SkinnedObject& character = skinnedObjects[static_cast<size_t>(selectedSkinned)];
        if (character.visible && character.vertexCount > 0) {
            // The deformed vertex buffer is NO LONGER an input of the
            // assembler: the scene pass returns it to unordered access as
            // soon as it finishes, because the next frame's compute rewrites it.
            // This pass goes AFTER that, so it has to borrow it
            // and leave it as it was.
            //
            // Without this the draw is invalid (vertex buffer in UNORDERED_ACCESS) and
            // a character's outline was simply not drawn. A static
            // object's was, because its vertex buffer is not touched by the skinning.
            D3D12_RESOURCE_BARRIER vb{};
            vb.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            vb.Transition.pResource   = character.outputVerts->GetResource();
            vb.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            vb.Transition.StateAfter  = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            vb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            commandList->ResourceBarrier(1, &vb);

            drawOutline(outlineSkinnedLdrPipeline.Get(), character.transform,
                        character.restMaxExtent, character.vertexBufferView,
                        character.indexBufferView, character.indexCount);

            std::swap(vb.Transition.StateBefore, vb.Transition.StateAfter);
            commandList->ResourceBarrier(1, &vb);
        }
    }
}

void D3D12Renderer::Impl::recordSceneGeometry(D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                                              D3D12_CPU_DESCRIPTOR_HANDLE dsv, UINT targetWidth,
                                              UINT targetHeight)
{
    // The WHOLE geometry pass: clear, meshes, characters, outline, sky and
    // the editor lines. It comes out of drawFrame so it can be repeated with another
    // camera and another destination, which is what the baking of a reflection probe
    // needs: six faces, the same scene.
    //
    // What does NOT go in: the timestamps (they measure the frame's pass, not a
    // face) and the post-processing, which goes after and over the HDR target.
    commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // Viewport and scissor are set every frame: after a resize the command list's
    // state is reset and carrying the old size would clip the image.
    D3D12_VIEWPORT viewport{};
    viewport.Width    = static_cast<float>(targetWidth);
    viewport.Height   = static_cast<float>(targetHeight);
    viewport.MaxDepth = 1.0f;
    commandList->RSSetViewports(1, &viewport);

    D3D12_RECT scissor{0, 0, static_cast<LONG>(targetWidth), static_cast<LONG>(targetHeight)};
    commandList->RSSetScissorRects(1, &scissor);

    // The mesh first: it writes depth and so the grid behind it
    // is covered where it should be.
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

        // Scene geometry, grouped by shared mesh: N identical visible
        // cubes come out of ONE instanced draw, and each instance takes
        // its matrix from the group's range (flags.x = 1). Culling is still
        // per object (what it decides is whether the object enters its group).
        //
        // The split is redone here and not once per frame because this function
        // also records the six faces of a probe, and each one sees a different
        // set. That the same range can be reused is because each face
        // is submitted and waited on before recording the next.
        const Culling::Frustum cameraFrustum = Culling::frustumFromViewProj(viewProj);

        batchCandidates.clear();
        batchCandidates.reserve(objects.size());
        for (const StaticObject& object : objects) {
            // The frustum test is conservative (it may let something through that
            // is not seen, never remove something that is) and a mesh without a box is drawn
            // always.
            const bool dibujable = object.meshVisible && object.indexCount > 0;
            const bool visible =
                dibujable && (!object.hasBounds ||
                              Culling::aabbVisible(cameraFrustum, object.aabbMin, object.aabbMax,
                                                   object.transform));
            if (dibujable && !visible)
                if (perfCapture) ++statCulledCount;

            // The factors enter the grouping key: since they left
            // makeSharedMeshKey, two objects of the same drawGroup can have
            // different finishes and cannot share a push constant.
            batchCandidates.push_back({object.drawGroup, visible, &object.transform,
                                       state->ssrEnabled() ? object.ssrStrength : 0.0f,
                                       object.metallic, object.roughness});
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

            // The representative's triplet is valid for the whole group: they share
            // mesh, material and probe, which is exactly what decides the group.
            D3D12_GPU_DESCRIPTOR_HANDLE table = srvHeap->GetGPUDescriptorHandleForHeapStart();
            table.ptr += static_cast<UINT64>(rep.srvBase) * srvSize;
            commandList->SetGraphicsRootDescriptorTable(2, table);
            commandList->SetGraphicsRootShaderResourceView(3,
                                                           instanceAddress(batch.firstInstance));

            PushData push{};
            // flags.x = 1: the model comes from the instance buffer, one per
            // instance. The push constant's transform is not looked at.
            // From the GROUP and not the representative: rep is the first object of the
            // drawGroup, and since the factors left the dedup key
            // their values need not be those of this batch. The grouping
            // has already split by factors, so the batch's applies to all
            // its instances; rep's would be drawn on objects that are not it.
            push.metallic  = batch.metallic;
            push.roughness = batch.roughness;
            push.flags     = glm::vec2(1.0f, batch.ssrStrength);
            commandList->SetGraphicsRoot32BitConstants(1, sizeof(PushData) / 4, &push, 0);

            commandList->IASetVertexBuffers(0, 1, &rep.vertexBufferView);
            commandList->IASetIndexBuffer(&rep.indexBufferView);
            commandList->DrawIndexedInstanced(rep.indexCount, batch.instanceCount, 0, 0, 0);
            if (perfCapture) ++statDraws;
            if (perfCapture) statInstanced += static_cast<int>(batch.instanceCount);
        }

        // Ground: shadow receiver and visual reference, NOT part of the
        // scene. It is only drawn when there is no geometry loaded: a project
        // usually brings its own plane, and overlaying another leaves the two
        // fighting over depth and casting shadow on each other.
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
            if (perfCapture) ++statDraws;
            if (perfCapture) ++statInstanced;
        }
    }

    // Characters: same shaders and same root signature as the cube, but the
    // vertex buffer is what the compute just wrote.
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
            // flags.x = 0: the model comes from here, not from the instance buffer,
            // which is the route the engine uses for skinne
            push.transform = character.transform;
            push.flags = glm::vec2(0.0f, state->ssrEnabled() ? character.ssrStrength : 0.0f);

            commandList->IASetVertexBuffers(0, 1, &character.vertexBufferView);
            commandList->IASetIndexBuffer(&character.indexBufferView);

            // One draw per submesh, each with its material's triplet AND with
            // its factors. The push is written INSIDE the loop for that reason: before,
            // it was written once per character with two constants, so the
            // submeshes of the same character could not have different
            // finishes or respond to the sliders. It is what Vulkan already did
            // (one push per submesh, reading from matGfx[sm.materialIndex]).
            for (const SkinnedSubMesh& sub : character.subMeshes) {
                if (sub.indexCount == 0)
                    continue;
                push.metallic  = sub.metallic;
                push.roughness = sub.roughness;
                commandList->SetGraphicsRoot32BitConstants(1, sizeof(PushData) / 4, &push, 0);
                D3D12_GPU_DESCRIPTOR_HANDLE table = srvHeap->GetGPUDescriptorHandleForHeapStart();
                table.ptr += static_cast<UINT64>(sub.srvBase) * srvSize;
                commandList->SetGraphicsRootDescriptorTable(2, table);
                commandList->DrawIndexedInstanced(sub.indexCount, 1, sub.indexStart, 0, 0);
                if (perfCapture) ++statDraws;
                if (perfCapture) ++statInstanced;
            }
        }
    }

    // The sky at the end of the geometry: it relies on the depth already written
    // to come out only where there is nothing, and so does not pay shading for pixels
    // that the scene will cover.
    recordSkybox();

    // The grid and the debug lines are an EDITOR matter: in an exported
    // game they paint nothing, and they came out anyway because this backend did not look at
    // headless mode.
    // The grid and the debug lines share pipeline and vertex
    // format, but NOT condition: before, the lines hung off the grid's `if`,
    // so a scene without a grid also took the editor's
    // gizmos down with it. Each with its own.
    const bool dibujaRejilla = gridVertexCount > 0;
    const bool dibujaLineas  = debugLineVertices > 0 && debugLinesAllocation;

    if (gizmoPipeline && !headless && (dibujaRejilla || dibujaLineas)) {
        commandList->SetPipelineState(gizmoPipeline.Get());
        commandList->SetGraphicsRootSignature(rootSignature.Get());
        // glm stores the matrix in columns and the translated HLSL declares it
        // row_major: the 16 raw floats are interpreted the same as in Vulkan,
        // without transposing.
        commandList->SetGraphicsRoot32BitConstants(0, 16, &viewProj[0][0], 0);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);

        if (dibujaRejilla) {
            commandList->IASetVertexBuffers(0, 1, &gridVertexBufferView);
            commandList->DrawInstanced(gridVertexCount, 1, 0, 0);
        }

        // Whatever the drawer sent this frame (in the editor,
        // ViewportPanel through submitDebugLines): colliders, lights,
        // camera frustum and selection axes.
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

    // The device was lost at some point that could not throw (the destructor
    // goes through waitForGpu). Here it can, and it is the same outcome the Present
    // below already has: continuing to draw over a broken fence only buries the real
    // error, which is already in d3d12_diag.log.
    if (d.deviceLost) {
        throw std::runtime_error(std::string("D3D12: device lost at ") +
                                 (d.deviceLostDonde ? d.deviceLostDonde : "?") + " (HRESULT " +
                                 hresultToString(d.deviceLostHr) + ")");
    }

    // First thing in the frame: the size recorded by the window callback. Here
    // we are already in the main loop, outside the WindowProc, so DXGI can be
    // touched and an exception has somewhere to go.
    d.applyPendingResize();

    // And a panel size change, which moves everything internal without touching the
    // swapchain.
    d.applyPendingRenderSize();

    // And an anti-aliasing change, which moves targets and pipelines: here, between
    // frames, not in the middle of one.
    d.applyPendingSampleCount();

    // And the shadow map side, for the same reason: between frames, not in
    // the middle of one.
    d.applyPendingShadowSize();

    // The times left by the last time this slot was used: moveToNextFrame
    // already waited on its fence, so they are complete. They are read BEFORE recording
    // anything, because the frame that starts is going to overwrite them.
    d.readTimestamps();

    // Which camera rules this frame: the edit one or the scene's if Play runs.
    // It goes before everything that draws or measures (probes bake with the
    // UBO, cascades are split over the frustum) because it changes the
    // projection and with it the culling and the shadow range.
    d.resolveFrameCamera();
    d.updateViewProj();
    d.computeCascades();

    // The draw groups and the instance buffer, BEFORE the probes: the
    // bake records the geometry pass six times, and that pass groups and
    // writes into this buffer. Without this, the first bake after loading a
    // scene captured the sky and nothing else, because the objects did not yet have a
    // group assigned.
    if (!d.objects.empty()) {
        if (d.drawGroupsDirty)
            d.rebuildDrawGroups();
        d.instanceRegionStride = d.objects.size();
        d.ensureSceneInstanceBuffer(d.instanceRegionStride * 2);
    }

    // Probes: additions, removals and baking. BEFORE opening the frame, because baking
    // records into this same command list and waits on the GPU: with the frame
    // half done, the allocator's Reset fails and nothing gets baked, silently.
    d.syncProbes();

    // And the occlusion slot, which depends on the SSAO switch.
    d.refreshAoSlots();

    // A Reset that fails leaves the frame unrecorded. Nothing has been submitted, so
    // the GPU is not left half done, but syncProbes() above HAS
    // already run: discarding it silently is how a bake gets lost without
    // anyone noticing.
    ID3D12CommandAllocator* allocator = d.allocators[d.frameIndex].Get();
    if (const HRESULT hr = allocator->Reset(); FAILED(hr)) {
        d.notarFrameDescartado("ID3D12CommandAllocator::Reset (drawFrame)", hr);
        return;
    }
    if (const HRESULT hr = d.commandList->Reset(allocator, nullptr); FAILED(hr)) {
        d.notarFrameDescartado("ID3D12GraphicsCommandList::Reset (drawFrame)", hr);
        return;
    }

    // ALL the marks at frame start, and then each pass overwrites its
    // own. The resolve copies the whole range, so a query that is not written this frame
    // would keep the tick from three frames ago and give an absurd subtraction
    // (a 735 ms Forward+ was seen with the mode off). By writing
    // all of them, a pass that does not run measures zero, which is the truth.
    for (UINT slot = 0; slot < Impl::TsCount; ++slot)
        d.markTimestamp(slot);

    // The previous frame's counts have already been read by the panel (the interface is
    // built before recording), so here they can be reset.
    d.statDraws       = 0;
    d.statInstanced   = 0;
    d.statCulledCount = 0;

    // The three computes go BEFORE opening the render target: they write the vertex
    // buffer that the graphics pass will read this same frame.
    if (!d.skinnedObjects.empty()) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsed = d.tickFrequency.QuadPart > 0
                                   ? static_cast<double>(now.QuadPart - d.lastTick.QuadPart) /
                                         static_cast<double>(d.tickFrequency.QuadPart)
                                   : 0.0;
        d.lastTick = now;

        // Each character advances in its own cycle: clips do not last the
        // same, and a shared time would make the short ones jump.
        for (Impl::SkinnedObject& character : d.skinnedObjects) {
            // Except those driven from outside: the editor advances the clock
            // per frame from the GameObject's Animator, and adding here too would
            // put them at double speed.
            if (character.externalClock)
                continue;
            character.animTime = advanceMeshClock(character.animTime, static_cast<float>(elapsed),
                                                  character.ticksPerSecond, character.animDuration,
                                                  character.visible);
        }

        d.recordSkinning();
    }

    // The UBO is written once per frame and read by both passes: the shadow
    // one needs lightSpaceMatrix, the main one everything else.
    d.updateSceneUbo();

    // And the split of the shadow/pre-pass range. It is rewritten entirely: moving an
    // object need not notify the renderer. It goes here, with the frame already
    // open, because a probe's bake may have changed the probe
    // assignment and with it the groups.
    if (!d.objects.empty()) {
        if (d.drawGroupsDirty)
            d.rebuildDrawGroups();
        d.buildShadowBatches();
    } else {
        d.shadowBatches.clear();
        d.instanceRegionStride = 0;
    }

    // The same for the characters: the shadow pass draws them with
    // StartInstanceLocation, and shadow.vert takes its model from this buffer.
    if (!d.skinnedObjects.empty()) {
        d.ensureSkinnedInstanceBuffer(d.skinnedObjects.size());
        if (d.skinnedInstanceMapped[d.frameIndex]) {
            auto* matrices = static_cast<glm::mat4*>(d.skinnedInstanceMapped[d.frameIndex]);
            for (size_t i = 0; i < d.skinnedObjects.size(); ++i)
                matrices[i] = d.skinnedObjects[i].transform;
        }
    }

    // Shadows before the main pass: pbr.frag samples the map that is recorded
    // here.
    if (d.shadowPipeline) {
        d.markTimestamp(Impl::TsShadow);
        d.recordShadowPasses();
        d.markTimestamp(Impl::TsShadow + 1);
    }

    // And the occlusion, which needs its own depth and produces it with two
    // computes: pbr.frag multiplies it into the ambient in the next pass.
    d.markTimestamp(Impl::TsSsao);
    d.recordDepthPrepassAndSsao();
    d.markTimestamp(Impl::TsSsao + 1);

    // Per-cell light split. It goes after the pre-pass because tiled mode
    // reduces each tile's depth from it.
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

    // With the scene in a texture, the final pass writes there and the backbuffer is
    // left for the interface, which will draw it inside its panel.
    D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv = backBufferRtv;
    const bool toTexture = d.renderToTexture && d.viewportAllocation;
    if (toTexture) {
        sceneRtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        sceneRtv.ptr += static_cast<SIZE_T>(kRtvViewport) * d.rtvSize;

        D3D12_RESOURCE_BARRIER toTarget{};
        toTarget.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toTarget.Transition.pResource   = d.viewportAllocation->GetResource();
        toTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toTarget.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        d.commandList->ResourceBarrier(1, &toTarget);

        // Already in RENDER_TARGET: the final AA pass writes it entirely further
        // down, but the very first one needs the first use.
        d.estrenarRenderTarget(d.viewportAllocation->GetResource(), d.viewportInicializado);
    }

    // The scene is NOT drawn into the backbuffer: it goes to the HDR target, which is the
    // only place where the bloom threshold can tell apart what exceeds
    // 1.0. The backbuffer is written afterwards by the composition pass.
    // With MSAA the scene is drawn into the multisampled pair and resolved on
    // closing the pass; without it, straight into the usual HDR.
    const bool multisampled = d.sampleCount > 1 && d.hdrMsAllocation && d.depthMsAllocation;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(multisampled ? kRtvSceneMsaa : kRtvSceneHdr) * d.rtvSize;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = d.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    if (multisampled)
        dsv.ptr += d.dsvSize;
    // The UI frame, BEFORE the scene pass: that is where WORLD canvases are
    // recorded, and this call is the one that builds their draw data, computes
    // their model matrix and sizes the buffer they share with the screen
    // ones. Only once per frame (see the comment on uiVertexCursor).
    // It goes AFTER resolveFrameCamera/updateViewProj because the model matrix
    // needs the frame's view for the billboard, and AFTER the probe bake
    // so that one does not take the sizing down with it.
    d.beginUiFrame();

    d.markTimestamp(Impl::TsScene);
    d.recordSceneGeometry(rtv, dsv, d.width, d.height);
    // The world canvases, at the end of the scene pass: the geometry and the sky
    // have already written depth, so a wall in front hides them. Here and not
    // inside recordSceneGeometry on purpose: that function is reused by the
    // probe bake, and a probe must not capture the interface.
    d.recordWorldCanvases(d.width, d.height);
    d.markTimestamp(Impl::TsScene + 1);

    // The deformed vertex buffer goes back to unordered access: the next
    // frame rewrites it with the compute, and it has to find it as the
    // previous one left it or the transition there would start from a state that it is not in.
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
        // Multisample to single sample: from here on all the post reads the
        // usual HDR, which is the only one with UAV and read views.
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

    // Reflections before the fog: they read the scene as it came out of the pass and
    // add the reflected part to it; the fog goes after because it tints EVERYTHING that is there.
    if (d.state->ssrEnabled()) {
        d.markTimestamp(Impl::TsSsr);
        d.recordSsr();
        d.markTimestamp(Impl::TsSsr + 1);
    }

    // Fog BEFORE the bloom: it rewrites the scene, and what the bloom filters
    // has to be already what will be seen.
    // Now that the switch lives in the shared state, it is respected: it is the
    // same one that turns off the fog in the editor's View menu.
    if (d.fogPipeline && d.state->fogEnabled()) {
        d.markTimestamp(Impl::TsFog);
        d.recordFog();
        d.markTimestamp(Impl::TsFog + 1);
    }

    // Motion blur after the fog and before the bloom: it blurs the image just
    // as it will be seen, and the trail drags the highlights so they bloom
    // with them. Off, it records not a single command.
    if (d.motionBlurActive()) {
        d.markTimestamp(Impl::TsMotionBlur);
        d.recordMotionBlur();
        d.markTimestamp(Impl::TsMotionBlur + 1);
    }

    // Bloom, composition with tone mapping and FXAA down to the backbuffer. The
    // anti-aliasing is timed inside: TAA and FXAA are sewn to this pass.
    if (d.compositePipeline) {
        d.markTimestamp(Impl::TsBloom);
        d.recordBloomAndComposite(sceneRtv);
        d.markTimestamp(Impl::TsBloom + 1);
    }

    if (toTexture) {
        // And to read, which is how the interface wants it.
        D3D12_RESOURCE_BARRIER toRead{};
        toRead.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRead.Transition.pResource   = d.viewportAllocation->GetResource();
        toRead.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toRead.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toRead.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        d.commandList->ResourceBarrier(1, &toRead);

        // Nobody has touched the backbuffer: it is cleared so the interface
        // does not draw over the previous frame's contents.
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

    // The last mark and the dump, now with everything recorded.
    d.markTimestamp(Impl::TsFrame + 1);
    d.resolveTimestamps();

    // Same as the Resets: without Close there is nothing to execute and the frame
    // falls entirely. Here it hurts more, because all the frame's work is already
    // recorded and the timestamps are left unresolved.
    if (const HRESULT hr = d.commandList->Close(); FAILED(hr)) {
        d.notarFrameDescartado("ID3D12GraphicsCommandList::Close (drawFrame)", hr);
        return;
    }

    ID3D12CommandList* lists[] = {d.commandList.Get()};
    d.queue->ExecuteCommandLists(1, lists);

    // This is where D3D12 chooses the presentation mode, and not when creating the
    // swapchain like Vulkan: SyncInterval 1 waits for the refresh and 0 does not.
    //
    // Mailbox has no equivalent in DXGI, so it falls back to Vsync (the UI already
    // disables it with its reason). Immediate also needs the tearing flag,
    // which is only valid if the swapchain was created with its own.
    UINT sync  = 1;
    UINT flags = 0;
    if (d.state->presentMode() == PresentMode::Immediate && d.tearingDisponible) {
        sync  = 0;
        flags = DXGI_PRESENT_ALLOW_TEARING;
    }
    const HRESULT presentHr = d.swapChain->Present(sync, flags);
    if (presentHr == DXGI_ERROR_DEVICE_REMOVED || presentHr == DXGI_ERROR_DEVICE_RESET) {
        // The dump BEFORE throwing: the exception takes the process down
        // and with it the device, which is the one that has to be asked.
        d.dumpDeviceRemoved("IDXGISwapChain3::Present", presentHr);
        throw std::runtime_error("D3D12: device lost during Present (HRESULT " +
                                 hresultToString(presentHr) + ")");
    }

    // Whole frame recorded, submitted and presented: the discard streak
    // is broken here. Without this, three isolated bad frames over a session
    // would add up and end up declaring the device lost for no reason.
    d.framesDescartadosSeguidos = 0;

    // What the debug layer said during this frame, to the file. It is a
    // queue: if it is not drained, it fills up and starts discarding.
    d.drainInfoQueue();

    d.moveToNextFrame();
}

// ── Startup splash ─────────────────────────────────────────────────────────
//
// Mirrors the Vulkan path (SplashScreen + Renderer::beginSplash/drawSplashFrame)
// with the same shaders (splash.vert/.frag, translated to DXIL by the build).
// Before this the backend inherited EditorRenderer's defaults (no splash), and
// the runtime silently started without one under DirectX 12.

bool D3D12Renderer::beginSplash(const std::string& logoPath)
{
    Impl& d = *m_impl;
    if (!d.initialized)
        return false;
    if (d.splashPipeline)
        return true;  // already set up: a second call must not leak the first logo

    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!loadSplashImage(logoPath, rgba, w, h) || w <= 0 || h <= 0)
        return false;  // no logo: start without a splash, as in Vulkan

    // splash.vert emits uv.y = 0 at clip y = -1. That is the TOP of the screen
    // in Vulkan and the BOTTOM in D3D12, and the SPIR-V -> HLSL translation
    // does not flip Y. The engine's own fullscreen passes don't notice (they
    // read targets the engine rendered with the same convention), but this
    // texture comes from a PNG: flip its rows so the logo is upright without
    // touching the shader the Vulkan path shares.
    const size_t rowBytes = static_cast<size_t>(w) * 4;
    for (int top = 0, bottom = h - 1; top < bottom; ++top, --bottom)
        std::swap_ranges(rgba.begin() + top * rowBytes, rgba.begin() + (top + 1) * rowBytes,
                         rgba.begin() + bottom * rowBytes);

    // Like SplashScreen::init in Vulkan: any failure from here on (missing
    // .dxil, root signature, PSO, a logo too big for a texture) means "no
    // splash", never an exception that would kill the exported game at startup.
    try {
    // sRGB, like the Vulkan splash (VK_FORMAT_R8G8B8A8_SRGB): sampling returns
    // linear and the sRGB render target view re-encodes it, so colours match.
    d.splashLogo = d.uploadTexture(rgba.data(), static_cast<UINT>(w), static_cast<UINT>(h), 1,
                                   DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4, kSrvSplash);
    d.splashLogoAspect = static_cast<float>(w) / static_cast<float>(h);

    // Root signature: the three floats of splash.frag's push block in b0, the
    // logo in t0 and a linear clamp sampler in s0 (same shape as FXAA).
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors     = 1;
    range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = 3;  // alpha, imgAR, screenAR
    params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &range;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister   = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters     = _countof(params);
    rootDesc.pParameters       = params;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers   = &sampler;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errorBlob;
    const HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                   &serialized, &errorBlob);
    if (FAILED(hr)) {
        std::string detail;
        if (errorBlob)
            detail.assign(static_cast<const char*>(errorBlob->GetBufferPointer()),
                          errorBlob->GetBufferSize());
        throw std::runtime_error("D3D12: splash root signature (HRESULT " + hresultToString(hr) +
                                 ") " + detail);
    }
    throwIfFailed(d.device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                serialized->GetBufferSize(),
                                                IID_PPV_ARGS(&d.splashRootSignature)),
                  "ID3D12Device::CreateRootSignature(splash)");

    const std::vector<char> vs = readBinaryFile("shaders/splash.vert.dxil");
    const std::vector<char> ps = readBinaryFile("shaders/splash.frag.dxil");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature        = d.splashRootSignature.Get();
    pso.VS                    = {vs.data(), vs.size()};
    pso.PS                    = {ps.data(), ps.size()};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets      = 1;
    pso.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;  // the splash's sRGB views
    pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc.Count      = 1;
    pso.SampleMask            = UINT_MAX;
    pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    for (auto& rt : pso.BlendState.RenderTarget)
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable   = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    throwIfFailed(d.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&d.splashPipeline)),
                  "ID3D12Device::CreateGraphicsPipelineState(splash)");
    } catch (const std::exception& e) {
        diagLog(std::string("beginSplash: ") + e.what() + " (starting without a splash)");
        d.splashPipeline.Reset();
        d.splashRootSignature.Reset();
        if (d.splashLogo) {
            d.waitForGpu();  // the upload may still be referenced by the queue
            d.splashLogo->Release();
            d.splashLogo = nullptr;
        }
        return false;
    }
    return true;
}

void D3D12Renderer::drawSplashFrame(float alpha)
{
    Impl& d = *m_impl;
    // Like the Vulkan version: without a splash, or with a lost device, the
    // frame is simply skipped; drawFrame is the one that reports errors.
    if (!d.initialized || !d.splashPipeline || d.deviceLost)
        return;

    // We are in the runtime's main loop, outside the WindowProc: a pending
    // resize can be applied here (it also re-creates the splash's sRGB views).
    d.applyPendingResize();

    // Same bookkeeping as drawFrame on failure: notarFrameDescartado logs it to
    // d3d12_diag.log and counts the streak that eventually flags the device.
    ID3D12CommandAllocator* allocator = d.allocators[d.frameIndex].Get();
    if (const HRESULT hr = allocator->Reset(); FAILED(hr)) {
        d.notarFrameDescartado("ID3D12CommandAllocator::Reset (splash)", hr);
        return;
    }
    if (const HRESULT hr = d.commandList->Reset(allocator, d.splashPipeline.Get()); FAILED(hr)) {
        d.notarFrameDescartado("ID3D12GraphicsCommandList::Reset (splash)", hr);
        return;
    }

    D3D12_RESOURCE_BARRIER toRenderTarget{};
    toRenderTarget.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRenderTarget.Transition.pResource   = d.renderTargets[d.frameIndex].Get();
    toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRenderTarget.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    d.commandList->ResourceBarrier(1, &toRenderTarget);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(kRtvSplash + d.frameIndex) * d.rtvSize;
    const float clear[4] = {0.05f, 0.05f, 0.06f, 1.0f};  // same background as splash.frag
    d.commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    d.commandList->ClearRenderTargetView(rtv, clear, 0, nullptr);

    // The swapchain's size, not the render size (width/height, which SSAA or
    // the editor's panel can make different): this draws straight to the back
    // buffer, like Vulkan with m_swapChainExtent.
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(d.swapWidth),
                                  static_cast<float>(d.swapHeight), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(d.swapWidth), static_cast<LONG>(d.swapHeight)};
    d.commandList->RSSetViewports(1, &viewport);
    d.commandList->RSSetScissorRects(1, &scissor);

    const float push[3] = {alpha, d.splashLogoAspect,
                           d.swapHeight > 0 ? static_cast<float>(d.swapWidth) /
                                                  static_cast<float>(d.swapHeight)
                                            : 1.0f};
    ID3D12DescriptorHeap* heaps[] = {d.srvHeap.Get()};
    d.commandList->SetDescriptorHeaps(1, heaps);
    d.commandList->SetGraphicsRootSignature(d.splashRootSignature.Get());
    d.commandList->SetGraphicsRoot32BitConstants(0, 3, push, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE logoTable = d.srvHeap->GetGPUDescriptorHandleForHeapStart();
    logoTable.ptr += static_cast<UINT64>(kSrvSplash) * d.srvSize;
    d.commandList->SetGraphicsRootDescriptorTable(1, logoTable);
    d.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.commandList->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER toPresent = toRenderTarget;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    d.commandList->ResourceBarrier(1, &toPresent);

    if (const HRESULT hr = d.commandList->Close(); FAILED(hr)) {
        d.notarFrameDescartado("ID3D12GraphicsCommandList::Close (splash)", hr);
        return;
    }
    ID3D12CommandList* lists[] = {d.commandList.Get()};
    d.queue->ExecuteCommandLists(1, lists);

    // Always vsync, whatever the project's present mode: the fade is timed, not
    // frame-counted, so it looks the same; it only caps how long each splash
    // frame blocks while assets load.
    const HRESULT presentHr = d.swapChain->Present(1, 0);
    if (presentHr == DXGI_ERROR_DEVICE_REMOVED || presentHr == DXGI_ERROR_DEVICE_RESET) {
        d.dumpDeviceRemoved("IDXGISwapChain3::Present (splash)", presentHr);
        throw std::runtime_error("D3D12: device lost during the splash Present (HRESULT " +
                                 hresultToString(presentHr) + ")");
    }
    d.drainInfoQueue();
    d.moveToNextFrame();
}

void D3D12Renderer::resize(uint32_t width, uint32_t height)
{
    Impl& d = *m_impl;
    if (!d.initialized)
        return;
    // Minimized window: DXGI rejects 0x0 and there is nothing to present.
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

    // The GPU cannot be using the old buffers.
    waitForGpu();

    // The value the next frame will start with. It is captured HERE, with
    // frameIndex still pointing at the slot that was just waited on: waitForGpu
    // left it at "completed + 1", which is the only value known to be reachable.
    const UINT64 nextFenceValue = fenceValues[frameIndex];

    // And NO live reference to them can remain, or ResizeBuffers fails
    // with E_INVALIDARG. Releasing renderTargets is not enough: a closed command list
    // retains the resources it recorded, and the last frame recorded precisely the
    // back buffer barriers. Resetting it releases that retention; it is closed
    // again because drawFrame expects to find it closed.
    for (auto& allocator : allocators) {
        if (allocator)
            allocator->Reset();
    }
    if (commandList && allocators[0]) {
        commandList->Reset(allocators[0].Get(), nullptr);
        commandList->Close();
    }
    releaseRenderTargets();

    // The tearing flag has to be REPEATED here: ResizeBuffers recreates the
    // buffers with the flags passed to it, not with the ones it had. Losing it
    // would leave Present(0, ALLOW_TEARING) failing from the first resize on.
    throwIfFailed(swapChain->ResizeBuffers(kFrameCount, pendingWidth, pendingHeight,
                                           DXGI_FORMAT_R8G8B8A8_UNORM,
                                           tearingDisponible
                                               ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
                                               : 0),
                  "IDXGISwapChain3::ResizeBuffers");

    swapWidth  = pendingWidth;
    swapHeight = pendingHeight;
    // Without a panel, the render is the window's size. With a panel the panel rules,
    // and resizing the window need not move it. The render one
    // comes from the output one, which with SSAA are not the same: it is recomputed by
    // applyPendingRenderSize in the next frame, here it is enough to leave the
    // output one up to date.
    if (!renderToTexture || pendingRenderWidth == 0) {
        outWidth  = pendingWidth;
        outHeight = pendingHeight;
        width     = pendingWidth;
        height    = pendingHeight;
    }
    frameIndex = swapChain->GetCurrentBackBufferIndex();

    // ResizeBuffers may return the index to ANY slot, not to the next one.
    // The per-slot fenceValues then no longer correspond to what has
    // really been signaled: a slot can end up holding a value that the
    // GPU will never reach, and moveToNextFrame's wait is
    // INFINITE: the process hangs silently, with no API or validation layer
    // error. Setting them all to the live value is what breaks that trap.
    fenceValues.fill(nextFenceValue);

    createRenderTargetViews();

    // The depth buffer has the window's size: if it is not recreated,
    // the test is done against a surface of another size.
    createDepthBuffer();

    // And the HDR target with the bloom levels, for the same reason. Only if they
    // already existed: on first startup init() creates them after the resize.
    if (hdrAllocation)
        createHdrTargets();

    // And the SSAO ones, which are also the window's size.
    if (ssaoRawAllocation) {
        createSsaoTargets();
        ssaoBlurNeedsUav = false;  // freshly created: it is already in write state
    }

    // The projection's aspect depends on the size: without this the grid
    // gets distorted when stretching the window.
    updateViewProj();

    // And the cascades are split over the camera frustum, which has just
    // changed shape: without recomputing them, the shadow volumes stay
    // fitted to the previous aspect.
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

    // The cascades are split over the camera frustum: without recomputing them
    // here they would keep covering the volume of the previous framing, and the shadow would
    // lag behind when moving the view.
    d.computeCascades();
}

void D3D12Renderer::setLights(const Light* lights, size_t count)
{
    Impl& d = *m_impl;
    d.sceneLights.clear();
    if (!lights || count == 0)
        return;

    count = (std::min)(count, static_cast<size_t>(MAX_LIGHTS));
    d.sceneLights.resize(count);
    std::memcpy(d.sceneLights.data(), lights, count * sizeof(ShaderLight));

    // ONLY light 0 casts shadow, and where its direction comes from is decided by
    // computeCascades: that of a point light points at the center of the shaded
    // volume, which depends on the camera. Deriving it here would leave it frozen
    // at whatever camera there was when the lights changed.
    d.computeCascades();
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

    // The index is decided HERE and not on insert, because three things that
    // are written further down come from it: the descriptor block, the `sharedMesh` of
    // whoever uploads the mesh and the `sharedMeshOwner` entry. With recycled
    // slots, `objects.size()` is no longer this object's index.
    const int  reciclado = d.objectSlots.acquire();
    const size_t indice  = reciclado >= 0 ? static_cast<size_t>(reciclado) : d.objects.size();

    // Is this same mesh with this same material already in VRAM? The key is by
    // CONTENT, so two cubes created separately share it. The
    // duplicate keeps the owner's handles and uploads not a single byte.
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

        // Local bounding box, once and for all: it does not depend on the
        // transform, so moving or rotating it does not force recomputing it.
        // Parentheses around the name: windows.h defines max as a macro and
        // without them it does not compile.
        glm::vec3 lo((std::numeric_limits<float>::max)());
        glm::vec3 hi(std::numeric_limits<float>::lowest());
        for (const Vertex& v : mesh.vertices) {
            lo = (glm::min)(lo, v.pos);
            hi = (glm::max)(hi, v.pos);
        }
        object.aabbMin   = lo;
        object.aabbMax   = hi;
        object.hasBounds = true;

        object.sharedMesh = static_cast<int>(indice);
        object.ownsGpu    = true;
        object.sharedRefs = 1;  // itself
    }

    // With an ORM map, the map rules: both are forced to 1.0 so that the shader's
    // clamp(orm.g/b * push.factor, ...) lets the texel through as
    // is, without the material's factor scaling it. Same criterion as
    // Renderer::createSharedGpuMesh (Vulkan) at the same point. Before this
    // fix, D3D12 copied mesh.material.metallic/roughness without checking whether there was
    // an ORM, and a material with a map but metallic=0.0 (the default) came out with
    // dead metalness only in this backend.
    const bool tieneMapaOrm = chooseTextureSource(mesh.material.metallicRoughnessPath,
                                                   mesh.material.embeddedMetallicRoughness) !=
                              TextureSource::None;
    object.metallic  = tieneMapaOrm ? 1.0f : mesh.material.metallic;
    object.roughness = tieneMapaOrm ? 1.0f : mesh.material.roughness;
    object.hasOrmMap = tieneMapaOrm;

    // Own triplet in the heap while slots remain. Past the cap it keeps
    // the global one: worse look, but it never writes outside the heap.
    //
    // The descriptor block is NOT shared even though the mesh is: t4 and t5
    // carry the reflection probe that applies to THIS object, and two identical cubes
    // in two different rooms reflect different things. What is
    // shared are the resources they point to, which is where the memory is.
    if (indice < kMaxObjectSlots) {
        const UINT slot = kSrvObjects + static_cast<UINT>(indice) * kSrvPerObject;
        object.srvBase  = slot;

        if (reusa) {
            // Same resources, new views. The formats are those chosen by
            // uploadMaterialTexture (resolveSrgb: slot + sidecar): they are read from the
            // resource itself.
            if (object.baseColorAllocation)
                d.createTexture2DSrv(object.baseColorAllocation->GetResource(),
                                     formatOf(object.baseColorAllocation), slot + 0);
            else
                d.createTexture2DSrv(d.baseColorAllocation->GetResource(),
                                     DXGI_FORMAT_R8G8B8A8_UNORM, slot + 0);

            if (object.normalMapAllocation)
                d.createTexture2DSrv(object.normalMapAllocation->GetResource(),
                                     formatOf(object.normalMapAllocation), slot + 1);
            else
                d.createTexture2DSrv(d.normalMapAllocation->GetResource(),
                                     DXGI_FORMAT_R8G8B8A8_UNORM, slot + 1);

            d.fillSharedSlots(slot);

            if (object.metalRoughAllocation)
                d.createTexture2DSrv(object.metalRoughAllocation->GetResource(),
                                     formatOf(object.metalRoughAllocation), slot + 3);
        } else {
            object.baseColorAllocation = d.uploadMaterialTexture(
                mesh.material.texturePath, mesh.material.embeddedTexture, TextureKind::BaseColor,
                slot + 0);
            if (!object.baseColorAllocation) {
                // With no material asking for a texture, white; if it asked and it could not
                // be read, checkerboard. Before, both fell to white and a missing
                // file went unnoticed.
                const bool sePidio = !mesh.material.texturePath.empty() ||
                                     !mesh.material.embeddedTexture.empty();
                ID3D12Resource* relleno =
                    (sePidio && d.missingTextureAllocation)
                        ? d.missingTextureAllocation->GetResource()
                        : d.baseColorAllocation->GetResource();
                d.createTexture2DSrv(relleno, DXGI_FORMAT_R8G8B8A8_UNORM, slot + 0);
            }

            object.normalMapAllocation = d.uploadMaterialTexture(
                mesh.material.normalMapPath, mesh.material.embeddedNormalMap, TextureKind::Normal,
                slot + 1);
            if (!object.normalMapAllocation)
                d.createTexture2DSrv(d.normalMapAllocation->GetResource(),
                                     DXGI_FORMAT_R8G8B8A8_UNORM, slot + 1);

            // t3..t7: shadows, environment and occlusion, which belong to everyone.
            d.fillSharedSlots(slot);

            // And on top, the own ORM if the material has one: it overwrites the neutral that
            // fillSharedSlots just left.
            object.metalRoughAllocation =
                d.uploadMaterialTexture(mesh.material.metallicRoughnessPath,
                                        mesh.material.embeddedMetallicRoughness, TextureKind::Orm,
                                        slot + 3);
        }
    }

    if (!reusa) {
        // The key is stored IN the object: when releasing it its entry has to be removed
        // from the map, and searching for it by value would be walking the whole thing.
        object.sharedKey = key;
        d.sharedMeshOwner[key] = static_cast<int>(indice);
    }
    if (reciclado >= 0)
        d.objects[indice] = object;
    else
        d.objects.push_back(object);
    d.drawGroupsDirty = true;
    return static_cast<int>(indice);
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

// See EditorRenderer::setObjectMaterialFactors. Here the factors ALREADY lived per
// object (StaticObject), so this is writing two floats: no waitForGpu, no
// re-upload, no touching the descriptor heap. The only thing to respect is
// the ORM map rule, which in this backend is decided on register
// (addStaticMesh / rebuildStaticMesh) and remembered in hasOrmMap.
//
// drawGroupsDirty is NOT raised on purpose: the factors stopped being in the
// draw group key (they left makeSharedMeshKey) and what separates them
// now is the INSTANCE GROUPING key, which is redone every frame in
// recordSceneGeometry. Raising it here would redo the groups on every frame of
// a drag without any of them having changed.
void D3D12Renderer::setObjectMaterialFactors(size_t objectIndex, float metallic, float roughness)
{
    if (objectIndex >= m_impl->objects.size())
        return;
    Impl::StaticObject& object = m_impl->objects[objectIndex];
    object.metallic  = object.hasOrmMap ? 1.0f : metallic;
    object.roughness = object.hasOrmMap ? 1.0f : roughness;
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
    // It is stored and used. Before it was discarded with a (void), and with it the
    // global radius was discarded too: the per-cell split always took
    // params.x, so the "Light radius" slider of the Forward+ panel did
    // nothing under DirectX 12 while in Vulkan it did.
    m_impl->lightRadii = radii;
}

D3D12Renderer::SlotUsage D3D12Renderer::slotUsage() const
{
    const Impl& d = *m_impl;
    SlotUsage u;
    // Live = entries of the vector minus those in the pool. The cap is the
    // descriptor heap layout (kSrvObjects + i * kSrvPerObject), not
    // a decision of the vector: past that number an object is drawn with the
    // global block and comes out flat.
    u.objects         = d.objects.size() - d.objectSlots.freeCount();
    u.objectCapacity  = kMaxObjectSlots;
    u.skinned         = d.skinnedObjects.size() - d.skinnedSlots.freeCount();
    u.skinnedCapacity = kMaxSkinnedSlots;
    return u;
}

void D3D12Renderer::tickDeferredDeletes()
{
    // Nothing pending: in this backend releases wait on the GPU at the
    // moment they are requested.
}

void D3D12Renderer::updateAnimation(int index, float deltaTime)
{
    Impl& d = *m_impl;
    if (index < 0 || static_cast<size_t>(index) >= d.skinnedObjects.size())
        return;

    // Advances THAT character's clock and keeps it inside the clip. The backend's
    // internal clock still exists for whoever does not call here, but for
    // this character it is turned off: they rule from outside.
    Impl::SkinnedObject& character = d.skinnedObjects[index];
    character.externalClock        = true;
    // The whole rule (pace, wrap and freezing the hidden one) lives in
    // advanceMeshClock, shared with Vulkan: written twice, the two
    // copies diverged (A13).
    character.animTime = advanceMeshClock(character.animTime, deltaTime, character.ticksPerSecond,
                                          character.animDuration, character.visible);
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

    // The buffers may be in use by the last presented frame: releasing them
    // with work in flight is silent corruption, not an API error.
    d.waitForGpu();

    // ALL are released at once, so the count is zeroed first: the
    // guard in releaseStaticObject protects the deletion of a SINGLE object, and here there
    // is nobody alive left that could still point at these buffers.
    for (Impl::StaticObject& object : d.objects)
        object.sharedRefs = object.ownsGpu ? 1 : 0;
    for (Impl::StaticObject& object : d.objects)
        d.releaseStaticObject(object);

    d.objects.clear();
    d.objectSlots.clear();   // the slots no longer exist: the vector is empty
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
    // Bounded as in Vulkan: out of range, clip 0 (see clampClipIndex).
    character.clipBase             = clampClipIndex(clipIndex, character.clipCount) * character.boneCount;
    character.animTime             = animTime;
    // A single sample from here on: an Animator's pose is sent again by
    // setAnimationPose every frame if needed.
    character.hasPose              = false;
    // The GameObject's Animator owns the clock: the backend does not add
    // time to it again on its own.
    character.externalClock = true;
}

void D3D12Renderer::setAnimationIk(int index, const AnimationIk& ik)
{
    Impl& d = *m_impl;
    if (index < 0 || static_cast<size_t>(index) >= d.skinnedObjects.size())
        return;
    Impl::SkinnedObject& character = d.skinnedObjects[index];
    character.ik = ik;
    // The indices come from the Animator, resolved against the SAME skeleton:
    // only what does not fit in the SSBO is filtered.
    for (int k = 0; k < character.ik.count; k++)
        if (character.ik.solves[k].bone >= static_cast<int>(character.boneCount))
            character.ik.solves[k].bone = -1;
}

void D3D12Renderer::setAnimationPose(int index, const AnimationPose& pose)
{
    Impl& d = *m_impl;
    if (index < 0 || static_cast<size_t>(index) >= d.skinnedObjects.size())
        return;
    Impl::SkinnedObject& character = d.skinnedObjects[index];
    character.pose    = pose;
    character.hasPose = true;
    // Each layer's mask is copied: the Animator's is only valid during this
    // call. recordSkinning re-points the pose at these copies.
    for (int L = 0; L < kMaxLayersPose; L++) {
        const std::vector<uint8_t>* m = (L < pose.layerCount) ? pose.layers[L].mask : nullptr;
        if (m) character.poseMasks[L] = *m; else character.poseMasks[L].clear();
        character.pose.layers[L].mask = nullptr;
    }
    // The Animator owns the clock: the backend does not add time on its own.
    character.externalClock = true;
    // Bounded as in Vulkan: an out-of-range clip would read another block.
    int masPesada = -1;
    for (int k = 0; k < character.pose.count; k++) {
        character.pose.samples[k].clip = static_cast<int>(
            clampClipIndex(static_cast<uint32_t>((std::max)(0, character.pose.samples[k].clip)), character.clipCount));
        if (masPesada < 0 || character.pose.samples[k].weight > character.pose.samples[masPesada].weight)
            masPesada = k;
    }
    // The rest of the backend still looks at a single clip: the one with the most weight.
    if (masPesada >= 0) {
        character.clipBase = static_cast<uint32_t>(character.pose.samples[masPesada].clip) * character.boneCount;
        character.animTime = character.pose.samples[masPesada].time;
    }
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

    // Its buffers may be in use by the last presented frame, and the deformed
    // vertex one is written by that same frame's compute.
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
    // The frame's one, without the TAA offset: whoever unprojects a click
    // wants the camera, not the sampling noise.
    return m_impl->cameraProj() * m_impl->cameraView;
}

void D3D12Renderer::rebuildSkinnedMesh(int index, const SkinnedMesh& mesh)
{
    Impl& d = *m_impl;
    if (index < 0 || index >= static_cast<int>(d.skinnedObjects.size()))
        return;

    // Slot, transform and animation state are kept: the GameObject's render
    // index cannot move, and whoever rebuilds it (adding or
    // removing clips) does not expect the character to jump to the initial pose.
    const Impl::SkinnedObject previous = d.skinnedObjects[index];

    // createSkinnedObject recycles a free slot when there is one, so the new
    // object is at `created`, not necessarily at back(): popping back() took
    // ANOTHER live character. The temporary slot is handed back either way.
    const size_t sizeBefore = d.skinnedObjects.size();
    const int created = d.createSkinnedObject(mesh);
    if (created < 0)
        return;

    Impl::SkinnedObject rebuilt = std::move(d.skinnedObjects[static_cast<size_t>(created)]);
    if (d.skinnedObjects.size() > sizeBefore) {
        d.skinnedObjects.pop_back();
    } else {
        Impl::SkinnedObject& temp = d.skinnedObjects[static_cast<size_t>(created)];
        temp          = Impl::SkinnedObject{};
        temp.visible  = false;
        temp.slotFree = true;
        d.skinnedSlots.release(created);
    }

    rebuilt.transform   = previous.transform;
    rebuilt.visible     = previous.visible;
    rebuilt.ssrStrength = previous.ssrStrength;
    rebuilt.animTime    = previous.animTime;
    rebuilt.clipBase    = previous.clipBase;

    // The old resources may be in use by the last presented frame.
    d.waitForGpu();
    // Through the caches: the textures and geometry are SHARED with `rebuilt`
    // (same key), and releasing them raw freed what the new one uses.
    Impl::SkinnedObject& slot = d.skinnedObjects[index];
    d.releaseSkinnedResources(slot);
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

        const std::shared_ptr<const Mesh> mesh = child->getMesh();
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

    // The slots are NOT compacted: the render indices of the other objects
    // are recorded in their GameObjects, and moving them would leave all of them pointing at
    // another mesh. What IS done is RELEASING their resources and returning the slot
    // to the pool so the next addition can reuse it (H32, H43). Before, this only
    // set meshVisible to false and the VRAM stayed until the scene was closed.
    //
    // One wait for the whole subtree, not one per child: releasing resources with
    // work in flight is silent corruption, not an error the API reports.
    // And deleting is an editor action, not something per frame, so the stall is
    // not noticeable. Same criterion as clearStaticMeshes.
    m_impl->waitForGpu();
    node->traverse([this](GameObject* child) {
        if (!child)
            return;
        if (child->staticRenderIndex >= 0) {
            m_impl->releaseObjectSlot(static_cast<size_t>(child->staticRenderIndex));
            child->staticRenderIndex = -1;
        }
        if (child->skinnedRenderIndex >= 0) {
            m_impl->releaseSkinnedSlot(static_cast<size_t>(child->skinnedRenderIndex));
            child->skinnedRenderIndex = -1;
        }
    });
}

void D3D12Renderer::removeMeshComponent(GameObject* node)
{
    // Same guard as Renderer::removeMeshComponent (Vulkan): the signal is
    // hasMesh(), not the GPU slots. An object with a mesh and no slot (the
    // async load still in flight, or past kMaxObjectSlots) also
    // has to lose the component; before, it left through the return below with
    // hasMesh() intact.
    if (!node || !node->hasMesh())
        return;
    // The waitForGpu only if there really is a slot to release: stopping the GPU
    // in order to release nothing is not justified.
    if (node->staticRenderIndex >= 0 || node->skinnedRenderIndex >= 0) {
        m_impl->waitForGpu();
        if (node->staticRenderIndex >= 0) {
            m_impl->releaseObjectSlot(static_cast<size_t>(node->staticRenderIndex));
            node->staticRenderIndex = -1;
        }
        if (node->skinnedRenderIndex >= 0) {
            m_impl->releaseSkinnedSlot(static_cast<size_t>(node->skinnedRenderIndex));
            node->skinnedRenderIndex = -1;
        }
    }
    // Parity with Vulkan, and the only thing that makes removing the component mean
    // something outside the GPU: without this hasMesh() stayed true, the Mesh section
    // kept being drawn with its name and textures, loadMeshForSelected refused
    // to load a replacement and the Log said "Mesh component removed" anyway.
    // The other caller (ContentBrowserPanel, when deleting the asset in use) was left
    // with the mesh of a file that no longer exists on disk.
    node->setMesh(nullptr);
}

void D3D12Renderer::replaceStaticTextureWithMissing(int renderIndex, TextureSlot slot)
{
    Impl& d = *m_impl;
    if (renderIndex < 0 || renderIndex >= static_cast<int>(d.objects.size()))
        return;

    Impl::StaticObject& object = d.objects[renderIndex];
    if (object.srvBase == kSrvBaseColor)
        return;  // no block of its own: it draws with the global neutrals

    // Its block stops saying the same as those sharing this mesh,
    // so it can no longer share a draw with them.
    ++object.materialVariant;
    d.drawGroupsDirty = true;

    // The neutral that already exists for each slot. It is not the magenta checkerboard of the
    // Vulkan path, but it leaves the object visible instead of with garbage, which
    // is what matters when a texture could not be read.
    switch (slot) {
        case TextureSlot::Diffuse:
            // Here it is ALWAYS a failure (that is why this function is called), so
            // the checkerboard goes and not the white neutral.
            d.createTexture2DSrv(d.missingTextureAllocation
                                     ? d.missingTextureAllocation->GetResource()
                                     : d.baseColorAllocation->GetResource(),
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

void D3D12Renderer::rebuildStaticMesh(int index, const Mesh& mesh)
{
    Impl& d = *m_impl;
    if (index < 0 || index >= static_cast<int>(d.objects.size()))
        return;

    Impl::StaticObject& object = d.objects[static_cast<size_t>(index)];
    // Recycled slot, or an already retired owner that only waits for the last
    // duplicate to die: in both cases the GameObject that could have asked for
    // this no longer exists, and its resources are still being drawn by others.
    //
    // The two exits here carry diagLog for the same reason as the other two of
    // this function: they are no-ops, the user asked for a material change and
    // will not see any, and without a line not even a trace remains of why.
    if (object.slotFree || object.pendingRelease) {
        diagLog("rebuildStaticMesh: slot " + std::to_string(index) +
                (object.slotFree ? " is free" : " belongs to an owner already retired") +
                ", so there is nobody to change the material on; nothing is touched.");
        return;
    }
    if (object.srvBase == kSrvBaseColor) {
        // Really reachable: past kMaxObjectSlots an object is drawn with
        // the global neutrals and has no descriptor block of its own to
        // write. In Vulkan the same change IS visible, so without this line
        // the difference between backends has no explanation anywhere.
        diagLog("rebuildStaticMesh: object " + std::to_string(index) +
                " has no descriptor block of its own (past the limit of " +
                std::to_string(kMaxObjectSlots) +
                " slots), so it draws with the global neutral ones and its material cannot be "
                "changed on this backend.");
        return;
    }

    // `ownsGpu` is the ONLY ownership flag there is, and it applies to all five
    // allocations at once: the `reusa` branch of addStaticMesh also copies the
    // THREE texture ones of the owner, and releaseStaticObject releases them in the same
    // loop as the buffers. There is no field saying "the geometry is
    // borrowed but these textures are mine".
    //
    // Everything below follows from that: for the new textures to have someone who
    // releases them, an object that shares a mesh has to split off from the group
    // with its own copy of EVERYTHING, which is what Vulkan's general path does.
    // And splitting off requires uploading the geometry again, so without it nothing is
    // touched: better to do nothing than to leave the object half done.
    const bool comparte = !object.ownsGpu || object.sharedRefs > 1;
    if (comparte && (mesh.vertices.empty() || mesh.indices.empty())) {
        // Without a line here, the user changes the texture and nothing happens.
        diagLog("rebuildStaticMesh: object " + std::to_string(index) +
                " shares a mesh and the mesh arrives without geometry, so it cannot be split "
                "from the group; it stays as it was.");
        return;
    }

    // Hands ownership of the shared mesh to the first duplicate and redirects
    // the others to it. -1 if the count says there are duplicates but none
    // shows up: then nothing is touched, because handing out ownership of a native resource
    // blindly is exactly what ends in a double free.
    auto cederPropiedad = [&](int duenyo) -> int {
        Impl::StaticObject& viejo = d.objects[static_cast<size_t>(duenyo)];

        // The key is copied to a local BEFORE touching anything: assigning a
        // std::string allocates memory and may throw, and doing it with the
        // ownership already moved would leave both objects marked as owners of the
        // SAME five allocations, which is a double free at teardown.
        const std::string claveDelDuenyo = viejo.sharedKey;

        int elegido = -1;
        for (size_t i = 0; i < d.objects.size(); ++i) {
            Impl::StaticObject& otro = d.objects[i];
            if (static_cast<int>(i) == duenyo || otro.ownsGpu || otro.slotFree ||
                otro.sharedMesh != duenyo)
                continue;
            if (elegido < 0) {
                otro.sharedKey  = claveDelDuenyo;  // the only thing here that can throw
                elegido         = static_cast<int>(i);
                otro.sharedMesh = elegido;
                // The count does NOT change when handing over: it is still the same
                // objects pointing at the same mesh (the one leaving included,
                // which keeps drawing it until it splits off), the only thing that
                // moves is who releases it.
                otro.sharedRefs = viejo.sharedRefs;
            } else {
                otro.sharedMesh = elegido;
            }
        }
        if (elegido < 0)
            return -1;
        // The map has to point at the new owner: if it kept pointing here,
        // the next object with this key would copy the pointers of one that no longer
        // owns anything and which is about to change them on top of that.
        if (!claveDelDuenyo.empty()) {
            auto it = d.sharedMeshOwner.find(claveDelDuenyo);
            if (it != d.sharedMeshOwner.end() && it->second == duenyo)
                it->second = elegido;
        }
        viejo.sharedKey.clear();
        viejo.sharedRefs = 0;
        viejo.sharedMesh = elegido;

        // The ownership transfer, in two consecutive lines and the LAST ones of the
        // block: nothing that can fail remains between them, so there is not
        // even an instant with two objects marked as owners of the same thing.
        d.objects[static_cast<size_t>(elegido)].ownsGpu = true;
        viejo.ownsGpu                                   = false;
        return elegido;
    };

    if (object.ownsGpu && object.sharedRefs > 1) {
        // The duplicates draw THESE resources and their SRVs point to them:
        // releasing them here is not flagged by any validation, it is paid for with the lost
        // device later. The whole mesh is handed over to them and this object becomes
        // one more of those that have it borrowed.
        if (cederPropiedad(index) < 0) {
            // The count says there are duplicates and none shows up: the
            // bookkeeping is broken. It exits touching NOTHING (neither the material
            // variant nor the GPU), but it is logged, which is exactly what one
            // would want to see the day it happens.
            diagLog("rebuildStaticMesh: object " + std::to_string(index) + " claims to have " +
                    std::to_string(object.sharedRefs) +
                    " references to its mesh but no duplicate shows up; nothing is touched.");
            return;
        }
    }

    // Its block stops saying the same as those sharing this mesh,
    // so it can no longer share a draw with them.
    ++object.materialVariant;
    d.drawGroupsDirty = true;

    // The old resources may be in use by the last presented frame, and
    // in addition the uploads below reset this frame's command allocator. A single wait covers
    // both things: what is uploaded afterwards waits again internally, so when releasing
    // the GPU is still stopped.
    d.waitForGpu();

    // ── First EVERYTHING that can throw ───────────────────────────────────
    // uploadBuffer and uploadTexture are full of throwIfFailed (OOM, device
    // lost). Until they have succeeded not a single field of the
    // object is touched nor anything released. Doing it on the fly (detaching from the owner
    // and then uploading) leaves, if the upload throws, a VISIBLE object with
    // buffers pointing at a just-released allocation and with sharedMesh at a
    // slot already recycled: that is not a leak, it is freed memory in the
    // draw pass. It is the same caution as addStaticMesh, which builds a local
    // StaticObject and only inserts it when it is already complete.
    //
    // A nuance the previous comment kept quiet about: "nothing is touched" holds for
    // the object's FIELDS and for releasing resources, not for everything (cederPropiedad,
    // ++materialVariant and the views written by the uploads have already run by
    // this point). The first is reversible without resources in between (it leaves the
    // object drawing the borrowed one, which is still alive); the views are repaired
    // by the catch below.
    const bool seSepara = !object.ownsGpu;
    const UINT slot     = object.srvBase;

    D3D12MA::Allocation* nuevosVertices = nullptr;
    D3D12MA::Allocation* nuevosIndices  = nullptr;
    D3D12MA::Allocation* nuevoColor     = nullptr;
    D3D12MA::Allocation* nuevaNormal    = nullptr;
    D3D12MA::Allocation* nuevoOrm       = nullptr;
    std::string          nuevaClave;
    bool                 claveHonesta = false;

    // The five allocations are local until the commit block, so if
    // an upload throws midway NOBODY releases them: the object keeps its
    // previous ones and these are lost until the process closes. With the catch
    // they are released right here. Mind the order inside the catch: uploadTexture has already
    // written the view of what has just been released, so the three slots have to be returned
    // to the global neutrals BEFORE leaving, otherwise the leak
    // turns into something worse, a descriptor pointing at freed memory that the
    // draw pass reads in the next frame.
    try {
        if (seSepara) {
            nuevosVertices =
                d.uploadBuffer(mesh.vertices.data(), mesh.vertices.size() * sizeof(Vertex),
                               D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            nuevosIndices =
                d.uploadBuffer(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t),
                               D3D12_RESOURCE_STATE_INDEX_BUFFER);
        }

        // The new ones BEFORE releasing the old ones: this way no view in the block
        // ever points at an already released resource, not even if an upload throws.
        // And as a bonus, on leaving here the three slots point at new resources or at
        // the global neutrals, never at the BORROWED textures of the owner this
        // object is about to detach from.
        nuevoColor = d.uploadMaterialTexture(mesh.material.texturePath,
                                             mesh.material.embeddedTexture, TextureKind::BaseColor,
                                             slot + 0);
        if (!nuevoColor) {
            // It asked for it and it could not be read: checkerboard, so it shows. It did not ask: white.
            const bool sePidio =
                chooseTextureSource(mesh.material.texturePath, mesh.material.embeddedTexture) !=
                TextureSource::None;
            ID3D12Resource* relleno = (sePidio && d.missingTextureAllocation)
                                          ? d.missingTextureAllocation->GetResource()
                                          : d.baseColorAllocation->GetResource();
            d.createTexture2DSrv(relleno, DXGI_FORMAT_R8G8B8A8_UNORM, slot + 0);
        }

        nuevaNormal = d.uploadMaterialTexture(mesh.material.normalMapPath,
                                              mesh.material.embeddedNormalMap, TextureKind::Normal,
                                              slot + 1);
        if (!nuevaNormal)
            d.createTexture2DSrv(d.normalMapAllocation->GetResource(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                 slot + 1);

        // The own ORM goes in +3, overwriting the neutral that fillSharedSlots left; without
        // it, that neutral is put back. The other shared slots (+2 and
        // +4..+6, since the block is kSrvPerObject = 7 slots) are NOT touched: the
        // reflection probe carried by this object is its own and filling them in again
        // would swap it for the global one.
        nuevoOrm = d.uploadMaterialTexture(mesh.material.metallicRoughnessPath,
                                           mesh.material.embeddedMetallicRoughness,
                                           TextureKind::Orm, slot + 3);
        if (!nuevoOrm)
            d.createTexture2DSrv(d.metalRoughAllocation->GetResource(),
                                 DXGI_FORMAT_R8G8B8A8_UNORM, slot + 3);

        // The new key and the prefix comparison, also up here: both
        // build std::string (`makeSharedMeshKey` a whole one, `prefijoGeometria`
        // a substr), and a bad_alloc down there would happen AFTER the object
        // already has the new material but BEFORE removing the old entry from the map
        // (which would stay saying that this key describes this object, which is
        // exactly the lie the re-keying exists to erase).
        //
        // The first two fields of the key are, in plain text and separated by '|', the
        // vertex count and the index count: makeSharedMeshKey puts them first
        // as exact discriminators, so comparing that prefix says whether a
        // key describes the geometry that is REALLY in VRAM without rehashing the
        // mesh. The helper is the same one used by Renderer::rebuildStaticMesh.
        auto prefijoGeometria = [](const std::string& clave) {
            const size_t primera = clave.find('|');
            if (primera == std::string::npos)
                return clave;
            // npos = no second '|' (a key that did not come from makeSharedMeshKey):
            // the whole string is compared, which is the conservative side.
            return clave.substr(0, clave.find('|', primera + 1));
        };

        nuevaClave = makeSharedMeshKey(mesh);

        // If it split off, the geometry was just uploaded FROM `mesh` and the new
        // key describes it by construction. If the change was in place the old one has to
        // be asked: that path does NOT re-upload geometry, and the contract
        // says that changing it through here is not supported but nothing prevents it.
        claveHonesta = seSepara || (!object.sharedKey.empty() &&
                                    prefijoGeometria(object.sharedKey) ==
                                        prefijoGeometria(nuevaClave));
        if (!claveHonesta)
            diagLog("rebuildStaticMesh: object " + std::to_string(index) +
                    " receives a mesh with different geometry (the contract says it does not "
                    "change here). Only its material is changed and it leaves the dedup map: it keeps "
                    "drawing the geometry it has in VRAM.");
    } catch (...) {
        for (D3D12MA::Allocation* nueva :
             {nuevosVertices, nuevosIndices, nuevoColor, nuevaNormal, nuevoOrm})
            if (nueva)
                nueva->Release();

        // The three slots go back to the GLOBAL neutrals and not to the textures from
        // before: object.*Allocation is still alive, but one of those was created with
        // an _SRGB format and a UNORM view over a resource typed as sRGB is not
        // valid in D3D12 (the resource would have to be typeless), so
        // rebuilding the previous view from here cannot be done without
        // also carrying the format each one was created with. The neutrals are
        // UNORM and always valid: the object is left without its textures until the
        // next rebuild, which is a poor but safe outcome for something
        // that only happens with OOM or a lost device.
        d.createTexture2DSrv(d.baseColorAllocation->GetResource(), DXGI_FORMAT_R8G8B8A8_UNORM,
                             slot + 0);
        d.createTexture2DSrv(d.normalMapAllocation->GetResource(), DXGI_FORMAT_R8G8B8A8_UNORM,
                             slot + 1);
        d.createTexture2DSrv(d.metalRoughAllocation->GetResource(), DXGI_FORMAT_R8G8B8A8_UNORM,
                             slot + 3);
        diagLog("rebuildStaticMesh: an upload of object " + std::to_string(index) +
                " threw; the half-made allocations are released and its three texture "
                "slots go back to the global neutral ones.");
        throw;
    }

    // ── And now with nothing that can throw, the state change ─────────────
    if (seSepara) {
        // It keeps the own copy of the geometry and its box, and only
        // THEN detaches from the owner: until this line the object was still
        // drawing the borrowed one, which is the only thing that can be drawn without
        // risk while the uploads may fail.
        const int duenyo = object.sharedMesh;

        object.vertexAllocation = nuevosVertices;
        object.vertexBufferView.BufferLocation =
            nuevosVertices->GetResource()->GetGPUVirtualAddress();
        object.vertexBufferView.SizeInBytes =
            static_cast<UINT>(mesh.vertices.size() * sizeof(Vertex));
        object.vertexBufferView.StrideInBytes = sizeof(Vertex);

        object.indexAllocation                = nuevosIndices;
        object.indexBufferView.BufferLocation = nuevosIndices->GetResource()->GetGPUVirtualAddress();
        object.indexBufferView.SizeInBytes =
            static_cast<UINT>(mesh.indices.size() * sizeof(uint32_t));
        object.indexBufferView.Format = DXGI_FORMAT_R32_UINT;
        object.indexCount             = static_cast<UINT>(mesh.indices.size());

        // The box is recomputed FROM `mesh`, which is where the vertices
        // have just come from: the rule is that whoever uploads the geometry computes its
        // box, just like in addStaticMesh. Inheriting the owner's would work
        // while the geometry was the same (the contract says it does not change
        // through here) but nothing prevents it and then the culling would lie.
        // Parentheses around the name: windows.h defines max as a macro.
        glm::vec3 lo((std::numeric_limits<float>::max)());
        glm::vec3 hi(std::numeric_limits<float>::lowest());
        for (const Vertex& v : mesh.vertices) {
            lo = (glm::min)(lo, v.pos);
            hi = (glm::max)(hi, v.pos);
        }
        object.aabbMin   = lo;
        object.aabbMax   = hi;
        object.hasBounds = true;

        object.sharedMesh = index;
        object.ownsGpu    = true;
        object.sharedRefs = 1;  // itself

        // The three texture ones were copies of the owner's: they are forgotten WITHOUT
        // releasing them, so the loop below does not see them.
        object.baseColorAllocation  = nullptr;
        object.normalMapAllocation  = nullptr;
        object.metalRoughAllocation = nullptr;

        // Now yes: one reference less to the owner, and finish it off if this was the
        // last one keeping it alive, just like releaseObjectSlot does.
        if (duenyo >= 0 && duenyo < static_cast<int>(d.objects.size())) {
            Impl::StaticObject& previo = d.objects[static_cast<size_t>(duenyo)];
            --previo.sharedRefs;
            if (previo.pendingRelease && previo.sharedRefs <= 0) {
                // releaseObjectSlot decrements before comparing, so its
                // count is left at 1 so that it drops to 0 and releases.
                previo.sharedRefs     = 1;
                previo.pendingRelease = false;
                d.releaseObjectSlot(static_cast<size_t>(duenyo));
            }
        }
    }

    // Whatever remains in the three pointers belongs to this object and nobody else
    // looks at it: either it was the sole owner from the start, or it just nulled them because
    // they were borrowed.
    for (D3D12MA::Allocation** vieja : {&object.baseColorAllocation, &object.normalMapAllocation,
                                        &object.metalRoughAllocation}) {
        if (*vieja)
            (*vieja)->Release();
        *vieja = nullptr;
    }

    object.baseColorAllocation  = nuevoColor;
    object.normalMapAllocation  = nuevaNormal;
    object.metalRoughAllocation = nuevoOrm;
    // Same criterion as addStaticMesh (and as Vulkan): with an ORM map, the map
    // rules and both factors are forced to 1.0.
    const bool tieneMapaOrm = chooseTextureSource(mesh.material.metallicRoughnessPath,
                                                   mesh.material.embeddedMetallicRoughness) !=
                              TextureSource::None;
    object.metallic  = tieneMapaOrm ? 1.0f : mesh.material.metallic;
    object.roughness = tieneMapaOrm ? 1.0f : mesh.material.roughness;
    object.hasOrmMap = tieneMapaOrm;

    // The old entry ALWAYS goes, honest or not: the map is keyed by
    // content AND material (makeSharedMeshKey puts in the texture paths and the
    // PBR factors), so it describes a material this object no longer has.
    // Leaving it would make the next object with the OLD material copy these
    // pointers and come out with the new material without having asked for it.
    if (!object.sharedKey.empty()) {
        auto it = d.sharedMeshOwner.find(object.sharedKey);
        if (it != d.sharedMeshOwner.end() && it->second == index)
            d.sharedMeshOwner.erase(it);
        object.sharedKey.clear();
    }
    // And it is entered with the new one if it describes what is in VRAM and there is no owner yet
    // for it: the map only admits one per key and whoever arrived first
    // stays. This `emplace` is the only thing left down here that can throw, and
    // it is left on purpose for the end: since the `erase` has already run, if it
    // fails it only loses dedup (the object keeps a private copy) and it never
    // leaves a key lying, which is the only unacceptable outcome.
    if (claveHonesta && d.sharedMeshOwner.emplace(nuevaClave, index).second)
        object.sharedKey = nuevaClave;

    // Careful when comparing this file with its Vulkan sibling: given the SAME
    // input (a sole owner receiving a mesh with other geometry) the two
    // backends are safe but do NOT do the same, and neither of the two has
    // a bug there.
    //
    //  - Vulkan sends that case to the general path (`separarAEntradaPropia`), which
    //    recreates the whole entry: it ends up drawing the new geometry and
    //    keeping dedup.
    //  - Here the prefix mismatch only suppresses the re-keying: the object
    //    keeps drawing the geometry it already had in VRAM and stays outside the
    //    map. The diagLog above makes it explicit.
    //
    // The divergence can be lived with because the contract of
    // EditorRenderer::rebuildStaticMesh declares that input NOT supported (to
    // change vertices the object has to be registered again), and what both backends
    // do share is what matters: neither leaves the dedup map saying that a key describes
    // a geometry that the entry does not have, which is the silent and SHARED corruption that
    // this block exists to close.
}

void D3D12Renderer::initSkybox(const std::array<std::string, 6>& facePaths)
{
    Impl& d = *m_impl;
    if (d.skyboxFacePaths == facePaths)
        return;  // it is already the one that is set up

    d.skyboxFacePaths = facePaths;

    // Before init() it is enough to record them: createSkyboxResources picks them up.
    if (!d.initialized)
        return;

    // And if a sky is already set up, it is swapped live. The cubemap may
    // be in the in-flight frame, and it is also the source of the global IBL, which
    // has to be re-convolved afterwards.
    d.waitForGpu();
    if (d.loadSkyboxCubemap()) {
        if (!d.skyboxPipeline)
            d.createSkyboxPipelineOnly();
        d.precomputeIbl();
    }
}

void D3D12Renderer::setShadowResolution(int v)
{
    if (v <= 0 || v == shadowResolution()) return;
    setShadowResolutionFlag(v);
    // It is recorded and drawFrame applies it between frames (applyPendingShadowSize).
    m_impl->pendingShadowMapSize = static_cast<UINT>(v);
}

void D3D12Renderer::setPresentMode(PresentMode v)
{
    // There is nothing to recreate here, unlike in Vulkan: DXGI chooses the mode
    // on every Present, so it is enough to store the value and the next frame
    // already comes out with it. The tearing flag does go in the swapchain, but it is requested
    // ALWAYS when the adapter allows it, not only when the mode is active.
    setPresentModeFlag(v);
}

bool D3D12Renderer::presentModeSupported(PresentMode v) const
{
    switch (v) {
        case PresentMode::Vsync:     return true;
        // DXGI has no mailbox equivalent: the flip model drops or queues,
        // but there is no mode that neither waits nor tears the image. It says no
        // instead of pretending yes and silently falling back to vsync.
        case PresentMode::Mailbox:   return false;
        case PresentMode::Immediate: return m_impl->tearingDisponible;
    }
    return false;
}

void D3D12Renderer::setBloomEnabled(bool v)
{
    setBloomEnabledFlag(v);
}

void D3D12Renderer::flushPendingUploads()
{
    // Nothing pending: each upload is submitted and waited on at once.
}

void D3D12Renderer::flushUploadsAndWait()
{
    m_impl->waitForGpu();
}

void D3D12Renderer::refitCameraRange()
{
    Impl& d = *m_impl;

    // Floor of the range: a tiny scene (or with everything at the same point) would give
    // far ~0 and not even the sky would be seen. 200 leaves far=600 and near=0.2, which covers
    // the camera the editor opens with without clipping small props. Same
    // value as the Vulkan path, so both give the same framing.
    constexpr float kMinCameraDistance = 200.0f;

    // Parentheses around min/max: windows.h defines them as macros.
    glm::vec3 lo((std::numeric_limits<float>::max)());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    bool      any = false;

    for (const Impl::StaticObject& object : d.objects) {
        if (!object.hasBounds)
            continue;

        // The 8 corners of the local AABB taken to world: with the object
        // rotated or scaled, the mesh's axis-aligned box no longer bounds it.
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

    // Of the characters only their origin goes in: a bound of the pose is not stored
    // here as in Vulkan, and this only sets near/far, it culls nothing. It serves
    // so that a scene made only of characters does not end up without range.
    for (const Impl::SkinnedObject& character : d.skinnedObjects) {
        const glm::vec3 origin(character.transform[3]);
        lo  = (glm::min)(lo, origin);
        hi  = (glm::max)(hi, origin);
        any = true;
    }

    // Nothing boundable: it keeps the current range instead of leaving it at infinities.
    // It is what happens with the empty scene of a newly created project.
    if (!any)
        return;

    const float maxDim = (glm::max)(hi.x - lo.x, (glm::max)(hi.y - lo.y, hi.z - lo.z));
    d.cameraDistance   = (glm::max)(maxDim * 1.2f, kMinCameraDistance);

    // The range has just changed shape: without this, the cascades would stay
    // split over the previous frustum until the next camera
    // movement.
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

// The OUTPUT size, which is what recordUiCanvas builds the canvas with.
// With SSAA `width`/`height` are larger: using those would leave the UI and the mouse
// in two different spaces.
uint32_t D3D12Renderer::uiWidth() const
{
    return m_impl->outWidth;
}

uint32_t D3D12Renderer::uiHeight() const
{
    return m_impl->outHeight;
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

    // The interface layer is recorded through the same slot that already existed for
    // ImGui: whoever sets it no longer has to register the callback by hand.
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
    // The FIRST screen canvas, in scene order: same criterion
    // as the Vulkan Renderer, and the same one used by the temporary shim (Task
    // 4) when there was only one canvas.
    for (auto& s : m_impl->uiSlots)
        if (s && s->mode == UiCanvasRenderMode::ScreenSpace) return s->canvas;
    return m_impl->uiCanvasFallback;
}

void D3D12Renderer::screenUiCanvases(std::vector<UiCanvas*>& out)
{
    // Same free function as Vulkan: the order of the UI pass (which walks
    // uiSlots in order) reversed, that is, the topmost first.
    screenCanvasesTopFirst(m_impl->uiSlots, out);
}

const UiCanvas* D3D12Renderer::uiCanvasOf(uint64_t ownerId) const
{
    // Same free function as Vulkan.
    return findCanvasByOwner(m_impl->uiSlots, ownerId);
}

void D3D12Renderer::syncUiCanvases(const std::vector<UiCanvasBinding>& bindings)
{
    Impl& d = *m_impl;

    // Matches by ownerId: surviving slots keep their tree and their
    // cache, so reordering canvases in the hierarchy does not rebuild what
    // has not changed.
    matchUiCanvasSlots(bindings, d.uiSlots);

    for (size_t i = 0; i < bindings.size(); i++)
    {
        UiCanvasSlot& s = *d.uiSlots[i];
        const UiCanvasBinding& b = bindings[i];
        if (b.canvas) b.canvas->applyTo(s.canvas);
        const UiCanvasRenderMode modo =
            b.canvas ? b.canvas->renderMode : UiCanvasRenderMode::ScreenSpace;
        // Same reason as in the Vulkan path: changing mode takes the canvas out
        // of the input split, and if it leaves mid-press it is left with an
        // orphaned capture that, when it comes back, steals the pointer from the one on top.
        if (modo != s.mode) s.canvas.releaseInput();
        s.mode      = modo;
        s.depthTest = b.canvas ? b.canvas->depthTest  : true;
        // Copy by value of the world settings and of the GameObject's
        // transform: the model matrix is computed when RECORDING (it needs the
        // camera view for the billboard) and by then the binding no longer
        // exists. Without a canvas the default component stays, which is never
        // read because the mode will be ScreenSpace.
        if (b.canvas) s.component = *b.canvas;
        s.worldTransform = b.worldTransform;
        syncUiWidgets(b.widgets, s.canvas, s.cache, *this);
    }
}

const UiElement* D3D12Renderer::findUiNode(const std::string& name) const
{
    // ALL canvases, not only the screen one.
    for (const auto& s : m_impl->uiSlots)
    {
        if (!s) continue;
        if (const UiElement* n = findUiNodeIn(s->canvas.root(), name)) return n;
    }
    return nullptr;
}

void D3D12Renderer::initSceneResources(const std::vector<Mesh>& meshes)
{
    // Phase 2 of startup for this backend: upload what is already there. The camera's
    // auto-fit and the resources that depend on the scene size are
    // resolved by init(), which has already run here.
    for (const Mesh& mesh : meshes)
        addStaticMesh(mesh);
    refitCameraRange();
}

void D3D12Renderer::drawFrame(Window& window)
{
    // The window is not needed: the size arrives through resize() from its callback.
    (void)window;
    drawFrame();
}

void D3D12Renderer::setHeadless(bool headless)
{
    m_impl->headless = headless;
}

void D3D12Renderer::notifyResize()
{
    // Vulkan only sets a flag because its swapchain recreates itself when the
    // present fails. Here the size comes from the window callback, which already calls
    // resize(): there is nothing left to do.
}

UiTextureAtlas* D3D12Renderer::loadUiAtlas(const std::string& path)
{
    Impl& d = *m_impl;
    if (!d.initialized)
        return nullptr;

    if (auto it = d.uiAtlasByPath.find(path); it != d.uiAtlasByPath.end())
        return it->second;

    auto atlas = std::make_unique<UiTextureAtlas>();
    if (!atlas->loadPixelsFromFile(path))
        return nullptr;
    // Sub-rects, if there are any: same sidecar and same rules as in Vulkan.
    atlas->loadSprites(UiTextureAtlas::spriteSheetPathFor(path));
    if (!d.registerUiAtlas(*atlas))
        return nullptr;

    d.uiAtlases.push_back(std::move(atlas));
    d.uiAtlasByPath[path] = d.uiAtlases.back().get();
    return d.uiAtlases.back().get();
}

uint64_t D3D12Renderer::uiAtlasTextureId(const UiTextureAtlas* atlas)
{
    Impl& d = *m_impl;
    if (!atlas || !d.srvHeap)
        return 0;

    // The SRV already exists: registerUiAtlas created it in the heap that the interface
    // shares with the backend, and its GPU handle IS what ImGui understands as a
    // texture (same criterion as EditorUI::registerUiTexture in D3D12).
    const auto it = d.uiAtlasSrv.find(atlas);
    if (it == d.uiAtlasSrv.end())
        return 0;

    D3D12_GPU_DESCRIPTOR_HANDLE handle = d.srvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(it->second) * d.srvSize;
    return handle.ptr;
}

uint64_t D3D12Renderer::uiThumbnailAtlasId()
{
    Impl& d = *m_impl;
    if (!d.ensureThumbAtlas())
        return 0;

    // Same criterion as uiAtlasTextureId: the SRV's GPU handle IS what
    // ImGui understands as a texture.
    D3D12_GPU_DESCRIPTOR_HANDLE handle = d.srvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(kSrvThumbAtlas) * d.srvSize;
    return handle.ptr;
}

bool D3D12Renderer::uploadUiThumbnails(const ThumbnailTile* tiles, size_t count)
{
    return m_impl->uploadThumbnailTiles(tiles, count);
}

UiFont* D3D12Renderer::loadUiFont(const std::string& path, float bakePx)
{
    Impl& d = *m_impl;
    if (!d.initialized)
        return nullptr;

    auto font = std::make_unique<UiFont>();
    // The baking is CPU: FreeType and MSDF know nothing of backends. The only thing specific
    // is uploading the atlas that comes out of there.
    if (!font->bakeFromFileCached(path, bakePx))
        return nullptr;
    if (!d.registerUiAtlas(font->atlas()))
        return nullptr;

    d.uiFonts.push_back(std::move(font));
    return d.uiFonts.back().get();
}

void D3D12Renderer::setAaMode(AaMode mode)
{
    // It is only recorded: the targets and pipelines are redone by the next frame,
    // with the GPU idle.
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
    // SSAA IS implemented in this backend (applyPendingRenderSize scales
    // the draw size and the resolve pass averages): the comment that
    // was here said the opposite and had been obsolete for a while.
    //
    // There is no need to mark anything: applyPendingRenderSize recomputes the requested
    // size every frame and exits through its early-out when it has not changed, so
    // touching the factor already causes the recreation in the next frame.
    setSsaaFactorFlag(v);
}

void D3D12Renderer::requestProbeBake(uint64_t ownerId)
{
    // It is only noted: baking rewrites the camera and waits on the GPU, so it is
    // done at the start of the next frame and not in the middle of whatever
    // the caller is doing.
    m_impl->probeBakeQueue.push_back(ownerId);
}

void D3D12Renderer::requestProbeBakeAll() { m_impl->probeBakeAllQueued = true; }
int   D3D12Renderer::probeCount() const { return static_cast<int>(m_impl->probes.size()); }
float D3D12Renderer::lastProbeBakeMs() const { return m_impl->probeLastBakeMs; }

uint64_t D3D12Renderer::probeMemoryBytes() const
{
    // rgba16f = 8 bytes per texel, 6 faces. Three resources per probe, and that is the
    // difference with Vulkan: there the CAPTURE cubemap is a single one for
    // all probes, here each one has its own (see createProbeResources), so
    // this backend's figure is larger. Showing Vulkan's under DX12 was
    // not just mixing backends: it underestimated (H51).
    constexpr uint64_t kBytesPorTexel = 8;
    constexpr uint64_t kCaras         = 6;

    uint64_t prefiltrado = 0;
    for (UINT mip = 0; mip < kIblPrefilterMips; ++mip) {
        const uint64_t lado = kIblPrefilterSize >> mip;
        prefiltrado += lado * lado * kCaras * kBytesPorTexel;
    }
    const uint64_t captura =
        (uint64_t)kProbeFaceSize * kProbeFaceSize * kCaras * kBytesPorTexel;
    const uint64_t irradiancia =
        (uint64_t)kIblIrradianceSize * kIblIrradianceSize * kCaras * kBytesPorTexel;

    return captura + irradiancia + prefiltrado;
}
float D3D12Renderer::probeBakeMs(uint64_t ownerId) const
{
    for (const Impl::GpuProbe& probe : m_impl->probes)
        if (probe.ownerId == ownerId)
            return probe.bakeMs;
    return 0.0f;
}

void D3D12Renderer::setPerfCaptureEnabled(bool on) { m_impl->perfCapture = on; }
// GPU times: measured by each pass's pair of marks, read two frames
// late (which is when the GPU has already finished the one that wrote them), just
// like in the Vulkan path.
float D3D12Renderer::renderGpuMs() const { return m_impl->gpuMs[Impl::TsFrame / 2]; }
float D3D12Renderer::ssaoGpuMs() const { return m_impl->gpuMs[Impl::TsSsao / 2]; }
float D3D12Renderer::ssrGpuMs() const { return m_impl->gpuMs[Impl::TsSsr / 2]; }
float D3D12Renderer::bloomGpuMs() const { return m_impl->gpuMs[Impl::TsBloom / 2]; }
float D3D12Renderer::fogGpuMs() const { return m_impl->gpuMs[Impl::TsFog / 2]; }
float D3D12Renderer::motionBlurGpuMs() const { return m_impl->gpuMs[Impl::TsMotionBlur / 2]; }
float D3D12Renderer::aaGpuMs() const { return m_impl->gpuMs[Impl::TsAa / 2]; }
float D3D12Renderer::sceneGpuMs() const { return m_impl->gpuMs[Impl::TsScene / 2]; }
float D3D12Renderer::shadowGpuMs() const { return m_impl->gpuMs[Impl::TsShadow / 2]; }
float D3D12Renderer::forwardPlusGpuMs() const { return m_impl->gpuMs[Impl::TsForwardPlus / 2]; }
int   D3D12Renderer::statDrawCalls() const { return m_impl->statDraws; }
int   D3D12Renderer::statInstances() const { return m_impl->statInstanced; }
// Objects the frustum left out this frame. Statics only: characters
// are always drawn (see the main pass).
int   D3D12Renderer::statCulled() const { return m_impl->statCulledCount; }
// Real measurements: the compute already wrote them, what was missing was bringing them
// back (see recordForwardPlusCull). Before they were wired to 0/0, so the
// panel said "0 lights per cell" and its overflowed-cells warning never
// fired under DirectX 12: a false diagnostic, which is worse than having none.
float    D3D12Renderer::forwardPlusAvgPerCell() const { return m_impl->fpAvgPerCell; }
uint32_t D3D12Renderer::forwardPlusOverflowCells() const { return m_impl->fpOverflowCells; }

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
    // It is only recorded: recreating targets requires the GPU idle, and drawFrame does that
    // when starting the next one. A collapsed panel gives zero and is ignored,
    // like the window being minimized.
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

    diagLog("shutdown(): starting.");
    // The device state BEFORE waiting on anyone: if it is already lost, the
    // wait below will never finish and the reason has to be asked
    // now, not later.
    if (d.device) {
        const HRESULT motivo = d.device->GetDeviceRemovedReason();
        if (motivo != S_OK)
            d.dumpDeviceRemoved("shutdown (device already lost on entry)", motivo);
    }

    // Nothing is released with work in flight: releasing a render target that the GPU
    // is still reading is silent corruption, not an API error.
    d.waitForGpu();

    if (d.fenceEvent != nullptr) {
        CloseHandle(d.fenceEvent);
        d.fenceEvent = nullptr;
    }

    d.releaseRenderTargets();

    // The suballocated resources go BEFORE the allocator: releasing the
    // allocator with live allocations is a leak that only shows up in the
    // ReportLiveObjects below.
    if (d.gridAllocation) {
        d.gridAllocation->Release();
        d.gridAllocation = nullptr;
    }
    d.gridVertexBufferView = {};
    d.gridVertexCount      = 0;

    // The UBOs are persistently mapped: they are unmapped before
    // releasing their allocation.
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

    // Scene geometry, before the allocator. Through the same function as
    // clearStaticMeshes, so the criterion of who releases lives in a single
    // place; here everything is closed, so the count is zeroed first. This
    // path ALSO released the ORM, which clearStaticMeshes left behind.
    for (Impl::StaticObject& object : d.objects)
        object.sharedRefs = object.ownsGpu ? 1 : 0;
    for (Impl::StaticObject& object : d.objects)
        d.releaseStaticObject(object);

    d.objects.clear();
    d.objectSlots.clear();
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
                              &d.shadowMapArrayAllocation, &d.bloomBlackAllocation,
                              &d.missingTextureAllocation}) {
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
    // The two of the statistics readback: the upload one is mapped, so
    // Unmap first, and the read one is a ComPtr that releases itself. Both
    // BEFORE allocator->Release(), like the rest of the suballocated.
    if (d.fpStatsZeros) {
        d.fpStatsZeros->GetResource()->Unmap(0, nullptr);
        d.fpStatsZeros->Release();
        d.fpStatsZeros       = nullptr;
        d.fpStatsZerosMapped = nullptr;
    }
    d.fpStatsMapped = nullptr;
    d.fpStatsReadback.Reset();
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

    // The capture depth does NOT go in releaseProbe: it is a SINGLE one,
    // shared by the six faces of all probes (createProbeDepth exits
    // through the door if it already exists), so releasing it there would kill it as soon as
    // the first probe was deleted while the others are still baking. Its place is
    // here, and it did not have one: it stayed alive when allocator->Release() closes the
    // allocator, and D3D12MA does assert() when destroyed with a non-empty block
    // (in Debug, abort() with exit 3 and a Windows window, without a dump). Same
    // failure and same outcome as that of the UI atlases further down.
    //
    // It only fires if the session got to bake a probe: it is bakeProbe that
    // calls createProbeDepth. A clean shutdown without probes proves nothing.
    if (d.probeDepthAllocation) {
        d.probeDepthAllocation->Release();
        d.probeDepthAllocation = nullptr;
    }

    // 2D UI buffers: they are mapped, so Unmap first.
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
    // The UI atlases (sprites and MSDF fonts, which go through the same
    // registerUiAtlas) go HERE, with the rest of the suballocated resources and not at the
    // end: their D3D12MA::Allocation had to be released before
    // allocator->Release() closed the allocator. It was not, and D3D12MA does
    // assert() when destroyed with a non-empty block: in Debug that is abort(),
    // that is, the editor dying on CLOSE with an exit code 3 and a
    // Windows window, without a dump and with nothing in the event viewer. A
    // PREEXISTING failure, from 545b9b8; the Vulkan backend already did it right
    // (Renderer.cpp, m_uiAtlases -> destroy).
    //
    // And BEFORE srvHeap.Reset(), by the same criterion with which Vulkan
    // releases the atlases before its descriptor pool. With a difference that
    // should not be confused: in D3D12 a descriptor is NOT destroyed (there is
    // no DestroyShaderResourceView, nor does it count references on the
    // resource), it is memory inside the heap and dies with it. So here there is
    // no slot to return: the only thing needed is for the GPU to be
    // stopped, and the waitForGpu() at the start already takes care of that.
    for (D3D12MA::Allocation* atlas : d.uiAtlasTextures) {
        if (atlas)
            atlas->Release();
    }
    d.uiAtlasTextures.clear();
    // The thumbnail atlas, with the other UI atlases and for the same reason: its
    // allocation has to be released before allocator->Release() or D3D12MA
    // asserts when destroyed (abort in Debug, exit code 3, no dump).
    if (d.thumbAtlas) {
        d.thumbAtlas->Release();
        d.thumbAtlas = nullptr;
    }
    d.thumbAtlasFailed = false;
    // Pointers and indices to what was just released: out before anyone
    // can request them again. uiAtlasSrv is the exact equivalent of Vulkan's
    // m_uiAtlasImGuiId (it is what uiAtlasTextureId reads), and
    // uiNextAtlasSlot goes back to zero so as not to hand out slots of a dead heap.
    d.uiAtlasSrv.clear();
    d.uiAtlasByPath.clear();
    // The CPU containers at the end: in D3D12 neither UiTextureAtlas nor UiFont
    // keep anything of the GPU (their destroy(GpuDevice&) belongs to the Vulkan path),
    // so the only thing they had of the GPU is the allocation just released.
    d.uiAtlases.clear();
    d.uiFonts.clear();
    d.uiNextAtlasSlot = 0;

    d.uiPipeline.Reset();
    d.uiWorldPipelineDepth.Reset();
    d.uiWorldPipelineNoDepth.Reset();
    d.uiRootSignature.Reset();

    d.fxaaPipeline.Reset();
    d.fxaaRootSignature.Reset();
    // SSAA had been left out of this list, and that was the whole leak of
    // H78: of the backend's 20 root signatures and 36 PSOs, these two were the
    // ONLY ones without a Reset in the whole file. They came out as
    // `Live ID3D12RootSignature: 1` + `ID3D12PipelineState: 1`, and their two
    // references were what left the device at Refcount 2.
    //
    // The list is written by hand and has 56 entries: forgetting one raises no error
    // or visible symptom, it only dirties the leak report. That is why it matters that
    // this report comes out CLEAN: as soon as it tolerates noise, it stops warning.
    d.ssaaPipeline.Reset();
    d.ssaaRootSignature.Reset();
    d.splashPipeline.Reset();
    d.splashRootSignature.Reset();
    if (d.splashLogo) {
        d.splashLogo->Release();
        d.splashLogo = nullptr;
    }
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
        // What remains alive in the allocator RIGHT BEFORE releasing it. D3D12MA
        // does assert() if anything is still allocated, and an assert in Debug takes
        // the process down with a plain "exit code 3": the JSON here
        // is the only thing that says WHAT was left unreleased.
        WCHAR* stats = nullptr;
        d.allocator->BuildStatsString(&stats, TRUE);
        if (stats) {
            diagLog("--- D3D12MA state before Release() ---");
            diagLog(narrow(stats));
            diagLog("--------------------------------------------");
            d.allocator->FreeStatsString(stats);
        }
        d.allocator->Release();
        d.allocator = nullptr;
    }

    // The last thing the layer said, before releasing the queue with the device.
    d.drainInfoQueue();
    d.infoQueue.Reset();
    // The hook points at this Impl: leaving it set after releasing the device would be
    // asking a half-dead object.
    g_volcarDeviceRemoved = nullptr;

    // GPU time measurement. It was not released, so the ReportLiveObjects
    // below came out dirty ALWAYS and stopped serving the only thing it is for:
    // detecting the leak of the day (H46). The readback also stays mapped from
    // when it is created (on purpose, it is read every frame), and it has to be unmapped before
    // releasing it.
    if (d.timestampReadback) {
        d.timestampReadback->Unmap(0, nullptr);
        d.timestampMapped = nullptr;
    }
    d.timestampReadback.Reset();
    d.timestampHeap.Reset();

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
    diagLog("shutdown(): finished without incidents.");

#ifndef NDEBUG
    // With the device already released, whatever is still alive is a leak of ours. It comes out through
    // the debug window (DebugView / the debugger's output), which is
    // where the validation layer also writes.
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
