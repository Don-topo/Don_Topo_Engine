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

// DirectX 12 presentation backend.
//
// SCOPE TODAY: parity with the Vulkan path. Static meshes and meshes
// animated by compute, PBR materials, IBL, cascaded and cubemap shadows,
// SSAO, SSR, volumetric fog, bloom, motion blur, anti-aliasing (FXAA,
// SSAA, MSAA and TAA), Forward+, reflection probes, selection outline and
// UI. What it does NOT have is in the parity table in docs/renderer-audit.md.
//
// (This block used to say "presents a background color and nothing else" long after
// that stopped being true: H47. The header is the first thing read to
// decide whether an effect exists, so lying here costs more than in the
// .cpp.)
//
// The DX12 state lives in a hidden Impl so this header does not drag
// d3d12.h into everyone that includes the engine; that is why the libs are PRIVATE in the
// CMakeLists of DonTopoCore.
// It inherits from RendererState just like the Vulkan Renderer: the bloom,
// fog, SSAO, SSR and anti-aliasing parameters are the same values in both
// backends, and having them only once is what lets the same options panel
// serve both.
class D3D12Renderer : public EditorRenderer {
public:
    D3D12Renderer();
    ~D3D12Renderer();

    D3D12Renderer(const D3D12Renderer&)            = delete;
    D3D12Renderer& operator=(const D3D12Renderer&) = delete;

    // Creates the device, queue and swapchain on the already existing window. Throws
    // std::runtime_error with the HRESULT and the step that failed: the caller decides
    // whether to abort or fall back to another backend, but it is never left half-built.
    void init(Window& window);

    // Waits for the GPU to finish and releases everything. Idempotent: the destructor
    // calls it, so calling it by hand beforehand does no harm.
    void shutdown() override;

    // ── Lifecycle through the interface ──────────────────────────────────────
    // What the runtime calls without knowing which backend is in use. This one sets up everything in
    // init(), so phase 1 is that same call and phase 2 is limited to uploading
    // the meshes; the sky is already loaded by init on its own.
    void initPresentation(Window& window) override { init(window); }
    void initSceneResources(const std::vector<Mesh>& meshes) override;
    void drawFrame(Window& window) override;
    void notifyResize() override;
    void setHeadless(bool headless) override;

    // Blocks until the GPU drains everything submitted. Needed before
    // releasing resources that do not belong to this backend (ImGui's, for example):
    // the last presented frame is still in flight, and releasing its vertex buffers
    // or its texture from under it corrupts the work in progress.
    void waitIdle();

    // One full frame: waits on this slot's fence, records the clear, executes
    // and presents.
    void drawFrame();

    // Startup splash, same visible result as the Vulkan path (SplashScreen):
    // the logo letterboxed over a dark background, faded by `alpha`. false if
    // the logo can't be loaded, and the runtime starts without a splash.
    bool beginSplash(const std::string& logoPath) override;
    void drawSplashFrame(float alpha) override;

    // RECORDS the new size; it does not touch the swapchain. The real work is done by
    // drawFrame() at the start of the next frame.
    //
    // This separation is NOT a whim: the caller here is the GLFW
    // callback, which Windows dispatches from inside the WindowProc. Touching DXGI there
    // would already be risky, but what makes it unacceptable is that any
    // exception would have to unwind through the kernel callback dispatcher
    // (KiUserCallbackDispatcher), which is not possible on x64: the
    // process hangs without leaving even an error message.
    //
    // width/height of 0 (minimized window) are ignored: DXGI rejects a zero
    // size and there is nothing to present.
    void resize(uint32_t width, uint32_t height);

    void setClearColor(float r, float g, float b, float a);

    // The scene and its root. This backend does not walk them on its own (the
    // geometry comes in through registerGameObject), but it stores them so that whoever
    // gave them can ask for them back.
    void setScene(Scene* scene) override;
    void setSceneRoot(GameObject* root) override;

    // Frame camera taken from the engine's: view, position and field of
    // view come from it.
    void setCamera(const Camera& camera) override;

    void setLights(const std::vector<Light>& lights) override;
    void setLightRadii(const std::vector<float>& radii) override;

    // There are no deferred deletions here: releases wait for the GPU on the
    // spot. Implemented to fulfill the interface.
    void tickDeferredDeletes() override;

    // Scene lights, in the same format as the shaders' UBO (up to
    // 16; the rest are discarded). Without calling this (or with count 0) the backend
    // lights with a directional of its own, which is what lights the startup
    // scene when there is no project.
    //
    // The POSITION of the first one also drives the split of the shadow cascades,
    // just like in the Vulkan Renderer: the shadow is always cast
    // by light 0, whatever its type.
    void setLights(const Light* lights, size_t count);

    // Framing used to draw everything: scene, grid, fog and the split
    // of the shadow cascades. `view` and `position` must come from the same
    // camera, since the fog unprojects with one and places the eye with the other.
    // fovDegrees <= 0 keeps whatever there was.
    //
    // Recomputes the cascades, so it is called when the camera moves, not
    // unconditionally every frame.
    void setCamera(const glm::mat4& view, const glm::vec3& position, float fovDegrees = 0.0f);

    // --- Scene ----------------------------------------------------------
    //
    // Same names and same semantics as the Vulkan Renderer, so that
    // whoever builds the scene does not have to know which backend it runs on.

    // Uploads the mesh to VRAM and returns its index, which is the one to use
    // later in setTransform/setObjectMeshVisible. -1 if the mesh is empty.
    //
    // decoded is ignored: uploads here are synchronous and the decompression
    // is already done inside the Mesh itself.
    int addStaticMesh(const Mesh& mesh,
                      const std::vector<DecodedImage>* decoded = nullptr) override;

    void setTransform(size_t objectIndex, const glm::mat4& transform) override;
    void setObjectMeshVisible(size_t objectIndex, bool visible) override;

    // How reflective this object is. It goes into the scene's alpha, which is where the
    // reflection trace reads it; at zero, that object reflects nothing.
    void setObjectSsr(size_t objectIndex, float strength) override;
    void setObjectMaterialFactors(size_t objectIndex, float metallic, float roughness) override;
    void setSkinnedSsr(int index, float strength) override;
    size_t objectCount() const;

    // Releases all static geometry. Waits for the GPU before releasing:
    // the buffers may be in use by the last presented frame.
    void clearStaticMeshes();

    // Uploads an animated character: keys, skeleton, undeformed vertices and the
    // buffer where the compute writes the deformed ones. -1 if the mesh has
    // no skeleton, vertices or clips.
    //
    // The animation advances on its own with the backend's clock, looping the
    // active clip (0 on load). Whoever has an Animator that computes it
    // on the CPU uses setAnimationState and does not depend on that clock.
    int addSkinnedMesh(const SkinnedMesh& mesh,
                       const std::vector<DecodedImage>* decoded = nullptr) override;
    void rebuildSkinnedMesh(int index, const SkinnedMesh& mesh) override;

    void setSkinnedTransform(int index, const glm::mat4& transform) override;
    void setSkinnedMeshVisible(int index, bool visible) override;

    // Sets a clip and time already computed elsewhere. Same contract as in the
    // Vulkan Renderer: it is a sink, it does not advance time.
    void setAnimationState(int index, uint32_t clipIndex, float animTime) override;
    void setAnimationPose(int index, const AnimationPose& pose) override;
    void setAnimationIk(int index, const AnimationIk& ik) override;
    void updateAnimation(int index, float deltaTime) override;

    // Per-view projection of the frame, the same one used to draw: it is what
    // is needed by whoever unprojects a viewport click to know what it points at.
    glm::mat4 viewProjMatrix() const;

    // Debug lines for THIS frame: colliders, axes, rays. The format is
    // that of DonTopo::GizmoVertex (position and color, three floats each, with no
    // gap between them), and it is passed as a plain float so as not to drag Gizmos.h
    // in here, which includes vulkan.h.
    //
    // Each call REPLACES what was sent before, and what was sent does not persist to the
    // next frame: whoever draws them sends them again every time, just like
    // with the Gizmos of the Vulkan path.
    void submitDebugLines(const float* vertices, size_t vertexCount);

    // What is drawn with a selection outline: indices from addStaticMesh and from
    // addSkinnedMesh, or -1 for none. One of each can be set.
    void setSelection(int staticIndex, int skinnedIndex);
    // Thickness of the hull extrusion, in world units.
    void setOutlineWidth(float width);

    size_t skinnedCount() const;
    void   clearSkinnedMeshes();

    // --- What the editor asks for ---------------------------------------
    //
    // Uploads or releases the geometry of a node and its children. It is the same
    // operation the sandbox path does by hand, with the render indices
    // recorded in the GameObject itself.
    void registerGameObject(GameObject* node) override;
    void removeGameObject(GameObject* node) override;
    void removeMeshComponent(GameObject* node) override;

    void replaceStaticTextureWithMissing(int renderIndex, TextureSlot slot) override;

    // Contract in EditorRenderer::rebuildStaticMesh. Here the object that changes
    // material is split off from the shared mesh group: resource ownership is
    // a single flag for all five (`ownsGpu`), so giving it its own textures
    // also requires giving it its own copy of the geometry.
    void rebuildStaticMesh(int index, const Mesh& mesh) override;

    // Uploads in this backend are synchronous: waiting for the GPU is enough.
    void flushUploadsAndWait() override;

    // Recomputes near/far from the scene box, like the Vulkan path and
    // with the same floor of 200 units, so both backends
    // give the same framing. (It used to be fixed, and the comment stayed behind.)
    void refitCameraRange() override;

    void setOutlineTarget(int staticIndex, int skinnedIndex) override;

    uint32_t renderWidth() const override;
    uint32_t renderHeight() const override;
    uint32_t uiWidth() const override;
    uint32_t uiHeight() const override;
    uint64_t uiAtlasTextureId(const UiTextureAtlas* atlas) override;
    uint64_t uiThumbnailAtlasId() override;
    bool     uploadUiThumbnails(const ThumbnailTile* tiles, size_t count) override;
    float    viewportAspect() const override;

    void      setUiLayer(UiLayer* ui) override;
    UiCanvas& uiCanvas() override;

    // ALL the screen ones, in input priority order (the topmost
    // first). Same criterion and same free function as in Vulkan.
    void screenUiCanvases(std::vector<UiCanvas*>& out) override;

    // The canvas of a GameObject by its id (nullptr if it has none). Same
    // free function as in Vulkan.
    const UiCanvas* uiCanvasOf(uint64_t ownerId) const override;

    // Builds the live tree of EACH canvas in the scene. Same contract as in
    // the Vulkan Renderer: uiCanvas() still returns only the screen one.
    void syncUiCanvases(const std::vector<UiCanvasBinding>& bindings) override;
    // Looks up a node by name in ALL canvases, not only the screen one.
    const UiElement* findUiNode(const std::string& name) const override;

    // Atlases and fonts of the 2D UI. Same signatures as in the Vulkan path: the
    // widget sync calls them through a template, without knowing which backend is underneath.
    // The owner is the backend; whoever asks for them keeps only the pointer.
    UiTextureAtlas* loadUiAtlas(const std::string& path) override;
    UiFont*         loadUiFont(const std::string& path, float bakePx = 48.0f) override;

    // Changing mode or sample count moves targets and pipelines, and that is applied
    // on the next frame with the GPU idle.
    void  setAaMode(AaMode mode) override;
    void  setMsaaSamples(int v) override;
    int   maxMsaaSamples() const override;
    void  setSsaoEnabled(bool v) override;

    // Here bloom releases nothing when turned off: the image chain lives with
    // the other targets. The switch is the one in the shared state.
    void  setBloomEnabled(bool v) override;
    // The project's sky. It used NOT to be overwritten and this backend used
    // six hardcoded paths, so it ignored the scene's skybox and the
    // global IBL that comes from convolving it.
    void  initSkybox(const std::array<std::string, 6>& facePaths) override;
    void  setShadowResolution(int v) override;
    void  setPresentMode(PresentMode v) override;
    bool  presentModeSupported(PresentMode v) const override;

    // No upload batch: here each one is submitted and waited on when made.
    void  flushPendingUploads() override;

    // SSAA does scale the draw resolution here: applyPendingRenderSize
    // recomputes it on the next frame and the resolve averages.
    void  setSsaaFactor(float v) override;

    // Reflection probes, with real baking: six faces per probe, their measured
    // GPU cost and the per-object assignment done by the scene pass.
    void  requestProbeBake(uint64_t ownerId) override;
    void  requestProbeBakeAll() override;
    int   probeCount() const override;
    float lastProbeBakeMs() const override;
    uint64_t probeMemoryBytes() const override;
    float probeBakeMs(uint64_t ownerId) const override;

    // Per-pass metrics: no timing queries in this backend yet.
    void     setPerfCaptureEnabled(bool on) override;
    SlotUsage slotUsage() const override;
    float    renderGpuMs() const override;
    float    ssaoGpuMs() const override;
    float    ssrGpuMs() const override;
    float    bloomGpuMs() const override;
    float    fogGpuMs() const override;
    float    motionBlurGpuMs() const override;
    float    aaGpuMs() const override;
    float    sceneGpuMs() const override;
    float    shadowGpuMs() const override;
    float    forwardPlusGpuMs() const override;

    // Frame counters: this backend does not keep them yet.
    int      statDrawCalls() const override;
    int      statInstances() const override;
    int      statCulled() const override;
    float    forwardPlusAvgPerCell() const override;
    uint32_t forwardPlusOverflowCells() const override;

    // Description of the adapter the device was created with. Empty before
    // init(). Used to write it to the Log.
    const std::string& adapterName() const;

    // --- User interface hook --------------------------------------------
    //
    // DonTopoCore cannot depend on ImGui (the exported runtime links it and
    // it does not exist there), so the backend does not draw the UI: it exposes just enough for
    // whoever does know ImGui to do it from outside. The pointers are returned
    // as void* on purpose, so as not to drag d3d12.h into this header.

    // Invoked inside the frame, with the command list open and the
    // backbuffer already as the target, right after the last post-effect.
    void setUiDrawCallback(std::function<void()> callback);

    // Sends the composed scene to a texture instead of the backbuffer, which
    // then carries ONLY the interface. It is what a viewport inside a
    // panel needs: whoever draws the UI receives the scene as an image.
    void setRenderToTexture(bool enabled);

    // Size at which the scene is drawn when it goes to a texture: that of the panel that
    // shows it, so it comes out 1:1 and not rescaled. It is recorded and applied
    // between frames, like the window resize; 0 is ignored.
    void setViewportSize(uint32_t width, uint32_t height) override;

    // GPU descriptor of that texture, ready to use as an ImTextureID. 0 if
    // there is no image yet (before init). The heap is the same one exposed
    // in uiDescriptorHeap(), so the ImGui backend already has it bound.
    uint64_t viewportTexture() const;

    void* nativeDevice() const;       // ID3D12Device*
    void* nativeCommandList() const;  // ID3D12GraphicsCommandList*, only valid inside the callback
    void* nativeQueue() const;        // ID3D12CommandQueue*
    void* uiDescriptorHeap() const;   // ID3D12DescriptorHeap* with the range reserved for the UI

    // First descriptor of the reserved range, and how many there are. The ImGui backend
    // reserves on its own as it needs them, so it needs the whole
    // range and not just the font's slot.
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
